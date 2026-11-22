// The module "Filtres": the filter designed, tried on a signal of the session (a preview before and after, on
// a thread of its own), then the whole signal filtered into the session in one step.
#include "qacq_test.h"

#include "qacq/app/filter_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

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
    QVariantMap design = filter.designInfo();
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

    // A band whose upper edge is below the lower one gets one above.
    filter.setRecipe({{QStringLiteral("kind"), QStringLiteral("iir")},
                      {QStringLiteral("response"), QStringLiteral("bandpass")},
                      {QStringLiteral("frequency1"), 1000.0},
                      {QStringLiteral("frequency2"), 500.0}});
    CHECK(filter.recipe().value(QStringLiteral("frequency2")).toDouble() > 1000.0);
    CHECK(filter.designInfo().value(QStringLiteral("ok")).toBool());

    // Text, and back.
    const QString text = filter.recipeText();
    filter.reset();
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

TEST_CASE("Filter controller: tried on a signal, before and after, then into the session", "[app]")
{
    Session session;
    SignalListModel *list = session.signalList();
    const qint64 id = list->addSignal(twoTones(20000.0, 20000, 2), QStringLiteral("Ajouter"));
    REQUIRE(id > 0);
    FilterController filter(&session);
    filter.addToSession(); // no signal chosen yet
    CHECK(filter.lastError().contains(QStringLiteral("Choisissez")));

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
