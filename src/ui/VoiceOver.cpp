// SPDX-License-Identifier: GPL-3.0-only
#include "VoiceOver.h"
#include "LocalWorkerJob.h"
#include "VoiceModels.h"
#include "VoiceProtocol.h"
#include "../tts/Narrators.h"
#include "../tts/RuStress.h"
#include "veyra/Log.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace veyra::ui {
namespace {
// (With lines of other people said over the main lane, what is left behind is one person's
// backlog, and is cut soon.)
constexpr double farBehindSeconds = 5.0;      // more speech than this still to come: cut it short
constexpr double secondsPerWaitingLine = 1.5; // a rough length of a line not made yet
constexpr int maxWaitingLines = 3;            // more than this not made yet: keep only the newest
constexpr double staleSeconds = 6.0;          // a line this long after it appeared is skipped if another follows
constexpr int maxCrashes = 3;
// Listening: the opening of a line is first judged this long after it appeared, then again every
// askEveryMs while too little of the voice was heard for a sure answer (0.45 s of it is 97.5 %
// right on acted speech), and the line is spoken anyway giveUpUs after it appeared -
// at least minWaitMs after it came - with the best answer there was.
constexpr qint64 firstAskUs = 600000, openingClipUs = 2000000, giveUpUs = 2000000;
constexpr qint64 minWaitMs = 600, askEveryMs = 300, knownWaitMs = 400;
constexpr double enoughVoiceSeconds = 0.45, plentyVoiceSeconds = 1.2;
constexpr float sureScore = 0.8f; // the gender network this far from zero (about 70 % against 30 %)
constexpr qint64 longestLineUs = 12000000, lineHeardUs = 10000000;
// The pace of the game's actors, in characters of the subtitle per second of their speech, until
// it is learnt from the lines heard (about 160 words a minute of English, written in Russian).
constexpr double usualCharsPerSecond = 16.5;
constexpr int paceSamplesNeeded = 3;
constexpr double fastestFit = 1.35;           // a line is said at most this much faster to fit
constexpr double overlapAfterSeconds = 1.0;   // another person's line goes over the main lane busy this long
constexpr double overlapFreshSeconds = 2.5;   // ...if it appeared no longer ago than this

// How briskly a line is said by how late it starts (seconds since it appeared, plus the speech
// still to be heard before it): always a little brisk, since Russian runs
// longer than the original, and smoothly more so from one second late to three and a half,
// never above 1.25 (stepwise pacing hurried far more lines on a real game's timing).
double pace(double lateSeconds) {
    return std::clamp(1.05 + 0.2 * (lateSeconds - 1.0) / 2.5, 1.05, 1.25);
}

// Members that are people, listed after the rows for unnamed lines: characters named in the
// subtitles (kept from session to session, as a game's profile) and voices
// recognised in the sound (this session only: they are numbered afresh every time).
bool sessionMember(const tts::CastMember& m) { return m.key.starts_with("name:") || m.key.starts_with("#"); }
bool namedCharacter(const tts::CastMember& m) { return m.key.starts_with("name:"); }
QString encodedKey(const std::string& key) { return QString::fromLatin1(QUrl::toPercentEncoding(QString::fromStdString(key))); }
bool recognisedVoice(const tts::CastMember& m) { return m.key.starts_with("#"); }
// Choices remembered between sessions: the rows for lines nobody named, and named characters.
bool remembered(const std::string& key) { return key.empty() || key.starts_with("@") || key.starts_with("name:"); }

std::optional<bool> optionalBool(const QJsonValue& value) {
    if (value.isBool()) return value.toBool();
    return std::nullopt;
}
}

VoiceOver::VoiceOver(const QString& dataDirectory, QObject* parent)
    : QObject(parent), directory_(dataDirectory), assets_(voice::root(dataDirectory), voice::assets()),
      listenAssets_(voice::root(dataDirectory), voice::listeningAssets()),
      settings_(QDir(dataDirectory).filePath(QStringLiteral("live-voice.ini")), QSettings::IniFormat) {
    enabled_ = settings_.value(QStringLiteral("enabled"), false).toBool();
    speed_ = std::clamp(settings_.value(QStringLiteral("speed"), 1.0).toDouble(), 0.7, 1.6);
    volume_ = std::clamp(settings_.value(QStringLiteral("volume"), 1.0).toDouble(), 0.0, 2.0);
    duck_ = std::clamp(settings_.value(QStringLiteral("duck"), 0.35).toDouble(), 0.0, 1.0);
    // (Before there was a choice of three, a switch: off meant names not told apart.)
    const int oldDefault = settings_.value(QStringLiteral("perCharacter"), true).toBool() ? OwnVoices : NamesIgnored;
    characterVoices_ = std::clamp(settings_.value(QStringLiteral("characterVoices"), oldDefault).toInt(),
                                  int(OwnVoices), int(NamesIgnored));
    for (const auto& reading : settings_.value(QStringLiteral("stressFixes")).toStringList())
        if (tts::RuStress::validFix(reading.toStdU32String())) stressFixes_ << reading;
    listenMode_ = std::clamp(settings_.value(QStringLiteral("listen"), int(ListenOff)).toInt(), int(ListenOff),
                             int(ListenSpeakers));
    expressive_ = settings_.value(QStringLiteral("expressive"), true).toBool();
    overlap_ = settings_.value(QStringLiteral("overlap"), true).toBool();
    // (Before 2026-11-25 the rows for unnamed lines kept one choice for both meanings: it was set
    // with names told apart, so it goes to that meaning, and the other starts afresh - except a
    // narrator chosen by hand, which is still wanted.)
    if (!settings_.value(QStringLiteral("choicesSplit")).toBool()) {
        for (const auto& [old, unnamed] : {std::pair{QStringLiteral("choices/all"), QStringLiteral("choices/unnamed")},
                                           std::pair{QStringLiteral("choices/%40male"), QStringLiteral("choices/unnamed%40male")},
                                           std::pair{QStringLiteral("choices/%40female"), QStringLiteral("choices/unnamed%40female")}}) {
            const auto value = settings_.value(old).toString();
            if (value.isEmpty()) continue;
            settings_.setValue(unnamed, value);
            if (value == QLatin1String("mute")) settings_.remove(old);
        }
        settings_.setValue(QStringLiteral("choicesSplit"), true);
    }
    settings_.beginGroup(QStringLiteral("narratorNames"));
    for (const auto& id : settings_.childKeys())
        if (tts::findNarrator(id.toStdString())) narratorNames_.insert(id, settings_.value(id).toString());
    settings_.endGroup();
    ensureUnnamedRows();
    loadCharacters();
    clock_.start();
    output_.setVolume(float(volume_));
    output_.setDuck(float(duck_));
    timer_.setInterval(40);
    connect(&timer_, &QTimer::timeout, this, &VoiceOver::tick);

    worker_.setStandardErrorFile(QProcess::nullDevice());
    worker_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    });
    connect(&assets_, &ModelAssets::changed, this, [this] {
        if (enabled_ && installed() && !downloading()) ensureWorker();
        emit changed();
    });
    connect(&listenAssets_, &ModelAssets::changed, this, [this] {
        // The listening models arrived: the worker loads them on its next start.
        if (enabled_ && listenMode_ != ListenOff && listenInstalled() && !listenDownloading()) {
            audio_.start();
            if (!workerAskedToListen_ && installed()) restartWorker();
        }
        emit changed();
    });
    connect(&worker_, &QProcess::started, this, [this] {
        job_ = attachLocalWorkerJob(DWORD(worker_.processId()));
        if (!job_) { fail(tr("无法启动语音朗读进程")); return; }
        QJsonObject config{
            {"folder", voice::modelFolder(directory_)},
            {"runtime", voice::runtimeLibrary(directory_)},
            {"cache", voice::cacheFolder(directory_)},
            {"threads", std::clamp(QThread::idealThreadCount() / 4, 2, 4)},
            {"stressFixes", QJsonArray::fromStringList(stressFixes_)},
        };
        workerAskedToListen_ = listenMode_ != ListenOff && listenInstalled();
        if (workerAskedToListen_)
            config.insert("listen", QJsonObject{{"silero", voice::vadModel(directory_)},
                                                {"gender", voice::genderFolder(directory_)},
                                                {"speaker", voice::speakerModel(directory_)},
                                                {"recognise", listenMode_ == ListenSpeakers}});
        send(int(voice::Frame::Config), 0, QJsonDocument(config).toJson(QJsonDocument::Compact));
        setStatus(tr("正在加载朗读语音…"));
    });
    connect(&worker_, &QProcess::readyReadStandardOutput, this, &VoiceOver::readOutput);
    connect(&worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(tr("无法启动语音朗读进程"));
    });
    connect(&worker_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (job_) CloseHandle(std::exchange(job_, nullptr));
        forgetWorkerState();
        if (std::exchange(stopping_, false) || !(enabled_ || previewing_)) return;
        log::warn("voice", std::format("worker exited code={}", code));
        if (++crashes_ < maxCrashes) ensureWorker();
        else fail(tr("语音朗读进程已停止，请重新启用"));
    });

    if (enabled_) {
        output_.start();
        if (listenMode_ != ListenOff && listenInstalled()) audio_.start();
        if (installed()) ensureWorker();
    }
}

VoiceOver::~VoiceOver() {
    worker_.disconnect(this);
    stopWorker();
    if (job_) CloseHandle(std::exchange(job_, nullptr));
    audio_.stop();
    output_.stop();
}

QStringList VoiceOver::voices() const {
    if (!workerVoices_.isEmpty()) return workerVoices_;
    return {QStringLiteral("ru_m5"), QStringLiteral("ru_f1"), QStringLiteral("ru_m1"), QStringLiteral("ru_f2"),
            QStringLiteral("eng_m4"), QStringLiteral("eng_f5"), QStringLiteral("eng_m3"), QStringLiteral("eng_f3")};
}

QString VoiceOver::narratorLabel(const QString& id) const {
    const auto* narrator = tts::findNarrator(id.toStdString());
    if (!narrator) return id;
    return (narrator->female ? QStringLiteral("♀ ") : QStringLiteral("♂ ")) + narratorName(id);
}

QString VoiceOver::status() const {
    if (assets_.downloading() || (!assets_.installed() && !assets_.status().isEmpty())) return assets_.status();
    if (listenAssets_.downloading() || (!listenAssets_.installed() && !listenAssets_.status().isEmpty()))
        return listenAssets_.status();
    if (enabled_ && !assets_.installed()) return tr("请先下载朗读语音");
    if (enabled_ && listenMode_ != ListenOff && !listenAssets_.installed()) return tr("请先下载声音识别模型");
    if (output_.failed()) return tr("无法打开音频输出设备");
    return status_;
}

void VoiceOver::setStatus(QString value) {
    if (status_ == value) return;
    status_ = std::move(value);
    emit changed();
}

bool VoiceOver::listening() const {
    return listenMode_ != ListenOff && workerListens_ && audio_.running();
}

void VoiceOver::setEnabled(bool value) {
    if (enabled_ == value) return;
    enabled_ = value;
    settings_.setValue(QStringLiteral("enabled"), value);
    crashes_ = 0;
    if (value) {
        output_.start();
        if (listenMode_ != ListenOff && listenInstalled()) audio_.start();
        if (installed()) ensureWorker();
    } else {
        previewing_ = false;
        silence();
        stopWorker();
        audio_.stop();
        output_.stop();
        status_.clear();
    }
    emit changed();
}

void VoiceOver::setVoice(QString value) {
    // The narrator of every line (with the game's sound listened to: of unnamed men).
    setSpeakerVoice(listenMode_ == ListenOff ? QString() : QStringLiteral("@male"), value);
}

void VoiceOver::setSpeed(double value) {
    value = std::clamp(value, 0.7, 1.6);
    if (qFuzzyCompare(value, speed_)) return;
    speed_ = value;
    settings_.setValue(QStringLiteral("speed"), speed_);
    emit changed();
}

void VoiceOver::setVolume(double value) {
    value = std::clamp(value, 0.0, 2.0);
    if (qFuzzyCompare(value + 1.0, volume_ + 1.0)) return;
    volume_ = value;
    output_.setVolume(float(value));
    settings_.setValue(QStringLiteral("volume"), volume_);
    emit changed();
}

void VoiceOver::setDuck(double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(value + 1.0, duck_ + 1.0)) return;
    duck_ = value;
    output_.setDuck(float(value));
    settings_.setValue(QStringLiteral("duck"), duck_);
    emit changed();
}

void VoiceOver::setPerCharacter(bool value) {
    setCharacterVoices(value ? OwnVoices : NamesIgnored);
}

void VoiceOver::setCharacterVoices(int value) {
    value = std::clamp(value, int(OwnVoices), int(NamesIgnored));
    if (characterVoices_ == value) return;
    characterVoices_ = value;
    settings_.setValue(QStringLiteral("characterVoices"), value);
    ensureUnnamedRows();
    reloadUnnamedChoices();
}

// A character as the profile keeps it: the narrator "auto" gave them, and what is known of their sex.
void VoiceOver::saveCharacter(const tts::CastMember& m) {
    if (!namedCharacter(m)) return;
    QJsonObject entry{{"title", QString::fromStdString(m.title)}, {"fallback", QString::fromStdString(m.fallback)},
                      {"heard", m.heard}, {"votes", m.votes}, {"misses", m.misses}};
    if (m.female) entry.insert("female", *m.female);
    if (m.pitch) entry.insert("pitch", double(*m.pitch));
    settings_.setValue(QStringLiteral("characters/") + encodedKey(m.key),
                       QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact)));
}

void VoiceOver::loadCharacters() {
    settings_.beginGroup(QStringLiteral("characters"));
    const auto keys = settings_.childKeys();
    std::vector<std::pair<std::string, QJsonObject>> found;
    for (const auto& encoded : keys) {
        const auto key = QUrl::fromPercentEncoding(encoded.toLatin1()).toStdString();
        const auto entry = QJsonDocument::fromJson(settings_.value(encoded).toString().toUtf8()).object();
        if (!key.starts_with("name:") || entry.isEmpty()) continue;
        // (Kept by an earlier version: a speaker described is no character any more.)
        if (tts::describedSpeaker(entry.value("title").toString().toStdU32String())) {
            settings_.remove(encoded);
            continue;
        }
        found.emplace_back(key, entry);
    }
    settings_.endGroup();
    for (const auto& [key, entry] : found) {
        tts::CastMember m;
        m.key = key;
        m.title = entry.value("title").toString().toStdString();
        m.fallback = entry.value("fallback").toString().toStdString();
        m.heard = entry.value("heard").toBool();
        m.votes = std::clamp(entry.value("votes").toInt(), -6, 6);
        m.misses = std::clamp(entry.value("misses").toInt(), 0, 2);
        if (entry.value("female").isBool()) m.female = entry.value("female").toBool();
        if (entry.value("pitch").isDouble()) m.pitch = float(entry.value("pitch").toDouble());
        if (m.title.empty()) continue;
        names_.canonical(QString::fromStdString(m.title).toStdU32String()); // misread later, still them
        cast_.restore(std::move(m));
        restoreChoice(key);
    }
}

void VoiceOver::forgetCharacters() {
    cast_.removeIf(namedCharacter);
    names_.clear();
    endedKeys_.clear();
    settings_.remove(QStringLiteral("characters"));
    settings_.beginGroup(QStringLiteral("choices"));
    for (const auto& key : settings_.childKeys())
        if (QUrl::fromPercentEncoding(key.toLatin1()).startsWith(QLatin1String("name:"))) settings_.remove(key);
    settings_.endGroup();
    emit speakersChanged();
}

bool VoiceOver::addStressFix(const QString& reading) {
    const auto wanted = reading.trimmed();
    if (!tts::RuStress::validFix(wanted.toStdU32String())) return false;
    const auto word = [](QString r) { return r.remove(QLatin1Char('+')).toLower().replace(QChar(u'ё'), QChar(u'е')); };
    const auto key = word(wanted);
    stressFixes_.erase(std::remove_if(stressFixes_.begin(), stressFixes_.end(),
                                      [&](const QString& r) { return word(r) == key; }), stressFixes_.end());
    stressFixes_.prepend(wanted);
    settings_.setValue(QStringLiteral("stressFixes"), stressFixes_);
    sendStressFixes();
    emit changed();
    return true;
}

void VoiceOver::removeStressFix(const QString& reading) {
    if (!stressFixes_.removeOne(reading)) return;
    settings_.setValue(QStringLiteral("stressFixes"), stressFixes_);
    sendStressFixes();
    emit changed();
}

void VoiceOver::sendStressFixes() {
    if (!ready_) return; // the worker reads them from its configuration when it starts
    send(int(voice::Frame::Stress), 0, QJsonDocument(QJsonArray::fromStringList(stressFixes_)).toJson(QJsonDocument::Compact));
}

void VoiceOver::setListenMode(int value) {
    value = std::clamp(value, int(ListenOff), int(ListenSpeakers));
    if (value == listenMode_) return;
    const int before = listenMode_;
    listenMode_ = value;
    settings_.setValue(QStringLiteral("listen"), value);
    cast_.removeIf(recognisedVoice);
    ensureUnnamedRows();
    emit speakersChanged();
    if (enabled_ && value != ListenOff && listenInstalled()) audio_.start();
    if (value == ListenOff) audio_.stop();
    // The worker loads the listening models (and the fingerprint model) only as configured.
    if (enabled_ && installed() && listenInstalled() && (before == ListenOff || value == ListenOff ||
        (before == ListenSpeakers) != (value == ListenSpeakers)))
        restartWorker();
    emit changed();
}

void VoiceOver::setExpressive(bool value) {
    if (expressive_ == value) return;
    expressive_ = value;
    settings_.setValue(QStringLiteral("expressive"), value);
    emit changed();
}

void VoiceOver::setOverlap(bool value) {
    if (overlap_ == value) return;
    overlap_ = value;
    settings_.setValue(QStringLiteral("overlap"), value);
    emit changed();
}

void VoiceOver::download() {
    if (!downloading()) assets_.download();
}

void VoiceOver::downloadListening() {
    if (!listenDownloading()) listenAssets_.download();
}

// Who reads this line: the character named in it, the voice recognised in the game's sound,
// a man's or a woman's narrator by the voice heard, or the one for every line (as Cast.h hands
// them out). Empty when that speaker is muted.
QString VoiceOver::narratorFor(const Line& line, std::string* keyOut) {
    std::string key, title;
    std::optional<bool> female = line.female;
    std::optional<float> pitch;
    bool character = false;
    if (perCharacter() && !line.name.isEmpty()) {
        key = (QStringLiteral("name:") + line.name.toLower()).toStdString();
        title = line.name.toStdString();
        pitch = line.pitch;
        // Met for the first time: the name guesses the sex; its first line heard decides it.
        female = tts::nameLooksFemale(line.name.toStdU32String());
        character = true;
    } else if (listenMode_ == ListenSpeakers && !line.heardVoice.isEmpty()) {
        key = (QStringLiteral("#") + line.heardVoice).toStdString();
        title = line.heardVoice.toStdString();
        pitch = line.pitch;
        character = true;
    } else if (listenMode_ != ListenOff) {
        // The voice heard decides; without it, what the subtitle says of the speaker; without
        // that, who spoke last.
        const auto sex = line.heardFemale ? line.heardFemale : line.describedFemale ? line.describedFemale : female;
        key = sex.value_or(false) ? "@female" : "@male";
        female.reset();
    } else if (line.describedFemale && perCharacter()) {
        // Not listened to, but the subtitle says it: "Незнакомка", "Мужчина".
        key = *line.describedFemale ? "@female" : "@male";
        female = line.describedFemale;
    }
    bool created = false;
    auto& member = cast_.member(key, title, female, pitch, &created);
    if (created) restoreChoice(key); // (changes the member in place: the reference holds)
    // A character's sex is settled once, by its first line heard, not by every line's opening
    // (Cast.h); later its whole lines vote (learnt).
    if (character && line.listened) cast_.hear(key, line.heardFemale, line.pitch);
    cast_.spoke(member);
    saveCharacter(member);
    const auto narrator = effectiveNarrator(member);
    emit speakersChanged();
    if (keyOut) *keyOut = key;
    return narrator;
}

// The rows for lines nobody named: one narrator for every line, or a man's and a woman's when
// the game's sound tells them apart.
void VoiceOver::ensureUnnamedRows() {
    const auto ensure = [&](const std::string& key, std::optional<bool> female) {
        bool created = false;
        cast_.member(key, {}, female, {}, &created);
        if (created) restoreChoice(key);
    };
    // Lines nobody named: one narrator, or a man's and a woman's by the voice heard. The two
    // also read for the characters when they are to have only those two voices.
    if (listenMode_ == ListenOff) ensure({}, std::nullopt);
    else cast_.remove({});
    if (listenMode_ != ListenOff || characterVoices_ == TwoVoices) {
        ensure("@male", false);
        ensure("@female", true);
    } else {
        cast_.remove("@male");
        cast_.remove("@female");
    }
    voice_ = memberNarrator(listenMode_ == ListenOff ? std::string() : std::string("@male"));
}

QString VoiceOver::choiceSetting(const std::string& key) const {
    const bool unnamedRow = key.empty() || key.starts_with("@");
    const QString prefix = unnamedRow && namesTold() ? QStringLiteral("choices/unnamed") : QStringLiteral("choices/");
    if (key.empty()) return namesTold() ? prefix : QStringLiteral("choices/all");
    return prefix + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromStdString(key)));
}

// The meaning of the rows for unnamed lines changed (names told apart or not): their choices too.
void VoiceOver::reloadUnnamedChoices() {
    for (const std::string key : {std::string(), std::string("@male"), std::string("@female")}) {
        if (!cast_.find(key)) continue;
        const auto saved = settings_.value(choiceSetting(key)).toString();
        cast_.setChoice(key, saved.isEmpty() ? std::string("auto") : saved.toStdString());
    }
    voice_ = memberNarrator(listenMode_ == ListenOff ? std::string() : std::string("@male"));
    emit speakersChanged();
    emit changed();
}

void VoiceOver::setNameStyle(int value) {
    if (nameStyle_ == value) return;
    nameStyle_ = value;
    reloadUnnamedChoices();
}

void VoiceOver::restoreChoice(const std::string& key) {
    if (!remembered(key)) return;
    const auto saved = settings_.value(choiceSetting(key)).toString();
    if (!saved.isEmpty()) cast_.setChoice(key, saved.toStdString());
}

QString VoiceOver::memberNarrator(const std::string& key) const {
    const auto* member = cast_.find(key);
    return member ? QString::fromStdString(cast_.narrator(*member)) : QString();
}

QString VoiceOver::autoNarrator(const tts::CastMember& m) const {
    if (characterVoices_ == TwoVoices && namedCharacter(m))
        return memberNarrator(m.female.value_or(false) ? "@female" : "@male");
    return QString::fromStdString(m.fallback);
}

QString VoiceOver::effectiveNarrator(const tts::CastMember& m) const {
    if (m.choice == "mute") return {};
    if (m.choice != "auto") return QString::fromStdString(cast_.narrator(m)); // chosen by hand
    return autoNarrator(m);
}

QString VoiceOver::narratorName(const QString& id) const {
    if (const auto found = narratorNames_.constFind(id); found != narratorNames_.constEnd()) return *found;
    const auto* narrator = tts::findNarrator(id.toStdString());
    return narrator ? QString::fromStdString(narrator->name) : id;
}

QVariantList VoiceOver::speakers() const {
    QVariantList out;
    const auto row = [&](const tts::CastMember& m) {
        const auto narrator = effectiveNarrator(m);
        QString title = QString::fromStdString(m.title);
        if (m.key.empty()) title = namesTold() ? tr("没有名字的台词") : tr("所有台词");
        else if (m.key == "@male") title = tr("男声");
        else if (m.key == "@female") title = tr("女声");
        QVariantMap r{{"key", QString::fromStdString(m.key)}, {"title", title},
                      {"choice", QString::fromStdString(m.choice)}, {"narrator", narrator},
                      {"narratorName", narrator.isEmpty() ? QString() : narratorName(narrator)},
                      {"automatic", m.choice == "auto"},
                      {"fallbackName", narratorName(autoNarrator(m))}};
        if (m.female) r.insert(QStringLiteral("female"), *m.female);
        out << r;
    };
    for (const auto& m : cast_.members())
        if (!sessionMember(m)) row(m);
    std::vector<const tts::CastMember*> spoken;
    for (const auto& m : cast_.members()) {
        if (m.key.starts_with("name:") && !perCharacter()) continue;
        if (m.key.starts_with("#") && listenMode_ != ListenSpeakers) continue;
        if (sessionMember(m)) spoken.push_back(&m);
    }
    std::sort(spoken.begin(), spoken.end(), [](const auto* a, const auto* b) { return a->lastSpoke > b->lastSpoke; });
    for (const auto* m : spoken) row(*m);
    return out;
}

QVariantList VoiceOver::narrators() const {
    QVariantList out;
    for (const auto& n : tts::narrators()) {
        const auto id = QString::fromStdString(n.id);
        if (!workerVoices_.isEmpty() && !workerVoices_.contains(id)) continue;
        out << QVariantMap{{"id", id}, {"name", narratorName(id)}, {"female", n.female}, {"byHand", n.byHandOnly}};
    }
    return out;
}

void VoiceOver::setSpeakerVoice(const QString& key, const QString& choice) {
    const auto k = key.toStdString();
    if (!cast_.find(k)) return;
    cast_.setChoice(k, choice.toStdString());
    if (remembered(k)) settings_.setValue(choiceSetting(k), QString::fromStdString(cast_.find(k)->choice));
    if (k == (listenMode_ == ListenOff ? std::string() : std::string("@male"))) voice_ = memberNarrator(k);
    emit speakersChanged();
    emit changed();
}

void VoiceOver::renameNarrator(const QString& id, const QString& name) {
    const auto* narrator = tts::findNarrator(id.toStdString());
    if (!narrator) return;
    const auto wanted = name.trimmed().left(24);
    const auto key = QStringLiteral("narratorNames/") + id;
    if (wanted.isEmpty() || wanted == QString::fromStdString(narrator->name)) {
        narratorNames_.remove(id);
        settings_.remove(key);
    } else {
        narratorNames_.insert(id, wanted);
        settings_.setValue(key, wanted);
    }
    emit changed();
    emit speakersChanged();
}

void VoiceOver::restartWorker() {
    stopWorker();
    crashes_ = 0;
    ensureWorker();
}

void VoiceOver::ensureWorker() {
    if (worker_.state() != QProcess::NotRunning || !installed() || downloading()) return;
    if (!ModelAssets::verify(voice::root(directory_), voice::assets()[0], false)) {
        fail(tr("朗读运行库不完整，请重新下载"));
        return;
    }
    ready_ = false;
    stopping_ = false;
    buffer_.clear();
    worker_.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--voice-worker")});
}

void VoiceOver::stopWorker() {
    if (worker_.state() == QProcess::NotRunning) return;
    stopping_ = true;
    send(int(voice::Frame::Quit), 0);
    worker_.closeWriteChannel();
    if (!worker_.waitForFinished(800)) {
        worker_.kill();
        worker_.waitForFinished(800);
    }
    forgetWorkerState();
}

// The worker is gone: what it was making will not come, and the voices it had learnt
// ("Голос 1", ...) are numbered afresh by the next one.
void VoiceOver::forgetWorkerState() {
    ready_ = false;
    workerListens_ = false;
    buffer_.clear();
    lastDone_ = speaking_;
    cast_.removeIf(recognisedVoice);
    emit speakersChanged();
    lastHeardVoice_.clear();
    lastFemale_.reset();
    laneOf_.clear();
    mainKey_.clear();
    mainNarrator_.clear();
    overlapId_ = 0;
}

void VoiceOver::send(int type, quint64 id, const QByteArray& payload) {
    if (worker_.state() != QProcess::Running) return;
    worker_.write(voice::frame(voice::Frame(type), id, payload));
}

void VoiceOver::fail(const QString& message) {
    log::warn("voice", message.toStdString());
    stopWorker();
    setStatus(message);
}

void VoiceOver::say(const QString& subtitle, qint64 appearedUs) {
    if (!enabled_) return;
    const bool names = nameStyle_ != 2; // (2: the game never names who speaks)
    auto cleaned = tts::cleanSubtitle(subtitle.toStdU32String(), names);
    // One character, one speaker: a name misread is the one already seen.
    if (names) names_.knownPrefix(cleaned);
    QString name = cleaned.speaker.empty() ? QString() : QString::fromStdU32String(names_.canonical(cleaned.speaker));
    // "Незнакомый человек:" on a subtitle of its own names the speaker of the next one.
    if (cleaned.text.empty()) {
        if (!name.isEmpty()) { pendingName_ = name; pendingNameAtMs_ = clock_.elapsed(); }
        return;
    }
    if (name.isEmpty() && !pendingName_.isEmpty() && clock_.elapsed() - pendingNameAtMs_ < pendingNameMs) name = pendingName_;
    pendingName_.clear();
    // "Незнакомый человек", "Прохожий": a different person each time - no character of their own,
    // so that the sex is heard (or read from the words) line by line.
    std::optional<bool> describedFemale;
    if (!name.isEmpty() && tts::describedSpeaker(name.toStdU32String(), &describedFemale)) name.clear();
    const auto text = QString::fromStdU32String(memory_.unsaid(cleaned.text, double(clock_.elapsed()) / 1000.0));
    if (text.isEmpty()) return;
    Line line;
    line.id = ++nextLine_;
    line.text = text;
    line.name = name;
    line.describedFemale = describedFemale;
    line.appearedUs = appearedUs;
    line.queuedAtMs = clock_.elapsed();
    const bool known = perCharacter() && !line.name.isEmpty()
        && !cast_.needsHearing((QStringLiteral("name:") + line.name.toLower()).toStdString());
    if (listening() && appearedUs > 0) {
        lineOver(appearedUs); // the line before it has left the screen
        onScreen_ = line;
        const qint64 nowMs = clock_.elapsed(), nowUs = sink::recordingClockUs();
        line.askAtMs = nowMs + std::max<qint64>(0, (appearedUs + firstAskUs - nowUs) / 1000);
        // A character whose voice was heard already needs no listening to know who it is: it
        // waits only a moment, for the manner of the line.
        line.giveUpAtMs = known ? line.askAtMs + knownWaitMs
            : std::max(line.askAtMs + minWaitMs, nowMs + (appearedUs + giveUpUs - nowUs) / 1000);
        line.known = known;
        line.decided = known && !expressive_;
    } else {
        line.decided = true;
    }
    lines_.push_back(std::move(line));
    if (!timer_.isActive()) timer_.start();
    flush();
}

void VoiceOver::tick() {
    const qint64 nowMs = clock_.elapsed();
    for (auto& line : lines_) {
        if (line.decided) continue;
        if (!line.asked && nowMs >= line.askAtMs) {
            // From the moment it appeared (only a little before: the line before was another voice).
            const qint64 until = std::min(audio_.endUs(), line.appearedUs + openingClipUs);
            const auto clip = audio_.clip(line.appearedUs - 50000, until);
            if (clip.size() >= size_t(voice::listenRate / 4) && ready_) {
                send(int(voice::Frame::Opening), line.id,
                     QByteArray(reinterpret_cast<const char*>(clip.data()), qsizetype(clip.size() * sizeof(float))));
                line.asked = true;
            }
        }
        if (nowMs >= line.giveUpAtMs) {
            // No sure answer in time: the best one there was, or - nothing heard at all - most
            // likely the one who spoke last is still talking.
            line.decided = true;
            line.listened = true;
            if (line.partialFemale) {
                line.female = line.partialFemale;
                if (line.partialScore && std::abs(*line.partialScore) >= sureScore / 2) line.heardFemale = line.partialFemale;
                line.pitch = line.partialPitch;
                line.heardVoice = line.partialVoice;
                lastFemale_ = line.partialFemale;
                if (!line.partialVoice.isEmpty()) lastHeardVoice_ = line.partialVoice;
            } else {
                line.female = lastFemale_;
                line.heardVoice = lastHeardVoice_;
            }
            line.arousal = line.partialArousal;
        }
    }
    if (onScreen_ && sink::recordingClockUs() - onScreen_->appearedUs > lineHeardUs)
        lineOver(onScreen_->appearedUs + lineHeardUs);
    flush();
    if (lines_.empty() && !onScreen_) timer_.stop();
}

void VoiceOver::flush() {
    while (!lines_.empty() && lines_.front().decided) {
        const auto line = std::move(lines_.front());
        lines_.pop_front();
        // Lines asked for and not made yet (the one on the second lane is not the main lane's).
        const bool overlapBusy = overlapId_ > lastDone_;
        auto waiting = speaking_ > lastDone_ ? double(speaking_ - lastDone_) : 0.0;
        if (overlapBusy) waiting = std::max(0.0, waiting - 1.0);
        double behind = output_.bufferedSeconds(0) + waiting * secondsPerWaitingLine;
        // How long ago the line appeared (by the picture's clock when known).
        const double waited = line.appearedUs > 0
            ? std::max(0.0, double(sink::recordingClockUs() - line.appearedUs) / 1e6)
            : std::max(0.0, double(clock_.elapsed() - line.queuedAtMs) / 1000.0);
        // Long gone from the screen and another line already waiting: skip it.
        if (waited > staleSeconds && !lines_.empty()) continue;
        std::string key;
        const auto narrator = narratorFor(line, &key);
        if (narrator.isEmpty()) continue; // that speaker is muted
        // Someone else starts talking while the main lane is still busy: said over it, as in the
        // original, rather than after it. Only people told apart (a name, a voice recognised or
        // a man against a woman) and with narrators of their own - one narrator over himself
        // would only be a muddle.
        const bool overlapFree = !overlapBusy && output_.bufferedSeconds(1) < 0.15;
        const bool other = !mainKey_.empty() && !key.empty() && key != mainKey_ && narrator != mainNarrator_;
        // (Fresh by when the line came here: listening to it may already have taken two seconds.)
        const double sinceCame = std::max(0.0, double(clock_.elapsed() - line.queuedAtMs) / 1000.0);
        const int lane = overlap_ && overlapFree && other && behind > overlapAfterSeconds
            && sinceCame < overlapFreshSeconds ? 1 : 0;
        if (lane == 0 && behind > farBehindSeconds) {
            // What is still to be heard belongs to a moment long past: cut it short rather than
            // let every following line come that late.
            output_.clear(0);
            behind = waiting * secondsPerWaitingLine;
        }
        const double late = waited + (lane == 0 ? behind : output_.bufferedSeconds(1));
        const auto id = speak(line.text, narrator, deliveryFor(line, late), lane);
        if (lane == 1) {
            overlapId_ = id;
        } else {
            mainKey_ = key.empty() ? std::string("\x01") : key; // ("" is a speaker too: every line nobody named)
            mainNarrator_ = narrator;
        }
    }
}

// How a line is said: brisker the later it is, fitted to the time the game's
// actors take for a line that long, and - expressive - in the manner of its original.
VoiceOver::Delivery VoiceOver::deliveryFor(const Line& line, double late) const {
    Delivery d;
    d.speed = speed_ * pace(late);
    if (expressive_) {
        // Heard in the game when listened to; otherwise only what the text shows.
        double manner = 0.0;
        if (line.arousal) manner = *line.arousal;
        else if (line.text.contains(QLatin1Char('!'))) manner = 0.3;
        else if (line.text.trimmed().endsWith(QStringLiteral("..."))) manner = -0.15;
        manner = std::clamp(manner, -1.0, 1.0);
        // Excited: a little higher, livelier, louder and quicker; calm or quiet: the other way.
        // (Measured on TeraTTSv2: the starting noise from 1.05 to 1.35 moves the pitch within a
        // line further while every word stays clear; a pitch of +-5 % keeps the narrator's voice.)
        d.lively = 0.15 * manner;
        d.pitch = 1.0 + 0.05 * manner;
        d.level = std::pow(10.0, 3.0 * manner / 20.0);
        d.speed *= 1.0 + 0.06 * manner;
    }
    const double perSecond = paceSamples_ >= paceSamplesNeeded ? gameCharsPerSecond_ : usualCharsPerSecond;
    d.fit = double(line.text.trimmed().size()) / perSecond;
    d.maxSpeed = std::max(d.speed, speed_ * fastestFit);
    return d;
}

// The line on screen has gone: its whole sound teaches the worker the voices of the session.
void VoiceOver::lineOver(qint64 untilUs) {
    if (!onScreen_) return;
    const auto line = std::move(*onScreen_);
    onScreen_.reset();
    if (!listening() || !ready_) return;
    const qint64 until = std::min({untilUs, line.appearedUs + longestLineUs, audio_.endUs()});
    const auto clip = audio_.clip(line.appearedUs, until);
    if (clip.size() < size_t(voice::listenRate)) return;
    if (perCharacter() && !line.name.isEmpty()) {
        endedKeys_[line.id] = (QStringLiteral("name:") + line.name.toLower()).toStdString();
        while (endedKeys_.size() > 32) endedKeys_.erase(std::min_element(endedKeys_.begin(), endedKeys_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; }));
    }
    endedChars_[line.id] = int(line.text.trimmed().size());
    while (endedChars_.size() > 32) endedChars_.erase(std::min_element(endedChars_.begin(), endedChars_.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; }));
    send(int(voice::Frame::Ended), line.id,
         QByteArray(reinterpret_cast<const char*>(clip.data()), qsizetype(clip.size() * sizeof(float))));
}

void VoiceOver::heard(quint64 id, const QJsonObject& info) {
    const auto line = std::find_if(lines_.begin(), lines_.end(), [&](const Line& l) { return l.id == id; });
    const auto number = [&](const char* name) {
        const auto value = info.value(QLatin1String(name));
        return value.isDouble() ? std::optional<float>(float(value.toDouble())) : std::nullopt;
    };
    const auto female = optionalBool(info.value("female"));
    const auto score = number("score"), pitch = number("pitch"), arousal = number("arousal");
    const double speech = info.value("speech").toDouble();
    const auto voiceName = info.value("voice").toString();
    if (line == lines_.end() || line->decided) {
        // Too late for its line, still a clear word on who is talking now.
        if (female && score && std::abs(*score) >= sureScore && speech >= enoughVoiceSeconds) lastFemale_ = female;
        if (!voiceName.isEmpty()) lastHeardVoice_ = voiceName;
        return;
    }
    // Sure: enough of the voice and a clear answer - or as much voice as there will be. A
    // character already known only waits for how the line is said.
    const bool sure = (line->known && arousal) || speech >= plentyVoiceSeconds
        || (speech >= enoughVoiceSeconds && score && std::abs(*score) >= sureScore);
    const qint64 nowMs = clock_.elapsed();
    const bool moreToHear = line->appearedUs + openingClipUs > audio_.endUs();
    if (!sure && moreToHear && nowMs + askEveryMs < line->giveUpAtMs) {
        // Not enough of the voice yet: keep the best answer so far and listen again in a moment.
        if (female && (!line->partialScore || !score || std::abs(*score) >= std::abs(*line->partialScore))) {
            line->partialFemale = female;
            line->partialScore = score;
            line->partialPitch = pitch;
            line->partialVoice = voiceName;
        }
        if (arousal) line->partialArousal = arousal;
        line->asked = false;
        line->askAtMs = nowMs + askEveryMs;
        return;
    }
    // The answer, or the better one heard before it.
    const bool takePartial = line->partialFemale
        && (!female || (line->partialScore && (!score || std::abs(*line->partialScore) > std::abs(*score))));
    const auto chosen = takePartial ? line->partialFemale : female;
    const auto chosenVoice = takePartial ? line->partialVoice : voiceName;
    if (chosen) lastFemale_ = chosen;
    if (!chosenVoice.isEmpty()) lastHeardVoice_ = chosenVoice;
    line->decided = true;
    line->listened = true;
    // What settles a character's sex (Cast::hear) has to be clear; a doubtful answer still picks
    // this line's narrator.
    const auto chosenScore = takePartial ? line->partialScore : score;
    line->heardFemale = sure || (chosenScore && std::abs(*chosenScore) >= sureScore / 2) ? chosen : std::nullopt;
    // Nothing audible in its opening: most likely the one who spoke last is still talking.
    line->female = chosen ? chosen : lastFemale_;
    line->heardVoice = chosenVoice.isEmpty() && !chosen ? lastHeardVoice_ : chosenVoice;
    if (chosen) line->pitch = takePartial ? line->partialPitch : pitch;
    line->arousal = arousal ? arousal : line->partialArousal;
    flush();
}

void VoiceOver::learnt(quint64 id, const QJsonObject& info) {
    // Voices joined into another one give back their narrators.
    for (const auto& gone : info.value("gone").toArray())
        cast_.remove((QStringLiteral("#") + gone.toString()).toStdString());
    // A whole line heard: how long its actor took for it teaches the game's pace.
    if (const auto chars = endedChars_.find(id); chars != endedChars_.end()) {
        const double speech = info.value("speech").toDouble();
        if (speech >= 0.8 && chars->second >= 12) {
            const double perSecond = std::clamp(double(chars->second) / speech, 10.0, 24.0);
            gameCharsPerSecond_ = paceSamples_ ? 0.8 * gameCharsPerSecond_ + 0.2 * perSecond : perSecond;
            ++paceSamples_;
        }
        endedChars_.erase(chars);
    }
    // A whole line heard: a vote on whether its speaker is a man or a woman (Cast::confirm).
    const auto female = optionalBool(info.value("female"));
    const auto voiceName = info.value("voice").toString();
    if (const auto named = endedKeys_.find(id); named != endedKeys_.end()) {
        if (female) {
            cast_.confirm(named->second, *female, {});
            if (const auto* m = cast_.find(named->second)) saveCharacter(*m);
        }
        endedKeys_.erase(named);
    } else if (female && listenMode_ == ListenSpeakers && !voiceName.isEmpty()) {
        cast_.confirm((QStringLiteral("#") + voiceName).toStdString(), *female, {});
    }
    emit speakersChanged();
}

quint64 VoiceOver::speak(const QString& text, const QString& narrator, double speed) {
    Delivery delivery;
    delivery.speed = speed;
    return speak(text, narrator, delivery, 0);
}

quint64 VoiceOver::speak(const QString& text, const QString& narrator, const Delivery& delivery, int lane) {
    output_.start();
    if (!ready_) {
        // The worker is still loading (or not started): only the newest line is worth keeping.
        waitingLine_ = text;
        waitingVoice_ = narrator;
        ensureWorker();
        return 0;
    }
    const auto id = ++nextId_;
    if (speaking_ > lastDone_ && speaking_ - lastDone_ >= quint64(maxWaitingLines)) {
        // Lines come much faster than they can be made: the ones still waiting are dropped.
        send(int(voice::Frame::Drop), id - 1);
        lastDone_ = id - 1; // and whatever of them is still on its way is not played
    }
    QJsonObject line{{"text", text}, {"voice", narrator}, {"speed", delivery.speed}};
    if (delivery.fit > 0.0) {
        line.insert("fit", delivery.fit);
        line.insert("maxSpeed", delivery.maxSpeed);
    }
    if (delivery.pitch != 1.0) line.insert("pitch", delivery.pitch);
    if (delivery.lively != 0.0) line.insert("lively", delivery.lively);
    if (delivery.level != 1.0) line.insert("level", delivery.level);
    send(int(voice::Frame::Say), id, QJsonDocument(line).toJson(QJsonDocument::Compact));
    speaking_ = id;
    std::erase_if(laneOf_, [&](const auto& entry) { return entry.first <= lastDone_; });
    if (lane) laneOf_[id] = lane;
    return id;
}

void VoiceOver::silence() {
    waitingLine_.clear();
    pendingName_.clear();
    memory_.clear();
    lines_.clear();
    onScreen_.reset();
    output_.clear();
    if (speaking_ > lastDone_) send(int(voice::Frame::Drop), speaking_);
    lastDone_ = speaking_;
    laneOf_.clear();
    mainKey_.clear();
    mainNarrator_.clear();
    overlapId_ = 0;
}

void VoiceOver::newSession() {
    // The characters stay (another scene of the same game); voices recognised by sound do not.
    cast_.removeIf(recognisedVoice);
    endedKeys_.clear();
    emit speakersChanged();
    lastFemale_.reset();
    lastHeardVoice_.clear();
    // Another game's actors keep another pace.
    gameCharsPerSecond_ = 0.0;
    paceSamples_ = 0;
    endedChars_.clear();
    if (ready_ && workerListens_) send(int(voice::Frame::Forget), 0);
}

void VoiceOver::preview() {
    previewNarrator(memberNarrator(listenMode_ == ListenOff ? std::string() : std::string("@male")));
}

void VoiceOver::previewNarrator(const QString& id) {
    if (!installed()) {
        setStatus(tr("请先下载朗读语音"));
        return;
    }
    const auto narrator = id.isEmpty() ? QString::fromStdString(tts::defaultNarrator(false).id) : id;
    previewing_ = true;
    silence();
    const auto sample = tr("我是%1，字幕就会这样读出来。").arg(narratorName(narrator));
    previewId_ = speak(sample, narrator, speed_);
    if (!previewId_) previewId_ = std::numeric_limits<quint64>::max(); // spoken once the worker is ready
}

void VoiceOver::readOutput() {
    buffer_ += worker_.readAllStandardOutput();
    while (buffer_.size() >= qsizetype(sizeof(voice::FrameHeader))) {
        voice::FrameHeader header;
        std::memcpy(&header, buffer_.constData(), sizeof(header));
        if (!voice::validHeader(header)) {
            fail(tr("语音朗读进程返回了无效数据"));
            return;
        }
        const qsizetype total = qsizetype(sizeof(header) + header.bytes);
        if (buffer_.size() < total) return;
        const QByteArray payload = buffer_.mid(sizeof(header), header.bytes);
        buffer_.remove(0, total);
        switch (header.type) {
        case voice::Frame::Ready: {
            ready_ = true;
            crashes_ = 0;
            const auto info = QJsonDocument::fromJson(payload).object();
            workerVoices_.clear();
            for (const auto& name : info.value("voices").toArray()) workerVoices_ << name.toString();
            workerListens_ = info.value("listening").toBool();
            const auto listenError = info.value("listenError").toString();
            log::info("voice", std::format("worker ready onnxruntime={} voices={} listening={}",
                info.value("runtime").toString().toStdString(), workerVoices_.size(), workerListens_));
            setStatus(listenMode_ != ListenOff && !listenError.isEmpty()
                ? tr("无法加载声音识别模型：%1").arg(listenError) : QString{});
            emit changed();
            if (!waitingLine_.isEmpty()) {
                const auto id = speak(std::exchange(waitingLine_, {}), waitingVoice_, speed_);
                if (previewId_ == std::numeric_limits<quint64>::max()) previewId_ = id;
            }
            break;
        }
        case voice::Frame::Audio:
            // Speech of a line that was dropped or silenced meanwhile is not played.
            if (header.id > lastDone_ && header.bytes % sizeof(float) == 0) {
                const auto lane = laneOf_.find(header.id);
                output_.push(reinterpret_cast<const float*>(payload.constData()), payload.size() / sizeof(float),
                             lane == laneOf_.end() ? 0 : lane->second);
            }
            break;
        case voice::Frame::Done:
            lastDone_ = std::max(lastDone_, header.id);
            std::erase_if(laneOf_, [&](const auto& entry) { return entry.first <= lastDone_; });
            if (previewing_ && header.id == previewId_ && !enabled_) {
                // A sample heard with the voice off: let it play out, then let the worker go.
                previewId_ = 0;
                QTimer::singleShot(int(output_.bufferedSeconds() * 1000) + 600, this, [this] {
                    if (enabled_ || previewId_) return;
                    previewing_ = false;
                    stopWorker();
                    output_.stop();
                });
            }
            break;
        case voice::Frame::Heard:
            heard(header.id, QJsonDocument::fromJson(payload).object());
            break;
        case voice::Frame::Learnt:
            learnt(header.id, QJsonDocument::fromJson(payload).object());
            break;
        case voice::Frame::Failed:
            if (payload.startsWith("listening:")) {
                log::warn("voice", payload.toStdString());
                // (Only a failed opening is waited for; a failed whole line only teaches nothing.)
                if (header.id && payload.startsWith("listening:opening:")) heard(header.id, {});
                break;
            }
            if (!header.id) {
                fail(tr("无法加载朗读语音：%1").arg(QString::fromUtf8(payload)));
                return;
            }
            log::warn("voice", std::format("request {} failed: {}", header.id, payload.toStdString()));
            lastDone_ = std::max(lastDone_, header.id);
            break;
        default:
            fail(tr("语音朗读进程返回了无效数据"));
            return;
        }
    }
}

}
