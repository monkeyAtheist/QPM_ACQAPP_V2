#include "qacq/app/curves_controller.h"

#include "qacq/app/analysis_controller.h"
#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/csv.h"
#include "qacq/plot/axis.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace qacq::app
{
namespace
{
constexpr std::size_t kToTheEnd = std::numeric_limits<std::size_t>::max();
const QString kModule = QStringLiteral("curves");

QString graphOf(analysis::CurveKind kind)
{
    return toQString(std::string(analysis::curveGraph(kind)));
}
} // namespace

CurvesController::CurvesController(Session *session, AnalysisController *analysis, QObject *parent)
    : QObject(parent), m_session(session), m_analysis(analysis)
{
    m_pool.setMaxThreadCount(1);
    m_spectrogramPool.setMaxThreadCount(1);
    m_progressTimer.setInterval(150);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        const std::shared_ptr<SpectrogramRun> run = m_spectrogramRun;
        if (!run) return;
        const double done = (static_cast<double>(run->index.load()) + 0.001 * static_cast<double>(run->permille.load())) /
                            static_cast<double>(std::max(1, run->total));
        if (std::abs(done - m_spectrogramProgressShown) < 0.005) return;
        m_spectrogramProgressShown = std::clamp(done, 0.0, 1.0);
        emit spectrogramProgressChanged();
    });
    if (analysis)
    {
        // Other segments or another window: the spectrograms again (the spectra are the analysis's own).
        connect(analysis, &AnalysisController::settingsChanged, this, [this] {
            if (settingsKey(spectrogramSettings()) != m_spectrogramSettingsKey) computeSpectrograms();
        });
    }
    if (session)
    {
        SignalListModel *list = session->signalList();
        const auto again = [this] {
            const model::SignalPtr signal = currentSignal();
            if (!m_set || m_set->signal != signal) compute();
            if (!m_spectrograms || m_spectrograms->signal != signal) computeSpectrograms();
        };
        connect(list, &QAbstractItemModel::dataChanged, this, again);
        connect(list, &QAbstractItemModel::rowsInserted, this, again);
        connect(list, &QAbstractItemModel::rowsRemoved, this, again);
        connect(list, &QAbstractItemModel::modelReset, this, again);
        connect(session, &Session::sessionReplaced, this, &CurvesController::load);
    }
    load();
}

CurvesController::~CurvesController()
{
    if (m_spectrogramRun) m_spectrogramRun->cancelled.store(true);
    m_spectrogramPool.waitForDone();
    m_pool.waitForDone();
}

void CurvesController::setSignalId(qint64 id)
{
    if (id == m_signalId) return;
    m_signalId = id;
    emit signalIdChanged();
    changed(false);
}

model::SignalPtr CurvesController::currentSignal() const
{
    // One read of the guarded pointer (GCC sees a possible null between two reads of a QPointer).
    Session *session = m_session.data();
    SignalListModel *list = session ? session->signalList() : nullptr;
    return list && m_signalId > 0 ? list->signalById(m_signalId) : model::SignalPtr();
}

bool CurvesController::hasTimeCurves() const
{
    return std::any_of(m_specs.begin(), m_specs.end(), [](const analysis::CurveSpec &c) { return analysis::isTimeCurve(c.kind); });
}

bool CurvesController::hasSpectra() const
{
    return std::any_of(m_specs.begin(), m_specs.end(), [](const analysis::CurveSpec &c) { return c.kind == analysis::CurveKind::Spectrum; });
}

bool CurvesController::hasSpectrograms() const
{
    return std::any_of(m_specs.begin(), m_specs.end(),
                       [](const analysis::CurveSpec &c) { return c.kind == analysis::CurveKind::Spectrogram; });
}

QString CurvesController::partText(const analysis::CurveSpec &spec, const model::Signal *signal) const
{
    if (!analysis::hasPart(spec.kind) || (spec.firstFrame == 0 && spec.frameCount == kToTheEnd)) return QString();
    if (!signal || !(signal->sampleRateHz > 0.0)) return tr("une partie");
    const auto [first, count] = analysis::curvePart(spec, *signal);
    const double t0 = signal->startSeconds + static_cast<double>(first) / signal->sampleRateHz;
    const double t1 = t0 + static_cast<double>(count) / signal->sampleRateHz;
    const int digits = plot::significantFor(std::max(std::abs(t0), std::abs(t1)), (t1 - t0) / 1000.0, 3, 9);
    return tr("%1 à %2").arg(formatQuantity(t0, QStringLiteral("s"), digits), formatQuantity(t1, QStringLiteral("s"), digits));
}

QVariantList CurvesController::curves() const
{
    QVariantList list;
    const model::SignalPtr signal = m_set ? m_set->signal : currentSignal();
    int timeIndex = 0;
    for (const analysis::CurveSpec &c : m_specs)
    {
        const bool time = analysis::isTimeCurve(c.kind);
        const bool missing = !signal || c.channel >= signal->channelCount();
        QString name = signal ? toQString(analysis::curveName(c, *signal)) : QString();
        if (name.isEmpty()) name = tr("Voie %1 (absente de ce signal)").arg(static_cast<int>(c.channel + 1));
        bool ready = !time || c.kind == analysis::CurveKind::Source;
        if (time && !ready && signal) ready = cached(c, *signal);
        if (c.kind == analysis::CurveKind::Spectrogram)
            ready = signal && spectrogramCached(c, *signal, m_spectrogramSettingsKey);
        list << QVariantMap{{QStringLiteral("id"), static_cast<int>(c.id)},
                            {QStringLiteral("kind"), toQString(std::string(analysis::curveKindKey(c.kind)))},
                            {QStringLiteral("kindName"), toQString(analysis::curveKindName(c.kind))},
                            {QStringLiteral("channel"), static_cast<int>(c.channel)},
                            {QStringLiteral("name"), name},
                            {QStringLiteral("unit"), signal ? toQString(analysis::curveUnit(c, *signal)) : QString()},
                            {QStringLiteral("graph"), graphOf(c.kind)},
                            {QStringLiteral("visible"), c.visible},
                            {QStringLiteral("ready"), ready},
                            {QStringLiteral("colorIndex"), time ? timeIndex : static_cast<int>(c.channel)},
                            {QStringLiteral("part"), partText(c, signal.get())},
                            {QStringLiteral("removeMean"), c.removeMean},
                            {QStringLiteral("missing"), missing}};
        if (time) ++timeIndex;
    }
    return list;
}

int CurvesController::add(const QString &kind, const QVariantList &channels, const QVariantMap &options)
{
    analysis::CurveKind k = analysis::CurveKind::Spectrum;
    if (!analysis::curveKindFromKey(toUtf8(kind), k)) return 0;
    const bool time = analysis::isTimeCurve(k);
    analysis::CurveSpec base;
    base.kind = k;
    base.removeMean = k == analysis::CurveKind::Integral && options.value(QStringLiteral("removeMean")).toBool();
    if (analysis::hasPart(k))
    {
        base.firstFrame = static_cast<std::size_t>(std::max<qint64>(0, options.value(QStringLiteral("firstFrame"), 0).toLongLong()));
        const qint64 count = options.value(QStringLiteral("frameCount"), -1).toLongLong();
        base.frameCount = count < 0 ? kToTheEnd : static_cast<std::size_t>(count);
    }
    const bool withSources = time && k != analysis::CurveKind::Source && options.value(QStringLiteral("withSources")).toBool();
    int done = 0;
    // A curve of the list at the same place is shown again; otherwise it is added at the end.
    const auto put = [this, &done](const analysis::CurveSpec &spec) {
        for (analysis::CurveSpec &existing : m_specs)
        {
            if (existing.samePlace(spec))
            {
                if (!existing.visible) ++done;
                existing.visible = true;
                return;
            }
        }
        analysis::CurveSpec added = spec;
        added.id = m_nextId++;
        m_specs.push_back(added);
        ++done;
    };
    for (const QVariant &v : channels)
    {
        bool ok = false;
        const int channel = v.toInt(&ok);
        if (!ok || channel < 0) continue;
        if (withSources)
        {
            analysis::CurveSpec source;
            source.kind = analysis::CurveKind::Source;
            source.channel = static_cast<std::size_t>(channel);
            put(source);
        }
        analysis::CurveSpec spec = base;
        spec.channel = static_cast<std::size_t>(channel);
        put(spec);
    }
    if (done > 0) changed(true);
    return done;
}

void CurvesController::remove(int id)
{
    const auto it = std::find_if(m_specs.begin(), m_specs.end(),
                                 [id](const analysis::CurveSpec &c) { return c.id == static_cast<std::uint64_t>(id); });
    if (it == m_specs.end()) return;
    m_cache.erase(it->id);
    m_spectrogramCache.erase(it->id);
    m_specs.erase(it);
    changed(true);
}

void CurvesController::setVisible(int id, bool visible)
{
    for (analysis::CurveSpec &c : m_specs)
    {
        if (c.id != static_cast<std::uint64_t>(id) || c.visible == visible) continue;
        c.visible = visible;
        changed(true);
        return;
    }
}

void CurvesController::clear(const QString &graph)
{
    const std::size_t before = m_specs.size();
    std::erase_if(m_specs, [&graph](const analysis::CurveSpec &c) { return graph.isEmpty() || graphOf(c.kind) == graph; });
    if (m_specs.size() == before) return;
    std::erase_if(m_cache, [this](const auto &entry) {
        return std::none_of(m_specs.begin(), m_specs.end(), [&entry](const analysis::CurveSpec &c) { return c.id == entry.first; });
    });
    std::erase_if(m_spectrogramCache, [this](const auto &entry) {
        return std::none_of(m_specs.begin(), m_specs.end(), [&entry](const analysis::CurveSpec &c) { return c.id == entry.first; });
    });
    changed(true);
}

qint64 CurvesController::keepAsSignal()
{
    Session *session = m_session.data();
    if (!session || !m_set) return 0;
    SignalListModel *list = session->signalList();
    analysis::Derived derived = analysis::signalFromCurves(*m_set);
    if (!derived.ok())
    {
        emit list->errorOccurred(toQString(derived.error));
        return 0;
    }
    model::Signal &signal = derived.signal;
    signal.name = toUtf8(list->uniqueName(toQString(signal.name)));
    signal.createdUtc = toUtf8(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return list->addSignal(std::move(signal), tr("Garder les courbes comme signal"));
}

bool CurvesController::exportTimeCurves(const QUrl &url)
{
    Session *session = m_session.data();
    if (!session) return false;
    SignalListModel *list = session->signalList();
    const auto fail = [list](const QString &message) {
        emit list->errorOccurred(message);
        return false;
    };
    if (!m_set) return fail(tr("Pas de courbe du temps à exporter."));
    const QString local = url.toLocalFile();
    if (local.isEmpty()) return fail(tr("Seuls les fichiers locaux peuvent être écrits."));
    const analysis::Derived derived = analysis::signalFromCurves(*m_set);
    if (!derived.ok()) return fail(toQString(derived.error));
    std::ofstream out(toPath(local), std::ios::binary | std::ios::trunc);
    if (!out) return fail(tr("Impossible de créer « %1 ».").arg(local));
    out << "\xEF\xBB\xBF"; // UTF-8 mark: a spreadsheet reads "∫" and the accents of the names
    model::CsvExportOptions csv;
    csv.decimalComma = true;
    const std::string error = model::writeCsv(out, derived.signal, csv);
    if (!error.empty()) return fail(toQString(error));
    out.close();
    if (!out) return fail(tr("L'écriture de « %1 » n'a pas abouti (disque plein ?).").arg(local));
    return true;
}

void CurvesController::changed(bool saveList)
{
    if (saveList && !m_loading) save();
    emit curvesChanged();
    handOverSpectra();
    compute();
    computeSpectrograms();
}

void CurvesController::handOverSpectra()
{
    AnalysisController *analysis = m_analysis.data();
    if (!analysis) return;
    std::vector<std::size_t> channels;
    for (const analysis::CurveSpec &c : m_specs)
    {
        if (c.kind == analysis::CurveKind::Spectrum && c.visible &&
            std::find(channels.begin(), channels.end(), c.channel) == channels.end())
            channels.push_back(c.channel);
    }
    std::sort(channels.begin(), channels.end());
    analysis->setSpectrumChannels(channels);
}

void CurvesController::save()
{
    Session *session = m_session.data();
    if (!session) return;
    session->setModuleSettings(kModule, {{QStringLiteral("list"), jsonToVariant(analysis::toJson(m_specs))}});
}

void CurvesController::load()
{
    Session *session = m_session.data();
    m_loading = true;
    m_specs = session ? analysis::curveSpecsFromJson(variantToJson(session->moduleSettings(kModule).value(QStringLiteral("list"))))
                      : std::vector<analysis::CurveSpec>();
    m_cache.clear();
    m_spectrogramCache.clear();
    m_nextId = 1;
    for (const analysis::CurveSpec &c : m_specs) m_nextId = std::max(m_nextId, c.id + 1);
    changed(false);
    m_loading = false;
}

bool CurvesController::cached(const analysis::CurveSpec &spec, const model::Signal &signal) const
{
    const auto it = m_cache.find(spec.id);
    return it != m_cache.end() && it->second.data == signal.data.get() && it->second.rateHz == signal.sampleRateHz &&
           it->second.spec.samePlace(spec) && it->second.values;
}

void CurvesController::compute()
{
    if (m_running)
    {
        m_again = true;
        return;
    }
    m_again = false;
    const model::SignalPtr signal = currentSignal();
    ++m_generation;
    if (!signal)
    {
        if (m_set)
        {
            m_set.reset();
            emit timeCurvesChanged();
            emit curvesChanged();
        }
        return;
    }
    std::vector<analysis::CurveSpec> wanted;
    for (const analysis::CurveSpec &c : m_specs)
    {
        if (analysis::isTimeCurve(c.kind) && c.kind != analysis::CurveKind::Source && c.channel < signal->channelCount() &&
            !cached(c, *signal))
            wanted.push_back(c);
    }
    if (wanted.empty())
    {
        rebuild();
        return;
    }
    rebuild(); // what is ready shows at once (the sources, the curves already computed)
    m_running = true;
    emit busyChanged();
    const quint64 generation = m_generation;
    m_pool.start([this, signal, wanted, generation] {
        Results results;
        for (const analysis::CurveSpec &c : wanted)
            results[c.id] = std::make_shared<const std::vector<double>>(analysis::computeTimeCurve(*signal, c));
        // The destructor waits for this task: `this` is still alive here, and the queued call is dropped if
        // it is destroyed before the call runs.
        QMetaObject::invokeMethod(
            this, [this, generation, signal, results] { finished(generation, signal, results); }, Qt::QueuedConnection);
    });
}

void CurvesController::finished(quint64 generation, model::SignalPtr signal, Results results)
{
    m_running = false;
    emit busyChanged();
    if (generation == m_generation && signal == currentSignal())
    {
        for (auto &entry : results)
        {
            const std::uint64_t id = entry.first;
            const auto spec = std::find_if(m_specs.begin(), m_specs.end(), [id](const analysis::CurveSpec &c) { return c.id == id; });
            if (spec == m_specs.end()) continue;
            m_cache[id] = Cached{signal->data.get(), signal->sampleRateHz, *spec, std::move(entry.second)};
        }
    }
    if (m_again || generation != m_generation)
    {
        compute();
        return;
    }
    rebuild();
}

void CurvesController::rebuild()
{
    const model::SignalPtr signal = currentSignal();
    auto set = std::make_shared<analysis::TimeCurveSet>();
    set->signal = signal;
    if (signal)
    {
        for (const analysis::CurveSpec &c : m_specs)
        {
            if (!analysis::isTimeCurve(c.kind)) continue;
            analysis::TimeCurve curve;
            curve.id = c.id;
            curve.channel = c.channel;
            curve.visible = c.visible;
            curve.name = analysis::curveName(c, *signal);
            curve.unit = analysis::curveUnit(c, *signal);
            if (c.channel < signal->channelCount() && signal->data && signal->data->channelCount() == signal->channelCount())
            {
                const auto [first, count] = analysis::curvePart(c, *signal);
                curve.firstFrame = first;
                if (c.kind == analysis::CurveKind::Source)
                {
                    curve.values = signal->channel(c.channel).subspan(first, count);
                }
                else if (cached(c, *signal))
                {
                    curve.computed = m_cache.at(c.id).values;
                    curve.values = *curve.computed;
                }
            }
            set->curves.push_back(std::move(curve));
        }
    }
    m_set = std::move(set);
    emit timeCurvesChanged();
    emit curvesChanged();
}

void CurvesController::waitForIdle()
{
    while (m_running || m_pool.activeThreadCount() > 0 || m_spectrogramRunning || m_spectrogramPool.activeThreadCount() > 0)
    {
        m_pool.waitForDone();
        m_spectrogramPool.waitForDone();
        QCoreApplication::processEvents();
    }
}

analysis::SpectrogramSettings CurvesController::spectrogramSettings() const
{
    analysis::SpectrogramSettings settings;
    const AnalysisController *controller = m_analysis.data(); // one read of the guarded pointer
    if (controller)
    {
        const analysis::AnalysisSettings &a = controller->analysisSettings();
        settings.window = a.window;
        settings.kaiserBeta = a.kaiserBeta;
        settings.segmentLength = a.spectrogramSegment;
        settings.overlap = a.spectrogramOverlap;
        settings.removeMean = a.removeMean; // as for the spectra: one setting
    }
    return settings;
}

std::string CurvesController::settingsKey(const analysis::SpectrogramSettings &settings)
{
    return std::to_string(static_cast<int>(settings.window)) + '|' + std::to_string(settings.kaiserBeta) + '|' +
           std::to_string(settings.segmentLength) + '|' + std::to_string(settings.overlap) + '|' +
           std::to_string(settings.maximumFrames) + (settings.removeMean ? "|m" : "|");
}

bool CurvesController::spectrogramCached(const analysis::CurveSpec &spec, const model::Signal &signal, const std::string &settings) const
{
    // An empty `settings`: whatever the settings (what was computed before shows while it is computed again).
    const auto it = m_spectrogramCache.find(spec.id);
    return it != m_spectrogramCache.end() && it->second.data == signal.data.get() &&
           it->second.rateHz == signal.sampleRateHz && it->second.spec.samePlace(spec) && it->second.result &&
           (settings.empty() || it->second.settings == settings);
}

void CurvesController::computeSpectrograms()
{
    const analysis::SpectrogramSettings settings = spectrogramSettings();
    const std::string key = settingsKey(settings);
    m_spectrogramSettingsKey = key;
    if (m_spectrogramRunning)
    {
        // What runs is no longer what is wanted: it stops early, then this starts again.
        m_spectrogramAgain = true;
        if (m_spectrogramRun) m_spectrogramRun->cancelled.store(true);
        rebuildSpectrograms();
        return;
    }
    m_spectrogramAgain = false;
    ++m_spectrogramGeneration;
    const model::SignalPtr signal = currentSignal();
    std::vector<analysis::CurveSpec> wanted;
    if (signal)
    {
        for (const analysis::CurveSpec &c : m_specs)
        {
            if (c.kind == analysis::CurveKind::Spectrogram && c.visible && c.channel < signal->channelCount() &&
                !spectrogramCached(c, *signal, key))
                wanted.push_back(c);
        }
    }
    rebuildSpectrograms(); // what is ready shows at once
    if (wanted.empty()) return;
    auto run = std::make_shared<SpectrogramRun>();
    run->total = static_cast<int>(wanted.size());
    m_spectrogramRun = run;
    m_spectrogramRunning = true;
    m_spectrogramProgressShown = 0.0;
    emit spectrogramProgressChanged();
    emit spectrogramBusyChanged();
    m_progressTimer.start();
    const quint64 generation = m_spectrogramGeneration;
    m_spectrogramPool.start([this, signal, wanted, settings, key, generation, run] {
        SpectrogramResults results;
        for (std::size_t i = 0; i < wanted.size(); ++i)
        {
            run->index.store(static_cast<int>(i));
            run->permille.store(0);
            auto result = std::make_shared<const dsp::Spectrogram>(
                analysis::computeSpectrogram(*signal, wanted[i], settings, dsp::Progress{&run->cancelled, &run->permille}));
            if (run->cancelled.load()) break; // stopped: what it gave is not a whole spectrogram
            results[wanted[i].id] = std::move(result);
        }
        // The destructor waits for this task: `this` is still alive here, and the queued call is dropped if
        // it is destroyed before the call runs.
        QMetaObject::invokeMethod(
            this, [this, generation, signal, key, results] { spectrogramsFinished(generation, signal, key, results); },
            Qt::QueuedConnection);
    });
}

void CurvesController::spectrogramsFinished(quint64 generation, model::SignalPtr signal, std::string settings,
                                            SpectrogramResults results)
{
    m_spectrogramRunning = false;
    m_progressTimer.stop();
    if (generation == m_spectrogramGeneration && signal == currentSignal())
    {
        for (auto &entry : results)
        {
            const std::uint64_t id = entry.first;
            const auto spec = std::find_if(m_specs.begin(), m_specs.end(), [id](const analysis::CurveSpec &c) { return c.id == id; });
            if (spec == m_specs.end()) continue;
            m_spectrogramCache[id] = SpectrogramCached{signal->data.get(), signal->sampleRateHz, *spec, settings, std::move(entry.second)};
        }
    }
    if (m_spectrogramAgain || generation != m_spectrogramGeneration)
    {
        computeSpectrograms();
        if (!m_spectrogramRunning) emit spectrogramBusyChanged();
        return;
    }
    emit spectrogramBusyChanged();
    rebuildSpectrograms();
    emit curvesChanged(); // "ready" of the chips
}

void CurvesController::rebuildSpectrograms()
{
    const model::SignalPtr signal = currentSignal();
    auto set = std::make_shared<analysis::SpectrogramSet>();
    set->signal = signal;
    if (signal && signal->sampleRateHz > 0.0)
    {
        for (const analysis::CurveSpec &c : m_specs)
        {
            if (c.kind != analysis::CurveKind::Spectrogram) continue;
            analysis::SpectrogramCurve curve;
            curve.id = c.id;
            curve.channel = c.channel;
            curve.visible = c.visible;
            curve.name = analysis::curveName(c, *signal);
            curve.unit = analysis::curveUnit(c, *signal);
            curve.startSeconds = signal->startSeconds + static_cast<double>(analysis::curvePart(c, *signal).first) / signal->sampleRateHz;
            // Computed with these settings, or with the ones before while it is computed again.
            if (spectrogramCached(c, *signal, std::string())) curve.result = m_spectrogramCache.at(c.id).result;
            set->curves.push_back(std::move(curve));
        }
    }
    m_spectrograms = std::move(set);
    emit spectrogramsChanged();
}
} // namespace qacq::app
