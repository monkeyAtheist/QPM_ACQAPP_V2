#pragma once

// Listening to a signal of a session on a sound card (Qt Multimedia, QAudioSink): its channels (one: mono,
// two: left and right) made ready on a thread of its own at the rate and in the format of the card
// (qacq/analysis/playback.h: resampling, level normalized or as it is), then played once or in a loop, at a
// volume that changes while it plays, on the output chosen. One player for the whole application: playing
// something else stops what played. The output, the level, the loop and the volume are remembered.

#include "qacq/analysis/playback.h"
#include "qacq/app/app_settings.h"
#include "qacq/model/signal.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <vector>

class QAudioSink;

namespace qacq::app
{
class PlaybackDevice;

class AudioPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged) // seconds into what is heard
    Q_PROPERTY(double duration READ duration NOTIFY stateChanged)     // seconds heard
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // "« Haut-parleurs » · 48 kHz · stéréo · normalisé (−7 dB) · en boucle", while it plays.
    Q_PROPERTY(QString description READ description NOTIFY stateChanged)
    // What is played: a session and a signal of it (0 when nothing plays).
    Q_PROPERTY(QObject *source READ source NOTIFY stateChanged)
    Q_PROPERTY(qint64 sourceId READ sourceId NOTIFY stateChanged)
    Q_PROPERTY(QStringList devices READ devices NOTIFY devicesChanged) // the outputs of the PC
    Q_PROPERTY(QString device READ device WRITE setDevice NOTIFY settingsChanged) // empty: the default one
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY settingsChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY settingsChanged) // 0 to 1
    Q_PROPERTY(bool normalized READ normalized WRITE setNormalized NOTIFY settingsChanged)

public:
    explicit AudioPlayer(AppSettings *settings = nullptr, QObject *parent = nullptr);
    ~AudioPlayer() override; // stops, and waits for a preparation in progress

    bool playing() const { return m_playing; }
    bool preparing() const { return m_preparing; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    QString error() const { return m_error; }
    QString description() const { return m_description; }
    QObject *source() const { return m_source.data(); }
    qint64 sourceId() const { return m_sourceId; }
    QStringList devices() const { return m_devices; }
    QString device() const { return m_device; }
    void setDevice(const QString &device);
    bool loop() const { return m_loop; }
    void setLoop(bool loop);
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool normalized() const { return m_normalized; }
    void setNormalized(bool normalized);

    // Signal `signalId` of `session` (a Session), its channels (indices: one or two). False (error) when
    // there is nothing to play or no output; the sound starts once ready (started), or failed says why.
    Q_INVOKABLE bool play(QObject *session, qint64 signalId, const QVariantList &channels);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void refreshDevices();

    // Tests: returns once the preparation in progress is done (and its result handled).
    void waitForPrepared();

signals:
    void stateChanged();
    void positionChanged();
    void devicesChanged();
    void settingsChanged();
    void started(const QString &text); // what plays and how, its warnings
    void failed(const QString &error);

private:
    void prepared(const std::shared_ptr<analysis::PlaybackBuffer> &buffer,
                  const std::shared_ptr<const std::vector<std::uint8_t>> &bytes, const QAudioDevice &device,
                  const QAudioFormat &format, quint64 ticket);
    void sinkStateChanged();
    void fail(const QString &error);
    void restart(); // with the settings changed, what plays plays again
    void save();

    QPointer<AppSettings> m_settings;
    QPointer<QObject> m_source;
    qint64 m_sourceId = 0;
    QVariantList m_channels;
    QThreadPool m_pool;
    quint64 m_ticket = 0; // of the last play() or stop(): an older preparation is dropped
    std::shared_ptr<std::atomic<bool>> m_cancel; // of the preparation in progress: set, it stops early
    std::unique_ptr<PlaybackDevice> m_bytes;
    std::unique_ptr<QAudioSink> m_sink;
    QTimer m_clock;
    bool m_playing = false;
    bool m_preparing = false;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_error;
    QString m_signalName; // of what plays
    QString m_description;
    QStringList m_devices;
    QString m_device;
    bool m_loop = false;
    double m_volume = 0.8;
    bool m_normalized = true;
};
} // namespace qacq::app
