#include "spectrogram_plot.h"

#include "qacq/analysis/analysis.h"
#include "qacq/app/curves_controller.h"
#include "qacq/app/format.h"
#include "qacq/plot/axis.h"

#include <QCursor>
#include <QDateTime>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>

using qacq::plot::Range;
namespace analysis = qacq::analysis;

namespace
{
// The same margins as the time plots: the time axes line up (the right one holds the scale of colours).
constexpr double kLeft = 62.0;
constexpr double kRight = 62.0;
constexpr double kTop = 18.0; // the tags of cursors A and B above the lanes
constexpr double kBottom = 40.0;
constexpr double kLaneGap = 12.0;
constexpr double kDefaultDynamic = 80.0;

QString text(const std::string &s)
{
    return QString::fromStdString(s);
}

QColor faded(QColor colour, double alpha)
{
    colour.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * static_cast<double>(colour.alphaF())));
    return colour;
}

// Colours of the levels, from the lowest (dark) to the highest (pale yellow), even steps for the eye
// (after the "inferno" scale).
const std::array<QRgb, 256> &palette()
{
    static const std::array<QRgb, 256> colours = [] {
        struct Stop
        {
            double at;
            int r, g, b;
        };
        const Stop stops[] = {{0.00, 0, 0, 4},      {0.13, 31, 12, 72},   {0.25, 85, 15, 109},
                              {0.38, 136, 34, 106}, {0.50, 186, 54, 85},  {0.63, 227, 89, 51},
                              {0.75, 249, 140, 10}, {0.88, 249, 201, 50}, {1.00, 252, 255, 164}};
        std::array<QRgb, 256> out{};
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const double v = static_cast<double>(i) / 255.0;
            std::size_t k = 0;
            while (k + 2 < std::size(stops) && v > stops[k + 1].at) ++k;
            const Stop &a = stops[k], &b = stops[k + 1];
            const double u = std::clamp((v - a.at) / (b.at - a.at), 0.0, 1.0);
            const auto mix = [u](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * u)); };
            out[i] = qRgb(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
        }
        return out;
    }();
    return colours;
}

// Half the time a frame covers around its centre: up to the next one, the first and the last to the ends
// of their samples (no empty band at the start and the end of a short record).
double reach(const qacq::dsp::Spectrogram &g, std::size_t frame)
{
    const double step = g.frameStepSeconds > 0.0 ? g.frameStepSeconds : g.frameDurationSeconds;
    const bool edge = frame == 0 || frame + 1 >= g.timeSeconds.size();
    return 0.5 * (edge ? std::max(step, g.frameDurationSeconds) : step) + 1e-12;
}

QRgb colourOf(double level, double low, double high)
{
    if (!std::isfinite(level)) return palette().front();
    const double u = std::clamp((level - low) / std::max(high - low, 1e-9), 0.0, 1.0);
    return palette()[static_cast<std::size_t>(std::lround(u * 255.0))];
}
} // namespace

SpectrogramPlot::SpectrogramPlot(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::CrossCursor));
    for (const QColor &c : {QColor(90, 170, 255), QColor(255, 167, 38), QColor(76, 195, 138), QColor(229, 115, 115)})
        m_channelColors << QVariant::fromValue(c);
    connect(this, &SpectrogramPlot::styleChanged, this, [this] { update(); });
}

QObject *SpectrogramPlot::curves() const
{
    return m_curves.data();
}

void SpectrogramPlot::setCurves(QObject *object)
{
    auto *controller = qobject_cast<qacq::app::CurvesController *>(object);
    if (controller == m_curves.data()) return;
    for (const QMetaObject::Connection &c : m_connections) disconnect(c);
    m_connections.clear();
    m_curves = controller;
    if (controller)
        m_connections << connect(controller, &qacq::app::CurvesController::spectrogramsChanged, this, &SpectrogramPlot::refresh);
    refresh();
    emit sourceChanged();
}

void SpectrogramPlot::refresh()
{
    const qacq::app::CurvesController *controller = m_curves.data(); // one read of the guarded pointer
    std::shared_ptr<const analysis::SpectrogramSet> set = controller ? controller->spectrograms() : nullptr;
    const qacq::model::SignalPtr signal = set ? set->signal : nullptr;
    const bool sameSignal = m_set && m_set->signal && signal && m_set->signal->id == signal->id;
    m_set = std::move(set);
    m_pictures.clear();
    m_tops.clear();
    if (m_set)
    {
        for (const analysis::SpectrogramCurve &c : m_set->curves)
        {
            double top = -std::numeric_limits<double>::infinity();
            if (c.result)
            {
                for (double v : c.result->amplitudeDb)
                {
                    if (std::isfinite(v)) top = std::max(top, v);
                }
            }
            m_tops.push_back(std::isfinite(top) ? top : 0.0);
        }
    }
    const Range limits = m_limits, view = m_view;
    const bool hadHistory = !m_history.empty();
    m_rate = signal && signal->sampleRateHz > 0.0 ? signal->sampleRateHz : 0.0;
    if (m_rate > 0.0)
        m_limits = {signal->startSeconds, signal->startSeconds + static_cast<double>(signal->frameCount()) / m_rate};
    if (!sameSignal || !m_viewSet)
    {
        // The span of the plots above is given right after (setTimeView): everything until then.
        m_view = m_limits;
        m_viewSet = m_rate > 0.0;
        m_history.clear();
    }
    else
    {
        const Range r = qacq::plot::clampRange(m_view, m_limits, 4.0 / std::max(m_rate, 1.0));
        m_view = r.valid() ? r : m_limits;
    }
    // Colours set by hand: for the curves still there, while it is the same signal.
    if (!sameSignal) m_manualLevels.clear();
    for (auto it = m_manualLevels.begin(); it != m_manualLevels.end();)
    {
        const bool present = m_set && std::any_of(m_set->curves.begin(), m_set->curves.end(),
                                                  [id = it->first](const analysis::SpectrogramCurve &c) { return c.id == id; });
        it = present ? std::next(it) : m_manualLevels.erase(it);
    }
    // The frequencies zoomed by hand stay while it is the same signal (and they still exist).
    const Range frequencies = qacq::plot::clampRange(m_frequencies, {0.0, nyquist()}, std::min(2.0 * binWidth(), nyquist()));
    if (m_frequenciesAutomatic || !sameSignal || !frequencies.valid())
    {
        m_frequencies = {0.0, nyquist()};
        m_frequenciesAutomatic = true;
    }
    else
    {
        m_frequencies = frequencies;
    }
    // How they are computed (the same settings for all): from the first one computed.
    m_summary.clear();
    if (m_set && m_rate > 0.0)
    {
        for (const analysis::SpectrogramCurve &c : m_set->curves)
        {
            if (!c.result || c.result->empty() || c.result->segmentLength == 0) continue;
            const qacq::dsp::Spectrogram &g = *c.result;
            const auto n = static_cast<double>(g.segmentLength);
            const double overlap = 100.0 * (1.0 - static_cast<double>(g.hop) / n);
            m_summary = tr("Segments de %1 éch. (Δf = %2, %3) · recouvrement %4 %")
                            .arg(qacq::app::formatCount(static_cast<qint64>(g.segmentLength)),
                                 qacq::app::formatQuantity(m_rate / n, QStringLiteral("Hz")),
                                 qacq::app::formatQuantity(n / m_rate, QStringLiteral("s")),
                                 text(qacq::plot::formatCompact(overlap, "", 3)));
            if (g.segmentsPerFrame > 1)
                m_summary += tr(" · %1 segments moyennés par colonne").arg(qacq::app::formatCount(static_cast<qint64>(g.segmentsPerFrame)));
            break;
        }
    }
    // The interface hears of it when what is shown changes (a new result included).
    QString signature = QString::number(signal ? static_cast<qlonglong>(signal->id) : 0);
    if (m_set)
    {
        for (const analysis::SpectrogramCurve &c : m_set->curves)
            signature += QStringLiteral("|%1:%2:%3")
                             .arg(static_cast<qulonglong>(c.id))
                             .arg(c.visible ? 1 : 0)
                             .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(c.result.get())));
    }
    signature += QStringLiteral("|") + m_summary;
    if (signature != m_signature)
    {
        m_signature = signature;
        emit contentChanged();
    }
    if (m_limits.low != limits.low || m_limits.high != limits.high || m_view.low != view.low || m_view.high != view.high ||
        hadHistory != !m_history.empty())
        emit viewChanged();
    noteScales();
    update();
}

bool SpectrogramPlot::hasData() const
{
    if (!m_set) return false;
    return std::any_of(m_set->curves.begin(), m_set->curves.end(),
                       [](const analysis::SpectrogramCurve &c) { return c.visible && c.result && !c.result->empty(); });
}

bool SpectrogramPlot::zoomed() const
{
    return m_limits.valid() && m_view.span() < m_limits.span() * 0.999;
}

double SpectrogramPlot::viewStart() const
{
    if (!zoomed() || !(m_limits.span() > 0.0)) return 0.0;
    return std::clamp((m_view.low - m_limits.low) / m_limits.span(), 0.0, 1.0);
}

double SpectrogramPlot::viewSize() const
{
    if (!zoomed() || !(m_limits.span() > 0.0)) return 1.0;
    return std::clamp(m_view.span() / m_limits.span(), 0.0, 1.0);
}

double SpectrogramPlot::nyquist() const
{
    return m_rate > 0.0 ? 0.5 * m_rate : 1.0;
}

double SpectrogramPlot::binWidth() const
{
    if (m_set)
    {
        for (const analysis::SpectrogramCurve &c : m_set->curves)
        {
            if (c.result && c.result->frequencyHz.size() > 1) return c.result->frequencyHz[1] - c.result->frequencyHz[0];
        }
    }
    return nyquist() / 512.0;
}

double SpectrogramPlot::topLevel(std::size_t curve) const
{
    return curve < m_tops.size() ? m_tops[curve] : 0.0;
}

QColor SpectrogramPlot::channelColour(int index) const
{
    if (m_channelColors.isEmpty()) return m_accent;
    const QVariant v = m_channelColors.at(index % m_channelColors.size());
    QColor c = v.value<QColor>();
    if (!c.isValid()) c = QColor(v.toString());
    return c.isValid() ? c : m_accent;
}

QRectF SpectrogramPlot::plotRect() const
{
    return QRectF(kLeft, kTop, std::max(0.0, width() - kLeft - kRight), std::max(0.0, height() - kTop - kBottom));
}

std::vector<SpectrogramPlot::Lane> SpectrogramPlot::lanes() const
{
    std::vector<Lane> out;
    if (!m_set) return out;
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < m_set->curves.size(); ++i)
    {
        if (m_set->curves[i].visible) shown.push_back(i);
    }
    if (shown.empty()) return out;
    const QRectF area = plotRect();
    const auto n = static_cast<double>(shown.size());
    const double h = std::max(10.0, (area.height() - kLaneGap * (n - 1.0)) / n);
    for (std::size_t k = 0; k < shown.size(); ++k)
        out.push_back({QRectF(area.left(), area.top() + static_cast<double>(k) * (h + kLaneGap), area.width(), h), shown[k]});
    return out;
}

double SpectrogramPlot::timeAt(double x) const
{
    const QRectF area = plotRect();
    if (!(area.width() > 0.0)) return m_view.low;
    return m_view.low + (x - area.left()) / area.width() * m_view.span();
}

double SpectrogramPlot::xOf(double time) const
{
    const QRectF area = plotRect();
    return area.left() + (time - m_view.low) / m_view.span() * area.width();
}

double SpectrogramPlot::frequencyAt(const QRectF &lane, double y) const
{
    if (!(lane.height() > 0.0)) return m_frequencies.low;
    return m_frequencies.high - (y - lane.top()) / lane.height() * m_frequencies.span();
}

double SpectrogramPlot::yOf(const QRectF &lane, double frequency) const
{
    return lane.top() + (m_frequencies.high - frequency) / m_frequencies.span() * lane.height();
}

SpectrogramPlot::Place SpectrogramPlot::placeAt(const QPointF &at, int *lane) const
{
    *lane = -1;
    const std::vector<Lane> all = lanes();
    if (all.empty()) return Place::None;
    const QRectF area = plotRect();
    const bool overArea = at.x() >= area.left() && at.x() <= area.right();
    if (at.y() > all.back().rect.bottom() + kLaneGap / 2) return overArea ? Place::TimeAxis : Place::None;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const QRectF &r = all[i].rect;
        if (at.y() < r.top() - kLaneGap / 2 || at.y() > r.bottom() + kLaneGap / 2) continue;
        *lane = static_cast<int>(i);
        if (at.x() < area.left()) return Place::FrequencyScale;
        if (at.x() > area.right()) return Place::ColourScale;
        return Place::Plot;
    }
    return overArea ? Place::Plot : Place::None;
}

const QImage &SpectrogramPlot::picture(const Lane &lane)
{
    const analysis::SpectrogramCurve &curve = m_set->curves[lane.curve];
    const int w = std::max(1, static_cast<int>(std::floor(lane.rect.width())));
    const int h = std::max(1, static_cast<int>(std::floor(lane.rect.height())));
    const QString key = QStringLiteral("%1x%2|%3|%4|%5|%6|%7|%8|%9")
                            .arg(w)
                            .arg(h)
                            .arg(m_view.low, 0, 'g', 17)
                            .arg(m_view.high, 0, 'g', 17)
                            .arg(m_frequencies.low, 0, 'g', 17)
                            .arg(m_frequencies.high, 0, 'g', 17)
                            .arg(levelsOf(lane.curve).low, 0, 'g', 17)
                            .arg(reinterpret_cast<quintptr>(curve.result.get()))
                            .arg(levelsOf(lane.curve).high, 0, 'g', 17);
    Picture &cached = m_pictures[curve.id];
    if (cached.key == key && !cached.image.isNull()) return cached.image;
    cached.key = key;
    cached.image = QImage(w, h, QImage::Format_RGB32);
    cached.image.fill(m_background);
    if (!curve.result || curve.result->empty()) return cached.image;
    const qacq::dsp::Spectrogram &g = *curve.result;

    // The frames under each column: those whose centre falls in it, or else the one that covers its middle.
    const std::vector<double> &times = g.timeSeconds;
    std::vector<int> frameFrom(static_cast<std::size_t>(w), -1), frameTo(static_cast<std::size_t>(w), -1);
    for (int x = 0; x < w; ++x)
    {
        const double t0 = timeAt(lane.rect.left() + x) - curve.startSeconds;
        const double t1 = timeAt(lane.rect.left() + x + 1) - curve.startSeconds;
        const auto from = std::lower_bound(times.begin(), times.end(), t0);
        const auto to = std::lower_bound(times.begin(), times.end(), t1);
        const auto ux = static_cast<std::size_t>(x);
        if (from < to)
        {
            frameFrom[ux] = static_cast<int>(from - times.begin());
            frameTo[ux] = static_cast<int>(to - times.begin()) - 1;
            continue;
        }
        const double middle = 0.5 * (t0 + t1);
        std::ptrdiff_t i = from - times.begin();
        if (i >= static_cast<std::ptrdiff_t>(times.size()) ||
            (i > 0 && middle - times[static_cast<std::size_t>(i - 1)] < times[static_cast<std::size_t>(i)] - middle))
            --i;
        if (i < 0) continue;
        if (std::abs(times[static_cast<std::size_t>(i)] - middle) <= reach(g, static_cast<std::size_t>(i)))
            frameFrom[ux] = frameTo[ux] = static_cast<int>(i);
    }
    // The bins under each row (the top row has the highest frequencies).
    const double df = g.frequencyHz.size() > 1 ? g.frequencyHz[1] - g.frequencyHz[0] : 1.0;
    const auto bins = static_cast<double>(g.bins);
    std::vector<int> binFrom(static_cast<std::size_t>(h), -1), binTo(static_cast<std::size_t>(h), -1);
    for (int y = 0; y < h; ++y)
    {
        const double f1 = frequencyAt(lane.rect, lane.rect.top() + y);
        const double f0 = frequencyAt(lane.rect, lane.rect.top() + y + 1);
        const double k0 = std::ceil(f0 / df - 1e-9), k1 = std::ceil(f1 / df - 1e-9) - 1.0;
        const auto uy = static_cast<std::size_t>(y);
        if (k0 <= k1)
        {
            const double a = std::clamp(k0, 0.0, bins - 1.0), b = std::clamp(k1, 0.0, bins - 1.0);
            if (k1 < 0.0 || k0 > bins - 1.0) continue;
            binFrom[uy] = static_cast<int>(a);
            binTo[uy] = static_cast<int>(b);
            continue;
        }
        const double k = std::round(0.5 * (f0 + f1) / df);
        if (k < 0.0 || k > bins - 1.0) continue;
        binFrom[uy] = binTo[uy] = static_cast<int>(k);
    }
    const qacq::plot::Range levels = levelsOf(lane.curve);
    const double high = levels.high, low = levels.low;
    for (int y = 0; y < h; ++y)
    {
        const auto uy = static_cast<std::size_t>(y);
        if (binFrom[uy] < 0) continue;
        auto *line = reinterpret_cast<QRgb *>(cached.image.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const auto ux = static_cast<std::size_t>(x);
            if (frameFrom[ux] < 0) continue;
            double best = -std::numeric_limits<double>::infinity();
            for (int f = frameFrom[ux]; f <= frameTo[ux]; ++f)
            {
                for (int k = binFrom[uy]; k <= binTo[uy]; ++k)
                    best = std::max(best, g.at(static_cast<std::size_t>(f), static_cast<std::size_t>(k)));
            }
            line[x] = colourOf(best, low, high);
        }
    }
    return cached.image;
}

void SpectrogramPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(boundingRect(), m_background);
    QFont font = p.font();
    font.setPixelSize(std::max(8, m_fontSize));
    p.setFont(font);
    const QRectF area = plotRect();
    if (area.width() < 40.0 || area.height() < 30.0) return;
    const std::vector<Lane> all = lanes();
    if (all.empty() || !(m_rate > 0.0))
    {
        p.setPen(m_muted);
        p.drawText(boundingRect().adjusted(24.0, 0.0, -24.0, 0.0), Qt::AlignCenter | Qt::TextWordWrap,
                   m_set && !m_set->curves.empty() ? tr("Tous les spectrogrammes sont masqués") : m_emptyText);
        return;
    }
    if (!m_view.valid()) m_view = m_limits;
    for (const Lane &lane : all) drawLane(p, lane);
    drawTimeAxis(p, all);
    if (m_selecting)
    {
        const double x0 = std::clamp(std::min(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        const double x1 = std::clamp(std::max(m_pressAt.x(), m_pointer.x()), area.left(), area.right());
        p.setPen(QPen(faded(m_accent, 0.8), 1.0));
        p.setBrush(faded(m_accent, 0.15));
        p.drawRect(QRectF(x0, all.front().rect.top(), x1 - x0, all.back().rect.bottom() - all.front().rect.top()));
        p.setBrush(Qt::NoBrush);
    }
    drawCursors(p, all);
    if (m_pointerInside && !m_panning && !m_selecting && m_dragging == 0) drawReadout(p, all);
}

void SpectrogramPlot::drawLane(QPainter &p, const Lane &lane)
{
    const QRectF &r = lane.rect;
    const analysis::SpectrogramCurve &curve = m_set->curves[lane.curve];
    const QFontMetricsF metrics(p.font());
    p.setRenderHint(QPainter::Antialiasing, false);
    if (curve.result && !curve.result->empty()) p.drawImage(r.topLeft(), picture(lane));
    else
    {
        p.setPen(curve.result ? m_muted : m_text);
        p.drawText(r, Qt::AlignCenter,
                   curve.result ? tr("Pas de spectrogramme pour cette voie")
                                : m_busyText.isEmpty() ? tr("Calcul du spectrogramme…") : m_busyText);
    }
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));

    // Frequencies, on the left.
    const qacq::plot::Ticks ticks = qacq::plot::linearTicks(m_frequencies.low, m_frequencies.high, r.height(), 28.0);
    const qacq::plot::AxisLabels labels = qacq::plot::axisLabels(ticks, "Hz");
    const QString frequencyUnit = labels.unit.empty() ? QString() : QStringLiteral(" ") + text(labels.unit);
    for (std::size_t i = 0; i < ticks.major.size() && i < labels.text.size(); ++i)
    {
        const double y = std::round(yOf(r, ticks.major[i])) + 0.5;
        if (y < r.top() - 0.5 || y > r.bottom() + 0.5) continue;
        p.setPen(QPen(m_grid, 1.0));
        p.drawLine(QPointF(r.left() - 4.0, y), QPointF(r.left(), y));
        p.setPen(m_muted);
        // Kept within the lane (a label at the very top or bottom is not cut by the next lane).
        const double ty = std::max(r.top() - 2.0, std::min(y - metrics.height() / 2.0, r.bottom() - metrics.height() + 2.0));
        p.drawText(QRectF(0.0, ty, r.left() - 6.0, metrics.height()), Qt::AlignRight | Qt::AlignVCenter,
                   text(labels.text[i]) + frequencyUnit);
    }

    // Scale of the colours, on the right: the highest level of the spectrogram, and down the range shown.
    const std::string levelUnit = analysis::levelUnit(curve.unit, analysis::SpectrumKind::Amplitude, analysis::LevelScale::Decibel);
    const qacq::plot::Range levels = levelsOf(lane.curve);
    const double high = levels.high, low = levels.low;
    const QRectF bar(r.right() + 6.0, r.top(), 9.0, r.height());
    for (int y = 0; y < static_cast<int>(bar.height()); ++y)
    {
        const double u = 1.0 - static_cast<double>(y) / std::max(1.0, bar.height() - 1.0);
        p.setPen(QColor(palette()[static_cast<std::size_t>(std::lround(u * 255.0))]));
        p.drawLine(QPointF(bar.left(), bar.top() + y + 0.5), QPointF(bar.right(), bar.top() + y + 0.5));
    }
    p.setPen(QPen(m_grid, 1.0));
    p.drawRect(bar.adjusted(-0.5, -0.5, 0.5, 0.5));
    const qacq::plot::Ticks levelTicks = qacq::plot::linearTicks(low, high, r.height(), 26.0);
    p.setPen(m_muted);
    for (double v : levelTicks.major)
    {
        const double y = bar.top() + (high - v) / std::max(high - low, 1e-9) * bar.height();
        if (y < bar.top() - 1.0 || y > bar.bottom() + 1.0) continue;
        p.drawText(QRectF(bar.right() + 3.0, y - metrics.height() / 2.0, width() - bar.right() - 4.0, metrics.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, text(qacq::plot::formatCompact(v, "", 3)));
    }

    // Name of the channel and the unit of its levels, top left.
    p.setRenderHint(QPainter::Antialiasing, true);
    const QString name = text(curve.name) + QStringLiteral(" (%1)").arg(text(levelUnit));
    const double w = metrics.horizontalAdvance(name) + 18.0;
    const double x = r.left() + 8.0, y = r.top() + 5.0;
    p.setPen(Qt::NoPen);
    p.setBrush(faded(m_panel, 0.85));
    p.drawRoundedRect(QRectF(x - 4.0, y - 2.0, w + 4.0, metrics.height() + 4.0), 4.0, 4.0);
    p.setBrush(channelColour(static_cast<int>(curve.channel)));
    p.drawRoundedRect(QRectF(x, y + metrics.height() / 2.0 - 2.0, 10.0, 4.0), 1.5, 1.5);
    p.setBrush(Qt::NoBrush);
    p.setPen(m_text);
    p.drawText(QPointF(x + 15.0, y + metrics.ascent()), name);
}

void SpectrogramPlot::drawTimeAxis(QPainter &p, const std::vector<Lane> &all) const
{
    const QFontMetricsF metrics(p.font());
    const QRectF area = plotRect();
    const double bottom = all.back().rect.bottom();
    const qacq::plot::Ticks ticks = qacq::plot::linearTicks(m_view.low, m_view.high, area.width(), 90.0);
    const qacq::plot::AxisLabels labels = qacq::plot::axisLabels(ticks, "s");
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(faded(m_muted, 0.6), 1.0));
    for (double t : ticks.minor)
    {
        const double x = std::round(xOf(t)) + 0.5;
        if (x >= area.left() && x <= area.right()) p.drawLine(QPointF(x, bottom), QPointF(x, bottom + 3.0));
    }
    p.setPen(QPen(m_grid, 1.0));
    for (double t : ticks.major)
    {
        const double x = std::round(xOf(t)) + 0.5;
        if (x >= area.left() && x <= area.right()) p.drawLine(QPointF(x, bottom), QPointF(x, bottom + 5.0));
    }
    p.setPen(m_muted);
    double lastRight = -1e9;
    for (std::size_t i = 0; i < ticks.major.size() && i < labels.text.size(); ++i)
    {
        const QString label = text(labels.text[i]);
        const double lw = metrics.horizontalAdvance(label);
        const double x = xOf(ticks.major[i]) - lw / 2.0;
        if (x < lastRight + 8.0 || x + lw > width() - 2.0) continue;
        p.drawText(QPointF(std::max(2.0, x), bottom + 4.0 + metrics.ascent()), label);
        lastRight = x + lw;
    }
    QString title = tr("Temps");
    if (!labels.unit.empty()) title += QStringLiteral(" (%1)").arg(text(labels.unit));
    p.drawText(QRectF(area.left(), bottom + 6.0 + metrics.height(), area.width(), metrics.height() + 2.0),
               Qt::AlignHCenter | Qt::AlignTop, title);
}

void SpectrogramPlot::drawCursors(QPainter &p, const std::vector<Lane> &all) const
{
    // As on the time plots: dashed lines, their tags above the lanes.
    if (!m_cursorsVisible) return;
    const double top = all.front().rect.top(), bottom = all.back().rect.bottom();
    const QRectF area = plotRect();
    const QColor colours[2] = {m_accent, QColor(229, 115, 115)};
    const double xs[2] = {xOf(m_cursorA), xOf(m_cursorB)};
    const QString names[2] = {QStringLiteral("A"), QStringLiteral("B")};
    const QFontMetricsF metrics(p.font());
    p.save();
    p.setClipRect(QRectF(area.left(), 0.0, area.width(), height()));
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int k = 0; k < 2; ++k)
    {
        if (xs[k] < area.left() - 1.0 || xs[k] > area.right() + 1.0) continue;
        const double x = std::round(xs[k]) + 0.5;
        // A dark edge under the dashes: visible on every colour of the levels.
        p.setPen(QPen(faded(m_background, 0.85), 3.0));
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
        p.setPen(QPen(colours[k], 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, top), QPointF(x, bottom));
        const double w = metrics.horizontalAdvance(names[k]) + 8.0;
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

void SpectrogramPlot::drawReadout(QPainter &p, const std::vector<Lane> &all) const
{
    const Lane *lane = nullptr;
    for (const Lane &l : all)
    {
        if (l.rect.contains(m_pointer)) lane = &l;
    }
    if (!lane) return;
    const analysis::SpectrogramCurve &curve = m_set->curves[lane->curve];
    if (!curve.result || curve.result->empty()) return;
    const qacq::dsp::Spectrogram &g = *curve.result;
    const double t = timeAt(m_pointer.x());
    const double f = frequencyAt(lane->rect, m_pointer.y());
    // The frame and the bin nearest to the pointer.
    const double local = t - curve.startSeconds;
    const auto next = std::lower_bound(g.timeSeconds.begin(), g.timeSeconds.end(), local);
    std::ptrdiff_t frame = next - g.timeSeconds.begin();
    if (frame >= static_cast<std::ptrdiff_t>(g.frames) ||
        (frame > 0 && local - g.timeSeconds[static_cast<std::size_t>(frame - 1)] < g.timeSeconds[static_cast<std::size_t>(frame)] - local))
        --frame;
    const double df = g.frequencyHz.size() > 1 ? g.frequencyHz[1] - g.frequencyHz[0] : 1.0;
    const double bin = std::clamp(std::round(f / df), 0.0, static_cast<double>(g.bins) - 1.0);
    const bool inside = frame >= 0 && std::abs(g.timeSeconds[static_cast<std::size_t>(frame)] - local) <=
                                          reach(g, static_cast<std::size_t>(frame));
    const std::string levelUnit = analysis::levelUnit(curve.unit, analysis::SpectrumKind::Amplitude, analysis::LevelScale::Decibel);
    QStringList lines;
    lines << QStringLiteral("t = %1").arg(text(qacq::plot::formatQuantity(t, "s", qacq::plot::significantFor(t, m_view.span() / 1000.0, 4, 12))));
    lines << QStringLiteral("f = %1").arg(text(qacq::plot::formatQuantity(f, "Hz", qacq::plot::significantFor(f, m_frequencies.span() / 1000.0, 3, 10))));
    if (inside)
    {
        const double level = g.at(static_cast<std::size_t>(frame), static_cast<std::size_t>(bin));
        lines << tr("%1 : %2 %3").arg(text(curve.name), text(qacq::plot::formatFixed(level, 1)), text(levelUnit));
    }
    else
    {
        lines << tr("%1 : hors de la partie calculée").arg(text(curve.name));
    }
    const QFontMetricsF metrics(p.font());
    double w = 0.0;
    for (const QString &l : lines) w = std::max(w, metrics.horizontalAdvance(l));
    w += 16.0;
    const double lineHeight = metrics.height() + 2.0;
    const double h = lineHeight * static_cast<double>(lines.size()) + 8.0;
    const QRectF area = plotRect();
    double bx = m_pointer.x() + 14.0, by = m_pointer.y() + 14.0;
    if (bx + w > area.right()) bx = m_pointer.x() - 14.0 - w;
    if (by + h > area.bottom()) by = std::max(area.top(), m_pointer.y() - 14.0 - h);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(m_grid, 1.0));
    p.setBrush(faded(m_panel, 0.94));
    p.drawRoundedRect(QRectF(bx, by, w, h), 6.0, 6.0);
    p.setBrush(Qt::NoBrush);
    for (qsizetype k = 0; k < lines.size(); ++k)
    {
        p.setPen(k < 2 ? m_muted : m_text);
        p.drawText(QPointF(bx + 8.0, by + 4.0 + lineHeight * static_cast<double>(k) + metrics.ascent()), lines.at(k));
    }
}

void SpectrogramPlot::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    update();
}

// ---- View ----

void SpectrogramPlot::setViewRange(const Range &range)
{
    const Range r = qacq::plot::clampRange(range, m_limits, 4.0 / std::max(m_rate, 1.0));
    if (!r.valid() || (r.low == m_view.low && r.high == m_view.high)) return;
    m_view = r;
    m_viewSet = true;
    emit viewChanged();
    update();
}

void SpectrogramPlot::setFrequencies(const Range &range)
{
    const Range r = qacq::plot::clampRange(range, {0.0, nyquist()}, std::min(2.0 * binWidth(), nyquist()));
    if (!r.valid()) return;
    m_frequencies = r;
    m_frequenciesAutomatic = r.low <= 0.0 && r.high >= nyquist();
    noteScales();
    update();
}

void SpectrogramPlot::setTimeView(double low, double high)
{
    // Nothing to follow yet: the span is taken again when the spectrograms come (contentChanged).
    if (!(m_rate > 0.0) || !std::isfinite(low) || !std::isfinite(high) || !(high > low)) return;
    if (low == m_view.low && high == m_view.high) return;
    setViewRange({low, high});
}

void SpectrogramPlot::rememberView(bool coalesce)
{
    if (!m_view.valid()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameGesture = coalesce && now - m_lastRemembered < 600;
    m_lastRemembered = coalesce ? now : 0;
    if (sameGesture) return;
    if (!m_history.empty() && m_history.back().low == m_view.low && m_history.back().high == m_view.high) return;
    m_history.push_back(m_view);
    if (m_history.size() > 50) m_history.erase(m_history.begin());
    emit viewChanged();
}

void SpectrogramPlot::goBack()
{
    if (m_history.empty()) return;
    const Range previous = m_history.back();
    m_history.pop_back();
    const Range r = qacq::plot::clampRange(previous, m_limits, 4.0 / std::max(m_rate, 1.0));
    if (r.valid()) m_view = r;
    emit viewChanged();
    update();
}

void SpectrogramPlot::showAll()
{
    rememberView();
    m_view = m_limits;
    emit viewChanged();
    autoScale();
}

void SpectrogramPlot::autoScale()
{
    const bool changed = !m_frequenciesAutomatic || m_dynamic != kDefaultDynamic || !m_manualLevels.empty();
    m_frequencies = {0.0, nyquist()};
    m_frequenciesAutomatic = true;
    m_dynamic = kDefaultDynamic;
    m_manualLevels.clear();
    m_manualScalesNoted = false;
    if (changed) emit scalesChanged();
    update();
}

void SpectrogramPlot::zoomTime(double factor)
{
    if (!(factor > 0.0)) return;
    rememberView();
    setViewRange(qacq::plot::zoomRange(m_view, factor, 0.5 * (m_view.low + m_view.high)));
}

void SpectrogramPlot::scrollTo(double start)
{
    if (!std::isfinite(start) || !m_limits.valid()) return;
    rememberView(true);
    const double low = m_limits.low + start * m_limits.span();
    setViewRange({low, low + m_view.span()});
}

void SpectrogramPlot::scrollBy(double viewFraction)
{
    if (!std::isfinite(viewFraction)) return;
    rememberView(true);
    const double shift = viewFraction * m_view.span();
    setViewRange({m_view.low + shift, m_view.high + shift});
}

bool SpectrogramPlot::manualScales() const
{
    return !m_frequenciesAutomatic || m_dynamic != kDefaultDynamic || !m_manualLevels.empty();
}

qacq::plot::Range SpectrogramPlot::levelsOf(std::size_t curve) const
{
    if (m_set && curve < m_set->curves.size())
    {
        const auto it = m_manualLevels.find(m_set->curves[curve].id);
        if (it != m_manualLevels.end()) return it->second;
    }
    const double high = topLevel(curve);
    return {high - m_dynamic, high};
}

void SpectrogramPlot::setLevelSpan(std::size_t curve, double decibels)
{
    if (!m_set || curve >= m_set->curves.size() || !std::isfinite(decibels)) return;
    const auto it = m_manualLevels.find(m_set->curves[curve].id);
    if (it == m_manualLevels.end())
    {
        setDynamicRange(decibels);
        return;
    }
    // Set by hand: its top stays, its bottom moves (as the range of the automatic ones).
    it->second.low = it->second.high - std::clamp(std::round(decibels), 10.0, 200.0);
    update();
}

int SpectrogramPlot::curveIndexOf(const QString &key) const
{
    if (!m_set || !key.startsWith(QStringLiteral("colours:"))) return -1;
    bool ok = false;
    const qulonglong id = key.mid(8).toULongLong(&ok);
    if (!ok) return -1;
    for (std::size_t i = 0; i < m_set->curves.size(); ++i)
    {
        if (m_set->curves[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

QString SpectrogramPlot::scaleKeyAt(const QPointF &at) const
{
    if (!hasData()) return {};
    int laneIndex = -1;
    const Place place = placeAt(at, &laneIndex);
    if (place == Place::TimeAxis) return QStringLiteral("time");
    if (place == Place::FrequencyScale) return QStringLiteral("frequency");
    if (place == Place::ColourScale)
    {
        const std::vector<Lane> all = lanes();
        if (laneIndex < 0 || static_cast<std::size_t>(laneIndex) >= all.size()) return {};
        return QStringLiteral("colours:%1").arg(static_cast<qulonglong>(m_set->curves[all[static_cast<std::size_t>(laneIndex)].curve].id));
    }
    return {};
}

QVariantMap SpectrogramPlot::scaleInfo(const QString &key) const
{
    QVariantMap m;
    if (!hasData()) return m;
    m.insert(QStringLiteral("key"), key);
    m.insert(QStringLiteral("lowLabel"), tr("Minimum"));
    m.insert(QStringLiteral("highLabel"), tr("Maximum"));
    if (key == QStringLiteral("time"))
    {
        m.insert(QStringLiteral("kind"), QStringLiteral("time"));
        m.insert(QStringLiteral("title"), tr("Axe du temps"));
        m.insert(QStringLiteral("unit"), QStringLiteral("s"));
        m.insert(QStringLiteral("low"), m_view.low);
        m.insert(QStringLiteral("high"), m_view.high);
        m.insert(QStringLiteral("automatic"), !zoomed());
        m.insert(QStringLiteral("lowLabel"), tr("Début"));
        m.insert(QStringLiteral("highLabel"), tr("Fin"));
        m.insert(QStringLiteral("autoText"), tr("Tout le signal"));
        return m;
    }
    if (key == QStringLiteral("frequency"))
    {
        m.insert(QStringLiteral("kind"), QStringLiteral("frequency"));
        m.insert(QStringLiteral("title"), tr("Fréquences"));
        m.insert(QStringLiteral("unit"), QStringLiteral("Hz"));
        m.insert(QStringLiteral("low"), m_frequencies.low);
        m.insert(QStringLiteral("high"), m_frequencies.high);
        m.insert(QStringLiteral("automatic"), m_frequenciesAutomatic);
        m.insert(QStringLiteral("autoText"), tr("Toutes les fréquences"));
        return m;
    }
    const int index = curveIndexOf(key);
    if (index < 0) return {};
    const analysis::SpectrogramCurve &curve = m_set->curves[static_cast<std::size_t>(index)];
    const QString unit = text(analysis::levelUnit(curve.unit, analysis::SpectrumKind::Amplitude, analysis::LevelScale::Decibel));
    const qacq::plot::Range levels = levelsOf(static_cast<std::size_t>(index));
    m.insert(QStringLiteral("kind"), QStringLiteral("colours"));
    m.insert(QStringLiteral("title"), tr("Couleurs de %1 (%2)").arg(text(curve.name), unit));
    m.insert(QStringLiteral("unit"), unit);
    m.insert(QStringLiteral("low"), levels.low);
    m.insert(QStringLiteral("high"), levels.high);
    m.insert(QStringLiteral("automatic"), m_manualLevels.count(curve.id) == 0 && m_dynamic == kDefaultDynamic);
    m.insert(QStringLiteral("autoText"), tr("Automatique"));
    return m;
}

bool SpectrogramPlot::setScaleRange(const QString &key, double low, double high)
{
    if (!hasData() || !std::isfinite(low) || !std::isfinite(high) || !(high > low)) return false;
    if (key == QStringLiteral("time"))
    {
        rememberView();
        setViewRange({low, high});
        return true;
    }
    if (key == QStringLiteral("frequency"))
    {
        setFrequencies({low, high});
        return true;
    }
    const int index = curveIndexOf(key);
    if (index < 0) return false;
    m_manualLevels[m_set->curves[static_cast<std::size_t>(index)].id] = {low, high};
    noteScales();
    update();
    return true;
}

void SpectrogramPlot::setScaleAutomatic(const QString &key)
{
    if (!hasData()) return;
    if (key == QStringLiteral("time"))
    {
        rememberView();
        setViewRange(m_limits);
        return;
    }
    if (key == QStringLiteral("frequency"))
    {
        setFrequencies({0.0, nyquist()});
        return;
    }
    const int index = curveIndexOf(key);
    if (index < 0) return;
    // From its highest level down by the usual range.
    m_manualLevels.erase(m_set->curves[static_cast<std::size_t>(index)].id);
    if (m_dynamic != kDefaultDynamic)
    {
        m_dynamic = kDefaultDynamic;
        emit scalesChanged();
    }
    noteScales();
    update();
}

void SpectrogramPlot::setDynamicRange(double decibels)
{
    if (!std::isfinite(decibels)) return;
    // (not "d": GCC of MinGW finds a member of that name in the Qt item, -Wshadow)
    const double clamped = std::clamp(std::round(decibels), 10.0, 200.0);
    if (clamped == m_dynamic) return;
    m_dynamic = clamped;
    m_manualScalesNoted = manualScales();
    emit scalesChanged();
    update();
}

void SpectrogramPlot::noteScales()
{
    const bool manual = manualScales();
    if (manual == m_manualScalesNoted) return;
    m_manualScalesNoted = manual;
    emit scalesChanged();
}

// ---- Cursors ----

void SpectrogramPlot::setCursorsVisible(bool visible)
{
    if (visible == m_cursorsVisible) return;
    m_cursorsVisible = visible;
    emit cursorsChanged();
    update();
}

void SpectrogramPlot::setCursorA(double time)
{
    if (!std::isfinite(time) || time == m_cursorA) return;
    m_cursorA = time;
    emit cursorsChanged();
    update();
}

void SpectrogramPlot::setCursorB(double time)
{
    if (!std::isfinite(time) || time == m_cursorB) return;
    m_cursorB = time;
    emit cursorsChanged();
    update();
}

int SpectrogramPlot::cursorNear(double x) const
{
    if (!m_cursorsVisible) return 0;
    const double da = std::abs(x - xOf(m_cursorA)), db = std::abs(x - xOf(m_cursorB));
    if (std::min(da, db) > 6.0) return 0;
    return da <= db ? 1 : 2;
}

// ---- Mouse and keys ----

void SpectrogramPlot::updateCursorShape()
{
    int lane = -1;
    const Place place = placeAt(m_pointer, &lane);
    Qt::CursorShape shape = Qt::CrossCursor;
    if (place == Place::FrequencyScale || place == Place::ColourScale) shape = Qt::SizeVerCursor;
    else if (cursorNear(m_pointer.x()) != 0 || place == Place::TimeAxis) shape = Qt::SizeHorCursor;
    setCursor(QCursor(shape));
}

void SpectrogramPlot::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->angleDelta();
    if (!hasData() || (delta.x() == 0 && delta.y() == 0))
    {
        event->ignore();
        return;
    }
    event->accept();
    // Sideways (touchpad swipe, tilting wheel): slide in time.
    if (std::abs(delta.x()) > std::abs(delta.y()))
    {
        scrollBy(-0.1 * delta.x() / 120.0);
        return;
    }
    const double steps = delta.y() / 120.0;
    const double factor = std::pow(0.8, steps);
    const QPointF at = event->position();
    int laneIndex = -1;
    const Place place = placeAt(at, &laneIndex);
    if (place == Place::ColourScale)
    {
        // Away from the user: fewer dB in the colours (the weak parts drop to black).
        const std::vector<Lane> all = lanes();
        if (laneIndex >= 0 && static_cast<std::size_t>(laneIndex) < all.size())
        {
            const std::size_t curve = all[static_cast<std::size_t>(laneIndex)].curve;
            setLevelSpan(curve, levelsOf(curve).span() - 10.0 * steps);
        }
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) || place == Place::FrequencyScale)
    {
        const std::vector<Lane> all = lanes();
        if (laneIndex < 0 || static_cast<std::size_t>(laneIndex) >= all.size()) return;
        setFrequencies(qacq::plot::zoomRange(m_frequencies, factor, frequencyAt(all[static_cast<std::size_t>(laneIndex)].rect, at.y())));
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier)
    {
        scrollBy(-0.1 * steps);
        return;
    }
    rememberView(true);
    setViewRange(qacq::plot::zoomRange(m_view, factor, timeAt(at.x())));
}

void SpectrogramPlot::keyPressEvent(QKeyEvent *event)
{
    if (!hasData())
    {
        event->ignore();
        return;
    }
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (event->key())
    {
    case Qt::Key_Left: scrollBy(shift ? -0.5 : -0.1); break;
    case Qt::Key_Right: scrollBy(shift ? 0.5 : 0.1); break;
    case Qt::Key_Up:
    case Qt::Key_Down:
        setFrequencies(qacq::plot::shiftRange(m_frequencies, (event->key() == Qt::Key_Up ? 1.0 : -1.0) * (shift ? 0.5 : 0.1)));
        break;
    case Qt::Key_Home: rememberView(); setViewRange({m_limits.low, m_limits.low + m_view.span()}); break;
    case Qt::Key_End: rememberView(); setViewRange({m_limits.high - m_view.span(), m_limits.high}); break;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomTime(0.8); break;
    case Qt::Key_Minus: zoomTime(1.25); break;
    default: event->ignore(); return;
    }
    event->accept();
}

void SpectrogramPlot::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    m_pressAt = event->position();
    m_pointer = m_pressAt;
    m_pressView = m_view;
    m_pressFrequencies = m_frequencies;
    m_pressDynamic = m_dynamic;
    event->accept();
    {
        int lane = -1;
        if (placeAt(m_pressAt, &lane) == Place::ColourScale)
        {
            const std::vector<Lane> all = lanes();
            if (lane >= 0 && static_cast<std::size_t>(lane) < all.size())
                m_pressDynamic = levelsOf(all[static_cast<std::size_t>(lane)].curve).span();
        }
    }
    m_rightPress = event->button() == Qt::RightButton;
    int laneIndex = -1;
    const Place place = placeAt(m_pressAt, &laneIndex);
    const bool onScale = place == Place::FrequencyScale || place == Place::ColourScale;
    if (m_cursorsVisible && hasData() && event->button() == Qt::LeftButton && !onScale)
    {
        // Ctrl + click places A, Shift + click B; a press on a cursor drags it.
        const double t = std::clamp(timeAt(m_pressAt.x()), m_limits.low, m_limits.high);
        if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
        {
            m_dragging = event->modifiers() & Qt::ControlModifier ? 1 : 2;
            (m_dragging == 1 ? m_cursorA : m_cursorB) = t;
            emit cursorsChanged();
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
    if (event->button() == Qt::RightButton && (place == Place::Plot || place == Place::TimeAxis))
    {
        m_selecting = true;
    }
    else if (event->button() == Qt::LeftButton && place != Place::None && hasData())
    {
        m_grab = place;
        m_grabLane = laneIndex;
        m_dragAxes = place == Place::Plot       ? qacq::plot::DragAxes::Undecided
                     : place == Place::TimeAxis ? qacq::plot::DragAxes::Horizontal
                                                : qacq::plot::DragAxes::Vertical;
        m_pressRemembered = m_view.valid() && (m_history.empty() || m_history.back().low != m_view.low ||
                                               m_history.back().high != m_view.high);
        rememberView();
        m_panning = true;
    }
    update();
}

void SpectrogramPlot::mouseMoveEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_dragging != 0)
    {
        (m_dragging == 1 ? m_cursorA : m_cursorB) = std::clamp(timeAt(m_pointer.x()), m_limits.low, m_limits.high);
        emit cursorsChanged();
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
        if ((m_dragAxes == DragAxes::Horizontal || m_dragAxes == DragAxes::Both) && m_grab != Place::ColourScale &&
            area.width() > 0.0)
        {
            const double dt = -dx / area.width() * m_pressView.span();
            setViewRange({m_pressView.low + dt, m_pressView.high + dt});
        }
        const std::vector<Lane> all = lanes();
        if ((m_dragAxes == DragAxes::Vertical || m_dragAxes == DragAxes::Both) && m_grabLane >= 0 &&
            static_cast<std::size_t>(m_grabLane) < all.size())
        {
            const double h = all[static_cast<std::size_t>(m_grabLane)].rect.height();
            if (m_grab == Place::ColourScale)
                setLevelSpan(all[static_cast<std::size_t>(m_grabLane)].curve, m_pressDynamic - 0.25 * dy); // up: more dB
            else if (h > 0.0)
                setFrequencies(qacq::plot::shiftRange(m_pressFrequencies, dy / h));
        }
    }
    update();
}

void SpectrogramPlot::mouseReleaseEvent(QMouseEvent *event)
{
    m_pointer = event->position();
    if (m_rightPress && std::abs(m_pointer.x() - m_pressAt.x()) <= 4.0 && std::abs(m_pointer.y() - m_pressAt.y()) <= 4.0)
    {
        // On a scale: the menu of that scale; elsewhere the plot's menu.
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
    // A click, or a drag of the frequencies only, leaves no step to go back to.
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

void SpectrogramPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    int laneIndex = -1;
    const Place place = placeAt(event->position(), &laneIndex);
    if (place == Place::FrequencyScale) setFrequencies({0.0, nyquist()});
    else if (place == Place::ColourScale)
    {
        const QString key = scaleKeyAt(event->position());
        if (!key.isEmpty()) setScaleAutomatic(key);
    }
    else showAll();
}

void SpectrogramPlot::hoverMoveEvent(QHoverEvent *event)
{
    m_pointer = event->position();
    m_pointerInside = true;
    updateCursorShape();
    update();
}

void SpectrogramPlot::hoverLeaveEvent(QHoverEvent *event)
{
    Q_UNUSED(event);
    m_pointerInside = false;
    update();
}
