#pragma once

// The module "Filtres": the filter being designed (its recipe, its design, what it does), tried on a signal
// of the session (whose rate the design takes, unless one is typed): a preview of one channel before and
// after, on a thread of its own (the newest wins), with the spectra of both; then the whole signal
// filtered, added to the session in one step of the history.
#include "qacq/analysis/filters.h"
#include "qacq/app/analysis_controller.h"
#include "qacq/app/session.h"

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

namespace qacq::app
{
class FilterController : public QObject
{
    Q_OBJECT
    // The recipe: {kind ("iir", "fir"), family, response, order, taps, window, kaiserBeta, sampleRate,
    // rateOfSignal, frequency1, frequency2, ripple, attenuation, application} (analysis::toJson).
    Q_PROPERTY(QVariantMap recipe READ recipe NOTIFY recipeChanged)
    // The design: {ok, error, warnings, text, summary (lines), sections [{b0, b1, b2, a1, a2}] or tapCount,
    // coefficients (to read: « b0 = … » a line, the first ones of a long FIR), poleCount, zeroCount,
    // rootsKnown, sampleRate}.
    Q_PROPERTY(QVariantMap design READ designInfo NOTIFY designChanged)
    // The signal of the session tried (0: none), and its channel shown before and after.
    Q_PROPERTY(qint64 inputId READ inputId WRITE setInputId NOTIFY inputChanged)
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY inputChanged)
    // {name, sampleRate, channels [names], frames}; empty without one.
    Q_PROPERTY(QVariantMap input READ inputInfo NOTIFY inputChanged)
    Q_PROPERTY(QObject *preview READ preview CONSTANT)
    Q_PROPERTY(qint64 previewId READ previewId CONSTANT)
    Q_PROPERTY(bool hasPreview READ hasPreview NOTIFY stateChanged)
    Q_PROPERTY(QObject *previewAnalysis READ previewAnalysis CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString previewError READ previewError NOTIFY stateChanged)
    Q_PROPERTY(QStringList previewWarnings READ previewWarnings NOTIFY stateChanged)
    Q_PROPERTY(QString previewNote READ previewNote NOTIFY stateChanged)
    // Computed only while the module is on screen.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool adding READ adding NOTIFY addingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantList examples READ examples CONSTANT)

public:
    // At most this many samples in the preview (the first ones).
    static constexpr std::size_t kPreviewFrames = 4194304;
    // At most this many lines of coefficients in `design.coefficients`.
    static constexpr qsizetype kShownCoefficientLines = 600;

    explicit FilterController(Session *session, QObject *parent = nullptr);
    ~FilterController() override;

    QVariantMap recipe() const;
    QVariantMap designInfo() const { return m_designInfo; }
    qint64 inputId() const { return m_inputId; }
    void setInputId(qint64 id);
    int channel() const { return m_channel; }
    void setChannel(int channel);
    QVariantMap inputInfo() const;
    QObject *preview() { return &m_preview; }
    Session *previewSession() { return &m_preview; }
    static qint64 previewId() { return kPreviewId; }
    bool hasPreview() const { return m_previewSignal != nullptr; }
    model::SignalPtr previewSignal() const { return m_previewSignal; }
    QObject *previewAnalysis() { return &m_analysis; }
    bool busy() const { return m_running; }
    double progress() const { return m_progress; }
    bool ready() const;
    QString previewError() const { return m_error; }
    QStringList previewWarnings() const { return m_warnings; }
    QString previewNote() const { return m_note; }
    bool active() const { return m_active; }
    void setActive(bool active);
    bool adding() const { return m_adding; }
    QString lastError() const { return m_lastError; }
    QVariantList examples() const;

    // The members given change (others stay): a change of kind gives the usual application of the new one
    // (a FIR aligned, an IIR causal), a band gets an upper edge above its lower one, "rateOfSignal" true
    // takes the rate of the signal tried.
    Q_INVOKABLE void setRecipe(const QVariantMap &changes);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void useExample(int index);
    Q_INVOKABLE QString recipeText() const; // JSON on one line
    // All the coefficients, to copy: what the filter is, then « b0 = … » a line by section, or h[k].
    Q_INVOKABLE QString coefficientsText() const;
    Q_INVOKABLE void restoreRecipe(const QString &text);
    // The whole signal tried, every channel filtered, as a new signal of the session (on a thread): added()
    // with its id, or lastError.
    Q_INVOKABLE void addToSession();

    // For the plots: the design shown, and its impulse and step responses (computed once per design).
    std::shared_ptr<const analysis::FilterDesign> currentDesign() const { return m_design; }
    std::shared_ptr<const analysis::ImpulseResponse> currentImpulse() const;
    analysis::FilterRecipe currentRecipe() const { return m_recipe; }

    // Tests: starts what waits for its delay, and returns once the preview (and an addition) is done.
    void waitForIdle();

signals:
    void recipeChanged();
    void designChanged();
    void inputChanged();
    void busyChanged();
    void progressChanged();
    void stateChanged();
    void activeChanged();
    void addingChanged();
    void lastErrorChanged();
    void added(qint64 id);

private:
    static constexpr qint64 kPreviewId = 1;
    struct Run
    {
        std::atomic<bool> cancelled{false};
        std::atomic<int> permille{0};
    };
    struct Outcome; // of a preview

    void replace(analysis::FilterRecipe recipe);
    void redesign();
    void refreshDesignInfo();
    QStringList coefficientLines(qsizetype maximum) const;
    void refreshInput();
    void schedule();
    void start();
    void finished(const std::shared_ptr<Run> &run, const std::shared_ptr<Outcome> &outcome, quint64 version);
    void setRunning(bool running);
    void pollProgress();
    void clearPreview();
    void setLastError(const QString &message);
    model::SignalPtr inputSignal() const;

    QPointer<Session> m_session;
    Session m_preview;             // before the analysis that watches it
    AnalysisController m_analysis; // of the preview: the spectra before and after
    analysis::FilterRecipe m_recipe;
    std::shared_ptr<const analysis::FilterDesign> m_design;
    QVariantMap m_designInfo;
    mutable std::shared_ptr<const analysis::ImpulseResponse> m_impulse;
    qint64 m_inputId = 0;
    int m_channel = 0;
    double m_inputRate = 0.0; // of the signal tried, as last seen
    std::weak_ptr<const model::Signal> m_seenInput; // the version of it the preview is about
    bool m_seenNone = false;                         // or none was seen last
    quint64 m_version = 1;    // of what the preview depends on (design, input, channel)
    quint64 m_previewVersion = 0;
    model::SignalPtr m_previewSignal;
    QString m_error;
    QStringList m_warnings;
    QString m_note;
    QString m_lastError;

    QTimer m_delay;
    QTimer m_progressTimer;
    QThreadPool m_pool;
    std::shared_ptr<Run> m_run;
    bool m_running = false;
    bool m_again = false;
    bool m_active = true;
    bool m_adding = false;
    double m_progress = 0.0;
};
} // namespace qacq::app
