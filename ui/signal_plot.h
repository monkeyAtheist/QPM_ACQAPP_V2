#pragma once

// The plot of the application, one engine for every module (qacq::plot): a signal of the session, or
// the acquisition in progress.
//
// What it shows: `session` + `signalId`, a signal of the session; or, with `acquisition` and `live`, the
// last `rollSeconds` received (rolling, newest on the right), or the last trigger capture while the
// trigger is on (time 0 at the trigger instant, its level marked on its channel). `frozen` keeps the
// picture still while the acquisition goes on. Once stopped, the signal kept is shown as the last window
// was, and `rollSeconds` is the width of the view (set it to see more or less of the record).
// Layout: one lane per channel, each with its own scale (`stacked`), or every channel on one axis.
// Mouse: the wheel zooms the time axis about the pointer (Ctrl, or over a scale: the values of the lane
// under it; live: the rolling window). A drag moves what it is mostly along: sideways the time, up or
// down the values of the lane (both scales of two superposed units), in between both; a drag on a scale
// moves its values only, on the time axis the time only (live: the values only). A right drag zooms on the
// span it covers; a double click shows everything again (on a scale: that scale automatic again). Keys:
// ← → the time, ↑ ↓ the values of the lane under the pointer. The readout follows the pointer: the time,
// and the value of each channel at the nearest sample. A right click on a scale or on the time axis asks
// for its menu (scaleMenuRequested: its ends typed, automatic again); elsewhere, for the plot's menu.

#include "qacq/analysis/curves.h"
#include "qacq/plot/axis.h"
#include "qacq/plot/trace.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace qacq::model
{
struct Signal;
struct SampleData;
}

namespace qacq::app
{
class AcquisitionController;
class CurvesController;
class Session;
}

class SignalPlot : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    // What is drawn.
    Q_PROPERTY(QObject *session READ session WRITE setSession NOTIFY sourceChanged)
    Q_PROPERTY(qint64 signalId READ signalId WRITE setSignalId NOTIFY sourceChanged)
    Q_PROPERTY(QObject *acquisition READ acquisition WRITE setAcquisition NOTIFY sourceChanged)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY sourceChanged)
    Q_PROPERTY(double rollSeconds READ rollSeconds WRITE setRollSeconds NOTIFY viewChanged)
    Q_PROPERTY(bool frozen READ frozen WRITE setFrozen NOTIFY viewChanged)
    // How.
    Q_PROPERTY(bool stacked READ stacked WRITE setStacked NOTIFY viewChanged)
    Q_PROPERTY(QVariantList hiddenChannels READ hiddenChannels WRITE setHiddenChannels NOTIFY viewChanged)
    // Colours and text of the theme.
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QColor panelColor MEMBER m_panel NOTIFY styleChanged)
    Q_PROPERTY(QVariantList channelColors MEMBER m_channelColors NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    // Shown when there is nothing to draw (no signal chosen...).
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    // For the interface.
    Q_PROPERTY(bool hasData READ hasData NOTIFY contentChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY contentChanged)
    Q_PROPERTY(QStringList channelNames READ channelNames NOTIFY contentChanged)
    Q_PROPERTY(bool capture READ capture NOTIFY contentChanged)   // showing a trigger capture
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)        // part of the signal only
    Q_PROPERTY(QString viewText READ viewText NOTIFY viewChanged) // "0 à 1,2 s (sur 10 s)"
    // The samples on screen of a signal of the session (what the analysis of the module "Signaux" covers): the first
    // one and how many; 0 and -1 when it shows no signal of the session.
    Q_PROPERTY(qint64 viewFirstFrame READ viewFirstFrame NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewFrameCount READ viewFrameCount NOTIFY viewChanged)
    // Cursors A and B: vertical lines dragged with the mouse (Ctrl + click places A, Shift + click B). For
    // the interface, cursorInfo: { a, b, delta, inverse, rows: [{ name, channel, a, b, delta, stats }] }.
    // Not on the rolling display of an acquisition (freeze it, or use the trigger).
    Q_PROPERTY(bool cursorsVisible READ cursorsVisible WRITE setCursorsVisible NOTIFY cursorsChanged)
    Q_PROPERTY(bool cursorsAvailable READ cursorsAvailable NOTIFY cursorsChanged)
    Q_PROPERTY(QVariantMap cursorInfo READ cursorInfo NOTIFY cursorsChanged)
    // The samples between A and B (both included) of the signal shown: first one and how many; 0 and -1
    // when there is no signal of the session or no cursors.
    Q_PROPERTY(qint64 cursorFirstFrame READ cursorFirstFrame NOTIFY cursorsChanged)
    Q_PROPERTY(qint64 cursorFrameCount READ cursorFrameCount NOTIFY cursorsChanged)
    // The views asked before (zoom, slide, all): goBack() returns to the last one.
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    // The curves added in the module "Signaux" (a CurvesController): drawn instead of a signal of the
    // session, on the time base of the signal they come from.
    Q_PROPERTY(QObject *curves READ curves WRITE setCurves NOTIFY sourceChanged)
    // Room for a scale on the right even when no channel needs one: plots one above the other keep the
    // same time axis. Superposed channels of two units have a scale each, left and right.
    Q_PROPERTY(bool rightAxisRoom MEMBER m_rightAxisRoom NOTIFY styleChanged)
    // The time span on screen and cursors A and B on the time axis, for a plot that follows another one.
    Q_PROPERTY(double viewLow READ viewLow NOTIFY viewChanged)
    Q_PROPERTY(double viewHigh READ viewHigh NOTIFY viewChanged)
    Q_PROPERTY(double cursorA READ cursorA WRITE setCursorA NOTIFY cursorsChanged)
    Q_PROPERTY(double cursorB READ cursorB WRITE setCursorB NOTIFY cursorsChanged)
    // Where the view is in all there is to see, as fractions (scroll bar): start in [0, 1 - size].
    Q_PROPERTY(double viewStart READ viewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewSize READ viewSize NOTIFY viewChanged)
    // A scale of values on screen was zoomed or moved by hand (autoScale() makes them automatic again).
    Q_PROPERTY(bool manualScales READ manualScales NOTIFY scalesChanged)

public:
    explicit SignalPlot(QQuickItem *parent = nullptr);
    ~SignalPlot() override;

    QObject *session() const;
    void setSession(QObject *session);
    qint64 signalId() const { return m_signalId; }
    void setSignalId(qint64 id);
    QObject *acquisition() const;
    void setAcquisition(QObject *acquisition);
    bool live() const { return m_live; }
    void setLive(bool live);
    double rollSeconds() const;
    void setRollSeconds(double seconds);
    bool frozen() const { return m_frozen; }
    void setFrozen(bool frozen);
    bool stacked() const { return m_stacked; }
    void setStacked(bool stacked);
    QVariantList hiddenChannels() const;
    void setHiddenChannels(const QVariantList &channels);

    bool hasData() const;
    int channelCount() const { return static_cast<int>(m_channels.size()); }
    QStringList channelNames() const;
    bool capture() const { return m_showingCapture; }
    bool zoomed() const;
    QString viewText() const;
    qint64 viewFirstFrame() const;
    qint64 viewFrameCount() const;
    bool cursorsVisible() const { return m_cursorsVisible; }
    void setCursorsVisible(bool visible);
    bool cursorsAvailable() const;
    QVariantMap cursorInfo() const { return m_cursorInfo; }
    qint64 cursorFirstFrame() const;
    qint64 cursorFrameCount() const;
    bool canGoBack() const { return !m_history.empty(); }
    Q_INVOKABLE void goBack();
    double viewStart() const;
    double viewSize() const;
    QObject *curves() const;
    void setCurves(QObject *curves);
    double viewLow() const { return m_view.low; }
    double viewHigh() const { return m_view.high; }
    double cursorA() const { return m_cursorA; }
    double cursorB() const { return m_cursorB; }
    void setCursorA(double time);
    void setCursorB(double time);
    // The time span shown, set by the plot this one follows (not a step of the history).
    Q_INVOKABLE void setTimeView(double low, double high);
    // The view moved to start at this fraction of the limits (scroll bar), or by a fraction of its width.
    Q_INVOKABLE void scrollTo(double start);
    Q_INVOKABLE void scrollBy(double viewFraction);

    // Everything on screen again (time span and automatic scales).
    Q_INVOKABLE void showAll();
    // The scales of the values automatic again; the time span stays.
    Q_INVOKABLE void autoScale();
    bool manualScales() const;
    // Zoom of the time axis about its centre (factor < 1 zooms in; live: the rolling window).
    Q_INVOKABLE void zoomTime(double factor);

    // The scales, for the menu of a scale. key: "time", or "values:<channel>:left|right" (a channel shown
    // on that scale). The map: { key, kind ("time" | "values"), title, unit, low, high, automatic,
    // lowLabel, highLabel, autoText, locked (why it cannot be set, or absent) }; empty when the scale is
    // gone (channels hidden, layout changed).
    Q_INVOKABLE QVariantMap scaleInfo(const QString &key) const;
    // Both ends (low < high), by hand; false when refused. The time span goes in the history.
    Q_INVOKABLE bool setScaleRange(const QString &key, double low, double high);
    Q_INVOKABLE void setScaleAutomatic(const QString &key);

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void viewChanged();
    void styleChanged();
    void contentChanged();
    void cursorsChanged();
    void scalesChanged();
    // A right click that did not drag (the plot's menu), at this point of the item.
    void contextMenuRequested(double x, double y);
    // The same on a scale or on the time axis: the menu of that scale (scaleInfo).
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
    struct Channel
    {
        QString name;
        QString unit;
        std::span<const double> values;
        double frameOffset = 0.0;                 // position of values[0] in the time base, in frames
        std::vector<std::size_t> breaks;          // indices in values
        const qacq::plot::MinMaxPyramid *pyramid = nullptr;
        qacq::plot::Range range{0.0, 0.0};        // value axis (automatic unless zoomed by hand)
        bool automatic = true;
    };
    struct Lane
    {
        QRectF rect;
        std::vector<int> channels; // indices in m_channels
        qacq::plot::Range range;
        QString unit;
        // Superposed channels of a second unit: their own scale, on the right.
        std::vector<int> rightChannels;
        qacq::plot::Range rightRange{0.0, 0.0};
        QString rightUnit;
        bool perChannel = false; // the scale is that of its channels (stacked, or one lane per unit)
        bool onRight(int c) const { return std::find(rightChannels.begin(), rightChannels.end(), c) != rightChannels.end(); }
        const qacq::plot::Range &rangeOf(int c) const { return onRight(c) ? rightRange : range; }
    };
    // What a point of the item is over (to drag, or for the wheel).
    enum class Place
    {
        None,
        Plot,       // a lane, or the plotting area between and above them
        LeftScale,  // the graduations on the left of a lane
        RightScale, // those on the right (superposed channels of a second unit)
        TimeAxis    // under the lanes
    };

    void reload();           // the source changed: rebuild everything
    void refreshSignal();    // the signal of the session (a new version after an edit)
    void refreshLive();      // copies the visible part of the acquisition
    void refreshCurves();    // the curves of a CurvesController
    bool rightAxisWanted() const; // superposed channels of exactly two units
    void onFrames();
    void connectSession();
    void connectAcquisition();
    void setViewRange(const qacq::plot::Range &range);
    qacq::plot::Range timeLimits() const;
    double minimumSpan() const;
    QRectF plotRect() const;
    std::vector<Lane> layoutLanes(const QRectF &area, const std::vector<qacq::plot::Trace> &traces);
    double timeAt(double x) const;
    double xOf(double time) const;
    QColor channelColour(int index) const;
    bool isHidden(int channel) const;
    int laneAt(double y) const;
    void drawMessage(QPainter &p, const QString &text) const;
    void drawReadout(QPainter &p, const std::vector<Lane> &lanes) const;
    void announce(); // contentChanged when the channels or the mode changed
    void rememberView(bool coalesce = false); // before a change of view asked by the user
    void placeCursors();                      // A and B at a third and two thirds of the view
    int cursorNear(double x) const;           // 1 A, 2 B, 0 none (within 6 pixels)
    void refreshCursors();                    // cursorInfo again, cursorsChanged
    void drawCursors(QPainter &p, const std::vector<Lane> &lanes) const;
    QString timeUnit() const { return m_rate > 0.0 ? QStringLiteral("s") : QString(); }
    // Scales of the values set by hand (lanes of the last paint).
    Place placeAt(const QPointF &at, int *lane) const;
    qacq::plot::Range scaleOf(const Lane &lane, bool right) const; // as it is now
    void setLaneScale(const Lane &lane, bool right, const qacq::plot::Range &range);
    void autoScaleLane(const Lane &lane, bool right);
    void resetScales();
    void dragValues(double dy);         // the drag of a lane or of a scale, dy pixels from the press
    void slideValues(double fraction);  // ↑ ↓: the lane under the pointer, or every lane
    void noteScales();                  // scalesChanged when manualScales() changed
    void updateCursorShape();
    QString scaleKeyAt(const QPointF &at) const;                   // "" when not over a scale
    const Lane *laneOfScale(const QString &key, bool *right) const; // in the lanes of the last paint

    QPointer<qacq::app::Session> m_session;
    qint64 m_signalId = 0;
    QPointer<qacq::app::AcquisitionController> m_acquisition;
    QPointer<qacq::app::CurvesController> m_curves;
    QList<QMetaObject::Connection> m_curveConnections;
    std::shared_ptr<const qacq::analysis::TimeCurveSet> m_curveSet;
    std::map<const double *, qacq::plot::MinMaxPyramid> m_curvePyramids; // by first sample of a curve
    std::vector<bool> m_curveVisible;
    bool m_rightAxisRoom = false;
    qacq::plot::Range m_overlayRightRange{0.0, 0.0}; // right scale of superposed channels
    bool m_overlayRightAutomatic = true;
    bool m_live = false;
    double m_rollSeconds = 2.0;
    bool m_frozen = false;
    bool m_stacked = true;
    std::vector<int> m_hidden;

    // The data drawn: a version of a signal (kept alive), or copies of the acquisition.
    std::shared_ptr<const qacq::model::Signal> m_signal;
    const qacq::model::SampleData *m_pyramidData = nullptr;
    std::vector<qacq::plot::MinMaxPyramid> m_pyramids;
    std::vector<std::vector<double>> m_buffers;
    std::vector<Channel> m_channels;
    double m_rate = 0.0;       // samples per second (0: unknown, the axis counts frames)
    double m_timeOrigin = 0.0; // time of frame position 0
    qacq::plot::Range m_view{0.0, 1.0};   // visible time span [s]
    qacq::plot::Range m_limits{0.0, 1.0}; // what there is to see
    bool m_viewSet = false;
    qacq::plot::Range m_overlayRange{0.0, 0.0}; // value axis of the single lane (not stacked)
    bool m_overlayAutomatic = true;
    QString m_signature; // channels and mode, to tell the interface only of real changes
    bool m_showingCapture = false;
    double m_triggerLevel = 0.0;
    int m_triggerChannel = -1;
    QString m_message;
    QList<QMetaObject::Connection> m_sessionConnections;
    QList<QMetaObject::Connection> m_acquisitionConnections;

    // Cursors and the views asked before.
    bool m_cursorsVisible = false;
    bool m_cursorsPlaced = false;
    double m_cursorA = 0.0, m_cursorB = 0.0; // on the time axis
    int m_dragging = 0;                      // cursor being dragged: 1 A, 2 B
    QVariantMap m_cursorInfo;
    std::vector<qacq::plot::Range> m_history;
    qint64 m_lastRemembered = 0; // ms since epoch, to gather the steps of one wheel turn

    // Interaction.
    bool m_panning = false;    // a left drag of the plot, of a scale or of the time axis
    bool m_pressRemembered = false; // the press put the view in the history
    Place m_grab = Place::None;
    qacq::plot::DragAxes m_dragAxes = qacq::plot::DragAxes::Undecided;
    int m_grabLane = -1;
    qacq::plot::Range m_pressRange{0.0, 0.0}, m_pressRightRange{0.0, 0.0}; // scales of that lane at the press
    bool m_manualScalesNoted = false;
    bool m_selecting = false;
    bool m_rightPress = false; // a right press: a click opens the menu, a drag zooms
    QPointF m_pressAt;
    QPointF m_pointer;
    bool m_pointerInside = false;
    qacq::plot::Range m_pressView;
    std::vector<Lane> m_lastLanes; // from the last paint, for the wheel on a lane

    QColor m_background{15, 17, 20}, m_grid{255, 255, 255, 26}, m_text{230, 232, 236}, m_muted{154, 161, 172},
        m_accent{90, 170, 255}, m_panel{27, 30, 35};
    QVariantList m_channelColors;
    int m_fontSize = 11;
    QString m_emptyText;
};
