#pragma once
// The spectrograms of the module "Signaux" (curves added, qacq::app::CurvesController): one lane per
// spectrogram shown, the frequencies upwards, the time across on the same axis as the signal above, the
// level (dB of the unit of the channel) in colours, with its scale on the right of the lane.
//
// Mouse, as on the time plots: the wheel zooms the time about the pointer (Ctrl, or over the scale of the
// frequencies: the frequencies; over the scale of the colours: the range of levels shown); a drag moves
// the time, the frequencies or both as it goes (on a scale: that scale only); a right drag zooms on the
// span it covers; a double click shows everything again (on a scale: that scale). Keys: ← → the time,
// ↑ ↓ the frequencies. Under the pointer: time, frequency and level. A right click on a scale (time,
// frequencies, colours of a lane) asks for its menu (scaleMenuRequested); elsewhere, for the plot's menu.
#include "qacq/analysis/curves.h"
#include "qacq/plot/trace.h"

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <vector>

namespace qacq::app
{
class CurvesController;
}

class SpectrogramPlot : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *curves READ curves WRITE setCurves NOTIFY sourceChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QColor panelColor MEMBER m_panel NOTIFY styleChanged)
    Q_PROPERTY(QVariantList channelColors MEMBER m_channelColors NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    // Said in a lane whose spectrogram is not computed yet ("Calcul du spectrogramme : 40 %").
    Q_PROPERTY(QString busyText MEMBER m_busyText NOTIFY styleChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY contentChanged)
    // How they are computed: "Segments de 1 024 éch. (Δf = 46,9 Hz, 21,3 ms) · recouvrement 75 %".
    Q_PROPERTY(QString summary READ summary NOTIFY contentChanged)
    // Of the signal they come from (0 without one): for the resolution shown with the settings.
    Q_PROPERTY(double sampleRate READ sampleRate NOTIFY contentChanged)
    // The time span on screen, as the plots above it (they follow one another).
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    Q_PROPERTY(double viewLow READ viewLow NOTIFY viewChanged)
    Q_PROPERTY(double viewHigh READ viewHigh NOTIFY viewChanged)
    Q_PROPERTY(double viewStart READ viewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewSize READ viewSize NOTIFY viewChanged)
    // Cursors A and B of the signal, drawn and dragged here too.
    Q_PROPERTY(bool cursorsVisible READ cursorsVisible WRITE setCursorsVisible NOTIFY cursorsChanged)
    Q_PROPERTY(double cursorA READ cursorA WRITE setCursorA NOTIFY cursorsChanged)
    Q_PROPERTY(double cursorB READ cursorB WRITE setCursorB NOTIFY cursorsChanged)
    // The frequencies zoomed or the range of levels changed by hand (autoScale() undoes both).
    Q_PROPERTY(bool manualScales READ manualScales NOTIFY scalesChanged)
    // Levels shown in colours: from the highest of each spectrogram down by this many dB.
    Q_PROPERTY(double dynamicRange READ dynamicRange WRITE setDynamicRange NOTIFY scalesChanged)

public:
    explicit SpectrogramPlot(QQuickItem *parent = nullptr);

    QObject *curves() const;
    void setCurves(QObject *curves);
    bool hasData() const;
    QString summary() const { return m_summary; }
    double sampleRate() const { return m_rate; }
    bool zoomed() const;
    bool canGoBack() const { return !m_history.empty(); }
    double viewLow() const { return m_view.low; }
    double viewHigh() const { return m_view.high; }
    double viewStart() const;
    double viewSize() const;
    bool cursorsVisible() const { return m_cursorsVisible; }
    void setCursorsVisible(bool visible);
    double cursorA() const { return m_cursorA; }
    double cursorB() const { return m_cursorB; }
    void setCursorA(double time);
    void setCursorB(double time);
    bool manualScales() const;
    double dynamicRange() const { return m_dynamic; }
    void setDynamicRange(double decibels);

    // Everything on screen again: all the time, all the frequencies, the usual range of levels.
    Q_INVOKABLE void showAll();
    // The frequencies and the range of levels as at first; the time span stays.
    Q_INVOKABLE void autoScale();
    Q_INVOKABLE void zoomTime(double factor);
    Q_INVOKABLE void goBack();
    // The time span shown, set by the plot this one follows (not a step of the history).
    Q_INVOKABLE void setTimeView(double low, double high);
    Q_INVOKABLE void scrollTo(double start);
    Q_INVOKABLE void scrollBy(double viewFraction);

    // The scales, for the menu of a scale: "time", "frequency" or "colours:<curve id>" (the levels shown
    // in colours in that lane: by hand, or from its highest level down by dynamicRange). The map: { key,
    // kind, title, unit, low, high, automatic, lowLabel, highLabel, autoText }; empty when it is gone.
    Q_INVOKABLE QVariantMap scaleInfo(const QString &key) const;
    Q_INVOKABLE bool setScaleRange(const QString &key, double low, double high);
    Q_INVOKABLE void setScaleAutomatic(const QString &key);

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void styleChanged();
    void contentChanged();
    void viewChanged();
    void cursorsChanged();
    void scalesChanged();
    // A right click that did not drag (the plot's menu), at this point of the item.
    void contextMenuRequested(double x, double y);
    // The same on a scale: the menu of that scale (scaleInfo).
    void scaleMenuRequested(double x, double y, const QVariantMap &scale);

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
    struct Lane
    {
        QRectF rect;
        std::size_t curve = 0; // in m_set->curves
    };
    enum class Place
    {
        None,
        Plot,           // a lane
        FrequencyScale, // its graduations on the left
        ColourScale,    // its scale of colours on the right
        TimeAxis        // under the lanes
    };
    // The picture of a lane, kept while nothing it depends on changes.
    struct Picture
    {
        QImage image;
        QString key;
    };

    void refresh();
    std::vector<Lane> lanes() const;
    QRectF plotRect() const;
    Place placeAt(const QPointF &at, int *lane) const;
    double timeAt(double x) const;
    double xOf(double time) const;
    double frequencyAt(const QRectF &lane, double y) const;
    double yOf(const QRectF &lane, double frequency) const;
    double nyquist() const;
    double binWidth() const;
    double topLevel(std::size_t curve) const; // highest level of a spectrogram (dB)
    // The levels shown in colours in the lane of a curve: by hand, or its highest down by dynamicRange.
    qacq::plot::Range levelsOf(std::size_t curve) const;
    // The span of those levels (wheel or drag on its scale): that lane's when set by hand, else all.
    void setLevelSpan(std::size_t curve, double decibels);
    QString scaleKeyAt(const QPointF &at) const; // "" when not over a scale
    int curveIndexOf(const QString &key) const;  // "colours:<id>" -> index in m_set->curves, or -1
    QColor channelColour(int index) const;
    const QImage &picture(const Lane &lane);
    void setViewRange(const qacq::plot::Range &range);
    void setFrequencies(const qacq::plot::Range &range);
    void rememberView(bool coalesce = false);
    void noteScales();
    int cursorNear(double x) const;
    void updateCursorShape();
    void drawLane(QPainter &p, const Lane &lane);
    void drawTimeAxis(QPainter &p, const std::vector<Lane> &lanes) const;
    void drawCursors(QPainter &p, const std::vector<Lane> &lanes) const;
    void drawReadout(QPainter &p, const std::vector<Lane> &lanes) const;

    QPointer<qacq::app::CurvesController> m_curves;
    QList<QMetaObject::Connection> m_connections;
    std::shared_ptr<const qacq::analysis::SpectrogramSet> m_set;
    std::vector<double> m_tops; // highest level of each curve of m_set (dB)
    std::map<std::uint64_t, qacq::plot::Range> m_manualLevels; // colours set by hand, by curve id
    std::map<std::uint64_t, Picture> m_pictures;
    double m_rate = 0.0;
    qacq::plot::Range m_limits{0.0, 1.0};
    qacq::plot::Range m_view{0.0, 1.0};
    bool m_viewSet = false;
    qacq::plot::Range m_frequencies{0.0, 1.0};
    bool m_frequenciesAutomatic = true;
    double m_dynamic = 80.0;
    bool m_manualScalesNoted = false;
    std::vector<qacq::plot::Range> m_history;
    qint64 m_lastRemembered = 0;
    QString m_signature; // what is shown, to tell the interface only of real changes
    QString m_summary;

    bool m_cursorsVisible = false;
    double m_cursorA = 0.0, m_cursorB = 0.0;
    int m_dragging = 0; // cursor dragged: 1 A, 2 B

    bool m_panning = false;
    bool m_pressRemembered = false;
    Place m_grab = Place::None;
    qacq::plot::DragAxes m_dragAxes = qacq::plot::DragAxes::Undecided;
    int m_grabLane = -1;
    bool m_selecting = false;
    bool m_rightPress = false;
    QPointF m_pressAt;
    QPointF m_pointer;
    bool m_pointerInside = false;
    qacq::plot::Range m_pressView{0.0, 1.0};
    qacq::plot::Range m_pressFrequencies{0.0, 1.0};
    double m_pressDynamic = 80.0;

    QColor m_background{15, 17, 20}, m_grid{255, 255, 255, 26}, m_text{230, 232, 236}, m_muted{154, 161, 172},
        m_accent{90, 170, 255}, m_panel{27, 30, 35};
    QVariantList m_channelColors;
    int m_fontSize = 11;
    QString m_emptyText;
    QString m_busyText;
};
