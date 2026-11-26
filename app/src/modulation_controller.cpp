#include "qacq/app/modulation_controller.h"

#include "qacq/app/qt_convert.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/plot/axis.h"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace qacq::app
{
namespace
{
constexpr int kDelayMs = 150; // after the last change, before the link is computed again

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

model::Json sweepJson(const analysis::BerSweep &s)
{
    model::Json j = model::Json::object();
    j["from"] = s.fromDb;
    j["to"] = s.toDb;
    j["step"] = s.stepDb;
    j["maxBits"] = static_cast<unsigned long long>(s.maxBits);
    j["targetErrors"] = static_cast<unsigned long long>(s.targetErrors);
    return j;
}

double finiteOr(const model::Json &j, std::string_view key, double fallback)
{
    const model::Json &v = j[key];
    if (!v.isNumber()) return fallback;
    const double x = v.toNumber(fallback);
    return std::isfinite(x) ? x : fallback;
}

analysis::BerSweep sweepFromJson(const model::Json &j, analysis::BerSweep s)
{
    if (!j.isObject()) return s;
    s.fromDb = finiteOr(j, "from", s.fromDb);
    s.toDb = finiteOr(j, "to", s.toDb);
    s.stepDb = finiteOr(j, "step", s.stepDb);
    s.maxBits = static_cast<std::size_t>(std::clamp(finiteOr(j, "maxBits", static_cast<double>(s.maxBits)), 0.0, 1e12));
    s.targetErrors = static_cast<std::size_t>(std::clamp(finiteOr(j, "targetErrors", static_cast<double>(s.targetErrors)), 1.0, 1e9));
    return s;
}

QVariantMap pointInfo(const analysis::BerPoint &p)
{
    return {{QStringLiteral("ebN0"), p.ebN0Db},     {QStringLiteral("bits"), static_cast<qint64>(p.bits)},
            {QStringLiteral("errors"), static_cast<qint64>(p.errors)},
            {QStringLiteral("ber"), p.ber},         {QStringLiteral("lower"), p.lower},
            {QStringLiteral("upper"), p.upper},     {QStringLiteral("theory"), p.theory},
            {QStringLiteral("batches"), static_cast<qint64>(p.batches)}};
}

// Before and after the channel, for the preview: on a carrier the real signals (V); as I/Q the two pairs. Through
// a channel the record received is longer (the delay, the clock, a few symbols after the end): the transmission
// is silent after its own end.
model::Signal previewOf(const analysis::LinkResult &r)
{
    model::Signal s;
    s.name = "Aperçu : " + analysis::linkShortText(r.recipe);
    s.origin = model::Origin::Generated;
    s.sampleRateHz = r.recipe.sampleRateHz;
    auto data = std::make_shared<model::SampleData>();
    if (r.recipe.representation == dsp::Representation::RealPassband)
    {
        data->channels.push_back(r.sentPassband);
        data->channels.push_back(r.receivedPassband);
        s.channels = {{"Émis", "V"}, {"Reçu", "V"}};
    }
    else
    {
        data->channels.push_back(r.sentI);
        data->channels.push_back(r.sentQ);
        data->channels.push_back(r.receivedI);
        data->channels.push_back(r.receivedQ);
        s.channels = {{"I émis", "V"}, {"Q émis", "V"}, {"I reçu", "V"}, {"Q reçu", "V"}};
        s.representation = model::Representation::IQ;
    }
    std::size_t longest = 0;
    for (const std::vector<double> &channel : data->channels) longest = std::max(longest, channel.size());
    for (std::vector<double> &channel : data->channels) channel.resize(longest, 0.0);
    s.data = std::move(data);
    return s;
}

// A signed number as read: "+200", "−100", "0".
QString signedText(double value, int digits)
{
    const QString text = toQString(plot::formatCompact(value, "", digits));
    return value > 0.0 ? QStringLiteral("+") + text : text;
}

// What the synchronized receiver found, against the truth, as the panel shows it.
QVariantMap syncInfo(const analysis::LinkResult &res)
{
    const analysis::LinkSync &y = res.sync;
    const bool fsk = res.recipe.modulation == dsp::Modulation::Fsk;
    QVariantMap info;
    info.insert(QStringLiteral("used"), y.used);
    if (!y.used) return info;
    info.insert(QStringLiteral("frameFound"), y.frameFound);
    info.insert(QStringLiteral("frameQuality"), y.frameQuality);
    info.insert(QStringLiteral("delay"), y.delaySymbols);
    info.insert(QStringLiteral("trueDelay"), y.trueDelaySymbols);
    info.insert(QStringLiteral("frequencyOffset"), y.frequencyOffsetHz);
    info.insert(QStringLiteral("trueFrequencyOffset"), y.trueFrequencyOffsetHz);
    info.insert(QStringLiteral("phase"), y.phaseDegrees);
    info.insert(QStringLiteral("truePhase"), y.truePhaseDegrees);
    info.insert(QStringLiteral("clock"), y.clockOffsetPpm);
    info.insert(QStringLiteral("trueClock"), y.trueClockOffsetPpm);
    info.insert(QStringLiteral("timingRms"), y.timingErrorRms);
    info.insert(QStringLiteral("phaseRms"), y.phaseErrorRms);
    info.insert(QStringLiteral("slips"), static_cast<qint64>(y.phaseSlips));
    info.insert(QStringLiteral("symbols"), static_cast<qint64>(y.timingError.size()));
    info.insert(QStringLiteral("preamble"), static_cast<qint64>(res.recipe.preambleSymbols));
    info.insert(QStringLiteral("carrier"), !fsk); // a carrier to find (not for the FSK, decided without phase)
    // Slips are counted where the constellation has a symmetry to slip to (not the ASK).
    info.insert(QStringLiteral("slipsCounted"), dsp::rotationalSymmetryDegrees(res.recipe.modulation) > 0.0);
    // As read: found, then the truth.
    info.insert(QStringLiteral("frameText"),
                QStringLiteral("%1 %2 symb. (vrai %3) · corrélation %4 %")
                    .arg(y.frameFound ? QStringLiteral("trouvée à") : QStringLiteral("mal trouvée, à"), toQString(plot::formatFixed(y.delaySymbols, 2)),
                         toQString(plot::formatCompact(y.trueDelaySymbols, "", 5)))
                    .arg(static_cast<int>(std::lround(100.0 * y.frameQuality))));
    info.insert(QStringLiteral("frequencyText"), QStringLiteral("%1 Hz (vrai %2 Hz)")
                                                     .arg(toQString(plot::formatFixed(y.frequencyOffsetHz, 2)),
                                                          toQString(plot::formatCompact(y.trueFrequencyOffsetHz, "", 5))));
    info.insert(QStringLiteral("phaseText"), QStringLiteral("%1° (vraie %2°)")
                                                 .arg(toQString(plot::formatFixed(y.phaseDegrees, 1)), toQString(plot::formatFixed(y.truePhaseDegrees, 1))));
    info.insert(QStringLiteral("clockText"),
                QStringLiteral("%1 ppm (vraie %2)").arg(signedText(std::round(10.0 * y.clockOffsetPpm) / 10.0, 5), signedText(y.trueClockOffsetPpm, 5)));
    info.insert(QStringLiteral("timingLost"), y.timingLost);
    info.insert(QStringLiteral("timingText"), QStringLiteral("%1 symbole efficace%2")
                                                  .arg(toQString(plot::formatFixed(y.timingErrorRms, 3)),
                                                       y.timingLost ? QStringLiteral(" (boucle décrochée)") : QString()));
    info.insert(QStringLiteral("phaseErrorText"),
                fsk ? QString() : QStringLiteral("%1° efficace").arg(toQString(plot::formatFixed(y.phaseErrorRms, 2))));
    return info;
}

// "4,62 dB", or nothing when unknown.
QString decibelText(double value)
{
    if (!std::isfinite(value)) return {};
    return toQString(plot::formatFixed(value, std::abs(value) < 10.0 ? 2 : 1)) + QStringLiteral(" dB");
}

// Several paths and the equalizer, as the panel shows them.
QVariantMap equalizationInfo(const analysis::LinkResult &res)
{
    const analysis::LinkEqualization &q = res.equalization;
    QVariantMap info;
    info.insert(QStringLiteral("paths"), q.paths);
    info.insert(QStringLiteral("pathCount"), static_cast<int>(q.typed.size()));
    info.insert(QStringLiteral("used"), q.used);
    info.insert(QStringLiteral("adaptive"), q.adaptive);
    info.insert(QStringLiteral("halfSymbolEarly"), q.halfSymbolEarly);
    info.insert(QStringLiteral("taps"), static_cast<int>(q.taps.size()));
    info.insert(QStringLiteral("feedback"), static_cast<int>(q.feedback.size()));
    // Each path: when it arrives, how strong once their powers add up to that of the transmission, turned by how much.
    QStringList paths;
    for (std::size_t k = 0; k < q.typed.size() && k < q.amplitude.size(); ++k)
    {
        QString path = QStringLiteral("%1 symb. : %2")
                           .arg(toQString(plot::formatCompact(q.typed[k].delaySymbols, "", 4)),
                                decibelText(20.0 * std::log10(std::max(1e-12, q.amplitude[k]))));
        if (q.typed[k].phaseDegrees != 0.0) path += QStringLiteral(" à %1°").arg(toQString(plot::formatCompact(q.typed[k].phaseDegrees, "", 4)));
        paths << path;
    }
    info.insert(QStringLiteral("pathsText"), paths.join(QStringLiteral(" · ")));
    // A symbol against the other symbols and the noise at the decisions: without equalizer, after it; what that is
    // worth per bit, and the bit error rate of the theory there (computed equalizer); the error a learnt one ends at.
    info.insert(QStringLiteral("beforeText"), decibelText(q.sinrBeforeDb));
    info.insert(QStringLiteral("afterText"), decibelText(q.sinrAfterDb));
    const bool predicted = q.used && !q.adaptive && res.recipe.noise && std::isfinite(q.equivalentEbN0Db);
    info.insert(QStringLiteral("equivalentText"), predicted ? decibelText(q.equivalentEbN0Db) : QString());
    info.insert(QStringLiteral("predictedText"), predicted ? toQString(analysis::rateText(q.predictedBer)) : QString());
    info.insert(QStringLiteral("errorText"), decibelText(q.settledErrorDb));
    // Where the decisions are taken against the strongest path (a timing loop settles between two paths a fraction
    // of a symbol apart), when it is not on it; and what the symbol itself is worth there against that path alone.
    info.insert(QStringLiteral("instant"), q.instantSymbols);
    info.insert(QStringLiteral("instantText"),
                q.paths && std::abs(q.instantSymbols) >= analysis::kShownDecisionOffsetSymbols
                    ? QStringLiteral("%1 symb. %2 le trajet le plus fort")
                          .arg(toQString(plot::formatFixed(std::abs(q.instantSymbols), 2)),
                               q.instantSymbols > 0.0 ? QStringLiteral("après") : QStringLiteral("avant"))
                    : QString());
    info.insert(QStringLiteral("symbolGain"), q.symbolGain);
    return info;
}
} // namespace

ModulationController::ModulationController(Session *session, QObject *parent)
    : QObject(parent), m_session(session), m_preview(nullptr, this), m_analysis(&m_preview, this),
      m_recipe(analysis::defaultLinkRecipe())
{
    m_pool.setMaxThreadCount(1);
    m_curvePool.setMaxThreadCount(1);
    m_delay.setSingleShot(true);
    m_delay.setInterval(kDelayMs);
    connect(&m_delay, &QTimer::timeout, this, &ModulationController::start);
    m_progressTimer.setInterval(150);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        const std::shared_ptr<Run> run = m_curveRun;
        if (!run || m_curvePoints == 0) return;
        // Points done, and the one under way.
        std::size_t done = 0;
        if (!m_curves.empty()) done = m_curves.back().points.size();
        const double p = (static_cast<double>(done) + run->permille.load(std::memory_order_relaxed) / 1000.0) /
                         static_cast<double>(m_curvePoints);
        if (p == m_curveProgress) return;
        m_curveProgress = std::min(1.0, p);
        emit curveProgressChanged();
    });
    m_analysis.setSignalId(kPreviewId);
    refreshResultInfo();
    // A blank page: nothing computed until the recipe is first changed.
}

ModulationController::~ModulationController()
{
    m_delay.stop();
    m_progressTimer.stop();
    if (m_run) m_run->cancelled.store(true);
    if (m_curveRun) m_curveRun->cancelled.store(true);
    m_pool.waitForDone();
    m_curvePool.waitForDone();
}

QVariantMap ModulationController::recipe() const
{
    return jsonToVariant(analysis::toJson(m_recipe)).toMap();
}

bool ModulationController::ready() const
{
    return !m_blank && !m_running && !m_delay.isActive() && m_resultVersion == m_version;
}

void ModulationController::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    if (m_active && m_resultVersion != m_version) schedule();
}

QVariantList ModulationController::examples() const
{
    QVariantList list;
    for (const analysis::LinkExample &e : analysis::linkExamples())
        list.append(QVariantMap{{QStringLiteral("name"), toQString(e.name)}, {QStringLiteral("tip"), toQString(e.tip)}});
    return list;
}

void ModulationController::setRecipe(const QVariantMap &changes)
{
    const model::Json json = variantToJson(changes);
    const bool wasBlank = std::exchange(m_blank, false);
    const bool changed = replace(analysis::linkRecipeFromJson(merged(analysis::toJson(m_recipe), json)));
    if (wasBlank) leaveBlank(changed);
}

void ModulationController::leaveBlank(bool changed)
{
    // The recipe chosen as it was (the first choice of the blank page may be what it already has): computed all the same.
    if (!changed)
    {
        ++m_version;
        schedule();
    }
    emit stateChanged();
}

void ModulationController::reset()
{
    // What was under way is dropped: its result would come after the page is blank.
    if (m_run) m_run->cancelled.store(true);
    m_again = false;
    m_delay.stop();
    const bool changed = analysis::toJson(m_recipe) != analysis::toJson(analysis::defaultLinkRecipe());
    m_recipe = analysis::defaultLinkRecipe();
    m_blank = true;
    ++m_version;
    m_result.reset();
    refreshResultInfo();
    if (m_previewSignal)
    {
        m_previewSignal.reset();
        m_preview.signalList()->resetSignals({});
    }
    if (changed) emit recipeChanged();
    emit resultChanged();
    emit stateChanged();
}

void ModulationController::useExample(int index)
{
    const auto &list = analysis::linkExamples();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) return;
    const bool wasBlank = std::exchange(m_blank, false);
    const bool changed = replace(list[static_cast<std::size_t>(index)].recipe);
    if (wasBlank) leaveBlank(changed);
}

QString ModulationController::recipeText() const
{
    return toQString(analysis::toJson(m_recipe).dump());
}

void ModulationController::restoreRecipe(const QString &text)
{
    const auto json = model::Json::parse(toUtf8(text));
    if (!json || !json->isObject()) return;
    const bool wasBlank = std::exchange(m_blank, false);
    const bool changed = replace(analysis::linkRecipeFromJson(*json));
    if (wasBlank) leaveBlank(changed);
}

bool ModulationController::replace(analysis::LinkRecipe recipe)
{
    if (analysis::toJson(recipe) == analysis::toJson(m_recipe)) return false;
    m_recipe = std::move(recipe);
    ++m_version;
    emit recipeChanged();
    schedule();
    return true;
}

void ModulationController::schedule()
{
    m_delay.start();
    emit stateChanged();
}

void ModulationController::start()
{
    if (!m_active || m_blank) return;
    if (m_running)
    {
        if (m_run) m_run->cancelled.store(true);
        m_again = true;
        return;
    }
    auto run = std::make_shared<Run>();
    m_run = run;
    setRunning(true);
    emit stateChanged();
    const quint64 version = m_version;
    const analysis::LinkRecipe recipe = m_recipe;
    m_pool.start([this, run, recipe, version] {
        const model::Progress progress{&run->cancelled, &run->permille};
        auto result = std::make_shared<const analysis::LinkResult>(analysis::simulateLink(recipe, progress));
        QMetaObject::invokeMethod(this, [this, run, result, version] { finished(run, result, version); }, Qt::QueuedConnection);
    });
}

void ModulationController::finished(const std::shared_ptr<Run> &run, const std::shared_ptr<const analysis::LinkResult> &result,
                                    quint64 version)
{
    if (m_run == run) m_run.reset();
    setRunning(false);
    if (m_again)
    {
        m_again = false;
        start();
        return;
    }
    if (run->cancelled.load())
    {
        emit stateChanged();
        return;
    }
    m_resultVersion = version;
    m_result = result;
    refreshResultInfo();
    if (result->ok())
    {
        model::Signal signal = previewOf(*result);
        signal.id = static_cast<std::uint64_t>(kPreviewId);
        signal.createdUtc = toUtf8(nowUtc());
        auto shown = std::make_shared<const model::Signal>(std::move(signal));
        m_previewSignal = shown;
        m_preview.signalList()->resetSignals({shown});
    }
    else if (m_previewSignal)
    {
        m_previewSignal.reset();
        m_preview.signalList()->resetSignals({});
    }
    emit resultChanged();
    emit stateChanged();
}

void ModulationController::refreshResultInfo()
{
    QVariantMap info;
    const analysis::LinkRecipe &r = m_result ? m_result->recipe : m_recipe;
    info.insert(QStringLiteral("text"), toQString(analysis::linkText(r)));
    info.insert(QStringLiteral("bitsPerSymbol"), dsp::bitsPerSymbol(r.modulation));
    info.insert(QStringLiteral("fsk"), r.modulation == dsp::Modulation::Fsk);
    info.insert(QStringLiteral("iq"), r.representation == dsp::Representation::ComplexBaseband);
    info.insert(QStringLiteral("rrc"), r.pulse == dsp::PulseShape::RootRaisedCosine && r.modulation != dsp::Modulation::Fsk);
    info.insert(QStringLiteral("channel"), analysis::linkHasChannel(r));
    info.insert(QStringLiteral("synchronizes"), r.receiver == analysis::LinkReceiver::Synchronized);
    info.insert(QStringLiteral("hasPaths"), analysis::linkHasPaths(r));
    // An equalizer asked, for a modulation that has one (not the FSK).
    info.insert(QStringLiteral("equalizes"), r.equalizer != analysis::LinkEqualizer::None && dsp::hasConstellation(r.modulation));
    info.insert(QStringLiteral("fades"), r.fading != analysis::LinkFading::None);
    if (!m_result)
    {
        info.insert(QStringLiteral("ok"), false);
        info.insert(QStringLiteral("error"), QString());
        m_resultInfo = info;
        return;
    }
    const analysis::LinkResult &res = *m_result;
    info.insert(QStringLiteral("ok"), res.ok());
    info.insert(QStringLiteral("error"), toQString(res.error));
    info.insert(QStringLiteral("warnings"), toQStringList(res.warnings));
    info.insert(QStringLiteral("summary"), toQStringList(analysis::linkSummary(res)));
    if (res.ok())
    {
        // (Wilson's; through fading, widened by blocks of symbols.)
        const double lower = res.berLower, upper = res.berUpper;
        info.insert(QStringLiteral("berBatches"), static_cast<int>(res.berBatches));
        info.insert(QStringLiteral("constellationSize"), static_cast<int>(res.constellation.size()));
        info.insert(QStringLiteral("bitRate"), res.bitRate);
        info.insert(QStringLiteral("bandwidth"), res.occupiedBandwidthHz);
        info.insert(QStringLiteral("samplesPerSymbol"), res.samplesPerSymbol);
        info.insert(QStringLiteral("symbols"), static_cast<qint64>(res.sentSymbols.size()));
        info.insert(QStringLiteral("skipSymbols"), static_cast<qint64>(res.skipSymbols));
        info.insert(QStringLiteral("skipEndSymbols"), static_cast<qint64>(res.skipEndSymbols));
        // The records: the transmission, and what the receiver got (longer through a channel).
        const bool passband = r.representation == dsp::Representation::RealPassband;
        info.insert(QStringLiteral("sentSamples"), static_cast<qint64>(passband ? res.sentPassband.size() : res.sentI.size()));
        info.insert(QStringLiteral("receivedSamples"), static_cast<qint64>(passband ? res.receivedPassband.size() : res.receivedI.size()));
        info.insert(QStringLiteral("sync"), syncInfo(res));
        info.insert(QStringLiteral("equalization"), equalizationInfo(res));
        info.insert(QStringLiteral("comparedBits"), static_cast<qint64>(res.comparedBits));
        info.insert(QStringLiteral("bitErrors"), static_cast<qint64>(res.bitErrors));
        info.insert(QStringLiteral("ber"), res.ber);
        info.insert(QStringLiteral("berLower"), lower);
        info.insert(QStringLiteral("berUpper"), upper);
        info.insert(QStringLiteral("theoryBer"), res.theoryBer);
        info.insert(QStringLiteral("awgnTheoryBer"), res.awgnTheoryBer);
        info.insert(QStringLiteral("awgnTheoryText"), toQString(analysis::rateText(res.awgnTheoryBer)));
        info.insert(QStringLiteral("comparedSymbols"), static_cast<qint64>(res.comparedSymbols));
        info.insert(QStringLiteral("symbolErrors"), static_cast<qint64>(res.symbolErrors));
        info.insert(QStringLiteral("ser"), res.ser);
        info.insert(QStringLiteral("theorySer"), res.theorySer);
        info.insert(QStringLiteral("measuredEbN0"), res.measuredEbN0Db);
        info.insert(QStringLiteral("evm"), res.evmPercent);
        info.insert(QStringLiteral("mer"), res.merDb);
        info.insert(QStringLiteral("berText"), toQString(analysis::rateText(res.ber)));
        info.insert(QStringLiteral("theoryText"), toQString(analysis::rateText(res.theoryBer)));
        info.insert(QStringLiteral("berLowerText"), toQString(analysis::rateText(lower)));
        info.insert(QStringLiteral("berUpperText"), toQString(analysis::rateText(upper)));
        info.insert(QStringLiteral("serText"), toQString(analysis::rateText(res.ser)));
        info.insert(QStringLiteral("theorySerText"), toQString(analysis::rateText(res.theorySer)));
        if (r.data == analysis::LinkData::Text)
        {
            info.insert(QStringLiteral("textSent"), toQString(res.sentText));
            info.insert(QStringLiteral("textReceived"), toQString(res.receivedText));
            info.insert(QStringLiteral("textErrors"), static_cast<qint64>(res.textErrors));
        }
    }
    m_resultInfo = info;
}

void ModulationController::setRunning(bool running)
{
    if (m_running == running) return;
    m_running = running;
    emit busyChanged();
}

QVariantMap ModulationController::sweep() const
{
    return jsonToVariant(sweepJson(m_sweep)).toMap();
}

void ModulationController::setSweep(const QVariantMap &changes)
{
    const analysis::BerSweep s = sweepFromJson(variantToJson(changes), m_sweep);
    std::string error;
    analysis::berSweepValues(s, &error);
    const QString text = toQString(error);
    if (sweepJson(s) == sweepJson(m_sweep) && text == m_sweepError) return;
    m_sweep = s;
    m_sweepError = text;
    emit sweepChanged();
}

QString ModulationController::sweepText() const
{
    return toQString(sweepJson(m_sweep).dump());
}

void ModulationController::restoreSweep(const QString &text)
{
    const auto json = model::Json::parse(toUtf8(text));
    if (!json || !json->isObject()) return;
    setSweep(jsonToVariant(*json).toMap());
}

QVariantList ModulationController::curvesInfo() const
{
    QVariantList list;
    for (const Curve &c : m_curves)
    {
        QVariantList points;
        for (const analysis::BerPoint &p : c.points) points.append(pointInfo(p));
        list.append(QVariantMap{{QStringLiteral("label"), toQString(c.label)},
                                {QStringLiteral("done"), c.done},
                                {QStringLiteral("points"), points}});
    }
    return list;
}

void ModulationController::startCurve()
{
    std::string error;
    const std::vector<double> values = analysis::berSweepValues(m_sweep, &error);
    if (values.empty())
    {
        setLastError(toQString(error));
        return;
    }
    analysis::LinkRecipe r = m_recipe;
    r.data = analysis::LinkData::Random;
    r.noise = true;
    // What the recipe has wrong, said at once (a link of a few symbols: as few as its preamble and its equalizer
    // allow).
    analysis::LinkRecipe tiny = r;
    tiny.symbols = analysis::minimumLinkSymbols(r);
    const analysis::LinkResult check = analysis::simulateLink(tiny);
    if (!check.ok())
    {
        setLastError(toQString(check.error));
        return;
    }
    stopCurve();
    if (static_cast<int>(m_curves.size()) >= kMaximumCurves) m_curves.erase(m_curves.begin());
    Curve curve;
    curve.recipe = r;
    curve.label = analysis::linkText(r);
    m_curves.push_back(std::move(curve));
    emit curvesChanged();

    auto run = std::make_shared<Run>();
    m_curveRun = run;
    m_curvePoints = values.size();
    m_curveRunning = true;
    m_curveProgress = 0.0;
    emit curveBusyChanged();
    emit curveProgressChanged();
    m_progressTimer.start();
    const analysis::BerSweep sweep = m_sweep;
    // Its own seed: two curves of the same recipe draw other bits and other noise.
    const auto seed = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    m_curvePool.start([this, run, r, sweep, values, seed] {
        for (std::size_t k = 0; k < values.size(); ++k)
        {
            if (run->cancelled.load()) break;
            run->permille.store(0);
            const model::Progress progress{&run->cancelled, &run->permille};
            const analysis::BerPoint point = analysis::measureBer(r, values[k], sweep, seed + 7919 * k, progress);
            const bool last = k + 1 == values.size() || point.cancelled;
            QMetaObject::invokeMethod(this, [this, run, k, point, last] { curvePoint(run, k, point, last); }, Qt::QueuedConnection);
            if (point.cancelled) break;
        }
    });
}

void ModulationController::curvePoint(const std::shared_ptr<Run> &run, std::size_t index, const analysis::BerPoint &point, bool last)
{
    if (run != m_curveRun || m_curves.empty()) return;
    Curve &curve = m_curves.back();
    if (!point.cancelled && curve.points.size() == index) curve.points.push_back(point);
    if (last)
    {
        curve.done = !point.cancelled;
        m_curveRun.reset();
        m_curveRunning = false;
        m_progressTimer.stop();
        m_curveProgress = curve.done ? 1.0 : m_curveProgress;
        emit curveBusyChanged();
        emit curveProgressChanged();
    }
    emit curvesChanged();
}

void ModulationController::stopCurve()
{
    if (!m_curveRun) return;
    m_curveRun->cancelled.store(true);
    m_curveRun.reset();
    m_curveRunning = false;
    m_progressTimer.stop();
    emit curveBusyChanged();
    emit curvesChanged();
}

void ModulationController::clearCurves()
{
    stopCurve();
    if (m_curves.empty()) return;
    m_curves.clear();
    m_curveProgress = 0.0;
    emit curveProgressChanged();
    emit curvesChanged();
}

void ModulationController::addToSession(bool received)
{
    Session *session = m_session.data();
    if (!session) return;
    const std::shared_ptr<const analysis::LinkResult> result = m_result;
    if (!result || !result->ok())
    {
        setLastError(result ? toQString(result->error) : tr("La liaison n'est pas encore calculée."));
        return;
    }
    model::Signal signal = analysis::linkSignal(*result, received);
    signal.name = toUtf8(session->signalList()->uniqueName(toQString(signal.name)));
    signal.createdUtc = toUtf8(nowUtc());
    const QString label = tr("Ajouter « %1 »").arg(toQString(signal.name));
    QString error;
    const qint64 id = session->signalList()->addSignal(std::move(signal), label, &error);
    if (id == 0)
        setLastError(error);
    else
        emit added(id);
}

void ModulationController::setLastError(const QString &message)
{
    m_lastError = message;
    emit lastErrorChanged();
}

void ModulationController::waitForIdle()
{
    for (int round = 0; round < 2000; ++round)
    {
        if (m_delay.isActive())
        {
            m_delay.stop();
            start();
        }
        m_pool.waitForDone();
        m_curvePool.waitForDone();
        QCoreApplication::processEvents(); // the results, sent back to this thread
        if (!m_running && !m_delay.isActive() && !m_again && !m_curveRunning) return;
        if (!m_active && !m_curveRunning) return;
    }
}
} // namespace qacq::app
