#pragma once
// The analysis of the module "Signaux": the spectra and the measurements of a zone of a signal of the session.
//
// The view says which signal and which zone (the part of it on screen) and the settings; the computation
// (qacq::analysis::analyse) runs on a thread of its own, one at a time: a request that comes while one
// runs cancels it and starts again with the latest values once it stops. Zones that change while the
// user zooms are gathered for a moment first. The result is shared read-only with the spectrum plot.
#include "qacq/analysis/analysis.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace qacq::app
{
class Session;

class AnalysisController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64 signalId READ signalId WRITE setSignalId NOTIFY signalIdChanged)
    // Settings of the computation and of the display, as qacq::analysis::toJson writes them.
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(bool twoSided READ twoSided NOTIFY resultChanged) // I/Q: frequencies from −fs/2 to +fs/2
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY resultChanged)  // "Hann · FFT de 4 800 points · Δf = 10 Hz"
    Q_PROPERTY(QString zoneText READ zoneText NOTIFY resultChanged) // "1,2 s à 3,4 s · 4 800 éch."
    Q_PROPERTY(QStringList notes READ notes NOTIFY resultChanged)
    // One map per channel: name, unit, channel, values [{label, value}], tone, toneText, toneValues,
    // harmonics [{order, frequency, amplitude, relative}].
    Q_PROPERTY(QVariantList measures READ measures NOTIFY resultChanged)
    // One map per curve of the spectrum: name, channel, levelUnit, peak ("1,000 kHz : −6,02 dBV").
    Q_PROPERTY(QVariantList curves READ curves NOTIFY resultChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit AnalysisController(Session *session, QObject *parent = nullptr);
    ~AnalysisController() override;

    qint64 signalId() const { return m_signalId; }
    void setSignalId(qint64 id);
    QVariantMap settings() const;
    bool busy() const { return m_running; }
    bool hasResult() const;
    bool twoSided() const { return m_result && m_result->twoSided; }
    QString error() const;
    QString summary() const;
    QString zoneText() const;
    QStringList notes() const;
    QVariantList measures() const;
    QVariantList curves() const;
    QString lastError() const { return m_lastError; }

    // The part of the signal to analyse (frames); count < 0: the whole signal. Gathered for 150 ms.
    Q_INVOKABLE void setZone(qint64 firstFrame, qint64 frameCount);
    // Changes some settings (keys of qacq::analysis::toJson); the others stay.
    Q_INVOKABLE void setSettings(const QVariantMap &values);
    // The channels whose spectrum is computed (the curves added in the module); the measurements cover
    // every channel whatever the list. Until called: every channel.
    void setSpectrumChannels(const std::vector<std::size_t> &channels);
    // Settings saved as JSON text (AppSettings), and back.
    Q_INVOKABLE QString settingsText() const;
    Q_INVOKABLE void restoreSettings(const QString &text);
    // The spectrum as a CSV table (";" and decimal comma, UTF-8 with its mark for spreadsheets).
    Q_INVOKABLE bool exportSpectrum(const QUrl &url);

    // The result on screen (null before the first one). Shared: it never changes once published.
    std::shared_ptr<const analysis::AnalysisResult> result() const { return m_result; }
    const analysis::AnalysisSettings &analysisSettings() const { return m_settings; }
    // Waits for the computation in progress (tests).
    void waitForIdle();

signals:
    void signalIdChanged();
    void settingsChanged();
    void busyChanged();
    void resultChanged();
    void lastErrorChanged();

private:
    void schedule(int delayMs);
    void start();
    void finished(quint64 generation, std::shared_ptr<const analysis::AnalysisResult> result, model::SignalPtr signal);
    void onSessionChanged();
    model::SignalPtr currentSignal() const; // the version of the chosen signal in the session now
    QString bandText(const analysis::HalfPowerBand &band, double binWidthHz) const;
    void setLastError(const QString &message);

    QPointer<Session> m_session;
    qint64 m_signalId = 0;
    qint64 m_firstFrame = 0;
    qint64 m_frameCount = -1;
    bool m_allSpectra = true;
    std::vector<std::size_t> m_spectrumChannels;
    analysis::AnalysisSettings m_settings;
    model::SignalPtr m_analysed; // the version of the signal the result (or the run) is about

    QTimer m_delay;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    quint64 m_generation = 0;
    bool m_running = false;
    bool m_again = false;

    std::shared_ptr<const analysis::AnalysisResult> m_result;
    model::SignalPtr m_resultSignal; // the version of the signal m_result was computed on
    QString m_lastError;
};
} // namespace qacq::app
