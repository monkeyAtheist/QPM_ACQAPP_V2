#pragma once
// The curves added under the signal in the module "Signaux" (qacq/analysis/curves.h).
//
// The list belongs to the session (saved in its file) and applies to the signal chosen: another signal,
// or a new version of it, and the curves are computed again. Curves of time (integral, derivative, values
// per period, the channels themselves) are computed on a thread of their own and shared read-only with
// the plot (timeCurves); each one is kept until its signal, part or options change. Spectra are computed
// by the AnalysisController, which is told which channels to do. Spectrograms take seconds on a long
// signal: they have a thread of their own too, stop early when they are no longer wanted, and tell how far
// they are (spectrogramProgress); their segments come from the settings of the AnalysisController.
#include "qacq/analysis/curves.h"

#include <QObject>
#include <QUrl>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace qacq::app
{
class AnalysisController;
class Session;

class CurvesController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64 signalId READ signalId WRITE setSignalId NOTIFY signalIdChanged)
    // One map per curve, in the order of the list: id, kind, kindName, channel, name, unit, graph ("time",
    // "frequency" or "spectrogram"), visible, ready (computed), colorIndex (its colour in its graph), part
    // ("" for all the signal, else "1,2 s à 3,4 s"), removeMean, missing (its channel is not in the signal).
    Q_PROPERTY(QVariantList curves READ curves NOTIFY curvesChanged)
    Q_PROPERTY(bool hasTimeCurves READ hasTimeCurves NOTIFY curvesChanged)
    Q_PROPERTY(bool hasSpectra READ hasSpectra NOTIFY curvesChanged)
    Q_PROPERTY(bool hasSpectrograms READ hasSpectrograms NOTIFY curvesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // A spectrogram being computed, and how far (0 to 1).
    Q_PROPERTY(bool spectrogramBusy READ spectrogramBusy NOTIFY spectrogramBusyChanged)
    Q_PROPERTY(double spectrogramProgress READ spectrogramProgress NOTIFY spectrogramProgressChanged)

public:
    // analysis may be null (tests of the curves of time alone).
    CurvesController(Session *session, AnalysisController *analysis, QObject *parent = nullptr);
    ~CurvesController() override;

    qint64 signalId() const { return m_signalId; }
    void setSignalId(qint64 id);
    QVariantList curves() const;
    bool hasTimeCurves() const;
    bool hasSpectra() const;
    bool hasSpectrograms() const;
    bool busy() const { return m_running; }
    bool spectrogramBusy() const { return m_spectrogramRunning; }
    double spectrogramProgress() const { return m_spectrogramProgressShown; }

    // Adds curves of `kind` ("spectrum", "source", "integral", "derivative", "cycleMean", "cycleRms",
    // "cycleFrequency") on the channels given. Options: removeMean (integral), firstFrame and frameCount
    // (curves of time; frameCount < 0 or absent: up to the end), withSources (the channels themselves too,
    // to lay the curves over them). A curve already in the list is shown again rather than added twice.
    // Returns how many curves were added or shown again.
    Q_INVOKABLE int add(const QString &kind, const QVariantList &channels, const QVariantMap &options = QVariantMap());
    Q_INVOKABLE void remove(int id);
    Q_INVOKABLE void setVisible(int id, bool visible);
    // Removes the curves of one graph ("time", "frequency"), or all of them (empty).
    Q_INVOKABLE void clear(const QString &graph = QString());
    // The curves of time on screen as a new signal of the session (one step of the history): its id, 0
    // on failure (the list model tells why).
    Q_INVOKABLE qint64 keepAsSignal();
    // The curves of time on screen in a CSV file for a spreadsheet, as the spectra are: the time, then one
    // column per curve ("nan" where a curve has no value), decimal comma, ';' between columns, UTF-8 with
    // its mark. False on failure (the list model tells why).
    Q_INVOKABLE bool exportTimeCurves(const QUrl &url);

    // What the plot of the curves of time draws (null when there is no signal). Never changes once given.
    std::shared_ptr<const analysis::TimeCurveSet> timeCurves() const { return m_set; }
    // What the plot of the spectrograms draws (null when there is no signal). Never changes once given.
    std::shared_ptr<const analysis::SpectrogramSet> spectrograms() const { return m_spectrograms; }
    const std::vector<analysis::CurveSpec> &specs() const { return m_specs; }
    // Waits for the computation in progress (tests).
    void waitForIdle();

signals:
    void signalIdChanged();
    void curvesChanged();
    void timeCurvesChanged();
    void busyChanged();
    void spectrogramsChanged();
    void spectrogramBusyChanged();
    void spectrogramProgressChanged();

private:
    struct Cached
    {
        const void *data = nullptr; // samples of the signal it was computed from
        double rateHz = 0.0;
        analysis::CurveSpec spec;
        std::shared_ptr<const std::vector<double>> values;
    };
    using Results = std::map<std::uint64_t, std::shared_ptr<const std::vector<double>>>;
    struct SpectrogramCached
    {
        const void *data = nullptr;
        double rateHz = 0.0;
        analysis::CurveSpec spec;
        std::string settings; // the segments and the window it was computed with
        std::shared_ptr<const dsp::Spectrogram> result;
    };
    using SpectrogramResults = std::map<std::uint64_t, std::shared_ptr<const dsp::Spectrogram>>;

    void changed(bool save); // the list changed: saved, the spectra handed over, computed again
    void save();
    void load(); // from the settings of the session
    void compute();
    void finished(quint64 generation, model::SignalPtr signal, Results results);
    void rebuild(); // the set of the plot, from what is computed
    void handOverSpectra();
    // Spectrograms: computed on their own thread, one after the other; a new request stops the one in
    // progress (it is then started again with what is wanted now).
    void computeSpectrograms();
    void spectrogramsFinished(quint64 generation, model::SignalPtr signal, std::string settings, SpectrogramResults results);
    void rebuildSpectrograms();
    analysis::SpectrogramSettings spectrogramSettings() const;
    static std::string settingsKey(const analysis::SpectrogramSettings &settings);
    bool spectrogramCached(const analysis::CurveSpec &spec, const model::Signal &signal, const std::string &settings) const;
    model::SignalPtr currentSignal() const;
    bool cached(const analysis::CurveSpec &spec, const model::Signal &signal) const;
    QString partText(const analysis::CurveSpec &spec, const model::Signal *signal) const;

    QPointer<Session> m_session;
    QPointer<AnalysisController> m_analysis;
    qint64 m_signalId = 0;
    std::vector<analysis::CurveSpec> m_specs;
    std::uint64_t m_nextId = 1;
    std::map<std::uint64_t, Cached> m_cache;
    std::shared_ptr<const analysis::TimeCurveSet> m_set;
    QThreadPool m_pool;
    bool m_running = false;
    bool m_again = false;
    quint64 m_generation = 0;
    bool m_loading = false;

    std::map<std::uint64_t, SpectrogramCached> m_spectrogramCache;
    std::shared_ptr<const analysis::SpectrogramSet> m_spectrograms;
    QThreadPool m_spectrogramPool;
    bool m_spectrogramRunning = false;
    bool m_spectrogramAgain = false;
    quint64 m_spectrogramGeneration = 0;
    // What the thread of the spectrograms shares with this one while it runs.
    struct SpectrogramRun
    {
        std::atomic<bool> cancelled{false};
        std::atomic<int> permille{0}; // of the spectrogram in progress
        std::atomic<int> index{0};    // which one of `total`
        int total = 1;
    };
    std::shared_ptr<SpectrogramRun> m_spectrogramRun;
    double m_spectrogramProgressShown = 0.0;
    QTimer m_progressTimer; // reads how far the computation is, while it runs
    std::string m_spectrogramSettingsKey; // of the last request (a change of the settings recomputes)
};
} // namespace qacq::app
