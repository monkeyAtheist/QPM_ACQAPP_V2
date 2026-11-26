#pragma once

// Module "Générateur": the recipe being edited (channels, each the sum of components:
// qacq/analysis/generation.h), its signal computed on a thread of its own a moment after each change and
// shown in a preview apart (a session of one signal, with its own analysis: spectrum and measurements),
// then added to the session on request, as one step of the history. Each signal made keeps its recipe in
// its provenance: takeUp() brings it back. The page opens blank (one channel without waveform:
// analysis::blankGeneratorRecipe), and so does reset(); the recipe of the last run is kept as text (recipeText,
// restoreRecipe, through AppSettings) for the page to offer it back.
// A waveform may be a signal of the session (a recorded signal: sources, sourceInfo, useSignal): the
// preview is made again when that signal changes, and the recipe follows its names.
// A waveform may be a line of a digital frame (frameInfo: the frame as built; addFrameLines: its other lines
// as channels; adoptFrameTiming: the signal framed on it); what is set on a line goes to the others.

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
    // "above"), variables; an operation between channels: expression, variables; a modulated carrier:
    // modulation ("am", "fm", "pm", "dsb", "usb", "lsb"), message, variables, index, deviation, phaseDeviation;
    // a recorded signal: source (id), sourceName, sourceFrames, sourceChannel, sourceChannelName, sourceStart,
    // loop, normalize (amplitude: a gain, or the peak it is scaled to; time: its delay); a digital frame:
    // protocol ("bits", "uart", "i2c", "spi", "usb"), data, format ("auto", "bin", "hex", "dec", "text"),
    // lsbFirst, code ("nrz", "nrzi", "manchester", "rz"), dataBits, parity ("none", "even", "odd"), stopBits,
    // i2cAddress, i2cTransfer ("write", "read", "write-read"), i2cNoAnswer, spiMode, reply, usbFullSpeed,
    // usbPacket ("setup", "out", "in", "sof", "data0", "data1", "ack", "nak", "stall", "setup-transaction"),
    // usbAddress, usbEndpoint, usbFrameNumber, line (which of its lines), low, repeat, gap (bits), group
    // (amplitude: its high level; frequency: its bit rate; time: its start)}]}]; a channel also output (false:
    // made, read by the others, left out of the signal).
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
    // The signals of the session a recorded-signal waveform may read: [{id, name, channels [names], sampleRate,
    // duration, summary}].
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    // Each channel of those signals, as a message to modulate a carrier with (useSignal "am"…): [{id, channel,
    // text ("Voix", "Voix — Micro"), tip (its summary)}].
    Q_PROPERTY(QVariantList messageSources READ messageSources NOTIFY sourcesChanged)
    // Nothing in the recipe yet: every channel without waveform nor offset (analysis::isBlankRecipe).
    Q_PROPERTY(bool blank READ blank NOTIFY recipeChanged)
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
    QVariantList sources() const;
    QVariantList messageSources() const;
    bool blank() const;
    QString lastError() const { return m_lastError; }
    bool active() const { return m_active; }
    void setActive(bool active);

    const analysis::GeneratorRecipe &currentRecipe() const { return m_recipe; }
    model::SignalPtr previewSignal() const { return m_previewSignal; }

    // Edits of the recipe (indices from 0); the preview follows a moment after the last one.
    Q_INVOKABLE void setRecipe(const QVariantMap &recipe);
    Q_INVOKABLE void setSignalSettings(const QVariantMap &values); // name, sampleRate, duration, seed
    Q_INVOKABLE int addChannel();                                  // blank; its index, -1 when there are 16
    // The last one stays, so does one that another reads (lastError says which); formulas renumbered.
    Q_INVOKABLE void removeChannel(int channel);
    // Its waveforms taken away and its offset set to 0 (it stays, and those that read it read 0).
    Q_INVOKABLE void clearChannel(int channel);
    Q_INVOKABLE void setChannel(int channel, const QVariantMap &values); // name, unit, offset
    // A component of that waveform ("sine", "square"... "noise", "sweep"), with usual values (an operation
    // between channels: the sum of two others, analysis::defaultOperation); its index, -1 when there are 32.
    Q_INVOKABLE int addComponent(int channel, const QString &waveform);
    Q_INVOKABLE void removeComponent(int channel, int component);
    Q_INVOKABLE void moveComponent(int channel, int from, int to);
    // Any key of a component; another waveform keeps what both share (analysis::withWaveform); another form
    // of the coefficients of a series keeps the series (analysis::withSeriesForm).
    Q_INVOKABLE void setComponent(int channel, int component, const QVariantMap &values);
    Q_INVOKABLE void reset(); // a blank page: one channel without waveform, 48 kHz, 1 s
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
    // An operation between channels or a modulated carrier: {error (what is wrong, at once), caution (x reads
    // nothing), note (the channels read by name, what x replaces)}; ready-made operations and messages
    // [{key, name, formula}] for that channel, and applying one.
    Q_INVOKABLE QVariantMap operationInfo(int channel, int component) const;
    Q_INVOKABLE QVariantList operationModels(int channel) const;
    Q_INVOKABLE void applyOperationModel(int channel, int component, const QString &model);
    // The messages also list each channel of each signal of the session ("signal:<id>:<channel>"): applied, that record
    // scaled to a peak of 1 goes just above the carrier, which reads it (analysis::useSignalAsMessage).
    Q_INVOKABLE QVariantList messageModels(int channel) const;
    Q_INVOKABLE void applyMessageModel(int channel, int component, const QString &model);
    // A recorded signal: {error (not in the session, channel gone), id and channel (those found), name,
    // channelName, channels (names of its channels, with their units), sampleRate, duration, summary ("48 kHz ·
    // 4 s · 192 000 éch."), resampled (its rate is not that of the recipe), differs (its rate or its duration
    // are not those of the recipe)}. To choose another: setComponent with source (id) and sourceChannel.
    Q_INVOKABLE QVariantMap sourceInfo(int channel, int component) const;
    // The recipe takes the rate and the duration of that record.
    Q_INVOKABLE void adoptSourceTiming(int channel, int component);
    // A line of a digital frame: {lines (names of the lines of its protocol), lineKeys, line (its own, from 0),
    // title ("UART 8N1"), members ("SDA (voie 1) · SCL (voie 2)": the lines of its frame in the recipe, empty
    // when it is alone), missing (names of the lines the recipe has not), error (what is wrong in its data or
    // settings), and without error: construction (the frame as built: "St 01000010 Sp │ …"), legend, summary
    // ("7 octets : 42 6F…"), bits, duration (s), samplesPerBit, timing ("70 bits · 7,292 ms · 10 échantillons
    // par bit"), sampled (enough samples for all its levels), fits (it ends before the signal does), warnings,
    // realRate (the bit rate of its protocol when it has one and it is not this one, else 0), differs
    // (adoptFrameTiming would change the recipe)}.
    Q_INVOKABLE QVariantMap frameInfo(int channel, int component) const;
    // The other lines of its protocol as new channels (lastError when there is no room).
    Q_INVOKABLE void addFrameLines(int channel, int component);
    // The signal framed on that frame: two bits of rest before and after it, ten samples a bit at least (a
    // whole number); with realRate, at the bit rate of its protocol (USB).
    Q_INVOKABLE void adoptFrameTiming(int channel, int component, bool realRate = false);
    // A channel of a signal of the session used in the recipe (analysis::useSignalInRecipe): "wave", a channel made
    // of it; "am" (or "fm", "pm", "dsb", "usb", "lsb"), a channel where it is the message, scaled to a peak of 1, of
    // a modulated carrier. A blank page becomes that channel alone, at the rate and for the duration of the record;
    // otherwise it takes the place of a blank channel, or comes after the others. The index of that channel
    // (signalUsed says what was done), -1 on failure (lastError).
    Q_INVOKABLE int useSignal(qint64 signalId, const QString &use, int channel = 0);
    // A channel of this recipe as the message, read as it is (v<k>), of a carrier modulated so ("am"…), in a channel
    // of its own (analysis::modulateChannelInRecipe): placed as useSignal places one; the timing stays. The index of
    // that channel (signalUsed says what was done), -1 on failure (lastError: an empty channel, no room…).
    Q_INVOKABLE int modulateChannel(int channel, const QString &use);
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
    void sourcesChanged();
    // useSignal or modulateChannel made that channel: what was done, in French, for the user.
    void signalUsed(int channel, const QString &text);

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
    std::vector<model::SignalPtr> sessionSignals() const;
    // The records the recipe reads, as they are in the session (null: not found), one a recorded component.
    std::vector<const model::Signal *> sourceStamp(const std::vector<model::SignalPtr> &sources) const;
    void sessionChanged();

    QPointer<Session> m_session;
    Session m_preview;             // before the analysis that watches it
    AnalysisController m_analysis; // of the preview
    analysis::GeneratorRecipe m_recipe;
    quint64 m_version = 1;        // of the recipe, one more at each change
    quint64 m_previewVersion = 0; // of the recipe the preview (or the error) is about
    model::SignalPtr m_previewSignal;
    std::vector<analysis::RecurrenceTrace> m_traces; // of the preview
    std::vector<const model::Signal *> m_sourceStamp; // the records the last generation read
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
