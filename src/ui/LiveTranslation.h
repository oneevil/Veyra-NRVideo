// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CaptionPresenter.h"
#include "LiveAsr.h"
#include "LiveSentences.h"
#include "VoiceOver.h"
#include "TranslationAssets.h"
#include <QCache>
#include <QElapsedTimer>
#include <QQueue>
#include <QRectF>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <deque>
#include <optional>

namespace veyra::ui {
class LiveTranslation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ sourceSupported NOTIFY changed)
    Q_PROPERTY(bool screenTextSupported READ screenTextSupported CONSTANT)
    Q_PROPERTY(int screenRegion READ screenRegion WRITE setScreenRegion NOTIFY changed)
    Q_PROPERTY(QRectF customRegion READ customRegion WRITE setCustomRegion NOTIFY changed)
    Q_PROPERTY(QString screenReading READ screenReading NOTIFY previewChanged)
    Q_PROPERTY(int screenWords READ screenWords NOTIFY previewChanged)
    // How the picture is read: 0 the neural reader (PP-OCRv5), 1 Windows' own.
    Q_PROPERTY(int screenReader READ screenReader WRITE setScreenReader NOTIFY changed)
    Q_PROPERTY(bool neuralInstalled READ neuralInstalled NOTIFY changed)
    Q_PROPERTY(bool neuralDownloading READ neuralDownloading NOTIFY changed)
    Q_PROPERTY(double neuralProgress READ neuralProgress NOTIFY changed)
    Q_PROPERTY(qint64 neuralBytes READ neuralBytes CONSTANT)
    // Whether the neural reader reads the source language (else Windows' reader is used).
    Q_PROPERTY(bool neuralReadsLanguage READ neuralReadsLanguage NOTIFY changed)
    Q_PROPERTY(bool screenTextNeuralReady READ screenTextNeural NOTIFY changed)
    // Lines reached the side of the area twice this session: it is too narrow.
    Q_PROPERTY(bool linesClipped READ linesClipped NOTIFY changed)
    // Looking for the subtitles over the whole picture (FindState), and the seconds still to look.
    Q_PROPERTY(int findState READ findState NOTIFY changed)
    Q_PROPERTY(int findSecondsLeft READ findSecondsLeft NOTIFY changed)
    Q_PROPERTY(bool useGpu READ useGpu WRITE setUseGpu NOTIFY changed)
    Q_PROPERTY(bool windowsCaptionsSupported READ windowsCaptionsSupported CONSTANT)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(int provider READ provider WRITE setProvider NOTIFY changed)
    Q_PROPERTY(int recognitionSource READ recognitionSource WRITE setRecognitionSource NOTIFY changed)
    Q_PROPERTY(int recognitionModel READ recognitionModel WRITE setRecognitionModel NOTIFY changed)
    Q_PROPERTY(int recognitionThreads READ recognitionThreads WRITE setRecognitionThreads NOTIFY changed)
    Q_PROPERTY(int recognitionEffectiveThreads READ recognitionEffectiveThreads NOTIFY changed)
    Q_PROPERTY(QString recognitionStatus READ recognitionStatus NOTIFY changed)
    Q_PROPERTY(bool recognitionInstalled READ recognitionInstalled NOTIFY changed)
    Q_PROPERTY(bool recognitionDownloading READ recognitionDownloading NOTIFY changed)
    Q_PROPERTY(double recognitionProgress READ recognitionProgress NOTIFY changed)
    Q_PROPERTY(qint64 recognitionBytes READ recognitionBytes NOTIFY changed)
    Q_PROPERTY(double recognitionRtf READ recognitionRtf NOTIFY changed)
    Q_PROPERTY(double averageTranslationMs READ averageTranslationMs NOTIFY changed)
    Q_PROPERTY(int skippedSentences READ skippedSentences NOTIFY changed)
    Q_PROPERTY(QString sourceLanguage READ sourceLanguage WRITE setSourceLanguage NOTIFY changed)
    Q_PROPERTY(QString targetLanguage READ targetLanguage WRITE setTargetLanguage NOTIFY changed)
    Q_PROPERTY(bool showOriginal READ showOriginal WRITE setShowOriginal NOTIFY changed)
    // Draw the caption on the picture. Off: recognition, translation and the voice keep working,
    // nothing is drawn (read aloud only).
    Q_PROPERTY(bool showOnVideo READ showOnVideo WRITE setShowOnVideo NOTIFY changed)
    // How the subtitles name who speaks (NameStyle): read off the picture and by the voice.
    Q_PROPERTY(int nameStyle READ nameStyle WRITE setNameStyle NOTIFY changed)
    Q_PROPERTY(QString apiBase READ apiBase WRITE setApiBase NOTIFY changed)
    Q_PROPERTY(QString apiModel READ apiModel WRITE setApiModel NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)
    Q_PROPERTY(QString translated READ translated NOTIFY captionChanged)
    Q_PROPERTY(bool modelInstalled READ modelInstalled NOTIFY changed)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY changed)
    Q_PROPERTY(qint64 downloadBytes READ downloadBytes CONSTANT)
    Q_PROPERTY(QObject* voice READ voice CONSTANT)
public:
    enum Provider { Local, OpenAi, MyMemory };
    enum Source { VeyraAudio, WindowsCaptions, ScreenText };
    // Gta6: the "GTA VI" template - its area, measured on a recording of the game, and the
    // game's own rule of what a subtitle is (only rows that begin with the speaker's name).
    enum ScreenRegion { BottomSubtitles, BottomWide, TopSubtitles, WholePicture, CustomRegion, Gta6 };
    // A game read by its own rules (the neural reader only): 0 none, 1 GTA VI.
    int screenTextGame() const { return screenRegion_ == Gta6 ? 1 : 0; }
    explicit LiveTranslation(const QString& directory, QObject* parent = nullptr);
    ~LiveTranslation() override;
    bool sourceSupported() const;
    static bool screenTextSupported();
    bool screenTextWanted() const { return enabled_ && recognitionSource_ == ScreenText; }
    int screenRegion() const { return screenRegion_; }
    QRectF customRegion() const { return customRegion_; }
    QRectF screenTextRect() const;
    bool screenTextCentered() const;
    QString screenTextLanguage() const;
    int screenTextIntervalMs() const;
    QString screenReading() const { return screenReading_; }
    int screenWords() const { return screenWords_; }
    enum Reader { NeuralReader, WindowsReader };
    int screenReader() const { return screenReader_; }
    bool neuralInstalled() const { return ocrAssets_.installed(); }
    bool neuralDownloading() const { return ocrAssets_.downloading(); }
    double neuralProgress() const { return double(ocrAssets_.received()) / double(std::max<qint64>(1, neuralBytes())); }
    qint64 neuralBytes() const { return ocrAssets_.totalBytes(); }
    bool neuralReadsLanguage() const;
    // What the engine is to read with: the neural reader when chosen, downloaded and able to read
    // the language; its model files and runtime (empty: Windows' reader).
    bool screenTextNeural() const;
    QString neuralDetector() const;
    QString neuralRecognizer() const;
    QString neuralRuntime() const;
    QString wordList() const;
    QString neuralFinderRecognizer() const; // a reading model for the search (any will do: nothing is read) // the stress dictionary's quick copy (may not exist yet)
    enum FindState { FindIdle, FindLooking, FindFound, FindNothing, FindFailed };
    int findState() const { return findState_; }
    int findSecondsLeft() const { return findSecondsLeft_; }
    // The engine asks: a search was requested (taken once).
    bool takeFindRequest() { return std::exchange(findRequested_, false); }
    // The engine reports on the search; `found`: the band as fractions of the picture.
    void acceptFind(int state, int secondsLeft, QRectF found);
    bool linesClipped() const { return linesClipped_; }
    void acceptClipped(uint32_t lines);
    bool useGpu() const { return useGpu_; }
    static bool windowsCaptionsSupported();
    bool enabled() const { return enabled_; }
    int provider() const { return provider_; }
    int recognitionSource() const { return recognitionSource_; }
    int recognitionModel() const { return recognitionModel_; }
    int recognitionThreads() const { return recognitionThreads_; }
    int recognitionEffectiveThreads() const { return asr_.threads(); }
    QString recognitionStatus() const { return asr_.status(); }
    bool recognitionInstalled() const { return asr_.installed(); }
    bool recognitionDownloading() const { return asr_.downloading(); }
    double recognitionProgress() const { return asr_.progress(); }
    qint64 recognitionBytes() const { return asr_.downloadBytes(); }
    double recognitionRtf() const { return asr_.realTimeFactor(); }
    double averageTranslationMs() const { return averageTranslationMs_; }
    int skippedSentences() const { return skippedSentences_; }
    QString sourceLanguage() const;
    QString targetLanguage() const { return targetLanguage_; }
    bool showOriginal() const { return showOriginal_; }
    bool showOnVideo() const { return showOnVideo_; }
    enum NameStyle { NamesWithColon, NamesOnOwnLine, NoNames };
    int nameStyle() const { return nameStyle_; }
    QString apiBase() const { return apiBase_; }
    QString apiModel() const { return apiModel_; }
    QString status() const;
    struct DisplayCaption {
        QString original;
        QString translated;
        QString draft;
    };
    DisplayCaption displayCaption() const;
    QString caption() const { return displayCaption().original; }
    QString translated() const { return displayCaption().translated; }
    bool modelInstalled() const { return assets_.installed(); }
    bool downloading() const { return assets_.downloads.downloading(); }
    double downloadProgress() const { return double(assets_.downloads.received()) / double(downloadBytes()); }
    qint64 downloadBytes() const { return assets_.downloads.totalBytes(); }
    QObject* voice() { return &voice_; }

    void setEnabled(bool value);
    void setProvider(int value);
    void setRecognitionSource(int value);
    void setScreenRegion(int value);
    void setCustomRegion(QRectF value);
    void setUseGpu(bool value);
    // `appearedUs`: when the line on screen first showed up (sink::recordingClockUs; 0 unknown).
    void acceptScreenText(const QString& text, bool ready, const QString& error,
                          const QString& reading = {}, int words = 0, qint64 appearedUs = 0);
    void setRecognitionModel(int value);
    void setRecognitionThreads(int value);
    void setSourceLanguage(QString value);
    void setTargetLanguage(QString value);
    void setShowOriginal(bool value);
    void setShowOnVideo(bool value);
    void setNameStyle(int value);
    void setScreenReader(int value);
    void setApiBase(QString value);
    void setApiModel(QString value);
    Q_INVOKABLE void setApiKey(const QString& value);
    Q_INVOKABLE void openWindowsCaptions();
    Q_INVOKABLE void downloadModel();
    Q_INVOKABLE void cancelModelDownload();
    Q_INVOKABLE void downloadRecognitionModel();
    Q_INVOKABLE void cancelRecognitionDownload() { asr_.cancel(); }
    Q_INVOKABLE void deleteRecognitionModel();
    Q_INVOKABLE void downloadNeuralReader() { ocrAssets_.download(); }
    Q_INVOKABLE void cancelNeuralDownload() { ocrAssets_.cancel(); }
    // Watch the whole picture for a few seconds and take the band where subtitles appear as the
    // area read (the neural reader's text finder; it must be downloaded).
    Q_INVOKABLE void findSubtitles();
    void setPlayback(quint64 session, bool active, bool discontinuity);
signals:
    void changed();
    void captionChanged();
    void previewChanged();
private:
    struct Work {
        quint64 id = 0;
        LiveSentence sentence;
        QStringList context;
        QString cacheKey;
        qint64 screenStart = -1;
        qint64 screenAppearedUs = 0; // when the screen line first showed up (0: unknown)
        int phrases = 1;
        std::vector<qint64> wordEnds;
    };
    struct Row {
        quint64 id = 0;
        qint64 until = 0;
        qint64 originalUntil = 0;
        QString original;
        QString translated;
    };

    struct SpeechRow {
        Work work;
        QString translated;
        qint64 readyAt = 0;
    };

    void setStatus(QString status);
    void speak(quint64 id, const QString& translation, qint64 appearedUs);
    void configureAsr();
    void tick();
    void readCaptions();
    void acceptCaption(const QString& text);
    void acceptScreenLine(const QString& text, qint64 appearedUs);
    void stepScreenPresenter();
    void tickScreen(qint64 now);
    void serverNotListening(qint64 now);
    bool usingGpu() const { return useGpu_ && !gpuFailed_ && assets_.vulkanInstalled(); }
    QString cacheKey(const QString& language, const QString& text) const;
    void addSentences(std::vector<LiveSentence> sentences);
    void displayTranslation(const Work& work, QString translation);
    void resetCaption();
    void cancelRequest();
    void discardHiddenWork();
    void addSpeech(std::vector<LiveSentence> sentences);
    bool resolveSpeech(const Work& work);
    void queueSpeechResult(const Work& work, QString translation);
    void skipSpeech(const Work& work, bool stale);
    void pruneSpeech(qint64 now);
    void advanceSpeech(qint64 now);
    void startLocal();
    void stopLocal();
    void requestTranslation(Work work);
    void receiveTranslation(QNetworkReply* reply, const Work& work);
    void summarize(qint64 now);
    void prune(qint64 now);

    TranslationAssets assets_;
    LiveAsr asr_;
    VoiceOver voice_; // reads the translated lines aloud
    QSettings settings_;
    QNetworkAccessManager remoteNetwork_, localNetwork_;
    QProcess reader_, server_;
    QTimer timer_;
    QElapsedTimer clock_;
    QNetworkReply* request_ = nullptr;
    HANDLE serverJob_ = nullptr;
    bool enabled_ = false, active_ = false, captionsOpen_ = false, serverFailed_ = false, readerStarted_ = false;
    int nameStyle_ = NamesWithColon;
    int screenReader_ = NeuralReader;
    ModelAssets ocrAssets_;
    int findState_ = FindIdle, findSecondsLeft_ = 0;
    bool findRequested_ = false, linesClipped_ = false;
    QString directory_;
    bool showOriginal_ = true, showOnVideo_ = true, useGpu_ = true, gpuFailed_ = false;
    int screenRegion_ = BottomSubtitles, screenWords_ = 0, screenRetries_ = 0;
    QRectF customRegion_{0.08, 0.70, 0.84, 0.27};
    QString screenCurrent_, screenReading_, screenError_;
    quint64 spokenId_ = 0;
    quint64 screenLine_ = 0, screenSequence_ = 0, screenRequested_ = 0;
    qint64 screenStart_ = 0, screenRetryAt_ = 0, screenAppearedUs_ = 0;
    CaptionPresenter<DisplayCaption> screenPresenter_;
    int provider_ = Local;
    int recognitionSource_ = VeyraAudio, recognitionModel_ = asr::Parakeet, recognitionThreads_ = 0;
    quint16 serverPort_ = 0;
    quint64 session_ = 0, nextSentence_ = 0, requestSentence_ = 0;
    qint64 requestedAt_ = 0, serverStartedAt_ = 0, lastCaptionReadAt_ = 0, lastSummaryAt_ = 0;
    QByteArray captionBuffer_, serverKey_;
    QString raw_, status_;
    QString sourceLanguage_, targetLanguage_, apiBase_, apiModel_;
    QByteArray apiKey_;
    LiveSentences sentences_;
    QQueue<Work> queue_;
    std::deque<Row> rows_;
    std::deque<SpeechRow> speechReady_;
    std::optional<SpeechRow> speechShown_;
    std::optional<Work> speechRequest_;
    QString speechDraft_;
    qint64 speechSwapAt_ = 0, speechBurstSwapAt_ = 0, speechHideAt_ = 0, draftHideAt_ = 0;
    QStringList context_;
    QCache<QString, QString> cache_{200};
    std::vector<qint64> requestTimes_, displayDelays_, spokenDelays_;
    int sentenceCount_ = 0, requestedCount_ = 0, cacheHits_ = 0, stale_ = 0, hidden_ = 0, failures_ = 0;
    int skippedSentences_ = 0;
    int earlySentences_ = 0, displayedSentences_ = 0;
    double averageTranslationMs_ = 0.0;
};
}
