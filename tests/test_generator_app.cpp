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
#include <memory>
#include <functional>
#include <numbers>
#include <vector>

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
    // A blank page: one channel without waveform, zero for 1 s at 48 kHz.
    CHECK(generator.blank());
    REQUIRE(generator.currentRecipe().channels.size() == 1);
    CHECK(generator.currentRecipe().channels[0].components.empty());
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->frameCount() == 48000);
    // The example of a sine.
    generator.useExample(0);
    CHECK(!generator.blank());
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

    // Its recipe taken up again after other edits (a blank page).
    generator.reset();
    CHECK(generator.blank());
    CHECK(generator.currentRecipe().channels[0].components.empty());
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

TEST_CASE("Generator: an operation between channels, a modulated carrier, a channel that is read", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    generator.reset();
    generator.setSignalSettings({{QStringLiteral("duration"), 0.01}});
    REQUIRE(generator.addComponent(0, QStringLiteral("sine")) == 0);
    REQUIRE(generator.addChannel() == 1);
    CHECK(generator.currentRecipe().channels[1].components.empty()); // a new channel is blank
    REQUIRE(generator.addComponent(1, QStringLiteral("sine")) == 0);
    generator.setChannel(0, {{QStringLiteral("name"), QStringLiteral("Tension")}});
    generator.setChannel(1, {{QStringLiteral("name"), QStringLiteral("Courant")}});
    REQUIRE(generator.addChannel() == 2);
    REQUIRE(generator.addComponent(2, QStringLiteral("sine")) == 0);

    // A new operation in the third channel: the sum of the other two, its models.
    const int sum = generator.addComponent(2, QStringLiteral("formula"));
    REQUIRE(sum == 1);
    CHECK(generator.currentRecipe().channels[2].components[1].expression == "v1 + v2");
    QVariantMap info = generator.operationInfo(2, sum);
    CHECK(info.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(info.value(QStringLiteral("note")).toString() == QStringLiteral("v1 : « Tension », v2 : « Courant »."));
    const QVariantList models = generator.operationModels(2);
    REQUIRE(models.size() == 9);
    CHECK(models[2].toMap().value(QStringLiteral("formula")).toString() == QStringLiteral("v1*v2"));
    generator.applyOperationModel(2, sum, QStringLiteral("product"));
    CHECK(generator.currentRecipe().channels[2].components[1].expression == "v1*v2");
    generator.removeComponent(2, 0); // the sine before it
    generator.waitForIdle();
    REQUIRE(generator.ready());
    const auto &data = generator.previewSignal()->data->channels;
    REQUIRE(data.size() == 3);
    CHECK(std::abs(data[2][37] - data[0][37] * data[1][37]) < 1e-12);

    // Wrong at once; what x reads.
    generator.setComponent(2, 0, {{QStringLiteral("expression"), QStringLiteral("v3*2")}});
    CHECK(generator.operationInfo(2, 0).value(QStringLiteral("error")).toString().contains(QStringLiteral("la voie elle-même")));
    generator.setComponent(2, 0, {{QStringLiteral("expression"), QStringLiteral("x^2")}});
    CHECK(generator.operationInfo(2, 0).value(QStringLiteral("caution")).toString().contains(QStringLiteral("x vaut 0")));
    CHECK(generator.operationInfo(0, 0).isEmpty()); // a sine

    // A sine made an operation: the sum of the other two.
    generator.setComponent(1, 0, {{QStringLiteral("waveform"), QStringLiteral("formula")}});
    CHECK(generator.currentRecipe().channels[1].components[0].expression == "v1 + v3");
    generator.setComponent(1, 0, {{QStringLiteral("waveform"), QStringLiteral("sine")}});

    // A modulated carrier of the channel 1, from its messages.
    const int carrier = generator.addComponent(1, QStringLiteral("modulation"));
    REQUIRE(carrier == 1);
    const QVariantList messages = generator.messageModels(1);
    REQUIRE(messages.size() == 5);
    CHECK(messages[3].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("v1"));
    generator.applyMessageModel(1, carrier, QStringLiteral("v1"));
    CHECK(generator.currentRecipe().channels[1].components[1].message == "v1");
    generator.setComponent(1, carrier, {{QStringLiteral("modulation"), QStringLiteral("fm")}, {QStringLiteral("deviation"), 500.0}});
    CHECK(generator.currentRecipe().channels[1].components[1].modulation == analysis::CarrierModulation::Fm);
    CHECK(generator.componentText(1, carrier).startsWith(QStringLiteral("Porteuse 10 kHz, 1 V crête, modulée en fréquence (Δf = 500 Hz) par v1")));
    generator.waitForIdle();
    CHECK(generator.ready());

    // The channel 1 is read: it stays, and says why.
    generator.removeChannel(0);
    CHECK(generator.currentRecipe().channels.size() == 3);
    CHECK(generator.lastError().startsWith(QStringLiteral("La voie 1 est lue par la voie 2")));
    // The third goes: nothing reads it.
    generator.removeChannel(2);
    CHECK(generator.currentRecipe().channels.size() == 2);
}

TEST_CASE("Generator: a signal of the session as a waveform, followed when it changes; channels left out", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    generator.reset();
    generator.waitForIdle();
    CHECK(generator.sources().isEmpty());
    CHECK(generator.messageSources().isEmpty());
    // Nothing in the session yet: the card says so.
    const int none = generator.addComponent(0, QStringLiteral("recorded"));
    REQUIRE(none == 0);
    CHECK(generator.currentRecipe().channels[0].components[0].sourceId == 0);
    CHECK(generator.sourceInfo(0, none).value(QStringLiteral("error")).toString().startsWith(QStringLiteral("Aucun signal dans la session")));
    REQUIRE(generator.addComponent(0, QStringLiteral("sine")) == 1);
    CHECK(generator.sourceInfo(0, 1).isEmpty()); // a sine
    generator.removeComponent(0, 1);
    generator.removeComponent(0, none);
    CHECK(generator.blank());

    // A record of two channels, 0,1 s at 8 kHz.
    model::Signal record;
    record.name = "Voix";
    record.sampleRateHz = 8000.0;
    record.channels = {{"Micro", "V"}, {"Ligne", "V"}};
    auto data = std::make_shared<model::SampleData>();
    data->channels.assign(2, std::vector<double>(800, 0.5));
    for (std::size_t i = 0; i < 800; ++i) data->channels[0][i] = 0.001 * static_cast<double>(i);
    record.data = data;
    const qint64 id = session.signalList()->addSignal(record, QStringLiteral("Ajouter"));
    REQUIRE(id > 0);
    REQUIRE(generator.sources().size() == 1);
    CHECK(generator.sources().at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Voix"));
    CHECK(generator.sources().at(0).toMap().value(QStringLiteral("id")).toLongLong() == id);
    // Each of its channels, a message to modulate a carrier with.
    const QVariantList messages = generator.messageSources();
    REQUIRE(messages.size() == 2);
    CHECK(messages.at(0).toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Voix — Micro"));
    CHECK(messages.at(1).toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Voix — Ligne"));
    CHECK(messages.at(1).toMap().value(QStringLiteral("channel")).toInt() == 1);
    CHECK(messages.at(1).toMap().value(QStringLiteral("id")).toLongLong() == id);

    // Used on a blank page: it is its only channel, which takes its rate and its duration; what was done is said.
    int usedChannel = -1;
    QString said;
    QObject::connect(&generator, &GeneratorController::signalUsed, [&](int channel, const QString &text) {
        usedChannel = channel;
        said = text;
    });
    CHECK(generator.useSignal(id, QStringLiteral("wave")) == 0);
    CHECK(usedChannel == 0);
    CHECK(said.startsWith(QStringLiteral("Voie 1 « Micro » : le signal « Voix » (voie Micro), à sa fréquence d'échantillonnage et pour sa durée (8 kHz, 100 ms).")));
    CHECK(generator.currentRecipe().sampleRateHz == 8000.0);
    CHECK(generator.currentRecipe().durationSeconds == 0.1);
    REQUIRE(generator.currentRecipe().channels.size() == 1);
    REQUIRE(generator.currentRecipe().channels[0].components.size() == 1);
    CHECK(generator.currentRecipe().channels[0].name == "Micro");
    CHECK(generator.currentRecipe().channels[0].components[0].waveform == analysis::WaveKind::Recorded);
    CHECK(generator.currentRecipe().channels[0].components[0].sourceId == static_cast<std::uint64_t>(id));
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK((generator.previewSignal()->data->channels[0] == data->channels[0]));
    QVariantMap info = generator.sourceInfo(0, 0);
    CHECK(info.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(info.value(QStringLiteral("id")).toLongLong() == id);
    CHECK(info.value(QStringLiteral("channel")).toInt() == 0);
    CHECK(info.value(QStringLiteral("channels")).toStringList() == QStringList({QStringLiteral("Micro (V)"), QStringLiteral("Ligne (V)")}));
    CHECK(!info.value(QStringLiteral("resampled")).toBool());
    CHECK(!info.value(QStringLiteral("differs")).toBool());

    // Its other channel; at another rate it is resampled, and the recipe can take its timing back.
    generator.setComponent(0, 0, {{QStringLiteral("sourceChannel"), 1}});
    CHECK(generator.currentRecipe().channels[0].components[0].sourceChannel == 1);
    CHECK(generator.currentRecipe().channels[0].components[0].sourceChannelName == "Ligne");
    generator.setSignalSettings({{QStringLiteral("sampleRate"), 48000.0}});
    info = generator.sourceInfo(0, 0);
    CHECK(info.value(QStringLiteral("resampled")).toBool());
    CHECK(info.value(QStringLiteral("differs")).toBool());
    generator.adoptSourceTiming(0, 0);
    CHECK(generator.currentRecipe().sampleRateHz == 8000.0);
    CHECK(generator.currentRecipe().durationSeconds == 0.1);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->data->channels[0][10] == 0.5);

    // The record changes in the session: the recipe follows its names, the preview is made again.
    CHECK(session.signalList()->removeChannel(id, 0)); // "Ligne" is now its first channel
    CHECK(!generator.ready());
    CHECK(generator.currentRecipe().channels[0].components[0].sourceChannel == 0);
    CHECK(generator.currentRecipe().channels[0].components[0].sourceChannelName == "Ligne");
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->data->channels[0][10] == 0.5);
    CHECK(session.signalList()->renameSignal(id, QStringLiteral("Parole")));
    CHECK(generator.currentRecipe().channels[0].components[0].sourceName == "Parole");
    CHECK(generator.componentText(0, 0) == QStringLiteral("Signal enregistré « Parole » (voie Ligne)"));
    // Gone: said, and no preview; back (undo): made again.
    CHECK(session.signalList()->removeSignal(id));
    generator.waitForIdle();
    CHECK(!generator.ready());
    CHECK(generator.error().contains(QStringLiteral("n'est pas dans la session")));
    CHECK(generator.sourceInfo(0, 0).value(QStringLiteral("error")).toString().startsWith(QStringLiteral("Aucun signal dans la session")));
    session.signalList()->undo();
    generator.waitForIdle();
    CHECK(generator.ready());

    // The message of a carrier: one channel more, the record scaled to a peak of 1 then the carrier it modulates.
    CHECK(generator.useSignal(id, QStringLiteral("fm")) == 1);
    REQUIRE(generator.currentRecipe().channels.size() == 2);
    REQUIRE(generator.currentRecipe().channels[1].components.size() == 2);
    CHECK(generator.currentRecipe().channels[1].name == "Parole modulé");
    CHECK(generator.currentRecipe().channels[1].components[0].waveform == analysis::WaveKind::Recorded);
    CHECK(generator.currentRecipe().channels[1].components[0].sourceNormalize);
    CHECK(generator.currentRecipe().channels[1].components[1].waveform == analysis::WaveKind::Modulation);
    CHECK(generator.currentRecipe().channels[1].components[1].modulation == analysis::CarrierModulation::Fm);
    CHECK(generator.currentRecipe().channels[1].components[1].message == "x");
    CHECK(generator.currentRecipe().channels[1].components[1].frequencyHz == 2000.0);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    REQUIRE(generator.previewSignal()->channelCount() == 2);
    // The carrier at 2 kHz, pushed 400 Hz up by a message of 1: 2,4 kHz.
    const std::vector<double> &carrier = generator.previewSignal()->data->channels[1];
    CHECK(std::abs(carrier[5] - std::cos(2.0 * std::numbers::pi * 2400.0 * 5.0 / 8000.0)) < 1e-9);

    // A channel left out of the signal: made, but neither in the preview nor in what is added.
    generator.setChannel(0, {{QStringLiteral("output"), false}});
    CHECK(!generator.currentRecipe().channels[0].output);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    REQUIRE(generator.previewSignal()->channelCount() == 1);
    CHECK(generator.previewSignal()->channels[0].name == "Parole modulé");
    CHECK(generator.sizeText().startsWith(QStringLiteral("800 échantillons · ")));
    CHECK(generator.sizeText().endsWith(QStringLiteral(" (plus 1 voie de calcul)")));
    const qint64 added = generator.addToSession();
    REQUIRE(added > 0);
    CHECK(session.signalList()->signalById(added)->channelCount() == 1);
    CHECK(generator.sources().size() == 2);
    generator.setChannel(1, {{QStringLiteral("output"), false}});
    generator.waitForIdle();
    CHECK(!generator.ready());
    CHECK(generator.problem().startsWith(QStringLiteral("Aucune voie n'est gardée dans le signal")));
    CHECK(generator.useSignal(999, QStringLiteral("wave")) == -1);
    CHECK(generator.lastError() == QStringLiteral("Ce signal n'existe plus."));
    CHECK(generator.useSignal(id, QStringLiteral("wave"), 3) == -1);
    CHECK(generator.lastError() == QStringLiteral("Ce signal n'a pas cette voie."));
}

TEST_CASE("Generator: a carrier's message chosen among the signals or the channels; a channel cleared, a blank one taken, removed", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    const analysis::GeneratorRecipe &r = generator.currentRecipe();
    // A record of one channel, 0,25 s at 8 kHz: a ramp up to 0,4 V.
    model::Signal record;
    record.name = "Rampe";
    record.sampleRateHz = 8000.0;
    record.channels = {{"Capteur", "V"}};
    auto data = std::make_shared<model::SampleData>();
    data->channels.assign(1, std::vector<double>(2000, 0.0));
    for (std::size_t i = 0; i < 2000; ++i) data->channels[0][i] = 0.0002 * static_cast<double>(i);
    record.data = data;
    const qint64 id = session.signalList()->addSignal(record, QStringLiteral("Ajouter"));
    REQUIRE(id > 0);

    // A carrier on the blank page; its message the record: placed above it, scaled to a peak of 1, read as x.
    REQUIRE(generator.addComponent(0, QStringLiteral("modulation")) == 0);
    const QVariantList models = generator.messageModels(0);
    REQUIRE(models.size() == 4); // a sine, a square, the waveforms above, the record
    const QVariantMap last = models.at(3).toMap();
    CHECK(last.value(QStringLiteral("name")).toString() == QStringLiteral("Le signal « Rampe »"));
    CHECK(last.value(QStringLiteral("key")).toString() == QStringLiteral("signal:%1:0").arg(id));
    generator.applyMessageModel(0, 0, last.value(QStringLiteral("key")).toString());
    REQUIRE(r.channels[0].components.size() == 2);
    CHECK(r.channels[0].components[0].waveform == analysis::WaveKind::Recorded);
    CHECK(r.channels[0].components[0].sourceNormalize);
    CHECK(r.channels[0].components[1].message == "x");
    CHECK(generator.operationInfo(0, 1).value(QStringLiteral("note")).toString() ==
          QStringLiteral("x : la forme d'onde au-dessus (Signal enregistré « Rampe » (voie Capteur), crête ramenée à 1 V). Elle module la "
                         "porteuse, qui la remplace dans la voie."));
    generator.applyMessageModel(0, 1, QStringLiteral("signal:999:0"));
    CHECK(generator.lastError() == QStringLiteral("Ce signal n'existe plus."));
    CHECK(r.channels[0].components.size() == 2);
    generator.waitForIdle();
    CHECK(generator.ready());

    // Cleared: blank again, its offset too.
    generator.setChannel(0, {{QStringLiteral("offset"), 0.5}});
    generator.clearChannel(0);
    CHECK(r.channels[0].components.empty());
    CHECK(r.channels[0].offset == 0.0);
    CHECK(generator.blank());

    // A sine in a second channel, the first one blank: a signal used takes the blank channel's place, the timing stays.
    REQUIRE(generator.addChannel() == 1);
    REQUIRE(generator.addComponent(1, QStringLiteral("sine")) == 0);
    CHECK(!generator.blank());
    CHECK(generator.useSignal(id, QStringLiteral("am")) == 0);
    REQUIRE(r.channels.size() == 2);
    CHECK(r.channels[0].name == "Rampe modulé");
    REQUIRE(r.channels[0].components.size() == 2);
    CHECK(r.channels[0].components[1].amIndex == 0.8);
    CHECK(r.sampleRateHz == 48000.0);
    // Removed: the sine stays alone; the last channel stays.
    generator.removeChannel(0);
    REQUIRE(r.channels.size() == 1);
    REQUIRE(r.channels[0].components.size() == 1);
    CHECK(r.channels[0].components[0].waveform == analysis::WaveKind::Sine);
    generator.removeChannel(0);
    CHECK(r.channels.size() == 1);
    generator.waitForIdle();
    CHECK(generator.ready());

    // The sine as the message of a carrier, read as it is: a channel of its own (v1), the timing kept; what was done is said.
    int usedChannel = -1;
    QString said;
    QObject::connect(&generator, &GeneratorController::signalUsed, [&](int channel, const QString &text) {
        usedChannel = channel;
        said = text;
    });
    CHECK(generator.modulateChannel(0, QStringLiteral("am")) == 1);
    CHECK(usedChannel == 1);
    CHECK(said.startsWith(QStringLiteral("Voie 2 « Voie 2 modulée » : la voie 1 « Voie 2 », lue telle quelle (v1), est le message d'une "
                                         "porteuse de 10 kHz modulée en amplitude")));
    REQUIRE(r.channels.size() == 2);
    REQUIRE(r.channels[1].components.size() == 1);
    CHECK(r.channels[1].components[0].message == "v1");
    CHECK(r.sampleRateHz == 48000.0);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->channelCount() == 2);
    // The message the carrier reads is not removed; the carrier is. No such channel, an empty one: said.
    generator.removeChannel(0);
    CHECK(generator.lastError() ==
          QStringLiteral("La voie 1 est lue par la voie 2 (v1 dans son opération ou son message) : changez d'abord sa formule."));
    CHECK(r.channels.size() == 2);
    generator.removeChannel(1);
    CHECK(r.channels.size() == 1);
    CHECK(generator.modulateChannel(3, QStringLiteral("am")) == -1);
    CHECK(generator.lastError() == QStringLiteral("Cette voie n'existe plus."));
    generator.clearChannel(0);
    CHECK(generator.blank());
    CHECK(generator.modulateChannel(0, QStringLiteral("am")) == -1);
    CHECK(generator.lastError() == QStringLiteral("La voie 1 « Voie 2 » est vide : rien pour moduler une porteuse."));
    CHECK(usedChannel == 1);
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

TEST_CASE("Generator: a digital frame, as built, its lines as channels, the signal framed on it", "[app]")
{
    Session session;
    GeneratorController generator(&session);
    generator.reset();
    generator.waitForIdle();
    const analysis::GeneratorRecipe &r = generator.currentRecipe();
    REQUIRE(generator.addComponent(0, QStringLiteral("sine")) == 0);
    // The sine becomes a frame: "Bonjour" as bits, at a rate it fits the signal at.
    generator.setComponent(0, 0, {{QStringLiteral("waveform"), QStringLiteral("frame")}});
    REQUIRE(r.channels[0].components[0].waveform == analysis::WaveKind::Frame);
    CHECK(r.channels[0].components[0].frequencyHz == 100.0);
    QVariantMap info = generator.frameInfo(0, 0);
    CHECK(info.value(QStringLiteral("title")).toString() == QStringLiteral("Bits NRZ"));
    CHECK(!info.contains(QStringLiteral("error")));
    CHECK(info.value(QStringLiteral("lines")).toStringList() == QStringList({QStringLiteral("Données"), QStringLiteral("Horloge")}));
    CHECK(info.value(QStringLiteral("lineKeys")).toStringList() == QStringList({QStringLiteral("data"), QStringLiteral("clock")}));
    CHECK(info.value(QStringLiteral("line")).toInt() == 0);
    CHECK(info.value(QStringLiteral("summary")).toString() == QStringLiteral("7 octets : 42 6F 6E 6A 6F 75 72"));
    CHECK(info.value(QStringLiteral("construction")).toString().startsWith(QStringLiteral("01000010 │ 01101111 │ ")));
    CHECK(info.value(QStringLiteral("legend")).toString().startsWith(QStringLiteral("NRZ : le niveau est le bit.")));
    CHECK(info.value(QStringLiteral("bits")).toDouble() == 56.0);
    CHECK(info.value(QStringLiteral("timing")).toString() == QStringLiteral("56 bits · 560 ms · 480 échantillons par bit"));
    CHECK(info.value(QStringLiteral("sampled")).toBool());
    CHECK(info.value(QStringLiteral("fits")).toBool());
    CHECK(info.value(QStringLiteral("missing")).toStringList() == QStringList({QStringLiteral("Horloge")}));
    CHECK(info.value(QStringLiteral("members")).toString().isEmpty());
    CHECK(info.value(QStringLiteral("realRate")).toDouble() == 0.0);
    CHECK(info.value(QStringLiteral("differs")).toBool());
    CHECK(info.value(QStringLiteral("warnings")).toStringList().isEmpty());
    CHECK(generator.frameInfo(0, 5).isEmpty());
    generator.waitForIdle();
    CHECK(generator.ready());
    CHECK(generator.warnings().isEmpty());
    CHECK(generator.componentText(0, 0) ==
          QStringLiteral("Bits NRZ à 100 bit/s (7 octets : 42 6F 6E 6A 6F 75 72), de 0 V à 1 V, à partir de 100 ms"));

    // A UART: the same data, least significant bit first, one line; too fast for the signal as it is.
    generator.setComponent(0, 0, {{QStringLiteral("protocol"), QStringLiteral("uart")}, {QStringLiteral("frequency"), 115200.0}});
    CHECK(r.channels[0].components[0].frame.protocol == analysis::FrameProtocol::Uart);
    CHECK(r.channels[0].components[0].frame.lsbFirst);
    CHECK(r.channels[0].components[0].frame.data == "Bonjour");
    CHECK(r.channels[0].components[0].frameLine == "tx");
    info = generator.frameInfo(0, 0);
    CHECK(info.value(QStringLiteral("title")).toString() == QStringLiteral("UART 8N1"));
    CHECK(info.value(QStringLiteral("missing")).toStringList().isEmpty());
    CHECK(info.value(QStringLiteral("construction")).toString().startsWith(QStringLiteral("St 01000010 Sp │ St 11110110 Sp │ ")));
    CHECK(info.value(QStringLiteral("timing")).toString() == QStringLiteral("70 bits · 607,6 µs · 0,4167 échantillon par bit"));
    CHECK(!info.value(QStringLiteral("sampled")).toBool());
    generator.waitForIdle();
    REQUIRE(generator.warnings().size() == 1);
    CHECK(generator.warnings().at(0).contains(QStringLiteral("0,4167 échantillon par bit")));
    // Framed: ten samples a bit, two bits of rest around the frame.
    generator.adoptFrameTiming(0, 0);
    CHECK(r.sampleRateHz == 1152000.0);
    CHECK(std::abs(r.durationSeconds - 74.0 / 115200.0) < 1e-12);
    CHECK(std::abs(r.channels[0].components[0].eventSeconds - 2.0 / 115200.0) < 1e-15);
    info = generator.frameInfo(0, 0);
    CHECK(!info.value(QStringLiteral("differs")).toBool());
    CHECK(info.value(QStringLiteral("sampled")).toBool());
    CHECK(info.value(QStringLiteral("samplesPerBit")).toDouble() == 10.0);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.warnings().isEmpty());
    CHECK(generator.previewSignal()->frameCount() == 740);

    // What is wrong in its data is said at once, and stops the preview.
    generator.setComponent(0, 0, {{QStringLiteral("data"), QStringLiteral("0x4G")}});
    info = generator.frameInfo(0, 0);
    CHECK(info.value(QStringLiteral("error")).toString() == QStringLiteral("Données : « G » n'est pas un chiffre hexadécimal (0 à 9, A à F)."));
    CHECK(!info.contains(QStringLiteral("construction")));
    CHECK(info.value(QStringLiteral("lines")).toStringList().size() == 1);
    generator.waitForIdle();
    CHECK(!generator.ready());
    CHECK(generator.error().contains(QStringLiteral("chiffre hexadécimal")));

    // An I2C bus: its clock as a second channel; what is set on a line goes to the other.
    generator.setComponent(0, 0, {{QStringLiteral("protocol"), QStringLiteral("i2c")},
                                  {QStringLiteral("data"), QStringLiteral("0x01 0xA5")},
                                  {QStringLiteral("frequency"), 100000.0}});
    CHECK(!r.channels[0].components[0].frame.lsbFirst);
    CHECK(r.channels[0].components[0].frameLine == "sda");
    info = generator.frameInfo(0, 0);
    CHECK(info.value(QStringLiteral("title")).toString() == QStringLiteral("I2C, écriture à 0x48"));
    CHECK(info.value(QStringLiteral("construction")).toString() == QStringLiteral("S 1001000 W A │ 00000001 A │ 10100101 A │ P"));
    CHECK(info.value(QStringLiteral("missing")).toStringList() == QStringList({QStringLiteral("SCL")}));
    generator.addFrameLines(0, 0);
    REQUIRE(r.channels.size() == 2);
    CHECK(r.channels[0].name == "SDA");
    CHECK(r.channels[1].name == "SCL");
    CHECK(r.channels[1].components[0].frameLine == "scl");
    CHECK(r.channels[0].components[0].frameGroup != 0);
    info = generator.frameInfo(1, 0);
    CHECK(info.value(QStringLiteral("line")).toInt() == 1);
    CHECK(info.value(QStringLiteral("members")).toString() == QStringLiteral("SDA (voie 1) · SCL (voie 2)"));
    CHECK(info.value(QStringLiteral("missing")).toStringList().isEmpty());
    generator.setComponent(1, 0, {{QStringLiteral("i2cAddress"), 0x20}, {QStringLiteral("amplitude"), 3.3}, {QStringLiteral("low"), 0.1}});
    CHECK(r.channels[0].components[0].frame.i2cAddress == 0x20);
    CHECK(r.channels[0].components[0].amplitude == 3.3);
    CHECK(r.channels[0].components[0].frameLow == 0.1);
    CHECK(r.channels[0].components[0].frameLine == "sda");
    CHECK(generator.frameInfo(0, 0).value(QStringLiteral("title")).toString() == QStringLiteral("I2C, écriture à 0x20"));
    generator.adoptFrameTiming(1, 0);
    CHECK(r.sampleRateHz == 2e6);
    CHECK(std::abs(r.channels[0].components[0].eventSeconds - 2e-5) < 1e-15);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->channelCount() == 2);
    CHECK(generator.previewSignal()->frameCount() == 660);

    // Another protocol, chosen from the clock: it stays a clock, the data line a data line; the lines it lacks.
    generator.setComponent(1, 0, {{QStringLiteral("protocol"), QStringLiteral("spi")}});
    CHECK(r.channels[1].components[0].frameLine == "sclk");
    CHECK(r.channels[0].components[0].frameLine == "mosi");
    CHECK(r.channels[0].components[0].frame.protocol == analysis::FrameProtocol::Spi);
    CHECK(generator.frameInfo(0, 0).value(QStringLiteral("missing")).toStringList() ==
          QStringList({QStringLiteral("MISO"), QStringLiteral("CS")}));
    generator.addFrameLines(1, 0);
    REQUIRE(r.channels.size() == 4);
    CHECK(r.channels[2].name == "MISO");
    CHECK(r.channels[3].name == "CS");
    CHECK(generator.frameInfo(3, 0).value(QStringLiteral("members")).toString() ==
          QStringLiteral("MOSI (voie 1) · SCLK (voie 2) · MISO (voie 3) · CS (voie 4)"));
    // The line of a card is its own; its group cannot be set from outside.
    const std::uint64_t group = r.channels[0].components[0].frameGroup;
    generator.setComponent(3, 0, {{QStringLiteral("line"), QStringLiteral("miso")}, {QStringLiteral("group"), 77}});
    CHECK(r.channels[3].components[0].frameLine == "miso");
    CHECK(r.channels[3].components[0].frameGroup == group);
    CHECK(r.channels[0].components[0].frameLine == "mosi");
    generator.setComponent(3, 0, {{QStringLiteral("line"), QStringLiteral("cs")}, {QStringLiteral("reply"), QStringLiteral("0x00 0xEF")}});
    CHECK(r.channels[2].components[0].frame.reply == "0x00 0xEF");
    CHECK(generator.frameInfo(0, 0).value(QStringLiteral("construction")).toString() ==
          QStringLiteral("MOSI  00000001 │ 10100101\nMISO  00000000 │ 11101111"));

    // USB: the bit rate of its speed.
    generator.setComponent(0, 0, {{QStringLiteral("protocol"), QStringLiteral("usb")}});
    info = generator.frameInfo(0, 0);
    CHECK(info.value(QStringLiteral("title")).toString() == QStringLiteral("USB bas débit, jeton SETUP"));
    CHECK(info.value(QStringLiteral("realRate")).toDouble() == 1.5e6);
    CHECK(r.channels[0].components[0].frameLine == "dp");
    CHECK(r.channels[2].components[0].frameLine == "dm");
    generator.adoptFrameTiming(0, 0, true);
    CHECK(r.channels[0].components[0].frequencyHz == 1.5e6);
    CHECK(r.channels[1].components[0].frequencyHz == 1.5e6);
    CHECK(r.sampleRateHz == 15e6);
    CHECK(generator.frameInfo(0, 0).value(QStringLiteral("realRate")).toDouble() == 0.0);
    generator.waitForIdle();
    REQUIRE(generator.ready());
    CHECK(generator.previewSignal()->frameCount() == 390);

    // No room for the lines of a bus: said.
    generator.reset();
    REQUIRE(generator.addComponent(0, QStringLiteral("sine")) == 0);
    while (generator.addChannel() >= 0 && r.channels.size() < 14) {}
    generator.setComponent(0, 0, {{QStringLiteral("waveform"), QStringLiteral("frame")}, {QStringLiteral("protocol"), QStringLiteral("spi")}});
    CHECK(r.channels[0].components[0].frame.protocol == analysis::FrameProtocol::Spi);
    generator.addFrameLines(0, 0);
    CHECK(r.channels.size() == 14);
    CHECK(generator.lastError() == QStringLiteral("Il faudrait 3 voies de plus pour les autres lignes : au plus 16 voies."));

    // The examples of frames: found in the menu, each generated without a warning.
    const QVariantList examples = generator.examples();
    int frames = 0;
    for (int i = 0; i < examples.size(); ++i)
    {
        const QString name = examples.at(i).toMap().value(QStringLiteral("name")).toString();
        if (!name.contains(QStringLiteral("UART")) && !name.contains(QStringLiteral("I2C")) && !name.contains(QStringLiteral("SPI")) &&
            !name.contains(QStringLiteral("USB")) && !name.contains(QStringLiteral("Manchester")))
            continue;
        ++frames;
        generator.useExample(i);
        generator.waitForIdle();
        CHECK(generator.ready());
        CHECK(generator.warnings().isEmpty());
        CHECK(!generator.frameInfo(0, 0).value(QStringLiteral("differs")).toBool()); // already framed
    }
    CHECK(frames == 5);
}
