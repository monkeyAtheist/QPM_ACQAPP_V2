#include "signal_plot.h"

#include "qacq/acquisition/recorder.h"
#include "qacq/acquisition/trigger.h"
#include "qacq/app/acquisition_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/signal.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

using qacq::plot::Range;

namespace
{
// Live copies are limited to this many frames per channel: a rolling window wider than that shows its
// most recent part (4 M frames, 32 Mo per channel).
constexpr std::size_t kMaximumLiveFrames = std::size_t{4} << 20;
// Pyramids of extremes for the signals longer than this.
constexpr std::size_t kPyramidFrom = std::size_t{1} << 15;

// Margins around the plotting area: value labels on the left, time labels and their title under it.
constexpr double kLeft = 62.0;
constexpr double kRight = 14.0;
constexpr double kTop = 8.0;
constexpr double kBottom = 40.0;
constexpr double kLaneGap = 12.0;

QString text(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string utf8(const QString &s)
{
    return s.toStdString();
}

QColor faded(QColor colour, double alpha)
{
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * static_cast<double>(colour.alphaF())));
    return colour;
}

// Breaks of the frames [first, first + count) kept by the recorder, as indices of the copy.
std::vector<std::size_t> breaksIn(const qacq::acquisition::LiveRecorder &recorder, std::uint64_t first, std::size_t count)
{
    std::vector<std::size_t> out;
    for (std::uint64_t b : recorder.discontinuities())
    {
        if (b > first && b < first + count) out.push_back(static_cast<std::size_t>(b - first));
    }
    return out;
}
} // namespace

SignalPlot::SignalPlot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
    for (const QColor &c : {QColor(90, 170, 255), QColor(255, 167, 38), QColor(76, 195, 138), QColor(229, 115, 115),
                            QColor(186, 104, 200), QColor(77, 208, 225), QColor(255, 213, 79), QColor(161, 136, 127)})
        m_channelColors << QVariant::fromValue(c);
    connect(this, &SignalPlot::styleChanged, this, [this] { update(); });
}

SignalPlot::~SignalPlot() = default;

QObject *SignalPlot::session() const
{
    return m_session.data();
}

void SignalPlot::setSession(QObject *session)
{
    auto *s = qobject_cast<qacq::app::Session *>(session);
    if (s == m_session.data()) return;
    m_session = s;
    connectSession();
    reload();
    emit sourceChanged();
}

void SignalPlot::setSignalId(qint64 id)
{
    if (id == m_signalId) return;
    m_signalId = id;
    reload();
    emit sourceChanged();
}

QObject *SignalPlot::acquisition() const
{
    return m_acquisition.data();
}

void SignalPlot::setAcquisition(QObject *acquisition)
{
    auto *a = qobject_cast<qacq::app::AcquisitionController *>(acquisition);
    if (a == m_acquisition.data()) return;
    m_acquisition = a;
    connectAcquisition();
    reload();
    emit sourceChanged();
}

void SignalPlot::setLive(bool live)
{
    if (live == m_live) return;
    m_live = live;
    m_viewSet = false;
    for (Channel &c : m_channels) c.range = {0.0, 0.0};
    m_overlayRange = {0.0, 0.0};
    reload();
    emit sourceChanged();
}

void SignalPlot::setRollSeconds(double seconds)
{
    if (!std::isfinite(seconds)) return;
    seconds = std::clamp(seconds, 1e-4, 3600.0);
    if (seconds == m_rollSeconds) return;
    m_rollSeconds = seconds;
    if (m_live) refreshLive();
    emit viewChanged();
    update();
}

void SignalPlot::setFrozen(bool frozen)
{
    if (frozen == m_frozen) return;
    m_frozen = frozen;
    if (!m_frozen && m_live) refreshLive();
    emit viewChanged();
    update();
}

void SignalPlot::setStacked(bool stacked)
{
    if (stacked == m_stacked) return;
    m_stacked = stacked;
    emit viewChanged();
    update();
}

QVariantList SignalPlot::hiddenChannels() const
{
    QVariantList out;
    for (int c : m_hidden) out << c;
    return out;
}

void SignalPlot::setHiddenChannels(const QVariantList &channels)
{
    std::vector<int> hidden;
    for (const QVariant &v : channels) hidden.push_back(v.toInt());
    std::sort(hidden.begin(), hidden.end());
    hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
    if (hidden == m_hidden) return;
    m_hidden = std::move(hidden);
    emit viewChanged();
    update();
}

bool SignalPlot::isHidden(int channel) const
{
    return std::binary_search(m_hidden.begin(), m_hidden.end(), channel);
}

bool SignalPlot::hasData() const
{
    return !m_channels.empty();
}

QStringList SignalPlot::channelNames() const
{
    QStringList names;
    for (const Channel &c : m_channels) names << c.name;
    return names;
}

bool SignalPlot::zoomed() const
{
    if (m_live && !m_showingCapture) return false;
    return m_limits.valid() && m_view.span() < m_limits.span() * 0.999;
}

QString SignalPlot::viewText() const
{
    const QString unit = timeUnit();
    const auto quantity = [&unit](double v) { return text(qacq::plot::formatCompact(v, utf8(unit), 3)); };
    if (m_channels.empty()) return QString();
    if (m_live && !m_showingCapture) return tr("Fenêtre de %1").arg(quantity(m_rollSeconds));
    if (!zoomed()) return m_showingCapture ? tr("Capture de %1").arg(quantity(m_limits.span())) : tr("Tout le signal : %1").arg(quantity(m_limits.span()));
    return tr("%1 à %2 (sur %3)").arg(quantity(m_view.low), quantity(m_view.high), quantity(m_limits.span()));
}

void SignalPlot::connectSession()
{
    for (const QMetaObject::Connection &c : m_sessionConnections) disconnect(c);
    m_sessionConnections.clear();
    if (!m_session) return;
    qacq::app::SignalListModel *list = m_session->signalList();
    const auto changed = [this] {
        if (!m_live) refreshSignal();
        update();
    };
    m_sessionConnections << connect(list, &QAbstractItemModel::dataChanged, this, changed)
                         << connect(list, &QAbstractItemModel::rowsInserted, this, changed)
                         << connect(list, &QAbstractItemModel::rowsRemoved, this, changed)
                         << connect(list, &QAbstractItemModel::modelReset, this, changed);
}

void SignalPlot::connectAcquisition()
{
    for (const QMetaObject::Connection &c : m_acquisitionConnections) disconnect(c);
    m_acquisitionConnections.clear();
    if (!m_acquisition) return;
    qacq::app::AcquisitionController *a = m_acquisition.data();
    const auto again = [this] {
        if (m_live) reload();
    };
    m_acquisitionConnections << connect(a, &qacq::app::AcquisitionController::framesArrived, this, [this] { onFrames(); })
                             << connect(a, &qacq::app::AcquisitionController::triggered, this, [this] { onFrames(); })
                             << connect(a, &qacq::app::AcquisitionController::channelsChanged, this, again)
                             << connect(a, &qacq::app::AcquisitionController::triggerChanged, this, again)
                             << connect(a, &qacq::app::AcquisitionController::stateChanged, this, again);
}

void SignalPlot::reload()
{
    if (m_live && m_acquisition) refreshLive();
    else refreshSignal();
    update();
}

void SignalPlot::announce()
{
    QString signature = QStringLiteral("%1|%2|").arg(m_showingCapture ? 1 : 0).arg(static_cast<int>(m_channels.size()));
    for (const Channel &c : m_channels) signature += c.name + QLatin1Char('|');
    if (signature == m_signature) return;
    m_signature = signature;
    emit contentChanged();
    emit viewChanged();
}

void SignalPlot::refreshSignal()
{
    m_showingCapture = false;
    m_triggerChannel = -1;
    m_buffers.clear();
    qacq::model::SignalPtr signal;
    if (m_session && m_signalId > 0) signal = m_session->signalList()->signalById(m_signalId);
    const bool sameSignal = m_signal && signal && m_signal->id == signal->id && !m_live;
    std::vector<Channel> previous = std::move(m_channels);
    m_channels.clear();
    m_signal = signal;
    if (!signal || !signal->data || signal->frameCount() == 0 || signal->channelCount() != signal->data->channelCount())
    {
        m_rate = 0.0;
        m_pyramids.clear();
        m_pyramidData = nullptr;
        m_message.clear(); // the empty text of the interface, as it is when painted
        announce();
        return;
    }
    m_message.clear();
    const qacq::model::SampleData &data = *signal->data;
    if (&data != m_pyramidData)
    {
        m_pyramids.clear();
        m_pyramids.resize(data.channelCount());
        for (std::size_t c = 0; c < data.channelCount(); ++c)
        {
            if (data.channels[c].size() >= kPyramidFrom) m_pyramids[c].build(data.channels[c]);
        }
        m_pyramidData = &data;
    }
    m_rate = signal->sampleRateHz > 0.0 ? signal->sampleRateHz : 1.0;
    m_timeOrigin = signal->startSeconds;
    for (std::size_t c = 0; c < data.channelCount(); ++c)
    {
        Channel ch;
        ch.name = text(signal->channels[c].name);
        ch.unit = text(signal->channels[c].unit);
        ch.values = data.channels[c];
        ch.breaks = data.discontinuities;
        ch.pyramid = m_pyramids[c].levelCount() > 0 ? &m_pyramids[c] : nullptr;
        if (sameSignal && c < previous.size())
        {
            ch.range = previous[c].range;
            ch.automatic = previous[c].automatic;
        }
        m_channels.push_back(std::move(ch));
    }
    m_limits = {m_timeOrigin, m_timeOrigin + static_cast<double>(signal->frameCount()) / m_rate};
    if (!sameSignal || !m_viewSet)
    {
        m_view = m_limits;
        m_viewSet = true;
        m_overlayAutomatic = true;
    }
    else
    {
        m_view = qacq::plot::clampRange(m_view, m_limits, minimumSpan());
    }
    announce();
    emit viewChanged();
}

void SignalPlot::refreshLive()
{
    std::vector<Channel> previous = std::move(m_channels);
    m_channels.clear();
    m_signal.reset();
    const bool wasCapture = m_showingCapture;
    qacq::app::AcquisitionController *a = m_acquisition.data();
    if (!a)
    {
        m_message.clear();
        announce();
        return;
    }
    const qacq::acquisition::LiveRecorder &recorder = a->recorder();
    const double declared = a->sampleRate();
    m_rate = declared > 0.0 ? declared : 0.0;
    const double rate = m_rate > 0.0 ? m_rate : 1.0;
    m_showingCapture = a->triggerEnabled();
    m_triggerChannel = -1;
    const std::size_t count = recorder.totalFrames() > 0 ? recorder.channelCount() : 0;
    if (count == 0)
    {
        m_buffers.clear();
        m_message = a->isRunning() ? tr("En attente des premières données…") : QString();
        announce();
        return;
    }
    m_buffers.resize(count);
    std::uint64_t first = 0;
    std::size_t frames = 0;
    double offset = 0.0;
    if (m_showingCapture)
    {
        const qacq::acquisition::TriggerCapture *capture = a->lastCaptureData();
        if (!capture)
        {
            m_buffers.clear();
            m_message = tr("En attente du déclenchement…");
            announce();
            return;
        }
        const qacq::acquisition::TriggerSettings &t = a->triggerSettings();
        const double pre = std::round(static_cast<double>(capture->length) * std::clamp(t.preTrigger, 0.0, 1.0));
        const double instant = capture->forced ? static_cast<double>(capture->firstFrame) + pre
                                               : static_cast<double>(capture->triggerFrame) - 1.0 + capture->triggerFraction;
        for (std::size_t c = 0; c < count; ++c) first = recorder.copy(c, capture->firstFrame, capture->length, m_buffers[c]);
        frames = m_buffers.front().size();
        offset = static_cast<double>(first) - instant;
        m_limits = {(static_cast<double>(capture->firstFrame) - instant) / rate,
                    (static_cast<double>(capture->firstFrame + capture->length) - instant) / rate};
        if (!wasCapture || !m_viewSet) m_view = m_limits;
        else m_view = qacq::plot::clampRange(m_view, m_limits, minimumSpan());
        m_viewSet = true;
        m_triggerChannel = static_cast<int>(t.channel);
        m_triggerLevel = t.level;
    }
    else
    {
        const std::uint64_t total = recorder.totalFrames();
        // Rate not known yet (measured after a second or two): the axis counts frames, 250 per second shown.
        const double rollRate = m_rate > 0.0 ? m_rate : 250.0;
        const auto wanted = static_cast<std::size_t>(std::clamp(std::llround(m_rollSeconds * rollRate), 16LL,
                                                                static_cast<long long>(kMaximumLiveFrames)));
        const std::uint64_t from = std::max(recorder.firstFrame(), total > wanted ? total - wanted : std::uint64_t{0});
        for (std::size_t c = 0; c < count; ++c) first = recorder.copy(c, from, static_cast<std::size_t>(total - from), m_buffers[c]);
        frames = m_buffers.front().size();
        offset = static_cast<double>(first);
        m_limits = {(static_cast<double>(total) - static_cast<double>(wanted)) / rate, static_cast<double>(total) / rate};
        m_view = m_limits;
        m_viewSet = true;
    }
    m_message.clear();
    m_timeOrigin = 0.0;
    const QStringList names = a->channelNames();
    const QStringList units = a->channelUnits();
    const std::vector<std::size_t> breaks = breaksIn(recorder, first, frames);
    for (std::size_t c = 0; c < count; ++c)
    {
        Channel ch;
        const auto i = static_cast<qsizetype>(c);
        ch.name = i < names.size() ? names.at(i) : tr("Voie %1").arg(i + 1);
        ch.unit = i < units.size() ? units.at(i) : QString();
        ch.values = m_buffers[c];
        ch.frameOffset = offset;
        ch.breaks = breaks;
        if (c < previous.size() && wasCapture == m_showingCapture)
        {
            ch.range = previous[c].range;
            ch.automatic = previous[c].automatic;
        }
        m_channels.push_back(std::move(ch));
    }
    announce();
}

void SignalPlot::onFrames()
{
    if (!m_live || m_frozen || !isVisible()) return;
    refreshLive();
    update();
}

double SignalPlot::minimumSpan() const
{
    return 4.0 / (m_rate > 0.0 ? m_rate : 1.0);
}

void SignalPlot::setViewRange(const Range &range)
{
    if (m_live && !m_showingCapture) return;
    const Range r = qacq::plot::clampRange(range, m_limits, minimumSpan());
    if (!r.valid()) return;
    if (r.low == m_view.low && r.high == m_view.high) return;
    m_view = r;
    m_viewSet = true;
    emit viewChanged();
    update();
}

void SignalPlot::showAll()
{
    m_view = m_limits;
    for (Channel &c : m_channels)
    {
        c.automatic = true;
        c.range = {0.0, 0.0};
    }
    m_overlayAutomatic = true;
    m_overlayRange = {0.0, 0.0};
    emit viewChanged();
    update();
}

void SignalPlot::zoomTime(double factor)
{
    if (!(factor > 0.0)) return;
    if (m_live && !m_showingCapture)
    {
        setRollSeconds(m_rollSeconds * factor);
        return;
    }
    setViewRange(qacq::plot::zoomRange(m_view, factor, 0.5 * (m_view.low + m_view.high)));
}

QRectF SignalPlot::plotRect() const
{
    return QRectF(kLeft, kTop, std::max(0.0, width() - kLeft - kRight), std::max(0.0, height() - kTop - kBottom));
}

double SignalPlot::timeAt(double x) const
{
    const QRectF area = plotRect();
    if (!(area.width() > 0.0)) return m_view.low;
    return m_view.low + (x - area.left()) / area.width() * m_view.span();
}

double SignalPlot::xOf(double time) const
{
    const QRectF area = plotRect();
    return area.left() + (time - m_view.low) / m_view.span() * area.width();
}

QColor SignalPlot::channelColour(int index) const
{
    if (m_channelColors.isEmpty()) return m_accent;
    const QVariant v = m_channelColors.at(index % m_channelColors.size());
    QColor c = v.value<QColor>();
    if (!c.isValid()) c = QColor(v.toString());
    return c.isValid() ? c : m_accent;
}

int SignalPlot::laneAt(double y) const
{
    for (std::size_t i = 0; i < m_lastLanes.size(); ++i)
    {
        const QRectF &r = m_lastLanes[i].rect;
        if (y >= r.top() - kLaneGap / 2 && y <= r.bottom() + kLaneGap / 2) return static_cast<int>(i);
    }
    return -1;
}

std::vector<SignalPlot::Lane> SignalPlot::layoutLanes(const QRectF &area, const std::vector<qacq::plot::Trace> &traces)
{
    std::vector<int> visible;
    for (int c = 0; c < static_cast<int>(m_channels.size()); ++c)
    {
        if (!isHidden(c)) visible.push_back(c);
    }
    std::vector<Lane> lanes;
    if (visible.empty()) return lanes;
    const bool steady = m_live && !m_showingCapture;
    const auto fit = [steady](const Range &current, double mn, double mx) {
        if (!(mn <= mx)) return current.valid() ? current : Range{-1.0, 1.0};
        return steady ? qacq::plot::steadyRange(current, mn, mx) : qacq::plot::paddedRange(mn, mx);
    };
    if (m_stacked)
    {
        const auto n = static_cast<double>(visible.size());
        const double h = std::max(10.0, (area.height() - kLaneGap * (n - 1.0)) / n);
        for (std::size_t k = 0; k < visible.size(); ++k)
        {
            const int c = visible[k];
            Channel &ch = m_channels[static_cast<std::size_t>(c)];
            if (ch.automatic) ch.range = fit(ch.range, traces[static_cast<std::size_t>(c)].minimum, traces[static_cast<std::size_t>(c)].maximum);
            Lane lane;
            lane.rect = QRectF(area.left(), area.top() + static_cast<double>(k) * (h + kLaneGap), area.width(), h);
            lane.channels = {c};
            lane.range = ch.range;
            lane.unit = ch.unit;
            lanes.push_back(std::move(lane));
        }
        return lanes;
    }
    double mn = std::numeric_limits<double>::infinity(), mx = -mn;
    QString unit = m_channels[static_cast<std::size_t>(visible.front())].unit;
    for (int c : visible)
    {
        const qacq::plot::Trace &t = traces[static_cast<std::size_t>(c)];
        if (!t.empty())
        {
            mn = std::min(mn, t.minimum);
            mx = std::max(mx, t.maximum);
        }
        if (m_channels[static_cast<std::size_t>(c)].unit != unit) unit.clear();
    }
    if (m_overlayAutomatic) m_overlayRange = fit(m_overlayRange, mn, mx);
    Lane lane;
    lane.rect = area;
    lane.channels = visible;
    lane.range = m_overlayRange;
    lane.unit = unit;
    lanes.push_back(std::move(lane));
    return lanes;
}

void SignalPlot::drawMessage(QPainter &p, const QString &message) const
{
    p.setPen(m_muted);
    p.drawText(boundingRect(), Qt::AlignCenter | Qt::TextWordWrap, message);
}

void SignalPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    QFont font = p.font();
    font.setPixelSize(std::max(8, m_fontSize));
    p.setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF area = plotRect();
    m_lastLanes.clear();
    if (area.width() < 40.0 || area.height() < 30.0) return;
    if (m_channels.empty())
    {
        drawMessage(p, !m_message.isEmpty() ? m_message : !m_emptyText.isEmpty() ? m_emptyText : tr("Aucun signal à afficher"));
        return;
    }
    const double rate = m_rate > 0.0 ? m_rate : 1.0;
    if (!m_view.valid()) m_view = m_limits.valid() ? m_limits : Range{0.0, 1.0};
    const int columns = static_cast<int>(std::floor(area.width()));
    const qacq::plot::XWindow window{(m_view.low - m_timeOrigin) * rate, m_view.span() * rate / columns, columns};

    std::vector<qacq::plot::Trace> traces(m_channels.size());
    for (std::size_t c = 0; c < m_channels.size(); ++c)
    {
        if (isHidden(static_cast<int>(c))) continue;
        const Channel &ch = m_channels[c];
        traces[c] = qacq::plot::buildTrace({ch.values, ch.frameOffset, ch.breaks, ch.pyramid}, window);
    }
    std::vector<Lane> lanes = layoutLanes(area, traces);
    if (lanes.empty())
    {
        drawMessage(p, tr("Toutes les voies sont masquées"));
        return;
    }

    // Time axis: grid across the lanes, labels and title under them.
    const QString tUnit = timeUnit();
    const qacq::plot::Ticks xTicks = qacq::plot::linearTicks(m_view.low, m_view.high, area.width(), 90.0);
    const qacq::plot::AxisLabels xLabels = qacq::plot::axisLabels(xTicks, utf8(tUnit));
    const double gridTop = lanes.front().rect.top();
    const double gridBottom = lanes.back().rect.bottom();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(m_grid, 1.0));
    for (double t : xTicks.major)
    {
        const double x = std::round(xOf(t)) + 0.5;
        for (const Lane &lane : lanes) p.drawLine(QPointF(x, lane.rect.top()), QPointF(x, lane.rect.bottom()));
    }
    p.setPen(QPen(faded(m_muted, 0.6), 1.0));
    for (double t : xTicks.minor)
    {
        const double x = std::round(xOf(t)) + 0.5;
        p.drawLine(QPointF(x, gridBottom), QPointF(x, gridBottom + 3.0));
    }
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (std::size_t i = 0; i < xTicks.major.size(); ++i)
    {
        const QString label = text(xLabels.text[i]);
        const double w = metrics.horizontalAdvance(label);
        const double x = xOf(xTicks.major[i]) - w / 2.0;
        if (x < lastRight + 8.0 || x + w > width() - 2.0) continue;
        p.drawText(QPointF(std::max(2.0, x), gridBottom + 4.0 + metrics.ascent()), label);
        lastRight = x + w;
    }
    QString title = m_rate > 0.0 ? (m_showingCapture ? tr("Temps depuis le déclenchement") : tr("Temps")) : tr("Trame");
    if (!xLabels.unit.empty()) title += QStringLiteral(" (%1)").arg(text(xLabels.unit));
    p.drawText(QRectF(area.left(), gridBottom + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, title);

    // The lanes.
    for (const Lane &lane : lanes)
    {
        const QRectF &r = lane.rect;
        const qacq::plot::YMap map{lane.range.low, lane.range.high, r.top(), r.height(), false};
        const qacq::plot::Ticks yTicks = qacq::plot::linearTicks(lane.range.low, lane.range.high, r.height(), 28.0);
        const qacq::plot::AxisLabels yLabels = qacq::plot::axisLabels(yTicks, utf8(lane.unit));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(m_grid, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
        for (std::size_t i = 0; i < yTicks.major.size(); ++i)
        {
            const double y = std::round(map.toPixel(yTicks.major[i])) + 0.5;
            if (y < r.top() || y > r.bottom()) continue;
            p.setPen(QPen(yTicks.major[i] == 0.0 ? faded(m_muted, 0.55) : m_grid, 1.0));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            const QString label = text(yLabels.text[i]);
            p.setPen(m_muted);
            p.drawText(QRectF(0.0, y - metrics.height() / 2.0, r.left() - 6.0, metrics.height()),
                       Qt::AlignRight | Qt::AlignVCenter, label);
        }

        p.save();
        p.setClipRect(r.adjusted(1.0, 1.0, -1.0, -1.0));
        // Discontinuities of the record: dashed lines (the first channel of the lane carries them).
        const Channel &firstChannel = m_channels[static_cast<std::size_t>(lane.channels.front())];
        if (!firstChannel.breaks.empty())
        {
            QPen dashes(faded(m_muted, 0.7), 1.0, Qt::DashLine);
            p.setPen(dashes);
            int drawn = 0;
            for (std::size_t b : firstChannel.breaks)
            {
                const double t = m_timeOrigin + (firstChannel.frameOffset + static_cast<double>(b)) / rate;
                if (t < m_view.low || t > m_view.high) continue;
                const double x = std::round(xOf(t)) + 0.5;
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                if (++drawn > 300) break;
            }
        }
        // Trigger: its level on its channel, and the instant.
        if (m_showingCapture && std::find(lane.channels.begin(), lane.channels.end(), m_triggerChannel) != lane.channels.end())
        {
            const double y = map.toPixel(m_triggerLevel);
            p.setPen(QPen(faded(m_accent, 0.8), 1.0, Qt::DashLine));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(m_accent);
            p.drawText(QPointF(r.right() - metrics.horizontalAdvance(QStringLiteral("T")) - 6.0, y - 3.0), QStringLiteral("T"));
        }
        if (m_showingCapture && 0.0 >= m_view.low && 0.0 <= m_view.high)
        {
            const double x = std::round(xOf(0.0)) + 0.5;
            p.setPen(QPen(faded(m_accent, 0.6), 1.0, Qt::DashLine));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        // The traces.
        for (int c : lane.channels)
        {
            const qacq::plot::Trace &trace = traces[static_cast<std::size_t>(c)];
            if (trace.columns.empty() && trace.points.empty()) continue;
            const QColor colour = channelColour(c);
            p.setRenderHint(QPainter::Antialiasing, !trace.dense);
            p.setPen(QPen(colour, trace.dense ? 1.0 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const std::vector<qacq::plot::PixelPoint> &line : qacq::plot::traceLines(trace, map))
            {
                if (line.size() == 1)
                {
                    p.drawPoint(QPointF(area.left() + static_cast<double>(line[0].x), static_cast<double>(line[0].y)));
                    continue;
                }
                QPolygonF polygon;
                polygon.reserve(static_cast<qsizetype>(line.size()));
                for (const qacq::plot::PixelPoint &pt : line) polygon << QPointF(area.left() + static_cast<double>(pt.x), static_cast<double>(pt.y));
                p.drawPolyline(polygon);
            }
            // Close enough to see each sample: a dot on it.
            if (!trace.dense && window.framesPerPixel > 0.0 && 1.0 / window.framesPerPixel >= 7.0)
            {
                p.setPen(Qt::NoPen);
                p.setBrush(colour);
                for (const qacq::plot::TracePoint &pt : trace.points)
                {
                    const double y = map.toPixel(pt.value);
                    if (std::isfinite(y)) p.drawEllipse(QPointF(area.left() + pt.x, y), 2.2, 2.2);
                }
                p.setBrush(Qt::NoBrush);
            }
        }
        p.restore();

        // Names of the channels of the lane (and the unit of its scale), in their colour.
        p.setRenderHint(QPainter::Antialiasing, true);
        double x = r.left() + 8.0;
        const double y = r.top() + 5.0;
        for (int c : lane.channels)
        {
            const Channel &ch = m_channels[static_cast<std::size_t>(c)];
            QString name = ch.name;
            if (m_stacked && !yLabels.unit.empty()) name += QStringLiteral(" (%1)").arg(text(yLabels.unit));
            const double w = metrics.horizontalAdvance(name) + 18.0;
            p.setPen(Qt::NoPen);
            p.setBrush(faded(m_panel, 0.85));
            p.drawRoundedRect(QRectF(x - 4.0, y - 2.0, w + 4.0, metrics.height() + 4.0), 4.0, 4.0);
            p.setBrush(channelColour(c));
            p.drawRoundedRect(QRectF(x, y + metrics.height() / 2.0 - 2.0, 10.0, 4.0), 1.5, 1.5);
            p.setBrush(Qt::NoBrush);
            p.setPen(m_text);
            p.drawText(QPointF(x + 15.0, y + metrics.ascent()), name);
            x += w + 8.0;
        }
        if (!m_stacked && !yLabels.unit.empty())
        {
            p.setPen(m_muted);
            p.drawText(QRectF(0.0, r.top() - 2.0, r.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignTop,
                       text(yLabels.unit));
        }
    }

    // The span chosen by a right drag.
    if (m_selecting)
    {
        const double x0 = std::clamp(std::min(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        const double x1 = std::clamp(std::max(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        p.setPen(QPen(faded(m_accent, 0.8), 1.0));
        p.setBrush(faded(m_accent, 0.15));
        p.drawRect(QRectF(x0, gridTop, x1 - x0, gridBottom - gridTop));
        p.setBrush(Qt::NoBrush);
    }
    m_lastLanes = lanes;
    if (m_pointerInside && !m_panning && !m_selecting && area.contains(m_pointer)) drawReadout(p, lanes);
}

void SignalPlot::drawReadout(QPainter &p, const std::vector<Lane> &lanes) const
{
    const double rate = m_rate > 0.0 ? m_rate : 1.0;
    const double t = timeAt(m_pointer.x());
    const QFontMetricsF metrics(p.font());
    QStringList lines;
    std::vector<QColor> colours;
    double shownTime = t;
    bool first = true;
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Lane &lane : lanes)
    {
        const qacq::plot::YMap map{lane.range.low, lane.range.high, lane.rect.top(), lane.rect.height(), false};
        for (int c : lane.channels)
        {
            const Channel &ch = m_channels[static_cast<std::size_t>(c)];
            const std::ptrdiff_t i = qacq::plot::nearestSample((t - m_timeOrigin) * rate - ch.frameOffset, ch.values.size());
            if (i < 0) continue;
            const double v = ch.values[static_cast<std::size_t>(i)];
            const double ts = m_timeOrigin + (ch.frameOffset + static_cast<double>(i)) / rate;
            if (first)
            {
                shownTime = ts;
                first = false;
            }
            const double x = xOf(ts);
            const double y = map.toPixel(v);
            if (std::isfinite(v) && std::isfinite(y) && y >= lane.rect.top() && y <= lane.rect.bottom() && x >= lane.rect.left() &&
                x <= lane.rect.right())
            {
                p.setPen(QPen(m_background, 1.5));
                p.setBrush(channelColour(c));
                p.drawEllipse(QPointF(x, y), 3.5, 3.5);
            }
            // As many digits as tell apart two values a thousandth of the lane apart (2,400000301 GHz).
            const int digits = qacq::plot::significantFor(v, lane.range.span() / 1000.0);
            lines << QStringLiteral("%1 : %2").arg(ch.name, text(qacq::plot::formatQuantity(v, utf8(ch.unit), digits)));
            colours.push_back(channelColour(c));
        }
    }
    p.setBrush(Qt::NoBrush);
    const QRectF area = plotRect();
    const double x = std::round(xOf(shownTime)) + 0.5;
    p.setPen(QPen(faded(m_text, 0.35), 1.0, Qt::DotLine));
    p.drawLine(QPointF(x, lanes.front().rect.top()), QPointF(x, lanes.back().rect.bottom()));

    const int timeDigits = qacq::plot::significantFor(shownTime, m_view.span() / 1000.0, 4, 12);
    const QString head = m_rate > 0.0 ? QStringLiteral("t = %1").arg(text(qacq::plot::formatQuantity(shownTime, "s", timeDigits)))
                                      : tr("Trame %1").arg(std::llround(shownTime));
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

void SignalPlot::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    update();
}

void SignalPlot::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0 || m_channels.empty())
    {
        event->ignore();
        return;
    }
    event->accept();
    const double factor = std::pow(0.8, steps); // away from the user: zoom in
    const QPointF at = event->position();
    if (event->modifiers() & Qt::ControlModifier)
    {
        const int laneIndex = laneAt(at.y());
        if (laneIndex < 0) return;
        const Lane &lane = m_lastLanes[static_cast<std::size_t>(laneIndex)];
        const qacq::plot::YMap map{lane.range.low, lane.range.high, lane.rect.top(), lane.rect.height(), false};
        const Range r = qacq::plot::zoomRange(lane.range, factor, map.toValue(at.y()));
        if (!r.valid()) return;
        if (m_stacked)
        {
            for (int c : lane.channels)
            {
                m_channels[static_cast<std::size_t>(c)].range = r;
                m_channels[static_cast<std::size_t>(c)].automatic = false;
            }
        }
        else
        {
            m_overlayRange = r;
            m_overlayAutomatic = false;
        }
        update();
        return;
    }
    if (m_live && !m_showingCapture)
    {
        setRollSeconds(m_rollSeconds * factor);
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier)
    {
        const double shift = -0.1 * steps * m_view.span();
        setViewRange({m_view.low + shift, m_view.high + shift});
        return;
    }
    setViewRange(qacq::plot::zoomRange(m_view, factor, timeAt(at.x())));
}

void SignalPlot::mousePressEvent(QMouseEvent *event)
{
    // A click on the plot leaves the field being edited (Space starts and stops again).
    forceActiveFocus(Qt::MouseFocusReason);
    m_pressAt = event->position();
    m_pointer = m_pressAt;
    m_pressView = m_view;
    if (event->button() == Qt::RightButton && !(m_live && !m_showingCapture)) m_selecting = true;
    else if (event->button() == Qt::LeftButton && !(m_live && !m_showingCapture)) m_panning = true;
    event->accept();
    update();
}

void SignalPlot::mouseMoveEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_panning)
    {
        const QRectF area = plotRect();
        if (area.width() > 0.0)
        {
            const double dt = -(m_pointer.x() - m_pressAt.x()) / area.width() * m_pressView.span();
            setViewRange({m_pressView.low + dt, m_pressView.high + dt});
        }
    }
    update();
}

void SignalPlot::mouseReleaseEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_selecting && std::abs(m_pointer.x() - m_pressAt.x()) > 4.0)
    {
        const double t0 = timeAt(std::min(m_pressAt.x(), m_pointer.x()));
        const double t1 = timeAt(std::max(m_pressAt.x(), m_pointer.x()));
        setViewRange({t0, t1});
    }
    m_selecting = false;
    m_panning = false;
    update();
}

void SignalPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    showAll();
}

void SignalPlot::hoverMoveEvent(QHoverEvent *event)
{
    m_pointer = event->position();
    m_pointerInside = true;
    update();
}

void SignalPlot::hoverLeaveEvent(QHoverEvent *event)
{
    Q_UNUSED(event);
    m_pointerInside = false;
    update();
}
