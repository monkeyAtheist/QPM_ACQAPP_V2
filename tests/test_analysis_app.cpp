// The "Analyse" controller: computation on its own thread, latest request wins, display-only changes.
#include "qacq_test.h"

#include "qacq/app/analysis_controller.h"
#include "qacq/app/curves_controller.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/model/signal.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <functional>
#include <memory>
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

model::Signal toneSignal()
{
    model::Signal s;
    s.name = "Sinus";
    s.sampleRateHz = 48000.0;
    s.channels = {{"Entrée", "V"}};
    auto data = std::make_shared<model::SampleData>();
    data->channels.assign(1, std::vector<double>(4800));
    for (std::size_t i = 0; i < 4800; ++i)
        data->channels[0][i] = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 48000.0);
    s.data = data;
    return s;
}
} // namespace

TEST_CASE("Analysis controller: spectrum and measurements of the zone on screen", "[app]")
{
    Session session;
    const qint64 id = session.signalList()->addSignal(toneSignal(), QStringLiteral("Ajouter"));
    AnalysisController analysis(&session);
    analysis.setSignalId(id);
    REQUIRE(settle([&] { return analysis.hasResult() && !analysis.busy(); }));
    REQUIRE(analysis.result()->zoneFrames == 4800);
    const QVariantList curves = analysis.curves();
    REQUIRE(curves.size() == 1);
    CHECK(curves[0].toMap().value(QStringLiteral("levelUnit")).toString() == QStringLiteral("dBV"));
    CHECK(curves[0].toMap().value(QStringLiteral("peak")).toString().startsWith(QStringLiteral("1,00 kHz : −6,02 dBV")));
    const QVariantList measures = analysis.measures();
    REQUIRE(measures.size() == 1);
    const QVariantMap m = measures[0].toMap();
    CHECK(m.value(QStringLiteral("tone")).toBool());
    const QVariantList tone = m.value(QStringLiteral("toneValues")).toList();
    REQUIRE(!tone.isEmpty());
    CHECK(tone[0].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("1,000000 kHz"));
    // Oscilloscope measures and peaks for the panel.
    const QVariantList wave = m.value(QStringLiteral("wave")).toList();
    REQUIRE(wave.size() >= 4);
    CHECK(wave[3].toMap().value(QStringLiteral("value")).toString().startsWith(QStringLiteral("1,0000 kHz")));
    const QVariantList peaks = curves[0].toMap().value(QStringLiteral("peaks")).toList();
    REQUIRE(!peaks.isEmpty());
    CHECK(peaks[0].toMap().value(QStringLiteral("frequency")).toString().startsWith(QStringLiteral("1,000")));
    CHECK(analysis.summary().contains(QStringLiteral("FFT de 4")));
    CHECK(analysis.zoneText().contains(QStringLiteral("100,0 ms")));

    // Zones that follow each other quickly: the last one counts.
    analysis.setZone(0, 1000);
    analysis.setZone(100, 2000);
    analysis.setZone(200, 2400);
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result() && analysis.result()->zoneFrames == 2400; }));
    CHECK(analysis.result()->zoneFirstFrame == 200);

    // The level scale changes the picture only: nothing is computed again.
    analysis.setSettings({{QStringLiteral("scale"), QStringLiteral("linear")}});
    CHECK(!analysis.busy());
    CHECK(analysis.result()->scale == analysis::LevelScale::Linear);
    CHECK(analysis.curves()[0].toMap().value(QStringLiteral("levelUnit")).toString() == QStringLiteral("V"));
    analysis.setSettings({{QStringLiteral("logFrequency"), true}});
    CHECK(!analysis.busy());
    CHECK(analysis.settings().value(QStringLiteral("logFrequency")).toBool());

    // Welch: computed again.
    analysis.setSettings({{QStringLiteral("kind"), QStringLiteral("density")}, {QStringLiteral("segmentLength"), 512}});
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result()->kind == analysis::SpectrumKind::Density; }));
    CHECK(analysis.summary().startsWith(QStringLiteral("Welch")));
    CHECK(analysis.result()->fftSize == 512);

    // Remembered as text, and back.
    const QString saved = analysis.settingsText();
    AnalysisController other(&session);
    other.restoreSettings(saved);
    CHECK(other.settings() == analysis.settings());

    // Export.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("spectre é.csv"));
    REQUIRE(analysis.exportSpectrum(QUrl::fromLocalFile(path)));
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray head = file.read(64);
    CHECK(head.startsWith(QByteArray("\xEF\xBB\xBF" "Fr\xC3\xA9quence (Hz);Entr\xC3\xA9" "e (dBV/")));

    // The signal edited, then removed: the analysis follows.
    session.signalList()->setTiming(id, 24000.0, 0.0);
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result()->sampleRateHz == 24000.0; }));
    session.signalList()->removeSignal(id);
    REQUIRE(settle([&] { return !analysis.busy() && !analysis.hasResult(); }));
    CHECK(!analysis.exportSpectrum(QUrl::fromLocalFile(path)));
    CHECK(!analysis.lastError().isEmpty());
}

TEST_CASE("Computed curves: integral of a signal added after it, undone in one step", "[app]")
{
    Session session;
    SignalListModel *list = session.signalList();
    const qint64 id = list->addSignal(toneSignal(), QStringLiteral("Ajouter"));
    list->addSignal(toneSignal(), QStringLiteral("Ajouter"));
    const qint64 made = list->deriveSignal(id, {{QStringLiteral("operation"), QStringLiteral("integral")},
                                               {QStringLiteral("firstFrame"), 480},
                                               {QStringLiteral("frameCount"), 960},
                                               {QStringLiteral("keepSource"), true}});
    REQUIRE(made > 0);
    CHECK(list->rowOf(made) == 1); // just after its source
    const model::SignalPtr s = list->signalById(made);
    REQUIRE(s);
    CHECK(s->origin == model::Origin::Derived);
    CHECK(s->name == "Intégrale de « Sinus »");
    REQUIRE(s->channelCount() == 2);
    CHECK(s->channels[1].unit == "V\xC2\xB7s");
    CHECK(s->frameCount() == 960);
    CHECK(!s->createdUtc.empty());
    // 1 kHz, 0,5 V: the integral of 20 whole periods comes back to 0.
    CHECK(std::abs(s->channel(1).back()) < 2e-6);
    CHECK(list->undoText().contains(QStringLiteral("Intégrale")));
    list->undo();
    CHECK(list->count() == 2);

    // Failures are said, nothing is added.
    QString error;
    QObject::connect(list, &SignalListModel::errorOccurred, [&](const QString &message) { error = message; });
    CHECK(list->deriveSignal(id, {{QStringLiteral("operation"), QStringLiteral("fft")}}) == 0);
    CHECK(!error.isEmpty());
    CHECK(list->deriveSignal(id, {{QStringLiteral("operation"), QStringLiteral("cycleMean")},
                                  {QStringLiteral("frameCount"), 1}}) == 0);
    CHECK(list->count() == 2);

    // The new measurements reach the panel: first and last values, area, whole periods.
    AnalysisController analysis(&session);
    analysis.setSignalId(id);
    REQUIRE(settle([&] { return analysis.hasResult() && !analysis.busy(); }));
    const QVariantMap m = analysis.measures()[0].toMap();
    QStringList labels;
    for (const QVariant &v : m.value(QStringLiteral("values")).toList()) labels << v.toMap().value(QStringLiteral("label")).toString();
    for (const QVariant &v : m.value(QStringLiteral("wave")).toList()) labels << v.toMap().value(QStringLiteral("label")).toString();
    CHECK(labels.contains(QStringLiteral("Première / dernière valeur")));
    CHECK(labels.contains(QStringLiteral("Intégrale (aire sous la courbe)")));
    CHECK(labels.contains(QStringLiteral("Moyenne sur 98 périodes entières")));
    CHECK(labels.contains(QStringLiteral("Montée (10 → 90 %)")));
    analysis.setSettings({{QStringLiteral("edgeLow"), 0.2}});
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result() && analysis.result()->measures[0].wave.edgeLowFraction == 0.2; }));
    QStringList again;
    for (const QVariant &v : analysis.measures()[0].toMap().value(QStringLiteral("wave")).toList())
        again << v.toMap().value(QStringLiteral("label")).toString();
    CHECK(again.contains(QStringLiteral("Montée (20 → 80 %)")));
    CHECK(analysis.curves()[0].toMap().value(QStringLiteral("band")).toString().startsWith(QStringLiteral("Bande à −3 dB")));
}

TEST_CASE("Curves of the module: added on the signal chosen, computed, saved with the session", "[app]")
{
    Session session;
    SignalListModel *list = session.signalList();
    const qint64 id = list->addSignal(toneSignal(), QStringLiteral("Ajouter"));
    AnalysisController analysis(&session);
    CurvesController curves(&session, &analysis);
    analysis.setSignalId(id);
    curves.setSignalId(id);
    CHECK(!curves.hasTimeCurves());
    CHECK(!curves.hasSpectra());
    // No curve: the measurements, no spectrum.
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result() && !analysis.result()->measures.empty(); }));
    CHECK(analysis.result()->curves.empty());

    // The integral of the channel, with the channel itself to lay it over.
    CHECK(curves.add(QStringLiteral("integral"), {0}, {{QStringLiteral("withSources"), true}}) == 2);
    CHECK(curves.hasTimeCurves());
    REQUIRE(settle([&] {
        const auto set = curves.timeCurves();
        return !curves.busy() && set && set->curves.size() == 2 && !set->curves[1].values.empty();
    }));
    const auto set = curves.timeCurves();
    CHECK(set->curves[0].name == "Entrée");
    CHECK(set->curves[1].name == "\xE2\x88\xAB Entrée");
    CHECK(set->curves[1].unit == "V\xC2\xB7s");
    REQUIRE(set->curves[1].values.size() == 4800);
    const QVariantList shown = curves.curves();
    REQUIRE(shown.size() == 2);
    CHECK(shown[1].toMap().value(QStringLiteral("graph")).toString() == QStringLiteral("time"));
    CHECK(shown[1].toMap().value(QStringLiteral("colorIndex")).toInt() == 1);
    CHECK(shown[1].toMap().value(QStringLiteral("ready")).toBool());
    // Twice the same: nothing more; hidden then asked again: shown again.
    CHECK(curves.add(QStringLiteral("integral"), {0}) == 0);
    const int integralId = shown[1].toMap().value(QStringLiteral("id")).toInt();
    curves.setVisible(integralId, false);
    CHECK(curves.add(QStringLiteral("integral"), {0}) == 1);

    // A spectrum: the analysis computes that channel only.
    CHECK(curves.add(QStringLiteral("spectrum"), {0}) == 1);
    CHECK(curves.hasSpectra());
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result() && analysis.result()->curves.size() == 1; }));

    // Saved in the session: another controller on it finds the same list.
    CurvesController again(&session, nullptr);
    CHECK(again.specs().size() == 3);

    // Kept as a signal of the session (undoable).
    const qint64 kept = curves.keepAsSignal();
    REQUIRE(kept > 0);
    const model::SignalPtr k = list->signalById(kept);
    REQUIRE(k);
    CHECK(k->origin == model::Origin::Derived);
    CHECK(k->channelCount() == 2);

    // Removing: one curve, then a whole graph.
    curves.remove(integralId);
    CHECK(curves.specs().size() == 2);
    curves.clear(QStringLiteral("time"));
    CHECK(!curves.hasTimeCurves());
    CHECK(curves.hasSpectra());
    curves.clear();
    CHECK(curves.specs().empty());
    REQUIRE(settle([&] { return !analysis.busy() && analysis.result() && analysis.result()->curves.empty(); }));
    curves.waitForIdle();
}
