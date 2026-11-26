#pragma once
// The plots of the module "Modulations" (qacq::app::ModulationController), as `view` says:
//   "constellation"  the decision points received in the I/Q plane (unit scale), each in the colour of the text
//                    when decided right, of the warning when wrong; the points of the constellation with their
//                    bits, the edges of the decision regions dashed (FSK: the plane of the energies of its two
//                    tones, the diagonal deciding); `showRaw`: under them, muted, the points before the equalizer
//                    when one ran, else those a receiver that only found the frame would decide (synchronized
//                    receiver: neither the carrier turned back nor the clock followed)
//   "eye"            the eye diagram: the output of the matched filter over two symbols about each decision instant,
//                    one band for I, one for Q (for the modulations that use Q); the levels of the constellation
//                    dotted, the decision instant dashed
//   "sync"           the synchronized receiver, symbol by symbol from the start of the frame: the error of its
//                    timing (in symbols) and, in a second band, of its carrier phase (in degrees, unwrapped: a
//                    slip is a step to another symmetry of the constellation, dashed); the preamble shaded
//   "channel"        several paths as the decisions see them, and the equalizer: in a first band, symbol by symbol,
//                    the modulus of the taps of the channel (what each symbol receives of its neighbours; the paths
//                    themselves as diamonds), of the equalizer, and of both together (computed equalizer: about a
//                    single 1); in a second band their gains along the frequency (dB), over one symbol rate about
//                    the carrier (beyond, they repeat); their names and colours above the frame
//   "ber"            the bit error rate along Eb/N0 (logarithmic): for each curve measured, its theory in a line and
//                    its points with their 95 % interval (a point without error: a triangle under its upper bound);
//                    the theory of the link as it is dashed, and its point (a diamond)
//
// Above the frame, the title of the plot and what it shows (symbols, those decided wrong, EVM; traces drawn), on as
// many lines as they need: never over the points.
//
// Mouse: the wheel zooms about the pointer (the constellation both ways; the eye its values, Ctrl its time; the
// curves Eb/N0, Ctrl the rates; the synchronization its symbols, Ctrl the errors of the band under the pointer; the
// channel the symbols or the frequencies of the band under the pointer, Ctrl its values); a drag moves the view; a
// double click shows everything again. Under the pointer, what is there (`readout`, also drawn at the bottom of
// the band under the pointer).
#include "qacq/plot/trace.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <complex>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qacq::analysis
{
struct LinkResult;
struct LinkRecipe;
}
namespace qacq::app
{
class ModulationController;
}

class ModulationPlot : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *modulation READ modulation WRITE setModulation NOTIFY modulationChanged)
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY kindChanged)
    Q_PROPERTY(bool showRaw READ showRaw WRITE setShowRaw NOTIFY showRawChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER m_muted NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    Q_PROPERTY(QColor warningColor MEMBER m_warning NOTIFY styleChanged)
    Q_PROPERTY(QColor panelColor MEMBER m_panel NOTIFY styleChanged)
    Q_PROPERTY(QVariantList curveColors MEMBER m_curveColors NOTIFY styleChanged)
    Q_PROPERTY(int fontSize MEMBER m_fontSize NOTIFY styleChanged)
    Q_PROPERTY(QString emptyText MEMBER m_emptyText NOTIFY styleChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY contentChanged)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY viewChanged)
    Q_PROPERTY(QString readout READ readout NOTIFY readoutChanged)

public:
    explicit ModulationPlot(QQuickItem *parent = nullptr);

    QObject *modulation() const;
    void setModulation(QObject *modulation);
    QString view() const;
    void setView(const QString &key);
    bool showRaw() const { return m_showRaw; }
    void setShowRaw(bool show);
    bool hasData() const;
    bool zoomed() const { return !m_view.automatic; }
    bool canGoBack() const { return !m_history.empty(); }
    QString readout() const { return m_readoutMore.isEmpty() ? m_readout : m_readout + QStringLiteral(" · ") + m_readoutMore; }

    // Everything again, automatic scale.
    Q_INVOKABLE void showAll();
    // About the centre of the view: factor < 1 zooms in (the constellation both ways, the eye its values, the curves
    // Eb/N0, the synchronization and the channel their symbols).
    Q_INVOKABLE void zoomView(double factor);
    Q_INVOKABLE void goBack();

    void paint(QPainter *painter) override;

signals:
    void modulationChanged();
    void kindChanged();
    void showRawChanged();
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

private:
    using Range = qacq::plot::Range;
    enum class Kind
    {
        Constellation,
        Eye,
        Ber,
        Sync,
        Channel
    };
    struct View
    {
        Range x{-1.0, 1.0};
        Range y{-1.0, 1.0};  // the rates: their log10; the synchronization: the timing error (symbols)
        Range y2{-1.0, 1.0}; // the synchronization: the phase error (degrees); the channel: the gains (dB)
        Range x2{-1.0, 1.0}; // the channel: the frequencies of its second band (Hz)
        bool automatic = true;
    };
    // Taps once per symbol (taps[m] is the one of index first + m), and their gain along the frequency (dB): evenly
    // over one symbol rate, from half of it under the carrier to half of it above.
    struct TapSeries
    {
        int first = 0;
        std::vector<std::complex<double>> taps;
        std::vector<double> gainDb;
    };
    // The plane of the constellation on screen: the same scale both ways.
    struct Plane
    {
        double centreX = 0.0, centreY = 0.0, scale = 1.0;
        QPointF origin;
        QPointF toScreen(double i, double q) const;
        double iAt(double x) const;
        double qAt(double y) const;
    };

    // A line above the frame: the title on the left, facts on the right.
    struct HeaderRow
    {
        QString left, right;
    };
    // What the view "channel" draws, by name: a dot of its colour (the paths themselves: a diamond).
    struct LegendEntry
    {
        QString name;
        QColor colour;
        bool diamond = false;
    };

    void contentUpdated();
    void countResult();
    QFont plotFont() const;
    QString headerTitle() const;
    QStringList headerFacts() const;
    std::vector<HeaderRow> headerRows() const; // the title, then the facts on as many lines as they need
    std::vector<LegendEntry> channelLegend() const; // none for the other views
    // Those names as they go above the frame: on the right of the title when they all fit beside it (`beside`),
    // else on as many lines as they need under it; the facts come after.
    std::vector<std::vector<LegendEntry>> legendRows(bool &beside) const;
    double topMargin() const;
    QRectF plotRect() const;
    View automaticView() const;
    View currentView() const; // automatic or as set
    void setViewRange(const View &view);
    void rememberView();
    void zoomAt(double factor, const QPointF &at, bool other);
    void moveBy(double dx, double dy);
    void updateReadout();
    Plane plane(const QRectF &area, const View &v) const;
    std::vector<QRectF> eyeLanes(const QRectF &area) const;
    std::vector<QRectF> syncLanes(const QRectF &area) const; // the timing, then the carrier (not for FSK)
    std::vector<QRectF> channelLanes(const QRectF &area) const; // symbol by symbol, then along the frequency
    std::vector<QRectF> lanesOf(const QRectF &area) const;   // of the view shown (one band: the area)
    bool rawShown() const;                                   // points before the equalizer or the synchronization to draw
    // The theory of a link's error rate along the Eb/N0 of the view, its log10 (computed once per view; fading:
    // averaged over the gain; several paths that fade: none). Its name: "QPSK", "QPSK, Rayleigh", "16-QAM, Rice K = 10 dB".
    const std::vector<double> &theory(const qacq::analysis::LinkRecipe &recipe, const Range &x, int count);
    static QString theoryName(const qacq::analysis::LinkRecipe &recipe);

    void paintConstellation(QPainter &p, const QRectF &area);
    void paintEye(QPainter &p, const QRectF &area);
    void paintSync(QPainter &p, const QRectF &area);
    void paintChannel(QPainter &p, const QRectF &area);
    void paintBer(QPainter &p, const QRectF &area);
    void paintMessage(QPainter &p, const QString &message);
    void paintHeader(QPainter &p);
    QColor curveColour(std::size_t index) const;

    QPointer<qacq::app::ModulationController> m_modulation;
    QMetaObject::Connection m_resultConnection, m_curvesConnection, m_recipeConnection;
    std::shared_ptr<const qacq::analysis::LinkResult> m_result;
    Kind m_kind = Kind::Constellation;
    View m_view;
    std::vector<View> m_history;
    std::map<std::string, std::vector<double>> m_theory;
    // Counted once per result: the symbols shown (ends left out), those decided wrong, one point drawn in
    // m_pointStep; the traces of the eye, one drawn in m_traceStep; the phase error of the synchronization unwrapped
    // (degrees), how far the points before the equalizer or the synchronization reach; the channel at the decision
    // instants, its equalizer and both together.
    std::size_t m_shown = 0, m_wrong = 0, m_pointStep = 1, m_traces = 0, m_traceStep = 1;
    std::vector<double> m_syncPhase;
    double m_rawReach = 0.0;
    TapSeries m_channel, m_equalizer, m_combined;
    bool m_showRaw = false;

    bool m_hovering = false;
    QPointF m_pointer;
    QString m_readout, m_readoutMore; // (the second: what a narrow band draws on a line of its own)
    bool m_dragging = false;
    QPointF m_lastAt, m_pressAt;

    QColor m_background = QColor(15, 17, 20);
    QColor m_grid = QColor(255, 255, 255, 23);
    QColor m_text = QColor(230, 232, 236);
    QColor m_muted = QColor(154, 161, 172);
    QColor m_accent = QColor(90, 170, 255);
    QColor m_warning = QColor(255, 110, 100);
    QColor m_panel = QColor(28, 31, 36);
    QVariantList m_curveColors;
    int m_fontSize = 11;
    QString m_emptyText;
};
