#include "qacq_test.h"

#include "qacq/io/decoder.h"
#include "qacq/model/csv.h"
#include "qacq/model/document.h"
#include "qacq/model/json.h"
#include "qacq/model/session_file.h"
#include "qacq/model/signal.h"
#include "qacq/model/wav.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace qacq::model;

namespace
{
std::shared_ptr<SampleData> rampData(std::size_t channels, std::size_t frames, double step = 0.25)
{
    auto data = std::make_shared<SampleData>();
    data->channels.assign(channels, std::vector<double>(frames));
    for (std::size_t c = 0; c < channels; ++c)
        for (std::size_t f = 0; f < frames; ++f)
            data->channels[c][f] = static_cast<double>(c) * 1000.0 + static_cast<double>(f) * step;
    return data;
}

Signal makeSignal(std::string name, std::size_t channels = 2, std::size_t frames = 100, double rate = 1000.0)
{
    Signal s;
    s.name = std::move(name);
    s.sampleRateHz = rate;
    s.channels = defaultChannels(channels, "V");
    s.data = rampData(channels, frames);
    return s;
}

bool sameBits(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b) || (std::isnan(a) && std::isnan(b));
}

std::filesystem::path tempFile(const std::string &name)
{
    return std::filesystem::temp_directory_path() / ("qacq_test_" + name);
}
} // namespace

// ------------------------------------------------------------------------------------------------ JSON
TEST_CASE("JSON: values, order of members, round trip", "[json]")
{
    Json j = Json::object();
    j["name"] = "Échelon";
    j["rate"] = 48000;
    j["gain"] = 0.1;
    j["tiny"] = 1e-7;
    j["flags"] = Json::array();
    j["flags"].push_back(true);
    j["flags"].push_back(nullptr);
    j["nested"]["deep"] = "x\"y\\z\n\t\x01";
    CHECK(j.members().front().key == "name");
    CHECK(j.members().back().key == "nested");
    const std::string compact = j.dump();
    CHECK(compact == "{\"name\":\"Échelon\",\"rate\":48000,\"gain\":0.1,\"tiny\":1e-07,\"flags\":[true,null],"
                     "\"nested\":{\"deep\":\"x\\\"y\\\\z\\n\\t\\u0001\"}}");
    for (int indent : {-1, 0, 2, 4})
    {
        const auto back = Json::parse(j.dump(indent));
        REQUIRE(back.has_value());
        CHECK(*back == j);
    }
    const Json &readOnly = j;
    CHECK(readOnly["missing"].isNull()); // reading through a const value inserts nothing
    CHECK(j["rate"].toNumber() == 48000.0);
    CHECK(j["name"].toNumber(-1.0) == -1.0);
    CHECK(j.contains("gain"));
    CHECK(j.erase("gain"));
    CHECK(!j.contains("gain"));
    CHECK(j.size() == 5);
    CHECK(j["flags"].at(0).toBool());
    CHECK(j["flags"].at(7).isNull());
    Json writable = Json::object();
    CHECK(writable["created"].isNull()); // the non-const form inserts the member
    CHECK(writable.size() == 1);
}

TEST_CASE("JSON: numbers are written at the shortest exact length", "[json]")
{
    CHECK(formatNumber(0.1) == "0.1");
    CHECK(formatNumber(100.0) == "100");
    CHECK(formatNumber(-0.0) == "0");
    CHECK(formatNumber(1e21) == "1e+21");
    CHECK(formatNumber(std::nan("")) == "null");
    CHECK(formatNumber(9007199254740992.0) == "9007199254740992");
    for (double v : {0.1, 1.0 / 3.0, 6.02214076e23, 1e-300, 4.9e-324, 123456.789, -2.5e-12})
    {
        const auto back = Json::parse(formatNumber(v));
        REQUIRE(back.has_value());
        CHECK(sameBits(back->toNumber(), v));
    }
    CHECK(Json::parse("1e400")->toNumber() == std::numeric_limits<double>::max());
    CHECK(Json::parse("-1e400")->toNumber() == -std::numeric_limits<double>::max());
    CHECK(Json::parse("1e-400")->toNumber() == 0.0);
    CHECK(Json::parse("1e-99999999999999999999")->toNumber() == 0.0);
    CHECK(Json::parse("12e99999999999999999999")->toNumber() == std::numeric_limits<double>::max());
    CHECK(Json::parse("0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                      "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                      "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                      "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001")
              ->toNumber() == 0.0);
    CHECK(Json::parse(std::string(400, '9'))->toNumber() == std::numeric_limits<double>::max());
}

TEST_CASE("JSON: unicode escapes and strict grammar", "[json]")
{
    CHECK(Json::parse("\"\\u00e9t\\u00E9\"")->toString() == "été");
    CHECK(Json::parse("\"\\ud83d\\ude00\"")->toString() == "\xF0\x9F\x98\x80");
    CHECK(Json::parse("\"\\/\"")->toString() == "/");
    CHECK(Json::parse(" [ 1 , 2 ] ")->size() == 2);
    for (const char *bad : {"01", "1.", ".5", "+1", "[1,]", "{\"a\":1,}", "'a'", "tru", "nul", "\"\\x\"", "\"a\nb\"", "\"abc",
                            "[1 2]", "{\"a\" 1}", "{a:1}", "\"\\ud83d\"", "\"\\ude00\"", "\"\\u12\"", "1 2", "", "[", "{"})
    {
        std::string error;
        CHECK(!Json::parse(bad, &error).has_value());
        CHECK(!error.empty());
    }
    std::string error;
    CHECK(!Json::parse("{\n  \"a\": [1,\n  2,]\n}", &error).has_value());
    CHECK(error.find("ligne 3") != std::string::npos);
    CHECK(Json::parse(std::string(256, '[') + std::string(256, ']')).has_value());
    CHECK(!Json::parse(std::string(300, '[') + std::string(300, ']')).has_value());
}

// ------------------------------------------------------------------------------------------------ model
TEST_CASE("Signals: validation, channel names, deinterleaving", "[model]")
{
    Signal s = makeSignal("a");
    CHECK(validateSignal(s).empty());
    CHECK(s.durationSeconds() == 0.1);
    CHECK(s.channel(1)[3] == 1000.75);
    CHECK(s.channel(5).empty());
    Signal bad = s;
    bad.sampleRateHz = 0.0;
    CHECK(!validateSignal(bad).empty());
    bad = s;
    bad.channels.pop_back();
    CHECK(!validateSignal(bad).empty());
    bad = s;
    auto ragged = std::make_shared<SampleData>(*s.data);
    ragged->channels[1].pop_back();
    bad.data = ragged;
    CHECK(!validateSignal(bad).empty());
    bad = s;
    auto breaks = std::make_shared<SampleData>(*s.data);
    breaks->discontinuities = {10, 5};
    bad.data = breaks;
    CHECK(!validateSignal(bad).empty());
    bad = makeSignal("iq", 3);
    bad.representation = Representation::IQ;
    CHECK(!validateSignal(bad).empty());

    const auto iq = defaultChannels(4, "", Representation::IQ);
    CHECK(iq[0].name == "I 1");
    CHECK(iq[3].name == "Q 2");
    CHECK(defaultChannels(2)[1].name == "Voie 2");

    qacq::io::SampleBlock block;
    block.channelCount = 3;
    block.values = {1, 2, 3, 4, 5, 6};
    block.discontinuities = {1};
    const auto data = deinterleave(block);
    CHECK(data->frameCount() == 2);
    CHECK(data->channels[2].at(1) == 6.0);
    CHECK(data->discontinuities == std::vector<std::size_t>{1});
}

TEST_CASE("Document: edits, undo, redo, labels and the modified flag", "[model]")
{
    Document doc;
    std::vector<DocumentChange> changes;
    doc.setListener([&](const DocumentChange &c) { changes.push_back(c); });
    CHECK(!doc.isModified());
    const auto a = doc.add(makeSignal("a"));
    const auto b = doc.add(makeSignal("b"));
    REQUIRE(a == 1);
    REQUIRE(b == 2);
    CHECK(doc.count() == 2);
    CHECK(doc.isModified());
    REQUIRE(changes.size() == 2);
    CHECK(changes[1].kind == DocumentChange::Kind::Inserted);
    CHECK(changes[1].first == 1);
    doc.setClean();
    CHECK(!doc.isModified());

    const auto dataBefore = doc.find(a)->data;
    CHECK(doc.rename(a, "alpha"));
    CHECK(doc.find(a)->name == "alpha");
    CHECK(doc.find(a)->data == dataBefore); // samples shared, not copied
    CHECK(changes.back().kind == DocumentChange::Kind::Updated);
    CHECK(changes.back().rows == std::vector<std::size_t>{0});
    CHECK(doc.undoText() == "Renommer « a »");
    CHECK(doc.isModified());
    CHECK(doc.undo());
    CHECK(doc.find(a)->name == "a");
    CHECK(!doc.isModified()); // back to the saved state
    CHECK(doc.redoText() == "Renommer « a »");
    CHECK(doc.redo());
    CHECK(doc.find(a)->name == "alpha");

    CHECK(doc.move(0, 1));
    CHECK(changes.back().kind == DocumentChange::Kind::Moved);
    CHECK(changes.back().first == 0);
    CHECK(changes.back().second == 1);
    CHECK(doc.at(0)->id == b);
    CHECK(doc.remove(a));
    CHECK(changes.back().kind == DocumentChange::Kind::Removed);
    CHECK(changes.back().first == 1);
    CHECK(doc.undoText() == "Supprimer « alpha »");
    CHECK(doc.undo());
    CHECK(changes.back().kind == DocumentChange::Kind::Inserted);
    CHECK(doc.count() == 2);
    CHECK(doc.undo()); // the move
    CHECK(doc.at(0)->id == a);

    // A new edit drops what could be redone.
    CHECK(doc.canRedo());
    doc.add(makeSignal("c"));
    CHECK(!doc.canRedo());
    CHECK(doc.uniqueName("c") == "c 2");
    CHECK(doc.uniqueName("d") == "d");

    std::string error;
    Signal broken = makeSignal("x");
    broken.sampleRateHz = -1.0;
    CHECK(doc.add(broken, "Ajouter", {}, &error) == 0);
    CHECK(!error.empty());
    CHECK(!doc.rename(999, "y", &error));
    CHECK(!doc.rename(a, "", &error));
}

TEST_CASE("Document: merged steps while a value is dragged", "[model]")
{
    Document doc;
    const auto id = doc.add(makeSignal("s"));
    const std::size_t steps = doc.historySteps();
    for (int k = 1; k <= 5; ++k)
    {
        Signal edited = *doc.find(id);
        edited.sampleRateHz = 1000.0 + k;
        CHECK(doc.replace(id, edited, "Fréquence", 42));
    }
    CHECK(doc.historySteps() == steps + 1);
    CHECK(doc.find(id)->sampleRateHz == 1005.0);
    doc.endMerge();
    Signal again = *doc.find(id);
    again.sampleRateHz = 2000.0;
    doc.replace(id, again, "Fréquence", 42);
    CHECK(doc.historySteps() == steps + 2);
    CHECK(doc.undo());
    CHECK(doc.find(id)->sampleRateHz == 1005.0);
    CHECK(doc.undo());
    CHECK(doc.find(id)->sampleRateHz == 1000.0);
}

TEST_CASE("Document: the history is bounded in memory and in steps", "[model]")
{
    Document doc;
    doc.setHistoryLimits(3 * 1024 * 1024, 200); // 3 MiB
    std::vector<std::uint64_t> ids;
    for (int k = 0; k < 4; ++k) ids.push_back(doc.add(makeSignal("big " + std::to_string(k), 1, 131072))); // 1 MiB each
    CHECK(doc.historyBytes() == 0); // everything is in the document
    for (auto id : ids) doc.remove(id);
    // The four removed signals would cost 4 MiB: the oldest steps go.
    CHECK(doc.historyBytes() <= 3u * 1024 * 1024);
    CHECK(doc.historyBytes() >= 2u * 1024 * 1024);
    int undone = 0;
    while (doc.undo()) ++undone;
    CHECK(undone < 8);
    CHECK(doc.count() >= 2);

    Document small;
    small.setHistoryLimits(Document::kDefaultHistoryBytes, 3);
    const auto id = small.add(makeSignal("s"));
    for (int k = 0; k < 10; ++k) small.rename(id, "n" + std::to_string(k));
    CHECK(small.historySteps() == 3);
    CHECK(small.isModified()); // the saved state was dropped from the history: still modified
}

TEST_CASE("Document: reset keeps or gives ids, describeChange", "[model]")
{
    Document doc;
    Signal a = makeSignal("a");
    a.id = 7;
    Signal b = makeSignal("b"); // id 0: one is given
    Signal c = makeSignal("c");
    c.id = 7; // duplicate: a new one is given
    doc.reset({std::make_shared<const Signal>(a), std::make_shared<const Signal>(b), std::make_shared<const Signal>(c)});
    CHECK(doc.at(0)->id == 7);
    CHECK(doc.at(1)->id == 8);
    CHECK(doc.at(2)->id == 9);
    CHECK(!doc.isModified());
    CHECK(!doc.canUndo());
    CHECK(doc.add(makeSignal("d")) == 10);

    const std::vector<SignalPtr> before = doc.signalList();
    auto x = std::make_shared<const Signal>(makeSignal("x"));
    CHECK(describeChange(before, before).kind == DocumentChange::Kind::None);
    std::vector<SignalPtr> up = before;
    std::rotate(up.begin() + 1, up.begin() + 3, up.begin() + 4); // item 3 moved to 1
    const auto moved = describeChange(before, up);
    CHECK(moved.kind == DocumentChange::Kind::Moved);
    CHECK(moved.first == 3);
    CHECK(moved.second == 1);
    std::vector<SignalPtr> mixed = {before[1], before[0], x};
    CHECK(describeChange(before, mixed).kind == DocumentChange::Kind::Reset);
}

// ------------------------------------------------------------------------------------------------ session
namespace
{
SessionContent sampleSession()
{
    SessionContent content;
    Signal a = makeSignal("Tension et courant", 2, 1000);
    a.id = 3;
    a.origin = Origin::Acquired;
    a.channels = {{"U", "V"}, {"I", "mA"}};
    auto data = std::make_shared<SampleData>(*a.data);
    data->channels[0].at(0) = std::numeric_limits<double>::quiet_NaN();
    data->channels[0].at(1) = std::numeric_limits<double>::infinity();
    data->channels[0].at(2) = -0.0;
    data->channels[0].at(3) = 4.9e-324;
    data->channels[0].at(4) = 0.1;
    data->discontinuities = {10, 500};
    a.data = data;
    a.startSeconds = 12.5;
    a.createdUtc = "2026-11-20T11:30:00Z";
    a.notes = "Banc n°2, « sonde » x10";
    a.provenance["source"] = "serial";
    a.provenance["port"] = "COM3";
    a.provenance["baud"] = 115200;
    Signal b = makeSignal("Comptes ADC", 1, 777);
    b.id = 9;
    b.representation = Representation::Real;
    auto counts = std::make_shared<SampleData>();
    counts->channels.assign(1, std::vector<double>(777));
    for (std::size_t i = 0; i < 777; ++i) counts->channels[0][i] = static_cast<double>(i % 4096);
    b.data = counts;
    Signal c = makeSignal("I/Q", 2, 64);
    c.id = 10;
    c.representation = Representation::IQ;
    c.origin = Origin::Derived;
    content.signalList = {std::make_shared<const Signal>(a), std::make_shared<const Signal>(b), std::make_shared<const Signal>(c)};
    content.settings["filters"]["cutoffHz"] = 1500;
    content.application = "QTacquisitionApp 2.0.0";
    content.savedUtc = "2026-11-20T12:00:00Z";
    return content;
}

void checkSame(const SessionContent &a, const SessionContent &b)
{
    REQUIRE(a.signalList.size() == b.signalList.size());
    for (std::size_t k = 0; k < a.signalList.size(); ++k)
    {
        const Signal &x = *a.signalList[k], &y = *b.signalList[k];
        CHECK(x.id == y.id);
        CHECK(x.name == y.name);
        CHECK(x.origin == y.origin);
        CHECK(x.representation == y.representation);
        CHECK(x.sampleRateHz == y.sampleRateHz);
        CHECK(x.startSeconds == y.startSeconds);
        CHECK(x.createdUtc == y.createdUtc);
        CHECK(x.notes == y.notes);
        CHECK(x.provenance == y.provenance);
        REQUIRE(x.channels.size() == y.channels.size());
        for (std::size_t c = 0; c < x.channels.size(); ++c)
        {
            CHECK(x.channels[c].name == y.channels[c].name);
            CHECK(x.channels[c].unit == y.channels[c].unit);
        }
        CHECK(x.data->discontinuities == y.data->discontinuities);
        bool identical = x.frameCount() == y.frameCount();
        for (std::size_t c = 0; c < x.channels.size() && identical; ++c)
            for (std::size_t f = 0; f < x.frameCount(); ++f)
                identical = identical && sameBits(x.data->channels[c][f], y.data->channels[c][f]);
        CHECK(identical);
    }
    CHECK(a.settings == b.settings);
}
} // namespace

TEST_CASE("Session file: everything comes back bit for bit", "[session]")
{
    const SessionContent content = sampleSession();
    std::stringstream file(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(writeSession(file, content).empty());
    const std::string bytes = file.str();
    CHECK(bytes.compare(0, 8, "QACQSESS") == 0);
    // The header is plain JSON: readable, and says how each channel is stored.
    std::uint64_t headerBytes = 0;
    for (int i = 7; i >= 0; --i) headerBytes = (headerBytes << 8) | static_cast<unsigned char>(bytes[12 + static_cast<std::size_t>(i)]);
    const auto header = Json::parse(bytes.substr(20, static_cast<std::size_t>(headerBytes)));
    REQUIRE(header.has_value());
    CHECK((*header)["signals"].at(0)["name"].toString() == "Tension et courant");
    CHECK((*header)["signals"].at(0)["data"].at(0)["encoding"].toString() == "f64le");
    CHECK((*header)["signals"].at(1)["data"].at(0)["encoding"].toString() == "f32le"); // integer counts
    file.seekg(0);
    std::string error;
    const auto back = readSession(file, &error);
    REQUIRE(back.has_value());
    CHECK(error.empty());
    checkSame(content, *back);
    CHECK(back->application == "QTacquisitionApp 2.0.0");
    CHECK(back->savedUtc == "2026-11-20T12:00:00Z");

    SessionWriteOptions wide;
    wide.float32WhenExact = false;
    std::stringstream file64(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(writeSession(file64, content, wide).empty());
    CHECK(file64.str().size() > bytes.size());
    file64.seekg(0);
    checkSame(content, *readSession(file64));
}

TEST_CASE("Session file: damage is detected and explained", "[session]")
{
    std::stringstream good(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(writeSession(good, sampleSession()).empty());
    const std::string bytes = good.str();
    auto read = [](const std::string &data) {
        std::stringstream s(data, std::ios::in | std::ios::binary);
        std::string error;
        const bool ok = readSession(s, &error).has_value();
        return ok ? std::string() : error;
    };
    std::string flipped = bytes;
    flipped[flipped.size() - 100] ^= 0x01;
    CHECK(read(flipped).find("endommagés") != std::string::npos);
    CHECK(read(bytes.substr(0, bytes.size() - 50)).find("tronqué") != std::string::npos);
    CHECK(read("hello world, this is not a session").find("Ce n'est pas") != std::string::npos);
    std::string future = bytes;
    future[8] = 2;
    CHECK(read(future).find("version plus récente") != std::string::npos);
    std::string badJson = bytes;
    badJson[20] = '[';
    badJson[21] = '}';
    CHECK(read(badJson).find("En-tête de session illisible") != std::string::npos);
    CHECK(read(bytes.substr(0, 10)).find("trop court") != std::string::npos);
}

TEST_CASE("Session file on disk: safe replacement of an existing file", "[session]")
{
    const auto path = tempFile("session.qacq");
    std::filesystem::remove(path);
    SessionContent content = sampleSession();
    REQUIRE(saveSession(path, content).empty());
    content.settings["filters"]["cutoffHz"] = 2500;
    REQUIRE(saveSession(path, content).empty()); // replaces it
    CHECK(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));
    std::string error;
    const auto back = loadSession(path, &error);
    REQUIRE(back.has_value());
    CHECK((back->settings)["filters"]["cutoffHz"].toNumber() == 2500.0);
    checkSame(content, *back);
    std::filesystem::remove(path);
    CHECK(!loadSession(path, &error).has_value());
    CHECK(error.find("Impossible d'ouvrir") != std::string::npos);
}

TEST_CASE("Session file: a document survives save and load with its ids", "[session][model]")
{
    Document doc;
    doc.add(makeSignal("a"));
    const auto b = doc.add(makeSignal("b"));
    doc.remove(1);
    SessionContent content;
    content.signalList = doc.signalList();
    std::stringstream file(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(writeSession(file, content).empty());
    file.seekg(0);
    Document loaded;
    loaded.reset(readSession(file)->signalList);
    CHECK(loaded.count() == 1);
    CHECK(loaded.at(0)->id == b);
    CHECK(loaded.add(makeSignal("c")) == b + 1);
}

TEST_CASE("Session file: speed on a long acquisition (2 x 2 million samples)", "[session]")
{
    Signal big = makeSignal("long", 2, 2'000'000, 48000.0);
    auto data = std::make_shared<SampleData>(*big.data);
    for (std::size_t f = 0; f < data->frameCount(); ++f) data->channels[0][f] = std::sin(0.001 * static_cast<double>(f));
    big.data = data;
    SessionContent content;
    content.signalList = {std::make_shared<const Signal>(big)};
    const auto path = tempFile("long.qacq");
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(saveSession(path, content).empty());
    const auto t1 = std::chrono::steady_clock::now();
    const auto back = loadSession(path);
    const auto t2 = std::chrono::steady_clock::now();
    REQUIRE(back.has_value());
    CHECK(back->signalList[0]->data->channels[0][1234567] == data->channels[0][1234567]);
    const double megabytes = static_cast<double>(std::filesystem::file_size(path)) / 1e6;
    std::printf("      %.0f MB (one channel stored as float32): saved in %.0f ms, loaded in %.0f ms\n", megabytes,
                std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
    CHECK(megabytes < 25.0); // 16 MB of doubles + 8 MB of floats
    std::filesystem::remove(path);
}

// ------------------------------------------------------------------------------------------------ CSV
TEST_CASE("CSV: export and import give back the exact values", "[csv]")
{
    Signal s = makeSignal("mesure", 2, 50, 250.0);
    s.channels = {{"U", "V"}, {"Courant, moyen", "mA"}};
    auto data = std::make_shared<SampleData>(*s.data);
    data->channels[1].at(7) = 1.0 / 3.0;
    data->channels[0].at(9) = -2.5e-12;
    s.data = data;
    s.startSeconds = 2.0;
    for (bool comma : {false, true})
    {
        std::stringstream file;
        CsvExportOptions out;
        out.decimalComma = comma;
        REQUIRE(writeCsv(file, s, out).empty());
        const std::string text = file.str();
        CHECK(text.rfind(comma ? "temps (s);U (V);Courant, moyen (mA)\r\n" : "temps (s),U (V),\"Courant, moyen (mA)\"\r\n", 0) == 0);
        file.seekg(0);
        CsvImportOptions in;
        in.decimalComma = comma;
        in.firstColumnIsTime = true;
        const ImportResult r = readCsv(file, "mesure", in);
        REQUIRE(r.ok());
        CHECK(r.warnings.empty());
        CHECK(r.signal.sampleRateHz == 250.0);
        CHECK(r.signal.startSeconds == 2.0);
        REQUIRE(r.signal.channels.size() == 2);
        CHECK(r.signal.channels[1].name == "Courant, moyen");
        CHECK(r.signal.channels[1].unit == "mA");
        bool exact = r.signal.frameCount() == 50;
        for (std::size_t c = 0; c < 2 && exact; ++c)
            for (std::size_t f = 0; f < 50; ++f) exact = exact && r.signal.data->channels[c][f] == s.data->channels[c][f];
        CHECK(exact);
    }
}

TEST_CASE("CSV: spreadsheet files, comments, irregular times, bad lines", "[csv]")
{
    std::stringstream excel("\xEF\xBB\xBF# export du banc\r\nt;U [V];I\r\n0;1,5;2\r\n0,001;1,6;2,1\r\n0,002;1,7;2,2\r\n0,0031;1,8;oops\r\n0,004;1,9;2,4\r\n");
    CsvImportOptions in;
    in.decimalComma = true;
    in.firstColumnIsTime = true;
    in.unit = "A";
    const ImportResult r = readCsv(excel, "banc", in);
    REQUIRE(r.ok());
    CHECK(r.signal.frameCount() == 4);
    CHECK(r.signal.sampleRateHz == 1000.0);
    CHECK(r.signal.channels[0].name == "U");
    CHECK(r.signal.channels[0].unit == "V");
    CHECK(r.signal.channels[1].unit == "A"); // default unit
    CHECK(r.signal.data->channels[0].at(3) == 1.9);
    CHECK(r.warnings.size() == 2); // one bad line, one irregular step (0.002 -> 0.004)
    CHECK(r.signal.data->discontinuities == std::vector<std::size_t>{3});

    std::stringstream bare("1 2\n3 4\n5 6");
    CsvImportOptions plain;
    plain.sampleRateHz = 8.0;
    const ImportResult p = readCsv(bare, "", plain);
    REQUIRE(p.ok());
    CHECK(p.signal.name == "Import CSV");
    CHECK(p.signal.frameCount() == 3); // the last line has no newline
    CHECK(p.signal.channels[1].name == "Voie 2");
    CHECK(p.signal.sampleRateHz == 8.0);

    std::stringstream empty("# rien\n\n");
    CHECK(!readCsv(empty, "x").ok());
    std::stringstream backwards("t,v\n0,1\n0,2\n0,3\n");
    CsvImportOptions timed;
    timed.firstColumnIsTime = true;
    CHECK(!readCsv(backwards, "x", timed).ok());
    CHECK(parseColumnTitle("Température (°C)").unit == "°C");
    CHECK(parseColumnTitle("x").unit.empty());
    CHECK(parseColumnTitle("(V)").name == "(V)");
}

TEST_CASE("CSV and WAV: a long import says how far it is, and stops when asked", "[csv]")
{
    // About 3 MB of text: read by blocks of 1 MB, the progress after each.
    std::string text = "t;v\n";
    for (int i = 0; i < 300000; ++i) text += std::to_string(i) + ";" + std::to_string(i % 100) + "\n";
    REQUIRE(text.size() > (2u << 20));
    CsvImportOptions in;
    in.firstColumnIsTime = true;
    std::atomic<int> permille{0};
    std::atomic<bool> stop{false};
    std::stringstream whole(text);
    const ImportResult r = readCsv(whole, "long", in, Progress{&stop, &permille});
    REQUIRE(r.ok());
    CHECK(!r.cancelled);
    CHECK(r.signal.frameCount() == 300000);
    CHECK(permille.load() == 1000);
    // Asked to stop: nothing kept, and said so.
    stop.store(true);
    permille.store(0);
    std::stringstream again(text);
    const ImportResult s = readCsv(again, "long", in, Progress{&stop, &permille});
    CHECK(s.cancelled);
    CHECK(!s.ok());
    CHECK(s.error == "Import annulé.");
    CHECK(s.signal.data == nullptr);
    CHECK(permille.load() < 1000);

    // A WAV file the same way.
    std::stringstream wav;
    REQUIRE(writeWav(wav, makeSignal("son", 1, 4800, 48000.0)).empty());
    wav.seekg(0);
    const ImportResult x = readWav(wav, "son", Progress{&stop, nullptr});
    CHECK(x.cancelled);
    CHECK(x.error == "Import annulé.");
    stop.store(false);
    wav.clear();
    wav.seekg(0);
    const ImportResult y = readWav(wav, "son", Progress{&stop, &permille});
    REQUIRE(y.ok());
    CHECK(y.signal.frameCount() == 4800);
    CHECK(permille.load() == 1000);
}

// ------------------------------------------------------------------------------------------------ WAV
TEST_CASE("WAV: canonical 16-bit header, round trips in every encoding", "[wav]")
{
    Signal s = makeSignal("son", 1, 4, 48000.0);
    auto data = std::make_shared<SampleData>();
    data->channels = {{0.0, 0.5, -1.0, 0.25}};
    s.data = data;
    s.channels = defaultChannels(1);
    std::stringstream file;
    REQUIRE(writeWav(file, s).empty());
    const std::string bytes = file.str();
    REQUIRE(bytes.size() == 44 + 8);
    const unsigned char expected[44] = {'R', 'I', 'F', 'F', 44, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                                        16,  0,   0,   0,   1,  0, 1, 0, 0x80, 0xBB, 0, 0, 0, 0x77, 1, 0,
                                        2,   0,   16,  0,   'd', 'a', 't', 'a', 8, 0, 0, 0};
    CHECK(std::memcmp(bytes.data(), expected, 44) == 0);
    CHECK(static_cast<unsigned char>(bytes[46]) == 0x00);
    CHECK(static_cast<unsigned char>(bytes[47]) == 0x40); // 0.5 -> 16384
    CHECK(static_cast<unsigned char>(bytes[49]) == 0x80); // -1 -> -32768

    Signal stereo = makeSignal("stéréo", 3, 300, 44100.0);
    auto values = std::make_shared<SampleData>();
    values->channels.assign(3, std::vector<double>(300));
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t f = 0; f < 300; ++f) values->channels[c][f] = std::sin(0.05 * static_cast<double>(f) + static_cast<double>(c)) * 0.9;
    stereo.data = values;
    struct Case
    {
        WavEncoding encoding;
        double tolerance;
    };
    for (const Case &c : {Case{WavEncoding::Pcm16, 1.0 / 32768.0}, Case{WavEncoding::Pcm24, 1.0 / 8388608.0},
                          Case{WavEncoding::Pcm32, 1e-9}, Case{WavEncoding::Float32, 1e-7}})
    {
        std::stringstream f;
        WavWriteOptions options;
        options.encoding = c.encoding;
        REQUIRE(writeWav(f, stereo, options).empty());
        f.seekg(0);
        const ImportResult r = readWav(f, "x");
        REQUIRE(r.ok());
        CHECK(r.signal.sampleRateHz == 44100.0);
        REQUIRE(r.signal.channels.size() == 3);
        REQUIRE(r.signal.frameCount() == 300);
        double worst = 0.0;
        for (std::size_t ch = 0; ch < 3; ++ch)
            for (std::size_t k = 0; k < 300; ++k)
                worst = std::max(worst, std::abs(r.signal.data->channels[ch][k] - values->channels[ch][k]));
        CHECK(worst <= c.tolerance);
    }
}

TEST_CASE("WAV: full scale, clipping, 8-bit files, truncated and foreign files", "[wav]")
{
    Signal volts = makeSignal("v", 1, 3, 8000.4);
    auto data = std::make_shared<SampleData>();
    data->channels = {{2.0, -5.0, 1.0}};
    volts.data = data;
    std::vector<std::string> warnings;
    std::stringstream clipped;
    REQUIRE(writeWav(clipped, volts, {}, &warnings).empty());
    CHECK(warnings.size() == 2); // rate rounded, two values clipped
    clipped.seekg(0);
    const ImportResult c = readWav(clipped, "v");
    CHECK(c.signal.sampleRateHz == 8000.0);
    CHECK(c.signal.data->channels[0].at(0) == 32767.0 / 32768.0);
    CHECK(c.signal.data->channels[0].at(1) == -1.0);

    WavWriteOptions normalize;
    normalize.normalize = true;
    std::stringstream scaled;
    REQUIRE(writeWav(scaled, volts, normalize).empty());
    scaled.seekg(0);
    const ImportResult n = readWav(scaled, "v");
    CHECK(n.signal.data->channels[0].at(1) == -1.0);
    CHECK(std::abs(n.signal.data->channels[0].at(0) - 0.4) < 1.0 / 32768.0);

    // 8-bit unsigned, 2 channels, built by hand; then the same with a data size larger than the file.
    std::string eight = std::string("RIFF") + std::string("\x28\0\0\0", 4) + "WAVEfmt " + std::string("\x10\0\0\0\x01\0\x02\0", 8) +
                        std::string("\x40\x1f\0\0\x80\x3e\0\0\x02\0\x08\0", 12) + "data" + std::string("\x04\0\0\0", 4) +
                        std::string("\x80\xFF\x00\xC0", 4);
    std::stringstream e(eight);
    const ImportResult r8 = readWav(e, "8");
    REQUIRE(r8.ok());
    CHECK(r8.signal.sampleRateHz == 8000.0);
    CHECK(r8.signal.data->channels[0].at(0) == 0.0);
    CHECK(r8.signal.data->channels[1].at(0) == 127.0 / 128.0);
    CHECK(r8.signal.data->channels[0].at(1) == -1.0);
    CHECK(r8.signal.data->channels[1].at(1) == 0.5);
    std::string longer = eight;
    longer[40] = '\x40'; // claims 64 bytes of data
    std::stringstream l(longer);
    const ImportResult rl = readWav(l, "8");
    REQUIRE(rl.ok());
    CHECK(rl.warnings.size() == 1);
    CHECK(rl.signal.frameCount() == 2);

    std::stringstream foreign("ID3\x03 this is an mp3");
    CHECK(!readWav(foreign, "x").ok());
    std::string rf64 = eight;
    rf64.replace(0, 4, "RF64");
    std::stringstream big(rf64);
    CHECK(readWav(big, "x").error.find("RF64") != std::string::npos);
}

TEST_CASE("CSV and WAV files on disk", "[csv][wav]")
{
    const auto csv = tempFile("export.csv");
    const auto wav = tempFile("export.wav");
    Signal s = makeSignal("disque", 2, 1000, 1000.0);
    REQUIRE(exportCsv(csv, s).empty());
    CsvImportOptions in;
    in.firstColumnIsTime = true;
    const ImportResult rc = importCsv(csv, in);
    REQUIRE(rc.ok());
    CHECK(rc.signal.name == "qacq_test_export");
    CHECK(rc.signal.provenance["file"].toString() == "qacq_test_export.csv");
    WavWriteOptions normalize;
    normalize.normalize = true;
    REQUIRE(exportWav(wav, s, normalize).empty());
    const ImportResult rw = importWav(wav);
    REQUIRE(rw.ok());
    CHECK(rw.signal.frameCount() == 1000);
    std::filesystem::remove(csv);
    std::filesystem::remove(wav);
}

TEST_CASE("CSV: the layout is recognised before import (decimal mark, header, time column)", "[csv]")
{
    using namespace qacq::model;
    {
        std::istringstream in("Temps (s);Tension (V);Courant (mA)\r\n0;1,5;-20\r\n0,001;1,6;-19,5\r\n0,002;1,7;-19\r\n");
        const CsvSniff s = sniffCsv(in);
        CHECK(s.decimalComma);
        CHECK(s.header);
        REQUIRE(s.titles.size() == 3);
        CHECK(s.titles[2] == "Courant (mA)");
        CHECK(s.columns == 3);
        CHECK(s.lines == 3);
        CHECK(s.firstColumnIsTime);
        CHECK(s.timeUnitSeconds == 1.0);
        CHECK(std::abs(s.timeStepSeconds - 0.001) < 1e-12);
        CHECK(s.preview.size() == 4);
        CHECK(s.preview[0] == "Temps (s);Tension (V);Courant (mA)");
    }
    {
        // Our own export, and a time column in milliseconds.
        std::istringstream in("time [ms],a,b\n0,1,2\n2,1.5,2\n4,1,2\n");
        const CsvSniff s = sniffCsv(in);
        CHECK(!s.decimalComma);
        CHECK(s.firstColumnIsTime);
        CHECK(s.timeUnitSeconds == 1e-3);
        CHECK(std::abs(s.timeStepSeconds - 0.002) < 1e-15);
    }
    {
        // No header: a regular first column is taken for time; two integers per line stay two columns.
        std::istringstream regular("0.0,1.0\n0.5,2\n1.0,3\n");
        const CsvSniff r = sniffCsv(regular);
        CHECK(!r.header);
        CHECK(r.firstColumnIsTime);
        CHECK(r.timeStepSeconds == 0.5);
        std::istringstream arduino("512,1023\n510,1020\n515,1019\n");
        const CsvSniff a = sniffCsv(arduino);
        CHECK(!a.decimalComma);
        CHECK(a.columns == 2);
        CHECK(!a.firstColumnIsTime);
        std::istringstream spaced("1,5 2,25\n1,75 -2,5e-1\n");
        CHECK(sniffCsv(spaced).decimalComma);
    }
    {
        // A first column that grows regularly but is named otherwise is a channel.
        std::istringstream in("Voie 1,Voie 2\n1,5\n2,6\n3,7\n");
        const CsvSniff s = sniffCsv(in);
        CHECK(s.header);
        CHECK(!s.firstColumnIsTime);
    }
    {
        // The import uses the unit of the time column.
        std::istringstream in("t (ms),x\n0,1\n2,2\n4,3\n");
        CsvImportOptions options;
        options.firstColumnIsTime = true;
        options.timeUnitSeconds = 1e-3;
        const ImportResult r = readCsv(in, "ms", options);
        REQUIRE(r.ok());
        CHECK(r.signal.sampleRateHz == 500.0);
        CHECK(r.signal.channelCount() == 1);
    }
}
