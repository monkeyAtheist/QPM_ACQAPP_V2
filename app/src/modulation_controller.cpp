#include "qacq/app/modulation_controller.h"

#include "qacq/app/qt_convert.h"
#include "qacq/app/signal_list_model.h"

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
            {QStringLiteral("upper"), p.upper},     {QStringLiteral("theory"), p.theory}};
}

// Before and after the channel, for the preview: on a carrier the real signals (V); as I/Q the two pairs.
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
    s.data = std::move(data);
    return s;
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
    schedule();
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
    return !m_running && !m_delay.isActive() && m_resultVersion == m_version;
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
    replace(analysis::linkRecipeFromJson(merged(analysis::toJson(m_recipe), json)));
}

void ModulationController::reset()
{
    replace(analysis::defaultLinkRecipe());
}

void ModulationController::useExample(int index)
{
    const auto &list = analysis::linkExamples();
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) return;
    replace(list[static_cast<std::size_t>(index)].recipe);
}

QString ModulationController::recipeText() const
{
    return toQString(analysis::toJson(m_recipe).dump());
}

void ModulationController::restoreRecipe(const QString &text)
{
    const auto json = model::Json::parse(toUtf8(text));
    if (!json || !json->isObject()) return;
    replace(analysis::linkRecipeFromJson(*json));
}

void ModulationController::replace(analysis::LinkRecipe recipe)
{
    if (analysis::toJson(recipe) == analysis::toJson(m_recipe)) return;
    m_recipe = std::move(recipe);
    ++m_version;
    emit recipeChanged();
    schedule();
}

void ModulationController::schedule()
{
    m_delay.start();
    emit stateChanged();
}

void ModulationController::start()
{
    if (!m_active) return;
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
        double lower = 0.0, upper = 0.0;
        analysis::binomialInterval(res.bitErrors, res.comparedBits, lower, upper);
        info.insert(QStringLiteral("constellationSize"), static_cast<int>(res.constellation.size()));
        info.insert(QStringLiteral("bitRate"), res.bitRate);
        info.insert(QStringLiteral("bandwidth"), res.occupiedBandwidthHz);
        info.insert(QStringLiteral("samplesPerSymbol"), res.samplesPerSymbol);
        info.insert(QStringLiteral("symbols"), static_cast<qint64>(res.sentSymbols.size()));
        info.insert(QStringLiteral("skipSymbols"), static_cast<qint64>(res.skipSymbols));
        info.insert(QStringLiteral("comparedBits"), static_cast<qint64>(res.comparedBits));
        info.insert(QStringLiteral("bitErrors"), static_cast<qint64>(res.bitErrors));
        info.insert(QStringLiteral("ber"), res.ber);
        info.insert(QStringLiteral("berLower"), lower);
        info.insert(QStringLiteral("berUpper"), upper);
        info.insert(QStringLiteral("theoryBer"), res.theoryBer);
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
    // What the recipe has wrong, said at once (a link of a few symbols).
    analysis::LinkRecipe tiny = r;
    tiny.symbols = analysis::kMinimumLinkSymbols;
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
