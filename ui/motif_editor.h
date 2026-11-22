#pragma once

// The editor of a drawn motif of the generator (panel Composition): its points, x from 0 to 1 across the
// period (or the whole signal), y from -1 to 1 (times the amplitude), and the curve the generator makes of
// them (qacq::analysis::MotifShape: steps, segments or a smooth curve).
// Mouse: a click adds a point; a point dragged moves between its neighbours; a double click or a right click
// on a point removes it; a stroke drawn elsewhere replaces the motif where it passes (analysis::withStroke).
// With `snap` (or Shift held, the other way round), the points go to the grid: 1/32 of the width, 1/8 of the
// height. Keys, on the point chosen last: Suppr removes it, the arrows move it (Shift: 8 times more).
// Each change is sent at once by edited(points); the points come back through `points`, as the recipe keeps
// them (meanwhile a point being dragged stays under the pointer).

#include "qacq/analysis/generation.h"

#include <QColor>
#include <QQuickPaintedItem>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QInputEvent;

class MotifEditor : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList points READ points WRITE setPoints NOTIFY pointsChanged) // [[x, y], ...]
    Q_PROPERTY(int pointCount READ pointCount NOTIFY pointsChanged)
    // "steps", "linear" or "smooth".
    Q_PROPERTY(QString interpolation READ interpolation WRITE setInterpolation NOTIFY shapeChanged)
    // Repeated at each period (the last point joined to the first one), or once over the signal.
    Q_PROPERTY(bool periodic READ periodic WRITE setPeriodic NOTIFY shapeChanged)
    Q_PROPERTY(bool snap MEMBER m_snap NOTIFY styleChanged)
    Q_PROPERTY(QString endLabel MEMBER m_endLabel NOTIFY styleChanged)   // under the right end: "T"
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged) // shown without any point
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    // The place under the pointer, or of the point moved: "x = 0,250 · y = -0,500"; empty elsewhere.
    Q_PROPERTY(QString readout READ readout NOTIFY readoutChanged)

public:
    explicit MotifEditor(QQuickItem *parent = nullptr);

    QVariantList points() const;
    void setPoints(const QVariantList &points);
    int pointCount() const { return static_cast<int>(m_points.size()); }
    QString interpolation() const;
    void setInterpolation(const QString &key);
    bool periodic() const { return m_periodic; }
    void setPeriodic(bool periodic);
    QString readout() const { return m_readout; }

    void paint(QPainter *painter) override;

signals:
    void pointsChanged();
    void shapeChanged();
    void styleChanged();
    void readoutChanged();
    void edited(const QVariantList &points); // the user changed the points

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    using Point = qacq::analysis::MotifPoint;

    QRectF area() const;
    QPointF toScreen(const Point &point) const;
    Point fromScreen(const QPointF &at, bool snapped) const;
    bool snapping(const QInputEvent *event) const; // `snap`, the other way round with Shift
    int pointAt(const QPointF &at) const;           // the nearest point within reach, or -1
    void showPosition(const Point &point);
    void setReadout(const QString &text);
    void replacePoints(std::vector<Point> points); // from outside
    void removePoint(int index);
    void commit(); // edited(points)

    std::vector<Point> m_points;
    std::vector<Point> m_stroke;  // being drawn
    std::vector<Point> m_pending; // points set from outside while a point is dragged
    bool m_hasPending = false;
    qacq::analysis::MotifInterpolation m_interpolation = qacq::analysis::MotifInterpolation::Linear;
    bool m_periodic = true;
    bool m_snap = false;
    int m_drag = -1;
    int m_selected = -1;
    int m_hover = -1;
    bool m_stroking = false;
    bool m_moved = false;
    QPointF m_pressAt;
    QString m_readout;
    QString m_endLabel = QStringLiteral("T");
    QString m_emptyText;
    QColor m_background = QColor(15, 17, 20);
    QColor m_grid = QColor(255, 255, 255, 23);
    QColor m_text = QColor(230, 232, 236);
    QColor m_muted = QColor(154, 161, 172);
    QColor m_accent = QColor(90, 170, 255);
    int m_fontSize = 11;
};
