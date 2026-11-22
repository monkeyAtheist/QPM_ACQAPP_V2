#include "qacq/app/generator_controller.h"

#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/analysis/expression.h"
#include "qacq/dsp/constants.h"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <utility>

namespace qacq::app
{
namespace
{
constexpr int kDelayMs = 120; // after the last change, before generating

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

// The members of `changes` over those of `base`.
model::Json merged(model::Json base, const model::Json &changes)
{
    if (!base.isObject()) base = model::Json::object();
    for (const model::JsonMember &m : changes.members()) base[m.key] = m.value;
    return base;
}
} // namespace

GeneratorController::GeneratorController(Session *session, QObject *parent)
    : QObject(parent), m_session(session), m_preview(nullptr, this), m_analysis(&m_preview, this),
      m_recipe(analysis::defaultGeneratorRecipe())
{
    m_pool.setMaxThreadCount(1);
    m_delay.setSingleShot(true);
    m_delay.setInterval(kDelayMs);
    connect(&m_delay, &QTimer::timeout, this, &GeneratorController::start);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &GeneratorController::pollProgress);
    m_analysis.setSignalId(kPreviewId);
    m_delay.start();
}

GeneratorController::~GeneratorController()
{
    m_delay.stop();
    m_progressTimer.stop();
    if (m_run) m_run->cancelled.store(true);
    m_pool.waitForDone();
}

QVariantMap GeneratorController::recipe() const
{
    return jsonToVariant(analysis::toJson(m_recipe)).toMap();
}

qint64 GeneratorController::sampleCount() const
{
    return static_cast<qint64>(analysis::generatorSampleCount(m_recipe));
}

QString GeneratorController::sizeText() const
{
    if (!analysis::checkGeneratorRecipe(m_recipe).empty()) return QString();
    const qint64 n = sampleCount();
    const auto channels = static_cast<qint64>(m_recipe.channels.size());
    const QString bytes = formatBytes(n * channels * static_cast<qint64>(sizeof(double)));
    if (channels == 1) return QStringLiteral("%1 échantillons · %2").arg(formatCount(n), bytes);
    return QStringLiteral("%1 échantillons par voie · %2 voies · %3").arg(formatCount(n)).arg(channels).arg(bytes);
}

QString GeneratorController::problem() const
{
    return toQString(analysis::checkGeneratorRecipe(m_recipe));
}

bool GeneratorController::ready() const
{
    return m_previewSignal && !m_running && !m_delay.isActive() && m_previewVersion == m_version && m_error.isEmpty();
}

QVariantList GeneratorController::examples() const
{
    QVariantList list;
    for (const analysis::GeneratorExample &e : analysis::generatorExamples())
        list.append(QVariantMap{{QStringLiteral("name"), toQString(e.name)}, {QStringLiteral("tip"), toQString(e.tip)}});
    return list;
}

void GeneratorController::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    // What changed meanwhile is generated now.
    if (active && !m_running && m_previewVersion != m_version) m_delay.start();
}

void GeneratorController::setRecipe(const QVariantMap &recipe)
{
    replace(analysis::generatorRecipeFromJson(variantToJson(recipe)));
}

void GeneratorController::setSignalSettings(const QVariantMap &values)
{
    model::Json changes = variantToJson(values);
    changes.erase("channels"); // edited one by one
    replace(analysis::generatorRecipeFromJson(merged(analysis::toJson(m_recipe), changes)));
}

int GeneratorController::addChannel()
{
    if (m_recipe.channels.size() >= analysis::kMaximumGeneratorChannels)
    {
        setLastError(QStringLiteral("Au plus %1 voies.").arg(static_cast<int>(analysis::kMaximumGeneratorChannels)));
        return -1;
    }
    m_recipe.channels.push_back(analysis::defaultGeneratorChannel(m_recipe.channels.size(), m_recipe.sampleRateHz));
    changed();
    return static_cast<int>(m_recipe.channels.size()) - 1;
}

void GeneratorController::removeChannel(int channel)
{
    if (!validChannel(channel) || m_recipe.channels.size() <= 1) return;
    m_recipe.channels.erase(m_recipe.channels.begin() + channel);
    changed();
}

void GeneratorController::setChannel(int channel, const QVariantMap &values)
{
    if (!validChannel(channel)) return;
    model::Json changes = variantToJson(values);
    changes.erase("components"); // edited one by one
    analysis::GeneratorRecipe r = m_recipe;
    const auto index = static_cast<std::size_t>(channel);
    r.channels[index] = analysis::generatorChannelFromJson(merged(analysis::toJson(r.channels[index]), changes), index);
    replace(std::move(r));
}

int GeneratorController::addComponent(int channel, const QString &waveform)
{
    if (!validChannel(channel)) return -1;
    analysis::WaveKind w;
    if (!analysis::waveformFromKey(toUtf8(waveform), w)) return -1;
    auto &list = m_recipe.channels[static_cast<std::size_t>(channel)].components;
    if (list.size() >= analysis::kMaximumGeneratorComponents)
    {
        setLastError(QStringLiteral("Au plus %1 composantes par voie.").arg(static_cast<int>(analysis::kMaximumGeneratorComponents)));
        return -1;
    }
    list.push_back(analysis::defaultGeneratorComponent(w, m_recipe.sampleRateHz, m_recipe.durationSeconds));
    changed();
    return static_cast<int>(list.size()) - 1;
}

void GeneratorController::removeComponent(int channel, int component)
{
    if (!validComponent(channel, component)) return;
    auto &list = m_recipe.channels[static_cast<std::size_t>(channel)].components;
    list.erase(list.begin() + component);
    changed();
}

void GeneratorController::moveComponent(int channel, int from, int to)
{
    if (!validComponent(channel, from) || !validComponent(channel, to) || from == to) return;
    auto &list = m_recipe.channels[static_cast<std::size_t>(channel)].components;
    const analysis::GeneratorComponent moved = list[static_cast<std::size_t>(from)];
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, moved);
    changed();
}

void GeneratorController::setComponent(int channel, int component, const QVariantMap &values)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    analysis::GeneratorComponent &c =
        r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    model::Json changes = variantToJson(values);
    if (changes.contains("waveform"))
    {
        analysis::WaveKind w;
        if (analysis::waveformFromKey(changes["waveform"].toString(), w))
            c = analysis::withWaveform(c, w, r.sampleRateHz, r.durationSeconds);
        changes.erase("waveform");
    }
    if (changes.contains("form") && c.waveform == analysis::WaveKind::FourierSeries)
    {
        // Another form of the coefficients: the same series, its formulas rewritten.
        const std::string form = changes["form"].toString();
        c = analysis::withSeriesForm(c,
                                     form == "cartesian" ? analysis::SeriesForm::Cartesian
                                     : form == "polar"   ? analysis::SeriesForm::Polar
                                                         : analysis::SeriesForm::SineCosine,
                                     r.sampleRateHz);
        changes.erase("form");
    }
    c = analysis::generatorComponentFromJson(merged(analysis::toJson(c), changes));
    replace(std::move(r));
}

void GeneratorController::reset()
{
    replace(analysis::defaultGeneratorRecipe());
}

void GeneratorController::useExample(int index)
{
    const auto &examples = analysis::generatorExamples();
    if (index < 0 || static_cast<std::size_t>(index) >= examples.size()) return;
    replace(examples[static_cast<std::size_t>(index)].recipe);
}

QString GeneratorController::componentText(int channel, int component) const
{
    if (!validComponent(channel, component)) return QString();
    const analysis::GeneratorChannel &c = m_recipe.channels[static_cast<std::size_t>(channel)];
    return toQString(analysis::componentText(c.components[static_cast<std::size_t>(component)], c.unit));
}

QVariantMap GeneratorController::seriesInfo(int channel, int component) const
{
    QVariantMap info;
    if (!validComponent(channel, component)) return info;
    const analysis::GeneratorComponent &c =
        m_recipe.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    if (c.waveform != analysis::WaveKind::FourierSeries) return info;
    const double fs = m_recipe.sampleRateHz;
    const std::size_t requested = analysis::seriesHarmonics(c, fs);
    std::size_t count = requested;
    if (c.bandLimit == dsp::BandLimit::Fourier && c.frequencyHz > 0.0)
        count = std::min(count, static_cast<std::size_t>(std::max(0.0, std::floor((0.5 - 1e-12) * fs / c.frequencyHz))));
    const analysis::SeriesCoefficients k = analysis::seriesCoefficients(c, fs, std::min<std::size_t>(12, count));
    info.insert(QStringLiteral("error"), toQString(k.error));
    info.insert(QStringLiteral("count"), static_cast<qint64>(count));
    info.insert(QStringLiteral("requested"), static_cast<qint64>(requested));
    info.insert(QStringLiteral("dc"), k.dc);
    QVariantList rows;
    for (std::size_t n = 0; n < k.a.size(); ++n)
    {
        // Rounding left by a change of form (cosd(−90) = 6e-17) is shown as 0.
        const double scale = std::max(std::abs(k.a[n]), std::abs(k.b[n]));
        const double a = std::abs(k.a[n]) < 1e-12 * scale ? 0.0 : k.a[n];
        const double b = std::abs(k.b[n]) < 1e-12 * scale ? 0.0 : k.b[n];
        rows.append(QVariantMap{{QStringLiteral("n"), static_cast<qint64>(n + 1)},
                                {QStringLiteral("a"), a},
                                {QStringLiteral("b"), b},
                                {QStringLiteral("amplitude"), std::hypot(a, b)},
                                {QStringLiteral("phase"), std::hypot(a, b) > 0.0 ? std::atan2(a, b) * 180.0 / dsp::kPi : 0.0}});
    }
    info.insert(QStringLiteral("rows"), rows);
    return info;
}

void GeneratorController::applySeriesModel(int channel, int component, const QString &model)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    analysis::GeneratorComponent &c =
        r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    // The E and D the variables already give, if they read well.
    double e = 1.0, d = 0.25;
    const analysis::Variables v = analysis::parseVariables(c.variables, {}, {});
    if (v.error.empty())
    {
        for (std::size_t i = 0; i < v.names.size(); ++i)
        {
            if (v.names[i] == "E") e = v.values[i];
            if (v.names[i] == "D" && v.values[i] > 0.0 && v.values[i] < 1.0) d = v.values[i];
        }
    }
    if (model == QStringLiteral("square")) d = 0.5;
    if (!analysis::applySeriesModel(c, toUtf8(model), e, d)) return;
    replace(std::move(r));
}

QString GeneratorController::motifText(int channel, int component) const
{
    if (!validComponent(channel, component)) return QString();
    const analysis::GeneratorComponent &c =
        m_recipe.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    return toQString(analysis::motifText(c.motif));
}

QString GeneratorController::setMotifText(int channel, int component, const QString &text)
{
    if (!validComponent(channel, component)) return QString();
    std::vector<analysis::MotifPoint> points;
    std::string why;
    if (!analysis::parseMotif(toUtf8(text), points, why)) return toQString(why);
    analysis::GeneratorRecipe r = m_recipe;
    r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)].motif = std::move(points);
    replace(std::move(r));
    return QString();
}

void GeneratorController::applyMotifModel(int channel, int component, const QString &model)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    if (!analysis::applyMotifModel(
            r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)], toUtf8(model)))
        return;
    replace(std::move(r));
}

QVariantMap GeneratorController::recurrenceInfo(int channel, int component) const
{
    QVariantMap info;
    if (!validComponent(channel, component)) return info;
    const analysis::GeneratorComponent &c =
        m_recipe.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    if (c.waveform != analysis::WaveKind::Recurrence) return info;
    const QString error = toQString(analysis::checkRecurrence(c, m_recipe.sampleRateHz));
    info.insert(QStringLiteral("error"), error);
    // What the input does, when it does nothing.
    QString note;
    if (error.isEmpty() && c.recurrenceInput != analysis::RecurrenceInput::None &&
        !analysis::recurrenceUsesInput(c, m_recipe.sampleRateHz))
    {
        note = QStringLiteral("La formule ne lit pas x[n] : l'entrée choisie ne change rien (Modèles ▾ → passe-bas, par exemple).");
    }
    else if (error.isEmpty() && c.recurrenceInput == analysis::RecurrenceInput::Above)
    {
        const auto &list = m_recipe.channels[static_cast<std::size_t>(channel)].components;
        const bool any = std::any_of(list.begin(), list.begin() + component,
                                     [](const analysis::GeneratorComponent &k) { return k.enabled; });
        if (!any) note = QStringLiteral("Aucune forme d'onde comptée au-dessus de celle-ci : x[n] vaut 0.");
    }
    info.insert(QStringLiteral("note"), note);
    QVariantList rows;
    for (const analysis::RecurrenceTrace &t : m_traces)
    {
        if (t.channel != static_cast<std::size_t>(channel) || t.component != static_cast<std::size_t>(component)) continue;
        for (std::size_t i = 0; i < t.y.size() && i < t.x.size(); ++i)
            rows.append(QVariantMap{{QStringLiteral("n"), static_cast<qint64>(i)},
                                    {QStringLiteral("x"), t.x[i]},
                                    {QStringLiteral("y"), t.y[i]}});
        break;
    }
    info.insert(QStringLiteral("rows"), rows);
    info.insert(QStringLiteral("current"), m_previewVersion == m_version && !m_running);
    return info;
}

void GeneratorController::applyRecurrenceModel(int channel, int component, const QString &model)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    analysis::GeneratorComponent &c =
        r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    // An oscillator (or a resonator) of a frequency the rate can carry.
    const double f = std::min(c.frequencyHz > 0.0 ? c.frequencyHz : 1000.0, r.sampleRateHz / 20.0);
    if (!analysis::applyRecurrenceModel(c, toUtf8(model), 1.0, f)) return;
    replace(std::move(r));
}

QString GeneratorController::recipeText() const
{
    return toQString(analysis::toJson(m_recipe).dump());
}

void GeneratorController::restoreRecipe(const QString &text)
{
    const auto json = model::Json::parse(toUtf8(text));
    if (json && json->isObject()) replace(analysis::generatorRecipeFromJson(*json));
}

bool GeneratorController::hasRecipe(qint64 signalId) const
{
    Session *session = m_session.data();
    const model::SignalPtr s = session ? session->signalList()->signalById(signalId) : model::SignalPtr();
    return s && s->provenance["generator"].isObject();
}

bool GeneratorController::takeUp(qint64 signalId)
{
    Session *session = m_session.data();
    const model::SignalPtr s = session ? session->signalList()->signalById(signalId) : model::SignalPtr();
    if (!s || !s->provenance["generator"].isObject())
    {
        setLastError(QStringLiteral("Ce signal n'a pas été fait par le générateur : pas de recette à reprendre."));
        return false;
    }
    replace(analysis::generatorRecipeFromJson(s->provenance["generator"]));
    return true;
}

qint64 GeneratorController::addToSession()
{
    Session *session = m_session.data();
    if (!session) return 0;
    if (!ready())
    {
        setLastError(!m_error.isEmpty() ? m_error : QStringLiteral("L'aperçu n'est pas encore à jour : un instant."));
        return 0;
    }
    model::Signal signal = *m_previewSignal; // the samples are shared
    signal.id = 0;
    signal.name = toUtf8(session->signalList()->uniqueName(toQString(signal.name)));
    signal.createdUtc = toUtf8(nowUtc());
    const QString label = QStringLiteral("Générer « %1 »").arg(toQString(signal.name));
    QString error;
    const qint64 id = session->signalList()->addSignal(std::move(signal), label, &error);
    if (id == 0) setLastError(error);
    return id;
}

void GeneratorController::waitForIdle()
{
    for (int round = 0; round < 1000; ++round)
    {
        if (m_delay.isActive())
        {
            m_delay.stop();
            start();
        }
        m_pool.waitForDone();
        QCoreApplication::processEvents(); // the result, sent back to this thread
        if (!m_active || (!m_running && !m_delay.isActive() && !m_again)) return;
    }
}

void GeneratorController::replace(analysis::GeneratorRecipe recipe)
{
    if (analysis::toJson(recipe) == analysis::toJson(m_recipe)) return;
    m_recipe = std::move(recipe);
    changed();
}

void GeneratorController::changed()
{
    ++m_version;
    emit recipeChanged();
    emit stateChanged(); // no longer ready
    m_delay.start();
}

void GeneratorController::start()
{
    if (!m_active) return; // setActive(true) starts it
    if (m_running)
    {
        // The run in progress is about an older recipe: it stops at its next component, this one follows.
        if (m_run) m_run->cancelled.store(true);
        m_again = true;
        return;
    }
    if (!analysis::checkGeneratorRecipe(m_recipe).empty())
    {
        // Nothing to compute: the preview goes, the panel says why (problem).
        m_previewVersion = m_version;
        m_error = problem();
        m_warnings.clear();
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
    const analysis::GeneratorRecipe recipe = m_recipe;
    m_pool.start([this, run, recipe, version] {
        const model::Progress progress{&run->cancelled, &run->permille};
        auto result = std::make_shared<analysis::GeneratorResult>(analysis::generateSignal(recipe, progress));
        QMetaObject::invokeMethod(this, [this, run, result, version] { finished(run, result, version); }, Qt::QueuedConnection);
    });
}

void GeneratorController::finished(const std::shared_ptr<Run> &run,
                                   const std::shared_ptr<analysis::GeneratorResult> &result, quint64 version)
{
    if (m_run == run) m_run.reset();
    setRunning(false);
    if (m_again)
    {
        m_again = false;
        start();
        return;
    }
    if (result->cancelled)
    {
        emit stateChanged();
        return;
    }
    m_previewVersion = version;
    m_error = toQString(result->error);
    m_warnings = toQStringList(result->warnings);
    m_traces = result->traces;
    if (result->ok())
    {
        model::Signal signal = std::move(result->signal);
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

void GeneratorController::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    if (running)
        m_progressTimer.start();
    else
        m_progressTimer.stop();
    emit busyChanged();
}

void GeneratorController::pollProgress()
{
    if (!m_run) return;
    const double p = m_run->permille.load(std::memory_order_relaxed) / 1000.0;
    if (p == m_progress) return;
    m_progress = p;
    emit progressChanged();
}

void GeneratorController::clearPreview()
{
    if (!m_previewSignal) return;
    m_previewSignal.reset();
    m_preview.signalList()->resetSignals({});
}

bool GeneratorController::validChannel(int channel) const
{
    return channel >= 0 && static_cast<std::size_t>(channel) < m_recipe.channels.size();
}

bool GeneratorController::validComponent(int channel, int component) const
{
    return validChannel(channel) && component >= 0 &&
           static_cast<std::size_t>(component) < m_recipe.channels[static_cast<std::size_t>(channel)].components.size();
}

void GeneratorController::setLastError(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
}
} // namespace qacq::app
