#pragma once

// Module "Générateur": the recipe being edited (channels, each the sum of components:
// qacq/analysis/generation.h), its signal computed on a thread of its own a moment after each change and
// shown in a preview apart (a session of one signal, with its own analysis: spectrum and measurements),
// then added to the session on request, as one step of the history. Each signal made keeps its recipe in
// its provenance: takeUp() brings it back. The page remembers the recipe from one run to the next
// (recipeText, restoreRecipe, through AppSettings).

#include "qacq/analysis/generation.h"
#include "qacq/app/analysis_controller.h"
#include "qacq/app/session.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

namespace qacq::app
{
class GeneratorController : public QObject
{
    Q_OBJECT
    // The preview: a session of one signal (previewId), and its analysis.
    Q_PROPERTY(qacq::app::Session *preview READ preview CONSTANT)
    Q_PROPERTY(qint64 previewId READ previewId CONSTANT)
    Q_PROPERTY(qacq::app::AnalysisController *previewAnalysis READ previewAnalysis CONSTANT)
    // The recipe as qacq::analysis::toJson writes it: name, sampleRate, duration, seed, channels [{name,
    // unit, offset, components [{waveform, enabled, amplitude, frequency, phase, duty, time, endFrequency,
    // law ("linear", "log"), noise ("gaussian", "uniform"), bandLimit ("sampled", "fourier"), harmonics; a
    // Fourier series: form, variables, a0, an, bn; a drawn motif: motif [[x, y]...], interpolation ("steps",
    // "linear", "smooth"), once; a recurrence: recurrence, initial, input ("none", "impulse", "step", "noise",
    // "above"), variables}]}].
    Q_PROPERTY(QVariantMap recipe READ recipe NOTIFY recipeChanged)
    Q_PROPERTY(qint64 sampleCount READ sampleCount NOTIFY recipeChanged) // per channel
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY recipeChanged)      // "48 000 échantillons · 375,0 Kio"
    Q_PROPERTY(QString problem READ problem NOTIFY recipeChanged)        // why it cannot be generated, or empty
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0 to 1, while busy
    // The preview is the recipe as it is now (nothing waiting, no error): it can be added to the session.
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool hasPreview READ hasPreview NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY stateChanged)
    Q_PROPERTY(QVariantList examples READ examples CONSTANT) // [{name, tip}], for useExample(index)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // Generates only while active (the module on screen): a recipe of fifty million samples brought back
    // at start-up waits until the module is opened. Active when made.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit GeneratorController(Session *session, QObject *parent = nullptr);
    ~GeneratorController() override; // the generation in progress is stopped, and waited for

    Session *preview() { return &m_preview; }
    qint64 previewId() const { return kPreviewId; }
    AnalysisController *previewAnalysis() { return &m_analysis; }
    QVariantMap recipe() const;
    qint64 sampleCount() const;
    QString sizeText() const;
    QString problem() const;
    bool busy() const { return m_running; }
    double progress() const { return m_progress; }
    bool ready() const;
    bool hasPreview() const { return m_previewSignal != nullptr; }
    QString error() const { return m_error; }
    QStringList warnings() const { return m_warnings; }
    QVariantList examples() const;
    QString lastError() const { return m_lastError; }
    bool active() const { return m_active; }
    void setActive(bool active);

    const analysis::GeneratorRecipe &currentRecipe() const { return m_recipe; }
    model::SignalPtr previewSignal() const { return m_previewSignal; }

    // Edits of the recipe (indices from 0); the preview follows a moment after the last one.
    Q_INVOKABLE void setRecipe(const QVariantMap &recipe);
    Q_INVOKABLE void setSignalSettings(const QVariantMap &values); // name, sampleRate, duration, seed
    Q_INVOKABLE int addChannel();                                  // its index, -1 when there are 16
    Q_INVOKABLE void removeChannel(int channel);                   // the last one stays
    Q_INVOKABLE void setChannel(int channel, const QVariantMap &values); // name, unit, offset
    // A component of that waveform ("sine", "square"... "noise", "sweep"), with usual values; its index,
    // -1 when there are 32.
    Q_INVOKABLE int addComponent(int channel, const QString &waveform);
    Q_INVOKABLE void removeComponent(int channel, int component);
    Q_INVOKABLE void moveComponent(int channel, int from, int to);
    // Any key of a component; another waveform keeps what both share (analysis::withWaveform); another form
    // of the coefficients of a series keeps the series (analysis::withSeriesForm).
    Q_INVOKABLE void setComponent(int channel, int component, const QVariantMap &values);
    Q_INVOKABLE void reset(); // one channel, a sine of 1 kHz
    Q_INVOKABLE void useExample(int index);
    Q_INVOKABLE QString componentText(int channel, int component) const; // "Sinus 1 kHz, 1 V crête"
    // A Fourier series: {error, count (harmonics generated), requested, rows: the first 12 [{n, a, b,
    // amplitude, phase}] (aₙ cos + bₙ sin = Aₙ sin(nω₀t + φₙ), φ in degrees)}.
    Q_INVOKABLE QVariantMap seriesInfo(int channel, int component) const;
    // Its coefficients as those of a known waveform ("square", "triangle", "sawtooth", "pulse", "fullwave",
    // "halfwave"), with the E (and D) of its variables when they have them.
    Q_INVOKABLE void applySeriesModel(int channel, int component, const QString &model);
    // A drawn motif: its points as text ("0 -1 ; 0,1 1"), typed back (an empty answer, or why not, the
    // points unchanged), a ready-made motif ("ramp", "stairs", "trapezoid", "rc", "heartbeat"). Its points
    // themselves go through setComponent (key "motif").
    Q_INVOKABLE QString motifText(int channel, int component) const;
    Q_INVOKABLE QString setMotifText(int channel, int component, const QString &text);
    Q_INVOKABLE void applyMotifModel(int channel, int component, const QString &model);
    // A recurrence: {error (what is wrong in its formula, variables or initial values, at once), rows: its
    // first values as the preview generated them [{n, x, y}], current (the preview is that of the recipe as
    // it is)}; a ready-made recurrence ("oscillator", "lowpass", "resonator", "average", "walk", "logistic").
    Q_INVOKABLE QVariantMap recurrenceInfo(int channel, int component) const;
    Q_INVOKABLE void applyRecurrenceModel(int channel, int component, const QString &model);
    Q_INVOKABLE QString recipeText() const; // JSON on one line
    Q_INVOKABLE void restoreRecipe(const QString &text);
    // The recipe of a signal of the session made here; false (lastError) if it has none.
    Q_INVOKABLE bool hasRecipe(qint64 signalId) const;
    Q_INVOKABLE bool takeUp(qint64 signalId);
    // The preview as a new signal of the session (its samples are shared, not copied), one step of the
    // history; its id, 0 on failure (lastError).
    Q_INVOKABLE qint64 addToSession();

    // Tests: starts what waits for its delay, and returns once the preview is up to date.
    void waitForIdle();

signals:
    void recipeChanged();
    void busyChanged();
    void progressChanged();
    void stateChanged();
    void lastErrorChanged();
    void activeChanged();

private:
    static constexpr qint64 kPreviewId = 1;
    struct Run
    {
        std::atomic<bool> cancelled{false};
        std::atomic<int> permille{0};
    };

    void replace(analysis::GeneratorRecipe recipe); // changed() if it differs
    void changed();
    void start();
    void finished(const std::shared_ptr<Run> &run, const std::shared_ptr<analysis::GeneratorResult> &result,
                  quint64 version);
    void setRunning(bool running);
    void pollProgress();
    void clearPreview();
    bool validChannel(int channel) const;
    bool validComponent(int channel, int component) const;
    void setLastError(const QString &message);

    QPointer<Session> m_session;
    Session m_preview;             // before the analysis that watches it
    AnalysisController m_analysis; // of the preview
    analysis::GeneratorRecipe m_recipe;
    quint64 m_version = 1;        // of the recipe, one more at each change
    quint64 m_previewVersion = 0; // of the recipe the preview (or the error) is about
    model::SignalPtr m_previewSignal;
    std::vector<analysis::RecurrenceTrace> m_traces; // of the preview
    QString m_error;
    QStringList m_warnings;
    QString m_lastError;

    QTimer m_delay;
    QTimer m_progressTimer;
    QThreadPool m_pool;
    std::shared_ptr<Run> m_run;
    bool m_running = false;
    bool m_again = false; // a newer recipe waits for the run in progress
    bool m_active = true;
    double m_progress = 0.0;
};
} // namespace qacq::app
