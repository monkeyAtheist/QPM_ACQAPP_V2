#pragma once
// The plots of the module "Modulations" (qacq::app::ModulationController), as `view` says:
//   "constellation"  the decision points received in the I/Q plane (unit scale), each in the colour of the text
//                    when decided right, of the warning when wrong; the points of the constellation with their
//                    bits, the edges of the decision regions dashed (FSK: the plane of the energies of its two
//                    tones, the diagonal deciding)
//   "eye"            the eye diagram: the output of the matched filter over two symbols about each decision instant,
//                    one band for I, one for Q (for the modulations that use Q); the levels of the constellation
//                    dotted, the decision instant dashed
//   "ber"            the bit error rate along Eb/N0 (logarithmic): for each curve measured, its theory in a line and
//                    its points with their 95 % interval (a point without error: a triangle under its upper bound);
//                    the theory of the link as it is dashed, and its point (a diamond)
//
// Above the frame, the title of the plot and what it shows (symbols, those decided wrong, EVM; traces drawn), on as
// many lines as they need: never over the points.
//
// Mouse: the wheel zooms about the pointer (the constellation both ways; the eye its values, Ctrl its time; the
// curves Eb/N0, Ctrl the rates); a drag moves the view; a double click shows everything again. Under the pointer,
// what is there (`readout`, also drawn at the bottom of the band under the pointer).
#include "qacq/plot/trace.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qacq::analysis
{
struct LinkResult;
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
    bool hasData() const;
    bool zoomed() const { return !m_view.automatic; }
    bool canGoBack() const { return !m_history.empty(); }
    QString readout() const { return m_readout; }

    // Everything again, automatic scale.
    Q_INVOKABLE void showAll();
    // About the centre of the view: factor < 1 zooms in (the constellation both ways, the eye its values, the curves
    // Eb/N0).
    Q_INVOKABLE void zoomView(double factor);
    Q_INVOKABLE void goBack();

    void paint(QPainter *painter) override;

signals:
    void modulationChanged();
    void kindChanged();
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
        Ber
    };
    struct View
    {
        Range x{-1.0, 1.0};
        Range y{-1.0, 1.0}; // the rates: their log10
        bool automatic = true;
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

    void contentUpdated();
    void countResult();
    QFont plotFont() const;
    QString headerTitle() const;
    QStringList headerFacts() const;
    std::vector<HeaderRow> headerRows() const; // the title, then the facts on as many lines as they need
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
    // The theory of a modulation along the Eb/N0 of the view, its log10 (computed once per view).
    const std::vector<double> &theory(int modulation, double askLowLevel, const Range &x, int count);

    void paintConstellation(QPainter &p, const QRectF &area);
    void paintEye(QPainter &p, const QRectF &area);
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
    // m_pointStep; the traces of the eye, one drawn in m_traceStep.
    std::size_t m_shown = 0, m_wrong = 0, m_pointStep = 1, m_traces = 0, m_traceStep = 1;

    bool m_hovering = false;
    QPointF m_pointer;
    QString m_readout;
    bool m_dragging = false;
    QPointF m_lastAt;

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
