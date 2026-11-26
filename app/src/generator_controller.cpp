#include "qacq/app/generator_controller.h"

#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/analysis/expression.h"
#include "qacq/dsp/constants.h"
#include "qacq/plot/axis.h"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

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
      m_recipe(analysis::blankGeneratorRecipe())
{
    m_pool.setMaxThreadCount(1);
    m_delay.setSingleShot(true);
    m_delay.setInterval(kDelayMs);
    connect(&m_delay, &QTimer::timeout, this, &GeneratorController::start);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &GeneratorController::pollProgress);
    m_analysis.setSignalId(kPreviewId);
    if (session)
    {
        // The signals a recorded-signal waveform reads: the preview follows them.
        SignalListModel *list = session->signalList();
        connect(list, &QAbstractItemModel::rowsInserted, this, &GeneratorController::sessionChanged);
        connect(list, &QAbstractItemModel::rowsRemoved, this, &GeneratorController::sessionChanged);
        connect(list, &QAbstractItemModel::rowsMoved, this, &GeneratorController::sessionChanged);
        connect(list, &QAbstractItemModel::dataChanged, this, &GeneratorController::sessionChanged);
        connect(list, &QAbstractItemModel::modelReset, this, &GeneratorController::sessionChanged);
    }
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
    // The channels of the signal made; those of computation are said apart.
    qint64 channels = 0;
    for (const analysis::GeneratorChannel &c : m_recipe.channels) channels += c.output ? 1 : 0;
    const qint64 apart = static_cast<qint64>(m_recipe.channels.size()) - channels;
    const QString bytes = formatBytes(n * channels * static_cast<qint64>(sizeof(double)));
    QString text = channels == 1 ? QStringLiteral("%1 échantillons · %2").arg(formatCount(n), bytes)
                                 : QStringLiteral("%1 échantillons par voie · %2 voies · %3").arg(formatCount(n)).arg(channels).arg(bytes);
    if (apart == 1) text += QStringLiteral(" (plus 1 voie de calcul)");
    if (apart > 1) text += QStringLiteral(" (plus %1 voies de calcul)").arg(apart);
    return text;
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

QVariantList GeneratorController::messageSources() const
{
    QVariantList out;
    for (const model::SignalPtr &s : sessionSignals())
    {
        if (!s) continue;
        const QString summary = signalSummary(*s);
        for (std::size_t k = 0; k < s->channels.size(); ++k)
        {
            QVariantMap m;
            m.insert(QStringLiteral("id"), static_cast<qint64>(s->id));
            m.insert(QStringLiteral("channel"), static_cast<int>(k));
            m.insert(QStringLiteral("text"), s->channels.size() > 1
                                                 ? QStringLiteral("%1 — %2").arg(toQString(s->name), toQString(s->channels[k].name))
                                                 : toQString(s->name));
            m.insert(QStringLiteral("tip"), summary);
            out.append(m);
        }
    }
    return out;
}

bool GeneratorController::blank() const
{
    return analysis::isBlankRecipe(m_recipe);
}

QVariantList GeneratorController::sources() const
{
    QVariantList out;
    for (const model::SignalPtr &s : sessionSignals())
    {
        if (!s) continue;
        QVariantMap m;
        m.insert(QStringLiteral("id"), static_cast<qint64>(s->id));
        m.insert(QStringLiteral("name"), toQString(s->name));
        QStringList channels;
        for (const model::ChannelInfo &c : s->channels) channels << toQString(c.name);
        m.insert(QStringLiteral("channels"), channels);
        m.insert(QStringLiteral("sampleRate"), s->sampleRateHz);
        m.insert(QStringLiteral("duration"), s->durationSeconds());
        m.insert(QStringLiteral("summary"), signalSummary(*s));
        out.append(m);
    }
    return out;
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
    m_recipe.channels.push_back(analysis::blankGeneratorChannel(m_recipe.channels.size()));
    changed();
    return static_cast<int>(m_recipe.channels.size()) - 1;
}

void GeneratorController::removeChannel(int channel)
{
    if (!validChannel(channel) || m_recipe.channels.size() <= 1) return;
    std::string why;
    if (!analysis::removeGeneratorChannel(m_recipe, static_cast<std::size_t>(channel), &why))
    {
        setLastError(toQString(why));
        return;
    }
    changed();
}

void GeneratorController::clearChannel(int channel)
{
    if (!validChannel(channel)) return;
    analysis::GeneratorRecipe r = m_recipe;
    analysis::GeneratorChannel &c = r.channels[static_cast<std::size_t>(channel)];
    c.components.clear();
    c.offset = 0.0;
    replace(std::move(r));
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
    if (w == analysis::WaveKind::Formula)
        list.back().expression = analysis::defaultOperation(static_cast<std::size_t>(channel), m_recipe.channels.size());
    if (w == analysis::WaveKind::Recorded)
    {
        // The last signal of the session, until another is chosen.
        const std::vector<model::SignalPtr> signalList = sessionSignals();
        if (!signalList.empty() && signalList.back()) analysis::setRecordedSource(list.back(), *signalList.back(), 0);
    }
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
        if (analysis::waveformFromKey(changes["waveform"].toString(), w) && w != c.waveform)
        {
            c = analysis::withWaveform(c, w, r.sampleRateHz, r.durationSeconds);
            if (w == analysis::WaveKind::Formula)
                c.expression = analysis::defaultOperation(static_cast<std::size_t>(channel), r.channels.size());
            if (w == analysis::WaveKind::Recorded)
            {
                const std::vector<model::SignalPtr> signalList = sessionSignals();
                if (!signalList.empty() && signalList.back()) analysis::setRecordedSource(c, *signalList.back(), 0);
            }
        }
        changes.erase("waveform");
    }
    if (c.waveform == analysis::WaveKind::Recorded && (changes.contains("source") || changes.contains("sourceChannel")))
    {
        // Another record, or another of its channels: by its id in the session, its names kept with it.
        const std::vector<model::SignalPtr> signalList = sessionSignals();
        model::SignalPtr chosen;
        if (changes.contains("source"))
        {
            const auto id = static_cast<std::uint64_t>(std::max(0.0, changes["source"].toNumber(0.0)));
            for (const model::SignalPtr &s : signalList)
                if (s && s->id == id) chosen = s;
        }
        else
        {
            chosen = analysis::findRecordedSource(c, signalList).signal;
        }
        if (chosen)
        {
            const double asked = changes.contains("sourceChannel") ? changes["sourceChannel"].toNumber(0.0) : 0.0;
            const auto index = static_cast<std::size_t>(std::clamp(asked, 0.0, static_cast<double>(chosen->channels.size() - 1)));
            analysis::setRecordedSource(c, *chosen, chosen->channels.empty() ? 0 : index);
        }
        for (const char *key : {"source", "sourceName", "sourceFrames", "sourceChannel", "sourceChannelName"}) changes.erase(key);
    }
    // A digital frame: another protocol keeps its data, and its line the part it played; what is set here goes
    // to the other lines of its frame.
    const analysis::FrameProtocol protocolBefore = c.frame.protocol;
    if (c.waveform == analysis::WaveKind::Frame)
    {
        analysis::FrameProtocol protocol = protocolBefore;
        if (changes.contains("protocol") && analysis::frameProtocolFromKey(changes["protocol"].toString(), protocol) &&
            protocol != protocolBefore)
        {
            c.frame = analysis::withFrameProtocol(c.frame, protocol);
            c.frameLine = std::string(analysis::frameLineFor(protocolBefore, c.frameLine, protocol));
        }
        changes.erase("protocol");
        changes.erase("group");
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
    if (c.waveform == analysis::WaveKind::Frame)
        analysis::spreadFrame(r, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), protocolBefore);
    replace(std::move(r));
}

void GeneratorController::reset()
{
    replace(analysis::blankGeneratorRecipe());
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

QVariantMap GeneratorController::operationInfo(int channel, int component) const
{
    QVariantMap info;
    if (!validComponent(channel, component)) return info;
    const auto ch = static_cast<std::size_t>(channel);
    const auto index = static_cast<std::size_t>(component);
    const analysis::GeneratorComponent &c = m_recipe.channels[ch].components[index];
    if (c.waveform != analysis::WaveKind::Formula && c.waveform != analysis::WaveKind::Modulation) return info;
    const analysis::OperationCheck check = analysis::checkOperation(m_recipe, ch, index);
    info.insert(QStringLiteral("error"), toQString(check.error));
    QString caution;
    QStringList notes;
    if (check.error.empty())
    {
        // The channels read by their names: v1 « Tension », v2 « Courant ».
        QStringList names;
        for (std::size_t k : check.channels)
        {
            if (k >= m_recipe.channels.size()) continue;
            names << QStringLiteral("v%1 : « %2 »").arg(static_cast<int>(k) + 1).arg(toQString(m_recipe.channels[k].name));
        }
        if (!names.isEmpty()) notes << names.join(QStringLiteral(", ")) + QStringLiteral(".");
        if (check.readsAbove)
        {
            const auto &list = m_recipe.channels[ch].components;
            const bool any = std::any_of(list.begin(), list.begin() + component,
                                         [](const analysis::GeneratorComponent &k) { return k.enabled; });
            if (!any)
                caution = QStringLiteral("Aucune forme d'onde comptée au-dessus de celle-ci : x vaut 0.");
            else if (c.waveform == analysis::WaveKind::Modulation)
            {
                // The message named: the waveforms above it, which the carrier replaces in the channel.
                QStringList above;
                for (int k = 0; k < component; ++k)
                {
                    const analysis::GeneratorComponent &a = list[static_cast<std::size_t>(k)];
                    if (a.enabled) above << toQString(analysis::componentText(a, m_recipe.channels[ch].unit));
                }
                notes << (above.size() == 1
                              ? QStringLiteral("x : la forme d'onde au-dessus (%1). Elle module la porteuse, qui la remplace dans la voie.").arg(above.front())
                              : QStringLiteral("x : la somme des %1 formes d'onde au-dessus (%2). Elle module la porteuse, qui la remplace dans la voie.")
                                    .arg(above.size())
                                    .arg(above.join(QStringLiteral(" + "))));
            }
            else
                notes << QStringLiteral("x : les formes d'onde au-dessus, que le résultat remplace.");
        }
    }
    info.insert(QStringLiteral("caution"), caution);
    info.insert(QStringLiteral("note"), notes.join(QStringLiteral(" ")));
    return info;
}

namespace
{
QVariantList modelList(const std::vector<analysis::OperationModel> &models)
{
    QVariantList list;
    for (const analysis::OperationModel &m : models)
        list.append(QVariantMap{{QStringLiteral("key"), toQString(m.key)},
                                {QStringLiteral("name"), toQString(m.name)},
                                {QStringLiteral("formula"), toQString(m.formula)}});
    return list;
}
} // namespace

QVariantList GeneratorController::operationModels(int channel) const
{
    if (!validChannel(channel)) return {};
    return modelList(analysis::operationModels(static_cast<std::size_t>(channel), m_recipe.channels.size()));
}

void GeneratorController::applyOperationModel(int channel, int component, const QString &model)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    analysis::GeneratorComponent &c =
        r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    if (!analysis::applyOperationModel(c, toUtf8(model), static_cast<std::size_t>(channel), r.channels.size())) return;
    replace(std::move(r));
}

QVariantList GeneratorController::messageModels(int channel) const
{
    if (!validChannel(channel)) return {};
    QVariantList list = modelList(analysis::messageModels(static_cast<std::size_t>(channel), m_recipe.channels.size()));
    // Each channel of each signal of the session: the record as the message, placed above the carrier (x).
    for (const model::SignalPtr &s : sessionSignals())
    {
        if (!s) continue;
        for (std::size_t k = 0; k < s->channels.size(); ++k)
        {
            const QString name = s->channels.size() > 1
                                     ? QStringLiteral("Le signal « %1 », voie %2").arg(toQString(s->name), toQString(s->channels[k].name))
                                     : QStringLiteral("Le signal « %1 »").arg(toQString(s->name));
            list.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("signal:%1:%2").arg(static_cast<qint64>(s->id)).arg(static_cast<int>(k))},
                                    {QStringLiteral("name"), name},
                                    {QStringLiteral("formula"), QStringLiteral("x, ramené à 1 V crête")}});
        }
    }
    return list;
}

void GeneratorController::applyMessageModel(int channel, int component, const QString &model)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    if (model.startsWith(QStringLiteral("signal:")))
    {
        // A signal of the session as the message: "signal:<id>:<channel>".
        const QStringList parts = model.split(QLatin1Char(':'));
        bool idRead = false, channelRead = false;
        const qint64 id = parts.size() == 3 ? parts[1].toLongLong(&idRead) : 0;
        const int source = parts.size() == 3 ? parts[2].toInt(&channelRead) : -1;
        Session *session = m_session.data();
        const model::SignalPtr signal = idRead && session ? session->signalList()->signalById(id) : model::SignalPtr();
        if (!signal || !channelRead || source < 0)
        {
            setLastError(QStringLiteral("Ce signal n'existe plus."));
            return;
        }
        std::string why;
        if (!analysis::useSignalAsMessage(r, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), *signal,
                                          static_cast<std::size_t>(source), &why))
        {
            setLastError(toQString(why));
            return;
        }
        replace(std::move(r));
        return;
    }
    analysis::GeneratorComponent &c =
        r.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    if (!analysis::applyMessageModel(c, toUtf8(model), static_cast<std::size_t>(channel), r.channels.size())) return;
    replace(std::move(r));
}

QVariantMap GeneratorController::sourceInfo(int channel, int component) const
{
    QVariantMap out;
    if (!validComponent(channel, component)) return out;
    const analysis::GeneratorComponent &c =
        m_recipe.channels[static_cast<std::size_t>(channel)].components[static_cast<std::size_t>(component)];
    if (c.waveform != analysis::WaveKind::Recorded) return out;
    const std::vector<model::SignalPtr> signalList = sessionSignals();
    if (signalList.empty())
    {
        out.insert(QStringLiteral("error"),
                   QStringLiteral("Aucun signal dans la session : importez un fichier, lancez une acquisition, ou ajoutez-y un signal "
                                  "fait ici (Ajouter)."));
        return out;
    }
    const analysis::RecordedSource source = analysis::findRecordedSource(c, signalList);
    if (!source.ok())
    {
        out.insert(QStringLiteral("error"), toQString(source.error));
        return out;
    }
    const model::Signal &s = *source.signal;
    out.insert(QStringLiteral("id"), static_cast<qint64>(s.id));
    out.insert(QStringLiteral("channel"), static_cast<int>(source.channel));
    out.insert(QStringLiteral("name"), toQString(s.name));
    out.insert(QStringLiteral("channelName"), toQString(s.channels[source.channel].name));
    out.insert(QStringLiteral("unit"), toQString(s.channels[source.channel].unit));
    QStringList channels;
    for (const model::ChannelInfo &info : s.channels)
        channels << (info.unit.empty() ? toQString(info.name) : toQString(info.name) + QStringLiteral(" (") + toQString(info.unit) + QLatin1Char(')'));
    out.insert(QStringLiteral("channels"), channels);
    out.insert(QStringLiteral("sampleRate"), s.sampleRateHz);
    out.insert(QStringLiteral("duration"), s.durationSeconds());
    out.insert(QStringLiteral("summary"), signalSummary(s));
    const bool resampled = std::abs(s.sampleRateHz - m_recipe.sampleRateHz) > 1e-9 * m_recipe.sampleRateHz;
    analysis::GeneratorRecipe adopted = m_recipe;
    const bool can = analysis::adoptRecordedTiming(adopted, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), signalList);
    out.insert(QStringLiteral("resampled"), resampled);
    out.insert(QStringLiteral("differs"),
               can && (resampled || std::abs(adopted.durationSeconds - m_recipe.durationSeconds) > 0.5 / m_recipe.sampleRateHz));
    return out;
}

void GeneratorController::adoptSourceTiming(int channel, int component)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    if (!analysis::adoptRecordedTiming(r, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), sessionSignals())) return;
    const std::string problem = analysis::checkGeneratorRecipe(r);
    if (!problem.empty())
    {
        setLastError(toQString(problem));
        return;
    }
    replace(std::move(r));
}

QVariantMap GeneratorController::frameInfo(int channel, int component) const
{
    QVariantMap out;
    if (!validComponent(channel, component)) return out;
    const auto ch = static_cast<std::size_t>(channel), k = static_cast<std::size_t>(component);
    const analysis::GeneratorComponent &c = m_recipe.channels[ch].components[k];
    if (c.waveform != analysis::WaveKind::Frame) return out;
    const std::span<const analysis::FrameLineInfo> lines = analysis::frameLines(c.frame.protocol);
    QStringList names, keys;
    for (const analysis::FrameLineInfo &line : lines)
    {
        names << toQString(std::string(line.name));
        keys << toQString(std::string(line.key));
    }
    out.insert(QStringLiteral("lines"), names);
    out.insert(QStringLiteral("lineKeys"), keys);
    out.insert(QStringLiteral("line"), static_cast<int>(analysis::frameLineIndex(c.frame.protocol, c.frameLine)));
    out.insert(QStringLiteral("title"), toQString(analysis::frameTitle(c.frame)));
    // The lines of its frame in the recipe, and those the recipe has not.
    const std::vector<analysis::FrameMember> members = analysis::frameMembers(m_recipe, ch, k);
    std::vector<bool> present(lines.size(), false);
    QStringList where, missing;
    for (const analysis::FrameMember &m : members)
    {
        present[m.line] = true;
        where << QStringLiteral("%1 (voie %2)").arg(names.at(static_cast<qsizetype>(m.line))).arg(static_cast<int>(m.channel) + 1);
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (!present[i]) missing << names.at(static_cast<qsizetype>(i));
    out.insert(QStringLiteral("members"), members.size() > 1 ? where.join(QStringLiteral(" · ")) : QString());
    out.insert(QStringLiteral("missing"), missing);

    const analysis::FrameTiming t = analysis::frameTiming(c, m_recipe.sampleRateHz);
    if (!t.ok())
    {
        out.insert(QStringLiteral("error"), toQString(t.frame.error));
        return out;
    }
    out.insert(QStringLiteral("construction"), toQString(analysis::frameConstruction(t.frame)));
    out.insert(QStringLiteral("legend"), toQString(t.frame.legend));
    out.insert(QStringLiteral("summary"), toQString(t.frame.summary));
    const double bits = t.frame.bitCount();
    out.insert(QStringLiteral("bits"), bits);
    out.insert(QStringLiteral("duration"), t.durationSeconds);
    out.insert(QStringLiteral("samplesPerBit"), t.samplesPerBit);
    const bool whole = bits == std::floor(bits);
    out.insert(QStringLiteral("timing"),
               (whole ? formatCount(static_cast<qint64>(bits)) : toQString(plot::formatCompact(bits, "", 6))) +
                   (bits >= 2.0 ? QStringLiteral(" bits · ") : QStringLiteral(" bit · ")) + toQString(plot::formatCompact(t.durationSeconds, "s", 4)) +
                   QStringLiteral(" · ") + toQString(plot::formatCompact(t.samplesPerBit, "", 4)) +
                   (t.samplesPerBit >= 2.0 ? QStringLiteral(" échantillons par bit") : QStringLiteral(" échantillon par bit")));
    const double duration = m_recipe.durationSeconds;
    out.insert(QStringLiteral("sampled"), t.samplesPerTick >= 1.0 - 1e-9);
    out.insert(QStringLiteral("fits"),
               c.frameRepeat || (c.eventSeconds < duration && t.endSeconds <= duration * (1.0 + 1e-9) + 0.5 / m_recipe.sampleRateHz));
    out.insert(QStringLiteral("warnings"), toQStringList(t.frame.warnings));
    const double real = analysis::fixedFrameRate(c.frame);
    out.insert(QStringLiteral("realRate"), real > 0.0 && std::abs(real - c.frequencyHz) > 1e-6 * real ? real : 0.0);
    // Whether framing the signal on it would change something.
    analysis::GeneratorRecipe framed = m_recipe;
    bool differs = false;
    if (analysis::adoptFrameTiming(framed, ch, k))
    {
        const auto apart = [](double a, double b) { return std::abs(a - b) > 1e-9 * std::max(std::abs(a), std::abs(b)); };
        differs = apart(framed.sampleRateHz, m_recipe.sampleRateHz) || apart(framed.durationSeconds, m_recipe.durationSeconds) ||
                  apart(framed.channels[ch].components[k].eventSeconds, c.eventSeconds);
    }
    out.insert(QStringLiteral("differs"), differs);
    return out;
}

void GeneratorController::addFrameLines(int channel, int component)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    std::string why;
    if (!analysis::addFrameLines(r, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), nullptr, &why))
    {
        if (!why.empty()) setLastError(toQString(why));
        return;
    }
    replace(std::move(r));
}

void GeneratorController::adoptFrameTiming(int channel, int component, bool realRate)
{
    if (!validComponent(channel, component)) return;
    analysis::GeneratorRecipe r = m_recipe;
    if (!analysis::adoptFrameTiming(r, static_cast<std::size_t>(channel), static_cast<std::size_t>(component), realRate)) return;
    const std::string problem = analysis::checkGeneratorRecipe(r);
    if (!problem.empty())
    {
        setLastError(toQString(problem));
        return;
    }
    replace(std::move(r));
}

int GeneratorController::useSignal(qint64 signalId, const QString &use, int channel)
{
    Session *session = m_session.data();
    const model::SignalPtr s = session ? session->signalList()->signalById(signalId) : model::SignalPtr();
    if (!s || s->channels.empty())
    {
        setLastError(QStringLiteral("Ce signal n'existe plus."));
        return -1;
    }
    if (channel < 0 || static_cast<std::size_t>(channel) >= s->channels.size())
    {
        setLastError(QStringLiteral("Ce signal n'a pas cette voie."));
        return -1;
    }
    analysis::GeneratorRecipe r = m_recipe;
    const analysis::SignalUse done = analysis::useSignalInRecipe(r, *s, static_cast<std::size_t>(channel), toUtf8(use));
    if (!done.ok())
    {
        setLastError(toQString(done.error));
        return -1;
    }
    replace(std::move(r));
    const int index = static_cast<int>(done.channel);
    emit signalUsed(index, toQString(done.text));
    return index;
}

int GeneratorController::modulateChannel(int channel, const QString &use)
{
    if (!validChannel(channel))
    {
        setLastError(QStringLiteral("Cette voie n'existe plus."));
        return -1;
    }
    analysis::GeneratorRecipe r = m_recipe;
    const analysis::SignalUse done = analysis::modulateChannelInRecipe(r, static_cast<std::size_t>(channel), toUtf8(use));
    if (!done.ok())
    {
        setLastError(toQString(done.error));
        return -1;
    }
    replace(std::move(r));
    const int index = static_cast<int>(done.channel);
    emit signalUsed(index, toQString(done.text));
    return index;
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
    // The signals of the session its recorded waveforms read (shared, never changed: a signal edited is another).
    const std::vector<model::SignalPtr> signalList = analysis::readsRecordedSignals(recipe) ? sessionSignals() : std::vector<model::SignalPtr>();
    m_sourceStamp = sourceStamp(signalList);
    m_pool.start([this, run, recipe, version, signalList] {
        const model::Progress progress{&run->cancelled, &run->permille};
        auto result = std::make_shared<analysis::GeneratorResult>(analysis::generateSignal(recipe, progress, signalList));
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

std::vector<model::SignalPtr> GeneratorController::sessionSignals() const
{
    Session *session = m_session.data();
    return session ? session->signalList()->document().signalList() : std::vector<model::SignalPtr>();
}

std::vector<const model::Signal *> GeneratorController::sourceStamp(const std::vector<model::SignalPtr> &sources) const
{
    std::vector<const model::Signal *> stamp;
    for (const analysis::GeneratorChannel &channel : m_recipe.channels)
        for (const analysis::GeneratorComponent &c : channel.components)
            if (c.waveform == analysis::WaveKind::Recorded) stamp.push_back(analysis::findRecordedSource(c, sources).signal.get());
    return stamp;
}

void GeneratorController::sessionChanged()
{
    emit sourcesChanged();
    if (!analysis::readsRecordedSignals(m_recipe)) return;
    // The recipe follows the names of the records it reads (renamed, a channel taken out); the preview is made
    // again if they are no longer the ones it was made from.
    const std::vector<model::SignalPtr> signalList = sessionSignals();
    analysis::GeneratorRecipe r = m_recipe;
    for (analysis::GeneratorChannel &channel : r.channels)
    {
        for (analysis::GeneratorComponent &c : channel.components)
        {
            if (c.waveform != analysis::WaveKind::Recorded) continue;
            const analysis::RecordedSource source = analysis::findRecordedSource(c, signalList);
            if (source.ok()) analysis::setRecordedSource(c, *source.signal, source.channel);
        }
    }
    const bool renamed = !(analysis::toJson(r) == analysis::toJson(m_recipe));
    if (renamed) m_recipe = std::move(r);
    if (renamed || sourceStamp(signalList) != m_sourceStamp) changed();
}
} // namespace qacq::app
