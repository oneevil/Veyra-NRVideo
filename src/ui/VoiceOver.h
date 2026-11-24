// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Live translation read aloud: every translated line that appears on the picture is spoken
// by a TeraTTSv2 narrator (see src/tts), made in a worker process ("--voice-worker") and
// played by VoiceOutput, with the player turned down while the voice speaks.
//
// Every line is spoken, in order, as soon as it is read; the
// further the voice falls behind the briskier it speaks (up to 1.25x), and when it is far
// behind the old speech is cut short. The speaker's name ("Анна: ...") is not read out but
// can pick a narrator of its own; sound descriptions ("[Смеётся]") are skipped, a repeated
// line is not said twice, and a subtitle that grew is only read for its new words.
//
// Listening to the game (optional, models downloaded on their own): while a line is on screen
// the game's sound is its speaker talking. Its opening tells a man from a woman, and - in the
// "every speaker" mode - which of the voices heard so far it is; a line waits up to a moment
// for that before it is spoken. When a line leaves the screen its whole sound teaches the
// worker the voices of the session (see src/tts/VoiceId.h). The opening is listened to again
// until enough of the voice was heard for a sure answer (0.45 s of it).
//
// A line is said in the manner of its original - louder, higher and livelier
// when the game's voice shouts, softer when it is quiet (from its loudness and pitch against the
// session's usual, and from "!" in the text); it is fitted to the time the game's own actors take
// for lines of that length (learnt from the lines heard); and when another person starts talking
// while the voice is still busy with a line, the new line is said over it on a second lane, as
// people talk over one another in the original, instead of waiting its turn.
#include "GameAudio.h"
#include "TranslationAssets.h"
#include "VoiceOutput.h"
#include "../tts/Cast.h"
#include "../tts/SubtitleText.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <windows.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>

class QJsonObject;

namespace veyra::ui {

class VoiceOver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY changed)
    Q_PROPERTY(QStringList voices READ voices NOTIFY changed)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
    Q_PROPERTY(double duck READ duck WRITE setDuck NOTIFY changed)
    Q_PROPERTY(bool perCharacter READ perCharacter WRITE setPerCharacter NOTIFY changed)
    // Who reads for the characters named in the subtitles (CharacterVoices).
    Q_PROPERTY(int characterVoices READ characterVoices WRITE setCharacterVoices NOTIFY changed)
    // The user's own stress readings ("Тр+евор"), which win over RUAccent.
    Q_PROPERTY(QStringList stressFixes READ stressFixes NOTIFY changed)
    Q_PROPERTY(int listenMode READ listenMode WRITE setListenMode NOTIFY changed)
    // Lines said in the manner of the original (by its sound and the "!" in the text).
    Q_PROPERTY(bool expressive READ expressive WRITE setExpressive NOTIFY changed)
    // Lines of different people at the same time said over one another rather than in turn.
    Q_PROPERTY(bool overlap READ overlap WRITE setOverlap NOTIFY changed)
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY changed)
    Q_PROPERTY(qint64 downloadBytes READ downloadBytes CONSTANT)
    Q_PROPERTY(bool listenInstalled READ listenInstalled NOTIFY changed)
    Q_PROPERTY(bool listenDownloading READ listenDownloading NOTIFY changed)
    Q_PROPERTY(double listenProgress READ listenProgress NOTIFY changed)
    Q_PROPERTY(qint64 listenBytes READ listenBytes CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    // Who has spoken and which narrator reads for each: the rows for lines
    // nobody named ("" every line, or "@male"/"@female" when the game's sound is listened to),
    // then characters named in the subtitles ("name:...") and voices recognised ("#Голос 2"),
    // the latest speakers first. [{key, title, female?, choice, narrator, narratorName,
    // automatic, fallbackName}]; choice: "auto", "mute" or a narrator id.
    Q_PROPERTY(QVariantList speakers READ speakers NOTIFY speakersChanged)
    // Every narrator: [{id, name, female, byHand}] (names as the user renamed them).
    Q_PROPERTY(QVariantList narrators READ narrators NOTIFY changed)
public:
    enum ListenMode { ListenOff, ListenGender, ListenSpeakers };
    // Each named character a narrator of their own; or only the man's and the woman's narrator
    // (by the character's sex); or names not told apart at all (every line as if unnamed).
    enum CharacterVoices { OwnVoices, TwoVoices, NamesIgnored };
    explicit VoiceOver(const QString& dataDirectory, QObject* parent = nullptr);
    ~VoiceOver() override;

    bool enabled() const { return enabled_; }
    QString voice() const { return voice_; }
    QStringList voices() const;
    double speed() const { return speed_; }
    double volume() const { return volume_; }
    double duck() const { return duck_; }
    bool perCharacter() const { return characterVoices_ != NamesIgnored; }
    int characterVoices() const { return characterVoices_; }
    QStringList stressFixes() const { return stressFixes_; }
    int listenMode() const { return listenMode_; }
    bool expressive() const { return expressive_; }
    bool overlap() const { return overlap_; }
    bool installed() const { return assets_.installed(); }
    bool downloading() const { return assets_.downloading(); }
    double downloadProgress() const { return double(assets_.received()) / double(std::max<qint64>(1, downloadBytes())); }
    qint64 downloadBytes() const { return assets_.totalBytes(); }
    bool listenInstalled() const { return listenAssets_.installed(); }
    bool listenDownloading() const { return listenAssets_.downloading(); }
    double listenProgress() const { return double(listenAssets_.received()) / double(std::max<qint64>(1, listenBytes())); }
    qint64 listenBytes() const { return listenAssets_.totalBytes(); }
    QString status() const;

    void setEnabled(bool value);
    void setVoice(QString value);
    void setSpeed(double value);
    void setVolume(double value);
    void setDuck(double value);
    void setPerCharacter(bool value);
    void setCharacterVoices(int value);
    // How the subtitles name who speaks (LiveTranslation::NameStyle): 2 = never.
    void setNameStyle(int value);
    void setListenMode(int value);
    void setExpressive(bool value);
    void setOverlap(bool value);

    // A line to read out (ignored while the voice is off). `appearedUs`: when it appeared on
    // screen, by sink::recordingClockUs() (0: unknown - it is not listened to).
    void say(const QString& text, qint64 appearedUs = 0);
    // Stop speaking and forget what is waiting (playback stopped, sought, the source changed).
    void silence();
    // Another video or game: the voices heard so far belong to other people.
    void newSession();
    Q_INVOKABLE void preview();
    // A narrator as the window shows it: "♂ Виктор", "♀ Лиза" (Narrators.h).
    Q_INVOKABLE QString narratorLabel(const QString& id) const;
    QVariantList speakers() const;
    QVariantList narrators() const;
    Q_INVOKABLE void setSpeakerVoice(const QString& key, const QString& choice);
    // A name of one's own for a narrator (empty: its own name back).
    Q_INVOKABLE void renameNarrator(const QString& id, const QString& name);
    Q_INVOKABLE void previewNarrator(const QString& id);
    // Adds or replaces a stress reading; false when it is not a word with one "+" before a vowel.
    Q_INVOKABLE bool addStressFix(const QString& reading);
    Q_INVOKABLE void removeStressFix(const QString& reading);
    // The characters remembered from earlier sessions are forgotten (another game).
    Q_INVOKABLE void forgetCharacters();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancelDownload() { assets_.cancel(); }
    Q_INVOKABLE void downloadListening();
    Q_INVOKABLE void cancelListeningDownload() { listenAssets_.cancel(); }

signals:
    void changed();
    void speakersChanged();

private:
    struct Line {
        quint64 id = 0;
        QString text, name;            // what to say; the name shown with it ("" none)
        // A speaker described, not named ("Незнакомка", "Мужчина"): the sex the words say, used
        // when the voice was not heard. Such a line has no name: each is another person.
        std::optional<bool> describedFemale;
        qint64 appearedUs = 0;
        qint64 queuedAtMs = 0;         // when say() got it (VoiceOver's own clock)
        qint64 askAtMs = 0, giveUpAtMs = 0;
        bool asked = false, decided = false;
        bool known = false;            // a character whose voice was heard already
        std::optional<bool> female;    // heard in the game's sound (or who spoke last)
        bool listened = false;         // its opening was listened to...
        std::optional<bool> heardFemale; // ...and this is what it was (empty: no voice in it)
        std::optional<float> pitch;    // of the voice heard, Hz
        std::optional<float> arousal;  // how it was said (VoiceListener::Heard::arousal)
        QString heardVoice;            // "Голос 2" when recognised
        // An answer not sure enough yet (too little voice heard so far): kept in case no better comes.
        std::optional<bool> partialFemale;
        std::optional<float> partialScore, partialPitch, partialArousal;
        QString partialVoice;
    };
    bool listening() const;
    void restartWorker();
    void ensureWorker();
    void stopWorker();
    void send(int type, quint64 id, const QByteArray& payload = {});
    struct Delivery { double speed = 1.0, fit = 0.0, maxSpeed = 1.35, pitch = 1.0, lively = 0.0, level = 1.0; };
    quint64 speak(const QString& text, const QString& narrator, double speed);
    quint64 speak(const QString& text, const QString& narrator, const Delivery& delivery, int lane);
    Delivery deliveryFor(const Line& line, double late) const;
    void forgetWorkerState();
    void tick();
    void flush();
    void lineOver(qint64 untilUs);
    QString narratorFor(const Line& line, std::string* key = nullptr);
    void ensureUnnamedRows();
    void restoreChoice(const std::string& key);
    // Whether the subtitles' names are told apart: then the rows for lines nobody named are only
    // for those, otherwise they read every line - a choice of their own, kept apart (muting the
    // unnamed lines of a game with names must not silence everything once names are off).
    bool namesTold() const { return perCharacter() && nameStyle_ != 2; }
    QString choiceSetting(const std::string& key) const;
    void reloadUnnamedChoices();
    QString narratorName(const QString& id) const;
    QString memberNarrator(const std::string& key) const;
    QString autoNarrator(const tts::CastMember& member) const;      // what "auto" means for it now
    QString effectiveNarrator(const tts::CastMember& member) const; // who reads for it ("" muted)
    void saveCharacter(const tts::CastMember& member);
    void loadCharacters();
    void sendStressFixes();
    void readOutput();
    void heard(quint64 id, const QJsonObject& info);
    void learnt(quint64 id, const QJsonObject& info);
    void fail(const QString& message);
    void setStatus(QString value);

    const QString directory_;
    ModelAssets assets_, listenAssets_;
    QSettings settings_;
    VoiceOutput output_;
    GameAudio audio_;
    QProcess worker_;
    HANDLE job_ = nullptr;
    QTimer timer_;
    QByteArray buffer_;
    QStringList workerVoices_;
    QString voice_, status_, waitingLine_, waitingVoice_;
    double speed_ = 1.0, volume_ = 1.0, duck_ = 0.35;
    bool enabled_ = false, ready_ = false, stopping_ = false, previewing_ = false;
    bool workerListens_ = false, workerAskedToListen_ = false;
    quint64 previewId_ = 0;
    int listenMode_ = ListenOff;
    bool expressive_ = true, overlap_ = true;
    int crashes_ = 0;
    quint64 nextId_ = 0, speaking_ = 0, lastDone_ = 0, nextLine_ = 0;
    tts::SpokenMemory memory_;
    tts::NameBook names_;                     // the characters' names as first read
    int characterVoices_ = OwnVoices;
    int nameStyle_ = 0;
    QStringList stressFixes_;
    std::unordered_map<quint64, std::string> endedKeys_; // line sent whole -> the character it is
    std::unordered_map<quint64, int> endedChars_;        // line sent whole -> characters in its text
    // The game's pace: characters of a line (as the subtitle has them) per second its actor
    // talks, learnt from the lines heard; and how many lines it was learnt from.
    double gameCharsPerSecond_ = 0.0;
    int paceSamples_ = 0;
    std::unordered_map<quint64, int> laneOf_; // request -> the output lane its sound goes to
    std::string mainKey_;                     // who the main lane speaks for now
    QString mainNarrator_;
    quint64 overlapId_ = 0;                   // the line on the second lane (0: none)
    QElapsedTimer clock_;
    std::deque<Line> lines_;                  // waiting to be decided and spoken, in order
    std::optional<Line> onScreen_;            // the newest line, whose sound is still being heard
    std::optional<bool> lastFemale_;          // who spoke last, for a line with nothing audible
    QString lastHeardVoice_;
    tts::Cast cast_;                          // who speaks and who reads for them, this session
    QHash<QString, QString> narratorNames_;   // narrator id -> the user's name for it
    QString pendingName_;                     // a name read on a subtitle of its own...
    qint64 pendingNameAtMs_ = 0;              // ...and when: it names the next line if that comes soon
    static constexpr qint64 pendingNameMs = 8000;
};

}
