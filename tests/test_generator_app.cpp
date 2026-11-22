// The module "Générateur": the recipe edited, its preview on a thread of its own, added to the session.
#include "qacq_test.h"

#include "qacq/app/audio_player.h"
#include "qacq/app/generator_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

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
} // namespace

TEST_CASE("Generator: the preview follows the recipe, then goes into the session in one step", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    const model::SignalPtr first = generator.previewSignal();
    REQUIRE(first != nullptr);
    CHECK(first->frameCount() == 48000);
    CHECK(first->origin == model::Origin::Generated);
    CHECK(generator.preview()->signalList()->count() == 1);
    CHECK(generator.preview()->signalList()->signalById(generator.previewId()) == first);
    CHECK(session.signalList()->count() == 0); // the preview is apart
    // Its analysis: the measurements of the sine.
    CHECK(settle([&] { return !generator.previewAnalysis()->busy() && generator.previewAnalysis()->hasResult(); }));
    CHECK(generator.previewAnalysis()->measures().size() == 1);

    // A noise added, then smaller; another rate and duration.
    const int noise = generator.addComponent(0, QStringLiteral("noise"));
    CHECK(noise == 1);
    CHECK(!generator.ready());
    generator.setComponent(0, noise, {{QStringLiteral("amplitude"), 0.01}});
    CHECK(generator.componentText(0, 1) == QStringLiteral("Bruit gaussien, écart type 10 mV"));
    generator.setSignalSettings({{QStringLiteral("sampleRate"), 8000.0},
                                 {QStringLiteral("duration"), 0.5},
                                 {QStringLiteral("name"), QStringLiteral("Essai")}});
    CHECK(generator.sampleCount() == 4000);
    CHECK(generator.sizeText().contains(QStringLiteral("chantillons")));
    CHECK(generator.problem().isEmpty());
    generator.waitForIdle();
    REQUIRE(generator.ready());
    const model::SignalPtr shown = generator.previewSignal();
    CHECK(shown->sampleRateHz == 8000.0 && shown->frameCount() == 4000);
    CHECK(shown->name == "Essai");
    CHECK(generator.warnings().isEmpty());

    // Into the session: the samples shared, the recipe with them, undone in one step.
    const qint64 id = generator.addToSession();
    REQUIRE(id > 0);
    const model::SignalPtr added = session.signalList()->signalById(id);
    REQUIRE(added != nullptr);
    CHECK(added->data == shown->data);
    CHECK(added->name == "Essai");
    CHECK(session.signalList()->undoText().contains(QStringLiteral("Générer « Essai »")));
    CHECK(generator.hasRecipe(id));
    const qint64 again = generator.addToSession();
    REQUIRE(again > 0);
    CHECK(session.signalList()->signalById(again)->name == "Essai 2");

    // Its recipe taken up again after other edits.
    generator.reset();
    CHECK(generator.currentRecipe().channels[0].components.size() == 1);
    REQUIRE(generator.takeUp(id));
    CHECK(generator.currentRecipe().sampleRateHz == 8000.0);
    CHECK(generator.currentRecipe().channels[0].components.size() == 2);
    session.signalList()->undo();
    CHECK(session.signalList()->count() == 1);
    CHECK(!generator.takeUp(12345));
    CHECK(!generator.lastError().isEmpty());
}

TEST_CASE("Generator: too large a recipe is refused before computing; the newest recipe wins", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    generator.setSignalSettings({{QStringLiteral("sampleRate"), 1e6}, {QStringLiteral("duration"), 100.0}});
    CHECK(!generator.problem().isEmpty());
    CHECK(generator.sizeText().isEmpty());
    generator.waitForIdle();
    CHECK(!generator.hasPreview());
    CHECK(!generator.ready());
    CHECK(generator.error() == generator.problem());
    CHECK(generator.preview()->signalList()->count() == 0);
    CHECK(generator.addToSession() == 0);
    CHECK(!generator.lastError().isEmpty());

    // Several changes in a row: one preview, of the last recipe.
    generator.setSignalSettings({{QStringLiteral("duration"), 0.01}});
    CHECK(generator.addChannel() == 1);
    generator.setChannel(1, {{QStringLiteral("name"), QStringLiteral("Courant")},
                             {QStringLiteral("unit"), QStringLiteral("A")},
                             {QStringLiteral("offset"), 1.0}});
    generator.waitForIdle();
    REQUIRE(generator.ready());
    REQUIRE(generator.previewSignal()->channelCount() == 2);
    CHECK(generator.previewSignal()->channels[1].name == "Courant");
    CHECK(generator.previewSignal()->channels[1].unit == "A");
    CHECK(generator.previewSignal()->frameCount() == 10000);

    // Remembered as text, read back the same.
    const QString text = generator.recipeText();
    GeneratorController other(&session);
    other.restoreRecipe(text);
    CHECK(other.recipeText() == text);
    other.restoreRecipe(QStringLiteral("pas du JSON"));
    CHECK(other.recipeText() == text);

    // Examples; another waveform keeps the amplitude; components moved; the last channel stays.
    CHECK(generator.examples().size() >= 8);
    generator.useExample(1);
    CHECK(generator.currentRecipe().name == "Sinus et bruit");
    generator.setComponent(0, 0, {{QStringLiteral("waveform"), QStringLiteral("square")}});
    CHECK(generator.currentRecipe().channels[0].components[0].waveform == analysis::WaveKind::Square);
    CHECK(generator.currentRecipe().channels[0].components[0].amplitude == 1.0);
    generator.moveComponent(0, 1, 0);
    CHECK(generator.currentRecipe().channels[0].components[0].waveform == analysis::WaveKind::Noise);
    generator.removeComponent(0, 0);
    CHECK(generator.currentRecipe().channels[0].components.size() == 1);
    generator.removeChannel(0);
    CHECK(generator.currentRecipe().channels.size() == 1);
    CHECK(generator.addComponent(0, QStringLiteral("inconnue")) == -1);
    generator.waitForIdle();
    CHECK(generator.ready());

    // A Fourier series: its first coefficients, a model of coefficients that keeps E.
    const int series = generator.addComponent(0, QStringLiteral("fourier"));
    REQUIRE(series == 1);
    generator.setComponent(0, series, {{QStringLiteral("variables"), QStringLiteral("E = 3")}});
    QVariantMap info = generator.seriesInfo(0, series);
    CHECK(info.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(info.value(QStringLiteral("count")).toLongLong() == 15);
    QVariantList rows = info.value(QStringLiteral("rows")).toList();
    REQUIRE(rows.size() == 12);
    CHECK(std::abs(rows[0].toMap().value(QStringLiteral("b")).toDouble() - 12.0 / std::numbers::pi) < 1e-12);
    CHECK(std::abs(rows[0].toMap().value(QStringLiteral("phase")).toDouble()) < 1e-12);
    generator.applySeriesModel(0, series, QStringLiteral("triangle"));
    CHECK(generator.currentRecipe().channels[0].components[1].variables == "E = 3");
    CHECK(generator.currentRecipe().channels[0].components[1].formulaB == "8*E/(pi^2*n^2)*sin(n*pi/2)");
    // The same coefficients in another form, written short, and back as they were.
    generator.setComponent(0, series, {{QStringLiteral("form"), QStringLiteral("cartesian")}});
    CHECK(generator.currentRecipe().channels[0].components[1].formulaB == "-4*E/(pi^2*n^2)*sin(n*pi/2)");
    generator.setComponent(0, series, {{QStringLiteral("form"), QStringLiteral("ab")}});
    CHECK(generator.currentRecipe().channels[0].components[1].formulaB == "8*E/(pi^2*n^2)*sin(n*pi/2)");
    generator.setComponent(0, series, {{QStringLiteral("bn"), QStringLiteral("4*D/n")}});
    info = generator.seriesInfo(0, series);
    CHECK(info.value(QStringLiteral("error")).toString().contains(QStringLiteral("Nom inconnu « D »")));
    generator.waitForIdle();
    CHECK(!generator.ready());
    CHECK(generator.error().contains(QStringLiteral("(Série de Fourier) — b")));
    generator.removeComponent(0, series);
    CHECK(generator.seriesInfo(0, 0).isEmpty()); // not a series
    generator.waitForIdle();
    CHECK(generator.ready());

    // A drawn motif: its points as text, typed back (or why not), a model, points from the editor.
    const int drawn = generator.addComponent(0, QStringLiteral("drawn"));
    REQUIRE(drawn == 1);
    const auto motif = [&generator] { return generator.currentRecipe().channels[0].components[1].motif; };
    CHECK(generator.motifText(0, drawn) == QStringLiteral("0 -1 ; 0,1 1 ; 0,5 1 ; 0,6 -1"));
    CHECK(generator.setMotifText(0, drawn, QStringLiteral("0 0 ; 0,5 1")).isEmpty());
    CHECK(motif().size() == 2);
    CHECK(generator.setMotifText(0, drawn, QStringLiteral("0 0 ; 2 1")).contains(QStringLiteral("entre 0 et 1")));
    CHECK(motif().size() == 2); // unchanged
    generator.applyMotifModel(0, drawn, QStringLiteral("stairs"));
    CHECK(motif().size() == 4);
    CHECK(generator.currentRecipe().channels[0].components[1].interpolation == analysis::MotifInterpolation::Steps);
    const QVariantList points{QVariant(QVariantList{0.5, -0.5}), QVariant(QVariantList{0.0, 0.5})};
    generator.setComponent(0, drawn, {{QStringLiteral("motif"), points}, {QStringLiteral("once"), true}});
    REQUIRE(motif().size() == 2);
    CHECK(motif()[0].x == 0.0); // in the order of x
    CHECK(generator.currentRecipe().channels[0].components[1].motifOnce);
    generator.waitForIdle();
    CHECK(generator.ready());
    generator.removeComponent(0, drawn);

    // A recurrence: what is wrong at once, its first values once generated, a model.
    const int recurrence = generator.addComponent(0, QStringLiteral("recurrence"));
    REQUIRE(recurrence == 1);
    generator.setComponent(0, recurrence, {{QStringLiteral("recurrence"), QStringLiteral("b*y[n-1]")}});
    QVariantMap steps = generator.recurrenceInfo(0, recurrence);
    CHECK(steps.value(QStringLiteral("error")).toString().contains(QStringLiteral("« b »")));
    generator.applyRecurrenceModel(0, recurrence, QStringLiteral("lowpass"));
    generator.waitForIdle();
    REQUIRE(generator.ready());
    steps = generator.recurrenceInfo(0, recurrence);
    CHECK(steps.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(steps.value(QStringLiteral("note")).toString().isEmpty());
    CHECK(steps.value(QStringLiteral("current")).toBool());
    const QVariantList values = steps.value(QStringLiteral("rows")).toList();
    REQUIRE(values.size() == 12);
    CHECK(values[0].toMap().value(QStringLiteral("x")).toDouble() == 1.0);
    CHECK(values[1].toMap().value(QStringLiteral("y")).toDouble() > values[0].toMap().value(QStringLiteral("y")).toDouble());
    CHECK(generator.recurrenceInfo(0, 0).isEmpty()); // not a recurrence
    generator.applyRecurrenceModel(0, recurrence, QStringLiteral("oscillator"));
    generator.setComponent(0, recurrence, {{QStringLiteral("input"), QStringLiteral("noise")}});
    CHECK(generator.recurrenceInfo(0, recurrence).value(QStringLiteral("note")).toString().contains(QStringLiteral("ne lit pas x[n]")));
    generator.removeComponent(0, recurrence);

    // Inactive (another module on screen): the changes wait, then are generated.
    generator.setActive(false);
    generator.setSignalSettings({{QStringLiteral("duration"), 0.02}});
    generator.waitForIdle();
    CHECK(!generator.ready());
    CHECK(generator.previewSignal()->frameCount() == 48000); // the example, 1 s at 48 kHz
    generator.setActive(true);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->frameCount() == 960);
}

TEST_CASE("Audio player: nothing to play is said, its settings stay within bounds", "[app]")
{
    // (Nothing is played here: a test does not make the PC sound.)
    AudioPlayer player;
    CHECK(!player.play(nullptr, 1, {0}));
    CHECK(player.error() == QStringLiteral("Pas de signal à écouter."));
    CHECK(!player.playing());
    Session session;
    CHECK(!player.play(&session, 999, {0}));
    CHECK(!player.preparing());
    player.setVolume(2.0);
    CHECK(player.volume() == 1.0);
    player.setVolume(-1.0);
    CHECK(player.volume() == 0.0);
    player.setLoop(true);
    player.setNormalized(false);
    CHECK(player.loop());
    CHECK(!player.normalized());
    player.stop();
    CHECK(player.sourceId() == 0);
}
