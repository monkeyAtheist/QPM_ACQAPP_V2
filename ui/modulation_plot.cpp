#include "modulation_plot.h"

#include "qacq/analysis/link.h"
#include "qacq/app/modulation_controller.h"
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
    }
    return QStringLiteral("constellation");
}

void ModulationPlot::setView(const QString &key)
{
    const Kind kind = key == QStringLiteral("eye") ? Kind::Eye : key == QStringLiteral("ber") ? Kind::Ber : Kind::Constellation;
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

void ModulationPlot::contentUpdated()
{
    const qacq::app::ModulationController *controller = m_modulation.data();
    std::shared_ptr<const analysis::LinkResult> result = controller ? controller->currentResult() : nullptr;
    // Another modulation: its plane, its levels; the view starts again.
    if (m_kind != Kind::Ber && result && m_result && result->ok() && m_result->ok() &&
        (result->recipe.modulation != m_result->recipe.modulation ||
         result->recipe.representation != m_result->recipe.representation || result->recipe.symbolRate != m_result->recipe.symbolRate))
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
    const analysis::LinkResult *r = m_result.get();
    if (!r || !r->ok()) return;
    const std::size_t n = std::min(r->reception.decisionPoints.size(), std::min(r->sentSymbols.size(), r->reception.symbols.size()));
    const std::size_t first = std::min(r->skipSymbols, n), last = n > r->skipSymbols ? n - r->skipSymbols : 0;
    m_shown = last > first ? last - first : 0;
    for (std::size_t k = first; k < last; ++k)
        if (r->sentSymbols[k] != r->reception.symbols[k]) ++m_wrong;
    m_pointStep = std::max<std::size_t>(1, (m_shown + kMaximumPoints - 1) / kMaximumPoints);
    const std::size_t t = r->reception.decisionTimes.size();
    const std::size_t tFirst = std::min(r->skipSymbols, t), tLast = t > r->skipSymbols ? t - r->skipSymbols : 0;
    m_traces = tLast > tFirst ? tLast - tFirst : 0;
    m_traceStep = std::max<std::size_t>(1, (m_traces + kMaximumTraces - 1) / kMaximumTraces);
}

bool ModulationPlot::hasData() const
{
    switch (m_kind)
    {
        case Kind::Constellation: return m_result && m_result->ok() && !m_result->reception.decisionPoints.empty();
        case Kind::Eye: return m_result && m_result->ok() && !m_result->reception.matchedI.empty();
        case Kind::Ber: return m_modulation != nullptr;
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
            break;
        }
        case Kind::Eye:
            if (!r->reception.matchedI.empty() && m_traces > 0)
                facts << tr("%1 sur %2").arg(counted((m_traces + m_traceStep - 1) / m_traceStep, tr("%1 trace"), tr("%1 traces")),
                                             counted(m_traces, tr("%1 symbole"), tr("%1 symboles")));
            break;
        case Kind::Ber: break;
    }
    return facts;
}

std::vector<ModulationPlot::HeaderRow> ModulationPlot::headerRows() const
{
    const QFontMetricsF metrics(plotFont());
    const double room = std::max(40.0, width() - kRight - 4.0);
    std::vector<HeaderRow> rows{{headerTitle(), QString()}};
    const double titleWidth = metrics.horizontalAdvance(rows.front().left) + 24.0;
    for (const QString &fact : headerFacts())
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
                    deepest(dsp::theoreticalBer(curve.recipe.modulation, v.x.high, curve.recipe.askLowLevel));
                    for (const analysis::BerPoint &p : curve.points) deepest(p.errors > 0 ? p.lower : p.upper);
                }
                const analysis::LinkRecipe rr = c->currentRecipe();
                deepest(dsp::theoreticalBer(rr.modulation, v.x.high, rr.askLowLevel));
            }
            v.y = {std::clamp(std::floor(lowest) - 0.25, -12.0, -1.0), std::log10(0.7)};
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
        m_history.back().x.high == v.x.high && m_history.back().y.low == v.y.low && m_history.back().y.high == v.y.high)
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
    }
    if (!(v.x.span() > 1e-9) || !(v.y.span() > 1e-9)) return;
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

const std::vector<double> &ModulationPlot::theory(int modulation, double askLowLevel, const Range &x, int count)
{
    char key[160];
    std::snprintf(key, sizeof key, "%d|%.17g|%.17g|%.17g|%d", modulation, askLowLevel, x.low, x.high, count);
    auto found = m_theory.find(key);
    if (found != m_theory.end()) return found->second;
    std::vector<double> values(static_cast<std::size_t>(std::max(2, count)));
    for (std::size_t k = 0; k < values.size(); ++k)
    {
        const double eb = x.low + x.span() * static_cast<double>(k) / static_cast<double>(values.size() - 1);
        values[k] = log10Rate(dsp::theoreticalBer(static_cast<dsp::Modulation>(modulation), eb, askLowLevel));
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
    }
    switch (m_kind)
    {
        case Kind::Constellation: paintConstellation(p, area); break;
        case Kind::Eye: paintEye(p, area); break;
        case Kind::Ber: paintBer(p, area); break;
    }
    paintHeader(p);
    if (m_hovering && !m_readout.isEmpty())
    {
        // At the bottom of the band under the pointer (the eye: I or Q).
        QRectF band = area;
        if (m_kind == Kind::Eye)
            for (const QRectF &lane : eyeLanes(area))
                if (m_pointer.y() >= lane.top() && m_pointer.y() <= lane.bottom()) band = lane;
        const QFontMetricsF metrics(p.font());
        const double w = std::min(band.width() - 8.0, metrics.horizontalAdvance(m_readout) + 14.0);
        const QRectF box(band.left() + 4.0, band.bottom() - metrics.height() - 10.0, w, metrics.height() + 6.0);
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.9));
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setPen(m_text);
        p.drawText(box.adjusted(7.0, 0.0, -7.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetricsF(p.font()).elidedText(m_readout, Qt::ElideRight, box.width() - 14.0));
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

    // The points received: right ones first, those decided wrong on top.
    const std::size_t n = std::min(r.reception.decisionPoints.size(), std::min(r.sentSymbols.size(), r.reception.symbols.size()));
    const std::size_t first = std::min(r.skipSymbols, n), last = n > r.skipSymbols ? n - r.skipSymbols : 0;
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
    const double fs = r.recipe.sampleRateHz;
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

    const std::size_t n = r.reception.decisionTimes.size();
    const std::size_t first = std::min(r.skipSymbols, n), last = n > r.skipSymbols ? n - r.skipSymbols : 0;
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
            for (auto i = static_cast<std::size_t>(i0); i <= static_cast<std::size_t>(i1); ++i)
                trace << QPointF(xOf((static_cast<double>(i) - t) / fs), std::clamp(map.toPixel(y[i]), lane.top() - 2.0 * lane.height(), lane.bottom() + 2.0 * lane.height()));
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
        const std::vector<double> &values = theory(static_cast<int>(recipe.modulation), recipe.askLowLevel, v.x, columns);
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
    const bool nowHasCurve = std::any_of(curves.begin(), curves.end(), [&now](const auto &c) {
        return c.recipe.modulation == now.modulation && c.recipe.askLowLevel == now.askLowLevel;
    });
    if (!nowHasCurve)
    {
        drawTheory(now, QPen(faded(m_muted, 0.9), 1.4, Qt::DashLine));
        legend << tr("Théorie %1").arg(text(std::string(dsp::modulationName(now.modulation))));
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
        double lower = 0.0, upper = 0.0;
        analysis::binomialInterval(r->bitErrors, r->comparedBits, lower, upper);
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
                       metrics.elidedText(legend[k], Qt::ElideRight, box.width() - 34.0));
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
    QString shown;
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
                    }
                if (!near.isEmpty())
                {
                    shown = near;
                }
                else
                {
                    const analysis::LinkRecipe now = controller->currentRecipe();
                    shown += tr(" · théorie %1 : %2")
                                 .arg(text(std::string(dsp::modulationName(now.modulation))), rate(dsp::theoreticalBer(now.modulation, eb, now.askLowLevel)));
                }
                break;
            }
        }
    }
    if (shown == m_readout) return;
    m_readout = shown;
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
    m_lastAt = event->position();
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
