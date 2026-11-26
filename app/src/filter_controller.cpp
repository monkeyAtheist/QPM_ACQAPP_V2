#include "qacq/app/filter_controller.h"

#include "qacq/analysis/filter_code.h"
#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/signal_list_model.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <limits>
#include <utility>

namespace qacq::app
{
namespace
{
constexpr int kDelayMs = 150; // after the last change, before the preview

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QStringList toQStringList(const std::vector<std::string> &list)
{
    QStringList out;
    for (const auto &s : list) out << toQString(s);
    return out;
}

model::Json merged(model::Json base, const model::Json &changes)
{
    if (!base.isObject()) base = model::Json::object();
    for (const model::JsonMember &m : changes.members()) base[m.key] = m.value;
    return base;
}

QString number(double value)
{
    return QString::number(value, 'g', 17);
}
} // namespace

// What a preview gives back to the thread of the interface.
struct FilterController::Outcome
{
    model::Signal signal;
    std::string error;
    std::vector<std::string> warnings;
    std::string note;
    bool cancelled = false;
};

FilterController::FilterController(Session *session, QObject *parent)
    : QObject(parent), m_session(session), m_preview(nullptr, this), m_analysis(&m_preview, this),
      m_recipe(analysis::blankFilterRecipe())
{
    m_pool.setMaxThreadCount(1);
    m_delay.setSingleShot(true);
    m_delay.setInterval(kDelayMs);
    connect(&m_delay, &QTimer::timeout, this, &FilterController::start);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &FilterController::pollProgress);
    m_analysis.setSignalId(kPreviewId);
    if (session)
    {
        SignalListModel *list = session->signalList();
        connect(list, &SignalListModel::countChanged, this, &FilterController::refreshInput);
        connect(list, &QAbstractItemModel::dataChanged, this, &FilterController::refreshInput);
        connect(list, &QAbstractItemModel::modelReset, this, &FilterController::refreshInput);
    }
    redesign();
}

FilterController::~FilterController()
{
    m_delay.stop();
    m_progressTimer.stop();
    if (m_run) m_run->cancelled.store(true);
    m_pool.waitForDone();
}

QVariantMap FilterController::recipe() const
{
    return jsonToVariant(analysis::toJson(m_recipe)).toMap();
}

// What the design is, for the interface; computed once per design (a long FIR has many coefficients).
void FilterController::refreshDesignInfo()
{
    QVariantMap info;
    const analysis::FilterDesign &d = *m_design;
    info.insert(QStringLiteral("ok"), d.ok());
    info.insert(QStringLiteral("error"), toQString(d.error));
    info.insert(QStringLiteral("warnings"), toQStringList(d.warnings));
    info.insert(QStringLiteral("text"), toQString(analysis::filterText(d.recipe)));
    info.insert(QStringLiteral("summary"), toQStringList(analysis::designSummary(d)));
    info.insert(QStringLiteral("sampleRate"), d.recipe.sampleRateHz);
    info.insert(QStringLiteral("poleCount"), static_cast<int>(d.poles.size()));
    info.insert(QStringLiteral("zeroCount"), static_cast<int>(d.zeros.size()));
    info.insert(QStringLiteral("rootsKnown"), d.rootsKnown);
    info.insert(QStringLiteral("fir"), d.fir());
    info.insert(QStringLiteral("applicationError"), toQString(d.applicationError));
    // The recipe as designed: from a template, the order, the taps, the frequencies and β it computed.
    info.insert(QStringLiteral("recipe"), jsonToVariant(analysis::toJson(d.recipe)).toMap());
    if (d.recipe.fromTemplate && analysis::designedKind(d.recipe.kind))
    {
        info.insert(QStringLiteral("templateHeld"), d.templateHeld);
        info.insert(QStringLiteral("templateLoss"), d.templateLossDb);
        info.insert(QStringLiteral("templateAttenuation"), d.templateAttenuationDb);
        info.insert(QStringLiteral("exactOrder"), d.exactOrder);
        info.insert(QStringLiteral("templateText"), toQString(analysis::templateText(d.recipe)));
    }
    if (d.recipe.kind == analysis::FilterKind::Typed)
    {
        // What was typed, as evaluated: shown under the fields (the first ones of a long list).
        const auto values = [](const std::vector<double> &v) {
            QVariantList list;
            for (std::size_t i = 0; i < v.size() && i < kShownTypedValues; ++i) list.append(v[i]);
            return list;
        };
        info.insert(QStringLiteral("typedB"), values(d.typedNumerator));
        info.insert(QStringLiteral("typedA"), values(d.typedDenominator));
        info.insert(QStringLiteral("typedBCount"), static_cast<int>(d.typedNumerator.size()));
        info.insert(QStringLiteral("typedACount"), static_cast<int>(d.typedDenominator.size()));
        info.insert(QStringLiteral("numeratorOfK"), d.numeratorOfK);
        info.insert(QStringLiteral("denominatorOfK"), d.denominatorOfK);
    }
    if (d.ok() && !d.fir())
    {
        QVariantList sections;
        for (const dsp::Biquad &s : d.sections)
            sections.append(QVariantMap{{QStringLiteral("b0"), s.b[0]}, {QStringLiteral("b1"), s.b[1]},
                                        {QStringLiteral("b2"), s.b[2]}, {QStringLiteral("a1"), s.a[1]},
                                        {QStringLiteral("a2"), s.a[2]}});
        info.insert(QStringLiteral("sections"), sections);
    }
    else if (d.ok())
    {
        info.insert(QStringLiteral("tapCount"), static_cast<int>(d.taps.size()));
    }
    // The coefficients to read in the panel: the first ones of a long FIR (« Copier » copies them all).
    const QStringList lines = coefficientLines(kShownCoefficientLines + 1);
    QStringList shown = lines.mid(0, kShownCoefficientLines);
    const qsizetype total = d.fir() ? static_cast<qsizetype>(d.taps.size())
                                    : static_cast<qsizetype>(d.typedNumerator.size() + d.typedDenominator.size());
    if (lines.size() > kShownCoefficientLines)
        shown << tr("… %1 coefficients en tout : « Copier les coefficients » les copie tous.")
                     .arg(formatCount(static_cast<qint64>(total)));
    info.insert(QStringLiteral("coefficients"), shown.join(QStringLiteral("\n")));
    m_designInfo = info;
}

QStringList FilterController::coefficientLines(qsizetype maximum) const
{
    QStringList lines;
    const analysis::FilterDesign &d = *m_design;
    if (!d.ok()) return lines;
    // The gain at the output first: the coefficients that follow are those of the filter at 0 dB.
    if (d.gain != 1.0)
        lines << QStringLiteral("# Gain en sortie (%1) : la sortie du filtre multipliée par g")
                     .arg(toQString(analysis::filterGainText(d.recipe.gainDb)))
              << QStringLiteral("g = %1").arg(number(d.gain)) << QString();
    if (!d.fir())
    {
        // Coefficients typed: b and a as they are applied (divided by a[0]), then the sections they make.
        if (d.recipe.kind == analysis::FilterKind::Typed && !d.typedDenominator.empty())
        {
            const double a0 = d.typedDenominator.front();
            lines << (a0 != 1.0 ? QStringLiteral("# b et a tapés, divisés par a[0] = %1").arg(number(a0))
                                : QStringLiteral("# b et a tapés"));
            for (std::size_t k = 0; k < d.typedNumerator.size() && lines.size() < maximum; ++k)
                lines << QStringLiteral("b[%1] = %2").arg(static_cast<qint64>(k)).arg(number(d.typedNumerator[k] / a0));
            for (std::size_t k = 0; k < d.typedDenominator.size() && lines.size() < maximum; ++k)
                lines << QStringLiteral("a[%1] = %2").arg(static_cast<qint64>(k)).arg(number(d.typedDenominator[k] / a0));
            lines << QString() << QStringLiteral("# En sections du second ordre, l'une après l'autre") << QString();
        }
        int index = 0;
        for (const dsp::Biquad &s : d.sections)
        {
            if (index > 0) lines << QString();
            lines << QStringLiteral("# Section %1").arg(++index);
            lines << QStringLiteral("b0 = %1").arg(number(s.b[0])) << QStringLiteral("b1 = %1").arg(number(s.b[1]))
                  << QStringLiteral("b2 = %1").arg(number(s.b[2])) << QStringLiteral("a1 = %1").arg(number(s.a[1]))
                  << QStringLiteral("a2 = %1").arg(number(s.a[2]));
            if (lines.size() >= maximum) break;
        }
        return lines;
    }
    for (std::size_t k = 0; k < d.taps.size() && lines.size() < maximum; ++k)
        lines << QStringLiteral("h[%1] = %2").arg(static_cast<qint64>(k)).arg(number(d.taps[k]));
    return lines;
}

QString FilterController::coefficientsText() const
{
    const analysis::FilterDesign &d = *m_design;
    if (!d.ok()) return {};
    QStringList lines;
    lines << QStringLiteral("# %1, fs = %2 Hz").arg(toQString(analysis::filterText(d.recipe)), number(d.recipe.sampleRateHz));
    const bool gain = d.gain != 1.0;
    if (d.fir())
        lines << QStringLiteral("# y[n] = %1h[0]*x[n] + h[1]*x[n-1] + ... + h[N-1]*x[n-N+1]%2, N = %3")
                     .arg(gain ? QStringLiteral("g*(") : QString(), gain ? QStringLiteral(")") : QString())
                     .arg(static_cast<qint64>(d.taps.size()));
    else if (d.recipe.kind == analysis::FilterKind::Typed)
        lines << QStringLiteral("# y[n] = b[0]*x[n] + ... + b[M]*x[n-M] - a[1]*y[n-1] - ... - a[N]*y[n-N] (a[0] = 1),")
              << QStringLiteral("# ou les %1 sections du second ordre l'une après l'autre (plus juste) :").arg(static_cast<qint64>(d.sections.size()))
              << QStringLiteral("# y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]");
    else
        lines << QStringLiteral("# %1 sections du second ordre l'une après l'autre, a0 = 1 :").arg(static_cast<qint64>(d.sections.size()))
              << QStringLiteral("# y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]");
    if (gain && !d.fir()) lines << QStringLiteral("# puis la sortie de la dernière section multipliée par g");
    lines << coefficientLines(std::numeric_limits<qsizetype>::max());
    return lines.join(QStringLiteral("\n")) + QStringLiteral("\n");
}

std::shared_ptr<const analysis::ImpulseResponse> FilterController::currentImpulse() const
{
    if (!m_impulse) m_impulse = std::make_shared<const analysis::ImpulseResponse>(analysis::impulseResponse(*m_design));
    return m_impulse;
}

void FilterController::setInputId(qint64 id)
{
    if (id == m_inputId) return;
    m_inputId = id;
    m_channel = 0;
    m_inputRate = 0.0;
    m_seenInput.reset();
    m_seenNone = false;
    refreshInput();
}

void FilterController::setChannel(int channel)
{
    const model::SignalPtr s = inputSignal();
    const int count = s ? static_cast<int>(s->channelCount()) : 1;
    channel = std::clamp(channel, 0, std::max(0, count - 1));
    if (channel == m_channel) return;
    m_channel = channel;
    ++m_version;
    emit inputChanged();
    schedule();
}

model::SignalPtr FilterController::inputSignal() const
{
    Session *session = m_session.data(); // read once
    if (!session || m_inputId <= 0) return nullptr;
    return session->signalList()->signalById(m_inputId);
}

QVariantMap FilterController::inputInfo() const
{
    const model::SignalPtr s = inputSignal();
    if (!s) return {};
    QStringList channels;
    for (const model::ChannelInfo &c : s->channels) channels << toQString(c.name);
    return {{QStringLiteral("name"), toQString(s->name)},
            {QStringLiteral("sampleRate"), s->sampleRateHz},
            {QStringLiteral("channels"), channels},
            {QStringLiteral("frames"), static_cast<qint64>(s->frameCount())}};
}

void FilterController::refreshInput()
{
    const model::SignalPtr s = inputSignal();
    // Another signal of the list changed: nothing to do here.
    if (s ? s == m_seenInput.lock() : m_seenNone) return;
    m_seenInput = s;
    m_seenNone = !s;
    const double rate = s ? s->sampleRateHz : 0.0;
    // The signal went, or its rate changed: the design follows it (when it takes the rate of the signal).
    if (rate != m_inputRate)
    {
        m_inputRate = rate;
        if (rate > 0.0 && m_recipe.rateOfSignal && rate != m_recipe.sampleRateHz)
        {
            analysis::FilterRecipe r = m_recipe;
            r.sampleRateHz = rate;
            replace(std::move(r));
        }
    }
    if (s && m_channel >= static_cast<int>(s->channelCount())) m_channel = 0;
    ++m_version;
    emit inputChanged();
    schedule();
}

bool FilterController::ready() const
{
    return !m_running && !m_delay.isActive() && m_previewVersion == m_version;
}

void FilterController::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    if (m_active && m_previewVersion != m_version) schedule();
}

QVariantList FilterController::examples() const
{
    QVariantList list;
    for (const analysis::FilterExample &e : analysis::filterExamples())
        list.append(QVariantMap{{QStringLiteral("name"), toQString(e.name)}, {QStringLiteral("tip"), toQString(e.tip)}});
    return list;
}

void FilterController::setRecipe(const QVariantMap &changes)
{
    const model::Json json = variantToJson(changes);
    analysis::FilterRecipe r = analysis::filterRecipeFromJson(merged(analysis::toJson(m_recipe), json));
    // Another kind: its usual application (a FIR without its delay, an IIR as in real time).
    if (r.kind != m_recipe.kind && !changes.contains(QStringLiteral("application")))
        r.application = r.kind == analysis::FilterKind::Fir ? analysis::FilterApplication::Aligned
                                                             : analysis::FilterApplication::Causal;
    // A band: its upper edge above its lower one.
    const bool band = r.response == dsp::FilterResponse::BandPass || r.response == dsp::FilterResponse::BandStop;
    if (band && !(r.frequency2Hz > r.frequency1Hz))
        r.frequency2Hz = std::min(2.0 * r.frequency1Hz, 0.45 * r.sampleRateHz);
    if (r.rateOfSignal && m_inputRate > 0.0) r.sampleRateHz = m_inputRate;
    const analysis::FilterDesign *designed = m_design.get();
    const bool computed = designed && designed->ok() && designed->recipe.fromTemplate;
    // A template switched on: the one it had if the filter as it is holds it (back from a template, nothing changed),
    // else one the filter holds, read on its gain (its least order is then at most the order chosen).
    const bool switchedOn = r.fromTemplate && !m_recipe.fromTemplate && analysis::designedKind(r.kind);
    const bool edgesGiven = changes.contains(QStringLiteral("pass1")) || changes.contains(QStringLiteral("pass2")) ||
                            changes.contains(QStringLiteral("stop1")) || changes.contains(QStringLiteral("stop2"));
    bool templateSet = edgesGiven;
    if (switchedOn && !edgesGiven && designed && designed->ok() && r.response == designed->recipe.response)
    {
        if (analysis::templateEdgesValid(r))
        {
            analysis::FilterDesign probe;
            probe.recipe = r;
            probe.sections = designed->sections;
            probe.taps = designed->taps;
            templateSet = analysis::measureTemplate(probe).held;
        }
        if (!templateSet) templateSet = analysis::templateFromDesign(*designed, r);
    }
    // Or another response for it: edges in the order the response wants, from the frequencies of the filter as
    // designed, unless those it had hold (edges typed wrong are left as typed: the design says what is wrong).
    if (!templateSet && r.fromTemplate && (switchedOn || r.response != m_recipe.response) && !analysis::templateEdgesValid(r))
    {
        if (computed && !changes.contains(QStringLiteral("frequency1")) && !changes.contains(QStringLiteral("frequency2")))
        {
            r.frequency1Hz = designed->recipe.frequency1Hz;
            r.frequency2Hz = designed->recipe.frequency2Hz;
            if (band && !(r.frequency2Hz > r.frequency1Hz))
                r.frequency2Hz = std::min(2.0 * r.frequency1Hz, 0.45 * r.sampleRateHz);
        }
        analysis::setTemplateFromFrequencies(r);
    }
    // Back from a template: the order, the frequencies, the taps and β it computed stay (unless changed at the same
    // time), so that the filter goes on from there.
    if (m_recipe.fromTemplate && !r.fromTemplate && computed)
    {
        const analysis::FilterRecipe &c = designed->recipe;
        const auto keep = [&changes](const char *key) { return !changes.contains(QString::fromLatin1(key)); };
        if (keep("order")) r.order = c.order;
        if (keep("taps")) r.taps = c.taps;
        if (keep("frequency1")) r.frequency1Hz = c.frequency1Hz;
        if (keep("frequency2") && band) r.frequency2Hz = c.frequency2Hz;
        if (keep("kaiserBeta")) r.kaiserBeta = c.kaiserBeta;
        if (keep("transition")) r.transitionHz = c.transitionHz;
    }
    replace(std::move(r));
}

void FilterController::reset()
{
    // The blank page: no filter (its rate, that of the signal chosen).
    analysis::FilterRecipe r = analysis::blankFilterRecipe(m_inputRate > 0.0 ? m_inputRate : m_recipe.sampleRateHz);
    replace(std::move(r));
}

void FilterController::useExample(int index)
{
    const auto &list = analysis::filterExamples();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) return;
    analysis::FilterRecipe r = list[static_cast<std::size_t>(index)].recipe;
    r.rateOfSignal = true;
    r.sampleRateHz = m_inputRate > 0.0 ? m_inputRate : m_recipe.sampleRateHz;
    replace(std::move(r));
}

QString FilterController::recipeText() const
{
    return toQString(analysis::toJson(m_recipe).dump());
}

void FilterController::restoreRecipe(const QString &text)
{
    const auto json = model::Json::parse(toUtf8(text));
    if (!json || !json->isObject()) return;
    analysis::FilterRecipe r = analysis::filterRecipeFromJson(*json);
    if (r.rateOfSignal && m_inputRate > 0.0) r.sampleRateHz = m_inputRate;
    replace(std::move(r));
}

void FilterController::replace(analysis::FilterRecipe recipe)
{
    if (analysis::toJson(recipe) == analysis::toJson(m_recipe)) return;
    m_recipe = std::move(recipe);
    emit recipeChanged();
    redesign();
}

void FilterController::redesign()
{
    m_design = std::make_shared<const analysis::FilterDesign>(analysis::designFilter(m_recipe));
    refreshDesignInfo();
    m_impulse.reset();
    ++m_version;
    emit designChanged();
    schedule();
}

void FilterController::schedule()
{
    m_delay.start();
    emit stateChanged();
}

void FilterController::start()
{
    if (!m_active) return;
    if (m_running)
    {
        if (m_run) m_run->cancelled.store(true);
        m_again = true;
        return;
    }
    const model::SignalPtr input = inputSignal();
    if (!input || !m_design->ok())
    {
        // Nothing to filter: the preview goes, and says why.
        m_previewVersion = m_version;
        m_error = !m_design->ok() ? toQString(m_design->error) : QString();
        m_warnings.clear();
        m_note.clear();
        clearPreview();
        emit stateChanged();
        return;
    }
    auto run = std::make_shared<Run>();
    m_run = run;
    m_progress = 0.0;
    emit progressChanged();
    setRunning(true);
    emit stateChanged();
    const quint64 version = m_version;
    const std::shared_ptr<const analysis::FilterDesign> design = m_design;
    const std::size_t channel = static_cast<std::size_t>(std::max(0, m_channel));
    m_pool.start([this, run, design, input, channel, version] {
        auto outcome = std::make_shared<Outcome>();
        const model::Progress progress{&run->cancelled, &run->permille};
        const std::size_t channels = input->channelCount();
        if (channel >= channels)
        {
            outcome->error = "Cette voie n'existe pas.";
        }
        else
        {
            analysis::FilteredSignal f =
                analysis::applyFilter(*design, *input, {channel}, kPreviewFrames, progress);
            outcome->cancelled = f.cancelled;
            outcome->error = f.error;
            outcome->warnings = f.warnings;
            if (f.error.empty() && !f.cancelled)
            {
                // Before and after, side by side: one signal of two channels.
                const std::size_t n = f.signal.frameCount();
                auto data = std::make_shared<model::SampleData>();
                const std::span<const double> x = input->channel(channel).first(n);
                data->channels.emplace_back(x.begin(), x.end());
                data->channels.push_back(f.signal.data->channels.front());
                data->discontinuities = f.signal.data->discontinuities;
                model::Signal &s = outcome->signal;
                s.name = "Aperçu : " + input->name;
                s.origin = model::Origin::Derived;
                s.sampleRateHz = input->sampleRateHz;
                s.startSeconds = input->startSeconds;
                const std::string unit = input->channels[channel].unit;
                s.channels = {{"Avant", unit}, {"Après", unit}};
                s.data = std::move(data);
                if (n < input->frameCount())
                    outcome->note = "Aperçu sur les " + toUtf8(formatCount(static_cast<qint64>(n))) +
                                    " premiers échantillons ; « Ajouter à la session » filtre tout le signal.";
            }
        }
        QMetaObject::invokeMethod(this, [this, run, outcome, version] { finished(run, outcome, version); },
                                  Qt::QueuedConnection);
    });
}

void FilterController::finished(const std::shared_ptr<Run> &run, const std::shared_ptr<Outcome> &outcome, quint64 version)
{
    if (m_run == run) m_run.reset();
    setRunning(false);
    if (m_again)
    {
        m_again = false;
        start();
        return;
    }
    if (outcome->cancelled)
    {
        emit stateChanged();
        return;
    }
    m_previewVersion = version;
    m_error = toQString(outcome->error);
    m_warnings = toQStringList(outcome->warnings);
    m_note = toQString(outcome->note);
    if (outcome->error.empty() && outcome->signal.data)
    {
        model::Signal signal = std::move(outcome->signal);
        signal.id = static_cast<std::uint64_t>(kPreviewId);
        signal.createdUtc = toUtf8(nowUtc());
        auto shown = std::make_shared<const model::Signal>(std::move(signal));
        m_previewSignal = shown;
        m_preview.signalList()->resetSignals({shown});
    }
    else
    {
        clearPreview();
    }
    emit stateChanged();
}

void FilterController::addToSession()
{
    Session *session = m_session.data();
    if (!session || m_adding) return;
    const model::SignalPtr input = inputSignal();
    if (!input)
    {
        setLastError(QStringLiteral("Choisissez d'abord un signal dans la liste : c'est lui qui sera filtré."));
        return;
    }
    if (!m_design->ok())
    {
        setLastError(toQString(m_design->error));
        return;
    }
    if (m_design->recipe.kind == analysis::FilterKind::None)
    {
        setLastError(QStringLiteral("Aucun filtre : le signal resterait le même. Choisissez d'abord un type de filtre (ruban ▸ Filtre) ou un exemple."));
        return;
    }
    m_adding = true;
    emit addingChanged();
    const std::shared_ptr<const analysis::FilterDesign> design = m_design;
    QPointer<Session> target = session;
    m_pool.start([this, design, input, target] {
        auto result = std::make_shared<analysis::FilteredSignal>(analysis::applyFilter(*design, *input));
        QMetaObject::invokeMethod(
            this,
            [this, result, target] {
                m_adding = false;
                emit addingChanged();
                if (!target) return;
                if (!result->error.empty())
                {
                    setLastError(toQString(result->error));
                    return;
                }
                model::Signal signal = std::move(result->signal);
                signal.name = toUtf8(target->signalList()->uniqueName(toQString(signal.name)));
                signal.createdUtc = toUtf8(nowUtc());
                for (const std::string &w : result->warnings) signal.notes += "\n" + w;
                const QString label = QStringLiteral("Filtrer « %1 »").arg(toQString(signal.name));
                QString error;
                const qint64 id = target->signalList()->addSignal(std::move(signal), label, &error);
                if (id == 0)
                    setLastError(error);
                else
                    emit added(id);
            },
            Qt::QueuedConnection);
    });
}

void FilterController::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    if (running)
        m_progressTimer.start();
    else
        m_progressTimer.stop();
    emit busyChanged();
}

void FilterController::pollProgress()
{
    if (!m_run) return;
    const double p = m_run->permille.load(std::memory_order_relaxed) / 1000.0;
    if (p == m_progress) return;
    m_progress = p;
    emit progressChanged();
}

void FilterController::clearPreview()
{
    if (!m_previewSignal) return;
    m_previewSignal.reset();
    m_preview.signalList()->resetSignals({});
}

namespace
{
analysis::CodeOptions codeOptions(const QVariantMap &options)
{
    analysis::CodeOptions o;
    analysis::CodeFormat format = o.format;
    if (analysis::codeFormatFromKey(toUtf8(options.value(QStringLiteral("format")).toString()), format)) o.format = format;
    analysis::CodeScaling scaling = o.scaling;
    if (analysis::codeScalingFromKey(toUtf8(options.value(QStringLiteral("scaling")).toString()), scaling)) o.scaling = scaling;
    const QString name = options.value(QStringLiteral("name")).toString().trimmed();
    if (!name.isEmpty()) o.name = toUtf8(name);
    return o;
}
} // namespace

QVariantMap FilterController::code(const QVariantMap &options) const
{
    if (m_codeDesign == m_design && m_codeOptions == options && !m_code.isEmpty()) return m_code;
    const analysis::CodeOptions o = codeOptions(options);
    const analysis::EmbeddedFilter embedded = analysis::embedFilter(*m_design, o);
    const analysis::GeneratedCode generated = analysis::generateCode(embedded, *m_design);
    QVariantMap out;
    out.insert(QStringLiteral("ok"), embedded.ok() && generated.error.empty());
    out.insert(QStringLiteral("error"), toQString(embedded.ok() ? generated.error : embedded.error));
    out.insert(QStringLiteral("header"), toQString(generated.header));
    out.insert(QStringLiteral("source"), toQString(generated.source));
    out.insert(QStringLiteral("headerName"), toQString(generated.headerName));
    out.insert(QStringLiteral("sourceName"), toQString(generated.sourceName));
    out.insert(QStringLiteral("notes"), toQStringList(embedded.notes));
    out.insert(QStringLiteral("warnings"), toQStringList(embedded.warnings));
    out.insert(QStringLiteral("format"), toQString(std::string(analysis::codeFormatKey(o.format))));
    out.insert(QStringLiteral("scaling"), toQString(std::string(analysis::codeScalingKey(o.scaling))));
    out.insert(QStringLiteral("name"), toQString(o.name));
    out.insert(QStringLiteral("fixed"), embedded.fixed);
    m_codeDesign = m_design;
    m_codeOptions = options;
    m_code = out;
    return out;
}

QString FilterController::saveCode(const QUrl &folder, const QVariantMap &options)
{
    const QVariantMap generated = code(options);
    if (!generated.value(QStringLiteral("ok")).toBool())
    {
        setLastError(generated.value(QStringLiteral("error")).toString());
        return {};
    }
    const QString directory = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (directory.isEmpty() || !QDir(directory).exists())
    {
        setLastError(tr("Le dossier « %1 » n'existe pas.").arg(directory));
        return {};
    }
    const QDir dir(directory);
    const auto write = [this](const QString &path, const QString &text) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            setLastError(tr("Impossible d'écrire « %1 » : %2").arg(QDir::toNativeSeparators(path), file.errorString()));
            return false;
        }
        file.write(text.toUtf8());
        if (!file.commit())
        {
            setLastError(tr("Impossible d'écrire « %1 » : %2").arg(QDir::toNativeSeparators(path), file.errorString()));
            return false;
        }
        return true;
    };
    const QString header = dir.filePath(generated.value(QStringLiteral("headerName")).toString());
    const QString source = dir.filePath(generated.value(QStringLiteral("sourceName")).toString());
    if (!write(header, generated.value(QStringLiteral("header")).toString())) return {};
    if (!write(source, generated.value(QStringLiteral("source")).toString())) return {};
    return QDir::toNativeSeparators(source);
}

void FilterController::setLastError(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
}

void FilterController::waitForIdle()
{
    for (int round = 0; round < 1000; ++round)
    {
        if (m_delay.isActive())
        {
            m_delay.stop();
            start();
        }
        m_pool.waitForDone();
        QCoreApplication::processEvents(); // the results, sent back to this thread
        if (!m_running && !m_delay.isActive() && !m_again && !m_adding) return;
        if (!m_active && !m_adding) return;
    }
}
} // namespace qacq::app
