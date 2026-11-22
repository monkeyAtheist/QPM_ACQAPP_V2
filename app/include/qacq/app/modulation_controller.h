#pragma once

// The module "Modulations": a digital link simulated end to end (analysis::LinkRecipe: the bits, the modulator,
// the noise, the receiver), computed again a moment after each change on a thread of its own (the newest wins),
// shown as a preview (the signals sent and received, their spectra; the constellation and the eye diagram read
// the result here); the bit error rate against Eb/N0, measured point by point on its own thread, as curves
// (several, to compare modulations); and the signals sent or received added to the session in one step.
#include "qacq/analysis/link.h"
#include "qacq/app/analysis_controller.h"
#include "qacq/app/session.h"

#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <vector>

namespace qacq::app
{
class ModulationController : public QObject
{
    Q_OBJECT
    // The recipe: {modulation ("ask", "fsk", "bpsk", "qpsk", "8psk", "16qam", "64qam", "256qam"), representation
    // ("passband", "iq"), sampleRate, symbolRate, carrier, amplitude, phase, pulse ("rect", "rrc"), rolloff, askLow,
    // fskDeviation, data ("random", "text", "bits"), dataSeed, text, uart, bits, symbols, noise, ebN0, noiseSeed}.
    Q_PROPERTY(QVariantMap recipe READ recipe NOTIFY recipeChanged)
    // What the link gave: {ok, error, warnings, text (the link in words), summary (lines), bitsPerSymbol,
    // constellationSize, fsk, iq, rrc, bitRate, bandwidth, samplesPerSymbol, symbols, skipSymbols, comparedBits,
    // bitErrors, ber, berLower, berUpper, theoryBer, comparedSymbols, symbolErrors, ser, theorySer, measuredEbN0,
    // evm, mer (NaN where it means nothing); the rates as read: berText, berLowerText, berUpperText, theoryText,
    // serText, theorySerText ("1,2×10⁻⁴"); a text: textSent, textReceived, textErrors}.
    Q_PROPERTY(QVariantMap result READ resultInfo NOTIFY resultChanged)
    Q_PROPERTY(QObject *preview READ preview CONSTANT)
    Q_PROPERTY(qint64 previewId READ previewId CONSTANT)
    Q_PROPERTY(bool hasPreview READ hasPreview NOTIFY stateChanged)
    Q_PROPERTY(QObject *previewAnalysis READ previewAnalysis CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    // Computed only while the module is on screen.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantList examples READ examples CONSTANT)
    // The curves of the bit error rate: how each is measured {from, to, step, maxBits, targetErrors} (dB, bits), and
    // the curves measured, oldest first [{label, done, points [{ebN0, bits, errors, ber, lower, upper, theory}]}].
    Q_PROPERTY(QVariantMap sweep READ sweep NOTIFY sweepChanged)
    Q_PROPERTY(QString sweepError READ sweepError NOTIFY sweepChanged)
    Q_PROPERTY(QVariantList curves READ curvesInfo NOTIFY curvesChanged)
    Q_PROPERTY(bool curveBusy READ curveBusy NOTIFY curveBusyChanged)
    Q_PROPERTY(double curveProgress READ curveProgress NOTIFY curveProgressChanged)

public:
    // At most this many curves kept (the oldest goes).
    static constexpr int kMaximumCurves = 6;

    explicit ModulationController(Session *session, QObject *parent = nullptr);
    ~ModulationController() override;

    QVariantMap recipe() const;
    QVariantMap resultInfo() const { return m_resultInfo; }
    QObject *preview() { return &m_preview; }
    Session *previewSession() { return &m_preview; }
    static qint64 previewId() { return kPreviewId; }
    bool hasPreview() const { return m_previewSignal != nullptr; }
    QObject *previewAnalysis() { return &m_analysis; }
    bool busy() const { return m_running; }
    bool ready() const;
    bool active() const { return m_active; }
    void setActive(bool active);
    QString lastError() const { return m_lastError; }
    QVariantList examples() const;
    QVariantMap sweep() const;
    QString sweepError() const { return m_sweepError; }
    QVariantList curvesInfo() const;
    bool curveBusy() const { return m_curveRunning; }
    double curveProgress() const { return m_curveProgress; }

    // The members given change (others stay).
    Q_INVOKABLE void setRecipe(const QVariantMap &changes);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void useExample(int index);
    Q_INVOKABLE QString recipeText() const; // JSON on one line
    Q_INVOKABLE void restoreRecipe(const QString &text);
    Q_INVOKABLE void setSweep(const QVariantMap &changes);
    Q_INVOKABLE QString sweepText() const;
    Q_INVOKABLE void restoreSweep(const QString &text);
    // A new curve for the recipe as it is (random bits; its modulation, pulses, carrier and rate), point by point.
    Q_INVOKABLE void startCurve();
    Q_INVOKABLE void stopCurve();
    Q_INVOKABLE void clearCurves();
    // The signal sent, or received (with its noise), as a new signal of the session: added() with its id, or
    // lastError.
    Q_INVOKABLE void addToSession(bool received);

    // For the plots.
    std::shared_ptr<const analysis::LinkResult> currentResult() const { return m_result; }
    analysis::LinkRecipe currentRecipe() const { return m_recipe; }
    struct Curve
    {
        analysis::LinkRecipe recipe;
        std::string label;
        std::vector<analysis::BerPoint> points;
        bool done = false;
    };
    const std::vector<Curve> &curves() const { return m_curves; }

    // Tests: starts what waits for its delay, and returns once the link (and a curve) is done.
    void waitForIdle();

signals:
    void recipeChanged();
    void resultChanged();
    void stateChanged();
    void busyChanged();
    void activeChanged();
    void lastErrorChanged();
    void sweepChanged();
    void curvesChanged();
    void curveBusyChanged();
    void curveProgressChanged();
    void added(qint64 id);

private:
    static constexpr qint64 kPreviewId = 1;
    struct Run
    {
        std::atomic<bool> cancelled{false};
        std::atomic<int> permille{0};
    };

    void replace(analysis::LinkRecipe recipe);
    void schedule();
    void start();
    void finished(const std::shared_ptr<Run> &run, const std::shared_ptr<const analysis::LinkResult> &result, quint64 version);
    void refreshResultInfo();
    void setRunning(bool running);
    void curvePoint(const std::shared_ptr<Run> &run, std::size_t index, const analysis::BerPoint &point, bool last);
    void setLastError(const QString &message);

    QPointer<Session> m_session;
    Session m_preview;             // before the analysis that watches it
    AnalysisController m_analysis; // of the preview: the spectra sent and received
    analysis::LinkRecipe m_recipe;
    std::shared_ptr<const analysis::LinkResult> m_result;
    QVariantMap m_resultInfo;
    model::SignalPtr m_previewSignal;
    quint64 m_version = 1;
    quint64 m_resultVersion = 0;
    QString m_lastError;

    analysis::BerSweep m_sweep;
    QString m_sweepError;
    std::vector<Curve> m_curves;
    std::shared_ptr<Run> m_curveRun;
    bool m_curveRunning = false;
    double m_curveProgress = 0.0;
    std::size_t m_curvePoints = 0; // of the curve being measured

    QTimer m_delay;
    QTimer m_progressTimer;
    QThreadPool m_pool;      // the link
    QThreadPool m_curvePool; // the curve, apart: the link keeps up while it runs
    std::shared_ptr<Run> m_run;
    bool m_running = false;
    bool m_again = false;
    bool m_active = true;
};
} // namespace qacq::app
