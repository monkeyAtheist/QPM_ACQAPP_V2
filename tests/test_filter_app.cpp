// The module "Filtres": the filter designed, tried on a signal of the session (a preview before and after, on
// a thread of its own), then the whole signal filtered into the session in one step.
#include "qacq_test.h"

#include "qacq/app/filter_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

using namespace qacq;
using namespace qacq::app;

namespace
{
bool settle(const std::function<bool()> &condition, int timeoutMs = 5000)
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

// 100 Hz and 5 kHz, of 1 V each, on `channels` channels (the second one at half the level).
model::Signal twoTones(double fs, std::size_t n, std::size_t channels = 1)
{
    model::Signal s;
    s.name = "Deux tons";
    s.sampleRateHz = fs;
    auto data = std::make_shared<model::SampleData>();
    for (std::size_t c = 0; c < channels; ++c)
    {
        s.channels.push_back({"Voie " + std::to_string(c + 1), "V"});
        std::vector<double> x(n);
        const double level = c == 0 ? 1.0 : 0.5;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double>(i) / fs;
            x[i] = level * (std::sin(2.0 * std::numbers::pi * 100.0 * t) + std::sin(2.0 * std::numbers::pi * 5000.0 * t));
        }
        data->channels.push_back(std::move(x));
    }
    s.data = data;
    return s;
}

// RMS over the second half (once the filter has settled).
double rms(std::span<const double> x)
{
    double sum = 0.0;
    const std::size_t from = x.size() / 2;
    for (std::size_t i = from; i < x.size(); ++i) sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(x.size() - from));
}
} // namespace

TEST_CASE("Filter controller: the design follows the recipe, and says what it is", "[app]")
{
    Session session;
    FilterController filter(&session);
    // A blank page: no filter, one coefficient 1; nothing to write in C.
    QVariantMap design = filter.designInfo();
    CHECK(filter.recipe().value(QStringLiteral("kind")).toString() == QStringLiteral("none"));
    CHECK(design.value(QStringLiteral("ok")).toBool());
    CHECK(design.value(QStringLiteral("text")).toString() == QStringLiteral("Aucun filtre"));
    CHECK(design.value(QStringLiteral("tapCount")).toInt() == 1);
    CHECK(design.value(QStringLiteral("coefficients")).toString() == QStringLiteral("h[0] = 1"));
    CHECK(design.value(QStringLiteral("summary")).toStringList().value(0).startsWith(QStringLiteral("Aucun filtre : la sortie est l'entrée")));
    CHECK(!filter.code({}).value(QStringLiteral("ok")).toBool());
    // A kind chosen: the usual first filter.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}});
    design = filter.designInfo();
    CHECK(design.value(QStringLiteral("ok")).toBool());
    CHECK(design.value(QStringLiteral("text")).toString() == QStringLiteral("Passe-bas Butterworth d'ordre 4, 1 kHz"));
    CHECK(design.value(QStringLiteral("sections")).toList().size() == 2);
    CHECK(design.value(QStringLiteral("poleCount")).toInt() == 4);
    CHECK(design.value(QStringLiteral("coefficients")).toString().startsWith(QStringLiteral("# Section 1\nb0 = ")));
    CHECK(design.value(QStringLiteral("coefficients")).toString().contains(QStringLiteral("\n\n# Section 2\nb0 = ")));
    const QString all = filter.coefficientsText();
    CHECK(all.startsWith(QStringLiteral("# Passe-bas Butterworth d'ordre 4, 1 kHz, fs = 48000 Hz\n")));
    CHECK(all.contains(QStringLiteral("a2 = ")));
    CHECK(all.endsWith(QStringLiteral("\n")));
    CHECK(!design.value(QStringLiteral("summary")).toStringList().isEmpty());
    filter.waitForIdle();
    CHECK(filter.ready());
    CHECK(!filter.hasPreview()); // nothing to try it on
    CHECK(filter.previewError().isEmpty());

    // A FIR: aligned, unless said otherwise; back to an IIR: causal.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("fir")}});
    CHECK(filter.recipe().value(QStringLiteral("kind")).toString() == QStringLiteral("fir"));
    CHECK(filter.recipe().value(QStringLiteral("application")).toString() == QStringLiteral("aligned"));
    design = filter.designInfo();
    CHECK(design.value(QStringLiteral("tapCount")).toInt() == 101);
    const std::string lines = design.value(QStringLiteral("coefficients")).toString().toStdString();
    CHECK(std::count(lines.begin(), lines.end(), '\n') == 100); // h[0] to h[100], one a line
    CHECK(lines.rfind("h[0] = ", 0) == 0);
    // A long one: the first ones to read, all of them to copy.
    filter.setRecipe({{QStringLiteral("taps"), 1001}});
    const QString shown = filter.designInfo().value(QStringLiteral("coefficients")).toString();
    CHECK(shown.contains(QStringLiteral("h[599] = ")));
    CHECK(!shown.contains(QStringLiteral("h[600] = ")));
    CHECK(shown.contains(QStringLiteral("coefficients en tout")));
    CHECK(filter.coefficientsText().contains(QStringLiteral("h[1000] = ")));
    filter.setRecipe({{QStringLiteral("taps"), 101}});
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}});
    CHECK(filter.recipe().value(QStringLiteral("application")).toString() == QStringLiteral("causal"));
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("fir")}, {QStringLiteral("application"), QStringLiteral("zerophase")}});
    CHECK(filter.recipe().value(QStringLiteral("application")).toString() == QStringLiteral("zerophase"));
    // At zero phase, the edges: predicted unless another extension is chosen, as the summary says.
    CHECK(filter.recipe().value(QStringLiteral("edges")).toString() == QStringLiteral("predicted"));
    const auto summary = [&filter] {
        return filter.designInfo().value(QStringLiteral("summary")).toStringList().join(QStringLiteral(" "));
    };
    CHECK(summary().contains(QStringLiteral("prédiction linéaire")));
    filter.setRecipe({{QStringLiteral("edges"), QStringLiteral("even")}});
    CHECK(filter.recipe().value(QStringLiteral("edges")).toString() == QStringLiteral("even"));
    CHECK(summary().contains(QStringLiteral("Bords prolongés en miroir")));
    filter.setRecipe({{QStringLiteral("edges"), QStringLiteral("predicted")}});

    // Equiripple (Parks-McClellan), as the interface asks for it; then a window again.
    filter.setRecipe({{QStringLiteral("firDesign"), QStringLiteral("equiripple")},
                      {QStringLiteral("frequency1"), 2000.0},
                      {QStringLiteral("transition"), 1400.0},
                      {QStringLiteral("ripple"), 0.1},
                      {QStringLiteral("attenuation"), 60.0}});
    CHECK(filter.recipe().value(QStringLiteral("firDesign")).toString() == QStringLiteral("equiripple"));
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("équiondulation")));
    CHECK(filter.designInfo().value(QStringLiteral("summary")).toStringList().join(QStringLiteral(" ")).contains(QStringLiteral("Parks-McClellan")));
    filter.setRecipe({{QStringLiteral("firDesign"), QStringLiteral("window")}, {QStringLiteral("window"), QStringLiteral("blackman")}});
    CHECK(filter.recipe().value(QStringLiteral("firDesign")).toString() == QStringLiteral("window"));
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("Blackman")));
    // A gain at the output: in the text, before the coefficients.
    filter.setRecipe({{QStringLiteral("gain"), 6.0}});
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("gain +6 dB")));
    CHECK(filter.designInfo().value(QStringLiteral("coefficients")).toString().startsWith(QStringLiteral("# Gain en sortie (+6 dB)")));
    CHECK(filter.coefficientsText().contains(QStringLiteral("g*(h[0]*x[n]")));
    filter.setRecipe({{QStringLiteral("gain"), 0.0}});
    CHECK(!filter.coefficientsText().contains(QStringLiteral("g = ")));
    // Elliptic.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}, {QStringLiteral("family"), QStringLiteral("elliptic")}});
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("elliptique")));
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("fir")}, {QStringLiteral("frequency1"), 1000.0}});

    // Coefficients typed: a FIR when a = 1, else sections; what was typed, as evaluated; unstable, not applied.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("typed")}, {QStringLiteral("b"), QStringLiteral("1/N")},
                      {QStringLiteral("bCount"), QStringLiteral("N")}, {QStringLiteral("a"), QStringLiteral("1")},
                      {QStringLiteral("variables"), QStringLiteral("N = 4")}});
    CHECK(filter.recipe().value(QStringLiteral("kind")).toString() == QStringLiteral("typed"));
    CHECK(filter.recipe().value(QStringLiteral("application")).toString() == QStringLiteral("causal"));
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("fir")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("numeratorOfK")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("typedBCount")).toInt() == 4);
    CHECK(filter.designInfo().value(QStringLiteral("typedB")).toList().value(3).toDouble() == 0.25);
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString() == QStringLiteral("Coefficients tapés, RIF de 4 coefficients"));
    CHECK(filter.coefficientsText().contains(QStringLiteral("h[3] = 0.25")));
    filter.setRecipe({{QStringLiteral("b"), QStringLiteral("alpha")}, {QStringLiteral("bCount"), QString()},
                      {QStringLiteral("a"), QStringLiteral("2 ; 2*(alpha - 1)")}, {QStringLiteral("variables"), QStringLiteral("alpha = 0,1")}});
    CHECK(!filter.designInfo().value(QStringLiteral("fir")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("typedACount")).toInt() == 2);
    const QString typedText = filter.coefficientsText();
    CHECK(typedText.contains(QStringLiteral("# b et a tapés, divisés par a[0] = 2")));
    CHECK(typedText.contains(QStringLiteral("b[0] = 0.050000000000000003")));
    CHECK(typedText.contains(QStringLiteral("a[1] = -0.90000000000000002")));
    CHECK(typedText.contains(QStringLiteral("# Section 1")));
    filter.setRecipe({{QStringLiteral("a"), QStringLiteral("1 ; -1,5")}});
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("applicationError")).toString().contains(QStringLiteral("instable")));
    filter.setRecipe({{QStringLiteral("b"), QStringLiteral("k")}});
    CHECK(!filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("numeratorOfK")).toBool()); // the count is asked for
    CHECK(filter.designInfo().value(QStringLiteral("error")).toString().contains(QStringLiteral("combien de coefficients")));
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}});

    // A band whose upper edge is below the lower one gets one above.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")},
                      {QStringLiteral("response"), QStringLiteral("bandpass")},
                      {QStringLiteral("frequency1"), 1000.0},
                      {QStringLiteral("frequency2"), 500.0}});
    CHECK(filter.recipe().value(QStringLiteral("frequency2")).toDouble() > 1000.0);
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());

    // Text, and back; « Par défaut »: the blank page again.
    const QString text = filter.recipeText();
    filter.reset();
    CHECK(filter.recipe().value(QStringLiteral("kind")).toString() == QStringLiteral("none"));
    CHECK(filter.recipe().value(QStringLiteral("response")).toString() == QStringLiteral("lowpass"));
    filter.restoreRecipe(text);
    CHECK(filter.recipe().value(QStringLiteral("response")).toString() == QStringLiteral("bandpass"));
    filter.restoreRecipe(QStringLiteral("pas du JSON"));
    CHECK(filter.recipe().value(QStringLiteral("response")).toString() == QStringLiteral("bandpass"));

    // The examples.
    const QVariantList examples = filter.examples();
    REQUIRE(examples.size() >= 5);
    filter.useExample(1);
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("45 Hz à 55 Hz")));
    CHECK(filter.recipe().value(QStringLiteral("application")).toString() == QStringLiteral("zerophase"));
    filter.useExample(-1); // nothing
    filter.useExample(1000);
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString().contains(QStringLiteral("45 Hz à 55 Hz")));

    // What cannot be designed: said, and no preview.
    filter.setRecipe({{QStringLiteral("response"), QStringLiteral("lowpass")}, {QStringLiteral("frequency1"), 30000.0}});
    CHECK(!filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(!filter.designInfo().value(QStringLiteral("error")).toString().isEmpty());
    CHECK(filter.currentDesign() != nullptr);
    CHECK(filter.currentImpulse()->impulse.empty());
    filter.addToSession();
    CHECK(!filter.lastError().isEmpty());
}

TEST_CASE("Filter controller: an order computed from a template, and back", "[app]")
{
    Session session;
    FilterController filter(&session);
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}}); // from the blank page
    const auto value = [&filter](const char *key) { return filter.recipe().value(QString::fromLatin1(key)); };
    // The recipe as designed: what the template computed.
    const auto designed = [&filter](const char *key) {
        return filter.designInfo().value(QStringLiteral("recipe")).toMap().value(QString::fromLatin1(key));
    };
    // Switched on: a template the filter holds, read on its gain (a Butterworth of order 4 at 1 kHz: 1 dB at most up
    // to 844 Hz, 40 dB at least from 3,13 kHz), and the least order for it: the same.
    filter.setRecipe({{QStringLiteral("fromTemplate"), true}});
    CHECK(value("fromTemplate").toBool());
    CHECK(value("pass1").toDouble() == 844.0);
    CHECK(value("stop1").toDouble() == 3130.0);
    QVariantMap design = filter.designInfo();
    CHECK(design.value(QStringLiteral("ok")).toBool());
    CHECK(design.value(QStringLiteral("templateHeld")).toBool());
    CHECK(designed("order").toInt() == 4);
    // A template typed: the order computed; the recipe keeps the order chosen.
    filter.setRecipe({{QStringLiteral("pass1"), 1000.0}, {QStringLiteral("stop1"), 2000.0}});
    design = filter.designInfo();
    CHECK(designed("order").toInt() == 8); // Butterworth, 1 dB up to 1 kHz, 40 dB from 2 kHz
    CHECK(value("order").toInt() == 4);
    CHECK(design.value(QStringLiteral("text")).toString() ==
          QStringLiteral("Passe-bas Butterworth d'ordre 8, gabarit 1 dB jusqu'à 1 kHz, 40 dB dès 2 kHz"));
    CHECK(design.value(QStringLiteral("templateText")).toString().startsWith(QStringLiteral("Gabarit : au plus 1 dB")));
    CHECK(design.value(QStringLiteral("templateAttenuation")).toDouble() > 40.0);
    // An edge typed wrong stays as typed: the design says why.
    filter.setRecipe({{QStringLiteral("stop1"), 900.0}});
    CHECK(value("stop1").toDouble() == 900.0);
    CHECK(!filter.designInfo().value(QStringLiteral("ok")).toBool());
    CHECK(filter.designInfo().value(QStringLiteral("error")).toString().contains(QStringLiteral("Gabarit d'un passe-bas")));
    filter.setRecipe({{QStringLiteral("stop1"), 2000.0}});
    // Another response: edges in its own order.
    filter.setRecipe({{QStringLiteral("response"), QStringLiteral("highpass")}});
    CHECK(value("stop1").toDouble() < value("pass1").toDouble());
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());
    filter.setRecipe({{QStringLiteral("response"), QStringLiteral("lowpass")}});
    CHECK(value("pass1").toDouble() < value("stop1").toDouble());
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());
    // A FIR from the same template.
    filter.setRecipe({{QStringLiteral("pass1"), 1000.0}, {QStringLiteral("stop1"), 2000.0}});
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("fir")}, {QStringLiteral("firDesign"), QStringLiteral("equiripple")}});
    design = filter.designInfo();
    CHECK(design.value(QStringLiteral("ok")).toBool());
    CHECK(design.value(QStringLiteral("templateHeld")).toBool());
    const int taps = designed("taps").toInt();
    CHECK(taps == 81);
    // Back to an order chosen: from the filter computed.
    filter.setRecipe({{QStringLiteral("fromTemplate"), false}});
    CHECK(!value("fromTemplate").toBool());
    CHECK(value("taps").toInt() == taps);
    CHECK(filter.designInfo().value(QStringLiteral("tapCount")).toInt() == taps);
    // Switched on again with the template given: that one.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}, {QStringLiteral("fromTemplate"), true},
                      {QStringLiteral("pass1"), 1000.0}, {QStringLiteral("stop1"), 2000.0}});
    CHECK(value("stop1").toDouble() == 2000.0);
    CHECK(designed("order").toInt() == 8);
    filter.setRecipe({{QStringLiteral("fromTemplate"), false}});
    CHECK(value("order").toInt() == 8);
    CHECK_NEAR(value("frequency1").toDouble(), 1087.8339627761857, 1e-9, 0.0); // −3 dB, as buttord gives it
    CHECK(filter.designInfo().value(QStringLiteral("text")).toString() == QStringLiteral("Passe-bas Butterworth d'ordre 8, 1,088 kHz"));
}

TEST_CASE("Filter controller: tried on a signal, before and after, then into the session", "[app]")
{
    Session session;
    SignalListModel *list = session.signalList();
    const qint64 id = list->addSignal(twoTones(20000.0, 20000, 2), QStringLiteral("Ajouter"));
    REQUIRE(id > 0);
    FilterController filter(&session);
    filter.addToSession(); // no signal chosen yet
    CHECK(filter.lastError().contains(QStringLiteral("Choisissez")));
    // A signal chosen, no filter yet: nothing to add.
    filter.setInputId(id);
    filter.addToSession();
    CHECK(filter.lastError().startsWith(QStringLiteral("Aucun filtre : le signal resterait le même.")));
    filter.setInputId(0);
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}});

    // The signal's rate taken by the design; the preview of its first channel.
    filter.setInputId(id);
    CHECK(filter.inputInfo().value(QStringLiteral("name")).toString() == QStringLiteral("Deux tons"));
    CHECK(filter.recipe().value(QStringLiteral("sampleRate")).toDouble() == 20000.0);
    CHECK(!filter.ready());
    filter.waitForIdle();
    REQUIRE(filter.ready());
    REQUIRE(filter.hasPreview());
    const model::SignalPtr preview = filter.previewSignal();
    REQUIRE(preview->channelCount() == 2);
    CHECK(preview->channels[0].name == "Avant");
    CHECK(preview->channels[1].name == "Après");
    CHECK(preview->frameCount() == 20000);
    CHECK(filter.previewNote().isEmpty()); // all of it
    CHECK(filter.previewSession()->signalList()->signalById(FilterController::previewId()) == preview);
    // 1 kHz low-pass: the 100 Hz stays (0,707 V RMS), the 5 kHz goes.
    CHECK_NEAR(rms(preview->channel(0)), 1.0, 0.01, 0.0);
    CHECK_NEAR(rms(preview->channel(1)), std::sqrt(0.5), 0.01, 0.0);
    CHECK(session.signalList()->count() == 1); // the preview is apart

    // The other channel.
    filter.setChannel(1);
    CHECK(filter.channel() == 1);
    filter.waitForIdle();
    REQUIRE(filter.hasPreview());
    CHECK_NEAR(rms(filter.previewSignal()->channel(1)), 0.5 * std::sqrt(0.5), 0.01, 0.0);
    filter.setChannel(7); // no such channel: the last one
    CHECK(filter.channel() == 1);

    // Another filter: the preview follows.
    filter.setRecipe({{QStringLiteral("response"), QStringLiteral("highpass")}});
    filter.waitForIdle();
    CHECK_NEAR(rms(filter.previewSignal()->channel(1)), 0.5 * std::sqrt(0.5), 0.01, 0.0);
    CHECK(filter.previewSignal()->channel(1)[100] != filter.previewSignal()->channel(0)[100]);
    filter.setRecipe({{QStringLiteral("response"), QStringLiteral("lowpass")}});

    // Into the session: every channel filtered, undone in one step.
    qint64 addedId = 0;
    QObject::connect(&filter, &FilterController::added, [&addedId](qint64 newId) { addedId = newId; });
    filter.addToSession();
    CHECK(filter.adding());
    REQUIRE(settle([&] { return addedId != 0; }));
    CHECK(!filter.adding());
    const model::SignalPtr added = list->signalById(addedId);
    REQUIRE(added != nullptr);
    CHECK(added->channelCount() == 2);
    CHECK(added->frameCount() == 20000);
    CHECK(added->name == "Deux tons · passe-bas 1 kHz");
    CHECK(added->origin == model::Origin::Derived);
    CHECK(list->undoText().contains(QStringLiteral("Filtrer")));
    CHECK_NEAR(rms(added->channel(0)), std::sqrt(0.5), 0.01, 0.0);
    CHECK_NEAR(rms(added->channel(1)), 0.5 * std::sqrt(0.5), 0.01, 0.0);
    // The same name again: another one.
    addedId = 0;
    filter.addToSession();
    REQUIRE(settle([&] { return addedId != 0; }));
    CHECK(list->signalById(addedId)->name != added->name);
    list->undo();
    list->undo();
    CHECK(list->count() == 1);

    // The signal tried removed: no more preview.
    CHECK(list->removeSignal(id));
    filter.waitForIdle();
    CHECK(filter.inputInfo().isEmpty());
    CHECK(!filter.hasPreview());
    // Back (undone): it is tried again.
    list->undo();
    filter.waitForIdle();
    CHECK(filter.hasPreview());
}

TEST_CASE("Filter controller: a long signal previewed on its start, the module away computes nothing", "[app]")
{
    Session session;
    SignalListModel *list = session.signalList();
    const std::size_t n = FilterController::kPreviewFrames + 1000;
    const qint64 id = list->addSignal(twoTones(48000.0, n), QStringLiteral("Ajouter"));
    REQUIRE(id > 0);
    FilterController filter(&session);
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}}); // from the blank page
    filter.setActive(false);
    filter.setInputId(id);
    filter.waitForIdle();
    CHECK(!filter.hasPreview());
    CHECK(!filter.ready());
    filter.setActive(true);
    filter.waitForIdle();
    REQUIRE(filter.hasPreview());
    CHECK(filter.previewSignal()->frameCount() == FilterController::kPreviewFrames);
    CHECK(filter.previewNote().contains(QStringLiteral("premiers échantillons")));
    // A rate typed: the signal is not at it, the preview says so.
    filter.setRecipe({{QStringLiteral("rateOfSignal"), false}, {QStringLiteral("sampleRate"), 44100.0}});
    CHECK(filter.recipe().value(QStringLiteral("sampleRate")).toDouble() == 44100.0);
    filter.waitForIdle();
    CHECK(!filter.hasPreview());
    CHECK(filter.previewError().contains(QStringLiteral("44,1 kHz")));
    // Back to the signal's.
    filter.setRecipe({{QStringLiteral("rateOfSignal"), true}});
    CHECK(filter.recipe().value(QStringLiteral("sampleRate")).toDouble() == 48000.0);
}

TEST_CASE("Filter controller: the C for a microcontroller, shown and saved", "[app]")
{
    FilterController filter(nullptr);
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")}, {QStringLiteral("order"), 4}, {QStringLiteral("frequency1"), 1000.0}});
    const QVariantMap options{{QStringLiteral("format"), QStringLiteral("q15")}, {QStringLiteral("name"), QStringLiteral("pb")}};
    const QVariantMap code = filter.code(options);
    REQUIRE(code.value(QStringLiteral("ok")).toBool());
    CHECK(code.value(QStringLiteral("headerName")).toString() == QStringLiteral("pb.h"));
    CHECK(code.value(QStringLiteral("sourceName")).toString() == QStringLiteral("pb.c"));
    CHECK(code.value(QStringLiteral("format")).toString() == QStringLiteral("q15"));
    CHECK(code.value(QStringLiteral("scaling")).toString() == QStringLiteral("sum"));
    CHECK(code.value(QStringLiteral("fixed")).toBool());
    CHECK(code.value(QStringLiteral("header")).toString().contains(QStringLiteral("int16_t pb_echantillon(pb_t *f, int16_t x);")));
    CHECK(code.value(QStringLiteral("source")).toString().contains(QStringLiteral("int pb_autotest(void)")));
    CHECK(!code.value(QStringLiteral("notes")).toStringList().isEmpty());
    // Asked again: the same; another design: another code.
    CHECK(filter.code(options) == code);
    filter.setRecipe({{QStringLiteral("order"), 2}});
    CHECK(filter.code(options).value(QStringLiteral("header")).toString().contains(QStringLiteral("#define PB_SECTIONS 1")));
    // A name C cannot take.
    const QVariantMap wrong = filter.code({{QStringLiteral("name"), QStringLiteral("passe-bas")}});
    CHECK(!wrong.value(QStringLiteral("ok")).toBool());
    CHECK(wrong.value(QStringLiteral("error")).toString().contains(QStringLiteral("ne peut pas nommer le code")));
    // Saved: both files in the folder.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString written = filter.saveCode(QUrl::fromLocalFile(dir.path()), options);
    CHECK(written.endsWith(QStringLiteral("pb.c")));
    CHECK(QFileInfo::exists(dir.filePath(QStringLiteral("pb.h"))));
    QFile source(dir.filePath(QStringLiteral("pb.c")));
    REQUIRE(source.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(source.readAll()).contains(QStringLiteral("#include \"pb.h\"")));
    CHECK(filter.saveCode(QUrl::fromLocalFile(dir.filePath(QStringLiteral("absent"))), options).isEmpty());
    CHECK(filter.lastError().contains(QStringLiteral("n'existe pas")));
}
