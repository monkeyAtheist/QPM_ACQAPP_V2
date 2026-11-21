#pragma once

// The plot of the application, one engine for every module (qacq::plot): a signal of the session, or
// the acquisition in progress.
//
// What it shows: `session` + `signalId`, a signal of the session; or, with `acquisition` and `live`, the
// last `rollSeconds` received (rolling, newest on the right), or the last trigger capture while the
// trigger is on (time 0 at the trigger instant, its level marked on its channel). `frozen` keeps the
// picture still while the acquisition goes on.
// Layout: one lane per channel, each with its own scale (`stacked`), or every channel on one axis.
// Mouse: the wheel zooms the time axis about the pointer (Ctrl: the values of the lane under it; live:
// the rolling window), a drag slides the time axis, a right drag zooms on the span it covers, a double
// click shows everything again. The readout follows the pointer: the time, and the value of each channel
// at the nearest sample.

#include "qacq/plot/axis.h"
#include "qacq/plot/trace.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

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
    double rollSeconds() const { return m_rollSeconds; }
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

    // Everything on screen again (time span and automatic scales).
    Q_INVOKABLE void showAll();
    // Zoom of the time axis about its centre (factor < 1 zooms in; live: the rolling window).
    Q_INVOKABLE void zoomTime(double factor);

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void viewChanged();
    void styleChanged();
    void contentChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
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
    };

    void reload();           // the source changed: rebuild everything
    void refreshSignal();    // the signal of the session (a new version after an edit)
    void refreshLive();      // copies the visible part of the acquisition
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
    QString timeUnit() const { return m_rate > 0.0 ? QStringLiteral("s") : QString(); }

    QPointer<qacq::app::Session> m_session;
    qint64 m_signalId = 0;
    QPointer<qacq::app::AcquisitionController> m_acquisition;
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

    // Interaction.
    bool m_panning = false;
    bool m_selecting = false;
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
