#include "modulation_plot.h"

#include "qacq/analysis/link.h"
#include "qacq/app/modulation_controller.h"
#include "qacq/dsp/equalizer.h"
#include "qacq/dsp/modulation.h"
#include "qacq/dsp/receiver.h"
#include "qacq/plot/axis.h"

#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace analysis = qacq::analysis;
namespace dsp = qacq::dsp;
namespace plot = qacq::plot;

namespace
{
constexpr double kLeft = 62.0;
constexpr double kRight = 14.0;
constexpr double kTop = 22.0;
constexpr double kBottom = 40.0;
constexpr std::size_t kHistory = 50;
constexpr std::size_t kMaximumPoints = 20000; // of the constellation drawn (evenly picked beyond)
constexpr std::size_t kMaximumTraces = 300;   // of the eye
constexpr double kFloorLog = -300.0;          // log10 of a rate of 0
constexpr std::size_t kGainPoints = 1025;     // of a gain along the frequency, over one symbol rate
constexpr double kTapSpread = 0.22;           // from the stem of the channel to the equalizer's, then to that of both (symbols)

QString text(const std::string &s)
{
    return QString::fromStdString(s);
}

QColor faded(QColor colour, double alpha)
{
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * static_cast<double>(colour.alphaF())));
    return colour;
}

// The bits of a symbol, most significant first.
QString bitsOf(std::size_t value, int bits)
{
    QString s;
    for (int b = bits - 1; b >= 0; --b) s += ((value >> b) & 1u) ? QLatin1Char('1') : QLatin1Char('0');
    return s;
}

double log10Rate(double rate)
{
    return rate > 0.0 ? std::log10(rate) : kFloorLog;
}

QString rate(double value)
{
    return text(analysis::rateText(value));
}

QString count(std::size_t value)
{
    const std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += "\xE2\x80\xAF";
        out += digits[i];
    }
    return text(out);
}

// Two links whose error rates the same theory gives (the same modulation, the same fading).
bool sameTheory(const analysis::LinkRecipe &a, const analysis::LinkRecipe &b)
{
    if (a.modulation != b.modulation || a.askLowLevel != b.askLowLevel || a.fading != b.fading) return false;
    if (a.fading == analysis::LinkFading::None) return true;
    return (a.fading != analysis::LinkFading::Rice || a.riceKDb == b.riceKDb) && analysis::linkHasPaths(a) == analysis::linkHasPaths(b);
}

// "1 erreur", "3 erreurs": French puts 0 and 1 in the singular.
QString counted(std::size_t value, const QString &one, const QString &many)
{
    return (value <= 1 ? one : many).arg(count(value));
}

// "10⁻³": the decade of a log10.
QString decade(int exponent)
{
    static const char *const digits[] = {"\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4",
                                         "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9"};
    if (exponent == 0) return QStringLiteral("1");
    std::string s = "10";
    if (exponent < 0) s += "\xE2\x81\xBB";
    const std::string n = std::to_string(std::abs(exponent));
    for (char c : n) s += digits[c - '0'];
    return text(s);
}

bool usesQ(dsp::Modulation m)
{
    return m == dsp::Modulation::Qpsk || m == dsp::Modulation::Psk8 || m == dsp::Modulation::Qam16 ||
           m == dsp::Modulation::Qam64 || m == dsp::Modulation::Qam256;
}

// Graduations of an angle in degrees: once the range is wide, parts of a turn rather than 1, 2, 5.
plot::Ticks degreeTicks(double low, double high, double pixels, double spacing)
{
    const double span = high - low;
    if (!(span >= 60.0)) return plot::linearTicks(low, high, pixels, spacing);
    const double room = std::max(1.0, pixels / spacing);
    double step = 360.0;
    for (double candidate : {15.0, 30.0, 45.0, 90.0, 180.0, 360.0})
        if (span / candidate <= room)
        {
            step = candidate;
            break;
        }
    while (span / step > room) step *= 2.0;
    plot::Ticks ticks;
    ticks.step = step;
    for (double a = std::ceil(low / step - 1e-9) * step; a <= high + 1e-9 * step; a += step) ticks.major.push_back(a == 0.0 ? 0.0 : a);
    return ticks;
}

// A value with its sign, as read: "+0,012", "−3,2", "0,000".
QString signedFixed(double value, int decimals)
{
    const QString shown = text(plot::formatFixed(value, decimals));
    return value > 0.0 && shown != text(plot::formatFixed(0.0, decimals)) ? QStringLiteral("+") + shown : shown;
}

// A value in dB as read: "4,62", "−12,3".
QString decibel(double value)
{
    return text(plot::formatFixed(value, std::abs(value) < 10.0 ? 2 : 1));
}

// A gain along the frequency (a table over one symbol rate, see TapSeries) at `cycles` per symbol from the carrier:
// taps once per symbol repeat every symbol rate.
double gainAt(const std::vector<double> &table, double cycles)
{
    if (table.size() < 2) return 0.0;
    double u = cycles + 0.5;
    u -= std::floor(u);
    const double at = u * static_cast<double>(table.size() - 1);
    const std::size_t k = std::min(static_cast<std::size_t>(at), table.size() - 2);
    return table[k] + (at - static_cast<double>(k)) * (table[k + 1] - table[k]);
}

// The points drawn under the others when asked: before the equalizer when one ran (already at unit scale), else
// before synchronization (to divide by the gain the receiver found).
struct RawPoints
{
    const std::vector<std::complex<double>> *points = nullptr;
    double gain = 1.0;
    bool equalizer = false;
};

RawPoints rawPointsOf(const analysis::LinkResult &r)
{
    if (r.equalization.used && !r.equalization.pointsBefore.empty()) return {&r.equalization.pointsBefore, 1.0, true};
    if (r.sync.used && !r.sync.rawPoints.empty()) return {&r.sync.rawPoints, r.sync.gain > 0.0 ? r.sync.gain : 1.0, false};
    return {};
}

// The levels of the constellation on one axis (distinct, sorted).
std::vector<double> levelsOf(const std::vector<std::complex<double>> &points, bool quadrature)
{
    std::vector<double> levels;
    for (const auto &p : points)
    {
        const double v = quadrature ? p.imag() : p.real();
        if (std::none_of(levels.begin(), levels.end(), [v](double l) { return std::abs(l - v) < 1e-9; })) levels.push_back(v);
    }
    std::sort(levels.begin(), levels.end());
    return levels;
}
} // namespace

QPointF ModulationPlot::Plane::toScreen(double i, double q) const
{
    return {origin.x() + (i - centreX) * scale, origin.y() - (q - centreY) * scale};
}

double ModulationPlot::Plane::iAt(double x) const
{
    return centreX + (x - origin.x()) / scale;
}

double ModulationPlot::Plane::qAt(double y) const
{
    return centreY - (y - origin.y()) / scale;
}

ModulationPlot::ModulationPlot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
    connect(this, &ModulationPlot::styleChanged, this, [this] { update(); });
}

QObject *ModulationPlot::modulation() const
{
    return m_modulation.data();
}

void ModulationPlot::setModulation(QObject *modulation)
{
    auto *controller = qobject_cast<qacq::app::ModulationController *>(modulation);
    if (controller == m_modulation.data()) return;
    disconnect(m_resultConnection);
    disconnect(m_curvesConnection);
    disconnect(m_recipeConnection);
    m_modulation = controller;
    if (controller)
    {
        m_resultConnection = connect(controller, &qacq::app::ModulationController::resultChanged, this, &ModulationPlot::contentUpdated);
        m_curvesConnection = connect(controller, &qacq::app::ModulationController::curvesChanged, this, &ModulationPlot::contentUpdated);
        m_recipeConnection = connect(controller, &qacq::app::ModulationController::sweepChanged, this, &ModulationPlot::contentUpdated);
    }
    emit modulationChanged();
    contentUpdated();
}

QString ModulationPlot::view() const
{
    switch (m_kind)
    {
        case Kind::Constellation: return QStringLiteral("constellation");
        case Kind::Eye: return QStringLiteral("eye");
        case Kind::Ber: return QStringLiteral("ber");
        case Kind::Sync: return QStringLiteral("sync");
        case Kind::Channel: return QStringLiteral("channel");
    }
    return QStringLiteral("constellation");
}

void ModulationPlot::setView(const QString &key)
{
    const Kind kind = key == QStringLiteral("eye")       ? Kind::Eye
                      : key == QStringLiteral("ber")     ? Kind::Ber
                      : key == QStringLiteral("sync")    ? Kind::Sync
                      : key == QStringLiteral("channel") ? Kind::Channel
                                                         : Kind::Constellation;
    if (kind == m_kind) return;
    m_kind = kind;
    m_view = View{};
    m_history.clear();
    emit kindChanged();
    emit viewChanged();
    emit contentChanged();
    updateReadout();
    update();
}

void ModulationPlot::setShowRaw(bool show)
{
    if (show == m_showRaw) return;
    m_showRaw = show;
    emit showRawChanged();
    emit contentChanged(); // the facts above the frame
    update();
}

bool ModulationPlot::rawShown() const
{
    return m_showRaw && m_kind == Kind::Constellation && m_result && m_result->ok() &&
           m_result->recipe.modulation != dsp::Modulation::Fsk && rawPointsOf(*m_result).points != nullptr;
}

void ModulationPlot::contentUpdated()
{
    const qacq::app::ModulationController *controller = m_modulation.data();
    std::shared_ptr<const analysis::LinkResult> result = controller ? controller->currentResult() : nullptr;
    // Another modulation: its plane, its levels; the view starts again.
    // (The synchronization: another number of symbols too, its horizontal axis. The channel: other paths or
    // another equalizer too, other taps to see.)
    if (m_kind != Kind::Ber && result && m_result && result->ok() && m_result->ok() &&
        (result->recipe.modulation != m_result->recipe.modulation ||
         result->recipe.representation != m_result->recipe.representation || result->recipe.symbolRate != m_result->recipe.symbolRate ||
         (m_kind == Kind::Sync && result->sync.timingError.size() != m_result->sync.timingError.size()) ||
         (m_kind == Kind::Channel &&
          (result->recipe.paths != m_result->recipe.paths || result->recipe.equalizer != m_result->recipe.equalizer ||
           result->recipe.equalizerTaps != m_result->recipe.equalizerTaps || result->recipe.receiver != m_result->recipe.receiver))))
    {
        m_view = View{};
        m_history.clear();
        emit viewChanged();
    }
    m_result = std::move(result);
    countResult();
    if (m_theory.size() > 64) m_theory.clear();
    emit contentChanged();
    updateReadout();
    update();
}

void ModulationPlot::countResult()
{
    m_shown = m_wrong = m_traces = 0;
    m_pointStep = m_traceStep = 1;
    m_syncPhase.clear();
    m_rawReach = 0.0;
    m_channel = m_equalizer = m_combined = TapSeries{};
    const analysis::LinkResult *r = m_result.get();
    if (!r || !r->ok()) return;
    // The symbols that count: after the preamble (or the edge of the pulses), before the edge at the end.
    // (As the link counts them: the last ones of the frame out, not those of what the receiver decided.)
    const std::size_t sent = r->sentSymbols.size(), counted = sent > r->skipEndSymbols ? sent - r->skipEndSymbols : 0;
    const std::size_t n = std::min(r->reception.decisionPoints.size(), std::min(sent, r->reception.symbols.size()));
    const std::size_t first = std::min(r->skipSymbols, n), last = std::min(n, counted);
    m_shown = last > first ? last - first : 0;
    for (std::size_t k = first; k < last; ++k)
        if (r->sentSymbols[k] != r->reception.symbols[k]) ++m_wrong;
    m_pointStep = std::max<std::size_t>(1, (m_shown + kMaximumPoints - 1) / kMaximumPoints);
    // (The synchronized receiver decides a few symbols after the end of the frame too: not traces of the eye.)
    const std::size_t t = std::min(r->reception.decisionTimes.size(), sent);
    const std::size_t tFirst = std::min(r->skipSymbols, t), tLast = std::min(t, counted);
    m_traces = tLast > tFirst ? tLast - tFirst : 0;
    m_traceStep = std::max<std::size_t>(1, (m_traces + kMaximumTraces - 1) / kMaximumTraces);
    // How far the points before the equalizer or the synchronization reach.
    if (const RawPoints raw = rawPointsOf(*r); raw.points)
        for (std::size_t k = first; k < std::min(last, raw.points->size()); k += m_pointStep)
            m_rawReach = std::max({m_rawReach, std::abs((*raw.points)[k].real()) / raw.gain, std::abs((*raw.points)[k].imag()) / raw.gain});
    // The channel at the decision instants (a single path: each symbol alone at its instant), the equalizer, both.
    if (dsp::hasConstellation(r->recipe.modulation))
    {
        const analysis::LinkEqualization &q = r->equalization;
        const auto series = [](const std::vector<std::complex<double>> &taps, int firstIndex) {
            TapSeries out;
            out.first = firstIndex;
            out.taps = taps;
            if (taps.empty()) return out;
            const dsp::SymbolTaps symbolTaps{taps, firstIndex};
            out.gainDb.resize(kGainPoints);
            for (std::size_t k = 0; k < kGainPoints; ++k)
            {
                const double cycles = static_cast<double>(k) / static_cast<double>(kGainPoints - 1) - 0.5;
                out.gainDb[k] = 20.0 * std::log10(std::max(1e-6, std::abs(dsp::tapsResponse(symbolTaps, cycles))));
            }
            return out;
        };
        if (!q.response.empty())
            m_channel = series(q.response, q.responseFirst);
        else if (q.used && !q.paths)
            m_channel = series({{1.0, 0.0}}, 0);
        if (q.used && !m_channel.taps.empty()) m_equalizer = series(q.taps, q.tapsFirst);
        if (!m_equalizer.taps.empty()) m_combined = series(q.combined, q.combinedFirst);
    }
    if (!r->sync.used) return;
    // The phase error from one symbol to the next without its jumps of a turn: a slip is a step.
    const std::vector<double> &e = r->sync.phaseError;
    m_syncPhase.resize(e.size());
    double unwrapped = e.empty() ? 0.0 : e.front();
    for (std::size_t k = 0; k < e.size(); ++k)
    {
        if (k > 0) unwrapped += std::remainder(e[k] - e[k - 1], 360.0);
        m_syncPhase[k] = unwrapped;
    }
}

bool ModulationPlot::hasData() const
{
    switch (m_kind)
    {
        case Kind::Constellation: return m_result && m_result->ok() && !m_result->reception.decisionPoints.empty();
        case Kind::Eye: return m_result && m_result->ok() && !m_result->reception.matchedI.empty();
        case Kind::Ber: return m_modulation != nullptr;
        case Kind::Sync: return m_result && m_result->ok() && m_result->sync.used && !m_result->sync.timingError.empty();
        case Kind::Channel: return m_result && m_result->ok() && !m_channel.taps.empty();
    }
    return false;
}

QFont ModulationPlot::plotFont() const
{
    QFont font;
    font.setPixelSize(std::max(8, m_fontSize));
    return font;
}

QString ModulationPlot::headerTitle() const
{
    const analysis::LinkResult *r = m_result.get();
    const bool ok = r && r->ok();
    switch (m_kind)
    {
        case Kind::Constellation:
            return ok && r->recipe.modulation == dsp::Modulation::Fsk ? tr("Énergie à fc − Δf (un 0)") : tr("Q (en quadrature)");
        case Kind::Eye:
            return ok && r->recipe.pulse == dsp::PulseShape::RootRaisedCosine ? tr("Sortie du filtre adapté (racine de cosinus surélevé)")
                                                                               : tr("Sortie du filtre adapté (moyenne sur un symbole)");
        case Kind::Ber: return tr("Taux d'erreur binaire (TEB)");
        case Kind::Sync:
            return ok && r->sync.phaseError.empty() ? tr("Synchronisation : erreur de rythme")
                                                    : tr("Synchronisation : erreurs de rythme et de porteuse");
        case Kind::Channel: return m_equalizer.taps.empty() ? tr("Canal") : tr("Canal et égaliseur");
    }
    return {};
}

QStringList ModulationPlot::headerFacts() const
{
    QStringList facts;
    const analysis::LinkResult *r = m_result.get();
    if (!r || !r->ok()) return facts;
    switch (m_kind)
    {
        case Kind::Constellation:
        {
            const QString symbols = counted(m_shown, tr("%1 symbole"), tr("%1 symboles"));
            facts << (m_wrong == 0   ? tr("%1, aucun décidé à tort").arg(symbols)
                      : m_wrong == 1 ? tr("%1, 1 décidé à tort (en rouge)").arg(symbols)
                                     : tr("%1, %2 décidés à tort (en rouge)").arg(symbols, count(m_wrong)));
            if (std::isfinite(r->evmPercent))
                facts << tr("EVM %1 %, MER %2 dB")
                             .arg(text(plot::formatFixed(r->evmPercent, r->evmPercent < 10.0 ? 2 : 1)),
                                  std::isfinite(r->merDb) ? text(plot::formatFixed(r->merDb, 1)) : QStringLiteral("∞"));
            if (m_pointStep > 1) facts << tr("un point juste sur %1 dessiné").arg(static_cast<qint64>(m_pointStep));
            if (rawShown()) facts << (rawPointsOf(*r).equalizer ? tr("en gris : avant l'égaliseur") : tr("en gris : avant synchronisation"));
            break;
        }
        case Kind::Eye:
            if (!r->reception.matchedI.empty() && m_traces > 0)
                facts << tr("%1 sur %2").arg(counted((m_traces + m_traceStep - 1) / m_traceStep, tr("%1 trace"), tr("%1 traces")),
                                             counted(m_traces, tr("%1 symbole"), tr("%1 symboles")));
            break;
        case Kind::Ber: break;
        case Kind::Sync:
        {
            const analysis::LinkSync &y = r->sync;
            if (!y.used) break;
            if (std::isfinite(y.timingErrorRms)) facts << tr("rythme %1 symbole efficace").arg(text(plot::formatFixed(y.timingErrorRms, 3)));
            if (!y.phaseError.empty() && std::isfinite(y.phaseErrorRms))
                facts << tr("porteuse %1° efficace").arg(text(plot::formatFixed(y.phaseErrorRms, 2)));
            if (dsp::rotationalSymmetryDegrees(r->recipe.modulation) > 0.0)
                facts << (y.phaseSlips == 0 ? tr("aucun glissement") : counted(y.phaseSlips, tr("%1 glissement"), tr("%1 glissements")));
            facts << (y.frameFound ? tr("corrélation du préambule %1 %") : tr("trame mal trouvée (corrélation %1 %)"))
                         .arg(static_cast<int>(std::lround(100.0 * y.frameQuality)));
            break;
        }
        case Kind::Channel:
        {
            // A symbol against the other symbols and the noise, without equalizer and after it.
            const analysis::LinkEqualization &q = r->equalization;
            if (m_channel.taps.empty()) break;
            const bool before = std::isfinite(q.sinrBeforeDb), after = q.used && std::isfinite(q.sinrAfterDb);
            if (before && after)
                facts << tr("SINR %1 dB, puis %2 dB égalisé").arg(decibel(q.sinrBeforeDb), decibel(q.sinrAfterDb));
            else if (before)
                facts << tr("SINR sans égaliseur %1 dB").arg(decibel(q.sinrBeforeDb));
            else if (after)
                facts << tr("SINR égalisé %1 dB").arg(decibel(q.sinrAfterDb));
            if (q.used) facts << (q.adaptive ? tr("%1 prises apprises") : tr("%1 prises calculées")).arg(static_cast<qint64>(q.taps.size()));
            if (q.used && !q.feedback.empty())
                facts << counted(q.feedback.size(), tr("%1 prise de retour (son écho retiré du canal × égaliseur)"),
                                 tr("%1 prises de retour (leurs échos retirés du canal × égaliseur)"));
            if (q.used && std::isfinite(q.settledErrorDb)) facts << tr("erreur finale %1 dB").arg(decibel(q.settledErrorDb));
            // Where the decisions are taken, when it is not on the strongest path (a timing loop between two paths).
            if (q.paths && std::abs(q.instantSymbols) >= analysis::kShownDecisionOffsetSymbols)
                facts << (q.instantSymbols > 0.0 ? tr("décision %1 symb. après le plus fort") : tr("décision %1 symb. avant le plus fort"))
                             .arg(text(plot::formatFixed(std::abs(q.instantSymbols), 2)));
            break;
        }
    }
    return facts;
}

std::vector<ModulationPlot::LegendEntry> ModulationPlot::channelLegend() const
{
    std::vector<LegendEntry> entries;
    if (m_kind != Kind::Channel || !m_result || !m_result->ok() || m_channel.taps.empty()) return entries;
    entries.push_back({tr("canal"), m_accent, false});
    if (!m_equalizer.taps.empty()) entries.push_back({tr("égaliseur"), curveColour(1), false});
    if (!m_combined.taps.empty()) entries.push_back({tr("canal × égaliseur"), m_text, false});
    const analysis::LinkEqualization &q = m_result->equalization;
    if (q.paths && q.mainPath < q.typed.size() && q.typed.size() == q.amplitude.size() && q.amplitude[q.mainPath] > 0.0)
        entries.push_back({tr("trajets"), m_muted, true});
    return entries;
}

std::vector<std::vector<ModulationPlot::LegendEntry>> ModulationPlot::legendRows(bool &beside) const
{
    beside = false;
    std::vector<std::vector<LegendEntry>> rows;
    const std::vector<LegendEntry> entries = channelLegend();
    if (entries.empty()) return rows;
    const QFontMetricsF metrics(plotFont());
    const double room = std::max(40.0, width() - kRight - 4.0);
    const auto widthOf = [&metrics](const LegendEntry &entry) { return 14.0 + metrics.horizontalAdvance(entry.name) + 12.0; };
    double all = 0.0;
    for (const LegendEntry &entry : entries) all += widthOf(entry);
    beside = metrics.horizontalAdvance(headerTitle()) + 24.0 + all <= room;
    double used = 0.0;
    for (const LegendEntry &entry : entries)
    {
        if (rows.empty() || used + widthOf(entry) > room)
        {
            rows.emplace_back();
            used = 0.0;
        }
        rows.back().push_back(entry);
        used += widthOf(entry);
    }
    return rows;
}

std::vector<ModulationPlot::HeaderRow> ModulationPlot::headerRows() const
{
    const QFontMetricsF metrics(plotFont());
    const double room = std::max(40.0, width() - kRight - 4.0);
    std::vector<HeaderRow> rows{{headerTitle(), QString()}};
    const double titleWidth = metrics.horizontalAdvance(rows.front().left) + 24.0;
    const QStringList facts = headerFacts();
    // The names of what the channel view draws take the right of the first line, or lines of their own under it:
    // the facts after them.
    bool beside = false;
    if (const std::size_t lines = legendRows(beside).size(); lines > 0)
    {
        for (std::size_t k = beside ? 1 : 0; k < lines; ++k) rows.push_back({});
        if (!facts.isEmpty()) rows.push_back({});
    }
    for (const QString &fact : facts)
    {
        const QString joined = rows.back().right.isEmpty() ? fact : rows.back().right + QStringLiteral(" · ") + fact;
        const double used = rows.size() == 1 ? titleWidth : 0.0;
        if (used + metrics.horizontalAdvance(joined) <= room || (rows.size() > 1 && rows.back().right.isEmpty()))
            rows.back().right = joined;
        else
            rows.push_back({QString(), fact});
    }
    return rows;
}

double ModulationPlot::topMargin() const
{
    const QFontMetricsF metrics(plotFont());
    return std::max(kTop, 6.0 + static_cast<double>(headerRows().size()) * (metrics.height() + 1.0));
}

QRectF ModulationPlot::plotRect() const
{
    const double top = topMargin();
    return QRectF(kLeft, top, std::max(0.0, width() - kLeft - kRight), std::max(0.0, height() - top - kBottom));
}

QColor ModulationPlot::curveColour(std::size_t index) const
{
    if (m_curveColors.isEmpty()) return m_accent;
    const QVariant v = m_curveColors.at(static_cast<qsizetype>(index % static_cast<std::size_t>(m_curveColors.size())));
    QColor c = v.value<QColor>();
    if (!c.isValid()) c = QColor(v.toString());
    return c.isValid() ? c : m_accent;
}

ModulationPlot::View ModulationPlot::automaticView() const
{
    View v;
    const analysis::LinkResult *r = m_result.get();
    switch (m_kind)
    {
        case Kind::Constellation:
        {
            if (!r || !r->ok()) return v;
            if (r->recipe.modulation == dsp::Modulation::Fsk)
            {
                double top = 1.0;
                for (const auto &p : r->reception.decisionPoints) top = std::max({top, p.real(), p.imag()});
                top = std::min(top, 2.5);
                v.x = {-0.12 * top, 1.12 * top};
                v.y = v.x;
                return v;
            }
            // The extent of the constellation: its square (QAM, ASK), its circle (PSK: QPSK without noise shows its
            // circle, not just its four points).
            const bool circle = r->recipe.modulation == dsp::Modulation::Qpsk || r->recipe.modulation == dsp::Modulation::Psk8;
            double m = 0.0;
            for (const auto &p : r->constellation)
                m = std::max(m, circle ? std::abs(p) : std::max(std::abs(p.real()), std::abs(p.imag())));
            if (!(m > 0.0)) m = 1.0;
            double reach = 0.0;
            const std::size_t n = r->reception.decisionPoints.size();
            const std::size_t step = std::max<std::size_t>(1, n / 4000);
            for (std::size_t k = 0; k < n; k += step)
            {
                const auto &p = r->reception.decisionPoints[k];
                reach = std::max({reach, std::abs(p.real()), std::abs(p.imag())});
            }
            // (Before synchronization the points turn: their ring whole.)
            if (rawShown()) reach = std::max(reach, m_rawReach);
            const double half = std::max(1.3 * m, std::min(1.05 * reach, 2.5 * m));
            v.x = {-half, half};
            v.y = {-half, half};
            return v;
        }
        case Kind::Eye:
        {
            if (!r || !r->ok()) return v;
            const double t = 1.0 / r->recipe.symbolRate;
            v.x = {-t, t};
            double low = 0.0, high = 0.0;
            std::size_t levels = 0;
            for (bool q : {false, true})
            {
                if (q && !usesQ(r->recipe.modulation)) continue;
                const std::vector<double> l = levelsOf(r->constellation, q);
                if (l.empty()) continue;
                low = levels == 0 ? l.front() : std::min(low, l.front());
                high = levels == 0 ? l.back() : std::max(high, l.back());
                levels = std::max(levels, l.size());
            }
            const double span = high - low;
            const double step = levels > 1 ? span / static_cast<double>(levels - 1) : 1.0;
            v.y = {low - 0.75 * step, high + 0.75 * step};
            if (!(v.y.span() > 0.0)) v.y = {-1.5, 1.5};
            return v;
        }
        case Kind::Ber:
        {
            const qacq::app::ModulationController *c = m_modulation.data();
            double from = 0.0, to = 12.0;
            if (c)
            {
                const QVariantMap s = c->sweep();
                from = s.value(QStringLiteral("from")).toDouble();
                to = s.value(QStringLiteral("to")).toDouble();
                for (const auto &curve : c->curves())
                    for (const analysis::BerPoint &p : curve.points)
                    {
                        from = std::min(from, p.ebN0Db);
                        to = std::max(to, p.ebN0Db);
                    }
            }
            if (r && r->ok() && r->recipe.noise)
            {
                from = std::min(from, r->recipe.ebN0Db);
                to = std::max(to, r->recipe.ebN0Db);
            }
            if (!(to > from)) to = from + 1.0;
            v.x = {from - 0.5, to + 0.5};
            // Down to the least rate on screen: the theories at the right end, the points measured.
            double lowest = -2.0;
            const auto deepest = [&lowest](double rateValue) {
                if (rateValue > 0.0 && std::isfinite(rateValue)) lowest = std::min(lowest, std::log10(rateValue));
            };
            if (c)
            {
                for (const auto &curve : c->curves())
                {
                    deepest(analysis::linkTheoryBer(curve.recipe, v.x.high));
                    for (const analysis::BerPoint &p : curve.points) deepest(p.errors > 0 ? p.lower : p.upper);
                }
                deepest(analysis::linkTheoryBer(c->currentRecipe(), v.x.high));
            }
            v.y = {std::clamp(std::floor(lowest) - 0.25, -12.0, -1.0), std::log10(0.7)};
            return v;
        }
        case Kind::Sync:
        {
            if (!r || !r->ok() || r->sync.timingError.empty()) return v;
            const auto n = static_cast<double>(r->sync.timingError.size());
            v.x = {-0.5 - 0.01 * n, n - 0.5 + 0.01 * n};
            double timing = 0.0;
            for (double e : r->sync.timingError) timing = std::max(timing, std::abs(e));
            timing = std::max(1.15 * timing, 0.005);
            v.y = {-timing, timing};
            double low = 0.0, high = 0.0;
            for (double e : m_syncPhase)
            {
                low = std::min(low, e);
                high = std::max(high, e);
            }
            if (high - low < 60.0)
            {
                // Without a slip: about 0.
                const double phase = std::max(1.15 * std::max(-low, high), 0.5);
                v.y2 = {-phase, phase};
            }
            else
            {
                const double pad = 0.08 * (high - low);
                v.y2 = {low - pad, high + pad};
            }
            return v;
        }
        case Kind::Channel:
        {
            if (!r || !r->ok() || m_channel.taps.empty()) return v;
            // The taps that weigh (a hundredth of the strongest of their kind) and one symbol each side of them, three
            // symbols each side of the decision at least; room above the strongest for the names.
            double top = 1.0;
            int low = -3, high = 3;
            double least = 0.0, most = 0.0;
            for (const TapSeries *s : {&m_channel, &m_equalizer, &m_combined})
            {
                double strongest = 0.0;
                for (const std::complex<double> &tap : s->taps) strongest = std::max(strongest, std::abs(tap));
                top = std::max(top, strongest);
                for (std::size_t k = 0; k < s->taps.size(); ++k)
                {
                    if (!(std::abs(s->taps[k]) >= 0.01 * strongest) || !(strongest > 0.0)) continue;
                    low = std::min(low, s->first + static_cast<int>(k) - 1);
                    high = std::max(high, s->first + static_cast<int>(k) + 1);
                }
                for (double gain : s->gainDb)
                {
                    least = std::min(least, gain);
                    most = std::max(most, gain);
                }
            }
            v.x = {static_cast<double>(low) - 0.5, static_cast<double>(high) + 0.5};
            v.y = {-0.06 * top, 1.18 * top};
            v.x2 = {-0.5 * r->recipe.symbolRate, 0.5 * r->recipe.symbolRate};
            // The gains: a notch is shown 60 dB deep at most; 6 dB each side of 0 at least.
            least = std::max(least, most - 60.0);
            const double pad = std::max(1.5, 0.08 * (most - least));
            v.y2 = {std::min(least - pad, -6.0), std::max(most + pad, 6.0)};
            return v;
        }
    }
    return v;
}

ModulationPlot::View ModulationPlot::currentView() const
{
    return m_view.automatic ? automaticView() : m_view;
}

void ModulationPlot::setViewRange(const View &view)
{
    m_view = view;
    m_view.automatic = false;
    emit viewChanged();
    updateReadout();
    update();
}

void ModulationPlot::rememberView()
{
    const View v = currentView();
    if (!m_history.empty() && m_history.back().automatic == m_view.automatic && m_history.back().x.low == v.x.low &&
        m_history.back().x.high == v.x.high && m_history.back().y.low == v.y.low && m_history.back().y.high == v.y.high &&
        m_history.back().y2.low == v.y2.low && m_history.back().y2.high == v.y2.high && m_history.back().x2.low == v.x2.low &&
        m_history.back().x2.high == v.x2.high)
        return;
    View remembered = v;
    remembered.automatic = m_view.automatic;
    m_history.push_back(remembered);
    if (m_history.size() > kHistory) m_history.erase(m_history.begin());
    emit viewChanged();
}

void ModulationPlot::showAll()
{
    if (m_view.automatic) return;
    rememberView();
    m_view = View{};
    emit viewChanged();
    updateReadout();
    update();
}

void ModulationPlot::goBack()
{
    if (m_history.empty()) return;
    m_view = m_history.back();
    m_history.pop_back();
    emit viewChanged();
    updateReadout();
    update();
}

void ModulationPlot::zoomView(double factor)
{
    const QRectF area = plotRect();
    zoomAt(factor, area.center(), false);
}

void ModulationPlot::zoomAt(double factor, const QPointF &at, bool other)
{
    if (!(factor > 0.0) || !hasData()) return;
    const QRectF area = plotRect();
    if (area.width() < 20.0 || area.height() < 20.0) return;
    View v = currentView();
    rememberView();
    const auto about = [factor](Range r, double centre) {
        return Range{centre + (r.low - centre) * factor, centre + (r.high - centre) * factor};
    };
    switch (m_kind)
    {
        case Kind::Constellation:
        {
            const Plane pl = plane(area, v);
            v.x = about(v.x, pl.iAt(at.x()));
            v.y = about(v.y, pl.qAt(at.y()));
            break;
        }
        case Kind::Eye:
        {
            if (other)
            {
                const double t = v.x.low + (at.x() - area.left()) / area.width() * v.x.span();
                v.x = about(v.x, t);
            }
            else
            {
                // About the value under the pointer in its band.
                const std::vector<QRectF> lanes = eyeLanes(area);
                QRectF lane = lanes.front();
                for (const QRectF &l : lanes)
                    if (at.y() >= l.top() && at.y() <= l.bottom()) lane = l;
                const double value = v.y.high - (at.y() - lane.top()) / lane.height() * v.y.span();
                v.y = about(v.y, value);
            }
            break;
        }
        case Kind::Ber:
        {
            if (other)
            {
                const double value = v.y.high - (at.y() - area.top()) / area.height() * v.y.span();
                v.y = about(v.y, value);
                v.y.high = std::min(v.y.high, 0.5);
            }
            else
            {
                const double eb = v.x.low + (at.x() - area.left()) / area.width() * v.x.span();
                v.x = about(v.x, eb);
            }
            break;
        }
        case Kind::Sync:
        {
            if (other)
            {
                // The errors of the band under the pointer, about the value there.
                const std::vector<QRectF> lanes = syncLanes(area);
                std::size_t index = 0;
                for (std::size_t k = 0; k < lanes.size(); ++k)
                    if (at.y() >= lanes[k].top() && at.y() <= lanes[k].bottom()) index = k;
                Range &range = index == 1 ? v.y2 : v.y;
                const double value = range.high - (at.y() - lanes[index].top()) / lanes[index].height() * range.span();
                range = about(range, value);
            }
            else
            {
                // The symbols, about the one under the pointer: never beyond the frame, never closer than 8 symbols.
                const Range frame{-1.0, static_cast<double>(m_result->sync.timingError.size())};
                const double symbol = v.x.low + (at.x() - area.left()) / area.width() * v.x.span();
                Range x = about(v.x, symbol);
                if (x.span() < 8.0) x = {symbol - 8.0 * (symbol - v.x.low) / v.x.span(), symbol + 8.0 * (v.x.high - symbol) / v.x.span()};
                if (x.span() >= frame.span()) x = automaticView().x;
                else if (x.low < frame.low) x = {frame.low, frame.low + x.span()};
                else if (x.high > frame.high) x = {frame.high - x.span(), frame.high};
                v.x = x;
            }
            break;
        }
        case Kind::Channel:
        {
            // The band under the pointer (the nearest one between the two): its symbols or its frequencies about the
            // pointer, never so wide that nothing is read, never closer than two symbols; Ctrl: its values.
            const std::vector<QRectF> lanes = channelLanes(area);
            const bool second = at.y() > 0.5 * (lanes[0].bottom() + lanes[1].top());
            const QRectF lane = lanes[second ? 1 : 0];
            Range &across = second ? v.x2 : v.x;
            Range &along = second ? v.y2 : v.y;
            if (other)
            {
                along = about(along, along.high - (at.y() - lane.top()) / lane.height() * along.span());
            }
            else
            {
                const double share = std::clamp((at.x() - lane.left()) / lane.width(), 0.0, 1.0);
                const double value = across.low + share * across.span();
                const double rate = m_result->recipe.symbolRate;
                const double span = std::clamp(across.span() * factor, second ? 1e-3 * rate : 2.0, second ? 4.0 * rate : 300.0);
                across = {value - share * span, value + (1.0 - share) * span};
            }
            break;
        }
    }
    if (!(v.x.span() > 1e-9) || !(v.y.span() > 1e-9) || !(v.y2.span() > 1e-9) || !(v.x2.span() > 1e-9)) return;
    setViewRange(v);
}

void ModulationPlot::moveBy(double dx, double dy)
{
    const QRectF area = plotRect();
    if (area.width() < 20.0 || area.height() < 20.0) return;
    View v = currentView();
    switch (m_kind)
    {
        case Kind::Constellation:
        {
            const Plane pl = plane(area, v);
            const double di = -dx / pl.scale, dq = dy / pl.scale;
            v.x = {v.x.low + di, v.x.high + di};
            v.y = {v.y.low + dq, v.y.high + dq};
            break;
        }
        case Kind::Eye:
        {
            const std::vector<QRectF> lanes = eyeLanes(area);
            const double dt = -dx / area.width() * v.x.span();
            const double dvalue = dy / lanes.front().height() * v.y.span();
            v.x = {v.x.low + dt, v.x.high + dt};
            v.y = {v.y.low + dvalue, v.y.high + dvalue};
            break;
        }
        case Kind::Ber:
        {
            const double deb = -dx / area.width() * v.x.span();
            const double dl = dy / area.height() * v.y.span();
            v.x = {v.x.low + deb, v.x.high + deb};
            v.y = {v.y.low + dl, v.y.high + dl};
            break;
        }
        case Kind::Sync:
        {
            // The symbols (kept within the frame), and the errors of the band the drag started in.
            const std::vector<QRectF> lanes = syncLanes(area);
            const Range frame{-1.0, static_cast<double>(m_result ? m_result->sync.timingError.size() : 0)};
            double ds = -dx / area.width() * v.x.span();
            ds = std::clamp(ds, std::min(0.0, frame.low - v.x.low), std::max(0.0, frame.high - v.x.high));
            v.x = {v.x.low + ds, v.x.high + ds};
            std::size_t index = 0;
            for (std::size_t k = 0; k < lanes.size(); ++k)
                if (m_pressAt.y() >= lanes[k].top() && m_pressAt.y() <= lanes[k].bottom()) index = k;
            Range &range = index == 1 ? v.y2 : v.y;
            const double dvalue = dy / lanes[index].height() * range.span();
            range = {range.low + dvalue, range.high + dvalue};
            break;
        }
        case Kind::Channel:
        {
            // The band the drag started in: its symbols or its frequencies, and its values.
            const std::vector<QRectF> lanes = channelLanes(area);
            const bool second = m_pressAt.y() > 0.5 * (lanes[0].bottom() + lanes[1].top());
            const QRectF lane = lanes[second ? 1 : 0];
            Range &across = second ? v.x2 : v.x;
            Range &along = second ? v.y2 : v.y;
            const double dacross = -dx / lane.width() * across.span(), dalong = dy / lane.height() * along.span();
            across = {across.low + dacross, across.high + dacross};
            along = {along.low + dalong, along.high + dalong};
            break;
        }
    }
    setViewRange(v);
}

ModulationPlot::Plane ModulationPlot::plane(const QRectF &area, const View &v) const
{
    Plane pl;
    pl.scale = std::min(area.width() / v.x.span(), area.height() / v.y.span());
    pl.centreX = 0.5 * (v.x.low + v.x.high);
    pl.centreY = 0.5 * (v.y.low + v.y.high);
    pl.origin = area.center();
    return pl;
}

std::vector<QRectF> ModulationPlot::eyeLanes(const QRectF &area) const
{
    const bool two = m_result && m_result->ok() && usesQ(m_result->recipe.modulation);
    if (!two) return {area};
    const double gap = 14.0;
    const double h = 0.5 * (area.height() - gap);
    return {QRectF(area.left(), area.top(), area.width(), h), QRectF(area.left(), area.top() + h + gap, area.width(), h)};
}

std::vector<QRectF> ModulationPlot::syncLanes(const QRectF &area) const
{
    const bool two = m_result && m_result->ok() && !m_result->sync.phaseError.empty();
    if (!two) return {area};
    const double gap = 14.0;
    const double h = 0.5 * (area.height() - gap);
    return {QRectF(area.left(), area.top(), area.width(), h), QRectF(area.left(), area.top() + h + gap, area.width(), h)};
}

std::vector<QRectF> ModulationPlot::channelLanes(const QRectF &area) const
{
    // Each band has its own horizontal axis: the graduations of the first and its title fit between the two.
    const double gap = 2.0 * QFontMetricsF(plotFont()).height() + 12.0;
    const double h = std::max(1.0, 0.5 * (area.height() - gap));
    return {QRectF(area.left(), area.top(), area.width(), h), QRectF(area.left(), area.top() + h + gap, area.width(), h)};
}

std::vector<QRectF> ModulationPlot::lanesOf(const QRectF &area) const
{
    switch (m_kind)
    {
        case Kind::Eye: return eyeLanes(area);
        case Kind::Sync: return syncLanes(area);
        case Kind::Channel: return channelLanes(area);
        default: return {area};
    }
}

QString ModulationPlot::theoryName(const analysis::LinkRecipe &recipe)
{
    const QString name = text(std::string(dsp::modulationName(recipe.modulation)));
    switch (recipe.fading)
    {
        case analysis::LinkFading::Rayleigh: return tr("%1, Rayleigh").arg(name);
        case analysis::LinkFading::Rice: return tr("%1, Rice K = %2 dB").arg(name, text(plot::formatCompact(recipe.riceKDb, "", 3)));
        case analysis::LinkFading::None: break;
    }
    return name;
}

const std::vector<double> &ModulationPlot::theory(const analysis::LinkRecipe &recipe, const Range &x, int count)
{
    char key[200];
    const bool fades = recipe.fading != analysis::LinkFading::None;
    std::snprintf(key, sizeof key, "%d|%.17g|%d|%.17g|%d|%.17g|%.17g|%d", static_cast<int>(recipe.modulation), recipe.askLowLevel,
                  static_cast<int>(recipe.fading), fades ? recipe.riceKDb : 0.0, fades && analysis::linkHasPaths(recipe) ? 1 : 0,
                  x.low, x.high, count);
    auto found = m_theory.find(key);
    if (found != m_theory.end()) return found->second;
    std::vector<double> values(static_cast<std::size_t>(std::max(2, count)));
    for (std::size_t k = 0; k < values.size(); ++k)
    {
        const double eb = x.low + x.span() * static_cast<double>(k) / static_cast<double>(values.size() - 1);
        values[k] = log10Rate(analysis::linkTheoryBer(recipe, eb));
    }
    return m_theory.emplace(key, std::move(values)).first->second;
}

void ModulationPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    p.setFont(plotFont());
    const QRectF area = plotRect();
    if (area.width() < 40.0 || area.height() < 30.0) return;
    const qacq::app::ModulationController *controller = m_modulation.data();
    if (!controller)
    {
        paintMessage(p, m_emptyText);
        return;
    }
    if (m_kind != Kind::Ber)
    {
        if (!m_result || !m_result->ok())
        {
            paintMessage(p, m_result && !m_result->error.empty() ? text(m_result->error)
                                                                  : !m_emptyText.isEmpty() ? m_emptyText : tr("Calcul de la liaison…"));
            return;
        }
        if (m_kind == Kind::Eye && m_result->reception.matchedI.empty())
        {
            paintMessage(p, tr("Pas de diagramme de l'œil pour la FSK non cohérente : chaque symbole y est décidé par "
                               "l'énergie de ses deux tonalités, sans filtre adapté à suivre."));
            return;
        }
        if (m_kind == Kind::Sync && !hasData())
        {
            // Asked for and not computed yet, or nothing to synchronize.
            const bool asked = controller->currentRecipe().receiver == analysis::LinkReceiver::Synchronized;
            paintMessage(p, asked ? (!m_emptyText.isEmpty() ? m_emptyText : tr("Calcul de la liaison…"))
                                  : tr("Le récepteur idéal connaît le canal et le défait exactement : rien à synchroniser. Avec le "
                                       "récepteur synchronisé (ruban Liaison ▸ Récepteur), ses erreurs de rythme et de porteuse "
                                       "se lisent ici, symbole par symbole."));
            return;
        }
        if (m_kind == Kind::Channel && !hasData())
        {
            paintMessage(p, m_result->recipe.modulation == dsp::Modulation::Fsk
                                ? tr("Pas de canal symbole par symbole pour la FSK non cohérente : chaque symbole y est décidé par "
                                     "l'énergie de ses deux tonalités, sans constellation où suivre ses voisins, et sans égaliseur.")
                                : m_result->equalization.paths
                                      ? tr("La réponse du canal aux instants de décision n'a pas pu être mesurée.")
                                      : tr("Un seul trajet : chaque symbole arrive seul à son instant de décision, rien à égaliser. "
                                           "Avec plusieurs trajets (ruban Liaison ▸ Canal ▸ Trajets), ce que chaque symbole reçoit "
                                           "de ses voisins se lit ici, et ce qu'en fait l'égaliseur (Récepteur ▸ Égaliseur)."));
            return;
        }
    }
    switch (m_kind)
    {
        case Kind::Constellation: paintConstellation(p, area); break;
        case Kind::Eye: paintEye(p, area); break;
        case Kind::Ber: paintBer(p, area); break;
        case Kind::Sync: paintSync(p, area); break;
        case Kind::Channel: paintChannel(p, area); break;
    }
    paintHeader(p);
    if (m_hovering && !m_readout.isEmpty())
    {
        // At the bottom of the band under the pointer (the eye: I or Q; the synchronization: timing or carrier; the
        // channel: symbols or frequencies).
        QRectF band = area;
        for (const QRectF &lane : lanesOf(area))
            if (m_pointer.y() >= lane.top() && m_pointer.y() <= lane.bottom()) band = lane;
        const QFontMetricsF metrics(p.font());
        // On one line when all of it fits in the band; else what comes after the first part on a second line.
        QString first = m_readout, second;
        if (!m_readoutMore.isEmpty())
        {
            const QString whole = m_readout + QStringLiteral(" · ") + m_readoutMore;
            if (metrics.horizontalAdvance(whole) + 14.0 <= band.width() - 8.0)
                first = whole;
            else
                second = m_readoutMore;
        }
        const double w = std::min(band.width() - 8.0, std::max(metrics.horizontalAdvance(first), metrics.horizontalAdvance(second)) + 14.0);
        const double line = metrics.height();
        const double boxHeight = (second.isEmpty() ? line : 2.0 * line) + 6.0;
        // (The channel: under the band, over its title and, on two lines, its graduations: no stem starts there.)
        const double top = m_kind == Kind::Channel ? band.bottom() + (second.isEmpty() ? line + 2.0 : 2.0) : band.bottom() - boxHeight - 4.0;
        const QRectF box(band.left() + 4.0, top, w, boxHeight);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, second.isEmpty() ? 0.9 : 0.96));
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setPen(m_text);
        p.drawText(QRectF(box.left() + 7.0, box.top() + 3.0, box.width() - 14.0, line), Qt::AlignVCenter | Qt::AlignLeft,
                   metrics.elidedText(first, Qt::ElideRight, box.width() - 14.0));
        if (!second.isEmpty())
            p.drawText(QRectF(box.left() + 7.0, box.top() + 3.0 + line, box.width() - 14.0, line), Qt::AlignVCenter | Qt::AlignLeft,
                       metrics.elidedText(second, Qt::ElideRight, box.width() - 14.0));
    }
}

void ModulationPlot::paintMessage(QPainter &p, const QString &message)
{
    p.setPen(m_muted);
    p.drawText(boundingRect().adjusted(24.0, 0.0, -24.0, 0.0), Qt::AlignCenter | Qt::TextWordWrap, message);
}

void ModulationPlot::paintHeader(QPainter &p)
{
    const QFontMetricsF metrics(p.font());
    const double line = metrics.height() + 1.0;
    const double right = width() - kRight;
    double y = 3.0;
    p.setPen(m_muted);
    for (const HeaderRow &row : headerRows())
    {
        double left = 4.0;
        if (!row.left.isEmpty())
        {
            p.drawText(QRectF(left, y, std::max(0.0, right - left), line), Qt::AlignLeft | Qt::AlignVCenter,
                       metrics.elidedText(row.left, Qt::ElideRight, std::max(0.0, right - left)));
            left += metrics.horizontalAdvance(row.left) + 24.0;
        }
        if (!row.right.isEmpty() && right - left > 20.0)
            p.drawText(QRectF(left, y, right - left, line), Qt::AlignRight | Qt::AlignVCenter,
                       metrics.elidedText(row.right, Qt::ElideRight, right - left));
        y += line;
    }
}

void ModulationPlot::paintConstellation(QPainter &p, const QRectF &area)
{
    const analysis::LinkResult &r = *m_result;
    const QFontMetricsF metrics(p.font());
    const View v = currentView();
    const Plane pl = plane(area, v);
    const bool fsk = r.recipe.modulation == dsp::Modulation::Fsk;
    // What the plane shows (wider than the view along one axis when the area is not square).
    const double iLow = pl.iAt(area.left()), iHigh = pl.iAt(area.right());
    const double qLow = pl.qAt(area.bottom()), qHigh = pl.qAt(area.top());

    // Grid and graduations.
    p.setRenderHint(QPainter::Antialiasing, false);
    const plot::Ticks iTicks = plot::linearTicks(iLow, iHigh, area.width(), 70.0);
    const plot::Ticks qTicks = plot::linearTicks(qLow, qHigh, area.height(), 40.0);
    const plot::AxisLabels iLabels = plot::axisLabels(iTicks, "");
    const plot::AxisLabels qLabels = plot::axisLabels(qTicks, "");
    for (std::size_t k = 0; k < iTicks.major.size(); ++k)
    {
        const double x = std::round(pl.toScreen(iTicks.major[k], 0.0).x()) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        p.setPen(m_muted);
        const QString label = k < iLabels.text.size() ? text(iLabels.text[k]) : QString();
        p.drawText(QRectF(x - 40.0, area.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop, label);
    }
    for (std::size_t k = 0; k < qTicks.major.size(); ++k)
    {
        const double y = std::round(pl.toScreen(0.0, qTicks.major[k]).y()) + 0.5;
        if (y < area.top() || y > area.bottom()) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        p.setPen(m_muted);
        const QString label = k < qLabels.text.size() ? text(qLabels.text[k]) : QString();
        p.drawText(QRectF(0.0, y - metrics.height() / 2.0, area.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter, label);
    }
    p.setPen(QPen(faded(m_muted, 0.6), 1.0));
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    // The axes through 0.
    const QPointF zero = pl.toScreen(0.0, 0.0);
    if (zero.x() > area.left() && zero.x() < area.right()) p.drawLine(QPointF(zero.x(), area.top()), QPointF(zero.x(), area.bottom()));
    if (zero.y() > area.top() && zero.y() < area.bottom()) p.drawLine(QPointF(area.left(), zero.y()), QPointF(area.right(), zero.y()));
    p.setPen(m_muted);
    p.drawText(QRectF(area.left(), area.bottom() + 4.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop,
               fsk ? tr("Énergie à fc + Δf (un 1)") : tr("I (en phase), amplitude de la porteuse = 1"));

    p.save();
    p.setClipRect(area);
    p.setRenderHint(QPainter::Antialiasing, true);
    // The edges of the decision regions.
    QPen edge(faded(m_text, r.constellation.size() > 16 ? 0.25 : 0.45), 1.0, Qt::DashLine);
    p.setPen(edge);
    const double far = 4.0 * std::max(v.x.span(), v.y.span());
    const auto line = [&](double i1, double q1, double i2, double q2) { p.drawLine(pl.toScreen(i1, q1), pl.toScreen(i2, q2)); };
    const dsp::Modulation m = r.recipe.modulation;
    switch (m)
    {
        case dsp::Modulation::Bpsk: line(0.0, -far, 0.0, far); break;
        case dsp::Modulation::Qpsk:
            line(0.0, -far, 0.0, far);
            line(-far, 0.0, far, 0.0);
            break;
        case dsp::Modulation::Psk8:
            for (int j = 0; j < 8; ++j)
            {
                const double a = (2.0 * j + 1.0) * std::numbers::pi / 8.0;
                line(0.0, 0.0, far * std::cos(a), far * std::sin(a));
            }
            break;
        case dsp::Modulation::Ask:
        {
            const double threshold = 0.5 * (1.0 + r.recipe.askLowLevel);
            line(threshold, -far, threshold, far);
            break;
        }
        case dsp::Modulation::Fsk: line(0.0, 0.0, far, far); break;
        default:
        {
            // Square QAM: between the levels of each axis.
            const std::vector<double> levels = levelsOf(r.constellation, false);
            for (std::size_t k = 0; k + 1 < levels.size(); ++k)
            {
                const double b = 0.5 * (levels[k] + levels[k + 1]);
                line(b, -far, b, far);
                line(-far, b, far, b);
            }
            break;
        }
    }
    if (m == dsp::Modulation::Qpsk || m == dsp::Modulation::Psk8)
    {
        p.setPen(QPen(faded(m_muted, 0.5), 1.0, Qt::DotLine));
        const QPointF c = pl.toScreen(0.0, 0.0);
        p.drawEllipse(c, pl.scale, pl.scale);
    }

    const std::size_t sent = r.sentSymbols.size();
    const std::size_t n = std::min(r.reception.decisionPoints.size(), std::min(sent, r.reception.symbols.size()));
    const std::size_t first = std::min(r.skipSymbols, n), last = std::min(n, sent > r.skipEndSymbols ? sent - r.skipEndSymbols : 0);
    // When asked, under the rest: the points before the equalizer (what the receiver would decide without it), else
    // before synchronization (what a receiver that only found the frame would decide, the carrier not turned back,
    // the clock not followed).
    if (const RawPoints before = rawShown() ? rawPointsOf(r) : RawPoints{}; before.points != nullptr)
    {
        const std::vector<std::complex<double>> &points = *before.points;
        const std::size_t end = std::min(last, points.size());
        QPolygonF raw;
        raw.reserve(static_cast<qsizetype>(std::min(m_shown, kMaximumPoints)));
        for (std::size_t k = first; k < end; k += m_pointStep) raw << pl.toScreen(points[k].real() / before.gain, points[k].imag() / before.gain);
        p.setPen(QPen(faded(m_muted, m_shown > 5000 ? 0.45 : 0.65), m_shown > 5000 ? 2.0 : 2.6, Qt::SolidLine, Qt::RoundCap));
        p.drawPoints(raw);
    }
    // The points received: right ones first, those decided wrong on top.
    QPolygonF right, wrong;
    right.reserve(static_cast<qsizetype>(std::min(m_shown, kMaximumPoints)));
    for (std::size_t k = first; k < last; ++k)
    {
        if (r.sentSymbols[k] != r.reception.symbols[k])
            wrong << pl.toScreen(r.reception.decisionPoints[k].real(), r.reception.decisionPoints[k].imag());
        else if ((k - first) % m_pointStep == 0)
            right << pl.toScreen(r.reception.decisionPoints[k].real(), r.reception.decisionPoints[k].imag());
    }
    const double dot = m_shown > 5000 ? 2.2 : 3.0;
    p.setPen(QPen(faded(m_accent, m_shown > 5000 ? 0.35 : 0.55), dot, Qt::SolidLine, Qt::RoundCap));
    p.drawPoints(right);
    p.setPen(QPen(m_warning, dot + 0.8, Qt::SolidLine, Qt::RoundCap));
    p.drawPoints(wrong);

    // The points of the constellation, with their bits when there is room (the least distance between two points
    // wide enough for them).
    const int bps = dsp::bitsPerSymbol(m);
    double spacing = 2.0;
    switch (m)
    {
        case dsp::Modulation::Ask: spacing = 1.0 - r.recipe.askLowLevel; break;
        case dsp::Modulation::Fsk:
        case dsp::Modulation::Qpsk: spacing = std::sqrt(2.0); break;
        case dsp::Modulation::Psk8: spacing = 2.0 * std::sin(std::numbers::pi / 8.0); break;
        case dsp::Modulation::Qam16:
        case dsp::Modulation::Qam64:
        case dsp::Modulation::Qam256: spacing = 2.0 / std::sqrt(2.0 * (static_cast<double>(r.constellation.size()) - 1.0) / 3.0); break;
        default: break;
    }
    const bool labels = spacing * pl.scale >= metrics.horizontalAdvance(QStringLiteral("0")) * (bps + 1.5);
    // A ring no wider than a fifth of the room between two points (256-QAM: a few pixels), hollow: the points
    // received show through it.
    const double ring = std::clamp(0.22 * spacing * pl.scale, 1.5, 4.5);
    p.setBrush(Qt::NoBrush);
    for (std::size_t s = 0; s < r.constellation.size(); ++s)
    {
        const QPointF at = pl.toScreen(r.constellation[s].real(), r.constellation[s].imag());
        p.setPen(QPen(m_text, ring < 2.5 ? 1.0 : 1.4));
        p.drawEllipse(at, ring, ring);
        if (!labels) continue;
        const QString bits = fsk ? QString::number(static_cast<int>(s)) : bitsOf(s, bps);
        const double w = metrics.horizontalAdvance(bits);
        const QRectF box(at.x() - w / 2.0 - 3.0, at.y() - ring - 3.0 - metrics.height(), w + 6.0, metrics.height());
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.75));
        p.drawRoundedRect(box, 3.0, 3.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(box, Qt::AlignCenter, bits);
    }
    p.restore();
}

void ModulationPlot::paintEye(QPainter &p, const QRectF &area)
{
    const analysis::LinkResult &r = *m_result;
    const QFontMetricsF metrics(p.font());
    const View v = currentView();
    const std::vector<QRectF> lanes = eyeLanes(area);
    // The rate of the filter output: the record's, or the synchronized receiver's own (8 samples per symbol).
    const double fs = r.reception.matchedRateHz > 0.0 ? r.reception.matchedRateHz : r.recipe.sampleRateHz;
    // Few samples per symbol of a band-limited output: the curve between them (a cubic through four samples), not
    // straight strokes.
    const double perSymbol = fs / r.recipe.symbolRate;
    const bool bandLimited = r.sync.used || r.recipe.pulse == dsp::PulseShape::RootRaisedCosine;
    const int sub = bandLimited && perSymbol < 24.0 ? static_cast<int>(std::ceil(24.0 / std::max(1.0, perSymbol))) : 1;
    const auto xOf = [&](double seconds) { return area.left() + (seconds - v.x.low) / v.x.span() * area.width(); };

    // Time axis (under the last band).
    p.setRenderHint(QPainter::Antialiasing, false);
    const plot::Ticks ticks = plot::linearTicks(v.x.low, v.x.high, area.width(), 80.0);
    const plot::AxisLabels labels = plot::axisLabels(ticks, "s");
    const QRectF bottomLane = lanes.back();
    for (std::size_t k = 0; k < ticks.major.size(); ++k)
    {
        const double x = std::round(xOf(ticks.major[k])) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        p.setPen(QPen(m_grid, 1.0));
        for (const QRectF &lane : lanes) p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
        p.setPen(m_muted);
        const QString label = k < labels.text.size() ? text(labels.text[k]) : QString();
        p.drawText(QRectF(x - 40.0, bottomLane.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop, label);
    }
    QString title = tr("Temps autour de l'instant de décision");
    if (!labels.unit.empty()) title += QStringLiteral(" (%1)").arg(text(labels.unit));
    p.setPen(m_muted);
    p.drawText(QRectF(area.left(), bottomLane.bottom() + 4.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, title);

    const std::size_t sent = r.sentSymbols.size();
    const std::size_t n = std::min(r.reception.decisionTimes.size(), sent);
    const std::size_t first = std::min(r.skipSymbols, n), last = std::min(n, sent > r.skipEndSymbols ? sent - r.skipEndSymbols : 0);
    const std::size_t traces = m_traces;
    const std::size_t step = m_traceStep;
    for (std::size_t laneIndex = 0; laneIndex < lanes.size(); ++laneIndex)
    {
        const QRectF lane = lanes[laneIndex];
        const bool quadrature = laneIndex == 1;
        const std::vector<double> &y = quadrature ? r.reception.matchedQ : r.reception.matchedI;
        const plot::YMap map{v.y.low, v.y.high, lane.top(), lane.height(), false};
        p.setRenderHint(QPainter::Antialiasing, false);
        const plot::Ticks yTicks = plot::linearTicks(v.y.low, v.y.high, lane.height(), 28.0);
        const plot::AxisLabels yLabels = plot::axisLabels(yTicks, "");
        for (std::size_t k = 0; k < yTicks.major.size(); ++k)
        {
            const double py = std::round(map.toPixel(yTicks.major[k])) + 0.5;
            if (py < lane.top() || py > lane.bottom()) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(lane.left(), py), QPointF(lane.right(), py));
            p.setPen(m_muted);
            const QString label = k < yLabels.text.size() ? text(yLabels.text[k]) : QString();
            p.drawText(QRectF(0.0, py - metrics.height() / 2.0, lane.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter, label);
        }
        p.setPen(QPen(faded(m_muted, 0.6), 1.0));
        p.drawRect(lane.adjusted(0.5, 0.5, -0.5, -0.5));

        p.save();
        p.setClipRect(lane);
        p.setRenderHint(QPainter::Antialiasing, true);
        // The traces, two symbols about each decision.
        p.setPen(QPen(faded(m_accent, traces > 100 ? 0.22 : 0.45), 1.0));
        const double from = v.x.low * fs, to = v.x.high * fs;
        QPolygonF trace;
        for (std::size_t k = first; k < last; k += step)
        {
            const double t = r.reception.decisionTimes[k];
            const double i0 = std::max(0.0, std::ceil(t + from - 1.0)), i1 = std::min(static_cast<double>(y.size()) - 1.0, std::floor(t + to + 1.0));
            if (!(i1 > i0)) continue;
            trace.clear();
            const auto add = [&](double sample, double value) {
                trace << QPointF(xOf((sample - t) / fs), std::clamp(map.toPixel(value), lane.top() - 2.0 * lane.height(), lane.bottom() + 2.0 * lane.height()));
            };
            const auto end = static_cast<std::size_t>(i1);
            for (auto i = static_cast<std::size_t>(i0); i <= end; ++i)
            {
                add(static_cast<double>(i), y[i]);
                if (sub <= 1 || i == end) continue;
                const double y0 = y[i > 0 ? i - 1 : i], y1 = y[i], y2 = y[i + 1], y3 = y[std::min(i + 2, y.size() - 1)];
                for (int j = 1; j < sub; ++j)
                {
                    const double u = static_cast<double>(j) / static_cast<double>(sub);
                    add(static_cast<double>(i) + u, 0.5 * (2.0 * y1 + (y2 - y0) * u + (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3) * u * u +
                                                           (3.0 * (y1 - y2) + y3 - y0) * u * u * u));
                }
            }
            p.drawPolyline(trace);
        }
        // The levels of the constellation, and the decision instant.
        p.setPen(QPen(faded(m_text, 0.55), 1.0, Qt::DotLine));
        for (double level : levelsOf(r.constellation, quadrature))
        {
            const double py = map.toPixel(level);
            p.drawLine(QPointF(lane.left(), py), QPointF(lane.right(), py));
        }
        p.setPen(QPen(faded(m_text, 0.7), 1.0, Qt::DashLine));
        const double x0 = xOf(0.0);
        p.drawLine(QPointF(x0, lane.top()), QPointF(x0, lane.bottom()));
        p.restore();
        // The band's name.
        const QString name = lanes.size() > 1 ? (quadrature ? tr("Q") : tr("I")) : tr("I");
        const QRectF tag(lane.left() + 6.0, lane.top() + 5.0, metrics.horizontalAdvance(name) + 12.0, metrics.height() + 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.85));
        p.drawRoundedRect(tag, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(tag, Qt::AlignCenter, name);
    }
}

void ModulationPlot::paintSync(QPainter &p, const QRectF &area)
{
    const analysis::LinkResult &r = *m_result;
    const analysis::LinkSync &sync = r.sync;
    const QFontMetricsF metrics(p.font());
    const View v = currentView();
    const std::vector<QRectF> lanes = syncLanes(area);
    const auto xOf = [&](double symbol) { return area.left() + (symbol - v.x.low) / v.x.span() * area.width(); };
    const double preamble = static_cast<double>(std::min(r.recipe.preambleSymbols, sync.timingError.size()));
    const double symmetry = dsp::rotationalSymmetryDegrees(r.recipe.modulation);

    // The symbols (under the last band).
    p.setRenderHint(QPainter::Antialiasing, false);
    const plot::Ticks ticks = plot::linearTicks(v.x.low, v.x.high, area.width(), 80.0);
    const plot::AxisLabels labels = plot::axisLabels(ticks, "");
    const QRectF bottomLane = lanes.back();
    for (std::size_t k = 0; k < ticks.major.size(); ++k)
    {
        const double x = std::round(xOf(ticks.major[k])) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        p.setPen(QPen(m_grid, 1.0));
        for (const QRectF &lane : lanes) p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
        p.setPen(m_muted);
        const QString label = k < labels.text.size() ? text(labels.text[k]) : QString();
        p.drawText(QRectF(x - 40.0, bottomLane.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop, label);
    }
    QString title = tr("Symbole, depuis le début de la trame");
    if (!labels.unit.empty()) title += QStringLiteral(" (%1)").arg(text(labels.unit));
    p.setPen(m_muted);
    p.drawText(QRectF(area.left(), bottomLane.bottom() + 4.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, title);

    const plot::XWindow window{v.x.low, v.x.span() / area.width(), static_cast<int>(std::ceil(area.width()))};
    for (std::size_t laneIndex = 0; laneIndex < lanes.size(); ++laneIndex)
    {
        const QRectF lane = lanes[laneIndex];
        const bool carrier = laneIndex == 1;
        const std::vector<double> &values = carrier ? m_syncPhase : sync.timingError;
        const Range range = carrier ? v.y2 : v.y;
        const plot::YMap map{range.low, range.high, lane.top(), lane.height(), false};
        p.setRenderHint(QPainter::Antialiasing, false);
        // The preamble: the symbols the receiver knows, where its loops settle.
        const double shadeFrom = std::max(lane.left(), xOf(-0.5)), shadeTo = std::min(lane.right(), xOf(preamble - 0.5));
        if (shadeTo > shadeFrom) p.fillRect(QRectF(shadeFrom, lane.top(), shadeTo - shadeFrom, lane.height()), faded(m_muted, 0.14));
        const plot::Ticks yTicks = carrier ? degreeTicks(range.low, range.high, lane.height(), 28.0)
                                           : plot::linearTicks(range.low, range.high, lane.height(), 28.0);
        const plot::AxisLabels yLabels = plot::axisLabels(yTicks, "");
        for (std::size_t k = 0; k < yTicks.major.size(); ++k)
        {
            const double py = std::round(map.toPixel(yTicks.major[k])) + 0.5;
            if (py < lane.top() || py > lane.bottom()) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(lane.left(), py), QPointF(lane.right(), py));
            p.setPen(m_muted);
            const QString label = k < yLabels.text.size() ? text(yLabels.text[k]) : QString();
            p.drawText(QRectF(0.0, py - metrics.height() / 2.0, lane.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter, label);
        }
        p.setPen(QPen(faded(m_muted, 0.6), 1.0));
        p.drawRect(lane.adjusted(0.5, 0.5, -0.5, -0.5));

        p.save();
        p.setClipRect(lane);
        // No error: the line through 0.
        const double zero = std::round(map.toPixel(0.0)) + 0.5;
        if (zero > lane.top() && zero < lane.bottom())
        {
            p.setPen(QPen(faded(m_text, 0.5), 1.0));
            p.drawLine(QPointF(lane.left(), zero), QPointF(lane.right(), zero));
        }
        // The carrier: the other symmetries of the constellation, where a slip lands (when they are a few pixels apart).
        if (carrier && symmetry > 0.0 && range.span() > 0.75 * symmetry && symmetry / range.span() * lane.height() >= 6.0)
        {
            p.setPen(QPen(faded(m_warning, 0.75), 1.0, Qt::DashLine));
            for (double a = std::ceil(range.low / symmetry) * symmetry; a <= range.high; a += symmetry)
            {
                if (std::abs(a) < 1e-9) continue;
                const double py = std::round(map.toPixel(a)) + 0.5;
                p.drawLine(QPointF(lane.left(), py), QPointF(lane.right(), py));
            }
        }
        // Where the preamble ends.
        const double edge = std::round(xOf(preamble - 0.5)) + 0.5;
        if (preamble > 0.0 && edge > lane.left() && edge < lane.right())
        {
            p.setPen(QPen(faded(m_text, 0.6), 1.0, Qt::DashLine));
            p.drawLine(QPointF(edge, lane.top()), QPointF(edge, lane.bottom()));
        }
        // The error, symbol by symbol (many per pixel: their extremes, no peak lost).
        const plot::Trace trace = plot::buildTrace(plot::ChannelView{values, 0.0, {}, nullptr}, window);
        p.setRenderHint(QPainter::Antialiasing, !trace.dense);
        p.setPen(QPen(m_accent, trace.dense ? 1.0 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double lowest = lane.top() - 2.0 * lane.height(), highest = lane.bottom() + 2.0 * lane.height();
        for (const std::vector<plot::PixelPoint> &line : plot::traceLines(trace, map))
        {
            if (line.size() == 1)
            {
                p.drawPoint(QPointF(area.left() + static_cast<double>(line[0].x), std::clamp(static_cast<double>(line[0].y), lowest, highest)));
                continue;
            }
            QPolygonF polygon;
            polygon.reserve(static_cast<qsizetype>(line.size()));
            for (const plot::PixelPoint &point : line)
                polygon << QPointF(area.left() + static_cast<double>(point.x), std::clamp(static_cast<double>(point.y), lowest, highest));
            p.drawPolyline(polygon);
        }
        p.restore();
        p.setRenderHint(QPainter::Antialiasing, true);
        // The band's name (and the factor of its graduations, if any), as the eye names its own; « préambule » over
        // its shade when it fits beside.
        QString name = carrier ? tr("Porteuse (°)") : tr("Rythme (symbole)");
        if (!yLabels.unit.empty()) name += QStringLiteral(" %1").arg(text(yLabels.unit));
        const double nameWidth = std::min(metrics.horizontalAdvance(name) + 12.0, lane.width() - 12.0);
        const QRectF tag(lane.left() + 6.0, lane.top() + 5.0, nameWidth, metrics.height() + 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.85));
        p.drawRoundedRect(tag, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(tag, Qt::AlignCenter, metrics.elidedText(name, Qt::ElideRight, nameWidth - 8.0));
        const QString known = tr("préambule");
        const double knownFrom = std::max(shadeFrom, tag.right() + 6.0);
        if (laneIndex == 0 && shadeTo - knownFrom >= metrics.horizontalAdvance(known) + 10.0)
        {
            p.setPen(m_muted);
            p.drawText(QRectF(knownFrom, tag.top(), shadeTo - knownFrom, tag.height()), Qt::AlignCenter, known);
        }
    }
}

void ModulationPlot::paintChannel(QPainter &p, const QRectF &area)
{
    const analysis::LinkResult &r = *m_result;
    const analysis::LinkEqualization &q = r.equalization;
    const QFontMetricsF metrics(p.font());
    const View v = currentView();
    const std::vector<QRectF> lanes = channelLanes(area);
    if (lanes.front().height() < 24.0) return;
    // What is drawn, in this order and in the colours of their names above the frame: the channel, its equalizer,
    // both together; then the paths themselves.
    const std::vector<LegendEntry> entries = channelLegend();
    struct Shown
    {
        const TapSeries *series;
        QColor colour;
    };
    std::vector<Shown> shown;
    bool diamonds = false;
    for (const LegendEntry &entry : entries)
    {
        if (entry.diamond)
            diamonds = true;
        else
            shown.push_back({shown.empty() ? &m_channel : shown.size() == 1 ? &m_equalizer : &m_combined, entry.colour});
    }
    if (shown.empty()) return;
    // Their names, on the lines kept for them above the frame.
    bool beside = false;
    const std::vector<std::vector<LegendEntry>> legend = legendRows(beside);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t row = 0; row < legend.size(); ++row)
    {
        double total = 0.0;
        for (const LegendEntry &entry : legend[row]) total += 14.0 + metrics.horizontalAdvance(entry.name) + 12.0;
        const double line = metrics.height() + 1.0;
        const double mid = 3.0 + (static_cast<double>(row) + (beside ? 0.5 : 1.5)) * line;
        double x = std::max(4.0, width() - kRight - total + 12.0);
        for (const LegendEntry &entry : legend[row])
        {
            if (entry.diamond)
            {
                QPolygonF diamond;
                diamond << QPointF(x + 4.0, mid - 4.5) << QPointF(x + 8.5, mid) << QPointF(x + 4.0, mid + 4.5) << QPointF(x - 0.5, mid);
                p.setPen(QPen(entry.colour, 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(diamond);
            }
            else
            {
                p.setPen(Qt::NoPen);
                p.setBrush(entry.colour);
                p.drawEllipse(QPointF(x + 4.0, mid), 3.5, 3.5);
                p.setBrush(Qt::NoBrush);
            }
            x += 14.0;
            p.setPen(m_text);
            p.drawText(QRectF(x, mid - 0.5 * line, metrics.horizontalAdvance(entry.name) + 2.0, line), Qt::AlignLeft | Qt::AlignVCenter, entry.name);
            x += metrics.horizontalAdvance(entry.name) + 12.0;
        }
    }

    // The frame of a band: the graduations of its values on the left, its grid.
    const auto frame = [&](const QRectF &lane, const Range &range, const plot::YMap &map) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const plot::Ticks ticks = plot::linearTicks(range.low, range.high, lane.height(), 22.0);
        const plot::AxisLabels labels = plot::axisLabels(ticks, "");
        for (std::size_t k = 0; k < ticks.major.size(); ++k)
        {
            const double py = std::round(map.toPixel(ticks.major[k])) + 0.5;
            if (py < lane.top() || py > lane.bottom()) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(lane.left(), py), QPointF(lane.right(), py));
            p.setPen(m_muted);
            const QString label = k < labels.text.size() ? text(labels.text[k]) : QString();
            p.drawText(QRectF(0.0, py - metrics.height() / 2.0, lane.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter, label);
        }
        p.setPen(QPen(faded(m_muted, 0.6), 1.0));
        p.drawRect(lane.adjusted(0.5, 0.5, -0.5, -0.5));
        return labels.unit;
    };
    // What the values of a band are, top left; under the band, what its horizontal axis is.
    const auto tagOf = [&](const QRectF &lane, const QString &name) {
        const double w = std::min(metrics.horizontalAdvance(name) + 12.0, lane.width() - 12.0);
        const QRectF tag(lane.left() + 6.0, lane.top() + 5.0, w, metrics.height() + 2.0);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.85));
        p.drawRoundedRect(tag, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(tag, Qt::AlignCenter, metrics.elidedText(name, Qt::ElideRight, w - 8.0));
    };
    const auto titleUnder = [&](const QRectF &lane, const QString &title) {
        // (Not while the pointer reads this band: what is under it is written there.)
        if (m_hovering && !m_readout.isEmpty() && m_pointer.y() >= lane.top() && m_pointer.y() <= lane.bottom()) return;
        p.setPen(m_muted);
        p.drawText(QRectF(lane.left(), lane.bottom() + 4.0 + metrics.height(), lane.width(), metrics.height() + 2.0),
                   Qt::AlignHCenter | Qt::AlignTop, metrics.elidedText(title, Qt::ElideRight, lane.width()));
    };

    // ---- Symbol by symbol: the modulus of each tap, a stem ----
    {
        const QRectF lane = lanes[0];
        const plot::YMap map{v.y.low, v.y.high, lane.top(), lane.height(), false};
        const auto xOf = [&](double symbol) { return lane.left() + (symbol - v.x.low) / v.x.span() * lane.width(); };
        p.setRenderHint(QPainter::Antialiasing, false);
        // Whole symbols only, under this band.
        const plot::Ticks ticks = plot::linearTicks(v.x.low, v.x.high, lane.width(), 46.0);
        for (double symbol : ticks.major)
        {
            if (std::abs(symbol - std::round(symbol)) > 1e-6) continue;
            const double x = std::round(xOf(symbol)) + 0.5;
            if (x < lane.left() || x > lane.right()) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
            p.setPen(m_muted);
            p.drawText(QRectF(x - 40.0, lane.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop,
                       text(plot::formatCompact(std::round(symbol) == 0.0 ? 0.0 : std::round(symbol), "", 6)));
        }
        const std::string unit = frame(lane, v.y, map);

        p.save();
        p.setClipRect(lane);
        const double base = map.toPixel(0.0);
        const double lowest = lane.top() - lane.height(), highest = lane.bottom() + lane.height();
        // Close stems when a symbol has few pixels: thinner, their heads smaller.
        const double perSymbol = lane.width() / v.x.span();
        const bool dense = perSymbol < 14.0;
        const double head = dense ? 1.8 : 3.0;
        // (The channel on the graduation of its symbol, as the paths are; the equalizer and both beside it.)
        const auto offsetOf = [](std::size_t index) { return static_cast<double>(index) * kTapSpread; };
        for (std::size_t index = 0; index < shown.size(); ++index)
        {
            const TapSeries &series = *shown[index].series;
            const double offset = offsetOf(index);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(shown[index].colour, dense ? 1.0 : 2.0, Qt::SolidLine, Qt::FlatCap));
            std::vector<QPointF> heads;
            for (std::size_t k = 0; k < series.taps.size(); ++k)
            {
                const double symbol = static_cast<double>(series.first) + static_cast<double>(k);
                if (symbol < v.x.low - 1.0 || symbol > v.x.high + 1.0) continue;
                const double x = std::round(xOf(symbol + offset)) + (dense ? 0.5 : 0.0);
                const double y = std::clamp(map.toPixel(std::abs(series.taps[k])), lowest, highest);
                p.drawLine(QPointF(x, base), QPointF(x, y));
                heads.emplace_back(x, y);
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(shown[index].colour);
            for (const QPointF &at : heads) p.drawEllipse(at, head, head);
            p.setBrush(Qt::NoBrush);
        }
        // The paths themselves: where each arrives against the decision instant (which the ideal receiver puts on
        // the strongest, the synchronized one where its timing loop settled), how strong against the symbol as the
        // decisions get it (a diamond, with the stems of the channel).
        if (diamonds)
        {
            const double decision = q.typed[q.mainPath].delaySymbols + q.instantSymbols;
            const double symbol = q.amplitude[q.mainPath] * (q.symbolGain > 0.0 ? q.symbolGain : 1.0);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(m_muted, 1.2));
            for (std::size_t k = 0; k < q.typed.size(); ++k)
            {
                const double x = xOf(q.typed[k].delaySymbols - decision);
                const double y = std::clamp(map.toPixel(q.amplitude[k] / symbol), lowest, highest);
                QPolygonF diamond;
                diamond << QPointF(x, y - 6.0) << QPointF(x + 6.0, y) << QPointF(x, y + 6.0) << QPointF(x - 6.0, y);
                p.drawPolygon(diamond);
            }
        }
        // No tap: the line through 0.
        p.setRenderHint(QPainter::Antialiasing, false);
        const double zero = std::round(base) + 0.5;
        if (zero > lane.top() && zero < lane.bottom())
        {
            p.setPen(QPen(faded(m_text, 0.5), 1.0));
            p.drawLine(QPointF(lane.left(), zero), QPointF(lane.right(), zero));
        }
        p.restore();

        titleUnder(lane, tr("Retard, en symboles (aux instants de décision)"));
        QString name = tr("Module");
        if (!unit.empty()) name += QStringLiteral(" %1").arg(text(unit));
        tagOf(lane, name);
    }

    // ---- Along the frequency: their gains, over one symbol rate about the carrier (beyond, they repeat) ----
    {
        const QRectF lane = lanes[1];
        const double rate = r.recipe.symbolRate;
        const plot::YMap map{v.y2.low, v.y2.high, lane.top(), lane.height(), false};
        const auto xOf = [&](double hertz) { return lane.left() + (hertz - v.x2.low) / v.x2.span() * lane.width(); };
        p.setRenderHint(QPainter::Antialiasing, false);
        const plot::Ticks ticks = plot::linearTicks(v.x2.low, v.x2.high, lane.width(), 70.0);
        const plot::AxisLabels labels = plot::axisLabels(ticks, "Hz");
        for (std::size_t k = 0; k < ticks.major.size(); ++k)
        {
            // (Half the symbol rate falls on the edges of the band: both ends keep their graduation.)
            const double x = std::round(xOf(ticks.major[k])) + 0.5;
            if (x < lane.left() - 1.0 || x > lane.right() + 1.0) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
            p.setPen(m_muted);
            const QString label = k < labels.text.size() ? text(labels.text[k]) : QString();
            p.drawText(QRectF(x - 40.0, lane.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop, label);
        }
        QString title = r.recipe.representation == dsp::Representation::ComplexBaseband ? tr("Fréquence") : tr("Fréquence autour de la porteuse");
        if (!labels.unit.empty()) title += QStringLiteral(" (%1)").arg(text(labels.unit));
        titleUnder(lane, title);
        const std::string unit = frame(lane, v.y2, map);

        p.save();
        p.setClipRect(lane);
        // Neither gain nor loss: the line through 0 dB; half the symbol rate each side of the carrier, when seen.
        const double zero = std::round(map.toPixel(0.0)) + 0.5;
        if (zero > lane.top() && zero < lane.bottom())
        {
            p.setPen(QPen(faded(m_text, 0.5), 1.0));
            p.drawLine(QPointF(lane.left(), zero), QPointF(lane.right(), zero));
        }
        p.setPen(QPen(faded(m_text, 0.6), 1.0, Qt::DashLine));
        for (double edge : {-0.5 * rate, 0.5 * rate})
        {
            const double x = std::round(xOf(edge)) + 0.5;
            if (x > lane.left() + 2.0 && x < lane.right() - 2.0) p.drawLine(QPointF(x, lane.top()), QPointF(x, lane.bottom()));
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        const double lowest = lane.top() - lane.height(), highest = lane.bottom() + lane.height();
        const int columns = std::max(2, static_cast<int>(std::ceil(lane.width() / 2.0)));
        for (const Shown &s : shown)
        {
            QPolygonF line;
            line.reserve(columns + 1);
            for (int column = 0; column <= columns; ++column)
            {
                const double x = lane.left() + lane.width() * static_cast<double>(column) / static_cast<double>(columns);
                const double hertz = v.x2.low + v.x2.span() * static_cast<double>(column) / static_cast<double>(columns);
                line << QPointF(x, std::clamp(map.toPixel(gainAt(s.series->gainDb, hertz / rate)), lowest, highest));
            }
            p.setPen(QPen(s.colour, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(line);
        }
        p.restore();
        QString name = tr("Gain (dB)");
        if (!unit.empty()) name += QStringLiteral(" %1").arg(text(unit));
        tagOf(lane, name);
    }
}

void ModulationPlot::paintBer(QPainter &p, const QRectF &area)
{
    const qacq::app::ModulationController *controller = m_modulation.data();
    const QFontMetricsF metrics(p.font());
    const View v = currentView();
    const plot::YMap map{v.y.low, v.y.high, area.top(), area.height(), false};
    const auto xOf = [&](double eb) { return area.left() + (eb - v.x.low) / v.x.span() * area.width(); };
    const auto yOf = [&](double log10rate) { return std::clamp(map.toPixel(log10rate), area.top() - 4.0 * area.height(), area.bottom() + 4.0 * area.height()); };

    // Eb/N0 axis.
    p.setRenderHint(QPainter::Antialiasing, false);
    const plot::Ticks ticks = plot::linearTicks(v.x.low, v.x.high, area.width(), 60.0);
    const plot::AxisLabels labels = plot::axisLabels(ticks, "");
    for (std::size_t k = 0; k < ticks.major.size(); ++k)
    {
        const double x = std::round(xOf(ticks.major[k])) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        p.setPen(m_muted);
        const QString label = k < labels.text.size() ? text(labels.text[k]) : QString();
        p.drawText(QRectF(x - 40.0, area.bottom() + 3.0, 80.0, metrics.height()), Qt::AlignHCenter | Qt::AlignTop, label);
    }
    p.setPen(m_muted);
    p.drawText(QRectF(area.left(), area.bottom() + 4.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, tr("Eb/N0 (dB)"));
    // Decades of the rate, and their 2 … 9 when there is room.
    const double perDecade = area.height() / v.y.span();
    for (int exponent = static_cast<int>(std::floor(v.y.low)); exponent <= static_cast<int>(std::ceil(v.y.high)); ++exponent)
    {
        const double y = std::round(map.toPixel(exponent)) + 0.5;
        if (y >= area.top() && y <= area.bottom())
        {
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(m_muted);
            p.drawText(QRectF(0.0, y - metrics.height() / 2.0, area.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter,
                       decade(exponent));
        }
        if (perDecade < 60.0) continue;
        p.setPen(QPen(faded(m_grid, 0.5), 1.0));
        for (int k = 2; k <= 9; ++k)
        {
            const double ym = std::round(map.toPixel(exponent + std::log10(static_cast<double>(k)))) + 0.5;
            if (ym >= area.top() && ym <= area.bottom()) p.drawLine(QPointF(area.left(), ym), QPointF(area.right(), ym));
        }
    }
    p.setPen(QPen(faded(m_muted, 0.6), 1.0));
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    if (!controller) return;

    p.save();
    p.setClipRect(area.adjusted(-1.0, -1.0, 1.0, 1.0));
    p.setRenderHint(QPainter::Antialiasing, true);
    const int columns = std::clamp(static_cast<int>(area.width() / 3.0), 16, 400);
    const auto drawTheory = [&](const analysis::LinkRecipe &recipe, const QPen &pen) {
        const std::vector<double> &values = theory(recipe, v.x, columns);
        QPolygonF line;
        for (std::size_t k = 0; k < values.size(); ++k)
        {
            if (values[k] <= kFloorLog + 1.0) break;
            const double eb = v.x.low + v.x.span() * static_cast<double>(k) / static_cast<double>(values.size() - 1);
            line << QPointF(xOf(eb), yOf(values[k]));
        }
        p.setPen(pen);
        p.drawPolyline(line);
    };

    QStringList legend;
    std::vector<QColor> legendColours;
    // The theory of the link as it is, dashed.
    const analysis::LinkRecipe now = controller->currentRecipe();
    const auto &curves = controller->curves();
    const bool nowHasCurve = std::any_of(curves.begin(), curves.end(), [&now](const auto &c) { return sameTheory(c.recipe, now); });
    if (!nowHasCurve && !std::isnan(analysis::linkTheoryBer(now, now.ebN0Db)))
    {
        drawTheory(now, QPen(faded(m_muted, 0.9), 1.4, Qt::DashLine));
        legend << tr("Théorie %1").arg(theoryName(now));
        legendColours.push_back(m_muted);
    }
    // Each curve: its theory, its points with their interval.
    for (std::size_t c = 0; c < curves.size(); ++c)
    {
        const QColor colour = curveColour(c);
        drawTheory(curves[c].recipe, QPen(faded(colour, 0.85), 1.6));
        p.setPen(QPen(colour, 1.4));
        for (const analysis::BerPoint &point : curves[c].points)
        {
            const double x = xOf(point.ebN0Db);
            if (point.errors > 0)
            {
                p.drawLine(QPointF(x, yOf(log10Rate(point.lower))), QPointF(x, yOf(log10Rate(point.upper))));
                p.setBrush(colour);
                p.drawEllipse(QPointF(x, yOf(log10Rate(point.ber))), 3.6, 3.6);
                p.setBrush(Qt::NoBrush);
            }
            else
            {
                // No error: under its upper bound.
                const double y = yOf(log10Rate(point.upper));
                QPolygonF triangle;
                triangle << QPointF(x - 5.0, y - 3.0) << QPointF(x + 5.0, y - 3.0) << QPointF(x, y + 5.0);
                p.drawPolygon(triangle);
            }
        }
        legend << QString::fromStdString(curves[c].label) +
                      (curves[c].done ? QString() : (c + 1 == curves.size() && controller->curveBusy() ? tr(" (en cours…)") : tr(" (arrêtée)")));
        legendColours.push_back(colour);
    }
    // The link as it is: its point.
    const analysis::LinkResult *r = m_result.get();
    if (r && r->ok() && r->recipe.noise)
    {
        const double lower = r->berLower, upper = r->berUpper; // (Wilson's; through fading, widened by blocks)
        const double x = xOf(r->recipe.ebN0Db);
        const double y = yOf(log10Rate(r->bitErrors > 0 ? r->ber : upper));
        p.setPen(QPen(m_text, 1.4));
        if (r->bitErrors > 0) p.drawLine(QPointF(x, yOf(log10Rate(lower))), QPointF(x, yOf(log10Rate(upper))));
        QPolygonF diamond;
        diamond << QPointF(x, y - 6.0) << QPointF(x + 6.0, y) << QPointF(x, y + 6.0) << QPointF(x - 6.0, y);
        if (r->bitErrors > 0)
            p.setBrush(m_text);
        else
            p.setBrush(Qt::NoBrush);
        p.drawPolygon(diamond);
        p.setBrush(Qt::NoBrush);
        legend << (r->bitErrors > 0 ? tr("◆ La liaison : %1 à %2 dB").arg(rate(r->ber))
                                    : tr("◇ La liaison : aucune erreur à %2 dB (moins de %1)").arg(rate(upper)))
                      .arg(text(plot::formatFixed(r->recipe.ebN0Db, 1)));
        legendColours.push_back(m_text);
    }
    p.restore();

    // Legend, top right.
    if (!legend.isEmpty())
    {
        const QString key = tr("Trait : théorie ; points : mesures, intervalle à 95 %");
        double w = metrics.horizontalAdvance(key) + 14.0;
        for (const QString &l : legend) w = std::max(w, metrics.horizontalAdvance(l) + 34.0);
        w = std::min(w, area.width() * 0.75);
        QStringList footer;
        if (metrics.horizontalAdvance(key) + 14.0 > w)
            footer << tr("Trait : théorie ;") << tr("points : mesures, intervalle à 95 %");
        else
            footer << key;
        const double h = metrics.height() * static_cast<double>(legend.size() + footer.size()) + 8.0;
        const QRectF box(area.right() - w - 6.0, area.top() + 6.0, w, h);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.9));
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        double y = box.top() + 4.0;
        for (qsizetype k = 0; k < legend.size(); ++k)
        {
            const double mid = y + metrics.height() / 2.0;
            p.setPen(QPen(legendColours[static_cast<std::size_t>(k)], 2.0));
            p.drawLine(QPointF(box.left() + 7.0, mid), QPointF(box.left() + 23.0, mid));
            p.setPen(m_text);
            p.drawText(QRectF(box.left() + 28.0, y, box.width() - 34.0, metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       metrics.elidedText(legend[k], Qt::ElideMiddle, box.width() - 34.0)); // (the end tells curves apart)
            y += metrics.height();
        }
        p.setPen(m_muted);
        for (const QString &l : footer)
        {
            p.drawText(QRectF(box.left() + 7.0, y, box.width() - 14.0, metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       metrics.elidedText(l, Qt::ElideRight, box.width() - 14.0));
            y += metrics.height();
        }
    }
    if (curves.empty())
    {
        p.setPen(m_muted);
        p.drawText(area.adjusted(20.0, 0.0, -20.0, 0.0), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("« Calculer » mesure le TEB de la liaison point par point (des bits tirés au hasard, autant qu'il "
                      "en faut pour compter de l'ordre de 200 erreurs) ; plusieurs courbes se comparent."));
    }
}

void ModulationPlot::updateReadout()
{
    QString shown, more; // (more: what a narrow band puts on a second line)
    const QRectF area = plotRect();
    if (m_hovering && area.contains(m_pointer) && hasData())
    {
        const View v = currentView();
        switch (m_kind)
        {
            case Kind::Constellation:
            {
                if (!m_result || !m_result->ok()) break;
                const Plane pl = plane(area, v);
                const double i = pl.iAt(m_pointer.x()), q = pl.qAt(m_pointer.y());
                const dsp::Modulation m = m_result->recipe.modulation;
                if (m == dsp::Modulation::Fsk)
                {
                    shown = tr("à fc + Δf %1 · à fc − Δf %2 · décidé : %3")
                                .arg(text(plot::formatFixed(i, 3)), text(plot::formatFixed(q, 3)), i > q ? QStringLiteral("1") : QStringLiteral("0"));
                }
                else
                {
                    const std::size_t s = dsp::decideSymbol(m, {i, q}, m_result->recipe.askLowLevel);
                    shown = tr("I %1 · Q %2 · décidé : %3").arg(text(plot::formatFixed(i, 3)), text(plot::formatFixed(q, 3)),
                                                              bitsOf(s, dsp::bitsPerSymbol(m)));
                }
                break;
            }
            case Kind::Eye:
            {
                if (!m_result || !m_result->ok()) break;
                const std::vector<QRectF> lanes = eyeLanes(area);
                for (std::size_t k = 0; k < lanes.size(); ++k)
                {
                    if (m_pointer.y() < lanes[k].top() || m_pointer.y() > lanes[k].bottom()) continue;
                    const double t = v.x.low + (m_pointer.x() - area.left()) / area.width() * v.x.span();
                    const double value = v.y.high - (m_pointer.y() - lanes[k].top()) / lanes[k].height() * v.y.span();
                    shown = tr("%1 · %2 de l'instant de décision · %3")
                                .arg(k == 1 ? tr("Q") : tr("I"), text(plot::formatQuantity(t, "s", 3)), text(plot::formatFixed(value, 3)));
                }
                break;
            }
            case Kind::Sync:
            {
                if (!m_result || !m_result->ok() || m_result->sync.timingError.empty()) break;
                const analysis::LinkSync &sync = m_result->sync;
                const double at = v.x.low + (m_pointer.x() - area.left()) / area.width() * v.x.span();
                const auto k = static_cast<std::size_t>(std::clamp(std::round(at), 0.0, static_cast<double>(sync.timingError.size() - 1)));
                shown = k < m_result->recipe.preambleSymbols ? tr("symbole %1 (préambule)").arg(count(k)) : tr("symbole %1").arg(count(k));
                shown += tr(" · rythme %1 symbole").arg(signedFixed(sync.timingError[k], 3));
                if (k < m_syncPhase.size()) shown += tr(" · porteuse %1°").arg(signedFixed(m_syncPhase[k], 1));
                break;
            }
            case Kind::Channel:
            {
                if (!m_result || !m_result->ok() || m_channel.taps.empty()) break;
                const std::vector<QRectF> lanes = channelLanes(area);
                const auto named = [this](const TapSeries *s) {
                    return s == &m_channel ? tr("canal") : s == &m_equalizer ? tr("égaliseur") : tr("ensemble");
                };
                if (m_pointer.y() >= lanes[0].top() && m_pointer.y() <= lanes[0].bottom())
                {
                    // The symbol whose stems are nearest (the channel's on its graduation, the others after it): each
                    // tap there, its modulus and (unless real and positive) its angle. The channel with the delay; the
                    // equalizer and both after it, on a second line in a narrow band.
                    const double middle = 0.5 * kTapSpread * ((m_equalizer.taps.empty() ? 0.0 : 1.0) + (m_combined.taps.empty() ? 0.0 : 1.0));
                    const double symbol = std::round(v.x.low + (m_pointer.x() - area.left()) / area.width() * v.x.span() - middle);
                    shown = tr("retard %1 symb.").arg(text(plot::formatCompact(symbol == 0.0 ? 0.0 : symbol, "", 6)));
                    for (const TapSeries *s : {&m_channel, &m_equalizer, &m_combined})
                    {
                        const double at = symbol - static_cast<double>(s->first);
                        if (s->taps.empty() || at < 0.0 || at >= static_cast<double>(s->taps.size())) continue;
                        const std::complex<double> tap = s->taps[static_cast<std::size_t>(at)];
                        const double modulus = std::abs(tap) < 5e-4 ? 0.0 : std::abs(tap);
                        QString part = tr("%1 %2").arg(named(s), text(plot::formatFixed(modulus, 3)));
                        double angle = std::round(std::arg(tap) * 180.0 / std::numbers::pi);
                        if (angle <= -180.0) angle = 180.0;
                        if (modulus > 0.0 && angle != 0.0) part += tr(" ∠ %1°").arg(text(plot::formatFixed(angle, 0)));
                        QString &line = s == &m_channel ? shown : more;
                        line += line.isEmpty() ? part : QStringLiteral(" · ") + part;
                    }
                }
                else if (m_pointer.y() >= lanes[1].top() && m_pointer.y() <= lanes[1].bottom())
                {
                    const double hertz = v.x2.low + (m_pointer.x() - area.left()) / area.width() * v.x2.span();
                    shown = text(plot::formatQuantity(hertz, "Hz", 4));
                    for (const TapSeries *s : {&m_channel, &m_equalizer, &m_combined})
                    {
                        if (s->gainDb.empty()) continue;
                        const QString part = tr("%1 %2 dB").arg(named(s), signedFixed(gainAt(s->gainDb, hertz / m_result->recipe.symbolRate), 1));
                        QString &line = s == &m_channel ? shown : more;
                        line += line.isEmpty() ? part : QStringLiteral(" · ") + part;
                    }
                }
                break;
            }
            case Kind::Ber:
            {
                const qacq::app::ModulationController *controller = m_modulation.data();
                if (!controller) break;
                const double eb = v.x.low + (m_pointer.x() - area.left()) / area.width() * v.x.span();
                shown = tr("Eb/N0 %1 dB").arg(text(plot::formatFixed(eb, 2)));
                // The point measured nearest the pointer (within a few pixels), or the theories there.
                double best = 12.0;
                QString near;
                const plot::YMap map{v.y.low, v.y.high, area.top(), area.height(), false};
                for (const auto &curve : controller->curves())
                    for (const analysis::BerPoint &point : curve.points)
                    {
                        const double x = area.left() + (point.ebN0Db - v.x.low) / v.x.span() * area.width();
                        const double y = map.toPixel(log10Rate(point.errors > 0 ? point.ber : point.upper));
                        const double distance = std::hypot(x - m_pointer.x(), y - m_pointer.y());
                        if (distance >= best) continue;
                        best = distance;
                        near = tr("%1 dB : TEB %2 (%3 sur %4 bits), théorie %5")
                                   .arg(text(plot::formatFixed(point.ebN0Db, 1)), rate(point.ber),
                                        point.errors == 0 ? tr("aucune erreur") : counted(point.errors, tr("%1 erreur"), tr("%1 erreurs")),
                                        count(point.bits), rate(point.theory));
                        if (point.batches > 0) near += tr(" · intervalle d'après %1 lots (les évanouissements groupent les erreurs)").arg(count(point.batches));
                    }
                if (!near.isEmpty())
                {
                    shown = near;
                }
                else
                {
                    const analysis::LinkRecipe now = controller->currentRecipe();
                    const double expected = analysis::linkTheoryBer(now, eb);
                    if (!std::isnan(expected)) shown += tr(" · théorie %1 : %2").arg(theoryName(now), rate(expected));
                }
                break;
            }
        }
    }
    if (shown == m_readout && more == m_readoutMore) return;
    m_readout = shown;
    m_readoutMore = more;
    emit readoutChanged();
    update();
}

void ModulationPlot::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0 || !hasData())
    {
        event->ignore();
        return;
    }
    zoomAt(std::pow(0.85, steps), event->position(), (event->modifiers() & Qt::ControlModifier) != 0);
    event->accept();
}

void ModulationPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !hasData())
    {
        event->ignore();
        return;
    }
    forceActiveFocus();
    rememberView();
    m_dragging = true;
    m_lastAt = m_pressAt = event->position();
    event->accept();
}

void ModulationPlot::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    const QPointF at = event->position();
    moveBy(at.x() - m_lastAt.x(), at.y() - m_lastAt.y());
    m_lastAt = at;
    m_pointer = at;
    event->accept();
}

void ModulationPlot::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    event->accept();
}

void ModulationPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    showAll();
    event->accept();
}

void ModulationPlot::hoverMoveEvent(QHoverEvent *event)
{
    m_hovering = true;
    m_pointer = event->position();
    updateReadout();
}

void ModulationPlot::hoverLeaveEvent(QHoverEvent *)
{
    m_hovering = false;
    updateReadout();
}
