#pragma once
// The spectrum of the "Analyse" module: the curves of qacq::app::AnalysisController (one per real
// channel or I/Q pair) on one frequency axis, linear or logarithmic, levels in dB or linear.
//
// Mouse, as on the time plot: the wheel zooms the frequencies about the pointer (Ctrl, or over the scale:
// the levels); a drag moves what it is mostly along (sideways the frequencies, up or down the levels, in
// between both), on the scale the levels only, on the frequency axis the frequencies only; a right drag
// zooms on the band it covers; a double click shows everything again (on the scale: automatic levels).
// Keys: ← → the frequencies, ↑ ↓ the levels.
// The readout gives, for each curve, the highest bin under the pointer (what the stroke shows there):
// its frequency and level (in dB, the amplitude as well). The harmonics found by the tone measurement
// are marked H2, H3...
#include "qacq/plot/axis.h"
#include "qacq/plot/trace.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

namespace qacq::analysis
{
struct AnalysisResult;
}
namespace qacq::app
{
class AnalysisController;
}

class SpectrumPlot : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *analysis READ analysis WRITE setAnalysis NOTIFY analysisChanged)
    Q_PROPERTY(bool logFrequency READ logFrequency WRITE setLogFrequency NOTIFY viewChanged)
    Q_PROPERTY(bool showHarmonics MEMBER m_showHarmonics NOTIFY styleChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QColor panelColor MEMBER m_panel NOTIFY styleChanged)
    Q_PROPERTY(QVariantList channelColors MEMBER m_channelColors NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY contentChanged)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)
    // Cursors A and B on the frequencies (dragged; Ctrl + click places A, Shift + click B). cursorInfo:
    // { a, b, delta, rows: [{ name, channel, a, b, delta, stats }] }, stats = what lies in the band A-B.
    Q_PROPERTY(bool cursorsVisible READ cursorsVisible WRITE setCursorsVisible NOTIFY cursorsChanged)
    Q_PROPERTY(QVariantMap cursorInfo READ cursorInfo NOTIFY cursorsChanged)
    // Marker on a peak (markPeak): "Pic 1/10 : 1,000 kHz · −6,02 dBV (500,0 mV)".
    Q_PROPERTY(QString markerText READ markerText NOTIFY markerChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    // Where the view is in all the frequencies, as fractions of the axis (scroll bar).
    Q_PROPERTY(double viewStart READ viewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewSize READ viewSize NOTIFY viewChanged)
    // The scale of the levels was zoomed or moved by hand (autoScale() makes it automatic again).
    Q_PROPERTY(bool manualScales READ manualScales NOTIFY scalesChanged)

public:
    explicit SpectrumPlot(QQuickItem *parent = nullptr);

    QObject *analysis() const;
    void setAnalysis(QObject *analysis);
    bool logFrequency() const { return m_log; }
    void setLogFrequency(bool log);
    bool hasData() const;
    bool zoomed() const;
    bool cursorsVisible() const { return m_cursorsVisible; }
    void setCursorsVisible(bool visible);
    QVariantMap cursorInfo() const { return m_cursorInfo; }
    QString markerText() const { return m_markerText; }
    bool canGoBack() const { return !m_history.empty(); }

    // The marker on the highest peak (step 0), then on the next lower one (1) or back up (-1).
    Q_INVOKABLE void markPeak(int step);
    Q_INVOKABLE void clearMarker();
    Q_INVOKABLE void goBack();

    // Every frequency and automatic levels again.
    Q_INVOKABLE void showAll();
    // Automatic levels again; the frequencies stay.
    Q_INVOKABLE void autoScale();
    bool manualScales() const { return !m_levelsAutomatic; }
    // Zoom of the frequencies about the centre of the view (factor < 1 zooms in).
    Q_INVOKABLE void zoomFrequency(double factor);
    double viewStart() const;
    double viewSize() const;
    // The view moved to start at this fraction of the axis (scroll bar), or by a fraction of its width.
    Q_INVOKABLE void scrollTo(double start);
    Q_INVOKABLE void scrollBy(double viewFraction);

    void paint(QPainter *painter) override;

signals:
    void analysisChanged();
    void viewChanged();
    void styleChanged();
    void contentChanged();
    void cursorsChanged();
    void scalesChanged();
    // A right click that did not drag (the plot's menu), at this point of the item.
    void contextMenuRequested(double x, double y);
    void markerChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;

private:
    // What a point of the item is over (to drag, or for the wheel).
    enum class Place
    {
        None,
        Plot,
        Scale,        // the graduations of the levels, on the left
        FrequencyAxis // under the plot
    };
    Place placeAt(const QPointF &at) const;
    void setLevels(const qacq::plot::Range &levels); // by hand
    void noteScales();                               // scalesChanged when manualScales() changed
    void updateCursorShape();
    void onResult();
    qacq::plot::Range limits() const; // frequencies there are to see (log: above 0)
    bool logActive() const;           // asked, and possible (one-sided spectrum)
    // The view in the coordinate the axis is linear in: Hz, or log10(Hz).
    qacq::plot::Range axisView() const;
    void setAxisView(const qacq::plot::Range &range);
    double frequencyAt(double x) const;
    double xOf(double frequency) const;
    QRectF plotRect() const;
    QColor channelColour(int index) const;
    void drawReadout(QPainter &p, const QRectF &area, const qacq::plot::YMap &map) const;
    void drawCursorsAndMarker(QPainter &p, const QRectF &area, const qacq::plot::YMap &map) const;
    void rememberView(bool coalesce = false);
    void placeCursors();
    int cursorNear(double x) const;
    void refreshCursors();
    void refreshMarker();

    QPointer<qacq::app::AnalysisController> m_analysis;
    std::shared_ptr<const qacq::analysis::AnalysisResult> m_result;
    QString m_axisSignature;  // what the frequency axis covers: a new one resets the view
    QString m_scaleSignature; // kind of spectrum and level scale: a new one resets the levels
    QList<QMetaObject::Connection> m_connections;
    bool m_log = false;
    qacq::plot::Range m_view{0.0, 1.0}; // Hz
    qacq::plot::Range m_levels{0.0, 1.0};
    bool m_levelsAutomatic = true;
    bool m_showHarmonics = true;

    QColor m_background{18, 20, 24};
    QColor m_grid{48, 52, 60};
    QColor m_text{230, 232, 236};
    QColor m_muted{150, 156, 166};
    QColor m_accent{90, 170, 255};
    QColor m_panel{30, 33, 39};
    QVariantList m_channelColors;
    int m_fontSize = 11;
    QString m_emptyText;

    struct View
    {
        qacq::plot::Range frequencies, levels;
        bool automatic = true;
    };
    std::vector<View> m_history;
    qint64 m_lastRemembered = 0;
    bool m_cursorsVisible = false;
    bool m_cursorsPlaced = false;
    double m_cursorA = 0.0, m_cursorB = 0.0; // Hz
    int m_dragging = 0;
    QVariantMap m_cursorInfo;
    int m_markerCurve = -1;
    int m_markerIndex = 0; // in the peaks of the curve
    QString m_markerText;

    bool m_panning = false; // a left drag of the plot, of the scale or of the frequency axis
    bool m_pressRemembered = false; // the press put the view in the history
    qacq::plot::DragAxes m_dragAxes = qacq::plot::DragAxes::Undecided;
    qacq::plot::Range m_pressLevels{0.0, 1.0};
    bool m_manualScalesNoted = false;
    bool m_selecting = false;
    bool m_rightPress = false; // a right press: a click opens the menu, a drag zooms
    bool m_pointerInside = false;
    QPointF m_pressAt;
    QPointF m_pointer;
    qacq::plot::Range m_pressView{0.0, 1.0}; // axis coordinate
};
