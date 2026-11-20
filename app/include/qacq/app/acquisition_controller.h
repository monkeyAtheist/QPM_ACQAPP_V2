#pragma once

// Drives one acquisition for the interface: starts the source on its thread, empties the pipe about 30
// times a second into the recorder, measures the rate, records to disk if asked, runs the trigger, and
// when the acquisition stops adds what was recorded to the session as an "acquired" signal (one undo step).
//
// Settings are exchanged with QML as the JSON of acquisition::AcquisitionSettings (a QVariantMap); a
// partial map only changes the members it holds.

#include "qacq/acquisition/rate_estimator.h"
#include "qacq/acquisition/recorder.h"
#include "qacq/acquisition/settings.h"
#include "qacq/acquisition/stream_writer.h"
#include "qacq/acquisition/trigger.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <vector>

class QThread;

namespace qacq::app
{
class Session;
class SourceWorker;
struct SourceShared;

class AcquisitionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged) // "idle", "connecting", "running", "error"
    Q_PROPERTY(bool running READ isRunning NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY warningsChanged)
    Q_PROPERTY(QVariantMap settings READ settings WRITE setSettings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap statistics READ statistics NOTIFY statisticsChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY channelsChanged)
    Q_PROPERTY(QStringList channelNames READ channelNames NOTIFY channelsChanged)
    Q_PROPERTY(double sampleRate READ sampleRate NOTIFY statisticsChanged)
    Q_PROPERTY(double measuredRate READ measuredRate NOTIFY statisticsChanged)
    Q_PROPERTY(QString instrument READ instrument NOTIFY instrumentChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(qint64 lastSignalId READ lastSignalId NOTIFY lastSignalIdChanged)

public:
    enum class State
    {
        Idle,
        Connecting,
        Running,
        Error
    };

    explicit AcquisitionController(Session *session = nullptr, QObject *parent = nullptr);
    ~AcquisitionController() override;

    QString state() const;
    State stateValue() const { return m_state; }
    bool isRunning() const { return m_state == State::Connecting || m_state == State::Running; }
    QString statusText() const { return m_status; }
    QString lastError() const { return m_lastError; }
    QStringList warnings() const { return m_warnings; }
    QVariantMap settings() const;
    void setSettings(const QVariantMap &settings);
    QVariantMap statistics() const;
    int channelCount() const { return static_cast<int>(m_channelNames.size()); }
    QStringList channelNames() const { return m_channelNames; }
    double sampleRate() const { return effectiveRate(); }
    double measuredRate() const;
    QString instrument() const { return m_instrument; }
    bool isRecording() const { return m_recording; }
    qint64 lastSignalId() const { return m_lastSignalId; }

    Q_INVOKABLE bool start();
    // Stops; with keep, what was recorded becomes a signal of the session. Returns its id (0 if none).
    Q_INVOKABLE qint64 stop(bool keep = true);
    // Forgets the frames recorded so far, the acquisition goes on.
    Q_INVOKABLE void clear();
    // Continuous recording to disk while acquiring: options format ("csv" | "wav"), decimalComma,
    // timeColumn, wavFloat, fullScale.
    Q_INVOKABLE bool startRecording(const QUrl &file, const QVariantMap &options = QVariantMap());
    Q_INVOKABLE void stopRecording();
    // {enabled, channel, level, hysteresis, edge ("rising" | "falling" | "either"), mode ("auto" | "normal" |
    // "single"), length, preTrigger, holdoff}
    Q_INVOKABLE void setTrigger(const QVariantMap &trigger);
    Q_INVOKABLE void armTrigger();
    // {firstFrame, length, triggerFrame, triggerFraction, forced, rising, channels: [[values]...]}; empty map
    // before the first capture.
    Q_INVOKABLE QVariantMap lastCapture() const;
    Q_INVOKABLE QString validate(const QVariantMap &settings) const;
    Q_INVOKABLE QVariantMap defaultSettings() const;
    // "arduino" or "c": example firmware sending the format of the current settings.
    Q_INVOKABLE QString firmwareExample(const QString &target) const;
    Q_INVOKABLE QStringList audioInputs() const;

    // C++ side (display, tests).
    const acquisition::AcquisitionSettings &acquisitionSettings() const { return m_settings; }
    void setAcquisitionSettings(const acquisition::AcquisitionSettings &settings);
    const acquisition::LiveRecorder &recorder() const { return m_recorder; }
    void setRecorderLimits(const acquisition::RecorderLimits &limits);
    void setPollInterval(int milliseconds);
    void poll(); // empties the pipe now (the timer calls it)

signals:
    void stateChanged();
    void lastErrorChanged();
    void warningsChanged();
    void settingsChanged();
    void statisticsChanged();
    void channelsChanged();
    void instrumentChanged();
    void recordingChanged();
    void lastSignalIdChanged();
    void framesArrived(qint64 totalFrames);
    void triggered(const QVariantMap &capture);

private:
    void setState(State state, const QString &status);
    void onOpened(const QString &description);
    void onFailed(const QString &message);
    void addWarning(const QString &message);
    void shutdownWorker();
    void updateChannelNames();
    double effectiveRate() const;
    qint64 keepSignal();
    QVariantMap captureToMap(const acquisition::TriggerCapture &capture, bool withValues) const;

    QPointer<Session> m_session;
    acquisition::AcquisitionSettings m_settings; // edited by the interface
    acquisition::AcquisitionSettings m_running;  // used by the acquisition in progress
    State m_state = State::Idle;
    QString m_status;
    QString m_lastError;
    QStringList m_warnings;

    QThread *m_thread = nullptr;
    std::unique_ptr<SourceWorker> m_worker;
    std::shared_ptr<SourceShared> m_shared;
    std::shared_ptr<acquisition::SamplePipe> m_pipe;
    quint64 m_generation = 0; // signals of an earlier worker are ignored

    acquisition::LiveRecorder m_recorder;
    acquisition::RecorderLimits m_limits;
    acquisition::RateEstimator m_rate;
    acquisition::RateEstimator m_byteRate;
    acquisition::StreamWriter m_writer;
    bool m_recording = false;
    acquisition::Trigger m_trigger;
    bool m_triggerEnabled = false;
    acquisition::TriggerCapture m_lastCapture;
    bool m_haveCapture = false;
    std::uint64_t m_pulled = 0;

    QTimer m_timer;
    int m_pollMilliseconds = 33;
    QElapsedTimer m_clock;
    QString m_startedUtc;
    QStringList m_channelNames;
    QString m_instrument;
    qint64 m_lastSignalId = 0;
    std::vector<double> m_values;
    std::vector<std::uint64_t> m_breaks;
    std::vector<double> m_triggerSamples;
};
} // namespace qacq::app
