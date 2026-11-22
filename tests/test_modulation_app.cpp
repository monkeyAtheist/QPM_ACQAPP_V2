// The module "Modulations": the link computed on a thread of its own after each change (its preview, its result),
// the curves of the bit error rate measured point by point, and the signals sent or received into the session.
#include "qacq_test.h"

#include "qacq/app/modulation_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <cmath>
#include <functional>

using namespace qacq;
using namespace qacq::app;

namespace
{
bool settle(const std::function<bool()> &condition, int timeoutMs = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
    }
    return condition();
}
} // namespace

TEST_CASE("Modulation controller: the link follows the recipe, its preview and its result", "[app]")
{
    Session session;
    ModulationController link(&session);
    link.waitForIdle();
    QVariantMap result = link.resultInfo();
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("text")).toString() ==
          QStringLiteral("QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz"));
    CHECK(result.value(QStringLiteral("bitsPerSymbol")).toInt() == 2);
    CHECK(result.value(QStringLiteral("constellationSize")).toInt() == 4);
    CHECK(result.value(QStringLiteral("symbols")).toLongLong() == 4000);
    CHECK(result.value(QStringLiteral("comparedBits")).toLongLong() == 2 * (4000 - 12));
    CHECK(std::abs(result.value(QStringLiteral("measuredEbN0")).toDouble() - 7.0) < 0.1);
    CHECK(result.value(QStringLiteral("theoryBer")).toDouble() > 7e-4);
    CHECK(link.hasPreview());
    CHECK(link.ready());
    // The preview: sent and received, on a carrier.
    const model::SignalPtr preview = link.previewSession()->signalList()->signalById(ModulationController::previewId());
    REQUIRE(preview != nullptr);
    CHECK(preview->channelCount() == 2);
    CHECK(preview->channels[1].name == "Reçu");
    CHECK(preview->frameCount() == 4000 * 48);
    REQUIRE(link.currentResult() != nullptr);
    CHECK(link.currentResult()->reception.decisionPoints.size() == 4000);

    // A change: computed again after its delay, the newest winning.
    link.setRecipe({{QStringLiteral("modulation"), QStringLiteral("16qam")}, {QStringLiteral("ebN0"), 30.0}});
    link.setRecipe({{QStringLiteral("representation"), QStringLiteral("iq")}});
    CHECK(!link.ready());
    link.waitForIdle();
    result = link.resultInfo();
    CHECK(result.value(QStringLiteral("text")).toString() ==
          QStringLiteral("16-QAM 1 kBd, racine de cosinus surélevé α = 0,35, en I/Q"));
    CHECK(result.value(QStringLiteral("bitErrors")).toLongLong() == 0);
    CHECK(result.value(QStringLiteral("iq")).toBool());
    const model::SignalPtr iq = link.previewSession()->signalList()->signalById(ModulationController::previewId());
    REQUIRE(iq != nullptr);
    CHECK(iq->channelCount() == 4);
    CHECK(iq->representation == model::Representation::IQ);
    CHECK(link.recipe().value(QStringLiteral("modulation")).toString() == QStringLiteral("16qam"));

    // A recipe that cannot be: said, and no preview.
    link.setRecipe({{QStringLiteral("symbolRate"), 1100.0}});
    link.waitForIdle();
    result = link.resultInfo();
    CHECK(!result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("nombre entier")));
    CHECK(!link.hasPreview());

    // A text, as received.
    link.useExample(8); // QPSK without noise: a text
    link.waitForIdle();
    result = link.resultInfo();
    CHECK(result.value(QStringLiteral("textReceived")).toString().startsWith(QStringLiteral("Bonjour ! Bonjour !")));
    CHECK(result.value(QStringLiteral("textErrors")).toLongLong() == 0);

    // Remembered as text, and back; reset.
    const QString saved = link.recipeText();
    link.reset();
    link.waitForIdle();
    CHECK(link.recipe().value(QStringLiteral("data")).toString() == QStringLiteral("random"));
    link.restoreRecipe(saved);
    CHECK(link.recipe().value(QStringLiteral("data")).toString() == QStringLiteral("text"));
    CHECK(link.examples().size() == 9);

    // Not on screen: nothing computed until it is again.
    link.setActive(false);
    link.setRecipe({{QStringLiteral("ebN0"), 3.0}, {QStringLiteral("noise"), true}});
    link.waitForIdle();
    CHECK(!link.ready());
    link.setActive(true);
    link.waitForIdle();
    CHECK(link.ready());
    CHECK(std::abs(link.resultInfo().value(QStringLiteral("measuredEbN0")).toDouble() - 3.0) < 0.1);
}

TEST_CASE("Modulation controller: curves of the bit error rate, and the signals into the session", "[app]")
{
    Session session;
    ModulationController link(&session);
    link.waitForIdle();
    link.setRecipe({{QStringLiteral("modulation"), QStringLiteral("bpsk")}, {QStringLiteral("pulse"), QStringLiteral("rect")}});
    link.waitForIdle();
    link.setSweep({{QStringLiteral("from"), 0.0}, {QStringLiteral("to"), 4.0}, {QStringLiteral("step"), 2.0},
                   {QStringLiteral("maxBits"), 20000}});
    CHECK(link.sweepError().isEmpty());
    link.startCurve();
    CHECK(link.curveBusy());
    CHECK(settle([&] { return !link.curveBusy(); }, 30000));
    QVariantList curves = link.curvesInfo();
    REQUIRE(curves.size() == 1);
    QVariantMap curve = curves[0].toMap();
    CHECK(curve.value(QStringLiteral("done")).toBool());
    CHECK(curve.value(QStringLiteral("label")).toString().startsWith(QStringLiteral("BPSK 1 kBd")));
    const QVariantList points = curve.value(QStringLiteral("points")).toList();
    REQUIRE(points.size() == 3);
    for (const QVariant &p : points)
    {
        const QVariantMap point = p.toMap();
        CHECK(point.value(QStringLiteral("bits")).toLongLong() >= 5000);
        CHECK(point.value(QStringLiteral("lower")).toDouble() <= point.value(QStringLiteral("ber")).toDouble());
        CHECK(point.value(QStringLiteral("upper")).toDouble() >= point.value(QStringLiteral("ber")).toDouble());
    }
    CHECK(std::abs(points[1].toMap().value(QStringLiteral("theory")).toDouble() - 0.0375061) < 1e-6);

    // A second curve beside it; stopped half-way it stays, unfinished; cleared.
    link.setRecipe({{QStringLiteral("modulation"), QStringLiteral("16qam")}});
    link.setSweep({{QStringLiteral("maxBits"), 4000000}, {QStringLiteral("to"), 12.0}});
    link.startCurve();
    CHECK(link.curveBusy());
    link.stopCurve();
    CHECK(!link.curveBusy());
    curves = link.curvesInfo();
    REQUIRE(curves.size() == 2);
    CHECK(!curves[1].toMap().value(QStringLiteral("done")).toBool());
    link.clearCurves();
    CHECK(link.curvesInfo().isEmpty());
    // A sweep that cannot be.
    link.setSweep({{QStringLiteral("step"), 0.0}});
    CHECK(!link.sweepError().isEmpty());
    link.startCurve();
    CHECK(!link.curveBusy());
    CHECK(!link.lastError().isEmpty());

    // The signals sent and received, into the session (undoable).
    link.waitForIdle();
    qint64 added = 0;
    QObject::connect(&link, &ModulationController::added, [&added](qint64 id) { added = id; });
    link.addToSession(false);
    REQUIRE(added > 0);
    const model::SignalPtr sent = session.signalList()->signalById(added);
    REQUIRE(sent != nullptr);
    CHECK(sent->name == "16-QAM 1 kBd (émis)");
    CHECK(sent->channelCount() == 1);
    link.addToSession(true);
    const model::SignalPtr received = session.signalList()->signalById(added);
    REQUIRE(received != nullptr);
    CHECK(received->name.rfind("16-QAM 1 kBd (reçu, Eb/N0 = ", 0) == 0);
    CHECK(session.signalList()->count() == 2);
}
