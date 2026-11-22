#pragma once
// The responses of the filter of the module "Filtres" (qacq::app::FilterController), as `response` says:
//   "gain"      |H| along the frequencies, in dB or linear (`decibels`)
//   "phase"     its phase in degrees, unwrapped (within ±180° with `wrapPhase`)
//   "delay"     its group delay, in samples
//   "impulse"   h[n], and "step" its running sum, along the samples
//   "polezero"  its poles (×) and zeros (○) in the z-plane, with the unit circle
// The filter as designed in a solid line; as applied (at zero phase, a FIR aligned) dashed, where that
// changes what is shown (`showApplied`). The edges of the band are marked along the frequencies.
//
// Mouse, as on the spectra: the wheel zooms the horizontal axis about the pointer (Ctrl, or over the scale:
// the vertical one; in the z-plane, both); a drag moves the view (sideways, up or down, or both); a right
// drag zooms on what it covers; a double click shows everything again. Keys: the arrows move the view.
// Under the pointer, the values (`readout`, also drawn in the corner); in the z-plane, the nearest pole or
// zero.
#include "qacq/plot/trace.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <complex>
#include <memory>
#include <vector>

namespace qacq::analysis
{
struct FilterDesign;
struct ImpulseResponse;
} // namespace qacq::analysis
namespace qacq::app
{
class FilterController;
}

class ResponsePlot : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString response READ response WRITE setResponse NOTIFY responseChanged)
    Q_PROPERTY(bool logFrequency READ logFrequency WRITE setLogFrequency NOTIFY viewChanged)
    Q_PROPERTY(bool decibels READ decibels WRITE setDecibels NOTIFY viewChanged)
    Q_PROPERTY(bool wrapPhase READ wrapPhase WRITE setWrapPhase NOTIFY viewChanged)
    Q_PROPERTY(bool showApplied READ showApplied WRITE setShowApplied NOTIFY viewChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QColor appliedColor MEMBER m_applied NOTIFY styleChanged)
    Q_PROPERTY(QColor warningColor MEMBER m_warning NOTIFY styleChanged)
    Q_PROPERTY(QColor panelColor MEMBER m_panel NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY contentChanged)
    // The application changes the response shown: a dashed curve shows it (not in the z-plane).
    Q_PROPERTY(bool appliedDiffers READ appliedDiffers NOTIFY contentChanged)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    // Under the pointer: "f = 1,000 kHz · gain −3,01 dB"; empty elsewhere.
    Q_PROPERTY(QString readout READ readout NOTIFY readoutChanged)

public:
    explicit ResponsePlot(QQuickItem *parent = nullptr);

    QObject *filter() const;
    void setFilter(QObject *filter);
    QString response() const;
    void setResponse(const QString &key);
    bool logFrequency() const { return m_log; }
    void setLogFrequency(bool log);
    bool decibels() const { return m_decibels; }
    void setDecibels(bool decibels);
    bool wrapPhase() const { return m_wrap; }
    void setWrapPhase(bool wrap);
    bool showApplied() const { return m_showApplied; }
    void setShowApplied(bool show);
    bool hasData() const;
    bool appliedDiffers() const;
    bool zoomed() const;
    bool canGoBack() const { return !m_history.empty(); }
    QString readout() const { return m_readout; }

    // Everything again, automatic scale.
    Q_INVOKABLE void showAll();
    // The horizontal axis (the z-plane: both) about the centre of the view: factor < 1 zooms in.
    Q_INVOKABLE void zoomFrequency(double factor);
    Q_INVOKABLE void goBack();

    void paint(QPainter *painter) override;

signals:
    void filterChanged();
    void responseChanged();
    void viewChanged();
    void styleChanged();
    void contentChanged();
    void readoutChanged();

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    using Range = qacq::plot::Range;
    enum class Kind
    {
        Gain,
        Phase,
        Delay,
        Impulse,
        Step,
        PoleZero
    };
    // What "Précédente" brings back.
    struct View
    {
        Range frequencies;
        bool frequenciesAll = true;
        Range samples;
        bool samplesAll = true;
        double planeX = 0.0, planeY = 0.0, planeHalf = 1.2; // centre, and half the height shown
        bool planeAll = true;
        Range levels;
        bool levelsAuto = true;
    };
    // The curves along the frequencies, one value a column (and its ends).
    struct Column
    {
        double frequency = 0.0;
        double designed = 0.0;
        double applied = 0.0;
        double magnitude = 0.0; // |H|, to leave out the delays where the gain is nothing
    };
    struct Plane // the z-plane on screen: the same scale both ways
    {
        double centreX = 0.0, centreY = 0.0, scale = 1.0; // pixels a unit
        QPointF origin;                                     // centre of the area
        QPointF toScreen(double re, double im) const;
        double reAt(double x) const;
        double imAt(double y) const;
    };
    struct Root
    {
        std::complex<double> value;
        int count = 1;
        bool pole = false;
    };

    bool frequencyKind() const { return m_kind == Kind::Gain || m_kind == Kind::Phase || m_kind == Kind::Delay; }
    bool sampleKind() const { return m_kind == Kind::Impulse || m_kind == Kind::Step; }
    QRectF plotRect() const;
    void designChanged();
    const qacq::analysis::ImpulseResponse *impulse() const;
    Range frequencyLimits() const;
    Range sampleLimits() const;
    double planeRadius() const; // what the whole z-plane view shows about 0
    Range frequencyView() const;
    Range sampleView() const;
    Plane plane(const QRectF &area) const;
    double xOf(double value, const QRectF &area) const;     // along the horizontal axis (frequency or n)
    double valueAt(double x, const QRectF &area) const;     // the other way
    const std::vector<Column> &columns(const QRectF &area); // computed again when the view changed
    double designedValue(double f) const;
    double appliedValue(double f) const;
    bool appliedShown() const;
    QString appliedName() const;
    std::vector<Root> roots() const;
    Range automaticLevels(const QRectF &area);
    void setFrequencyView(Range range);
    void setSampleView(Range range);
    void setLevels(Range range);
    void zoomAt(double factor, const QPointF &at, bool vertical);
    void moveBy(double dx, double dy); // pixels: the view follows the pointer
    void rememberView(bool coalesce);
    void updateReadout();
    void resetView();
    void changed(); // update(), viewChanged()

    void paintFrequencies(QPainter &p, const QRectF &area);
    void paintSamples(QPainter &p, const QRectF &area);
    void paintPlane(QPainter &p, const QRectF &area);
    void paintLevelAxis(QPainter &p, const QRectF &area, const QString &title);
    void paintLegend(QPainter &p, const QRectF &area);
    void paintReadout(QPainter &p, const QRectF &area);
    void paintNote(QPainter &p, const QRectF &area, const QString &note);

    QPointer<qacq::app::FilterController> m_filter;
    QMetaObject::Connection m_connection;
    std::shared_ptr<const qacq::analysis::FilterDesign> m_design;
    mutable std::shared_ptr<const qacq::analysis::ImpulseResponse> m_impulse;
    Kind m_kind = Kind::Gain;
    bool m_log = false;
    bool m_decibels = true;
    bool m_wrap = false;
    bool m_showApplied = true;
    View m_view;
    std::vector<View> m_history;
    qint64 m_lastRemembered = 0;

    // The columns, and what they were computed for.
    std::vector<Column> m_columns;
    const void *m_columnsDesign = nullptr;
    Kind m_columnsKind = Kind::Gain;
    Range m_columnsRange;
    int m_columnsCount = 0;
    bool m_columnsLog = false;
    bool m_columnsDecibels = true;
    bool m_columnsWrap = false;

    // Pointer and gestures.
    bool m_hovering = false;
    QPointF m_pointer;
    QString m_readout;
    enum class Drag
    {
        None,
        Move,
        Band
    };
    Drag m_drag = Drag::None;
    QPointF m_pressAt;
    QPointF m_lastAt;
    bool m_moved = false;
    int m_direction = 0; // of a move: 0 not yet, 1 sideways, 2 up or down, 3 both

    QColor m_background = QColor(15, 17, 20);
    QColor m_grid = QColor(255, 255, 255, 23);
    QColor m_text = QColor(230, 232, 236);
    QColor m_muted = QColor(154, 161, 172);
    QColor m_accent = QColor(90, 170, 255);
    QColor m_applied = QColor(255, 176, 80);
    QColor m_warning = QColor(255, 110, 100);
    QColor m_panel = QColor(28, 31, 36);
    int m_fontSize = 11;
    QString m_emptyText;
};
