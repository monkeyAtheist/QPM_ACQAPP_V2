#include "qacq_test.h"

#include "qacq/analysis/analysis.h"
#include "qacq/analysis/curves.h"

#include <cmath>
#include <limits>
#include <numbers>

using namespace qacq::analysis;
using qacq::model::ChannelInfo;
using qacq::model::SampleData;
using qacq::model::Signal;

namespace
{
Signal twoChannels()
{
    // Voie 1: 1 kHz sine; voie 2: 1 kHz square-ish (fundamental + third harmonic at −20 dB), 48 kHz.
    auto data = std::make_shared<SampleData>();
    data->channels.assign(2, std::vector<double>(4800));
    for (std::size_t i = 0; i < 4800; ++i)
    {
        const double phase = 2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 48000.0;
        data->channels[0][i] = 0.5 * std::sin(phase);
        data->channels[1][i] = std::sin(phase) + 0.1 * std::sin(3.0 * phase);
    }
    Signal s;
    s.id = 3;
    s.name = "Essai";
    s.sampleRateHz = 48000.0;
    s.startSeconds = 2.0;
    s.channels = {{"Tension", "V"}, {"Courant", "A"}};
    s.data = data;
    return s;
}
} // namespace

TEST_CASE("curves: kinds, names, units, parts", "[analysis]")
{
    CurveKind k = CurveKind::Spectrum;
    REQUIRE(curveKindFromKey("cycleRms", k));
    CHECK(k == CurveKind::CycleRms);
    CHECK(curveKindKey(CurveKind::Source) == "source");
    CHECK(!curveKindFromKey("fft", k));
    CHECK(!isTimeCurve(CurveKind::Spectrum));
    CHECK(isTimeCurve(CurveKind::Source));

    const Signal s = twoChannels();
    CurveSpec c;
    c.kind = CurveKind::Integral;
    c.channel = 0;
    CHECK(curveName(c, s) == "\xE2\x88\xAB Tension");
    CHECK(curveUnit(c, s) == "V\xC2\xB7s");
    c.kind = CurveKind::Source;
    CHECK(curveName(c, s) == "Tension");
    CHECK(curveUnit(c, s) == "V");
    c.channel = 7;
    CHECK(curveName(c, s).empty());
    c.channel = 1;
    c.firstFrame = 4000;
    c.frameCount = 2000;
    CHECK((curvePart(c, s) == std::pair<std::size_t, std::size_t>(4000, 800)));
    c.firstFrame = 9000;
    CHECK(curvePart(c, s).second == 0);
}

TEST_CASE("curves: the list in the session file", "[analysis]")
{
    std::vector<CurveSpec> list(3);
    list[0] = {5, CurveKind::Spectrum, 1, true, false, 0, std::numeric_limits<std::size_t>::max()};
    list[1] = {6, CurveKind::Integral, 0, false, true, 100, 500};
    list[2] = {7, CurveKind::Source, 0, true, false, 0, std::numeric_limits<std::size_t>::max()};
    const std::vector<CurveSpec> back = curveSpecsFromJson(toJson(list));
    REQUIRE(back.size() == 3);
    CHECK(back[0].id == 5);
    CHECK(back[0].kind == CurveKind::Spectrum);
    CHECK(back[0].channel == 1);
    CHECK(back[1].removeMean);
    CHECK(!back[1].visible);
    CHECK(back[1].firstFrame == 100);
    CHECK(back[1].frameCount == 500);
    CHECK(back[2].frameCount == std::numeric_limits<std::size_t>::max());
    CHECK(back[1].samePlace(list[1]));
    CHECK(!back[1].samePlace(list[2]));

    // Bad entries left out; repeated or missing ids renumbered.
    const auto parsed = qacq::model::Json::parse(
        R"([{"id":2,"kind":"integral","channel":0},{"id":2,"kind":"derivative","channel":1},{"kind":"fft","channel":0},)"
        R"({"id":9,"kind":"source"},{"kind":"cycleRms","channel":1},"x"])");
    REQUIRE(parsed);
    const std::vector<CurveSpec> read = curveSpecsFromJson(*parsed);
    REQUIRE(read.size() == 3);
    CHECK(read[0].id == 2);
    CHECK(read[1].id != 2);
    CHECK(read[2].id != 0);
    CHECK(read[1].id != read[2].id);
    CHECK(curveSpecsFromJson(qacq::model::Json::object()).empty());
}

TEST_CASE("curves: computed over their part, kept as a signal", "[analysis]")
{
    const Signal s = twoChannels();
    CurveSpec integral{1, CurveKind::Integral, 0, true, false, 480, 960};
    const std::vector<double> y = computeTimeCurve(s, integral);
    const std::vector<double> expected = cumulativeIntegral(s.channel(0).subspan(480, 960), 48000.0);
    REQUIRE(y.size() == expected.size());
    CHECK(y == expected);
    CHECK(computeTimeCurve(s, {2, CurveKind::Source, 0}).empty());
    CHECK(computeTimeCurve(s, {3, CurveKind::Integral, 9}).empty());

    // Two curves: the source channel (whole) and the integral (a part): one signal over their union.
    TimeCurveSet set;
    set.signal = std::make_shared<const Signal>(s);
    TimeCurve source;
    source.id = 2;
    source.name = "Tension";
    source.unit = "V";
    source.values = set.signal->channel(0);
    auto computed = std::make_shared<const std::vector<double>>(y);
    TimeCurve area;
    area.id = 1;
    area.name = "\xE2\x88\xAB Tension";
    area.unit = "V\xC2\xB7s";
    area.firstFrame = 480;
    area.computed = computed;
    area.values = *computed;
    TimeCurve hidden = area;
    hidden.visible = false;
    set.curves = {source, area, hidden};
    const Derived d = signalFromCurves(set);
    REQUIRE(d.ok());
    const Signal &k = d.signal;
    CHECK(k.origin == qacq::model::Origin::Derived);
    REQUIRE(k.channelCount() == 2); // the hidden curve is left out
    CHECK(k.frameCount() == 4800);
    CHECK_NEAR(k.startSeconds, 2.0, 1e-12, 0.0);
    CHECK(std::isnan(k.channel(1)[0]));
    CHECK(k.channel(1)[480] == 0.0);
    CHECK(std::isnan(k.channel(1)[480 + 960]));
    CHECK(k.channels[1].unit == "V\xC2\xB7s");
    CHECK(qacq::model::validateSignal(k).empty());
    CHECK(k.provenance["operation"].toString() == "curves");

    set.curves = {hidden};
    CHECK(!signalFromCurves(set).ok());
}

TEST_CASE("analysis: spectra of the channels asked for only, every channel measured", "[analysis]")
{
    const Signal s = twoChannels();
    const std::vector<std::size_t> none;
    const AnalysisResult r0 = analyse(s, 0, 4800, AnalysisSettings{}, nullptr, &none);
    CHECK(r0.error.empty());
    CHECK(r0.curves.empty());
    CHECK(!r0.ok());
    REQUIRE(r0.measures.size() == 2);
    CHECK(r0.measures[1].tone.detected);

    const std::vector<std::size_t> second{1};
    const AnalysisResult r1 = analyse(s, 0, 4800, AnalysisSettings{}, nullptr, &second);
    REQUIRE(r1.ok());
    REQUIRE(r1.curves.size() == 1);
    REQUIRE(r1.curves[0].channels.size() == 1);
    CHECK(r1.curves[0].channels[0] == 1);
    CHECK(r1.curves[0].name == "Courant");
    // The harmonics of channel 1 are judged on its own curve (H3 at −20 dB stands out).
    REQUIRE(r1.measures[1].harmonicStandsOut.size() >= 3);
    CHECK(r1.measures[1].harmonicStandsOut[2]);

    // A zone too long for one FFT is no error when no spectrum is asked for.
    const AnalysisResult all = analyse(s, 0, 4800, AnalysisSettings{});
    CHECK(all.curves.size() == 2);
}
