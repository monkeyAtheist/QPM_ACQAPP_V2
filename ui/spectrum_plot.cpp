#include "spectrum_plot.h"

#include "qacq/analysis/analysis.h"
#include "qacq/analysis/measures.h"
#include "qacq/app/analysis_controller.h"

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
#include <cmath>
#include <limits>

using qacq::plot::Range;
namespace analysis = qacq::analysis;

namespace
{
constexpr double kLeft = 62.0;
constexpr double kRight = 14.0;
constexpr double kTop = 20.0; // the unit of the levels above the axis
constexpr double kBottom = 40.0;

QString text(const std::string &s)
{
    return QString::fromStdString(s);
}

QColor faded(QColor colour, double alpha)
{
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * static_cast<double>(colour.alphaF())));
    return colour;
}

// The unit the level axis of every curve shares ("" when they differ).
std::string sharedLevelUnit(const analysis::AnalysisResult &r)
{
    std::string unit;
    for (std::size_t i = 0; i < r.curves.size(); ++i)
    {
        const std::string u = analysis::levelUnit(r.curves[i].unit, r.kind, r.scale);
        if (i == 0) unit = u;
        else if (u != unit) return {};
    }
    return unit;
}

// The highest bin of a curve in [from, to] (bins), or the nearest one to `at` when none lies inside.
std::size_t highestBin(const analysis::SpectrumCurve &c, double from, double to, double at)
{
    const auto n = static_cast<double>(c.level.size());
    if (n < 1.0) return 0;
    const double a = std::clamp(std::ceil(from), 0.0, n - 1.0);
    const double b = std::clamp(std::floor(to), 0.0, n - 1.0);
    if (b < a) return static_cast<std::size_t>(std::clamp(std::round(at), 0.0, n - 1.0));
    auto best = static_cast<std::size_t>(a);
    for (auto k = static_cast<std::size_t>(a); k <= static_cast<std::size_t>(b); ++k)
    {
        if (c.level[k] > c.level[best] || !std::isfinite(c.level[best])) best = k;
    }
    return best;
}
} // namespace

SpectrumPlot::SpectrumPlot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
    for (const QColor &c : {QColor(90, 170, 255), QColor(255, 167, 38), QColor(76, 195, 138), QColor(229, 115, 115)})
        m_channelColors << QVariant::fromValue(c);
    connect(this, &SpectrumPlot::styleChanged, this, [this] { update(); });
}

QObject *SpectrumPlot::analysis() const
{
    return m_analysis.data();
}

void SpectrumPlot::setAnalysis(QObject *object)
{
    auto *controller = qobject_cast<qacq::app::AnalysisController *>(object);
    if (controller == m_analysis.data()) return;
    for (const QMetaObject::Connection &c : m_connections) disconnect(c);
    m_connections.clear();
    m_analysis = controller;
    if (controller)
    {
        m_connections << connect(controller, &qacq::app::AnalysisController::resultChanged, this, &SpectrumPlot::onResult)
                      << connect(controller, &qacq::app::AnalysisController::busyChanged, this, [this] { update(); });
    }
    onResult();
    emit analysisChanged();
}

void SpectrumPlot::setLogFrequency(bool log)
{
    if (log == m_log) return;
    m_log = log;
    m_view = limits();
    m_history.clear();
    emit viewChanged();
    update();
}

bool SpectrumPlot::hasData() const
{
    return m_result && m_result->ok();
}

bool SpectrumPlot::zoomed() const
{
    const Range l = limits();
    return hasData() && m_view.span() < l.span() * 0.999;
}

bool SpectrumPlot::logActive() const
{
    return m_log && m_result && m_result->ok() && !m_result->twoSided && m_result->bins > 2;
}

Range SpectrumPlot::limits() const
{
    if (!m_result || !m_result->ok()) return {0.0, 1.0};
    const analysis::AnalysisResult &r = *m_result;
    const double last = r.frequencyAt(static_cast<double>(r.bins - 1));
    if (logActive()) return {std::max(r.firstFrequencyHz, r.binWidthHz), last};
    return {r.firstFrequencyHz, last};
}

void SpectrumPlot::onResult()
{
    const qacq::app::AnalysisController *controller = m_analysis.data();
    m_result = controller ? controller->result() : nullptr;
    QString axis;
    QString scale;
    if (m_result && m_result->ok())
    {
        const analysis::AnalysisResult &r = *m_result;
        axis = QString::number(r.twoSided ? 1 : 0) + QLatin1Char('|') + QString::number(r.firstFrequencyHz, 'g', 12) +
               QLatin1Char('|') + QString::number(r.frequencyAt(static_cast<double>(r.bins - 1)), 'g', 12);
        scale = QString::number(static_cast<int>(r.kind)) + QLatin1Char('|') + QString::number(static_cast<int>(r.scale));
    }
    if (axis != m_axisSignature || scale != m_scaleSignature)
    {
        // Another frequency axis (another signal, rate or zone length): everything in view; another kind
        // of spectrum or level scale: automatic levels again.
        if (axis != m_axisSignature)
        {
            m_view = limits();
            m_history.clear();
            const qacq::plot::Range l = limits();
            if (m_cursorA < l.low || m_cursorA > l.high || m_cursorB < l.low || m_cursorB > l.high) m_cursorsPlaced = false;
        }
        else m_view = qacq::plot::clampRange(m_view, limits(), 0.0);
        m_levelsAutomatic = true;
        m_axisSignature = axis;
        m_scaleSignature = scale;
        emit viewChanged();
    }
    emit contentChanged();
    if (m_cursorsVisible)
    {
        if (!m_cursorsPlaced) placeCursors();
        refreshCursors();
    }
    refreshMarker();
    noteScales();
    update();
}

Range SpectrumPlot::axisView() const
{
    if (logActive() && m_view.low > 0.0) return {std::log10(m_view.low), std::log10(m_view.high)};
    return m_view;
}

void SpectrumPlot::setAxisView(const Range &range)
{
    if (!m_result || !m_result->ok()) return;
    const Range l = limits();
    const bool log = logActive();
    const Range axisLimits = log ? Range{std::log10(l.low), std::log10(l.high)} : l;
    const double minimum = log ? 0.002 : 4.0 * m_result->binWidthHz;
    const Range r = qacq::plot::clampRange(range, axisLimits, std::min(minimum, axisLimits.span()));
    if (!r.valid()) return;
    m_view = log ? Range{std::pow(10.0, r.low), std::pow(10.0, r.high)} : r;
    emit viewChanged();
    update();
}

void SpectrumPlot::showAll()
{
    rememberView();
    m_view = limits();
    m_levelsAutomatic = true;
    emit viewChanged();
    noteScales();
    update();
}

void SpectrumPlot::autoScale()
{
    if (m_levelsAutomatic) return;
    rememberView();
    m_levelsAutomatic = true;
    noteScales();
    update();
}

void SpectrumPlot::setLevels(const Range &levels)
{
    if (!levels.valid()) return;
    m_levels = levels;
    m_levelsAutomatic = false;
    noteScales();
    update();
}

QVariantMap SpectrumPlot::scaleInfo(const QString &key) const
{
    QVariantMap m;
    if (!hasData()) return m;
    const analysis::AnalysisResult &r = *m_result;
    m.insert(QStringLiteral("key"), key);
    m.insert(QStringLiteral("lowLabel"), tr("Minimum"));
    m.insert(QStringLiteral("highLabel"), tr("Maximum"));
    if (key == QStringLiteral("frequency"))
    {
        m.insert(QStringLiteral("kind"), QStringLiteral("frequency"));
        m.insert(QStringLiteral("title"), logActive() ? tr("Fréquences (échelle logarithmique)") : tr("Fréquences"));
        m.insert(QStringLiteral("unit"), QStringLiteral("Hz"));
        m.insert(QStringLiteral("low"), m_view.low);
        m.insert(QStringLiteral("high"), m_view.high);
        m.insert(QStringLiteral("automatic"), !zoomed());
        m.insert(QStringLiteral("autoText"), tr("Toutes les fréquences"));
        m.insert(QStringLiteral("positive"), logActive());
        return m;
    }
    if (key == QStringLiteral("levels"))
    {
        const bool decibel = r.scale == analysis::LevelScale::Decibel;
        std::string unit = sharedLevelUnit(r);
        if (unit.empty() && decibel) unit = "dB"; // each curve its reference: dB all the same
        m.insert(QStringLiteral("kind"), QStringLiteral("levels"));
        m.insert(QStringLiteral("title"), unit.empty() ? tr("Niveaux") : tr("Niveaux (%1)").arg(text(unit)));
        m.insert(QStringLiteral("unit"), text(unit));
        m.insert(QStringLiteral("low"), m_levels.low);
        m.insert(QStringLiteral("high"), m_levels.high);
        m.insert(QStringLiteral("automatic"), m_levelsAutomatic);
        m.insert(QStringLiteral("autoText"), tr("Automatique"));
        return m;
    }
    return {};
}

bool SpectrumPlot::setScaleRange(const QString &key, double low, double high)
{
    if (!hasData() || !std::isfinite(low) || !std::isfinite(high) || !(high > low)) return false;
    if (key == QStringLiteral("frequency"))
    {
        if (logActive() && !(low > 0.0)) return false;
        rememberView();
        setAxisView(logActive() ? Range{std::log10(low), std::log10(high)} : Range{low, high});
        return true;
    }
    if (key == QStringLiteral("levels"))
    {
        rememberView();
        setLevels({low, high});
        return true;
    }
    return false;
}

void SpectrumPlot::setScaleAutomatic(const QString &key)
{
    if (!hasData()) return;
    if (key == QStringLiteral("frequency"))
    {
        rememberView();
        m_view = limits();
        emit viewChanged();
        update();
    }
    else if (key == QStringLiteral("levels"))
    {
        autoScale();
    }
}

void SpectrumPlot::noteScales()
{
    const bool manual = manualScales();
    if (manual == m_manualScalesNoted) return;
    m_manualScalesNoted = manual;
    emit scalesChanged();
}

SpectrumPlot::Place SpectrumPlot::placeAt(const QPointF &at) const
{
    if (!hasData()) return Place::None;
    const QRectF area = plotRect();
    const bool overWidth = at.x() >= area.left() && at.x() <= area.right();
    if (at.y() > area.bottom()) return overWidth ? Place::FrequencyAxis : Place::None;
    if (at.x() < area.left()) return at.y() >= area.top() ? Place::Scale : Place::None;
    return overWidth ? Place::Plot : Place::None; // above the frame too (the tags of the cursors)
}

void SpectrumPlot::updateCursorShape()
{
    const Place place = placeAt(m_pointer);
    Qt::CursorShape shape = Qt::CrossCursor;
    if (place == Place::Scale) shape = Qt::SizeVerCursor;
    else if (cursorNear(m_pointer.x()) != 0 || place == Place::FrequencyAxis) shape = Qt::SizeHorCursor;
    setCursor(QCursor(shape));
}

void SpectrumPlot::zoomFrequency(double factor)
{
    rememberView();
    const Range a = axisView();
    setAxisView(qacq::plot::zoomRange(a, factor, 0.5 * (a.low + a.high)));
}

namespace
{
Range axisOf(const Range &frequencies, bool log)
{
    return log ? Range{std::log10(frequencies.low), std::log10(frequencies.high)} : frequencies;
}
} // namespace

double SpectrumPlot::viewStart() const
{
    if (!zoomed()) return 0.0;
    const Range l = axisOf(limits(), logActive());
    return l.span() > 0.0 ? std::clamp((axisView().low - l.low) / l.span(), 0.0, 1.0) : 0.0;
}

double SpectrumPlot::viewSize() const
{
    if (!zoomed()) return 1.0;
    const Range l = axisOf(limits(), logActive());
    return l.span() > 0.0 ? std::clamp(axisView().span() / l.span(), 0.0, 1.0) : 1.0;
}

void SpectrumPlot::scrollTo(double start)
{
    if (!std::isfinite(start) || !hasData()) return;
    rememberView(true);
    const Range l = axisOf(limits(), logActive());
    const Range a = axisView();
    const double low = l.low + start * l.span();
    setAxisView({low, low + a.span()});
}

void SpectrumPlot::scrollBy(double viewFraction)
{
    if (!std::isfinite(viewFraction) || !hasData()) return;
    rememberView(true);
    const Range a = axisView();
    const double shift = viewFraction * a.span();
    setAxisView({a.low + shift, a.high + shift});
}

void SpectrumPlot::keyPressEvent(QKeyEvent *event)
{
    if (!hasData())
    {
        event->ignore();
        return;
    }
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const Range l = axisOf(limits(), logActive());
    const Range a = axisView();
    switch (event->key())
    {
    case Qt::Key_Left: rememberView(true); scrollBy(shift ? -0.5 : -0.1); break;
    case Qt::Key_Right: rememberView(true); scrollBy(shift ? 0.5 : 0.1); break;
    case Qt::Key_Home: rememberView(); setAxisView({l.low, l.low + a.span()}); break;
    case Qt::Key_End: rememberView(); setAxisView({l.high - a.span(), l.high}); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomFrequency(0.8); break;
    case Qt::Key_Minus: zoomFrequency(1.25); break;
    case Qt::Key_Up:
    case Qt::Key_Down: // up shows higher levels
        rememberView(true);
        setLevels(qacq::plot::shiftRange(m_levels, (event->key() == Qt::Key_Up ? 1.0 : -1.0) * (shift ? 0.5 : 0.1)));
        break;
    default: event->ignore(); return;
    }
    event->accept();
}

QRectF SpectrumPlot::plotRect() const
{
    return QRectF(kLeft, kTop, std::max(0.0, width() - kLeft - kRight), std::max(0.0, height() - kTop - kBottom));
}

double SpectrumPlot::frequencyAt(double x) const
{
    const QRectF area = plotRect();
    const Range a = axisView();
    const double v = area.width() > 0.0 ? a.low + (x - area.left()) / area.width() * a.span() : a.low;
    return logActive() ? std::pow(10.0, v) : v;
}

double SpectrumPlot::xOf(double frequency) const
{
    const QRectF area = plotRect();
    const Range a = axisView();
    double v = frequency;
    if (logActive()) v = frequency > 0.0 ? std::log10(frequency) : -std::numeric_limits<double>::infinity();
    return area.left() + (v - a.low) / a.span() * area.width();
}

QColor SpectrumPlot::channelColour(int index) const
{
    if (m_channelColors.isEmpty()) return m_accent;
    const QVariant v = m_channelColors.at(index % m_channelColors.size());
    QColor c = v.value<QColor>();
    if (!c.isValid()) c = QColor(v.toString());
    return c.isValid() ? c : m_accent;
}

void SpectrumPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    QFont font = p.font();
    font.setPixelSize(std::max(8, m_fontSize));
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF area = plotRect();
    if (area.width() < 40.0 || area.height() < 30.0) return;

    if (!m_result || !m_result->ok())
    {
        QString message = m_emptyText;
        const qacq::app::AnalysisController *controller = m_analysis.data();
        if (controller && controller->busy()) message = tr("Calcul du spectre…");
        else if (m_result && !m_result->error.empty()) message = text(m_result->error);
        p.setPen(m_muted);
        p.drawText(boundingRect().adjusted(24.0, 0.0, -24.0, 0.0), Qt::AlignCenter | Qt::TextWordWrap, message);
        return;
    }
    const analysis::AnalysisResult &r = *m_result;
    const bool log = logActive();
    if (!m_view.valid() || (log && !(m_view.low > 0.0))) m_view = limits();
    const int columns = static_cast<int>(std::floor(area.width()));

    // The curves, column by column.
    std::vector<qacq::plot::Trace> traces(r.curves.size());
    std::vector<double> edges;
    if (log)
    {
        edges.resize(static_cast<std::size_t>(columns) + 1);
        for (int c = 0; c <= columns; ++c) edges[static_cast<std::size_t>(c)] = r.binAt(frequencyAt(area.left() + c));
    }
    const qacq::plot::XWindow window{r.binAt(m_view.low), m_view.span() / r.binWidthHz / columns, columns};
    double mn = std::numeric_limits<double>::infinity(), mx = -mn;
    for (std::size_t i = 0; i < r.curves.size(); ++i)
    {
        const analysis::SpectrumCurve &c = r.curves[i];
        const qacq::plot::ChannelView view{c.level, 0.0, {}, c.pyramid.levelCount() > 0 ? &c.pyramid : nullptr};
        // Peaks of each column, as an analyser shows a spectrum: the notches between lines are not filled.
        traces[i] = log ? qacq::plot::buildTraceEdges(view, edges, qacq::plot::Columns::Peaks)
                        : qacq::plot::buildTrace(view, window, qacq::plot::Columns::Peaks);
        if (!traces[i].empty())
        {
            mn = std::min(mn, traces[i].minimum);
            mx = std::max(mx, traces[i].maximum);
        }
    }
    if (m_levelsAutomatic) m_levels = analysis::levelRange(mn, mx, r.scale);
    const qacq::plot::YMap map{m_levels.low, m_levels.high, area.top(), area.height(), false};
    const std::string unit = sharedLevelUnit(r);
    const bool decibel = r.scale == analysis::LevelScale::Decibel;

    // Frequency axis.
    p.setRenderHint(QPainter::Antialiasing, false);
    qacq::plot::Ticks xTicks;
    std::vector<std::string> xText;
    QString xTitle = tr("Fréquence");
    if (log)
    {
        xTicks = qacq::plot::logTicks(m_view.low, m_view.high, area.width(), 70.0);
        xText = qacq::plot::logAxisLabels(xTicks, "Hz");
    }
    else
    {
        xTicks = qacq::plot::linearTicks(m_view.low, m_view.high, area.width(), 90.0);
        const qacq::plot::AxisLabels labels = qacq::plot::axisLabels(xTicks, "Hz");
        xText = labels.text;
        if (!labels.unit.empty()) xTitle += QStringLiteral(" (%1)").arg(text(labels.unit));
    }
    p.setPen(QPen(log ? faded(m_grid, 0.55) : faded(m_muted, 0.6), 1.0));
    for (double f : xTicks.minor)
    {
        const double x = std::round(xOf(f)) + 0.5;
        if (x < area.left() || x > area.right()) continue;
        if (log) p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        else p.drawLine(QPointF(x, area.bottom()), QPointF(x, area.bottom() + 3.0));
    }
    p.setPen(QPen(m_grid, 1.0));
    for (double f : xTicks.major)
    {
        const double x = std::round(xOf(f)) + 0.5;
        if (x >= area.left() && x <= area.right()) p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (std::size_t i = 0; i < xTicks.major.size() && i < xText.size(); ++i)
    {
        const QString label = text(xText[i]);
        const double w = metrics.horizontalAdvance(label);
        const double x = xOf(xTicks.major[i]) - w / 2.0;
        if (x < lastRight + 8.0 || x + w > width() - 2.0 || x + w / 2.0 < area.left() - 1.0) continue;
        p.drawText(QPointF(std::max(2.0, x), area.bottom() + 4.0 + metrics.ascent()), label);
        lastRight = x + w;
    }
    p.drawText(QRectF(area.left(), area.bottom() + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, xTitle);

    // Level axis.
    const qacq::plot::Ticks yTicks = qacq::plot::linearTicks(m_levels.low, m_levels.high, area.height(), 28.0);
    const qacq::plot::AxisLabels yLabels = qacq::plot::axisLabels(yTicks, decibel ? std::string() : unit);
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    for (std::size_t i = 0; i < yTicks.major.size(); ++i)
    {
        const double y = std::round(map.toPixel(yTicks.major[i])) + 0.5;
        if (y < area.top() || y > area.bottom()) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        p.setPen(m_muted);
        p.drawText(QRectF(0.0, y - metrics.height() / 2.0, area.left() - 6.0, metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter, text(yLabels.text[i]));
    }
    const QString levelTitle = decibel ? text(unit) : text(yLabels.unit);

    // Where the names of the curves go (top left): the marks of the harmonics keep clear of them.
    std::vector<QRectF> chips;
    QStringList chipNames;
    std::vector<int> chipChannels;
    {
        double chipX = area.left() + 8.0;
        const double chipY = area.top() + 5.0;
        for (std::size_t i = 0; i < r.curves.size(); ++i)
        {
            const analysis::SpectrumCurve &c = r.curves[i];
            QString name = text(c.name);
            // dB: each its own reference; linear: the prefixed unit of the axis, or each its own when they differ.
            QString shown = decibel ? text(analysis::levelUnit(c.unit, r.kind, r.scale)) : text(yLabels.unit);
            if (!decibel && unit.empty()) shown = text(analysis::levelUnit(c.unit, r.kind, r.scale));
            if (!shown.isEmpty()) name += QStringLiteral(" (%1)").arg(shown);
            const double w = metrics.horizontalAdvance(name) + 18.0;
            chips.push_back(QRectF(chipX - 4.0, chipY - 2.0, w + 4.0, metrics.height() + 4.0));
            chipNames << name;
            chipChannels.push_back(c.channels.empty() ? static_cast<int>(i) : c.channels.front());
            chipX += w + 8.0;
        }
    }
    const auto onChip = [&chips](const QRectF &rect) {
        return std::any_of(chips.begin(), chips.end(), [&rect](const QRectF &c) { return c.intersects(rect); });
    };

    // The curves.
    p.save();
    p.setClipRect(area.adjusted(1.0, 1.0, -1.0, -1.0));
    for (std::size_t i = 0; i < r.curves.size(); ++i)
    {
        const qacq::plot::Trace &trace = traces[i];
        const int channel = r.curves[i].channels.empty() ? static_cast<int>(i) : r.curves[i].channels.front();
        p.setRenderHint(QPainter::Antialiasing, !trace.dense);
        p.setPen(QPen(channelColour(channel), trace.dense ? 1.0 : 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const std::vector<qacq::plot::PixelPoint> &line : qacq::plot::traceLines(trace, map))
        {
            if (line.size() == 1)
            {
                p.drawPoint(QPointF(area.left() + static_cast<double>(line[0].x), static_cast<double>(line[0].y)));
                continue;
            }
            QPolygonF polygon;
            polygon.reserve(static_cast<qsizetype>(line.size()));
            for (const qacq::plot::PixelPoint &pt : line)
                polygon << QPointF(area.left() + static_cast<double>(pt.x), static_cast<double>(pt.y));
            p.drawPolyline(polygon);
        }
    }

    // Harmonics found by the tone measurement.
    if (m_showHarmonics && r.kind == analysis::SpectrumKind::Amplitude)
    {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const analysis::SpectrumCurve &c : r.curves)
        {
            if (c.channels.empty()) continue;
            const auto m = static_cast<std::size_t>(c.channels.front());
            if (m >= r.measures.size() || !r.measures[m].tone.detected) continue;
            const analysis::ChannelMeasures &measures = r.measures[m];
            const QColor colour = channelColour(c.channels.front());
            double lastLabel = -1e9;
            double lastMark = -1e9;
            for (std::size_t j = 0; j < measures.tone.harmonics.size(); ++j)
            {
                const qacq::dsp::Harmonic &h = measures.tone.harmonics[j];
                // Only what stands out of the noise: a harmonic at the level of the noise (or a numerical
                // zero) is in the table of the panel, not pointed at on the plot.
                if (j >= measures.harmonicStandsOut.size() || !measures.harmonicStandsOut[j]) continue;
                if (h.frequencyHz < m_view.low || h.frequencyHz > m_view.high) continue;
                const double centre = r.binAt(h.frequencyHz);
                const std::size_t k = highestBin(c, centre - 1.0, centre + 1.0, centre);
                const double x = xOf(r.frequencyAt(static_cast<double>(k)));
                const double y = map.toPixel(c.level[k]);
                if (!std::isfinite(y) || y < area.top() || y > area.bottom()) continue;
                // Harmonics closer than a few pixels: the view is too wide to tell them apart.
                if (x - lastMark < 6.0) break;
                lastMark = x;
                const QString label = QStringLiteral("H%1").arg(h.order);
                const double w = metrics.horizontalAdvance(label);
                // Above the peak, or beside it when the peak touches the top of the plot.
                const bool above = y - 12.0 - metrics.ascent() >= area.top();
                p.setPen(Qt::NoPen);
                p.setBrush(colour);
                if (above)
                {
                    const QPointF marker[3] = {QPointF(x, y - 4.0), QPointF(x - 3.5, y - 10.0), QPointF(x + 3.5, y - 10.0)};
                    p.drawPolygon(marker, 3);
                }
                else
                {
                    const QPointF marker[3] = {QPointF(x + 4.0, y), QPointF(x + 10.0, y - 3.5), QPointF(x + 10.0, y + 3.5)};
                    p.drawPolygon(marker, 3);
                }
                p.setBrush(Qt::NoBrush);
                const QPointF at = above ? QPointF(x - w / 2.0, y - 12.0)
                                         : QPointF(x + 12.0, y + 0.5 * metrics.ascent() - 1.0);
                const QRectF labelRect(at.x(), at.y() - metrics.ascent(), w, metrics.height());
                if (labelRect.left() < lastLabel + 4.0 || onChip(labelRect)) continue;
                p.setPen(faded(m_text, 0.85));
                p.drawText(at, label);
                lastLabel = labelRect.right();
            }
        }
    }
    p.restore();

    // Names of the curves (drawn last, over the curves).
    p.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t i = 0; i < chips.size(); ++i)
    {
        const QRectF &chip = chips[i];
        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_panel, 0.85));
        p.drawRoundedRect(chip, 4.0, 4.0);
        p.setBrush(channelColour(chipChannels[i]));
        p.drawRoundedRect(QRectF(chip.left() + 4.0, chip.center().y() - 2.0, 10.0, 4.0), 1.5, 1.5);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(QPointF(chip.left() + 19.0, chip.top() + 2.0 + metrics.ascent()), chipNames[i]);
    }
    if (!levelTitle.isEmpty())
    {
        // Above the axis: right-aligned on the graduations, or from the left edge when longer ("dB re 1 A").
        const double w = metrics.horizontalAdvance(levelTitle);
        const bool fits = w <= area.left() - 8.0;
        p.setPen(m_muted);
        p.drawText(QRectF(fits ? 0.0 : 4.0, area.top() - metrics.height() - 3.0, fits ? area.left() - 6.0 : w + 2.0,
                          metrics.height()),
                   (fits ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignTop, levelTitle);
    }

    if (m_selecting)
    {
        const double x0 = std::clamp(std::min(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        const double x1 = std::clamp(std::max(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        p.setPen(QPen(faded(m_accent, 0.8), 1.0));
        p.setBrush(faded(m_accent, 0.15));
        p.drawRect(QRectF(x0, area.top(), x1 - x0, area.height()));
        p.setBrush(Qt::NoBrush);
    }
    drawCursorsAndMarker(p, area, map);
    if (m_pointerInside && !m_panning && !m_selecting && m_dragging == 0 && area.contains(m_pointer))
        drawReadout(p, area, map);
}

void SpectrumPlot::drawReadout(QPainter &p, const QRectF &area, const qacq::plot::YMap &map) const
{
    const analysis::AnalysisResult &r = *m_result;
    const QFontMetricsF metrics(p.font());
    const double from = r.binAt(frequencyAt(m_pointer.x() - 0.5));
    const double to = r.binAt(frequencyAt(m_pointer.x() + 0.5));
    const double at = r.binAt(frequencyAt(m_pointer.x()));
    const bool decibel = r.scale == analysis::LevelScale::Decibel;
    QStringList lines;
    std::vector<QColor> colours;
    double shownFrequency = frequencyAt(m_pointer.x());
    p.setRenderHint(QPainter::Antialiasing, true);
    for (std::size_t i = 0; i < r.curves.size(); ++i)
    {
        const analysis::SpectrumCurve &c = r.curves[i];
        if (c.level.empty()) continue;
        const std::size_t k = highestBin(c, from, to, at);
        const double f = r.frequencyAt(static_cast<double>(k));
        if (i == 0) shownFrequency = f;
        const double level = c.level[k];
        const int channel = c.channels.empty() ? static_cast<int>(i) : c.channels.front();
        const double y = map.toPixel(level);
        const double x = xOf(f);
        if (std::isfinite(y) && y >= area.top() && y <= area.bottom() && x >= area.left() && x <= area.right())
        {
            p.setPen(QPen(m_background, 1.5));
            p.setBrush(channelColour(channel));
            p.drawEllipse(QPointF(x, y), 3.5, 3.5);
            p.setBrush(Qt::NoBrush);
        }
        const std::string unit = analysis::levelUnit(c.unit, r.kind, r.scale);
        QString value;
        if (decibel)
        {
            const std::string linearUnit = analysis::levelUnit(c.unit, r.kind, analysis::LevelScale::Linear);
            const double linear = analysis::levelOf(c.linear[k], r.kind, analysis::LevelScale::Linear);
            value = QStringLiteral("%1 %2 (%3)").arg(text(qacq::plot::formatFixed(level, 2)), text(unit),
                                                    text(qacq::plot::formatQuantity(linear, linearUnit, 4)));
        }
        else
        {
            value = text(qacq::plot::formatQuantity(level, unit, 4));
        }
        lines << QStringLiteral("%1 : %2").arg(text(c.name), value);
        colours.push_back(channelColour(channel));
    }
    const double x = std::round(xOf(shownFrequency)) + 0.5;
    p.setPen(QPen(faded(m_text, 0.35), 1.0, Qt::DotLine));
    p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));

    const int digits = qacq::plot::significantFor(shownFrequency, r.binWidthHz, 4, 12);
    const QString head = QStringLiteral("f = %1").arg(text(qacq::plot::formatQuantity(shownFrequency, "Hz", digits)));
    double w = metrics.horizontalAdvance(head);
    for (const QString &l : lines) w = std::max(w, metrics.horizontalAdvance(l) + 14.0);
    const double lineHeight = metrics.height() + 2.0;
    const double h = lineHeight * static_cast<double>(lines.size() + 1) + 8.0;
    w += 16.0;
    double bx = m_pointer.x() + 14.0;
    double by = m_pointer.y() + 14.0;
    if (bx + w > area.right()) bx = m_pointer.x() - 14.0 - w;
    if (by + h > area.bottom()) by = std::max(area.top(), m_pointer.y() - 14.0 - h);
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(faded(m_panel, 0.94));
    p.drawRoundedRect(QRectF(bx, by, w, h), 6.0, 6.0);
    p.setPen(m_muted);
    p.drawText(QPointF(bx + 8.0, by + 4.0 + metrics.ascent()), head);
    for (qsizetype k = 0; k < lines.size(); ++k)
    {
        const double ly = by + 4.0 + lineHeight * static_cast<double>(k + 1);
        p.setPen(Qt::NoPen);
        p.setBrush(colours[static_cast<std::size_t>(k)]);
        p.drawEllipse(QPointF(bx + 11.0, ly + metrics.height() / 2.0), 3.0, 3.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(QPointF(bx + 20.0, ly + metrics.ascent()), lines.at(k));
    }
}

void SpectrumPlot::setCursorsVisible(bool visible)
{
    if (visible == m_cursorsVisible) return;
    m_cursorsVisible = visible;
    if (visible && (!m_cursorsPlaced || m_cursorA < m_view.low || m_cursorA > m_view.high || m_cursorB < m_view.low ||
                    m_cursorB > m_view.high))
        placeCursors();
    refreshCursors();
    update();
}

void SpectrumPlot::placeCursors()
{
    if (!hasData()) return;
    const QRectF area = plotRect();
    m_cursorA = frequencyAt(area.left() + area.width() / 3.0);
    m_cursorB = frequencyAt(area.left() + 2.0 * area.width() / 3.0);
    m_cursorsPlaced = true;
}

int SpectrumPlot::cursorNear(double x) const
{
    if (!m_cursorsVisible || !hasData()) return 0;
    const double da = std::abs(x - xOf(m_cursorA)), db = std::abs(x - xOf(m_cursorB));
    if (std::min(da, db) > 6.0) return 0;
    return da <= db ? 1 : 2;
}

void SpectrumPlot::rememberView(bool coalesce)
{
    if (!hasData()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameGesture = coalesce && now - m_lastRemembered < 600;
    m_lastRemembered = coalesce ? now : 0;
    if (sameGesture) return;
    if (!m_history.empty() && m_history.back().frequencies.low == m_view.low &&
        m_history.back().frequencies.high == m_view.high && m_history.back().levels.low == m_levels.low &&
        m_history.back().levels.high == m_levels.high)
        return;
    m_history.push_back({m_view, m_levels, m_levelsAutomatic});
    if (m_history.size() > 50) m_history.erase(m_history.begin());
    emit viewChanged();
}

void SpectrumPlot::goBack()
{
    if (m_history.empty()) return;
    const View previous = m_history.back();
    m_history.pop_back();
    m_view = previous.frequencies;
    m_levels = previous.levels;
    m_levelsAutomatic = previous.automatic;
    emit viewChanged();
    noteScales();
    update();
}

void SpectrumPlot::refreshCursors()
{
    QVariantMap info;
    if (m_cursorsVisible && hasData())
    {
        const analysis::AnalysisResult &r = *m_result;
        const double resolution = r.binWidthHz;
        const auto freq = [&](double f) {
            return text(qacq::plot::formatQuantity(f, "Hz", qacq::plot::significantFor(f, resolution, 3, 10)));
        };
        const double df = m_cursorB - m_cursorA;
        info[QStringLiteral("a")] = freq(m_cursorA);
        info[QStringLiteral("b")] = freq(m_cursorB);
        info[QStringLiteral("delta")] = freq(df);
        info[QStringLiteral("inverse")] = QString();
        const bool decibel = r.scale == analysis::LevelScale::Decibel;
        QVariantList rows;
        for (std::size_t i = 0; i < r.curves.size(); ++i)
        {
            const analysis::SpectrumCurve &c = r.curves[i];
            if (c.level.empty()) continue;
            const auto levelAt = [&](double f) {
                const double k = std::clamp(std::round(r.binAt(f)), 0.0, static_cast<double>(c.level.size() - 1));
                return c.level[static_cast<std::size_t>(k)];
            };
            const std::string unit = analysis::levelUnit(c.unit, r.kind, r.scale);
            const double la = levelAt(m_cursorA), lb = levelAt(m_cursorB);
            const auto level = [&](double v) {
                return decibel ? text(qacq::plot::formatFixed(v, 2)) + QStringLiteral(" ") + text(unit)
                               : text(qacq::plot::formatQuantity(v, unit, 4));
            };
            const QString delta = decibel ? text(qacq::plot::formatFixed(lb - la, 2)) + QStringLiteral(" dB")
                                          : text(qacq::plot::formatQuantity(lb - la, unit, 4));
            // What lies in the band: its rms value (and the same in dB for a decibel scale).
            const double meanSquare = analysis::bandMeanSquare(r, i, m_cursorA, m_cursorB);
            const double rms = std::sqrt(std::max(meanSquare, 0.0));
            QString stats = tr("bande A-B : efficace %1").arg(text(qacq::plot::formatQuantity(rms, c.unit, 4)));
            if (decibel && rms > 0.0)
            {
                const std::string amplitudeUnit = analysis::levelUnit(c.unit, analysis::SpectrumKind::Amplitude,
                                                                       analysis::LevelScale::Decibel);
                stats += QStringLiteral(" (%1 %2)").arg(text(qacq::plot::formatFixed(20.0 * std::log10(rms), 2)),
                                                        text(amplitudeUnit));
            }
            rows << QVariantMap{{QStringLiteral("name"), text(c.name)},
                                {QStringLiteral("channel"), c.channels.empty() ? static_cast<int>(i) : c.channels.front()},
                                {QStringLiteral("a"), level(la)},
                                {QStringLiteral("b"), level(lb)},
                                {QStringLiteral("delta"), delta},
                                {QStringLiteral("stats"), stats}};
        }
        info[QStringLiteral("rows")] = rows;
    }
    m_cursorInfo = info;
    emit cursorsChanged();
}

void SpectrumPlot::markPeak(int step)
{
    if (!hasData()) return;
    const analysis::AnalysisResult &r = *m_result;
    if (step == 0 || m_markerCurve < 0 || m_markerCurve >= static_cast<int>(r.curves.size()))
    {
        // The curve whose highest peak is the highest.
        m_markerCurve = -1;
        double best = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < r.curves.size(); ++i)
        {
            if (!r.curves[i].peaks.empty() && r.curves[i].peaks.front().level > best)
            {
                best = r.curves[i].peaks.front().level;
                m_markerCurve = static_cast<int>(i);
            }
        }
        m_markerIndex = 0;
    }
    else
    {
        const auto count = static_cast<int>(r.curves[static_cast<std::size_t>(m_markerCurve)].peaks.size());
        m_markerIndex = std::clamp(m_markerIndex + step, 0, std::max(0, count - 1));
    }
    refreshMarker();
    // The marked peak brought into view.
    if (m_markerCurve >= 0)
    {
        const auto &peaks = r.curves[static_cast<std::size_t>(m_markerCurve)].peaks;
        if (m_markerIndex < static_cast<int>(peaks.size()))
        {
            const double f = peaks[static_cast<std::size_t>(m_markerIndex)].frequencyHz;
            if (f < m_view.low || f > m_view.high)
            {
                rememberView();
                const Range a = axisView();
                const double centre = logActive() ? std::log10(std::max(f, 1e-300)) : f;
                setAxisView({centre - 0.5 * a.span(), centre + 0.5 * a.span()});
            }
        }
    }
    update();
}

void SpectrumPlot::clearMarker()
{
    m_markerCurve = -1;
    refreshMarker();
    update();
}

void SpectrumPlot::refreshMarker()
{
    QString textNow;
    if (hasData() && m_markerCurve >= 0 && m_markerCurve < static_cast<int>(m_result->curves.size()))
    {
        const analysis::AnalysisResult &r = *m_result;
        const analysis::SpectrumCurve &c = r.curves[static_cast<std::size_t>(m_markerCurve)];
        if (m_markerIndex < static_cast<int>(c.peaks.size()))
        {
            const analysis::Peak &peak = c.peaks[static_cast<std::size_t>(m_markerIndex)];
            const std::string unit = analysis::levelUnit(c.unit, r.kind, r.scale);
            const std::string linearUnit = analysis::levelUnit(c.unit, r.kind, analysis::LevelScale::Linear);
            const double linear = analysis::levelOf(peak.linear, r.kind, analysis::LevelScale::Linear);
            const QString level =
                r.scale == analysis::LevelScale::Decibel
                    ? QStringLiteral("%1 %2 (%3)").arg(text(qacq::plot::formatFixed(peak.level, 2)), text(unit),
                                                      text(qacq::plot::formatQuantity(linear, linearUnit, 4)))
                    : text(qacq::plot::formatQuantity(peak.level, unit, 4));
            textNow = tr("%1, pic %2 sur %3 : %4 · %5")
                          .arg(text(c.name))
                          .arg(m_markerIndex + 1)
                          .arg(static_cast<int>(c.peaks.size()))
                          .arg(text(qacq::plot::formatQuantity(peak.frequencyHz, "Hz",
                                                               qacq::plot::significantFor(peak.frequencyHz, r.binWidthHz * 0.1, 4, 10))),
                               level);
        }
        else
        {
            m_markerCurve = -1;
        }
    }
    else
    {
        m_markerCurve = -1;
    }
    if (textNow != m_markerText)
    {
        m_markerText = textNow;
        emit markerChanged();
    }
}

void SpectrumPlot::drawCursorsAndMarker(QPainter &p, const QRectF &area, const qacq::plot::YMap &map) const
{
    const QFontMetricsF metrics(p.font());
    p.save();
    p.setClipRect(QRectF(area.left(), area.top() - 20.0, area.width(), area.height() + 20.0));
    if (m_cursorsVisible && hasData())
    {
        const double xa = xOf(m_cursorA), xb = xOf(m_cursorB);
        p.setRenderHint(QPainter::Antialiasing, false);
        const double x0 = std::clamp(std::min(xa, xb), area.left(), area.right());
        const double x1 = std::clamp(std::max(xa, xb), area.left(), area.right());
        if (x1 > x0) p.fillRect(QRectF(x0, area.top(), x1 - x0, area.height()), faded(m_accent, 0.07));
        const QColor colours[2] = {m_accent, QColor(229, 115, 115)};
        const double xs[2] = {xa, xb};
        const QString names[2] = {QStringLiteral("A"), QStringLiteral("B")};
        for (int k = 0; k < 2; ++k)
        {
            if (xs[k] < area.left() - 1.0 || xs[k] > area.right() + 1.0) continue;
            const double x = std::round(xs[k]) + 0.5;
            p.setPen(QPen(colours[k], 1.0, Qt::DashLine));
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            const double w = metrics.horizontalAdvance(names[k]) + 8.0;
            const QRectF tag(x - w / 2.0, area.top() - metrics.height() - 2.0, w, metrics.height() + 2.0);
            p.setPen(Qt::NoPen);
            p.setBrush(colours[k]);
            p.drawRoundedRect(tag, 3.0, 3.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(m_background);
            p.drawText(tag, Qt::AlignCenter, names[k]);
        }
    }
    if (hasData() && m_markerCurve >= 0 && m_markerCurve < static_cast<int>(m_result->curves.size()))
    {
        const analysis::SpectrumCurve &c = m_result->curves[static_cast<std::size_t>(m_markerCurve)];
        if (m_markerIndex < static_cast<int>(c.peaks.size()))
        {
            const analysis::Peak &peak = c.peaks[static_cast<std::size_t>(m_markerIndex)];
            const double x = xOf(peak.frequencyHz), y = map.toPixel(peak.level);
            if (std::isfinite(y) && x >= area.left() && x <= area.right())
            {
                p.setRenderHint(QPainter::Antialiasing, true);
                const double yy = std::clamp(y, area.top() + 6.0, area.bottom());
                const QPointF diamond[4] = {QPointF(x, yy - 6.0), QPointF(x + 5.0, yy), QPointF(x, yy + 6.0),
                                            QPointF(x - 5.0, yy)};
                p.setPen(QPen(m_background, 1.5));
                p.setBrush(m_text);
                p.drawPolygon(diamond, 4);
                p.setBrush(Qt::NoBrush);
                const QString label = QStringLiteral("M%1").arg(m_markerIndex + 1);
                p.setPen(m_text);
                p.drawText(QPointF(x + 8.0, yy - 6.0), label);
            }
        }
    }
    p.restore();
}

void SpectrumPlot::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    update();
}

void SpectrumPlot::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    if (!hasData() || (delta.x() == 0 && delta.y() == 0))
    {
        event->ignore();
        return;
    }
    event->accept();
    // Sideways (touchpad swipe, tilting wheel): slide along the frequencies.
    if (std::abs(delta.x()) > std::abs(delta.y()))
    {
        rememberView(true);
        scrollBy(-0.1 * delta.x() / 120.0);
        return;
    }
    const double steps = delta.y() / 120.0;
    const double factor = std::pow(0.8, steps);
    const QPointF at = event->position();
    rememberView(true);
    // Ctrl, or over the scale: the levels, about the pointer.
    if ((event->modifiers() & Qt::ControlModifier) || placeAt(at) == Place::Scale)
    {
        const QRectF area = plotRect();
        const qacq::plot::YMap map{m_levels.low, m_levels.high, area.top(), area.height(), false};
        setLevels(qacq::plot::zoomRange(m_levels, factor, map.toValue(at.y())));
        return;
    }
    const Range a = axisView();
    if (event->modifiers() & Qt::ShiftModifier)
    {
        scrollBy(-0.1 * steps);
        return;
    }
    const QRectF area = plotRect();
    const double anchor = area.width() > 0.0 ? a.low + (at.x() - area.left()) / area.width() * a.span() : a.low;
    setAxisView(qacq::plot::zoomRange(a, factor, anchor));
}

void SpectrumPlot::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    m_pressAt = event->position();
    m_pointer = m_pressAt;
    m_pressView = axisView();
    event->accept();
    m_rightPress = event->button() == Qt::RightButton;
    m_pressLevels = m_levels;
    const Place place = placeAt(m_pressAt);
    if (m_cursorsVisible && hasData() && event->button() == Qt::LeftButton && place != Place::Scale)
    {
        const Range l = limits();
        const double f = std::clamp(frequencyAt(m_pressAt.x()), l.low, l.high);
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
        {
            (event->modifiers() & Qt::ControlModifier ? m_cursorA : m_cursorB) = f;
            m_dragging = event->modifiers() & Qt::ControlModifier ? 1 : 2;
            refreshCursors();
            update();
            return;
        }
        m_dragging = cursorNear(m_pressAt.x());
        if (m_dragging != 0)
        {
            update();
            return;
        }
    }
    if (event->button() == Qt::RightButton && (place == Place::Plot || place == Place::FrequencyAxis)) m_selecting = true;
    else if (event->button() == Qt::LeftButton && place != Place::None)
    {
        // The plot: the frequencies, the levels or both, as the drag goes (qacq::plot::dragAxes); the
        // scale: the levels; the frequency axis: the frequencies.
        m_dragAxes = place == Place::Plot    ? qacq::plot::DragAxes::Undecided
                     : place == Place::Scale ? qacq::plot::DragAxes::Vertical
                                             : qacq::plot::DragAxes::Horizontal;
        // A step of the history, taken back at the release if nothing moved.
        m_pressRemembered = m_history.empty() || m_history.back().frequencies.low != m_view.low ||
                            m_history.back().frequencies.high != m_view.high || m_history.back().levels.low != m_levels.low ||
                            m_history.back().levels.high != m_levels.high;
        rememberView();
        m_panning = true;
    }
    update();
}

void SpectrumPlot::mouseMoveEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_dragging != 0)
    {
        const Range l = limits();
        (m_dragging == 1 ? m_cursorA : m_cursorB) = std::clamp(frequencyAt(m_pointer.x()), l.low, l.high);
        refreshCursors();
    }
    else if (m_panning)
    {
        using qacq::plot::DragAxes;
        const double dx = m_pointer.x() - m_pressAt.x(), dy = m_pointer.y() - m_pressAt.y();
        if (m_dragAxes == DragAxes::Undecided)
        {
            m_dragAxes = qacq::plot::dragAxes(dx, dy);
            if (m_dragAxes != DragAxes::Undecided) setCursor(QCursor(Qt::ClosedHandCursor));
        }
        const QRectF area = plotRect();
        if ((m_dragAxes == DragAxes::Horizontal || m_dragAxes == DragAxes::Both) && area.width() > 0.0)
        {
            const double shift = -dx / area.width() * m_pressView.span();
            setAxisView({m_pressView.low + shift, m_pressView.high + shift});
        }
        // The curves follow the pointer: dragged down, the scale shows higher levels.
        if ((m_dragAxes == DragAxes::Vertical || m_dragAxes == DragAxes::Both) && area.height() > 0.0)
            setLevels(qacq::plot::shiftRange(m_pressLevels, dy / area.height()));
    }
    update();
}

void SpectrumPlot::mouseReleaseEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_rightPress && std::abs(m_pointer.x() - m_pressAt.x()) <= 4.0 && std::abs(m_pointer.y() - m_pressAt.y()) <= 4.0)
    {
        // On the scale or on the frequency axis: the menu of that scale; elsewhere the plot's menu.
        const Place place = placeAt(m_pointer);
        const QVariantMap scale = place == Place::Scale           ? scaleInfo(QStringLiteral("levels"))
                                  : place == Place::FrequencyAxis ? scaleInfo(QStringLiteral("frequency"))
                                                                  : QVariantMap();
        if (!scale.isEmpty()) emit scaleMenuRequested(m_pointer.x(), m_pointer.y(), scale);
        else emit contextMenuRequested(m_pointer.x(), m_pointer.y());
    }
    m_rightPress = false;
    if (m_selecting && std::abs(m_pointer.x() - m_pressAt.x()) > 4.0)
    {
        const QRectF area = plotRect();
        const Range a = axisView();
        const auto axisAt = [&](double x) { return a.low + (x - area.left()) / area.width() * a.span(); };
        rememberView();
        setAxisView({axisAt(std::min(m_pressAt.x(), m_pointer.x())), axisAt(std::max(m_pressAt.x(), m_pointer.x()))});
    }
    // A click that did not move the view leaves no step to go back to.
    const Range now = axisView();
    const bool unchanged = now.low == m_pressView.low && now.high == m_pressView.high &&
                           m_levels.low == m_pressLevels.low && m_levels.high == m_pressLevels.high;
    if (m_panning && m_pressRemembered && unchanged && !m_history.empty() && m_history.back().frequencies.low == m_view.low &&
        m_history.back().frequencies.high == m_view.high && m_history.back().levels.low == m_levels.low &&
        m_history.back().levels.high == m_levels.high)
    {
        m_history.pop_back();
        emit viewChanged();
    }
    m_selecting = false;
    m_panning = false;
    m_pressRemembered = false;
    m_dragAxes = qacq::plot::DragAxes::Undecided;
    m_dragging = 0;
    updateCursorShape();
    update();
}

void SpectrumPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    // On the scale: automatic levels again; elsewhere everything on screen.
    if (placeAt(event->position()) == Place::Scale) autoScale();
    else showAll();
}

void SpectrumPlot::hoverMoveEvent(QHoverEvent *event)
{
    m_pointer = event->position();
    m_pointerInside = true;
    updateCursorShape();
    update();
}

void SpectrumPlot::hoverLeaveEvent(QHoverEvent *event)
{
    Q_UNUSED(event);
    m_pointerInside = false;
    update();
}
