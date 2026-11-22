#include "response_plot.h"

#include "qacq/analysis/filters.h"
#include "qacq/app/filter_controller.h"
#include "qacq/app/format.h"
#include "qacq/plot/axis.h"

#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace analysis = qacq::analysis;
namespace plot = qacq::plot;

namespace
{
constexpr double kLeft = 62.0;
constexpr double kRight = 14.0;
constexpr double kTop = 22.0; // the unit of the vertical axis above it
constexpr double kBottom = 40.0;
constexpr double kDegrees = 180.0 / std::numbers::pi;
constexpr double kFloorDb = -400.0; // |H| = 0
constexpr std::size_t kHistory = 50;

QString text(const std::string &s)
{
    return QString::fromStdString(s);
}

QColor faded(QColor colour, double alpha)
{
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * static_cast<double>(colour.alphaF())));
    return colour;
}

QString quantity(double value, const char *unit, int digits = 4)
{
    return text(plot::formatQuantity(value, unit, digits));
}

QString fixed(double value, int decimals)
{
    return text(plot::formatFixed(value, decimals));
}

// Within (−180, 180].
double wrapped(double degrees)
{
    double d = std::remainder(degrees, 360.0);
    if (d <= -180.0) d += 360.0;
    return d;
}

struct Key
{
    const char *key;
    int kind;
};
constexpr std::array<Key, 6> kKinds{{{"gain", 0}, {"phase", 1}, {"delay", 2}, {"impulse", 3}, {"step", 4}, {"polezero", 5}}};

// A pixel kept within reach of the area (QPainter and very large coordinates do not mix).
double bounded(double y, const QRectF &area)
{
    return std::clamp(y, area.top() - 4.0 * area.height(), area.bottom() + 4.0 * area.height());
}
} // namespace

QPointF ResponsePlot::Plane::toScreen(double re, double im) const
{
    return {origin.x() + (re - centreX) * scale, origin.y() - (im - centreY) * scale};
}

double ResponsePlot::Plane::reAt(double x) const
{
    return centreX + (x - origin.x()) / scale;
}

double ResponsePlot::Plane::imAt(double y) const
{
    return centreY - (y - origin.y()) / scale;
}

ResponsePlot::ResponsePlot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setActiveFocusOnTab(true);
    setCursor(QCursor(Qt::CrossCursor));
    connect(this, &ResponsePlot::styleChanged, this, [this] { update(); });
}

QObject *ResponsePlot::filter() const
{
    return m_filter.data();
}

void ResponsePlot::setFilter(QObject *filter)
{
    auto *controller = qobject_cast<qacq::app::FilterController *>(filter);
    if (controller == m_filter.data()) return;
    disconnect(m_connection);
    m_filter = controller;
    if (controller)
        m_connection = connect(controller, &qacq::app::FilterController::designChanged, this, &ResponsePlot::designChanged);
    emit filterChanged();
    designChanged();
}

QString ResponsePlot::response() const
{
    for (const Key &k : kKinds)
        if (k.kind == static_cast<int>(m_kind)) return QString::fromLatin1(k.key);
    return QStringLiteral("gain");
}

void ResponsePlot::setResponse(const QString &key)
{
    Kind kind = m_kind;
    for (const Key &k : kKinds)
        if (key == QString::fromLatin1(k.key)) kind = static_cast<Kind>(k.kind);
    if (kind == m_kind) return;
    m_kind = kind;
    m_view.levelsAuto = true;
    m_history.clear();
    emit responseChanged();
    emit contentChanged();
    updateReadout();
    changed();
}

void ResponsePlot::setLogFrequency(bool log)
{
    if (log == m_log) return;
    m_log = log;
    m_view.frequenciesAll = true;
    m_history.clear();
    updateReadout();
    changed();
}

void ResponsePlot::setDecibels(bool decibels)
{
    if (decibels == m_decibels) return;
    m_decibels = decibels;
    m_view.levelsAuto = true;
    updateReadout();
    changed();
}

void ResponsePlot::setWrapPhase(bool wrap)
{
    if (wrap == m_wrap) return;
    m_wrap = wrap;
    m_view.levelsAuto = true;
    updateReadout();
    changed();
}

void ResponsePlot::setShowApplied(bool show)
{
    if (show == m_showApplied) return;
    m_showApplied = show;
    if (m_kind != Kind::PoleZero) m_view.levelsAuto = true;
    updateReadout();
    changed();
}

void ResponsePlot::designChanged()
{
    const std::shared_ptr<const analysis::FilterDesign> previous = m_design;
    qacq::app::FilterController *controller = m_filter.data(); // read once
    m_design = controller ? controller->currentDesign() : nullptr;
    m_impulse.reset();
    m_columns.clear();
    m_columnsDesign = nullptr;
    // Another rate: the frequencies shown no longer mean the same.
    if (!previous || !m_design || previous->recipe.sampleRateHz != m_design->recipe.sampleRateHz)
        m_view.frequenciesAll = true;
    if (!m_view.frequenciesAll)
    {
        const Range limits = frequencyLimits();
        if (!(m_view.frequencies.low >= limits.low && m_view.frequencies.high <= limits.high)) m_view.frequenciesAll = true;
    }
    if (!previous || !m_design || previous->fir() != m_design->fir()) m_view.samplesAll = true;
    emit contentChanged();
    updateReadout();
    changed();
}

bool ResponsePlot::hasData() const
{
    if (!m_design || !m_design->ok()) return false;
    if (m_kind == Kind::PoleZero) return m_design->rootsKnown || !m_design->sections.empty();
    return true;
}

bool ResponsePlot::appliedDiffers() const
{
    if (!m_design || !m_design->ok()) return false;
    switch (m_design->recipe.application)
    {
        case analysis::FilterApplication::Causal: return false;
        case analysis::FilterApplication::ZeroPhase: return m_kind != Kind::PoleZero;
        case analysis::FilterApplication::Aligned: return m_kind != Kind::PoleZero && m_kind != Kind::Gain;
    }
    return false;
}

bool ResponsePlot::appliedShown() const
{
    return m_showApplied && appliedDiffers();
}

QString ResponsePlot::appliedName() const
{
    if (!m_design) return {};
    return m_design->recipe.application == analysis::FilterApplication::ZeroPhase ? tr("appliqué à phase nulle")
                                                                                   : tr("appliqué sans son retard");
}

bool ResponsePlot::zoomed() const
{
    if (frequencyKind()) return !m_view.frequenciesAll || !m_view.levelsAuto;
    if (sampleKind()) return !m_view.samplesAll || !m_view.levelsAuto;
    return !m_view.planeAll;
}

const analysis::ImpulseResponse *ResponsePlot::impulse() const
{
    qacq::app::FilterController *controller = m_filter.data(); // read once
    if (!m_impulse && controller && m_design && m_design->ok()) m_impulse = controller->currentImpulse();
    return m_impulse.get();
}

QRectF ResponsePlot::plotRect() const
{
    return {kLeft, kTop, std::max(0.0, width() - kLeft - kRight), std::max(0.0, height() - kTop - kBottom)};
}

ResponsePlot::Range ResponsePlot::frequencyLimits() const
{
    const double nyquist = m_design ? 0.5 * m_design->recipe.sampleRateHz : 24000.0;
    if (!m_log) return {0.0, nyquist};
    // Three decades below fs/2, or a decade below the lowest edge.
    double low = std::pow(10.0, std::floor(std::log10(nyquist)) - 3.0);
    // (Coefficients typed: no edge. A template: its lowest edge.)
    double f1 = 0.0;
    if (m_design && m_design->recipe.kind != analysis::FilterKind::Typed)
    {
        const analysis::FilterRecipe &r = m_design->recipe;
        f1 = r.fromTemplate ? std::min(r.passEdge1Hz, r.stopEdge1Hz) : r.frequency1Hz;
    }
    if (f1 > 0.0) low = std::min(low, std::pow(10.0, std::floor(std::log10(f1 / 10.0))));
    return {low, nyquist};
}

ResponsePlot::Range ResponsePlot::sampleLimits() const
{
    const analysis::ImpulseResponse *r = impulse();
    if (!r || r->impulse.empty()) return {0.0, 1.0};
    double low = 0.0;
    double high = static_cast<double>(r->impulse.size() - 1);
    if (appliedShown() && !r->appliedImpulse.empty())
    {
        low = std::min(low, static_cast<double>(r->appliedStart));
        high = std::max(high, static_cast<double>(r->appliedStart) + static_cast<double>(r->appliedImpulse.size() - 1));
    }
    const double pad = std::max(1.0, 0.02 * (high - low));
    return {low - pad, high + pad};
}

double ResponsePlot::planeRadius() const
{
    double r = 1.0;
    if (m_design)
    {
        for (const auto &z : m_design->zeros) r = std::max(r, std::abs(z));
        for (const auto &p : m_design->poles) r = std::max(r, std::abs(p));
    }
    return std::max(1.15, std::min(6.0, 1.08 * r));
}

ResponsePlot::Range ResponsePlot::frequencyView() const
{
    if (m_view.frequenciesAll || !m_view.frequencies.valid()) return frequencyLimits();
    if (m_log && !(m_view.frequencies.low > 0.0)) return frequencyLimits();
    return m_view.frequencies;
}

ResponsePlot::Range ResponsePlot::sampleView() const
{
    if (m_view.samplesAll || !m_view.samples.valid()) return sampleLimits();
    return m_view.samples;
}

ResponsePlot::Plane ResponsePlot::plane(const QRectF &area) const
{
    Plane p;
    p.origin = area.center();
    if (m_view.planeAll)
    {
        p.scale = std::min(area.width(), area.height()) / (2.0 * planeRadius());
    }
    else
    {
        p.centreX = m_view.planeX;
        p.centreY = m_view.planeY;
        p.scale = area.height() / (2.0 * m_view.planeHalf);
    }
    if (!(p.scale > 0.0) || !std::isfinite(p.scale)) p.scale = 1.0;
    return p;
}

double ResponsePlot::xOf(double value, const QRectF &area) const
{
    if (frequencyKind())
    {
        const Range v = frequencyView();
        if (m_log)
        {
            if (!(value > 0.0)) return area.left() - 1e4;
            return area.left() + (std::log10(value) - std::log10(v.low)) / (std::log10(v.high) - std::log10(v.low)) * area.width();
        }
        return area.left() + (value - v.low) / v.span() * area.width();
    }
    const Range v = sampleView();
    return area.left() + (value - v.low) / v.span() * area.width();
}

double ResponsePlot::valueAt(double x, const QRectF &area) const
{
    const double t = area.width() > 0.0 ? (x - area.left()) / area.width() : 0.0;
    if (frequencyKind())
    {
        const Range v = frequencyView();
        if (m_log) return std::pow(10.0, std::log10(v.low) + t * (std::log10(v.high) - std::log10(v.low)));
        return v.low + t * v.span();
    }
    const Range v = sampleView();
    return v.low + t * v.span();
}

double ResponsePlot::designedValue(double f) const
{
    const analysis::FilterDesign &design = *m_design;
    switch (m_kind)
    {
        case Kind::Gain:
        {
            const double h = std::abs(design.response(f));
            return m_decibels ? (h > 0.0 ? std::max(kFloorDb, 20.0 * std::log10(h)) : kFloorDb) : h;
        }
        case Kind::Phase:
        {
            const double deg = design.phase(f) * kDegrees;
            return m_wrap ? wrapped(deg) : deg;
        }
        case Kind::Delay: return design.groupDelay(f);
        default: break;
    }
    return 0.0;
}

double ResponsePlot::appliedValue(double f) const
{
    const analysis::FilterDesign &design = *m_design;
    switch (m_kind)
    {
        case Kind::Gain:
        {
            const double h = std::abs(design.applied(f));
            return m_decibels ? (h > 0.0 ? std::max(kFloorDb, 20.0 * std::log10(h)) : kFloorDb) : h;
        }
        case Kind::Phase:
        {
            const double deg = design.appliedPhase(f) * kDegrees;
            return m_wrap ? wrapped(deg) : deg;
        }
        case Kind::Delay: return design.appliedGroupDelay(f);
        default: break;
    }
    return 0.0;
}

const std::vector<ResponsePlot::Column> &ResponsePlot::columns(const QRectF &area)
{
    const int count = std::max(2, static_cast<int>(std::ceil(area.width())) + 1);
    const Range v = frequencyView();
    if (m_columnsDesign == m_design.get() && m_columnsKind == m_kind && m_columnsCount == count &&
        m_columnsRange.low == v.low && m_columnsRange.high == v.high && m_columnsLog == m_log &&
        m_columnsDecibels == m_decibels && m_columnsWrap == m_wrap && !m_columns.empty())
        return m_columns;
    m_columns.assign(static_cast<std::size_t>(count), Column{});
    const bool applied = appliedDiffers();
    double largest = 0.0;
    for (int c = 0; c < count; ++c)
    {
        Column &col = m_columns[static_cast<std::size_t>(c)];
        col.frequency = valueAt(area.left() + area.width() * c / (count - 1), area);
        col.designed = designedValue(col.frequency);
        col.applied = applied ? appliedValue(col.frequency) : col.designed;
        if (m_kind == Kind::Delay)
        {
            col.magnitude = std::abs(m_design->response(col.frequency));
            largest = std::max(largest, col.magnitude);
        }
    }
    // The delay where the gain is nothing (on a zero of the circle, fs/2 for a bilinear low-pass): 0/0, not
    // drawn.
    if (m_kind == Kind::Delay)
    {
        for (Column &col : m_columns)
        {
            if (!(col.magnitude > 1e-9 * largest))
            {
                col.designed = std::numeric_limits<double>::quiet_NaN();
                col.applied = std::numeric_limits<double>::quiet_NaN();
            }
        }
    }
    m_columnsDesign = m_design.get();
    m_columnsKind = m_kind;
    m_columnsCount = count;
    m_columnsRange = v;
    m_columnsLog = m_log;
    m_columnsDecibels = m_decibels;
    m_columnsWrap = m_wrap;
    return m_columns;
}

ResponsePlot::Range ResponsePlot::automaticLevels(const QRectF &area)
{
    const bool applied = appliedShown();
    if (frequencyKind())
    {
        const std::vector<Column> &cols = columns(area);
        std::vector<double> values;
        values.reserve(2 * cols.size());
        double largest = 0.0;
        for (const Column &c : cols) largest = std::max(largest, c.magnitude);
        for (const Column &c : cols)
        {
            // The delay where there is a gain only: about the zeros it means nothing and runs away.
            if (m_kind == Kind::Delay && c.magnitude < 1e-4 * largest) continue;
            if (std::isfinite(c.designed)) values.push_back(c.designed);
            if (applied && std::isfinite(c.applied)) values.push_back(c.applied);
        }
        if (values.empty()) return {-1.0, 1.0};
        std::sort(values.begin(), values.end());
        double low = values.front(), high = values.back();
        if (m_kind == Kind::Delay && values.size() > 20)
        {
            // Without the spikes of the narrow places.
            low = values[values.size() / 100];
            high = values[values.size() - 1 - values.size() / 100];
        }
        // In dB, a hundred down at most; a template's stop band line in view (20 dB under it).
        if (m_kind == Kind::Gain && m_decibels)
        {
            const double span = templateShown() ? std::max(100.0, m_design->recipe.stopbandAttenuationDb + 20.0) : 100.0;
            low = std::max(low, high - span);
        }
        if (m_kind == Kind::Gain && !m_decibels) low = std::min(0.0, low);
        return plot::paddedRange(low, high, 0.06);
    }
    // Along the samples: what is visible, and 0.
    const analysis::ImpulseResponse *r = impulse();
    if (!r) return {-1.0, 1.0};
    const Range v = sampleView();
    double low = 0.0, high = 0.0;
    const auto scan = [&](const std::vector<double> &data, double start) {
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            const double n = start + static_cast<double>(i);
            if (n < v.low || n > v.high || !std::isfinite(data[i])) continue;
            low = std::min(low, data[i]);
            high = std::max(high, data[i]);
        }
    };
    scan(m_kind == Kind::Step ? r->step : r->impulse, 0.0);
    if (applied) scan(m_kind == Kind::Step ? r->appliedStep : r->appliedImpulse, static_cast<double>(r->appliedStart));
    return plot::paddedRange(low, high, 0.08);
}

void ResponsePlot::changed()
{
    update();
    emit viewChanged();
}

void ResponsePlot::resetView()
{
    if (frequencyKind())
        m_view.frequenciesAll = true;
    else if (sampleKind())
        m_view.samplesAll = true;
    else
        m_view.planeAll = true;
    m_view.levelsAuto = true;
}

void ResponsePlot::showAll()
{
    if (!hasData()) return;
    rememberView(false);
    resetView();
    updateReadout();
    changed();
}

void ResponsePlot::rememberView(bool coalesce)
{
    if (!hasData()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameGesture = coalesce && now - m_lastRemembered < 600;
    m_lastRemembered = coalesce ? now : 0;
    if (sameGesture) return;
    m_history.push_back(m_view);
    if (m_history.size() > kHistory) m_history.erase(m_history.begin());
    emit viewChanged();
}

void ResponsePlot::goBack()
{
    if (m_history.empty()) return;
    m_view = m_history.back();
    m_history.pop_back();
    m_lastRemembered = 0;
    updateReadout();
    changed();
}

void ResponsePlot::setFrequencyView(Range range)
{
    const Range limits = frequencyLimits();
    if (m_log)
    {
        // In decades.
        const Range decades{std::log10(limits.low), std::log10(limits.high)};
        Range r{std::log10(std::max(range.low, 1e-300)), std::log10(std::max(range.high, 1e-300))};
        r = plot::clampRange(r, decades, 1e-4);
        m_view.frequencies = {std::pow(10.0, r.low), std::pow(10.0, r.high)};
        m_view.frequenciesAll = r.span() >= decades.span() * (1.0 - 1e-9);
    }
    else
    {
        const Range r = plot::clampRange(range, limits, limits.span() * 1e-6);
        m_view.frequencies = r;
        m_view.frequenciesAll = r.span() >= limits.span() * (1.0 - 1e-9);
    }
    updateReadout();
    changed();
}

void ResponsePlot::setSampleView(Range range)
{
    const Range limits = sampleLimits();
    // Zoomed out up to ten times what there is; in at most to four samples.
    const Range wide{limits.low - 4.5 * limits.span(), limits.high + 4.5 * limits.span()};
    const Range r = plot::clampRange(range, wide, 4.0);
    m_view.samples = r;
    m_view.samplesAll = std::abs(r.low - limits.low) < 1e-9 * limits.span() && std::abs(r.high - limits.high) < 1e-9 * limits.span();
    updateReadout();
    changed();
}

void ResponsePlot::setLevels(Range range)
{
    if (!range.valid() || !(range.span() > 0.0)) return;
    m_view.levels = range;
    m_view.levelsAuto = false;
    updateReadout();
    changed();
}

void ResponsePlot::zoomFrequency(double factor)
{
    if (!hasData() || !(factor > 0.0)) return;
    rememberView(false);
    const QRectF area = plotRect();
    zoomAt(factor, area.center(), false);
}

void ResponsePlot::zoomAt(double factor, const QPointF &at, bool vertical)
{
    const QRectF area = plotRect();
    if (m_kind == Kind::PoleZero)
    {
        const Plane p = plane(area);
        const double re = p.reAt(at.x()), im = p.imAt(at.y());
        const double half = 0.5 * area.height() / p.scale * factor;
        if (!(half > 1e-9 && half < 1e4)) return;
        // The point under the pointer stays where it is.
        m_view.planeHalf = half;
        const double pixels = area.height() / (2.0 * half); // a unit, once zoomed
        m_view.planeX = re - (at.x() - area.center().x()) / pixels;
        m_view.planeY = im + (at.y() - area.center().y()) / pixels;
        m_view.planeAll = false;
        updateReadout();
        changed();
        return;
    }
    if (vertical)
    {
        const Range levels = m_view.levelsAuto ? automaticLevels(area) : m_view.levels;
        const plot::YMap map{levels.low, levels.high, area.top(), area.height(), false};
        setLevels(plot::zoomRange(levels, factor, map.toValue(at.y())));
        return;
    }
    if (frequencyKind())
    {
        const Range v = frequencyView();
        const double anchor = valueAt(at.x(), area);
        if (m_log)
        {
            const Range decades{std::log10(v.low), std::log10(v.high)};
            const Range z = plot::zoomRange(decades, factor, std::log10(anchor));
            setFrequencyView({std::pow(10.0, z.low), std::pow(10.0, z.high)});
        }
        else
        {
            setFrequencyView(plot::zoomRange(v, factor, anchor));
        }
        return;
    }
    setSampleView(plot::zoomRange(sampleView(), factor, valueAt(at.x(), area)));
}

void ResponsePlot::moveBy(double dx, double dy)
{
    const QRectF area = plotRect();
    if (area.width() <= 0.0 || area.height() <= 0.0) return;
    if (m_kind == Kind::PoleZero)
    {
        const Plane p = plane(area);
        m_view.planeX = p.centreX - dx / p.scale;
        m_view.planeY = p.centreY + dy / p.scale;
        m_view.planeHalf = 0.5 * area.height() / p.scale;
        m_view.planeAll = false;
        updateReadout();
        changed();
        return;
    }
    if (dy != 0.0)
    {
        const Range levels = m_view.levelsAuto ? automaticLevels(area) : m_view.levels;
        const double shift = dy / area.height() * levels.span();
        setLevels({levels.low + shift, levels.high + shift});
    }
    if (dx != 0.0)
    {
        const double fraction = -dx / area.width();
        if (frequencyKind())
        {
            const Range v = frequencyView();
            if (m_log)
            {
                const Range decades = plot::shiftRange({std::log10(v.low), std::log10(v.high)}, fraction);
                setFrequencyView({std::pow(10.0, decades.low), std::pow(10.0, decades.high)});
            }
            else
            {
                setFrequencyView(plot::shiftRange(v, fraction));
            }
        }
        else
        {
            setSampleView(plot::shiftRange(sampleView(), fraction));
        }
    }
}

std::vector<ResponsePlot::Root> ResponsePlot::roots() const
{
    std::vector<Root> out;
    if (!m_design) return out;
    const auto add = [&out](const std::complex<double> &z, bool pole) {
        for (Root &r : out)
        {
            if (r.pole == pole && std::abs(r.value - z) <= 1e-6 * std::max(1.0, std::abs(z)))
            {
                ++r.count;
                return;
            }
        }
        out.push_back({z, 1, pole});
    };
    for (const auto &z : m_design->zeros) add(z, false);
    for (const auto &p : m_design->poles) add(p, true);
    // A FIR: as many poles at the origin as it has zeros.
    if (m_design->fir() && !m_design->zeros.empty())
        out.push_back({{0.0, 0.0}, static_cast<int>(m_design->zeros.size()), true});
    return out;
}

void ResponsePlot::updateReadout()
{
    QString value;
    const QRectF area = plotRect();
    if (m_hovering && hasData() && area.contains(m_pointer))
    {
        const analysis::FilterDesign &design = *m_design;
        const double fs = design.recipe.sampleRateHz;
        const bool applied = appliedShown();
        if (frequencyKind())
        {
            const double f = valueAt(m_pointer.x(), area);
            value = QStringLiteral("f = %1").arg(quantity(f, "Hz", 5));
            const auto shown = [this](double v) -> QString {
                switch (m_kind)
                {
                    case Kind::Gain:
                        if (!m_decibels) return text(plot::formatQuantity(v, "", 5));
                        if (v <= kFloorDb) return QStringLiteral("−∞ dB");
                        return QString(fixed(v, 2) + QStringLiteral(" dB"));
                    case Kind::Phase: return QString(fixed(v, 1) + QStringLiteral("°"));
                    default: return QString(fixed(v, 2) + tr(" éch."));
                }
            };
            const double v = designedValue(f);
            const QString name = m_kind == Kind::Gain ? tr("gain") : m_kind == Kind::Phase ? tr("phase") : tr("retard");
            value += QStringLiteral(" · %1 %2").arg(name, shown(v));
            if (m_kind == Kind::Delay && std::isfinite(v)) value += QStringLiteral(" (%1)").arg(quantity(v / fs, "s", 4));
            if (applied) value += QStringLiteral(" · %1 %2").arg(tr("appliqué"), shown(appliedValue(f)));
        }
        else if (sampleKind())
        {
            const analysis::ImpulseResponse *r = impulse();
            const double n = std::round(valueAt(m_pointer.x(), area));
            value = QStringLiteral("n = %1 (%2)").arg(QString::number(static_cast<long long>(n)), quantity(n / fs, "s", 4));
            const auto at = [n](const std::vector<double> &data, double start) {
                const double i = n - start;
                if (i < 0.0 || i >= static_cast<double>(data.size())) return 0.0;
                return data[static_cast<std::size_t>(i)];
            };
            if (r)
            {
                const bool step = m_kind == Kind::Step;
                value += QStringLiteral(" · %1 = %2").arg(step ? QStringLiteral("s") : QStringLiteral("h"),
                                                          text(plot::formatQuantity(at(step ? r->step : r->impulse, 0.0), "", 5)));
                if (applied)
                    value += QStringLiteral(" · %1 %2").arg(tr("appliqué"), text(plot::formatQuantity(
                                                                         at(step ? r->appliedStep : r->appliedImpulse,
                                                                            static_cast<double>(r->appliedStart)),
                                                                         "", 5)));
            }
        }
        else
        {
            const Plane p = plane(area);
            const std::vector<Root> list = roots();
            const Root *nearest = nullptr;
            double best = 10.0;
            for (const Root &r : list)
            {
                const QPointF s = p.toScreen(r.value.real(), r.value.imag());
                const double dist = std::hypot(s.x() - m_pointer.x(), s.y() - m_pointer.y());
                if (dist < best)
                {
                    best = dist;
                    nearest = &r;
                }
            }
            std::complex<double> z(p.reAt(m_pointer.x()), p.imAt(m_pointer.y()));
            QString what = QStringLiteral("z");
            if (nearest)
            {
                z = nearest->value;
                what = nearest->pole ? tr("Pôle") : tr("Zéro");
            }
            const QString sign = z.imag() < 0.0 ? QStringLiteral("−") : QStringLiteral("+");
            value = QStringLiteral("%1 %2 %3 %4j · |z| = %5 · %6")
                        .arg(what, fixed(z.real(), 4), sign, fixed(std::abs(z.imag()), 4), fixed(std::abs(z), 5),
                             quantity(std::abs(std::arg(z)) * fs / (2.0 * std::numbers::pi), "Hz", 4));
            if (nearest && nearest->count > 1) value += QStringLiteral(" (×%1)").arg(nearest->count);
        }
    }
    if (value == m_readout) return;
    m_readout = value;
    emit readoutChanged();
}

// ------------------------------------------------------------------------------------------------ paint

void ResponsePlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    QFont font = p.font();
    font.setPixelSize(std::max(8, m_fontSize));
    p.setFont(font);
    const QRectF area = plotRect();
    if (area.width() < 40.0 || area.height() < 30.0) return;

    QString message;
    if (!m_design)
        message = m_emptyText;
    else if (!m_design->ok())
        message = text(m_design->error);
    else if (m_kind == Kind::PoleZero && !hasData())
        message = tr("Les zéros d'un RIF de plus de %1 coefficients ne sont pas calculés ici : ses réponses, si.")
                      .arg(static_cast<int>(analysis::kMaximumFirRoots));
    if (!message.isEmpty())
    {
        p.setPen(m_muted);
        p.drawText(boundingRect().adjusted(24.0, 0.0, -24.0, 0.0), Qt::AlignCenter | Qt::TextWordWrap, message);
        return;
    }
    if (frequencyKind())
        paintFrequencies(p, area);
    else if (sampleKind())
        paintSamples(p, area);
    else
        paintPlane(p, area);
    // The band a right drag covers.
    if (m_drag == Drag::Band && m_moved)
    {
        const double x1 = std::clamp(std::min(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        const double x2 = std::clamp(std::max(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        QRectF band(x1, area.top(), x2 - x1, area.height());
        if (m_kind == Kind::PoleZero)
        {
            const double y1 = std::clamp(std::min(m_pressAt.y(), m_pointer.y()), area.top(), area.bottom());
            const double y2 = std::clamp(std::max(m_pressAt.y(), m_pointer.y()), area.top(), area.bottom());
            band = QRectF(x1, y1, x2 - x1, y2 - y1);
        }
        p.setPen(QPen(faded(m_accent, 0.8), 1.0));
        p.setBrush(faded(m_accent, 0.15));
        p.drawRect(band);
        p.setBrush(Qt::NoBrush);
    }
    paintReadout(p, area);
}

void ResponsePlot::paintLevelAxis(QPainter &p, const QRectF &area, const QString &title)
{
    const QFontMetricsF metrics(p.font());
    const Range levels = m_view.levels;
    const plot::YMap map{levels.low, levels.high, area.top(), area.height(), false};
    const plot::Ticks ticks = plot::linearTicks(levels.low, levels.high, area.height(), 28.0);
    const plot::AxisLabels labels = plot::axisLabels(ticks, "");
    p.setRenderHint(QPainter::Antialiasing, false);
    for (std::size_t i = 0; i < ticks.major.size(); ++i)
    {
        const double y = std::round(map.toPixel(ticks.major[i])) + 0.5;
        if (y < area.top() || y > area.bottom()) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        p.setPen(m_muted);
        p.drawText(QRectF(0.0, y - metrics.height() / 2.0, area.left() - 6.0, metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter, text(labels.text[i]));
    }
    QString shown = title;
    if (!labels.unit.empty()) shown += QStringLiteral(" (%1)").arg(text(labels.unit));
    p.setPen(m_muted);
    p.drawText(QRectF(4.0, 2.0, area.width(), kTop - 4.0), Qt::AlignLeft | Qt::AlignVCenter, shown);
}

void ResponsePlot::paintFrequencies(QPainter &p, const QRectF &area)
{
    const QFontMetricsF metrics(p.font());
    const std::vector<Column> &cols = columns(area);
    if (m_view.levelsAuto) m_view.levels = automaticLevels(area);
    const Range levels = m_view.levels;
    const plot::YMap map{levels.low, levels.high, area.top(), area.height(), false};
    const Range v = frequencyView();

    // Frequency axis.
    p.setRenderHint(QPainter::Antialiasing, false);
    plot::Ticks ticks;
    std::vector<std::string> labels;
    QString title = tr("Fréquence");
    if (m_log)
    {
        ticks = plot::logTicks(v.low, v.high, area.width(), 70.0);
        labels = plot::logAxisLabels(ticks, "Hz");
    }
    else
    {
        ticks = plot::linearTicks(v.low, v.high, area.width(), 90.0);
        const plot::AxisLabels l = plot::axisLabels(ticks, "Hz");
        labels = l.text;
        if (!l.unit.empty()) title += QStringLiteral(" (%1)").arg(text(l.unit));
    }
    p.setPen(QPen(m_log ? faded(m_grid, 0.55) : faded(m_muted, 0.6), 1.0));
    for (double f : ticks.minor)
    {
        const double x = std::round(xOf(f, area)) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        if (m_log)
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        else
            p.drawLine(QPointF(x, area.bottom()), QPointF(x, area.bottom() + 3.0));
    }
    p.setPen(QPen(m_grid, 1.0));
    for (double f : ticks.major)
    {
        const double x = std::round(xOf(f, area)) + 0.5;
        if (x >= area.left() && x <= area.right()) p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (std::size_t i = 0; i < ticks.major.size() && i < labels.size(); ++i)
    {
        const QString label = text(labels[i]);
        const double w = metrics.horizontalAdvance(label);
        const double x = xOf(ticks.major[i], area) - w / 2.0;
        if (x < lastRight + 8.0 || x + w > width() - 2.0 || x + w / 2.0 < area.left() - 1.0) continue;
        p.drawText(QPointF(std::max(2.0, x), area.bottom() + 4.0 + metrics.ascent()), label);
        lastRight = x + w;
    }
    p.drawText(QRectF(area.left(), area.bottom() + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, title);

    const QString levelTitle = m_kind == Kind::Gain    ? (m_decibels ? tr("Gain (dB)") : tr("Gain"))
                               : m_kind == Kind::Phase ? tr("Phase (°)")
                                                       : tr("Temps de propagation de groupe (échantillons)");
    paintLevelAxis(p, area, levelTitle);
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));

    // The edges of the band. A band's labels outside it (f₁ to the left of its line, f₂ to the right), on the
    // other side where there is no room; a label that would cover the one before goes a line higher (the edges
    // of a template 200 Hz apart at 10 kHz), and is left out only when it would cover one there too.
    const analysis::FilterRecipe &r = m_design->recipe;
    const bool band = r.response == qacq::dsp::FilterResponse::BandPass || r.response == qacq::dsp::FilterResponse::BandStop;
    QPen edgePen(faded(m_text, 0.45), 1.0, Qt::DotLine);
    p.setPen(edgePen);
    double labelRight[2] = {-1e9, -1e9}; // where the labels end, on the bottom line and on the one above
    const auto edge = [&](double f, const QString &name, bool leftward) {
        const double x = std::round(xOf(f, area)) + 0.5;
        if (x < area.left() || x > area.right()) return;
        p.setPen(edgePen);
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        const double w = metrics.horizontalAdvance(name);
        double at = leftward ? x - 3.0 - w : x + 3.0;
        if (at < area.left() + 2.0) at = x + 3.0;
        if (at + w > area.right() - 2.0) at = x - 3.0 - w;
        if (at < area.left() + 2.0) return;
        for (int line = 0; line < 2; ++line)
        {
            if (at < labelRight[line] + 4.0) continue;
            p.setPen(m_muted);
            p.drawText(QPointF(at, area.bottom() - 4.0 - line * (metrics.height() + 2.0)), name);
            labelRight[line] = at + w;
            return;
        }
    };
    if (r.kind == analysis::FilterKind::Typed)
    {
        // Coefficients typed: no edge was given.
    }
    else if (r.fromTemplate)
    {
        // A template: its edges, in the order along the frequencies (each label on the side of its band).
        switch (r.response)
        {
            case qacq::dsp::FilterResponse::LowPass:
                edge(r.passEdge1Hz, QStringLiteral("fp"), true);
                edge(r.stopEdge1Hz, QStringLiteral("fa"), false);
                break;
            case qacq::dsp::FilterResponse::HighPass:
                edge(r.stopEdge1Hz, QStringLiteral("fa"), true);
                edge(r.passEdge1Hz, QStringLiteral("fp"), false);
                break;
            case qacq::dsp::FilterResponse::BandPass:
                edge(r.stopEdge1Hz, QStringLiteral("fa₁"), true);
                edge(r.passEdge1Hz, QStringLiteral("fp₁"), false);
                edge(r.passEdge2Hz, QStringLiteral("fp₂"), true);
                edge(r.stopEdge2Hz, QStringLiteral("fa₂"), false);
                break;
            case qacq::dsp::FilterResponse::BandStop:
                edge(r.passEdge1Hz, QStringLiteral("fp₁"), true);
                edge(r.stopEdge1Hz, QStringLiteral("fa₁"), false);
                edge(r.stopEdge2Hz, QStringLiteral("fa₂"), true);
                edge(r.passEdge2Hz, QStringLiteral("fp₂"), false);
                break;
        }
    }
    else if (band)
    {
        edge(r.frequency1Hz, QStringLiteral("f₁"), true);
        edge(r.frequency2Hz, QStringLiteral("f₂"), false);
    }
    else
    {
        edge(r.frequency1Hz, QStringLiteral("fc"), false);
    }
    if (templateShown()) paintTemplate(p, area, map);

    // The curves.
    p.setRenderHint(QPainter::Antialiasing, true);
    p.save();
    p.setClipRect(area.adjusted(0.0, 0.0, 1.0, 1.0));
    const auto curve = [&](bool applied) {
        QPolygonF line;
        line.reserve(static_cast<qsizetype>(cols.size()));
        const auto flush = [&] {
            if (line.size() > 1) p.drawPolyline(line);
            line.clear();
        };
        for (const Column &c : cols)
        {
            const double value = applied ? c.applied : c.designed;
            if (!std::isfinite(value))
            {
                flush();
                continue;
            }
            line << QPointF(xOf(c.frequency, area), bounded(map.toPixel(value), area));
        }
        flush();
    };
    const bool applied = appliedShown();
    p.setPen(QPen(m_accent, 1.6));
    curve(false);
    if (applied)
    {
        QPen dashed(m_applied, 1.6, Qt::DashLine);
        p.setPen(dashed);
        curve(true);
    }
    // Under the pointer.
    if (m_hovering && area.contains(m_pointer))
    {
        p.setPen(QPen(faded(m_text, 0.35), 1.0));
        p.drawLine(QPointF(m_pointer.x(), area.top()), QPointF(m_pointer.x(), area.bottom()));
        const double f = valueAt(m_pointer.x(), area);
        const auto dot = [&](double value, const QColor &colour) {
            if (!std::isfinite(value)) return;
            const double y = map.toPixel(value);
            if (y < area.top() || y > area.bottom()) return;
            p.setPen(Qt::NoPen);
            p.setBrush(colour);
            p.drawEllipse(QPointF(m_pointer.x(), y), 3.0, 3.0);
        };
        dot(designedValue(f), m_accent);
        if (applied) dot(appliedValue(f), m_applied);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
    paintLegend(p, area);
    if (m_kind == Kind::Phase && !m_design->phaseUnwrapped())
        paintNote(p, area, tr("Phase entre ±180° : ce FIR n'est pas symétrique, et ses zéros ne sont pas calculés."));
}

bool ResponsePlot::templateShown() const
{
    return m_kind == Kind::Gain && m_design && m_design->ok() && m_design->recipe.fromTemplate &&
           m_design->recipe.kind != analysis::FilterKind::Typed;
}

// The template on the gain: where it must not go, shaded (above its pass band, below it, above its stop band),
// edged by a line; at the gain of the output (the zones are those of the filter at 0 dB, times g).
void ResponsePlot::paintTemplate(QPainter &p, const QRectF &area, const plot::YMap &map)
{
    const analysis::TemplateZones z = analysis::templateZones(m_design->recipe);
    const double g = m_design->gain;
    const auto y = [&](double linear) {
        const double v = linear * g;
        const double shown = m_decibels ? (v > 0.0 ? std::max(kFloorDb, 20.0 * std::log10(v)) : kFloorDb) : v;
        return std::clamp(map.toPixel(shown), area.top(), area.bottom());
    };
    const auto x = [&](double f) { return std::clamp(xOf(f, area), area.left(), area.right()); };
    const QColor fill = faded(m_warning, 0.12);
    const QPen line(faded(m_warning, 0.75), 1.2);
    p.save();
    p.setClipRect(area);
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto zone = [&](double f0, double f1, double top, double bottom) {
        const double x0 = x(f0), x1 = x(f1);
        if (x1 - x0 < 0.5 || bottom - top < 0.5) return;
        p.fillRect(QRectF(x0, top, x1 - x0, bottom - top), fill);
    };
    const auto bound = [&](double f0, double f1, double level) {
        p.setPen(line);
        const double at = std::round(level) + 0.5;
        p.drawLine(QPointF(x(f0), at), QPointF(x(f1), at));
    };
    for (const std::array<double, 2> &b : z.passBands)
    {
        const double high = y(z.passHigh), low = y(z.passLow);
        zone(b[0], b[1], area.top(), high);
        zone(b[0], b[1], low, area.bottom());
        bound(b[0], b[1], high);
        bound(b[0], b[1], low);
    }
    for (const std::array<double, 2> &b : z.stopBands)
    {
        const double high = y(z.stopHigh);
        zone(b[0], b[1], area.top(), high);
        bound(b[0], b[1], high);
    }
    p.restore();
}

void ResponsePlot::paintSamples(QPainter &p, const QRectF &area)
{
    const QFontMetricsF metrics(p.font());
    const analysis::ImpulseResponse *r = impulse();
    if (!r) return;
    if (m_view.levelsAuto) m_view.levels = automaticLevels(area);
    const Range levels = m_view.levels;
    const plot::YMap map{levels.low, levels.high, area.top(), area.height(), false};
    const Range v = sampleView();

    // Axis of the samples.
    p.setRenderHint(QPainter::Antialiasing, false);
    plot::Ticks ticks = plot::linearTicks(v.low, v.high, area.width(), 80.0);
    // Whole samples only.
    ticks.major.erase(std::remove_if(ticks.major.begin(), ticks.major.end(), [](double n) { return n != std::round(n); }),
                      ticks.major.end());
    p.setPen(QPen(m_grid, 1.0));
    for (double n : ticks.major)
    {
        const double x = std::round(xOf(n, area)) + 0.5;
        if (x >= area.left() && x <= area.right()) p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (double n : ticks.major)
    {
        const QString label = text(plot::formatFixed(n, 0));
        const double w = metrics.horizontalAdvance(label);
        const double x = xOf(n, area) - w / 2.0;
        if (x < lastRight + 8.0 || x + w > width() - 2.0 || x + w / 2.0 < area.left() - 1.0) continue;
        p.drawText(QPointF(std::max(2.0, x), area.bottom() + 4.0 + metrics.ascent()), label);
        lastRight = x + w;
    }
    p.drawText(QRectF(area.left(), area.bottom() + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, tr("n (échantillons)"));
    paintLevelAxis(p, area, m_kind == Kind::Step ? tr("Réponse indicielle s[n]") : tr("Réponse impulsionnelle h[n]"));
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    // n = 0 and the level 0.
    const double zeroY = map.toPixel(0.0);
    p.setPen(QPen(faded(m_text, 0.3), 1.0));
    if (zeroY > area.top() && zeroY < area.bottom()) p.drawLine(QPointF(area.left(), zeroY), QPointF(area.right(), zeroY));
    const double zeroX = xOf(0.0, area);
    if (zeroX > area.left() && zeroX < area.right()) p.drawLine(QPointF(zeroX, area.top()), QPointF(zeroX, area.bottom()));

    p.setRenderHint(QPainter::Antialiasing, true);
    p.save();
    p.setClipRect(area.adjusted(0.0, 0.0, 1.0, 1.0));
    const double perPixel = v.span() / area.width();
    const bool stems = perPixel < 0.25; // at least four pixels a sample
    const auto draw = [&](const std::vector<double> &data, double start, const QColor &colour, bool dashed) {
        if (data.empty()) return;
        const double first = std::max(0.0, std::floor(v.low - start));
        const double last = std::min(static_cast<double>(data.size() - 1), std::ceil(v.high - start));
        if (last < first) return;
        const auto i0 = static_cast<std::size_t>(first), i1 = static_cast<std::size_t>(last);
        QPen pen(colour, dashed ? 1.4 : 1.5, dashed ? Qt::DashLine : Qt::SolidLine);
        if (stems)
        {
            for (std::size_t i = i0; i <= i1; ++i)
            {
                const double x = xOf(start + static_cast<double>(i), area);
                const double y = bounded(map.toPixel(data[i]), area);
                p.setPen(pen);
                p.drawLine(QPointF(x, bounded(zeroY, area)), QPointF(x, y));
                if (dashed)
                {
                    p.setPen(QPen(colour, 1.2));
                    p.setBrush(Qt::NoBrush);
                }
                else
                {
                    p.setPen(Qt::NoPen);
                    p.setBrush(colour);
                }
                p.drawEllipse(QPointF(x, y), 2.6, 2.6);
            }
            p.setBrush(Qt::NoBrush);
            return;
        }
        // Dense: the extremes of each column, joined.
        QPolygonF line;
        int column = std::numeric_limits<int>::min();
        double lo = 0.0, hi = 0.0, entry = 0.0, exit = 0.0;
        const auto flush = [&] {
            if (column == std::numeric_limits<int>::min()) return;
            const double x = area.left() + column + 0.5;
            line << QPointF(x, bounded(map.toPixel(entry), area));
            if (hi != lo)
            {
                line << QPointF(x, bounded(map.toPixel(entry <= exit ? lo : hi), area));
                line << QPointF(x, bounded(map.toPixel(entry <= exit ? hi : lo), area));
            }
            line << QPointF(x, bounded(map.toPixel(exit), area));
        };
        for (std::size_t i = i0; i <= i1; ++i)
        {
            const int c = static_cast<int>(std::floor(xOf(start + static_cast<double>(i), area) - area.left()));
            const double value = data[i];
            if (c != column)
            {
                flush();
                column = c;
                lo = hi = entry = exit = value;
                continue;
            }
            lo = std::min(lo, value);
            hi = std::max(hi, value);
            exit = value;
        }
        flush();
        p.setPen(pen);
        p.drawPolyline(line);
    };
    const bool step = m_kind == Kind::Step;
    draw(step ? r->step : r->impulse, 0.0, m_accent, false);
    const bool applied = appliedShown();
    if (applied) draw(step ? r->appliedStep : r->appliedImpulse, static_cast<double>(r->appliedStart), m_applied, true);
    if (m_hovering && area.contains(m_pointer))
    {
        const double x = xOf(std::round(valueAt(m_pointer.x(), area)), area);
        p.setPen(QPen(faded(m_text, 0.35), 1.0));
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    p.restore();
    paintLegend(p, area);
    if (!r->settled)
        paintNote(p, area, tr("Réponse coupée à %1 échantillons : elle n'est pas encore éteinte (un pôle très près du cercle).")
                               .arg(qacq::app::formatCount(static_cast<qint64>(r->impulse.size()))));
}

void ResponsePlot::paintPlane(QPainter &p, const QRectF &area)
{
    const QFontMetricsF metrics(p.font());
    const Plane pl = plane(area);
    const double reLow = pl.reAt(area.left()), reHigh = pl.reAt(area.right());
    const double imLow = pl.imAt(area.bottom()), imHigh = pl.imAt(area.top());

    // Grid, the same steps both ways.
    p.setRenderHint(QPainter::Antialiasing, false);
    const plot::Ticks ticks = plot::linearTicks(imLow, imHigh, area.height(), 40.0);
    const double step = ticks.step > 0.0 ? ticks.step : 0.5;
    p.setPen(QPen(m_grid, 1.0));
    for (double re = std::ceil(reLow / step) * step; re <= reHigh; re += step)
    {
        const double x = std::round(pl.toScreen(re, 0.0).x()) + 0.5;
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    for (double im = std::ceil(imLow / step) * step; im <= imHigh; im += step)
    {
        const double y = std::round(pl.toScreen(0.0, im).y()) + 0.5;
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    const int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(step) + 1e-9)));
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (double re = std::ceil(reLow / step) * step; re <= reHigh; re += step)
    {
        const QString label = fixed(std::abs(re) < 1e-12 ? 0.0 : re, decimals);
        const double w = metrics.horizontalAdvance(label);
        const double x = pl.toScreen(re, 0.0).x() - w / 2.0;
        if (x < lastRight + 8.0 || x + w > width() - 2.0) continue;
        p.drawText(QPointF(std::max(2.0, x), area.bottom() + 4.0 + metrics.ascent()), label);
        lastRight = x + w;
    }
    for (double im = std::ceil(imLow / step) * step; im <= imHigh; im += step)
    {
        const double y = pl.toScreen(0.0, im).y();
        p.drawText(QRectF(0.0, y - metrics.height() / 2.0, area.left() - 6.0, metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter, fixed(std::abs(im) < 1e-12 ? 0.0 : im, decimals));
    }
    p.drawText(QRectF(area.left(), area.bottom() + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, tr("Partie réelle"));
    p.drawText(QRectF(4.0, 2.0, area.width(), kTop - 4.0), Qt::AlignLeft | Qt::AlignVCenter, tr("Partie imaginaire"));
    p.setPen(QPen(m_grid, 1.0));
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));

    p.save();
    p.setClipRect(area.adjusted(0.0, 0.0, 1.0, 1.0));
    p.setRenderHint(QPainter::Antialiasing, true);
    // The axes and the unit circle.
    const QPointF o = pl.toScreen(0.0, 0.0);
    p.setPen(QPen(faded(m_text, 0.3), 1.0));
    p.drawLine(QPointF(area.left(), o.y()), QPointF(area.right(), o.y()));
    p.drawLine(QPointF(o.x(), area.top()), QPointF(o.x(), area.bottom()));
    p.setPen(QPen(faded(m_text, 0.6), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(o, pl.scale, pl.scale);
    // The edges of the band on the circle.
    const analysis::FilterRecipe &r = m_design->recipe;
    const auto mark = [&](double f) {
        const double a = 2.0 * std::numbers::pi * f / r.sampleRateHz;
        for (const double s : {1.0, -1.0})
        {
            const QPointF inner = pl.toScreen(0.94 * std::cos(a), s * 0.94 * std::sin(a));
            const QPointF outer = pl.toScreen(1.06 * std::cos(a), s * 1.06 * std::sin(a));
            p.drawLine(inner, outer);
        }
    };
    p.setPen(QPen(faded(m_text, 0.45), 1.0));
    if (r.kind != analysis::FilterKind::Typed)
    {
        mark(r.frequency1Hz);
        if (r.response == qacq::dsp::FilterResponse::BandPass || r.response == qacq::dsp::FilterResponse::BandStop)
            mark(r.frequency2Hz);
    }

    // Zeros (○) and poles (×), how many where several meet.
    int outside = 0;
    double farthest = 0.0;
    const std::vector<Root> list = roots();
    for (const Root &root : list)
    {
        const QPointF s = pl.toScreen(root.value.real(), root.value.imag());
        if (!area.adjusted(-6.0, -6.0, 6.0, 6.0).contains(s))
        {
            outside += root.count;
            farthest = std::max(farthest, std::abs(root.value));
            continue;
        }
        const bool unstable = root.pole && std::abs(root.value) >= 1.0;
        const QColor colour = unstable ? m_warning : m_accent;
        if (root.pole)
        {
            p.setPen(QPen(colour, 1.8));
            p.drawLine(s + QPointF(-4.5, -4.5), s + QPointF(4.5, 4.5));
            p.drawLine(s + QPointF(-4.5, 4.5), s + QPointF(4.5, -4.5));
        }
        else
        {
            p.setPen(QPen(colour, 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(s, 4.5, 4.5);
        }
        if (root.count > 1)
        {
            p.setPen(m_text);
            p.drawText(s + QPointF(6.0, -6.0), QString::number(root.count));
        }
    }
    p.restore();
    // Legend.
    {
        const double x = area.left() + 10.0, y = area.top() + 8.0 + metrics.ascent();
        p.setPen(m_muted);
        const QString legend = tr("○ zéros   × pôles");
        p.drawText(QPointF(x, y), legend);
    }
    if (outside > 0)
        paintNote(p, area,
                  (outside == 1 ? tr("Une racine hors du cadre (|z| = %1) : la molette éloigne la vue.").arg(fixed(farthest, 2))
                                : tr("%1 racines hors du cadre (|z| jusqu'à %2) : la molette éloigne la vue.")
                                      .arg(outside)
                                      .arg(fixed(farthest, 2))));
}

void ResponsePlot::paintLegend(QPainter &p, const QRectF &area)
{
    if (!appliedShown()) return;
    const QFontMetricsF metrics(p.font());
    double x = area.left() + 10.0;
    const double y = area.top() + 8.0 + metrics.height() / 2.0;
    const auto item = [&](const QString &name, const QColor &colour, bool dashed) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(colour, 1.6, dashed ? Qt::DashLine : Qt::SolidLine));
        p.drawLine(QPointF(x, y), QPointF(x + 22.0, y));
        p.setPen(m_text);
        p.drawText(QPointF(x + 28.0, y + metrics.ascent() / 2.0 - 1.0), name);
        x += 28.0 + metrics.horizontalAdvance(name) + 16.0;
    };
    const QRectF back(area.left() + 4.0, area.top() + 4.0,
                      62.0 + metrics.horizontalAdvance(tr("conçu")) + metrics.horizontalAdvance(appliedName()) + 34.0,
                      metrics.height() + 8.0);
    p.setPen(Qt::NoPen);
    p.setBrush(faded(m_panel, 0.85));
    p.drawRoundedRect(back, 4.0, 4.0);
    p.setBrush(Qt::NoBrush);
    item(tr("conçu"), m_accent, false);
    item(appliedName(), m_applied, true);
}

void ResponsePlot::paintReadout(QPainter &p, const QRectF &area)
{
    if (m_readout.isEmpty()) return;
    const QFontMetricsF metrics(p.font());
    const double w = std::min(area.width() - 8.0, metrics.horizontalAdvance(m_readout) + 16.0);
    const QRectF box(area.right() - w - 4.0, area.top() + 4.0, w, metrics.height() + 8.0);
    p.setPen(Qt::NoPen);
    p.setBrush(faded(m_panel, 0.9));
    p.drawRoundedRect(box, 4.0, 4.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(m_text);
    p.drawText(box.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignVCenter | Qt::AlignRight,
               metrics.elidedText(m_readout, Qt::ElideLeft, w - 16.0));
}

void ResponsePlot::paintNote(QPainter &p, const QRectF &area, const QString &note)
{
    const QFontMetricsF metrics(p.font());
    const double w = std::min(area.width() - 8.0, metrics.horizontalAdvance(note) + 16.0);
    const QRectF box(area.left() + 4.0, area.bottom() - metrics.height() - 12.0, w, metrics.height() + 8.0);
    p.setPen(Qt::NoPen);
    p.setBrush(faded(m_panel, 0.9));
    p.drawRoundedRect(box, 4.0, 4.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(m_muted);
    p.drawText(box.adjusted(8.0, 0.0, -8.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
               metrics.elidedText(note, Qt::ElideRight, w - 16.0));
}

// ------------------------------------------------------------------------------------------------ input

void ResponsePlot::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    if (!hasData() || (delta.x() == 0 && delta.y() == 0))
    {
        event->ignore();
        return;
    }
    event->accept();
    rememberView(true);
    const QRectF area = plotRect();
    if (std::abs(delta.x()) > std::abs(delta.y()))
    {
        moveBy(0.1 * area.width() * delta.x() / 120.0, 0.0);
        return;
    }
    const double factor = std::pow(0.8, delta.y() / 120.0);
    const QPointF at = event->position();
    const bool vertical = (event->modifiers() & Qt::ControlModifier) || at.x() < area.left();
    zoomAt(factor, at, vertical && m_kind != Kind::PoleZero);
}

void ResponsePlot::mousePressEvent(QMouseEvent *event)
{
    if (!hasData())
    {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    m_pressAt = m_lastAt = event->position();
    m_moved = false;
    m_direction = 0;
    m_drag = event->button() == Qt::RightButton ? Drag::Band : Drag::Move;
    setKeepMouseGrab(true);
    event->accept();
}

void ResponsePlot::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF at = event->position();
    m_pointer = at;
    if (m_drag == Drag::None) return;
    const QPointF total = at - m_pressAt;
    if (!m_moved && std::hypot(total.x(), total.y()) < 4.0) return;
    if (!m_moved)
    {
        m_moved = true;
        rememberView(false);
        const double ax = std::abs(total.x()), ay = std::abs(total.y());
        m_direction = m_kind == Kind::PoleZero ? 3 : ay < 0.5 * ax ? 1 : ax < 0.5 * ay ? 2 : 3;
        if (m_drag == Drag::Move) setCursor(QCursor(Qt::ClosedHandCursor));
    }
    if (m_drag == Drag::Move)
    {
        const QPointF step = at - m_lastAt;
        moveBy(m_direction & 1 ? step.x() : 0.0, m_direction & 2 ? step.y() : 0.0);
    }
    else
    {
        update(); // the band
    }
    m_lastAt = at;
    event->accept();
}

void ResponsePlot::mouseReleaseEvent(QMouseEvent *event)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    setKeepMouseGrab(false);
    setCursor(QCursor(Qt::CrossCursor));
    event->accept();
    if (drag != Drag::Band || !m_moved) return;
    const QRectF area = plotRect();
    const double x1 = std::clamp(std::min(m_pressAt.x(), event->position().x()), area.left(), area.right());
    const double x2 = std::clamp(std::max(m_pressAt.x(), event->position().x()), area.left(), area.right());
    if (x2 - x1 < 6.0)
    {
        update();
        return;
    }
    if (m_kind == Kind::PoleZero)
    {
        const Plane p = plane(area);
        const double y1 = std::min(m_pressAt.y(), event->position().y());
        const double y2 = std::max(m_pressAt.y(), event->position().y());
        const double half = 0.5 * std::max((y2 - y1), (x2 - x1) * area.height() / area.width()) / p.scale;
        m_view.planeX = p.reAt(0.5 * (x1 + x2));
        m_view.planeY = p.imAt(0.5 * (y1 + y2));
        m_view.planeHalf = std::max(half, 1e-9);
        m_view.planeAll = false;
        updateReadout();
        changed();
        return;
    }
    const double a = valueAt(x1, area), b = valueAt(x2, area);
    if (frequencyKind())
        setFrequencyView({a, b});
    else
        setSampleView({a, b});
}

void ResponsePlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    const QRectF area = plotRect();
    // On the scale: automatic levels only.
    if (event->position().x() < area.left() && m_kind != Kind::PoleZero)
    {
        rememberView(false);
        m_view.levelsAuto = true;
        changed();
        return;
    }
    showAll();
}

void ResponsePlot::hoverMoveEvent(QHoverEvent *event)
{
    m_hovering = true;
    m_pointer = event->position();
    updateReadout();
    update();
}

void ResponsePlot::hoverLeaveEvent(QHoverEvent *)
{
    m_hovering = false;
    updateReadout();
    update();
}

void ResponsePlot::keyPressEvent(QKeyEvent *event)
{
    if (!hasData())
    {
        event->ignore();
        return;
    }
    const QRectF area = plotRect();
    const double fraction = (event->modifiers() & Qt::ShiftModifier) ? 0.5 : 0.1;
    double dx = 0.0, dy = 0.0;
    switch (event->key())
    {
        case Qt::Key_Left: dx = fraction * area.width(); break;
        case Qt::Key_Right: dx = -fraction * area.width(); break;
        case Qt::Key_Up: dy = fraction * area.height(); break;
        case Qt::Key_Down: dy = -fraction * area.height(); break;
        default: event->ignore(); return;
    }
    rememberView(true);
    moveBy(dx, dy);
    event->accept();
}
