// SPDX-License-Identifier: GPL-3.0-only
// The voice worker: a child process of Veyra that loads TeraTTSv2 once and speaks the lines
// it is sent, and - when asked to - listens to the game's sound to tell who speaks a line.
// Separate from Veyra itself, as speech recognition is: ONNX Runtime's threads and memory stay
// out of the player, a crash costs only the voice, and the onnxruntime.dll of the recognition
// runtime is never loaded next to the one RIFE frame generation loads.
//
// Two threads do the work, so that listening to one line never holds up speaking another:
// one makes speech, the other judges voices.
#include "VoiceProtocol.h"
#include "../tts/Ort.h"
#include "../tts/TeraVoice.h"
#include "../tts/VoiceId.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace veyra::ui::voice {
namespace {

bool readBytes(HANDLE input, void* data, DWORD size) {
    auto* bytes = static_cast<char*>(data);
    while (size) {
        DWORD got = 0;
        if (!ReadFile(input, bytes, size, &got, nullptr) || !got) return false;
        bytes += got;
        size -= got;
    }
    return true;
}

class Output {
public:
    explicit Output(HANDLE handle) : handle_(handle) {}
    bool send(Frame type, uint64_t id, QByteArrayView data = {}) {
        const auto bytes = frame(type, id, data);
        std::lock_guard lock(mutex_);
        const char* p = bytes.constData();
        DWORD left = DWORD(bytes.size());
        while (left) {
            DWORD written = 0;
            if (!WriteFile(handle_, p, left, &written, nullptr) || !written) return false;
            p += written;
            left -= written;
        }
        return true;
    }
    bool sendJson(Frame type, uint64_t id, const QJsonObject& object) {
        return send(type, id, QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

private:
    HANDLE handle_;
    std::mutex mutex_;
};

struct Request {
    Frame type = Frame::Say;
    uint64_t id = 0;
    QByteArray payload;
};

// Requests waiting for one of the two threads.
class Queue {
public:
    // `urgent`: served before everything else waiting (the opening of a line is waited for
    // before it is spoken; learning from a finished line can wait).
    void push(Request request, bool urgent = false) {
        {
            std::lock_guard lock(mutex_);
            (urgent ? urgent_ : requests_).push_back(std::move(request));
        }
        ready_.notify_one();
    }
    void drop(uint64_t upTo) {
        std::lock_guard lock(mutex_);
        dropped_ = std::max(dropped_.load(), upTo);
        std::erase_if(requests_, [&](const Request& r) { return r.id <= upTo; });
    }
    void close() {
        { std::lock_guard lock(mutex_); closed_ = true; }
        ready_.notify_all();
    }
    bool pop(Request& out) { // false once closed and empty
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !requests_.empty() || !urgent_.empty(); });
        auto& from = !urgent_.empty() ? urgent_ : requests_;
        if (from.empty()) return false;
        out = std::move(from.front());
        from.pop_front();
        return true;
    }
    void clear() {
        { std::lock_guard lock(mutex_); requests_.clear(); urgent_.clear(); closed_ = true; }
        ready_.notify_all();
    }
    bool dropped(uint64_t id) const { return id <= dropped_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> requests_, urgent_;
    std::atomic<uint64_t> dropped_{0};
    bool closed_ = false;
};

QJsonValue maybe(const std::optional<bool>& value) { return value ? QJsonValue(*value) : QJsonValue(); }

std::vector<std::string> stressFixes(const QJsonArray& list) {
    std::vector<std::string> out;
    for (const auto& item : list) out.push_back(item.toString().toStdString());
    return out;
}

std::vector<float> pcmOf(const QByteArray& payload) {
    std::vector<float> wave(size_t(payload.size()) / sizeof(float));
    std::memcpy(wave.data(), payload.constData(), wave.size() * sizeof(float));
    return wave;
}

} // namespace

int runWorker() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(outputHandle) != FILE_TYPE_PIPE) return 2;
    Output output(outputHandle);

    FrameHeader header;
    if (!readBytes(input, &header, sizeof(header))) return 0;
    if (!validHeader(header) || header.type != Frame::Config || !header.bytes) return 2;
    QByteArray config(header.bytes, Qt::Uninitialized);
    if (!readBytes(input, config.data(), header.bytes)) return 0;
    const auto settings = QJsonDocument::fromJson(config).object();
    const int threads = std::clamp(settings.value("threads").toInt(4), 1, 8);

    std::unique_ptr<tts::OrtRuntime> runtime;
    std::unique_ptr<tts::TeraVoice> voice;
    try {
        runtime = std::make_unique<tts::OrtRuntime>(settings.value("runtime").toString().toStdWString());
        tts::TeraVoice::Options options;
        options.threads = threads;
        // A livelier pitch within a line than the model's own 1.0 (1.0, 1.15 and 1.3 were
        // listened to); from about 1.4 speech breaks up.
        options.noise = 1.2f;
        voice = std::make_unique<tts::TeraVoice>(settings.value("folder").toString().toStdWString(),
                                                 settings.value("cache").toString().toStdWString(), *runtime, options);
        voice->setStressFixes(stressFixes(settings.value("stressFixes").toArray()));
    } catch (const std::exception& error) {
        output.send(Frame::Failed, 0, QByteArray(error.what()));
        return 3;
    }
    // Listening is optional: if its models cannot be loaded the voice still speaks.
    std::unique_ptr<tts::VoiceListener> listener;
    QString listenError;
    const auto listen = settings.value("listen").toObject();
    if (!listen.isEmpty()) {
        try {
            listener = std::make_unique<tts::VoiceListener>(*runtime, listen.value("silero").toString().toStdWString(),
                listen.value("gender").toString().toStdWString(), listen.value("speaker").toString().toStdWString(),
                std::clamp(threads / 2, 1, 4), listen.value("recognise").toBool());
        } catch (const std::exception& error) {
            listenError = QString::fromUtf8(error.what());
        }
    }
    QJsonArray voices;
    for (const auto& name : voice->voices()) voices.append(QString::fromStdString(name));
    if (!output.sendJson(Frame::Ready, 0, QJsonObject{{"voices", voices},
            {"runtime", QString::fromStdString(runtime->version())},
            {"listening", listener != nullptr}, {"listenError", listenError}}))
        return 0;

    // Shared with the reader, which may outlive this function (it is detached at the end).
    const auto speechQueue = std::make_shared<Queue>(), hearingQueue = std::make_shared<Queue>();
    auto& speech = *speechQueue;
    auto& hearing = *hearingQueue;
    std::thread reader([input, speechQueue, hearingQueue] {
        FrameHeader h;
        while (readBytes(input, &h, sizeof(h)) && validHeader(h)) {
            QByteArray payload(h.bytes, Qt::Uninitialized);
            if (h.bytes && !readBytes(input, payload.data(), h.bytes)) break;
            if (h.type == Frame::Quit) {
                // Nothing still waiting is wanted any more.
                speechQueue->clear();
                hearingQueue->clear();
                return;
            }
            switch (h.type) {
            case Frame::Drop: speechQueue->drop(h.id); break;
            case Frame::Say: speechQueue->push({h.type, h.id, std::move(payload)}); break;
            // (urgent: a drop of what is waiting to be said does not take it away)
            case Frame::Stress: speechQueue->push({h.type, h.id, std::move(payload)}, true); break;
            case Frame::Opening: hearingQueue->push({h.type, h.id, std::move(payload)}, true); break;
            case Frame::Ended: case Frame::Forget: hearingQueue->push({h.type, h.id, std::move(payload)}); break;
            default: break;
            }
        }
        speechQueue->close();
        hearingQueue->close();
    });

    std::thread judge([&] {
        Request request;
        while (hearing.pop(request)) {
            if (!listener) continue;
            try {
                if (request.type == Frame::Forget) {
                    listener->reset();
                } else if (request.type == Frame::Opening) {
                    const auto heard = listener->opening(pcmOf(request.payload));
                    const auto number = [](const std::optional<float>& v) { return v ? QJsonValue(double(*v)) : QJsonValue(); };
                    output.sendJson(Frame::Heard, request.id, QJsonObject{{"female", maybe(heard.female)},
                        {"score", number(heard.score)}, {"arousal", number(heard.arousal)},
                        {"voice", QString::fromStdString(heard.voice)}, {"speech", double(heard.speechSeconds)},
                        {"pitch", number(heard.pitch)}});
                } else {
                    std::optional<bool> female;
                    float speechSeconds = 0.0f;
                    const auto learnt = listener->lineEnded(pcmOf(request.payload), female, &speechSeconds);
                    QJsonArray gone, born;
                    for (const auto& name : learnt.gone) gone.append(QString::fromStdString(name));
                    for (const auto& [name, isFemale] : learnt.born)
                        born.append(QJsonObject{{"voice", QString::fromStdString(name)}, {"female", maybe(isFemale)}});
                    output.sendJson(Frame::Learnt, request.id, QJsonObject{{"voice", QString::fromStdString(learnt.voice)},
                        {"female", maybe(female)}, {"isNew", learnt.isNew}, {"gone", gone}, {"born", born},
                        {"speech", double(speechSeconds)}});
                }
            } catch (const std::exception& error) {
                output.send(Frame::Failed, request.id,
                            QByteArray(request.type == Frame::Opening ? "listening:opening: " : "listening:ended: ")
                                + error.what());
            }
        }
    });

    Request request;
    while (speech.pop(request)) {
        if (request.type == Frame::Stress) { // the user's stress readings changed
            voice->setStressFixes(stressFixes(QJsonDocument::fromJson(request.payload).array()));
            continue;
        }
        if (speech.dropped(request.id)) continue;
        const auto line = QJsonDocument::fromJson(request.payload).object();
        std::vector<float> sound;
        try {
            // How it is said (VoiceOver: the game's pace, and the manner of the original line).
            tts::TeraVoice::Delivery delivery;
            delivery.speed = float(std::clamp(line.value("speed").toDouble(1.0), 0.5, 2.0));
            delivery.fitSeconds = float(std::clamp(line.value("fit").toDouble(0.0), 0.0, 60.0));
            delivery.maxSpeed = float(std::clamp(line.value("maxSpeed").toDouble(1.35), 1.0, 2.0));
            delivery.pitch = float(line.value("pitch").toDouble(1.0));
            delivery.liveliness = float(line.value("lively").toDouble(0.0));
            delivery.level = float(line.value("level").toDouble(1.0));
            sound = voice->synthesize(line.value("text").toString().toStdString(),
                                      line.value("voice").toString().toStdString(), delivery);
        } catch (const std::exception& error) {
            if (!output.send(Frame::Failed, request.id, QByteArray(error.what()))) break;
            continue;
        }
        if (speech.dropped(request.id)) continue; // superseded while it was being made
        bool sent = true;
        constexpr size_t perFrame = 16384; // samples
        for (size_t at = 0; sent && at < sound.size(); at += perFrame) {
            const size_t count = std::min(perFrame, sound.size() - at);
            sent = output.send(Frame::Audio, request.id,
                QByteArrayView(reinterpret_cast<const char*>(sound.data() + at), qsizetype(count * sizeof(float))));
        }
        if (!sent || !output.send(Frame::Done, request.id)) break;
    }
    hearing.close();
    if (judge.joinable()) judge.join();
    // The reader is blocked in ReadFile until Veyra closes the pipe or ends this process.
    reader.detach();
    return 0;
}

}
