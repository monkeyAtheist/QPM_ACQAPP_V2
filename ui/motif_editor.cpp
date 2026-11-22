#include "motif_editor.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>

namespace analysis = qacq::analysis;

namespace
{
constexpr double kReach = 8.0;        // pixels: a press this close to a point takes it
constexpr double kGridX = 1.0 / 32.0; // the grid of `snap`
constexpr double kGridY = 1.0 / 8.0;
constexpr double kStepX = 1.0 / 256.0; // an arrow key (Shift: 8 times)
constexpr double kStepY = 1.0 / 128.0;

// "0,250", "-0,500" (no "-0,000").
QString decimal(double v)
{
    const double r = std::abs(v) < 5e-4 ? 0.0 : v;
    std::array<char, 40> buffer{};
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), r, std::chars_format::fixed, 3);
    std::string text = ec == std::errc() ? std::string(buffer.data(), end) : std::string("0");
    std::replace(text.begin(), text.end(), '.', ',');
    return QString::fromStdString(text);
}
} // namespace

MotifEditor::MotifEditor(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
    connect(this, &MotifEditor::styleChanged, this, [this] { update(); });
    connect(this, &MotifEditor::shapeChanged, this, [this] { update(); });
}

QVariantList MotifEditor::points() const
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_points.size()));
    for (const Point &p : m_points) list.append(QVariant(QVariantList{QVariant(p.x), QVariant(p.y)}));
    return list;
}

void MotifEditor::setPoints(const QVariantList &list)
{
    std::vector<Point> points;
    for (const QVariant &item : list)
    {
        const QVariantList pair = item.toList();
        if (pair.size() >= 2)
        {
            points.push_back({pair[0].toDouble(), pair[1].toDouble()});
            continue;
        }
        const QVariantMap map = item.toMap();
        if (map.contains(QStringLiteral("x")) && map.contains(QStringLiteral("y")))
            points.push_back({map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()});
    }
    points = analysis::normalizedMotif(std::move(points));
    if (m_drag >= 0)
    {
        // The point under the pointer stays where it is until the drag ends.
        m_pending = std::move(points);
        m_hasPending = true;
        return;
    }
    replacePoints(std::move(points));
}

void MotifEditor::replacePoints(std::vector<Point> points)
{
    if (points == m_points) return;
    m_points = std::move(points);
    if (m_selected >= pointCount()) m_selected = -1;
    if (m_hover >= pointCount()) m_hover = -1;
    update();
    emit pointsChanged();
}

QString MotifEditor::interpolation() const
{
    switch (m_interpolation)
    {
        case analysis::MotifInterpolation::Steps: return QStringLiteral("steps");
        case analysis::MotifInterpolation::Smooth: return QStringLiteral("smooth");
        case analysis::MotifInterpolation::Linear: break;
    }
    return QStringLiteral("linear");
}

void MotifEditor::setInterpolation(const QString &key)
{
    const analysis::MotifInterpolation interpolation = key == QStringLiteral("steps")    ? analysis::MotifInterpolation::Steps
                                                       : key == QStringLiteral("smooth") ? analysis::MotifInterpolation::Smooth
                                                                                         : analysis::MotifInterpolation::Linear;
    if (interpolation == m_interpolation) return;
    m_interpolation = interpolation;
    emit shapeChanged();
}

void MotifEditor::setPeriodic(bool periodic)
{
    if (periodic == m_periodic) return;
    m_periodic = periodic;
    emit shapeChanged();
}

QRectF MotifEditor::area() const
{
    const double left = 30.0, right = 10.0, top = 10.0, bottom = 20.0;
    return QRectF(left, top, std::max(1.0, width() - left - right), std::max(1.0, height() - top - bottom));
}

QPointF MotifEditor::toScreen(const Point &point) const
{
    const QRectF a = area();
    return QPointF(a.left() + point.x * a.width(), a.top() + (1.0 - point.y) / 2.0 * a.height());
}

MotifEditor::Point MotifEditor::fromScreen(const QPointF &at, bool snapped) const
{
    const QRectF a = area();
    double x = std::clamp((at.x() - a.left()) / a.width(), 0.0, 1.0);
    double y = std::clamp(1.0 - 2.0 * (at.y() - a.top()) / a.height(), -1.0, 1.0);
    if (snapped)
    {
        x = std::round(x / kGridX) * kGridX;
        y = std::round(y / kGridY) * kGridY;
    }
    return {x, y};
}

bool MotifEditor::snapping(const QInputEvent *event) const
{
    bool shift = false;
    if (event->modifiers() & Qt::ShiftModifier) shift = true;
    return m_snap != shift;
}

int MotifEditor::pointAt(const QPointF &at) const
{
    int found = -1;
    double nearest = kReach * kReach;
    for (std::size_t i = 0; i < m_points.size(); ++i)
    {
        const QPointF p = toScreen(m_points[i]);
        const double dx = p.x() - at.x(), dy = p.y() - at.y();
        const double distance = dx * dx + dy * dy;
        if (distance <= nearest)
        {
            nearest = distance;
            found = static_cast<int>(i);
        }
    }
    return found;
}

void MotifEditor::showPosition(const Point &point)
{
    setReadout(QStringLiteral("x = %1 · y = %2").arg(decimal(point.x), decimal(point.y)));
}

void MotifEditor::setReadout(const QString &text)
{
    if (text == m_readout) return;
    m_readout = text;
    emit readoutChanged();
}

void MotifEditor::removePoint(int index)
{
    if (index < 0 || index >= pointCount()) return;
    m_points.erase(m_points.begin() + index);
    m_selected = -1;
    m_hover = -1;
    setCursor(QCursor(Qt::CrossCursor));
    update();
    emit pointsChanged();
    commit();
}

void MotifEditor::commit()
{
    emit edited(points());
}

void MotifEditor::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    // In the panel that scrolls, the drag stays here.
    setKeepMouseGrab(true);
    event->accept();
    m_pressAt = event->position();
    m_moved = false;
    const int hit = pointAt(m_pressAt);
    if (event->button() == Qt::RightButton)
    {
        removePoint(hit);
        return;
    }
    if (hit >= 0)
    {
        m_drag = hit;
        m_selected = hit;
        setCursor(QCursor(Qt::ClosedHandCursor));
        showPosition(m_points[static_cast<std::size_t>(hit)]);
        update();
        return;
    }
    m_stroking = true;
    m_stroke.assign(1, fromScreen(m_pressAt, snapping(event)));
    update();
}

void MotifEditor::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
    const QPointF at = event->position();
    if (!m_moved && std::hypot(at.x() - m_pressAt.x(), at.y() - m_pressAt.y()) < 3.0) return;
    m_moved = true;
    if (m_drag >= 0 && m_drag < pointCount())
    {
        // Between its neighbours: the order of the points stays.
        const auto i = static_cast<std::size_t>(m_drag);
        Point p = fromScreen(at, snapping(event));
        const double low = i > 0 ? m_points[i - 1].x : 0.0;
        const double high = i + 1 < m_points.size() ? m_points[i + 1].x : 1.0;
        p.x = std::clamp(p.x, low, high);
        showPosition(p);
        if (p == m_points[i]) return;
        m_points[i] = p;
        update();
        commit();
        return;
    }
    if (m_stroking)
    {
        m_stroke.push_back(fromScreen(at, snapping(event)));
        showPosition(m_stroke.back());
        update();
    }
}

void MotifEditor::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
    setKeepMouseGrab(false);
    if (m_stroking)
    {
        m_stroking = false;
        if (!m_moved)
        {
            // A click: one point more, the point chosen.
            const Point p = m_stroke.empty() ? fromScreen(event->position(), snapping(event)) : m_stroke.front();
            std::vector<Point> points = m_points;
            points.push_back(p);
            m_points = analysis::normalizedMotif(std::move(points));
            m_selected = -1;
            for (std::size_t i = m_points.size(); i-- > 0;)
            {
                if (m_points[i] == p)
                {
                    m_selected = static_cast<int>(i);
                    break;
                }
            }
        }
        else
        {
            m_points = analysis::withStroke(m_points, m_stroke);
            m_selected = -1;
        }
        m_stroke.clear();
        update();
        emit pointsChanged();
        commit();
    }
    if (m_drag >= 0)
    {
        m_drag = -1;
        setCursor(QCursor(m_hover >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor));
        update();
    }
    if (m_hasPending)
    {
        m_hasPending = false;
        std::vector<Point> pending;
        pending.swap(m_pending);
        replacePoints(std::move(pending));
    }
}

void MotifEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    removePoint(pointAt(event->position()));
}

void MotifEditor::hoverMoveEvent(QHoverEvent *event)
{
    const QPointF at = event->position();
    const int hit = pointAt(at);
    if (hit != m_hover)
    {
        m_hover = hit;
        setCursor(QCursor(hit >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor));
        update();
    }
    showPosition(hit >= 0 ? m_points[static_cast<std::size_t>(hit)] : fromScreen(at, false));
}

void MotifEditor::hoverLeaveEvent(QHoverEvent *event)
{
    event->accept();
    m_hover = -1;
    setReadout(QString());
    update();
}

void MotifEditor::keyPressEvent(QKeyEvent *event)
{
    if (m_selected < 0 || m_selected >= pointCount() || m_drag >= 0)
    {
        event->ignore();
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace)
    {
        event->accept();
        removePoint(m_selected);
        return;
    }
    double factor = 1.0;
    if (event->modifiers() & Qt::ShiftModifier) factor = 8.0;
    double dx = 0.0, dy = 0.0;
    if (key == Qt::Key_Left) dx = -kStepX * factor;
    else if (key == Qt::Key_Right) dx = kStepX * factor;
    else if (key == Qt::Key_Up) dy = kStepY * factor;
    else if (key == Qt::Key_Down) dy = -kStepY * factor;
    else
    {
        event->ignore();
        return;
    }
    event->accept();
    const auto i = static_cast<std::size_t>(m_selected);
    Point p = m_points[i];
    const double low = i > 0 ? m_points[i - 1].x : 0.0;
    const double high = i + 1 < m_points.size() ? m_points[i + 1].x : 1.0;
    p.x = std::clamp(p.x + dx, low, high);
    p.y = std::clamp(p.y + dy, -1.0, 1.0);
    showPosition(p);
    if (p == m_points[i]) return;
    m_points[i] = p;
    update();
    commit();
}

void MotifEditor::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    QFont font = p.font();
    font.setPixelSize(std::max(8, m_fontSize));
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF a = area();
    if (a.width() < 20.0 || a.height() < 20.0) return;
    p.setRenderHint(QPainter::Antialiasing, true);

    // The grid: quarters of the width, halves of the height; finer, that of `snap`.
    if (m_snap)
    {
        QColor fine = m_grid;
        fine.setAlphaF(fine.alphaF() * 0.5f);
        p.setPen(QPen(fine, 1.0));
        for (int i = 1; i < 32; ++i)
        {
            if (i % 8 == 0) continue;
            const double x = a.left() + a.width() * i / 32.0;
            p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        }
        for (int j = 1; j < 16; ++j)
        {
            if (j % 4 == 0) continue;
            const double y = a.top() + a.height() * j / 16.0;
            p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
        }
    }
    p.setPen(QPen(m_grid, 1.0));
    for (int i = 0; i <= 4; ++i)
    {
        const double x = a.left() + a.width() * i / 4.0;
        p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        const double y = a.top() + a.height() * i / 4.0;
        p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
    }
    QColor axis = m_muted;
    axis.setAlphaF(0.6f);
    p.setPen(QPen(axis, 1.0));
    const double zero = a.top() + a.height() / 2.0;
    p.drawLine(QPointF(a.left(), zero), QPointF(a.right(), zero));

    // Scales: the amplitude up and down, the period (or the signal) across.
    p.setPen(m_muted);
    const double h = metrics.height();
    const QString yLabels[3] = {QStringLiteral("+A"), QStringLiteral("0"), QStringLiteral("-A")};
    for (int j = 0; j < 3; ++j)
    {
        const double y = a.top() + a.height() * j / 2.0;
        p.drawText(QRectF(0.0, y - h / 2.0, a.left() - 5.0, h), Qt::AlignRight | Qt::AlignVCenter, yLabels[j]);
    }
    const double below = a.bottom() + 3.0;
    p.drawText(QRectF(a.left() - 20.0, below, 40.0, h), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("0"));
    if (m_periodic && m_endLabel == QStringLiteral("T"))
        p.drawText(QRectF(a.left() + a.width() / 2.0 - 30.0, below, 60.0, h), Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("T/2"));
    p.drawText(QRectF(a.right() - 60.0, below, 70.0, h), Qt::AlignRight | Qt::AlignTop, m_endLabel);

    // The curve, as the generator makes it.
    const analysis::MotifShape shape(m_points, m_interpolation, m_periodic);
    const int columns = std::max(64, static_cast<int>(std::ceil(a.width() * 2.0)));
    QPolygonF curve;
    curve.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i)
    {
        const double u = static_cast<double>(i) / columns;
        curve << toScreen({u, std::clamp(shape(u), -1.0, 1.0)});
    }
    p.setPen(QPen(m_accent, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(curve);

    // The stroke being drawn.
    if (m_stroking && m_stroke.size() > 1)
    {
        QColor ink = m_text;
        ink.setAlphaF(0.7f);
        QPolygonF line;
        line.reserve(static_cast<qsizetype>(m_stroke.size()));
        for (const Point &q : m_stroke) line << toScreen(q);
        p.setPen(QPen(ink, 1.5, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
    }

    // The points: the one under the pointer, moved or chosen larger.
    for (std::size_t i = 0; i < m_points.size(); ++i)
    {
        const int index = static_cast<int>(i);
        const bool lit = index == m_hover || index == m_drag || index == m_selected;
        const double r = lit ? 5.0 : 3.5;
        p.setPen(QPen(m_accent, 1.5));
        p.setBrush(lit ? m_accent : m_background);
        p.drawEllipse(toScreen(m_points[i]), r, r);
    }
    p.setBrush(Qt::NoBrush);

    if (m_points.empty() && !m_stroking && !m_emptyText.isEmpty())
    {
        p.setPen(m_muted);
        p.drawText(a.adjusted(8.0, 0.0, -8.0, -h), Qt::AlignCenter | Qt::TextWordWrap, m_emptyText);
    }
}
