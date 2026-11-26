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
    // A blank page: nothing computed, even given time, until the recipe is first changed.
    CHECK(link.blank());
    link.waitForIdle();
    CHECK(link.blank());
    CHECK(!link.ready());
    CHECK(!link.hasPreview());
    CHECK(link.currentResult() == nullptr);
    CHECK(!link.resultInfo().value(QStringLiteral("ok")).toBool());
    link.addToSession(false);
    CHECK(link.lastError() == QStringLiteral("La liaison n'est pas encore calculée."));
    // The first change, even to what it already is: computed.
    link.setRecipe({});
    CHECK(!link.blank());
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

    // Remembered as text, and back; reset: the blank page again, nothing left of the link.
    const QString saved = link.recipeText();
    link.reset();
    CHECK(link.blank());
    CHECK(!link.hasPreview());
    CHECK(link.currentResult() == nullptr);
    link.waitForIdle();
    CHECK(link.blank());
    CHECK(link.recipe().value(QStringLiteral("data")).toString() == QStringLiteral("random"));
    link.restoreRecipe(QStringLiteral("pas du JSON"));
    CHECK(link.blank());
    link.restoreRecipe(saved);
    CHECK(!link.blank());
    CHECK(link.recipe().value(QStringLiteral("data")).toString() == QStringLiteral("text"));
    CHECK(link.examples().size() == 21);

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
    link.setRecipe({}); // from the blank page
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

    // The curve of a synchronized link, of an equalized one (linear, decision feedback), of a synchronized one through
    // fading: started. (The recipe is first checked on a link as short as its preamble and its taps allow: with 16
    // symbols, any preamble and any equalizer were refused.)
    link.setSweep({{QStringLiteral("step"), 1.0}});
    CHECK(link.sweepError().isEmpty());
    for (int example : {9, 15, 19, 20})
    {
        link.useExample(example);
        link.startCurve();
        CHECK(link.curveBusy());
        link.stopCurve();
    }
    link.clearCurves();
}

TEST_CASE("Modulation controller: a channel, the synchronized receiver and what it found", "[app]")
{
    const auto text = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toString(); };
    const auto number = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toDouble(); };
    const auto count = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toLongLong(); };
    const auto flag = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toBool(); };
    Session session;
    ModulationController link(&session);
    link.setRecipe({}); // from the blank page
    link.waitForIdle();
    // Without a channel, the ideal receiver: nothing to synchronize, as many samples received as sent.
    QVariantMap result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(!flag(result, "channel"));
    CHECK(!flag(result, "synchronizes"));
    CHECK(result.contains(QStringLiteral("sync")));
    CHECK(!flag(result.value(QStringLiteral("sync")).toMap(), "used"));
    CHECK(count(result, "sentSamples") == 192000);
    CHECK(count(result, "receivedSamples") == 192000);
    CHECK(count(result, "skipSymbols") == 6);
    CHECK(count(result, "skipEndSymbols") == 6);
    CHECK(text(link.recipe(), "receiver") == QStringLiteral("ideal"));
    CHECK(number(link.recipe(), "preamble") == 64.0);

    // QPSK through a channel (40 Hz, 70 degrees, 5.3 symbols, +200 ppm), the receiver finding it.
    link.useExample(9);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(text(result, "text") == QStringLiteral("QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz, synchronisée"));
    CHECK(flag(result, "channel"));
    CHECK(flag(result, "synchronizes"));
    CHECK(count(result, "symbols") == 4000);
    CHECK(count(result, "skipSymbols") == 64); // the preamble
    CHECK(count(result, "skipEndSymbols") == 6);
    CHECK(count(result, "comparedBits") == 7860);
    CHECK(count(result, "bitErrors") == 0);
    CHECK(count(result, "sentSamples") == 192000);
    CHECK(count(result, "receivedSamples") == 192677); // the delay, a tail of 8 symbols, the clock's 200 ppm
    const QVariantMap recipe = link.recipe();
    CHECK(text(recipe, "receiver") == QStringLiteral("sync"));
    CHECK(number(recipe, "frequencyOffset") == 40.0);
    CHECK(number(recipe, "phaseOffset") == 70.0);
    CHECK(number(recipe, "delay") == 5.3);
    CHECK(number(recipe, "clockPpm") == 200.0);
    CHECK(number(recipe, "timingBandwidth") == 0.001);
    CHECK(number(recipe, "phaseBandwidth") == 0.003);
    QVariantMap sync = result.value(QStringLiteral("sync")).toMap();
    REQUIRE(flag(sync, "used"));
    CHECK(flag(sync, "frameFound"));
    CHECK(flag(sync, "carrier"));
    CHECK(flag(sync, "slipsCounted"));
    CHECK(number(sync, "frameQuality") > 0.9);
    CHECK(number(sync, "trueDelay") == 5.3);
    CHECK(number(sync, "trueFrequencyOffset") == 40.0);
    CHECK(number(sync, "trueClock") == 200.0);
    CHECK(std::abs(number(sync, "truePhase") - 70.0) < 1e-9);
    CHECK(std::abs(number(sync, "delay") - 5.3) < 0.05);
    CHECK(std::abs(number(sync, "frequencyOffset") - 40.0) < 0.1);
    CHECK(std::abs(number(sync, "phase") - 70.0) < 2.0);
    CHECK(std::abs(number(sync, "clock") - 200.0) < 5.0);
    CHECK(number(sync, "timingRms") < 0.02);
    CHECK(number(sync, "phaseRms") < 2.0);
    CHECK(count(sync, "slips") == 0);
    CHECK(count(sync, "symbols") == 4000);
    CHECK(count(sync, "preamble") == 64);
    CHECK(text(sync, "frameText").startsWith(QStringLiteral("trouvée à 5,30 symb. (vrai 5,3) · corrélation 9")));
    CHECK(text(sync, "frameText").endsWith(QStringLiteral(" %")));
    CHECK(text(sync, "frequencyText") == QStringLiteral("40,00 Hz (vrai 40 Hz)"));
    CHECK(text(sync, "phaseText").endsWith(QStringLiteral("° (vraie 70,0°)")));
    CHECK(text(sync, "clockText").startsWith(QStringLiteral("+")));
    CHECK(text(sync, "clockText").endsWith(QStringLiteral(" ppm (vraie +200)")));
    CHECK(text(sync, "timingText") == QStringLiteral("0,006 symbole efficace"));
    CHECK(text(sync, "phaseErrorText").endsWith(QStringLiteral("° efficace")));
    // The preview: the record received is longer; the transmission is silent after its own end.
    model::SignalPtr preview = link.previewSession()->signalList()->signalById(ModulationController::previewId());
    REQUIRE(preview != nullptr);
    REQUIRE(preview->channelCount() == 2);
    CHECK(preview->frameCount() == 192677);
    CHECK(preview->data->channels[0].size() == 192677);
    CHECK(preview->data->channels[1].size() == 192677);
    CHECK(preview->data->channels[0][192000] == 0.0);
    CHECK(preview->data->channels[0].back() == 0.0);
    REQUIRE(link.currentResult() != nullptr);
    CHECK(link.currentResult()->sentPassband.size() == 192000);
    CHECK(link.currentResult()->sync.rawPoints.size() == link.currentResult()->reception.decisionPoints.size());
    CHECK(link.currentResult()->reception.matchedRateHz == 8000.0);

    // As I/Q: four channels of the same length.
    link.setRecipe({{QStringLiteral("representation"), QStringLiteral("iq")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(count(result, "receivedSamples") == 192677);
    preview = link.previewSession()->signalList()->signalById(ModulationController::previewId());
    REQUIRE(preview != nullptr);
    REQUIRE(preview->channelCount() == 4);
    for (const std::vector<double> &channel : preview->data->channels) CHECK(channel.size() == 192677);
    CHECK(flag(result.value(QStringLiteral("sync")).toMap(), "frameFound"));

    // The same channel and the ideal receiver, which knows it: no preamble, nothing found.
    link.setRecipe({{QStringLiteral("representation"), QStringLiteral("passband")}, {QStringLiteral("receiver"), QStringLiteral("ideal")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(flag(result, "channel"));
    CHECK(!flag(result, "synchronizes"));
    CHECK(!flag(result.value(QStringLiteral("sync")).toMap(), "used"));
    CHECK(count(result, "skipSymbols") == 6);
    CHECK(count(result, "comparedBits") == 7976);
    CHECK(count(result, "bitErrors") == 0);
    CHECK(count(result, "receivedSamples") == 192677);

    // FSK, decided without phase: a timing to follow, no carrier.
    link.setRecipe({{QStringLiteral("receiver"), QStringLiteral("sync")}, {QStringLiteral("modulation"), QStringLiteral("fsk")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    sync = result.value(QStringLiteral("sync")).toMap();
    REQUIRE(flag(sync, "used"));
    CHECK(!flag(sync, "carrier"));
    CHECK(!flag(sync, "slipsCounted"));
    CHECK(text(sync, "phaseErrorText").isEmpty());
    CHECK(text(sync, "timingText").endsWith(QStringLiteral(" symbole efficace")));
    CHECK(count(sync, "symbols") == 4000);
    REQUIRE(link.currentResult() != nullptr);
    CHECK(link.currentResult()->sync.phaseError.empty());

    // The carrier loop that slips: counted, and said.
    link.useExample(11);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    sync = result.value(QStringLiteral("sync")).toMap();
    CHECK(flag(sync, "slipsCounted"));
    CHECK(count(sync, "slips") >= 1);
    CHECK(number(result, "ber") > 2.0 * number(result, "theoryBer"));
    CHECK(text(sync, "clockText").startsWith(QStringLiteral("−10")));
    CHECK(text(sync, "clockText").endsWith(QStringLiteral(" ppm (vraie −100)")));

    // A delay typed on the ribbon, then none: the keys of the channel one by one.
    link.reset();
    link.setRecipe({{QStringLiteral("delay"), 2.5}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(flag(result, "channel"));
    CHECK(count(result, "receivedSamples") > 192000);
    link.setRecipe({{QStringLiteral("delay"), 0.0}, {QStringLiteral("clockPpm"), -50.0}});
    link.waitForIdle();
    CHECK(flag(link.resultInfo(), "channel"));
    link.setRecipe({{QStringLiteral("clockPpm"), 0.0}});
    link.waitForIdle();
    CHECK(!flag(link.resultInfo(), "channel"));
    CHECK(count(link.resultInfo(), "receivedSamples") == 192000);
}

TEST_CASE("Modulation controller: several paths, the equalizer, and what the panel reads of them", "[app]")
{
    const auto text = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toString(); };
    const auto number = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toDouble(); };
    const auto count = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toLongLong(); };
    const auto flag = [](const QVariantMap &map, const char *key) { return map.value(QString::fromUtf8(key)).toBool(); };
    const auto equalization = [](const QVariantMap &result) { return result.value(QStringLiteral("equalization")).toMap(); };
    Session session;
    ModulationController link(&session);
    link.setRecipe({}); // from the blank page
    link.waitForIdle();
    // One path, no equalizer: nothing to say of either.
    QVariantMap result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(!flag(result, "hasPaths"));
    CHECK(!flag(result, "equalizes"));
    QVariantMap q = equalization(result);
    CHECK(!flag(q, "paths"));
    CHECK(!flag(q, "used"));
    CHECK(count(q, "pathCount") == 0);
    CHECK(text(q, "pathsText").isEmpty());
    CHECK(text(q, "beforeText").isEmpty());
    CHECK(text(q, "afterText").isEmpty());
    CHECK(text(q, "predictedText").isEmpty());
    CHECK(text(q, "errorText").isEmpty());
    CHECK(text(link.recipe(), "paths").isEmpty());
    CHECK(text(link.recipe(), "equalizer") == QStringLiteral("none"));
    CHECK(number(link.recipe(), "equalizerTaps") == 11.0);
    CHECK(number(link.recipe(), "equalizerStep") == 0.05);

    // An echo of half the amplitude, one symbol later: the record received holds it, each symbol gets half the one
    // before, the errors come though the theory announces none.
    link.useExample(13);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(text(result, "text") == QStringLiteral("QPSK 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz, 2 trajets"));
    CHECK(text(link.recipe(), "paths") == QStringLiteral("0 : 0 ; 1 : -6"));
    CHECK(flag(result, "channel"));
    CHECK(flag(result, "hasPaths"));
    CHECK(!flag(result, "equalizes"));
    CHECK(count(result, "sentSamples") == 192000);
    CHECK(count(result, "receivedSamples") == 192432); // the echo, and a tail of 8 symbols
    CHECK(count(result, "comparedBits") == 7976);
    CHECK(count(result, "bitErrors") > 5);
    CHECK(number(result, "theoryBer") < 1e-7);
    q = equalization(result);
    CHECK(flag(q, "paths"));
    CHECK(count(q, "pathCount") == 2);
    CHECK(!flag(q, "used"));
    CHECK(text(q, "pathsText") == QStringLiteral("0 symb. : −0,97 dB · 1 symb. : −6,97 dB"));
    CHECK(text(q, "beforeText") == QStringLiteral("5,36 dB"));
    CHECK(text(q, "afterText").isEmpty());
    CHECK(text(q, "equivalentText").isEmpty());
    CHECK(text(q, "instantText").isEmpty());
    CHECK(number(q, "instant") == 0.0);
    CHECK(std::abs(number(q, "symbolGain") - 1.0) < 1e-3);
    REQUIRE(link.currentResult() != nullptr);
    CHECK(link.currentResult()->equalization.response.size() == 18);
    CHECK(link.currentResult()->equalization.pointsBefore.empty());

    // The same, equalized by the ideal receiver: taps computed from the channel it knows, and what they announce.
    link.setRecipe({{QStringLiteral("equalizer"), QStringLiteral("linear")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(text(result, "text").endsWith(QStringLiteral(", 2 trajets, égalisée")));
    CHECK(flag(result, "equalizes"));
    CHECK(count(result, "skipSymbols") == 6);
    CHECK(count(result, "skipEndSymbols") == 11); // five more: the symbols the 11 taps have no neighbours for
    CHECK(count(result, "comparedBits") == 7966);
    CHECK(count(result, "bitErrors") <= 2);
    q = equalization(result);
    CHECK(flag(q, "used"));
    CHECK(!flag(q, "adaptive"));
    CHECK(count(q, "taps") == 11);
    CHECK(text(q, "beforeText") == QStringLiteral("5,36 dB"));
    CHECK(text(q, "afterText") == QStringLiteral("12,9 dB"));
    CHECK(text(q, "equivalentText") == QStringLiteral("9,90 dB"));
    CHECK(!text(q, "predictedText").isEmpty());
    CHECK(text(q, "errorText").isEmpty());
    REQUIRE(link.currentResult() != nullptr);
    CHECK(link.currentResult()->equalization.pointsBefore.size() == link.currentResult()->reception.decisionPoints.size());
    // Without noise nothing is announced per bit.
    link.setRecipe({{QStringLiteral("noise"), false}});
    link.waitForIdle();
    q = equalization(link.resultInfo());
    CHECK(flag(q, "used"));
    CHECK(text(q, "equivalentText").isEmpty());
    CHECK(text(q, "predictedText").isEmpty());
    CHECK(count(link.resultInfo(), "bitErrors") == 0);
    link.setRecipe({{QStringLiteral("noise"), true}});
    // An even number of taps typed: the next odd one.
    link.setRecipe({{QStringLiteral("equalizerTaps"), 20}});
    link.waitForIdle();
    CHECK(number(link.recipe(), "equalizerTaps") == 21.0);
    CHECK(count(equalization(link.resultInfo()), "taps") == 21);
    CHECK(count(link.resultInfo(), "skipEndSymbols") == 16);

    // Paths that cannot be read: said, what was typed kept.
    link.setRecipe({{QStringLiteral("paths"), QStringLiteral("0 : 0 ; nope")}});
    link.waitForIdle();
    result = link.resultInfo();
    CHECK(!flag(result, "ok"));
    CHECK(text(result, "error").startsWith(QStringLiteral("Trajet 2 (« nope »)")));
    CHECK(!flag(result, "hasPaths"));
    CHECK(text(link.recipe(), "paths") == QStringLiteral("0 : 0 ; nope"));
    CHECK(!link.hasPreview());
    // None typed again: one path.
    link.setRecipe({{QStringLiteral("paths"), QString()}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(!flag(result, "hasPaths"));
    CHECK(!flag(result, "channel"));
    CHECK(flag(result, "equalizes"));
    CHECK(flag(equalization(result), "used"));
    CHECK(!flag(equalization(result), "paths"));
    CHECK(count(result, "receivedSamples") == 192000);

    // The synchronized receiver learns its taps: on the preamble, then on its own decisions.
    link.useExample(16);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(text(result, "text") == QStringLiteral("16-QAM 1 kBd, racine de cosinus surélevé α = 0,35, porteuse 6 kHz, 2 trajets, synchronisée, égalisée"));
    CHECK(flag(result, "synchronizes"));
    CHECK(flag(result, "equalizes"));
    CHECK(count(result, "skipSymbols") == 64);
    CHECK(count(result, "skipEndSymbols") == 11);
    CHECK(count(result, "comparedBits") == 15700);
    CHECK(count(result, "bitErrors") <= 2);
    CHECK(count(result, "receivedSamples") == 192725);
    q = equalization(result);
    CHECK(flag(q, "used"));
    CHECK(flag(q, "adaptive"));
    CHECK(!flag(q, "halfSymbolEarly"));
    CHECK(count(q, "taps") == 11);
    CHECK(text(q, "pathsText") == QStringLiteral("0 symb. : −0,64 dB · 1 symb. : −8,64 dB"));
    CHECK(text(q, "beforeText").startsWith(QStringLiteral("7,9"))); // seen from where its timing loop settled
    CHECK(text(q, "instantText").isEmpty());                         // (on the strongest path, to a hundredth of a symbol)
    CHECK(std::abs(number(q, "symbolGain") - 1.0) < 0.02);
    CHECK(text(q, "afterText").isEmpty());
    CHECK(text(q, "predictedText").isEmpty());
    CHECK(text(q, "errorText").startsWith(QStringLiteral("−2")));
    CHECK(text(q, "errorText").endsWith(QStringLiteral(" dB")));
    QVariantMap sync = result.value(QStringLiteral("sync")).toMap();
    REQUIRE(flag(sync, "used"));
    CHECK(!flag(sync, "timingLost"));
    CHECK(count(sync, "slips") == 0);
    CHECK(text(sync, "frequencyText").startsWith(QStringLiteral("40,0")));
    CHECK(text(sync, "timingText").endsWith(QStringLiteral(" symbole efficace")));
    // Without it, the same link is lost.
    link.setRecipe({{QStringLiteral("equalizer"), QStringLiteral("none")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(!flag(result, "equalizes"));
    CHECK(!flag(equalization(result), "used"));
    CHECK(number(result, "ber") > 0.1);

    // The FSK has the paths, and no equalizer whatever is asked.
    link.setRecipe({{QStringLiteral("equalizer"), QStringLiteral("linear")}, {QStringLiteral("modulation"), QStringLiteral("fsk")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(flag(result, "hasPaths"));
    CHECK(!flag(result, "equalizes"));
    q = equalization(result);
    CHECK(flag(q, "paths"));
    CHECK(!flag(q, "used"));
    CHECK(text(q, "beforeText").isEmpty());
    CHECK(count(result, "skipEndSymbols") == 0);

    // Decision feedback: its feedback taps counted, the forward ones too.
    link.useExample(19);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(text(result, "text").endsWith(QStringLiteral(", 2 trajets, égalisée (retour de décision)")));
    CHECK(text(link.recipe(), "equalizer") == QStringLiteral("dfe"));
    CHECK(number(link.recipe(), "feedbackTaps") == 4.0);
    CHECK(flag(result, "equalizes"));
    CHECK(!flag(result, "fades"));
    CHECK(count(result, "berBatches") == 0);
    q = equalization(result);
    CHECK(flag(q, "used"));
    CHECK(count(q, "taps") == 31);
    CHECK(count(q, "feedback") == 4);
    CHECK(text(q, "afterText") == QStringLiteral("13,0 dB"));
    link.setRecipe({{QStringLiteral("feedbackTaps"), 2}});
    link.waitForIdle();
    CHECK(count(equalization(link.resultInfo()), "feedback") == 2);
    link.setRecipe({{QStringLiteral("equalizer"), QStringLiteral("linear")}});
    link.waitForIdle();
    CHECK(count(equalization(link.resultInfo()), "feedback") == 0);

    // Fading: said, its theory averaged over the gain, the one without fading beside it.
    link.useExample(17);
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(flag(result, "fades"));
    CHECK(flag(result, "channel"));
    CHECK(!flag(result, "hasPaths"));
    CHECK(text(link.recipe(), "fading") == QStringLiteral("rayleigh"));
    CHECK(number(link.recipe(), "doppler") == 20.0);
    CHECK(text(result, "theoryText") == QStringLiteral("0,0077"));
    CHECK(text(result, "awgnTheoryText") == QStringLiteral("9,1×10⁻¹⁶"));
    CHECK(number(result, "awgnTheoryBer") < 1e-15);
    CHECK(number(result, "ber") > 0.003);
    CHECK(count(result, "berBatches") == 16); // its interval from blocks of 250 symbols: the errors come in bursts
    CHECK(number(result, "berLower") < number(result, "ber"));
    link.setRecipe({{QStringLiteral("fading"), QStringLiteral("rice")}, {QStringLiteral("riceK"), 20.0}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(flag(result, "fades"));
    CHECK(number(result, "theoryBer") < 1e-5);
    CHECK(text(result, "text").endsWith(QStringLiteral("évanouissements de Rice (K = 20,0 dB, fd = 20 Hz)")));
    // Too fast for the symbol rate: said.
    link.setRecipe({{QStringLiteral("doppler"), 400.0}});
    link.waitForIdle();
    result = link.resultInfo();
    CHECK(!flag(result, "ok"));
    CHECK(text(result, "error").contains(QStringLiteral("un quart du débit symbole")));
    link.setRecipe({{QStringLiteral("fading"), QStringLiteral("none")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    CHECK(!flag(result, "fades"));
    CHECK(!flag(result, "channel"));

    // An echo half a symbol later: the ideal receiver brings the symbol back to its size (it gets 1.44 times the
    // strongest path there); the timing loop of the synchronized one settles between the two paths, and says where.
    link.reset();
    link.setRecipe({{QStringLiteral("paths"), QStringLiteral("0 : 0 ; 0,5 : -3")}, {QStringLiteral("noise"), false}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    q = equalization(result);
    CHECK(text(q, "instantText").isEmpty());
    CHECK(std::abs(number(q, "symbolGain") - 1.44) < 0.02);
    CHECK((text(q, "beforeText") == QStringLiteral("9,69 dB") || text(q, "beforeText") == QStringLiteral("9,70 dB")));
    CHECK(std::abs(number(result, "mer") - 9.69) < 0.15);
    CHECK(count(result, "bitErrors") == 0);
    link.setRecipe({{QStringLiteral("receiver"), QStringLiteral("sync")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    q = equalization(result);
    CHECK(text(q, "instantText").startsWith(QStringLiteral("0,")));
    CHECK(text(q, "instantText").endsWith(QStringLiteral(" symb. après le trajet le plus fort")));
    CHECK(number(q, "instant") > 0.1);
    CHECK(number(q, "instant") < 0.4);
    CHECK(count(result, "bitErrors") == 0);

    // An echo in phase one symbol later digs the edges of the band, where the timing loop reads the clock: it loses
    // its lock, and the panel says so.
    link.useExample(9);
    link.setRecipe({{QStringLiteral("paths"), QStringLiteral("0 : 0 ; 1 : -3")}, {QStringLiteral("equalizer"), QStringLiteral("linear")}});
    link.waitForIdle();
    result = link.resultInfo();
    REQUIRE(flag(result, "ok"));
    sync = result.value(QStringLiteral("sync")).toMap();
    CHECK(flag(sync, "timingLost"));
    CHECK(text(sync, "timingText").endsWith(QStringLiteral(" symbole efficace (boucle décrochée)")));
    CHECK(number(result, "ber") > 0.2);
    bool warned = false;
    for (const QString &warning : result.value(QStringLiteral("warnings")).toStringList())
        warned = warned || warning.startsWith(QStringLiteral("La boucle de rythme a décroché"));
    CHECK(warned);
}
