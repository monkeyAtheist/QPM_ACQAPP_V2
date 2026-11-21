#include "qacq/app/analysis_controller.h"
#include "qacq/analysis/calculus.h"
#include "qacq/analysis/measures.h"

#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/dsp/window.h"
#include "qacq/plot/axis.h"

#include <QAbstractItemModel>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace qacq::app
{
namespace
{
QString text(const std::string &utf8)
{
    return toQString(utf8);
}

QString quantity(double value, const std::string &unit, int digits = 4)
{
    return text(plot::formatQuantity(value, unit, digits));
}

// Beyond ±200 dB a ratio says only "numerically exact" (a synthetic signal): it is written so.
QString decibels(double value, int decimals = 1)
{
    if (value > 200.0) return QStringLiteral("> 200 dB");
    if (value < -200.0) return QStringLiteral("< −200 dB");
    return text(plot::formatFixed(value, decimals)) + QStringLiteral(" dB");
}

QString windowText(const analysis::AnalysisSettings &s, bool capital)
{
    QString name = text(std::string(dsp::windowName(s.window)));
    if (s.window == dsp::Window::FlatTop) name = QStringLiteral("à sommet plat");
    if (s.window == dsp::Window::Kaiser)
        name += QStringLiteral(" (β = %1)").arg(text(plot::formatCompact(s.kaiserBeta, "", 3)));
    return capital ? QObject::tr("Fenêtre %1").arg(name) : QObject::tr("fenêtre %1").arg(name);
}

// A value that is a rounding residue of the computation next to `scale` (the mean of a centred
// signal: 3×10⁻¹⁷ A next to 0,17 A) reads 0.
double cleaned(double value, double scale)
{
    return std::abs(value) <= 1e-12 * std::abs(scale) ? 0.0 : value;
}

QVariantMap row(const QString &label, const QString &value)
{
    return {{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}
} // namespace

AnalysisController::AnalysisController(Session *session, QObject *parent) : QObject(parent), m_session(session)
{
    m_pool.setMaxThreadCount(1);
    m_delay.setSingleShot(true);
    connect(&m_delay, &QTimer::timeout, this, &AnalysisController::start);
    if (session)
    {
        SignalListModel *list = session->signalList();
        connect(list, &QAbstractItemModel::dataChanged, this, &AnalysisController::onSessionChanged);
        connect(list, &QAbstractItemModel::rowsInserted, this, &AnalysisController::onSessionChanged);
        connect(list, &QAbstractItemModel::rowsRemoved, this, &AnalysisController::onSessionChanged);
        connect(list, &QAbstractItemModel::modelReset, this, &AnalysisController::onSessionChanged);
    }
}

AnalysisController::~AnalysisController()
{
    if (m_cancel) m_cancel->store(true);
    m_pool.waitForDone();
}

void AnalysisController::setSignalId(qint64 id)
{
    if (id == m_signalId) return;
    m_signalId = id;
    // The view tells the zone of the new signal within the delay; until then, all of it.
    m_firstFrame = 0;
    m_frameCount = -1;
    emit signalIdChanged();
    schedule(150);
}

void AnalysisController::setZone(qint64 firstFrame, qint64 frameCount)
{
    firstFrame = std::max<qint64>(0, firstFrame);
    if (firstFrame == m_firstFrame && frameCount == m_frameCount) return;
    m_firstFrame = firstFrame;
    m_frameCount = frameCount;
    schedule(150);
}

QVariantMap AnalysisController::settings() const
{
    return jsonToVariant(analysis::toJson(m_settings)).toMap();
}

void AnalysisController::setSettings(const QVariantMap &values)
{
    model::Json json = analysis::toJson(m_settings);
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) json[toUtf8(it.key())] = variantToJson(it.value());
    const analysis::AnalysisSettings next = analysis::analysisSettingsFromJson(json);
    const analysis::AnalysisSettings previous = m_settings;
    if (analysis::toJson(next) == analysis::toJson(previous)) return;
    m_settings = next;
    emit settingsChanged();

    // Only the way it is drawn: no new computation.
    analysis::AnalysisSettings computed = next;
    computed.scale = previous.scale;
    computed.logFrequency = previous.logFrequency;
    if (analysis::toJson(computed) == analysis::toJson(previous))
    {
        if (next.scale != previous.scale && m_result && !m_running)
        {
            auto copy = std::make_shared<analysis::AnalysisResult>(*m_result);
            analysis::setScale(*copy, next.scale);
            m_result = std::move(copy);
            emit resultChanged();
        }
        else if (next.scale != previous.scale)
        {
            schedule(0);
        }
        return;
    }
    schedule(0);
}

void AnalysisController::setSpectrumChannels(const std::vector<std::size_t> &channels)
{
    if (!m_allSpectra && channels == m_spectrumChannels) return;
    m_allSpectra = false;
    m_spectrumChannels = channels;
    schedule(0);
}

QString AnalysisController::settingsText() const
{
    return toQString(analysis::toJson(m_settings).dump());
}

void AnalysisController::restoreSettings(const QString &text)
{
    const std::optional<model::Json> json = model::Json::parse(toUtf8(text));
    if (!json) return;
    m_settings = analysis::analysisSettingsFromJson(*json);
    emit settingsChanged();
    schedule(0);
}

void AnalysisController::schedule(int delayMs)
{
    m_delay.start(delayMs);
}

void AnalysisController::start()
{
    if (m_running)
    {
        // Once the run in progress stops (it is told to stop early).
        m_again = true;
        if (m_cancel) m_cancel->store(true);
        return;
    }
    m_again = false;
    const model::SignalPtr signal = currentSignal();
    m_analysed = signal;
    ++m_generation;
    if (!signal)
    {
        if (m_result)
        {
            m_result.reset();
            m_resultSignal.reset();
            emit resultChanged();
        }
        return;
    }
    const auto frames = static_cast<qint64>(signal->frameCount());
    const qint64 first = std::clamp<qint64>(m_firstFrame, 0, frames);
    const qint64 count = m_frameCount < 0 ? frames - first : std::min(m_frameCount, frames - first);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = true;
    emit busyChanged();

    const quint64 generation = m_generation;
    const std::shared_ptr<std::atomic<bool>> cancel = m_cancel;
    const analysis::AnalysisSettings settings = m_settings;
    const bool all = m_allSpectra;
    const std::vector<std::size_t> channels = m_spectrumChannels;
    m_pool.start([this, signal, first, count, settings, generation, cancel, all, channels] {
        auto result = std::make_shared<const analysis::AnalysisResult>(
            analysis::analyse(*signal, static_cast<std::size_t>(first), static_cast<std::size_t>(count), settings,
                              cancel.get(), all ? nullptr : &channels));
        // The destructor waits for this task: `this` is still alive here, and the queued call is dropped
        // if it is destroyed before the call runs.
        QMetaObject::invokeMethod(
            this, [this, generation, result, signal] { finished(generation, result, signal); }, Qt::QueuedConnection);
    });
}

void AnalysisController::finished(quint64 generation, std::shared_ptr<const analysis::AnalysisResult> result,
                                  model::SignalPtr signal)
{
    m_running = false;
    if (m_again || generation != m_generation)
    {
        start();
        if (!m_running) emit busyChanged();
        return;
    }
    emit busyChanged();
    if (result && result->error == "annulé") return;
    m_result = std::move(result);
    m_resultSignal = std::move(signal);
    emit resultChanged();
}

void AnalysisController::onSessionChanged()
{
    if (currentSignal() != m_analysed) schedule(0);
}

model::SignalPtr AnalysisController::currentSignal() const
{
    // One read of the guarded pointer (GCC sees a possible null between two reads of a QPointer).
    Session *session = m_session.data();
    SignalListModel *list = session ? session->signalList() : nullptr;
    return list && m_signalId > 0 ? list->signalById(m_signalId) : model::SignalPtr();
}

void AnalysisController::waitForIdle()
{
    m_pool.waitForDone();
}

bool AnalysisController::hasResult() const
{
    return m_result && m_result->ok();
}

QString AnalysisController::error() const
{
    if (!m_result) return QString();
    return text(m_result->error);
}

QString AnalysisController::summary() const
{
    if (!m_result || !m_result->ok()) return QString();
    const analysis::AnalysisResult &r = *m_result;
    const QString df = quantity(r.binWidthHz, "Hz", 3);
    if (r.kind == analysis::SpectrumKind::Density)
    {
        const QString segments = r.segments == 1 ? tr("1 segment") : tr("%1 segments").arg(formatCount(static_cast<qint64>(r.segments)));
        return tr("Welch, %1 · %2 de %3 éch. · Δf = %4")
            .arg(windowText(m_settings, false), segments, formatCount(static_cast<qint64>(r.fftSize)), df);
    }
    return tr("%1 · FFT de %2 points · Δf = %3 · bande de bruit %4")
        .arg(windowText(m_settings, true), formatCount(static_cast<qint64>(r.fftSize)), df,
             quantity(r.enbwHz, "Hz", 3));
}

QString AnalysisController::zoneText() const
{
    if (!m_result || !m_resultSignal || !(m_resultSignal->sampleRateHz > 0.0)) return QString();
    const double fs = m_resultSignal->sampleRateHz;
    const double t0 = m_resultSignal->startSeconds + static_cast<double>(m_result->zoneFirstFrame) / fs;
    const double span = static_cast<double>(m_result->zoneFrames) / fs;
    const int digits = plot::significantFor(std::max(std::abs(t0), span), span / 1000.0, 3, 9);
    return tr("%1 à %2 · %3 éch.")
        .arg(quantity(t0, "s", digits), quantity(t0 + span, "s", digits),
             formatCount(static_cast<qint64>(m_result->zoneFrames)));
}

QStringList AnalysisController::notes() const
{
    QStringList list;
    if (!m_result) return list;
    for (const std::string &note : m_result->notes) list << text(note);
    return list;
}

QVariantList AnalysisController::measures() const
{
    QVariantList list;
    if (!m_result) return list;
    const bool iq = m_result->twoSided;
    int index = 0;
    for (const analysis::ChannelMeasures &m : m_result->measures)
    {
        QVariantMap channel;
        channel[QStringLiteral("name")] = text(m.name);
        channel[QStringLiteral("unit")] = text(m.unit);
        channel[QStringLiteral("channel")] = index++;
        QVariantList values;
        if (m.samples == 0)
        {
            values << row(tr("Échantillons"), tr("aucun nombre dans la zone"));
        }
        else
        {
            const double scale = std::max(std::abs(m.minimum), std::abs(m.maximum));
            values << row(tr("Moyenne"), quantity(cleaned(m.mean, scale), m.unit))
                   << row(tr("Efficace (RMS)"), quantity(m.rms, m.unit))
                   << row(tr("Efficace sans la moyenne"), quantity(cleaned(m.acRms, scale), m.unit))
                   << row(tr("Minimum"), quantity(cleaned(m.minimum, scale), m.unit))
                   << row(tr("Maximum"), quantity(cleaned(m.maximum, scale), m.unit))
                   << row(tr("Crête à crête"), quantity(m.peakToPeak, m.unit))
                   << row(tr("Facteur de crête"), text(plot::formatFixed(m.crestFactor, 2)))
                   << row(tr("Première / dernière valeur"),
                          quantity(cleaned(m.first, scale), m.unit) + QStringLiteral(" / ") + quantity(cleaned(m.last, scale), m.unit));
            const double duration = m_result->sampleRateHz > 0.0 ? static_cast<double>(m_result->zoneFrames) / m_result->sampleRateHz : 0.0;
            values << row(tr("Intégrale (aire sous la courbe)"),
                          quantity(cleaned(m.integral, scale * duration), analysis::operationUnit(analysis::Operation::Integral, m.unit)));
        }
        channel[QStringLiteral("values")] = values;

        const dsp::ToneAnalysis &t = m.tone;
        channel[QStringLiteral("tone")] = t.detected;
        QString toneText;
        if (iq) toneText = tr("Pas de mesure de tonalité sur une paire I/Q.");
        else if (m.samples < m_result->zoneFrames) toneText = tr("Pas de mesure de tonalité : la zone a des échantillons manquants.");
        else if (!t.detected) toneText = tr("Pas de sinusoïde nettement au-dessus du bruit (moins de 15 dB).");
        channel[QStringLiteral("toneText")] = toneText;
        QVariantList toneValues;
        QVariantList harmonics;
        if (t.detected)
        {
            toneValues << row(tr("Fréquence"), quantity(t.fundamentalHz, "Hz", 7))
                       << row(tr("Amplitude (crête)"), quantity(t.fundamentalAmplitude, m.unit))
                       << row(tr("Distorsion (THD)"),
                              (t.thdDb < -200.0 ? QStringLiteral("≈ 0 %") : text(plot::formatQuantity(t.thdPercent, "%", 3))) +
                                  QStringLiteral(" · ") + decibels(t.thdDb))
                       << row(tr("SINAD"), decibels(t.sinadDb)) << row(tr("Rapport signal/bruit"), decibels(t.snrDb))
                       << row(tr("Dynamique sans parasite (SFDR)"), decibels(t.sfdrDb))
                       << row(tr("Bits effectifs (ENOB)"),
                              t.sinadDb > 200.0 ? QStringLiteral("> 33") : text(plot::formatFixed(t.enob, 2)));
            for (std::size_t k = 0; k < t.harmonics.size(); ++k)
            {
                const dsp::Harmonic &h = t.harmonics[k];
                // A numerical zero (pure synthetic signal) is not a measurement worth digits.
                const bool zero = h.order != 1 && h.relativeDb <= -200.0;
                harmonics << QVariantMap{
                    {QStringLiteral("order"), QStringLiteral("H%1").arg(h.order)},
                    {QStringLiteral("frequency"), quantity(h.frequencyHz, "Hz", 5)},
                    {QStringLiteral("amplitude"), zero ? QStringLiteral("≈ 0") : quantity(h.amplitude, m.unit)},
                    {QStringLiteral("relative"), h.order == 1 ? tr("réf.")
                                                 : zero    ? QStringLiteral("< −200 dB")
                                                           : decibels(h.relativeDb)},
                    // Above the noise of the spectrum: marked on the plot, written plainly in the table.
                    {QStringLiteral("clear"), k < m.harmonicStandsOut.size() && m.harmonicStandsOut[k]}};
            }
            // What the harmonics are compared with (amplitude spectrum).
            if (m.spectrumFloor > 0.0)
                toneValues << row(tr("Plancher du spectre (médiane)"), quantity(m.spectrumFloor, m.unit));
        }
        channel[QStringLiteral("toneValues")] = toneValues;
        channel[QStringLiteral("harmonics")] = harmonics;

        // What an oscilloscope measures on its own (real channels).
        const analysis::WaveMeasures &w = m.wave;
        QVariantList wave;
        QString waveText;
        if (iq) waveText = QString();
        else if (!w.levels) waveText = tr("Signal plat dans la zone (ou échantillons manquants).");
        else
        {
            wave << row(tr("Haut / bas"), quantity(w.top, m.unit) + QStringLiteral(" / ") + quantity(w.base, m.unit))
                 << row(tr("Amplitude (haut − bas)"), quantity(w.amplitude, m.unit));
            if (w.periodic)
                wave << row(tr("Période"), quantity(w.periodSeconds, "s", 5))
                     << row(tr("Fréquence (fronts)"), quantity(w.frequencyHz, "Hz", 5));
            if (w.duty) wave << row(tr("Rapport cyclique"), text(plot::formatFixed(w.dutyCyclePercent, 1)) + QStringLiteral(" %"));
            const int low = static_cast<int>(std::lround(100.0 * w.edgeLowFraction));
            if (w.rise) wave << row(tr("Montée (%1 → %2 %)").arg(low).arg(100 - low), quantity(w.riseSeconds, "s"));
            if (w.fall) wave << row(tr("Descente (%1 → %2 %)").arg(100 - low).arg(low), quantity(w.fallSeconds, "s"));
            if (w.cycles)
            {
                const QString periods = w.cycleCount == 1 ? tr("1 période entière")
                                                          : tr("%1 périodes entières").arg(formatCount(static_cast<qint64>(w.cycleCount)));
                const double scale = std::max(std::abs(w.top), std::abs(w.base));
                wave << row(tr("Moyenne sur %1").arg(periods), quantity(cleaned(w.cycleMean, scale), m.unit))
                     << row(tr("Efficace sur %1").arg(periods), quantity(w.cycleRms, m.unit));
            }
            if (w.settlingMeasured)
            {
                const QString edge = w.lastEdgeRising ? tr("après le dernier front montant") : tr("après le dernier front descendant");
                const auto settling = [](const analysis::Settling &st) {
                    return st.reached ? quantity(st.seconds, "s") : tr("pas atteint dans la zone");
                };
                wave << row(tr("Établissement à ±1 % (%1)").arg(edge), settling(w.settling1))
                     << row(tr("Établissement à ±0,1 %"), settling(w.settling01));
            }
            wave << row(tr("Dépassement haut / bas"), text(plot::formatFixed(w.overshootPercent, 1)) + QStringLiteral(" % / ") +
                                                          text(plot::formatFixed(w.undershootPercent, 1)) + QStringLiteral(" %"))
                 << row(tr("Fronts montants / descendants"), QStringLiteral("%1 / %2")
                                                                 .arg(formatCount(static_cast<qint64>(w.risingEdges)))
                                                                 .arg(formatCount(static_cast<qint64>(w.fallingEdges))));
            if (!w.periodic) waveText = tr("Moins de deux fronts montants dans la zone : pas de période.");
        }
        channel[QStringLiteral("wave")] = wave;
        channel[QStringLiteral("waveText")] = waveText;
        list << channel;
    }
    return list;
}

QString AnalysisController::bandText(const analysis::HalfPowerBand &b, double binWidthHz) const
{
    if (!b.found) return QString();
    const auto frequency = [binWidthHz](double hz) {
        return quantity(hz, "Hz", plot::significantFor(hz, binWidthHz * 0.1, 3, 10));
    };
    if (b.lowOpen) return tr("Passe-bas : à −3 dB du maximum dès %1").arg(frequency(b.highHz));
    if (b.highOpen) return tr("Passe-haut : à −3 dB du maximum sous %1").arg(frequency(b.lowHz));
    return tr("Bande à −3 dB autour du maximum : %1 → %2 (largeur %3, centre %4)")
        .arg(frequency(b.lowHz), frequency(b.highHz), frequency(b.widthHz()), frequency(b.centreHz()));
}

QVariantList AnalysisController::curves() const
{
    QVariantList list;
    if (!m_result || !m_result->ok()) return list;
    const analysis::AnalysisResult &r = *m_result;
    for (const analysis::SpectrumCurve &c : r.curves)
    {
        const std::string unit = analysis::levelUnit(c.unit, r.kind, r.scale);
        const double f = r.frequencyAt(static_cast<double>(c.peakBin));
        const double level = c.peakBin < c.level.size() ? c.level[c.peakBin] : 0.0;
        const QString levelText = r.scale == analysis::LevelScale::Decibel
                                      ? text(plot::formatFixed(level, 2)) + QStringLiteral(" ") + text(unit)
                                      : quantity(level, unit);
        list << QVariantMap{{QStringLiteral("name"), text(c.name)},
                            {QStringLiteral("channel"), c.channels.empty() ? 0 : c.channels.front()},
                            {QStringLiteral("levelUnit"), text(unit)},
                            {QStringLiteral("peaks"), [&] {
                                 QVariantList peaks;
                                 int rank = 0;
                                 for (const analysis::Peak &p : c.peaks)
                                 {
                                     if (++rank > 8) break;
                                     const QString value = r.scale == analysis::LevelScale::Decibel
                                                               ? text(plot::formatFixed(p.level, 2)) + QStringLiteral(" ") + text(unit)
                                                               : quantity(p.level, unit);
                                     peaks << QVariantMap{{QStringLiteral("rank"), rank},
                                                          {QStringLiteral("frequency"),
                                                           quantity(p.frequencyHz, "Hz", plot::significantFor(p.frequencyHz, r.binWidthHz * 0.1, 4, 10))},
                                                          {QStringLiteral("level"), value}};
                                 }
                                 return peaks;
                             }()},
                            {QStringLiteral("peak"),
                             tr("%1 : %2").arg(quantity(f, "Hz", plot::significantFor(f, r.binWidthHz, 3, 10)), levelText)},
                            {QStringLiteral("band"), bandText(c.band, r.binWidthHz)}};
    }
    return list;
}

bool AnalysisController::exportSpectrum(const QUrl &url)
{
    if (!m_result || !m_result->ok())
    {
        setLastError(tr("Pas de spectre à exporter."));
        return false;
    }
    std::ofstream out(toPath(url.toLocalFile()), std::ios::binary);
    if (!out)
    {
        setLastError(tr("Impossible d'écrire « %1 ».").arg(url.toLocalFile()));
        return false;
    }
    out << "\xEF\xBB\xBF";
    analysis::writeSpectrumCsv(out, *m_result);
    out.close();
    if (!out)
    {
        setLastError(tr("L'écriture de « %1 » n'a pas abouti (disque plein ?).").arg(url.toLocalFile()));
        return false;
    }
    setLastError(QString());
    return true;
}

void AnalysisController::setLastError(const QString &message)
{
    if (message == m_lastError) return;
    m_lastError = message;
    emit lastErrorChanged();
}
} // namespace qacq::app
