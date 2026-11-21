// Qt application layer: list model over the document, session files, import/export, settings.
#include "qacq_test.h"

#include "qacq/app/app_settings.h"
#include "qacq/app/format.h"
#include "qacq/app/qt_convert.h"
#include "qacq/app/session.h"
#include "qacq/app/signal_list_model.h"
#include "qacq/app/units.h"
#include "qacq/model/signal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

using namespace qacq;
using namespace qacq::app;

namespace
{
model::Signal makeSignal(const std::string &name, std::size_t channels = 2, std::size_t frames = 480, double rate = 48000.0)
{
    model::Signal s;
    s.name = name;
    s.sampleRateHz = rate;
    s.channels = model::defaultChannels(channels, "V");
    auto data = std::make_shared<model::SampleData>();
    data->channels.assign(channels, std::vector<double>(frames));
    for (std::size_t c = 0; c < channels; ++c)
        for (std::size_t f = 0; f < frames; ++f) data->channels[c][f] = std::sin(0.01 * static_cast<double>(f + 100 * c));
    s.data = data;
    return s;
}

struct Counter
{
    int inserted = 0, removed = 0, moved = 0, changed = 0, reset = 0, count = 0, history = 0, modified = 0, errors = 0;
};

void watch(SignalListModel &model, Counter &c)
{
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&c] { ++c.inserted; });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&c] { ++c.removed; });
    QObject::connect(&model, &QAbstractItemModel::rowsMoved, [&c] { ++c.moved; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&c] { ++c.changed; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&c] { ++c.reset; });
    QObject::connect(&model, &SignalListModel::countChanged, [&c] { ++c.count; });
    QObject::connect(&model, &SignalListModel::historyChanged, [&c] { ++c.history; });
    QObject::connect(&model, &SignalListModel::modifiedChanged, [&c] { ++c.modified; });
    QObject::connect(&model, &SignalListModel::errorOccurred, [&c] { ++c.errors; });
}

QString name(const SignalListModel &model, int row)
{
    return model.data(model.index(row), SignalListModel::NameRole).toString();
}
} // namespace

TEST_CASE("Conversions: JSON and QVariant, file names with accents", "[app]")
{
    model::Json j = model::Json::object();
    j["name"] = "Échelon";
    j["rate"] = 48000;
    j["on"] = true;
    j["list"] = model::Json::array();
    j["list"].push_back(1.5);
    j["list"].push_back("x");
    j["nested"]["k"] = 2;
    const QVariant v = jsonToVariant(j);
    const QVariantMap map = v.toMap();
    CHECK(map.value(QStringLiteral("name")).toString() == QStringLiteral("Échelon"));
    CHECK(map.value(QStringLiteral("rate")).toDouble() == 48000.0);
    CHECK(map.value(QStringLiteral("list")).toList().size() == 2);
    const model::Json back = variantToJson(v);
    CHECK(back["name"].toString() == "Échelon");
    CHECK(back["nested"]["k"].toNumber() == 2.0);
    CHECK(back["list"].at(1).toString() == "x");
    CHECK(variantToJson(QVariant(QStringList{QStringLiteral("a"), QStringLiteral("b")})).size() == 2);
    CHECK(variantToJson(QVariant()).isNull());

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString accented = dir.filePath(QStringLiteral("été à 50 Hz.txt"));
    {
        std::ofstream out(toPath(accented));
        out << "ok";
    }
    CHECK(QFileInfo::exists(accented));
    CHECK(fromPath(toPath(accented)) == accented);
}

TEST_CASE("Formatting: French decimal comma and engineering prefixes", "[app]")
{
    CHECK(formatQuantity(48000.0, QStringLiteral("Hz")) == QStringLiteral("48 kHz"));
    CHECK(formatQuantity(0.00125, QStringLiteral("s")) == QStringLiteral("1,25 ms"));
    CHECK(formatQuantity(3.3, QStringLiteral("V")) == QStringLiteral("3,3 V"));
    CHECK(formatQuantity(0.0, QStringLiteral("V")) == QStringLiteral("0 V"));
    CHECK(formatQuantity(2.5e6, QStringLiteral("Hz")) == QStringLiteral("2,5 MHz"));
    CHECK(formatQuantity(12.5, QStringLiteral("%")) == QStringLiteral("12,5 %"));
    CHECK(formatDuration(200.0) == QStringLiteral("3 min 20 s"));
    CHECK(formatDuration(3900.0) == QStringLiteral("1 h 05 min"));
    CHECK(formatBytes(1536) == QStringLiteral("1,5 Kio"));
    const QString summary = signalSummary(makeSignal("s", 2, 480000));
    CHECK(summary.startsWith(QStringLiteral("2 voies · 48 kHz · 10 s · ")));
    CHECK(summary.endsWith(QStringLiteral(" éch.")));
}

TEST_CASE("Signal list model: rows follow edits, undo and redo", "[app]")
{
    SignalListModel model;
    Counter c;
    watch(model, c);
    CHECK(model.rowCount() == 0);
    const qint64 a = model.addSignal(makeSignal("a"), QStringLiteral("Ajouter « a »"));
    const qint64 b = model.addSignal(makeSignal("b", 1, 100, 1000.0), QStringLiteral("Ajouter « b »"));
    REQUIRE(a > 0);
    REQUIRE(b > 0);
    CHECK(model.rowCount() == 2);
    CHECK(model.count() == 2);
    CHECK(c.inserted == 2);
    CHECK(c.count == 2);
    CHECK(model.isModified());
    CHECK(model.canUndo());
    CHECK(model.undoText() == QStringLiteral("Ajouter « b »"));
    CHECK(name(model, 1) == QStringLiteral("b"));
    CHECK(model.data(model.index(1), SignalListModel::SampleRateRole).toDouble() == 1000.0);
    CHECK(model.data(model.index(0), SignalListModel::ChannelNamesRole).toStringList() ==
          (QStringList{QStringLiteral("Voie 1"), QStringLiteral("Voie 2")}));
    CHECK(model.data(model.index(0), SignalListModel::IdRole).toLongLong() == a);
    CHECK(model.roleNames().value(SignalListModel::NameRole) == QByteArray("name"));

    CHECK(model.renameSignal(a, QStringLiteral("  alpha ")));
    CHECK(name(model, 0) == QStringLiteral("alpha"));
    CHECK(c.changed == 1);
    model.undo();
    CHECK(name(model, 0) == QStringLiteral("a"));
    CHECK(c.changed == 2);
    model.redo();
    CHECK(name(model, 0) == QStringLiteral("alpha"));

    CHECK(model.moveSignal(0, 1));
    CHECK(c.moved == 1);
    CHECK(name(model, 0) == QStringLiteral("b"));
    CHECK(name(model, 1) == QStringLiteral("alpha"));
    CHECK(model.rowOf(a) == 1);
    CHECK(model.idAt(0) == b);
    model.undo();
    CHECK(name(model, 0) == QStringLiteral("alpha"));

    CHECK(model.removeSignal(b));
    CHECK(c.removed == 1);
    CHECK(model.rowCount() == 1);
    model.undo();
    CHECK(model.rowCount() == 2);
    CHECK(name(model, 1) == QStringLiteral("b"));
    CHECK(c.reset == 0); // every change was described precisely

    CHECK(!model.renameSignal(a, QStringLiteral("   ")));
    CHECK(c.errors == 1);
    CHECK(!model.removeSignal(12345));
    CHECK(c.errors == 2);
}

TEST_CASE("Signal list model: metadata edits, merged timing steps, information", "[app]")
{
    SignalListModel model;
    const qint64 id = model.addSignal(makeSignal("m"), QStringLiteral("Ajouter"));
    CHECK(model.setNotes(id, QStringLiteral("sonde x10")));
    CHECK(model.setChannel(id, 1, QStringLiteral("Courant"), QStringLiteral("mA")));
    CHECK(!model.setChannel(id, 5, QStringLiteral("x"), QStringLiteral("V")));
    for (int k = 1; k <= 4; ++k) CHECK(model.setTiming(id, 48000.0 + k, 0.0));
    model.endMerge();
    CHECK(model.data(model.index(0), SignalListModel::SampleRateRole).toDouble() == 48004.0);
    model.undo(); // the four timing changes at once
    CHECK(model.data(model.index(0), SignalListModel::SampleRateRole).toDouble() == 48000.0);
    CHECK(model.data(model.index(0), SignalListModel::ChannelUnitsRole).toStringList().value(1) == QStringLiteral("mA"));
    const QVariantMap info = model.info(id);
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("m"));
    CHECK(info.value(QStringLiteral("notes")).toString() == QStringLiteral("sonde x10"));
    CHECK(info.value(QStringLiteral("channels")).toList().size() == 2);
    CHECK(info.value(QStringLiteral("rateText")).toString() == QStringLiteral("48 kHz"));
    CHECK(model.uniqueName(QStringLiteral("m")) == QStringLiteral("m 2"));
    CHECK(model.info(999).isEmpty());
}

TEST_CASE("Session: save, reopen, modified flag, title, module settings", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    AppSettings settings(dir.filePath(QStringLiteral("settings.ini")));
    Session session(&settings);
    CHECK(session.title() == QStringLiteral("Sans titre — QTacquisitionApp"));
    CHECK(!session.isModified());
    CHECK(!session.save()); // no file yet: the interface asks for one
    session.signalList()->addSignal(makeSignal("Tension"), QStringLiteral("Ajouter"));
    session.setModuleSettings(QStringLiteral("filters"), {{QStringLiteral("cutoffHz"), 1500.0}});
    CHECK(session.isModified());
    CHECK(session.title().contains(QStringLiteral("•")));

    const QString file = dir.filePath(QStringLiteral("mesure du banc"));
    CHECK(session.saveAs(QUrl::fromLocalFile(file)));
    CHECK(session.lastError().isEmpty());
    CHECK(session.fileName() == QStringLiteral("mesure du banc.qacq")); // suffix added
    CHECK(QFileInfo::exists(file + QStringLiteral(".qacq")));
    CHECK(!session.isModified());
    CHECK(session.title() == QStringLiteral("mesure du banc.qacq — QTacquisitionApp"));
    CHECK(settings.recentFiles().value(0).endsWith(QStringLiteral("mesure du banc.qacq")));

    session.newSession();
    CHECK(session.signalList()->count() == 0);
    CHECK(session.fileUrl().isEmpty());
    CHECK(session.moduleSettings(QStringLiteral("filters")).isEmpty());

    int replaced = 0;
    QObject::connect(&session, &Session::sessionReplaced, [&replaced] { ++replaced; });
    CHECK(session.open(QUrl::fromLocalFile(file + QStringLiteral(".qacq"))));
    CHECK(replaced == 1);
    CHECK(session.signalList()->count() == 1);
    CHECK(name(*session.signalList(), 0) == QStringLiteral("Tension"));
    CHECK(session.moduleSettings(QStringLiteral("filters")).value(QStringLiteral("cutoffHz")).toDouble() == 1500.0);
    CHECK(!session.isModified());
    CHECK(!session.signalList()->canUndo());

    session.signalList()->renameSignal(session.signalList()->idAt(0), QStringLiteral("U"));
    CHECK(session.isModified());
    CHECK(session.save());
    CHECK(!session.isModified());

    CHECK(!session.open(QUrl::fromLocalFile(dir.filePath(QStringLiteral("absent.qacq")))));
    CHECK(!session.lastError().isEmpty());
    CHECK(session.signalList()->count() == 1); // unchanged after a failure
    QFile junk(dir.filePath(QStringLiteral("junk.qacq")));
    REQUIRE(junk.open(QIODevice::WriteOnly));
    junk.write("not a session at all, just some text");
    junk.close();
    CHECK(!session.open(QUrl::fromLocalFile(junk.fileName())));
    CHECK(session.lastError().contains(QStringLiteral("pas un fichier de session")));
}

TEST_CASE("Session: import and export CSV and WAV", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Session session;
    {
        QFile csv(dir.filePath(QStringLiteral("banc.csv")));
        REQUIRE(csv.open(QIODevice::WriteOnly));
        csv.write("t;U (V);I (mA)\r\n0;1,5;2\r\n0,001;1,6;2,1\r\n0,002;1,7;2,2\r\n");
    }
    QVariantMap options;
    options.insert(QStringLiteral("decimalComma"), true);
    options.insert(QStringLiteral("timeColumn"), true);
    const qint64 id = session.importFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("banc.csv"))), options);
    REQUIRE(id > 0);
    const model::SignalPtr s = session.signalList()->signalById(id);
    REQUIRE(s != nullptr);
    CHECK(s->name == "banc");
    CHECK(s->origin == model::Origin::Imported);
    CHECK(s->sampleRateHz == 1000.0);
    CHECK(s->channels.at(1).unit == "mA");
    CHECK(!s->createdUtc.empty());
    CHECK(session.signalList()->undoText() == QStringLiteral("Importer « banc »"));

    // Same file again: a unique name.
    const qint64 again = session.importFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("banc.csv"))), options);
    CHECK(session.signalList()->signalById(again)->name == "banc 2");

    const QString wav = dir.filePath(QStringLiteral("banc.wav"));
    QVariantMap wavOptions;
    wavOptions.insert(QStringLiteral("encoding"), QStringLiteral("float32"));
    wavOptions.insert(QStringLiteral("normalize"), true);
    CHECK(session.exportSignal(id, QUrl::fromLocalFile(wav), wavOptions));
    CHECK(session.lastWarnings().size() == 0);
    const qint64 fromWav = session.importFile(QUrl::fromLocalFile(wav));
    REQUIRE(fromWav > 0);
    CHECK(session.signalList()->signalById(fromWav)->frameCount() == 3);
    CHECK(session.signalList()->signalById(fromWav)->sampleRateHz == 1000.0);

    const QString csvOut = dir.filePath(QStringLiteral("sortie.csv"));
    CHECK(session.exportSignal(id, QUrl::fromLocalFile(csvOut), {{QStringLiteral("decimalComma"), true}}));
    QFile written(csvOut);
    REQUIRE(written.open(QIODevice::ReadOnly));
    CHECK(written.readLine().startsWith("temps (s);U (V);I (mA)"));

    CHECK(session.importFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("image.png")))) == 0);
    CHECK(session.lastError().contains(QStringLiteral("png")));
    CHECK(!session.exportSignal(id, QUrl::fromLocalFile(dir.filePath(QStringLiteral("x.mp3")))));
    CHECK(!session.exportSignal(9999, QUrl::fromLocalFile(csvOut)));
}

TEST_CASE("Session: a text file is inspected, then imported as recognised", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Session session;
    {
        QFile csv(dir.filePath(QStringLiteral("tableur.csv")));
        REQUIRE(csv.open(QIODevice::WriteOnly));
        csv.write("Temps (ms);Tension (V);Courant (mA)\r\n0;1,5;-20\r\n0,5;1,6;-19,5\r\n1;1,7;-19\r\n1,5;1,8;-18\r\n");
    }
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("tableur.csv")));
    const QVariantMap seen = session.inspectImport(url);
    CHECK(seen.value(QStringLiteral("ok")).toBool());
    CHECK(seen.value(QStringLiteral("kind")).toString() == QStringLiteral("csv"));
    CHECK(seen.value(QStringLiteral("decimalComma")).toBool());
    CHECK(seen.value(QStringLiteral("timeColumn")).toBool());
    CHECK(seen.value(QStringLiteral("timeUnitSeconds")).toDouble() == 1e-3);
    CHECK(seen.value(QStringLiteral("columns")).toInt() == 3);
    CHECK(seen.value(QStringLiteral("titles")).toStringList().size() == 3);
    CHECK(seen.value(QStringLiteral("preview")).toStringList().size() == 5);
    // No option given: what was recognised.
    const qint64 id = session.importFile(url);
    REQUIRE(id > 0);
    const model::SignalPtr s = session.signalList()->signalById(id);
    CHECK(s->sampleRateHz == 2000.0);
    CHECK(s->channelCount() == 2);
    CHECK(s->channels.at(0).unit == "V");
    CHECK(s->data->channels.at(1).at(1) == -19.5);
    // Given options win.
    const qint64 plain = session.importFile(url, {{QStringLiteral("timeColumn"), false}, {QStringLiteral("sampleRateHz"), 10.0}});
    REQUIRE(plain > 0);
    CHECK(session.signalList()->signalById(plain)->channelCount() == 3);
    CHECK(session.signalList()->signalById(plain)->sampleRateHz == 10.0);
    CHECK(session.inspectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("absent.csv")))).value(QStringLiteral("ok")).toBool() == false);
    CHECK(session.inspectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("tableur.csv")))).value(QStringLiteral("lines")).toInt() == 4);
}

TEST_CASE("Session: an import on its own thread, followed, cancelled", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Session session;
    // About 6 MB of text (times in ms): long enough to be stopped on the way.
    const QString big = dir.filePath(QStringLiteral("long.csv"));
    {
        std::ofstream out(toPath(big), std::ios::binary);
        out << "t;v\n";
        for (int i = 0; i < 600000; ++i) out << i << ';' << (i % 100) << '\n';
    }
    const QVariantMap options{{QStringLiteral("timeColumn"), true},
                              {QStringLiteral("decimalComma"), false},
                              {QStringLiteral("timeUnitSeconds"), 1e-3}};
    std::vector<std::pair<qint64, bool>> finished;
    QObject::connect(&session, &Session::importFinished,
                     [&finished](qint64 id, bool cancelled) { finished.emplace_back(id, cancelled); });

    REQUIRE(session.startImport(QUrl::fromLocalFile(big), options));
    CHECK(session.importing());
    CHECK(session.importName() == QStringLiteral("long.csv"));
    CHECK(session.importProgress() == 0.0);
    CHECK(!session.startImport(QUrl::fromLocalFile(big), options)); // one at a time
    CHECK(session.lastError().contains(QStringLiteral("déjà")));
    session.waitForImport();
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].first > 0);
    CHECK(!finished[0].second);
    CHECK(!session.importing());
    const model::SignalPtr s = session.signalList()->signalById(finished[0].first);
    REQUIRE(s != nullptr);
    CHECK(s->frameCount() == 600000);
    CHECK(s->sampleRateHz == 1000.0);
    CHECK(session.lastError().isEmpty());

    // Cancelled on the way: nothing added.
    const int count = session.signalList()->count();
    REQUIRE(session.startImport(QUrl::fromLocalFile(big), options));
    session.cancelImport();
    session.waitForImport();
    REQUIRE(finished.size() == 2);
    CHECK(finished[1].first == 0);
    CHECK(finished[1].second);
    CHECK(session.signalList()->count() == count);

    // A file that cannot be read: finished with 0, the error said.
    REQUIRE(session.startImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("image.png")))));
    session.waitForImport();
    REQUIRE(finished.size() == 3);
    CHECK(finished[2].first == 0);
    CHECK(!finished[2].second);
    CHECK(session.lastError().contains(QStringLiteral("png")));

    // A new session while it reads: what it reads was for the one before.
    REQUIRE(session.startImport(QUrl::fromLocalFile(big), options));
    session.newSession();
    session.waitForImport();
    REQUIRE(finished.size() == 4);
    CHECK(finished[3].first == 0);
    CHECK(finished[3].second);
    CHECK(session.signalList()->count() == 0);
}

TEST_CASE("Settings: recent files, last folder, presets, persistence", "[app]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("settings.ini"));
    {
        AppSettings settings(ini);
        for (int k = 0; k < 12; ++k)
        {
            const QString f = dir.filePath(QStringLiteral("s%1.qacq").arg(k));
            QFile file(f);
            REQUIRE(file.open(QIODevice::WriteOnly));
            file.close();
            settings.addRecentFile(f);
        }
        settings.addRecentFile(dir.filePath(QStringLiteral("s5.qacq"))); // again: moves to the top
        const QStringList recent = settings.recentFiles();
        CHECK(recent.size() == AppSettings::kMaximumRecentFiles);
        CHECK(recent.value(0).endsWith(QStringLiteral("s5.qacq")));
        CHECK(recent.value(1).endsWith(QStringLiteral("s11.qacq")));
        CHECK(recent.count(recent.value(0)) == 1);
        QFile::remove(dir.filePath(QStringLiteral("s11.qacq")));
        CHECK(!settings.recentFiles().value(1).endsWith(QStringLiteral("s11.qacq"))); // vanished files are skipped
        CHECK(QDir(settings.lastDirectory()) == QDir(dir.path()));

        QVariantMap frame;
        frame.insert(QStringLiteral("sync"), QStringLiteral("AA 55"));
        frame.insert(QStringLiteral("channels"), 3);
        frame.insert(QStringLiteral("checksum"), QStringLiteral("crc16-ccitt"));
        settings.savePreset(QStringLiteral("frames"), QStringLiteral("Carte STM32"), frame);
        settings.savePreset(QStringLiteral("frames"), QStringLiteral("Arduino / 8 voies"), {{QStringLiteral("channels"), 8}});
        settings.setValue(QStringLiteral("serial/port"), QStringLiteral("COM3"));
        settings.setValue(QStringLiteral("serial/baud"), 115200);
        settings.sync();
    }
    AppSettings reopened(ini);
    CHECK(reopened.presetNames(QStringLiteral("frames")).size() == 2);
    CHECK(reopened.preset(QStringLiteral("frames"), QStringLiteral("Carte STM32")).value(QStringLiteral("channels")).toInt() == 3);
    CHECK(reopened.preset(QStringLiteral("frames"), QStringLiteral("Arduino / 8 voies")).value(QStringLiteral("channels")).toInt() == 8);
    CHECK(reopened.value(QStringLiteral("serial/port")).toString() == QStringLiteral("COM3"));
    CHECK(reopened.value(QStringLiteral("serial/baud")).toInt() == 115200);
    CHECK(reopened.value(QStringLiteral("absent"), 7).toInt() == 7);
    reopened.removePreset(QStringLiteral("frames"), QStringLiteral("Carte STM32"));
    CHECK(reopened.presetNames(QStringLiteral("frames")) == QStringList{QStringLiteral("Arduino / 8 voies")});
    reopened.clearRecentFiles();
    CHECK(reopened.recentFiles().isEmpty());
}

TEST_CASE("Units for QML: shown, typed, prefix kept for a bare number", "[app]")
{
    qacq::app::Units units;
    CHECK(units.format(0.001234, QStringLiteral("s")) == QStringLiteral("1,234 ms"));
    CHECK(units.compact(48000.0, QStringLiteral("Hz")) == QStringLiteral("48 kHz"));
    CHECK(units.compact(115200.0, QString()) == QStringLiteral("115200"));
    const QVariantMap ok = units.parse(QStringLiteral("2,5 kHz"), QStringLiteral("Hz"));
    CHECK(ok.value(QStringLiteral("ok")).toBool());
    CHECK(ok.value(QStringLiteral("value")).toDouble() == 2500.0);
    // The field shows "48 kHz": "44,1" typed over it means 44,1 kHz.
    const double factor = units.shownFactor(48000.0, QStringLiteral("Hz"));
    CHECK(factor == 1000.0);
    CHECK(units.parse(QStringLiteral("44,1"), QStringLiteral("Hz"), factor).value(QStringLiteral("value")).toDouble() == 44100.0);
    CHECK(units.shownFactor(0.0, QStringLiteral("V")) == 1.0);
    CHECK(units.shownFactor(5000.0, QStringLiteral("s")) == 1.0);
    const QVariantMap wrong = units.parse(QStringLiteral("3 V"), QStringLiteral("Hz"));
    CHECK(!wrong.value(QStringLiteral("ok")).toBool());
    CHECK(wrong.value(QStringLiteral("error")).toString().contains(QStringLiteral("Hz")));
    CHECK(units.count(480000).size() == 7); // "480 000", with the French group separator
}
