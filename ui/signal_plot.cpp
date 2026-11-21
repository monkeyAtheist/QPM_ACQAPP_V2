#include "signal_plot.h"

#include "qacq/acquisition/recorder.h"
#include "qacq/analysis/calculus.h"
#include "qacq/analysis/measures.h"
#include "qacq/acquisition/trigger.h"
#include "qacq/app/acquisition_controller.h"
#include "qacq/app/curves_controller.h"
#include "qacq/app/format.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/signal.h"

#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QKeyEvent>
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
constexpr double kRightAxis = 62.0; // room for a scale on the right
constexpr double kTop = 8.0;
constexpr double kTopShared = 20.0; // one axis for every channel: its unit above it
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
    // Whether cursors can be shown depends on the data and the mode (stopped acquisition: yes).
    connect(this, &SignalPlot::contentChanged, this, &SignalPlot::cursorsChanged);
    connect(this, &SignalPlot::sourceChanged, this, &SignalPlot::cursorsChanged);
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
    // An acquisition that stops on its rolling display: the signal it became is shown as the last
    // window was (its end), the whole record a slide away (drag, scroll bar, arrows) rather than
    // squeezed into the plot.
    const bool stopping = m_live && !live && !m_showingCapture;
    const qacq::app::AcquisitionController *acquisition = m_acquisition.data(); // one read of the QPointer
    m_live = live;
    m_viewSet = false;
    for (Channel &c : m_channels) c.range = {0.0, 0.0};
    m_overlayRange = {0.0, 0.0};
    m_overlayRightRange = {0.0, 0.0};
    reload();
    if (stopping && m_signal && acquisition != nullptr && acquisition->lastSignalId() == m_signalId &&
        m_limits.span() > m_rollSeconds * 1.001)
    {
        m_view = {m_limits.high - m_rollSeconds, m_limits.high};
        m_viewSet = true;
        emit viewChanged();
        update();
    }
    emit sourceChanged();
}

double SignalPlot::rollSeconds() const
{
    // Stopped (a signal of the session on screen): the width of the view.
    if ((m_live && !m_showingCapture) || m_channels.empty() || !m_view.valid()) return m_rollSeconds;
    return m_view.span();
}

void SignalPlot::setRollSeconds(double seconds)
{
    if (!std::isfinite(seconds)) return;
    seconds = std::clamp(seconds, 1e-4, 3600.0);
    if (!m_live && !m_channels.empty() && m_view.valid())
    {
        // Not rolling: the view takes this width, ending where it ends (the next acquisition too).
        m_rollSeconds = seconds;
        if (std::abs(seconds - m_view.span()) <= 1e-12 * seconds) return;
        rememberView();
        const double end = std::min(m_view.high, m_limits.high);
        setViewRange({end - seconds, end});
        emit viewChanged();
        return;
    }
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
    emit cursorsChanged();
    if (!m_frozen && m_live) refreshLive();
    emit viewChanged();
    update();
}

void SignalPlot::setStacked(bool stacked)
{
    if (stacked == m_stacked) return;
    m_stacked = stacked;
    emit viewChanged();
    noteScales();
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
    noteScales();
    if (m_cursorsVisible) refreshCursors();
    update();
}

bool SignalPlot::isHidden(int channel) const
{
    if (m_curves && channel >= 0 && static_cast<std::size_t>(channel) < m_curveVisible.size() &&
        !m_curveVisible[static_cast<std::size_t>(channel)])
        return true;
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
        if (!m_live && !m_curves) refreshSignal();
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
    else if (m_curves) refreshCurves();
    else refreshSignal();
    update();
}

void SignalPlot::announce()
{
    noteScales(); // the channels may have changed (another signal: automatic scales)
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
        m_overlayRightAutomatic = true;
    }
    else
    {
        m_view = qacq::plot::clampRange(m_view, m_limits, minimumSpan());
    }
    announce();
    if (!sameSignal) m_history.clear();
    emit viewChanged();
    if (m_cursorsVisible) refreshCursors();
}

QObject *SignalPlot::curves() const
{
    return m_curves.data();
}

void SignalPlot::setCurves(QObject *curves)
{
    auto *c = qobject_cast<qacq::app::CurvesController *>(curves);
    if (c == m_curves.data()) return;
    for (const QMetaObject::Connection &k : m_curveConnections) disconnect(k);
    m_curveConnections.clear();
    m_curves = c;
    if (c)
        m_curveConnections << connect(c, &qacq::app::CurvesController::timeCurvesChanged, this, [this] {
            if (!m_live) refreshCurves();
            update();
        });
    reload();
    emit sourceChanged();
}

void SignalPlot::refreshCurves()
{
    m_showingCapture = false;
    m_triggerChannel = -1;
    m_buffers.clear();
    const qacq::app::CurvesController *controller = m_curves.data(); // one read of the QPointer
    std::shared_ptr<const qacq::analysis::TimeCurveSet> set = controller ? controller->timeCurves() : nullptr;
    const qacq::model::SignalPtr signal = set ? set->signal : nullptr;
    const bool sameSignal = m_signal && signal && m_signal->id == signal->id;
    std::vector<Channel> previous = std::move(m_channels);
    m_channels.clear();
    m_curveVisible.clear();
    m_curveSet = set;
    m_signal = signal;
    if (!signal || set->curves.empty() || signal->frameCount() == 0)
    {
        m_rate = 0.0;
        m_curvePyramids.clear();
        m_message.clear();
        announce();
        return;
    }
    m_message.clear();
    m_rate = signal->sampleRateHz > 0.0 ? signal->sampleRateHz : 1.0;
    m_timeOrigin = signal->startSeconds;
    // Pyramids of the long curves, kept while the same samples are drawn.
    std::map<const double *, qacq::plot::MinMaxPyramid> pyramids;
    for (const qacq::analysis::TimeCurve &curve : set->curves)
    {
        if (curve.values.size() < kPyramidFrom) continue;
        const double *key = curve.values.data();
        if (pyramids.count(key)) continue;
        const auto old = m_curvePyramids.find(key);
        if (old != m_curvePyramids.end()) pyramids.emplace(key, std::move(old->second));
        else pyramids[key].build(curve.values);
    }
    m_curvePyramids = std::move(pyramids);
    const std::vector<std::size_t> noBreaks;
    const std::vector<std::size_t> &breaks = signal->data ? signal->data->discontinuities : noBreaks;
    for (std::size_t k = 0; k < set->curves.size(); ++k)
    {
        const qacq::analysis::TimeCurve &curve = set->curves[k];
        Channel ch;
        ch.name = text(curve.name);
        ch.unit = text(curve.unit);
        ch.values = curve.values;
        ch.frameOffset = static_cast<double>(curve.firstFrame);
        for (std::size_t b : breaks)
        {
            if (b > curve.firstFrame && b < curve.firstFrame + curve.values.size()) ch.breaks.push_back(b - curve.firstFrame);
        }
        const auto pyramid = m_curvePyramids.find(curve.values.data());
        ch.pyramid = pyramid != m_curvePyramids.end() && !curve.values.empty() ? &pyramid->second : nullptr;
        if (sameSignal && k < previous.size() && previous[k].name == ch.name)
        {
            ch.range = previous[k].range;
            ch.automatic = previous[k].automatic;
        }
        m_channels.push_back(std::move(ch));
        m_curveVisible.push_back(curve.visible);
    }
    m_limits = {m_timeOrigin, m_timeOrigin + static_cast<double>(signal->frameCount()) / m_rate};
    if (!sameSignal || !m_viewSet)
    {
        m_view = m_limits;
        m_viewSet = true;
        m_overlayAutomatic = true;
        m_overlayRightAutomatic = true;
    }
    else
    {
        m_view = qacq::plot::clampRange(m_view, m_limits, minimumSpan());
    }
    announce();
    if (!sameSignal) m_history.clear();
    emit viewChanged();
    if (m_cursorsVisible) refreshCursors();
}

void SignalPlot::setTimeView(double low, double high)
{
    if (!std::isfinite(low) || !std::isfinite(high) || !(high > low) || (m_live && !m_showingCapture)) return;
    if (low == m_view.low && high == m_view.high) return;
    setViewRange({low, high});
}

void SignalPlot::setCursorA(double time)
{
    if (!std::isfinite(time) || time == m_cursorA) return;
    m_cursorA = time;
    m_cursorsPlaced = true;
    refreshCursors();
    update();
}

void SignalPlot::setCursorB(double time)
{
    if (!std::isfinite(time) || time == m_cursorB) return;
    m_cursorB = time;
    m_cursorsPlaced = true;
    refreshCursors();
    update();
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
    if (m_cursorsVisible) refreshCursors();
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

qint64 SignalPlot::viewFirstFrame() const
{
    if (!m_signal || m_live || !(m_rate > 0.0)) return 0;
    const auto frames = static_cast<double>(m_signal->frameCount());
    const double first = std::ceil((m_view.low - m_timeOrigin) * m_rate - 1e-6);
    return static_cast<qint64>(std::clamp(first, 0.0, frames));
}

qint64 SignalPlot::viewFrameCount() const
{
    if (!m_signal || m_live || !(m_rate > 0.0)) return -1;
    const auto frames = static_cast<double>(m_signal->frameCount());
    const double end = std::clamp(std::ceil((m_view.high - m_timeOrigin) * m_rate - 1e-6), 0.0, frames);
    return std::max<qint64>(0, static_cast<qint64>(end) - viewFirstFrame());
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
    rememberView();
    m_view = m_limits;
    resetScales();
    emit viewChanged();
    noteScales();
    update();
}

void SignalPlot::resetScales()
{
    for (Channel &c : m_channels)
    {
        c.automatic = true;
        c.range = {0.0, 0.0};
    }
    m_overlayAutomatic = true;
    m_overlayRange = {0.0, 0.0};
    m_overlayRightAutomatic = true;
    m_overlayRightRange = {0.0, 0.0};
}

void SignalPlot::autoScale()
{
    resetScales();
    noteScales();
    update();
}

bool SignalPlot::manualScales() const
{
    // As layoutLanes lays them out: stacked, each channel its scale; superposed, one or two units share
    // the scales of the overlay, more units a lane each with the scale of its channels.
    QStringList units;
    bool channelSetByHand = false;
    for (int c = 0; c < static_cast<int>(m_channels.size()); ++c)
    {
        if (isHidden(c)) continue;
        const Channel &ch = m_channels[static_cast<std::size_t>(c)];
        channelSetByHand = channelSetByHand || !ch.automatic;
        if (!units.contains(ch.unit)) units << ch.unit;
    }
    if (units.isEmpty()) return false;
    if (m_stacked || units.size() > 2) return channelSetByHand;
    return !m_overlayAutomatic || (units.size() == 2 && !m_overlayRightAutomatic);
}

void SignalPlot::noteScales()
{
    const bool manual = manualScales();
    if (manual == m_manualScalesNoted) return;
    m_manualScalesNoted = manual;
    emit scalesChanged();
}

SignalPlot::Place SignalPlot::placeAt(const QPointF &at, int *lane) const
{
    *lane = -1;
    if (m_lastLanes.empty()) return Place::None;
    const QRectF area = plotRect();
    const bool overArea = at.x() >= area.left() && at.x() <= area.right();
    if (at.y() > m_lastLanes.back().rect.bottom() + kLaneGap / 2) return overArea ? Place::TimeAxis : Place::None;
    const int index = laneAt(at.y());
    if (index < 0) return overArea ? Place::Plot : Place::None; // above the lanes: the time only
    *lane = index;
    if (at.x() < area.left()) return Place::LeftScale;
    if (at.x() > area.right())
        return m_lastLanes[static_cast<std::size_t>(index)].rightChannels.empty() ? Place::None : Place::RightScale;
    return Place::Plot;
}

Range SignalPlot::scaleOf(const Lane &lane, bool right) const
{
    if (lane.perChannel)
    {
        const auto c = static_cast<std::size_t>(lane.channels.front());
        return c < m_channels.size() ? m_channels[c].range : lane.range;
    }
    return right ? m_overlayRightRange : m_overlayRange;
}

void SignalPlot::setLaneScale(const Lane &lane, bool right, const Range &range)
{
    if (!range.valid()) return;
    if (lane.perChannel)
    {
        for (int c : lane.channels)
        {
            if (c < 0 || static_cast<std::size_t>(c) >= m_channels.size()) continue;
            m_channels[static_cast<std::size_t>(c)].range = range;
            m_channels[static_cast<std::size_t>(c)].automatic = false;
        }
    }
    else if (right)
    {
        m_overlayRightRange = range;
        m_overlayRightAutomatic = false;
    }
    else
    {
        m_overlayRange = range;
        m_overlayAutomatic = false;
    }
    noteScales();
    update();
}

void SignalPlot::autoScaleLane(const Lane &lane, bool right)
{
    if (lane.perChannel)
    {
        for (int c : lane.channels)
        {
            if (c < 0 || static_cast<std::size_t>(c) >= m_channels.size()) continue;
            m_channels[static_cast<std::size_t>(c)].automatic = true;
            m_channels[static_cast<std::size_t>(c)].range = {0.0, 0.0};
        }
    }
    else if (right)
    {
        m_overlayRightAutomatic = true;
        m_overlayRightRange = {0.0, 0.0};
    }
    else
    {
        m_overlayAutomatic = true;
        m_overlayRange = {0.0, 0.0};
    }
    noteScales();
    update();
}

QString SignalPlot::scaleKeyAt(const QPointF &at) const
{
    if (m_channels.empty()) return {};
    int index = -1;
    const Place place = placeAt(at, &index);
    if (place == Place::TimeAxis) return QStringLiteral("time");
    if ((place != Place::LeftScale && place != Place::RightScale) || index < 0 ||
        static_cast<std::size_t>(index) >= m_lastLanes.size())
        return {};
    const bool right = place == Place::RightScale;
    const Lane &lane = m_lastLanes[static_cast<std::size_t>(index)];
    const std::vector<int> &channels = right ? lane.rightChannels : lane.channels;
    if (channels.empty()) return {};
    return QStringLiteral("values:%1:%2").arg(channels.front()).arg(right ? QStringLiteral("right") : QStringLiteral("left"));
}

const SignalPlot::Lane *SignalPlot::laneOfScale(const QString &key, bool *right) const
{
    const QStringList parts = key.split(QLatin1Char(':'));
    if (parts.size() != 3 || parts.at(0) != QStringLiteral("values")) return nullptr;
    bool ok = false;
    const int channel = parts.at(1).toInt(&ok);
    if (!ok) return nullptr;
    *right = parts.at(2) == QStringLiteral("right");
    for (const Lane &lane : m_lastLanes)
    {
        const std::vector<int> &channels = *right ? lane.rightChannels : lane.channels;
        if (std::find(channels.begin(), channels.end(), channel) != channels.end()) return &lane;
    }
    return nullptr;
}

QVariantMap SignalPlot::scaleInfo(const QString &key) const
{
    QVariantMap m;
    if (key == QStringLiteral("time"))
    {
        if (m_channels.empty()) return m;
        m.insert(QStringLiteral("key"), key);
        m.insert(QStringLiteral("kind"), QStringLiteral("time"));
        m.insert(QStringLiteral("title"), tr("Axe du temps"));
        m.insert(QStringLiteral("unit"), timeUnit());
        m.insert(QStringLiteral("low"), m_view.low);
        m.insert(QStringLiteral("high"), m_view.high);
        m.insert(QStringLiteral("automatic"), !zoomed());
        m.insert(QStringLiteral("lowLabel"), tr("Début"));
        m.insert(QStringLiteral("highLabel"), tr("Fin"));
        m.insert(QStringLiteral("autoText"), tr("Tout le signal"));
        if (m_live && !m_showingCapture)
            m.insert(QStringLiteral("locked"), tr("En direct, la largeur de la fenêtre se règle avec le champ Fenêtre du ruban."));
        return m;
    }
    bool right = false;
    const Lane *lane = laneOfScale(key, &right);
    if (!lane) return m;
    const std::vector<int> &channels = right ? lane->rightChannels : lane->channels;
    QStringList names;
    bool automatic = true;
    for (int c : channels)
    {
        if (c < 0 || static_cast<std::size_t>(c) >= m_channels.size()) continue;
        const Channel &ch = m_channels[static_cast<std::size_t>(c)];
        automatic = automatic && ch.automatic;
        if (names.size() < 3) names << ch.name;
        else if (names.size() == 3) names << QStringLiteral("…");
    }
    if (!lane->perChannel) automatic = right ? m_overlayRightAutomatic : m_overlayAutomatic;
    const QString unit = right ? lane->rightUnit : lane->unit;
    const Range r = scaleOf(*lane, right);
    m.insert(QStringLiteral("key"), key);
    m.insert(QStringLiteral("kind"), QStringLiteral("values"));
    m.insert(QStringLiteral("title"), names.join(QStringLiteral(", ")) + (unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit)));
    m.insert(QStringLiteral("unit"), unit);
    m.insert(QStringLiteral("low"), r.low);
    m.insert(QStringLiteral("high"), r.high);
    m.insert(QStringLiteral("automatic"), automatic);
    m.insert(QStringLiteral("lowLabel"), tr("Minimum"));
    m.insert(QStringLiteral("highLabel"), tr("Maximum"));
    m.insert(QStringLiteral("autoText"), tr("Automatique"));
    return m;
}

bool SignalPlot::setScaleRange(const QString &key, double low, double high)
{
    if (!std::isfinite(low) || !std::isfinite(high) || !(high > low)) return false;
    if (key == QStringLiteral("time"))
    {
        if (m_channels.empty() || (m_live && !m_showingCapture)) return false;
        rememberView();
        setViewRange({low, high}); // within the signal, at least a few samples
        return true;
    }
    bool right = false;
    const Lane *lane = laneOfScale(key, &right);
    if (!lane) return false;
    setLaneScale(*lane, right, {low, high});
    return true;
}

void SignalPlot::setScaleAutomatic(const QString &key)
{
    if (key == QStringLiteral("time"))
    {
        if (m_channels.empty() || (m_live && !m_showingCapture)) return;
        rememberView();
        setViewRange(m_limits);
        return;
    }
    bool right = false;
    const Lane *lane = laneOfScale(key, &right);
    if (lane) autoScaleLane(*lane, right);
}

void SignalPlot::dragValues(double dy)
{
    if (m_grabLane < 0 || static_cast<std::size_t>(m_grabLane) >= m_lastLanes.size()) return;
    const Lane &lane = m_lastLanes[static_cast<std::size_t>(m_grabLane)];
    if (!(lane.rect.height() > 0.0)) return;
    // The curves follow the pointer: dragged down, the scale shows higher values. Both scales of two
    // superposed units move together (the picture slides), unless one of them is the one grabbed.
    const double fraction = dy / lane.rect.height();
    if (m_grab != Place::RightScale) setLaneScale(lane, false, qacq::plot::shiftRange(m_pressRange, fraction));
    if (m_grab != Place::LeftScale && !lane.rightChannels.empty())
        setLaneScale(lane, true, qacq::plot::shiftRange(m_pressRightRange, fraction));
}

void SignalPlot::slideValues(double fraction)
{
    const int under = m_pointerInside ? laneAt(m_pointer.y()) : -1;
    for (std::size_t i = 0; i < m_lastLanes.size(); ++i)
    {
        if (under >= 0 && static_cast<int>(i) != under) continue;
        const Lane &lane = m_lastLanes[i];
        setLaneScale(lane, false, qacq::plot::shiftRange(scaleOf(lane, false), fraction));
        if (!lane.rightChannels.empty()) setLaneScale(lane, true, qacq::plot::shiftRange(scaleOf(lane, true), fraction));
    }
}

void SignalPlot::updateCursorShape()
{
    int lane = -1;
    const Place place = placeAt(m_pointer, &lane);
    Qt::CursorShape shape = Qt::CrossCursor;
    if (place == Place::LeftScale || place == Place::RightScale) shape = Qt::SizeVerCursor;
    else if (m_cursorsVisible && cursorsAvailable() && cursorNear(m_pointer.x()) != 0) shape = Qt::SizeHorCursor;
    else if (place == Place::TimeAxis && !(m_live && !m_showingCapture)) shape = Qt::SizeHorCursor;
    setCursor(QCursor(shape));
}

void SignalPlot::zoomTime(double factor)
{
    if (!(factor > 0.0)) return;
    if (m_live && !m_showingCapture)
    {
        setRollSeconds(m_rollSeconds * factor);
        return;
    }
    rememberView();
    setViewRange(qacq::plot::zoomRange(m_view, factor, 0.5 * (m_view.low + m_view.high)));
}

QRectF SignalPlot::plotRect() const
{
    const double top = m_stacked ? kTop : kTopShared;
    const double right = m_rightAxisRoom || rightAxisWanted() ? kRightAxis : kRight;
    return QRectF(kLeft, top, std::max(0.0, width() - kLeft - right), std::max(0.0, height() - top - kBottom));
}

bool SignalPlot::rightAxisWanted() const
{
    if (m_stacked) return false;
    QStringList units;
    for (int c = 0; c < static_cast<int>(m_channels.size()); ++c)
    {
        if (isHidden(c)) continue;
        const QString &unit = m_channels[static_cast<std::size_t>(c)].unit;
        if (!units.contains(unit)) units << unit;
    }
    return units.size() == 2;
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
            lane.perChannel = true;
            lanes.push_back(std::move(lane));
        }
        return lanes;
    }
    // Superposed, grouped by unit. One or two units: one lane, the second unit on a scale of its own on
    // the right. More: one lane per unit, its channels superposed.
    std::vector<QString> units;
    for (int c : visible)
    {
        const QString &u = m_channels[static_cast<std::size_t>(c)].unit;
        if (std::find(units.begin(), units.end(), u) == units.end()) units.push_back(u);
    }
    const auto extent = [&traces](const std::vector<int> &channels) {
        double mn = std::numeric_limits<double>::infinity(), mx = -mn;
        for (int c : channels)
        {
            const qacq::plot::Trace &t = traces[static_cast<std::size_t>(c)];
            if (t.empty()) continue;
            mn = std::min(mn, t.minimum);
            mx = std::max(mx, t.maximum);
        }
        return std::pair<double, double>{mn, mx};
    };
    const auto ofUnit = [&](const QString &unit) {
        std::vector<int> channels;
        for (int c : visible)
        {
            if (m_channels[static_cast<std::size_t>(c)].unit == unit) channels.push_back(c);
        }
        return channels;
    };
    if (units.size() <= 2)
    {
        const std::vector<int> left = ofUnit(units.front());
        const auto [mn, mx] = extent(left);
        if (m_overlayAutomatic) m_overlayRange = fit(m_overlayRange, mn, mx);
        Lane lane;
        lane.rect = area;
        lane.channels = visible;
        lane.range = m_overlayRange;
        lane.unit = units.front();
        if (units.size() == 2)
        {
            lane.rightChannels = ofUnit(units.back());
            const auto [rmn, rmx] = extent(lane.rightChannels);
            if (m_overlayRightAutomatic) m_overlayRightRange = fit(m_overlayRightRange, rmn, rmx);
            lane.rightRange = m_overlayRightRange;
            lane.rightUnit = units.back();
        }
        lanes.push_back(std::move(lane));
        return lanes;
    }
    const auto n = static_cast<double>(units.size());
    const double h = std::max(10.0, (area.height() - kLaneGap * (n - 1.0)) / n);
    for (std::size_t k = 0; k < units.size(); ++k)
    {
        const std::vector<int> channels = ofUnit(units[k]);
        // One scale for the channels of the lane (a zoom of the values applies to them all).
        bool automatic = true;
        for (int c : channels) automatic = automatic && m_channels[static_cast<std::size_t>(c)].automatic;
        Range range = m_channels[static_cast<std::size_t>(channels.front())].range;
        if (automatic)
        {
            const auto [mn, mx] = extent(channels);
            range = fit(range, mn, mx);
        }
        for (int c : channels) m_channels[static_cast<std::size_t>(c)].range = range;
        Lane lane;
        lane.rect = QRectF(area.left(), area.top() + static_cast<double>(k) * (h + kLaneGap), area.width(), h);
        lane.channels = channels;
        lane.range = range;
        lane.unit = units[k];
        lane.perChannel = true;
        lanes.push_back(std::move(lane));
    }
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
        const bool hasRight = !lane.rightChannels.empty();
        const qacq::plot::YMap rightMap{lane.rightRange.low, lane.rightRange.high, r.top(), r.height(), false};
        const qacq::plot::Ticks rightTicks = hasRight ? qacq::plot::linearTicks(lane.rightRange.low, lane.rightRange.high, r.height(), 28.0)
                                                      : qacq::plot::Ticks{};
        const qacq::plot::AxisLabels rightLabels = qacq::plot::axisLabels(rightTicks, utf8(lane.rightUnit));
        // Two scales: each in the colour of its channel when it has only one, so that the eye pairs them.
        QColor leftTint = m_muted, rightTint = m_muted;
        if (hasRight)
        {
            const auto leftCount = std::count_if(lane.channels.begin(), lane.channels.end(), [&lane](int c) { return !lane.onRight(c); });
            if (leftCount == 1)
            {
                for (int c : lane.channels)
                {
                    if (!lane.onRight(c)) leftTint = channelColour(c);
                }
            }
            if (lane.rightChannels.size() == 1) rightTint = channelColour(lane.rightChannels.front());
        }
        // The scale on the right: graduations outside the frame, no grid of its own.
        for (std::size_t i = 0; i < rightTicks.major.size(); ++i)
        {
            const double y = std::round(rightMap.toPixel(rightTicks.major[i])) + 0.5;
            if (y < r.top() || y > r.bottom()) continue;
            p.setPen(QPen(m_grid, 1.0));
            p.drawLine(QPointF(r.right(), y), QPointF(r.right() + 4.0, y));
            p.setPen(rightTint);
            p.drawText(QRectF(r.right() + 7.0, y - metrics.height() / 2.0, std::max(0.0, width() - r.right() - 9.0), metrics.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, text(rightLabels.text[i]));
        }
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
            p.setPen(leftTint);
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
            const qacq::plot::YMap &scale = lane.onRight(c) ? rightMap : map;
            p.setRenderHint(QPainter::Antialiasing, !trace.dense);
            p.setPen(QPen(colour, trace.dense ? 1.0 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const std::vector<qacq::plot::PixelPoint> &line : qacq::plot::traceLines(trace, scale))
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
                    const double y = scale.toPixel(pt.value);
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
            // The unit of its scale beside each name, unless the lane has a single scale titled above it.
            const std::string &unitShown = lane.onRight(c) ? rightLabels.unit : yLabels.unit;
            if ((lane.perChannel || hasRight) && !unitShown.empty()) name += QStringLiteral(" (%1)").arg(text(unitShown));
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
        if (!lane.perChannel && !yLabels.unit.empty())
        {
            p.setPen(leftTint);
            p.drawText(QRectF(0.0, r.top() - metrics.height() - 3.0, r.left() - 6.0, metrics.height()),
                       Qt::AlignRight | Qt::AlignTop, text(yLabels.unit));
        }
        if (hasRight && !rightLabels.unit.empty())
        {
            p.setPen(rightTint);
            p.drawText(QRectF(r.right() + 7.0, r.top() - metrics.height() - 3.0, std::max(0.0, width() - r.right() - 9.0), metrics.height()),
                       Qt::AlignLeft | Qt::AlignTop, text(rightLabels.unit));
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
    drawCursors(p, lanes);
    if (m_pointerInside && !m_panning && !m_selecting && m_dragging == 0 && area.contains(m_pointer)) drawReadout(p, lanes);
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
        for (int c : lane.channels)
        {
            const Range &range = lane.rangeOf(c);
            const qacq::plot::YMap map{range.low, range.high, lane.rect.top(), lane.rect.height(), false};
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
            const int digits = qacq::plot::significantFor(v, range.span() / 1000.0);
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

bool SignalPlot::cursorsAvailable() const
{
    return !m_channels.empty() && (!m_live || m_frozen || m_showingCapture);
}

void SignalPlot::setCursorsVisible(bool visible)
{
    if (visible == m_cursorsVisible) return;
    m_cursorsVisible = visible;
    if (visible)
    {
        const bool inView = m_cursorA >= m_view.low && m_cursorA <= m_view.high && m_cursorB >= m_view.low &&
                            m_cursorB <= m_view.high;
        if (!m_cursorsPlaced || !inView) placeCursors();
    }
    refreshCursors();
    update();
}

void SignalPlot::placeCursors()
{
    m_cursorA = m_view.low + m_view.span() / 3.0;
    m_cursorB = m_view.low + 2.0 * m_view.span() / 3.0;
    m_cursorsPlaced = true;
}

int SignalPlot::cursorNear(double x) const
{
    if (!m_cursorsVisible) return 0;
    const double da = std::abs(x - xOf(m_cursorA)), db = std::abs(x - xOf(m_cursorB));
    if (std::min(da, db) > 6.0) return 0;
    return da <= db ? 1 : 2;
}

void SignalPlot::rememberView(bool coalesce)
{
    if ((m_live && !m_showingCapture) || !m_view.valid()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameGesture = coalesce && now - m_lastRemembered < 600;
    m_lastRemembered = coalesce ? now : 0;
    if (sameGesture) return;
    if (!m_history.empty() && m_history.back().low == m_view.low && m_history.back().high == m_view.high) return;
    m_history.push_back(m_view);
    if (m_history.size() > 50) m_history.erase(m_history.begin());
    emit viewChanged();
}

void SignalPlot::goBack()
{
    if (m_history.empty()) return;
    const Range previous = m_history.back();
    m_history.pop_back();
    const Range r = qacq::plot::clampRange(previous, m_limits, minimumSpan());
    if (r.valid()) m_view = r;
    m_viewSet = true;
    emit viewChanged();
    update();
}

void SignalPlot::refreshCursors()
{
    QVariantMap info;
    if (m_cursorsVisible && cursorsAvailable())
    {
        const double rate = m_rate > 0.0 ? m_rate : 1.0;
        const double lo = std::min(m_cursorA, m_cursorB), hi = std::max(m_cursorA, m_cursorB);
        const double dt = m_cursorB - m_cursorA;
        const int digits = qacq::plot::significantFor(std::max(std::abs(m_cursorA), std::abs(m_cursorB)),
                                                      m_view.span() / 1000.0, 4, 12);
        const auto timeText = [&](double t) {
            return m_rate > 0.0 ? text(qacq::plot::formatQuantity(t, "s", digits))
                                : tr("trame %1").arg(std::llround(t));
        };
        info[QStringLiteral("a")] = timeText(m_cursorA);
        info[QStringLiteral("b")] = timeText(m_cursorB);
        info[QStringLiteral("delta")] =
            m_rate > 0.0 ? text(qacq::plot::formatQuantity(dt, "s", qacq::plot::significantFor(dt, m_view.span() / 1000.0, 3, 9)))
                         : tr("%1 trames").arg(std::llround(dt));
        info[QStringLiteral("inverse")] =
            m_rate > 0.0 && std::abs(dt) > 0.0 ? text(qacq::plot::formatQuantity(1.0 / std::abs(dt), "Hz", 4)) : QString();
        QVariantList rows;
        for (int c = 0; c < static_cast<int>(m_channels.size()); ++c)
        {
            if (isHidden(c)) continue;
            const Channel &ch = m_channels[static_cast<std::size_t>(c)];
            const auto at = [&](double t) {
                const std::ptrdiff_t i = qacq::plot::nearestSample((t - m_timeOrigin) * rate - ch.frameOffset, ch.values.size());
                return i < 0 ? std::numeric_limits<double>::quiet_NaN() : ch.values[static_cast<std::size_t>(i)];
            };
            const double va = at(m_cursorA), vb = at(m_cursorB);
            const std::string unit = utf8(ch.unit);
            // The samples between A and B (both included).
            const auto n = static_cast<double>(ch.values.size());
            const double first = std::clamp(std::ceil((lo - m_timeOrigin) * rate - ch.frameOffset - 1e-9), 0.0, n);
            const double last = std::clamp(std::floor((hi - m_timeOrigin) * rate - ch.frameOffset + 1e-9) + 1.0, 0.0, n);
            const auto span = last > first ? ch.values.subspan(static_cast<std::size_t>(first), static_cast<std::size_t>(last - first))
                                           : std::span<const double>();
            const qacq::analysis::SpanStatistics st = qacq::analysis::spanStatistics(span);
            // Digits enough to tell apart the values between A and B (a frequency of 440 Hz ± 4 mHz reads
            // "440,0021 Hz", not "440,0 Hz" for all of them); Δ down to the step of the last digit of A and B.
            const double spread = st.samples > 0 ? st.peakToPeak : 0.0;
            const double scale = std::max(std::abs(va), std::abs(vb));
            const int vd = std::max(qacq::plot::significantFor(scale, std::max(scale, 1e-300) * 1e-4, 4, 8),
                                    qacq::plot::significantForSpread(scale, spread));
            const int dd = qacq::plot::significantFor(vb - va, scale * std::pow(10.0, 1.0 - vd), 3, 9);
            QString stats;
            if (st.samples > 0)
            {
                const int sd = qacq::plot::significantForSpread(std::max(std::abs(st.minimum), std::abs(st.maximum)), spread);
                stats = tr("min %1 · max %2 · moy %3 · eff %4 · c-c %5")
                            .arg(text(qacq::plot::formatQuantity(st.minimum, unit, sd)),
                                 text(qacq::plot::formatQuantity(st.maximum, unit, sd)),
                                 text(qacq::plot::formatQuantity(st.mean, unit, sd)),
                                 text(qacq::plot::formatQuantity(st.rms, unit, sd)),
                                 text(qacq::plot::formatQuantity(st.peakToPeak, unit, 4)));
                // Area between A and B (trapezoids), in unit·s.
                if (m_rate > 0.0 && span.size() >= 2)
                    stats += tr(" · ∫ %1").arg(text(qacq::plot::formatQuantity(
                        qacq::analysis::integral(span, m_rate),
                        qacq::analysis::operationUnit(qacq::analysis::Operation::Integral, unit), 4)));
                stats += tr(" (%1 éch.)").arg(qacq::app::formatCount(static_cast<qint64>(st.samples)));
            }
            rows << QVariantMap{{QStringLiteral("name"), ch.name},
                                {QStringLiteral("channel"), c},
                                {QStringLiteral("a"), text(qacq::plot::formatQuantity(va, unit, vd))},
                                {QStringLiteral("b"), text(qacq::plot::formatQuantity(vb, unit, vd))},
                                {QStringLiteral("delta"), text(qacq::plot::formatQuantity(vb - va, unit, dd))},
                                {QStringLiteral("stats"), stats}};
        }
        info[QStringLiteral("rows")] = rows;
    }
    m_cursorInfo = info;
    emit cursorsChanged();
}

void SignalPlot::drawCursors(QPainter &p, const std::vector<Lane> &lanes) const
{
    if (!m_cursorsVisible || !cursorsAvailable() || lanes.empty()) return;
    const double top = lanes.front().rect.top(), bottom = lanes.back().rect.bottom();
    const QRectF area = plotRect();
    const double xa = xOf(m_cursorA), xb = xOf(m_cursorB);
    p.save();
    p.setClipRect(QRectF(area.left(), area.top() - 20.0, area.width(), area.height() + 20.0));
    p.setRenderHint(QPainter::Antialiasing, false);
    const double x0 = std::clamp(std::min(xa, xb), area.left(), area.right());
    const double x1 = std::clamp(std::max(xa, xb), area.left(), area.right());
    if (x1 > x0) p.fillRect(QRectF(x0, top, x1 - x0, bottom - top), faded(m_accent, 0.07));
    const QColor colours[2] = {m_accent, QColor(229, 115, 115)};
    const double xs[2] = {xa, xb};
    const QString names[2] = {QStringLiteral("A"), QStringLiteral("B")};
    const QFontMetricsF metrics(p.font());
    for (int k = 0; k < 2; ++k)
    {
        if (xs[k] < area.left() - 1.0 || xs[k] > area.right() + 1.0) continue;
        const double x = std::round(xs[k]) + 0.5;
        // A dark edge under the dashes: the line stays visible on a dense trace of its own colour.
        p.setPen(QPen(faded(m_background, 0.85), 3.0));
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
        p.setPen(QPen(colours[k], 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
        const double w = metrics.horizontalAdvance(names[k]) + 8.0;
        // Above the first band, within the item (one band per channel leaves only 8 pixels above it).
        const QRectF tag(x - w / 2.0, std::max(0.0, top - metrics.height() - 2.0), w, metrics.height() + 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(colours[k]);
        p.drawRoundedRect(tag, 3.0, 3.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_background);
        p.drawText(tag, Qt::AlignCenter, names[k]);
    }
    p.restore();
}

void SignalPlot::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    update();
}

void SignalPlot::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    if (m_channels.empty() || (delta.x() == 0 && delta.y() == 0))
    {
        event->ignore();
        return;
    }
    // Sideways (touchpad swipe, tilting wheel): slide in time.
    if (std::abs(delta.x()) > std::abs(delta.y()))
    {
        event->accept();
        if (m_live && !m_showingCapture) return;
        rememberView(true);
        scrollBy(-0.1 * delta.x() / 120.0);
        return;
    }
    const double steps = delta.y() / 120.0;
    event->accept();
    const double factor = std::pow(0.8, steps); // away from the user: zoom in
    const QPointF at = event->position();
    int laneIndex = -1;
    const Place place = placeAt(at, &laneIndex);
    const bool onScale = place == Place::LeftScale || place == Place::RightScale;
    if ((event->modifiers() & Qt::ControlModifier) || onScale)
    {
        if (laneIndex < 0) return;
        const Lane &lane = m_lastLanes[static_cast<std::size_t>(laneIndex)];
        // Two scales: the one under the pointer, or with Ctrl on the plot, the right half of the lane for
        // the one on the right.
        const bool right = !lane.rightChannels.empty() &&
                           (onScale ? place == Place::RightScale : at.x() > lane.rect.center().x());
        const Range current = scaleOf(lane, right);
        const qacq::plot::YMap map{current.low, current.high, lane.rect.top(), lane.rect.height(), false};
        setLaneScale(lane, right, qacq::plot::zoomRange(current, factor, map.toValue(at.y())));
        return;
    }
    if (m_live && !m_showingCapture)
    {
        setRollSeconds(m_rollSeconds * factor);
        return;
    }
    rememberView(true);
    if (event->modifiers() & Qt::ShiftModifier)
    {
        scrollBy(-0.1 * steps);
        return;
    }
    setViewRange(qacq::plot::zoomRange(m_view, factor, timeAt(at.x())));
}

void SignalPlot::keyPressEvent(QKeyEvent *event)
{
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    // ↑ ↓: the values (also while the acquisition rolls); up shows higher values.
    if (!m_channels.empty() && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down))
    {
        slideValues((event->key() == Qt::Key_Up ? 1.0 : -1.0) * (shift ? 0.5 : 0.1));
        event->accept();
        return;
    }
    if (m_channels.empty() || (m_live && !m_showingCapture))
    {
        event->ignore();
        return;
    }
    switch (event->key())
    {
    case Qt::Key_Left: rememberView(true); scrollBy(shift ? -0.5 : -0.1); break;
    case Qt::Key_Right: rememberView(true); scrollBy(shift ? 0.5 : 0.1); break;
    case Qt::Key_Home: rememberView(); setViewRange({m_limits.low, m_limits.low + m_view.span()}); break;
    case Qt::Key_End: rememberView(); setViewRange({m_limits.high - m_view.span(), m_limits.high}); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomTime(0.8); break;
    case Qt::Key_Minus: zoomTime(1.25); break;
    default: event->ignore(); return;
    }
    event->accept();
}

double SignalPlot::viewStart() const
{
    if (!zoomed() || !(m_limits.span() > 0.0)) return 0.0;
    return std::clamp((m_view.low - m_limits.low) / m_limits.span(), 0.0, 1.0);
}

double SignalPlot::viewSize() const
{
    if (!zoomed() || !(m_limits.span() > 0.0)) return 1.0;
    return std::clamp(m_view.span() / m_limits.span(), 0.0, 1.0);
}

void SignalPlot::scrollTo(double start)
{
    if (!std::isfinite(start) || !m_limits.valid()) return;
    rememberView(true);
    const double low = m_limits.low + start * m_limits.span();
    setViewRange({low, low + m_view.span()});
}

void SignalPlot::scrollBy(double viewFraction)
{
    if (!std::isfinite(viewFraction)) return;
    rememberView(true);
    const double shift = viewFraction * m_view.span();
    setViewRange({m_view.low + shift, m_view.high + shift});
}

qint64 SignalPlot::cursorFirstFrame() const
{
    if (!m_signal || m_live || !(m_rate > 0.0) || !m_cursorsVisible) return 0;
    const auto frames = static_cast<double>(m_signal->frameCount());
    const double lo = std::min(m_cursorA, m_cursorB);
    return static_cast<qint64>(std::clamp(std::ceil((lo - m_timeOrigin) * m_rate - 1e-9), 0.0, frames));
}

qint64 SignalPlot::cursorFrameCount() const
{
    if (!m_signal || m_live || !(m_rate > 0.0) || !m_cursorsVisible) return -1;
    const auto frames = static_cast<double>(m_signal->frameCount());
    const double hi = std::max(m_cursorA, m_cursorB);
    const double end = std::clamp(std::floor((hi - m_timeOrigin) * m_rate + 1e-9) + 1.0, 0.0, frames);
    return std::max<qint64>(0, static_cast<qint64>(end) - cursorFirstFrame());
}

void SignalPlot::mousePressEvent(QMouseEvent *event)
{
    // A click on the plot leaves the field being edited (Space starts and stops again).
    forceActiveFocus(Qt::MouseFocusReason);
    m_pressAt = event->position();
    m_pointer = m_pressAt;
    m_pressView = m_view;
    event->accept();
    m_rightPress = event->button() == Qt::RightButton;
    const bool rolling = m_live && !m_showingCapture;
    int laneIndex = -1;
    const Place place = placeAt(m_pressAt, &laneIndex);
    const bool onScale = place == Place::LeftScale || place == Place::RightScale;
    if (m_cursorsVisible && cursorsAvailable() && event->button() == Qt::LeftButton && !onScale)
    {
        // Ctrl + click places A, Shift + click B; a press on a cursor drags it.
        const Range limits = m_limits.valid() ? m_limits : m_view;
        const double t = std::clamp(timeAt(m_pressAt.x()), limits.low, limits.high);
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
        {
            (event->modifiers() & Qt::ControlModifier ? m_cursorA : m_cursorB) = t;
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
    if (event->button() == Qt::RightButton && !rolling && (place == Place::Plot || place == Place::TimeAxis))
        m_selecting = true;
    else if (event->button() == Qt::LeftButton && place != Place::None && !(rolling && place == Place::TimeAxis))
    {
        // The plot: the time, the values or both, as the drag goes (qacq::plot::dragAxes); a scale: its
        // values; the time axis: the time. While the acquisition rolls, only the values move.
        m_grab = place;
        m_grabLane = laneIndex;
        m_dragAxes = place == Place::Plot       ? qacq::plot::DragAxes::Undecided
                     : place == Place::TimeAxis ? qacq::plot::DragAxes::Horizontal
                                                : qacq::plot::DragAxes::Vertical;
        if (laneIndex >= 0)
        {
            const Lane &lane = m_lastLanes[static_cast<std::size_t>(laneIndex)];
            m_pressRange = scaleOf(lane, false);
            m_pressRightRange = scaleOf(lane, true);
        }
        // A step of the history, taken back at the release if the time did not move.
        m_pressRemembered = !rolling && m_view.valid() &&
                            (m_history.empty() || m_history.back().low != m_view.low || m_history.back().high != m_view.high);
        if (!rolling) rememberView();
        m_panning = true;
    }
    update();
}

void SignalPlot::mouseMoveEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_dragging != 0)
    {
        const Range limits = m_limits.valid() ? m_limits : m_view;
        (m_dragging == 1 ? m_cursorA : m_cursorB) = std::clamp(timeAt(m_pointer.x()), limits.low, limits.high);
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
        const bool rolling = m_live && !m_showingCapture;
        const QRectF area = plotRect();
        if (!rolling && (m_dragAxes == DragAxes::Horizontal || m_dragAxes == DragAxes::Both) && area.width() > 0.0)
        {
            const double dt = -dx / area.width() * m_pressView.span();
            setViewRange({m_pressView.low + dt, m_pressView.high + dt});
        }
        if (m_dragAxes == DragAxes::Vertical || m_dragAxes == DragAxes::Both) dragValues(dy);
    }
    update();
}

void SignalPlot::mouseReleaseEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_rightPress && std::abs(m_pointer.x() - m_pressAt.x()) <= 4.0 && std::abs(m_pointer.y() - m_pressAt.y()) <= 4.0)
    {
        // On a scale or on the time axis: the menu of that scale; elsewhere the plot's menu.
        const QString key = scaleKeyAt(m_pointer);
        const QVariantMap scale = key.isEmpty() ? QVariantMap() : scaleInfo(key);
        if (!scale.isEmpty()) emit scaleMenuRequested(m_pointer.x(), m_pointer.y(), scale);
        else emit contextMenuRequested(m_pointer.x(), m_pointer.y());
    }
    m_rightPress = false;
    if (m_selecting && std::abs(m_pointer.x() - m_pressAt.x()) > 4.0)
    {
        const double t0 = timeAt(std::min(m_pressAt.x(), m_pointer.x()));
        const double t1 = timeAt(std::max(m_pressAt.x(), m_pointer.x()));
        rememberView();
        setViewRange({t0, t1});
    }
    // A click, or a drag of the values only, leaves no step to go back to.
    if (m_panning && m_pressRemembered && !m_history.empty() && m_view.low == m_pressView.low &&
        m_view.high == m_pressView.high && m_history.back().low == m_view.low && m_history.back().high == m_view.high)
    {
        m_history.pop_back();
        emit viewChanged();
    }
    m_selecting = false;
    m_panning = false;
    m_pressRemembered = false;
    m_grab = Place::None;
    m_grabLane = -1;
    m_dragAxes = qacq::plot::DragAxes::Undecided;
    m_dragging = 0;
    updateCursorShape();
    update();
}

void SignalPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    // On a scale: that one automatic again; elsewhere everything on screen.
    int laneIndex = -1;
    const Place place = placeAt(event->position(), &laneIndex);
    if ((place == Place::LeftScale || place == Place::RightScale) && laneIndex >= 0)
    {
        autoScaleLane(m_lastLanes[static_cast<std::size_t>(laneIndex)], place == Place::RightScale);
        return;
    }
    showAll();
}

void SignalPlot::hoverMoveEvent(QHoverEvent *event)
{
    m_pointer = event->position();
    m_pointerInside = true;
    updateCursorShape();
    update();
}

void SignalPlot::hoverLeaveEvent(QHoverEvent *event)
{
    Q_UNUSED(event);
    m_pointerInside = false;
    update();
}
