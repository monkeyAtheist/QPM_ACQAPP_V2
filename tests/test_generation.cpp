#include "qacq_test.h"

#include "qacq/analysis/generation.h"
#include "qacq/dsp/modulation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <span>
#include <string>
#include <vector>

using namespace qacq::analysis;
using qacq::dsp::BandLimit;
using qacq::dsp::ChirpLaw;
using qacq::dsp::NoiseDistribution;
using qacq::analysis::SeriesForm;
using Waveform = qacq::analysis::WaveKind;
using qacq::model::Json;

namespace
{
GeneratorComponent make(Waveform w, double amplitude, double frequency = 1000.0)
{
    GeneratorComponent c;
    c.waveform = w;
    c.amplitude = amplitude;
    c.frequencyHz = frequency;
    return c;
}

bool contains(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

double correlation(std::span<const double> a, std::span<const double> b)
{
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(a.size());
    mb /= static_cast<double>(b.size());
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        ab += (a[i] - ma) * (b[i] - mb);
        aa += (a[i] - ma) * (a[i] - ma);
        bb += (b[i] - mb) * (b[i] - mb);
    }
    return ab / std::sqrt(aa * bb);
}
} // namespace

TEST_CASE("generator: each channel is its offset plus the sum of its enabled components", "[analysis]")
{
    GeneratorRecipe r;
    r.name = "Essai";
    r.sampleRateHz = 1000.0;
    r.durationSeconds = 1.0;
    GeneratorChannel tension;
    tension.name = "Tension";
    tension.unit = "V";
    tension.offset = 0.5;
    GeneratorComponent off = make(Waveform::Square, 3.0, 5.0);
    off.enabled = false;
    tension.components = {make(Waveform::Sine, 1.0, 10.0), make(Waveform::Dc, 0.25), off};
    GeneratorChannel courant;
    courant.name = "Courant";
    courant.unit = "A";
    courant.offset = 2.0;
    r.channels = {tension, courant};

    REQUIRE(checkGeneratorRecipe(r).empty());
    CHECK(generatorSampleCount(r) == 1000);
    const GeneratorResult g = generateSignal(r);
    REQUIRE(g.ok());
    CHECK(!g.cancelled);
    CHECK(g.warnings.empty());
    const auto &s = g.signal;
    CHECK(s.name == "Essai");
    CHECK(s.origin == qacq::model::Origin::Generated);
    CHECK(s.sampleRateHz == 1000.0);
    REQUIRE(s.channelCount() == 2);
    CHECK(s.frameCount() == 1000);
    CHECK(s.channels[0].name == "Tension");
    CHECK(s.channels[1].unit == "A");
    CHECK(qacq::model::validateSignal(s).empty());
    const auto x = s.channel(0);
    CHECK_NEAR(x[0], 0.75, 0.0, 1e-12);
    CHECK_NEAR(x[25], 1.75, 0.0, 1e-12); // a quarter of a period of 10 Hz
    CHECK_NEAR(x[75], -0.25, 0.0, 1e-12);
    const auto y = s.channel(1);
    CHECK(y[0] == 2.0 && y[999] == 2.0);
    // The recipe goes with the signal, and a line per channel says what it holds.
    CHECK(s.provenance["generator"] == toJson(r));
    CHECK(contains(s.notes, "Généré à 1 kHz pendant 1 s."));
    CHECK(contains(s.notes, "Tension : Sinus 10 Hz, 1 V crête + Continu 250 mV ; décalage 500 mV."));
    CHECK(contains(s.notes, "Courant : Continu 2 A."));
}

TEST_CASE("generator: noise is the same for the same seed, and independent from one component to another", "[analysis]")
{
    GeneratorRecipe r;
    r.sampleRateHz = 10000.0;
    r.durationSeconds = 1.0;
    r.seed = 7;
    GeneratorChannel a;
    a.components = {make(Waveform::Noise, 1.0)};
    GeneratorChannel b = a;
    b.name = "Voie 2";
    r.channels = {a, b};
    const GeneratorResult first = generateSignal(r);
    const GeneratorResult again = generateSignal(r);
    REQUIRE(first.ok() && again.ok());
    CHECK(first.signal.data->channels == again.signal.data->channels);
    const auto x = first.signal.channel(0);
    const auto y = first.signal.channel(1);
    CHECK(std::abs(correlation(x, y)) < 0.05);
    double power = 0.0;
    for (double v : x) power += v * v;
    CHECK_REL(power / static_cast<double>(x.size()), 1.0, 0.05);
    r.seed = 8;
    const GeneratorResult other = generateSignal(r);
    REQUIRE(other.ok());
    CHECK(other.signal.data->channels[0] != first.signal.data->channels[0]);
}

TEST_CASE("generator: a Fourier series limited to a few harmonics", "[analysis]")
{
    GeneratorRecipe r;
    r.sampleRateHz = 1000.0;
    r.durationSeconds = 0.1;
    GeneratorComponent square = make(Waveform::Square, 1.0, 10.0);
    square.bandLimit = BandLimit::Fourier;
    square.harmonics = 3; // orders 1, 2 (zero for a square) and 3
    r.channels = {GeneratorChannel{}};
    r.channels[0].components = {square};
    const GeneratorResult g = generateSignal(r);
    REQUIRE(g.ok());
    // A quarter of a period: 4/π (sin π/2 + sin(3π/2) / 3).
    CHECK_NEAR(g.signal.channel(0)[25], 8.0 / (3.0 * std::numbers::pi), 0.0, 1e-9);
    CHECK(componentText(square, "V") == "Carré 10 Hz, 1 V crête, série de Fourier (3 harmoniques)");
}

TEST_CASE("generator: the recipe as JSON, read back within its bounds", "[analysis]")
{
    GeneratorRecipe r;
    r.name = "Deux voies";
    r.sampleRateHz = 44100.0;
    r.durationSeconds = 2.5;
    r.seed = 42;
    GeneratorComponent sweep = make(Waveform::Sweep, 0.5, 20.0);
    sweep.endFrequencyHz = 20000.0;
    sweep.sweepLaw = ChirpLaw::Logarithmic;
    sweep.phaseDegrees = 90.0;
    GeneratorComponent noise = make(Waveform::Noise, 0.01);
    noise.noise = NoiseDistribution::Uniform;
    noise.enabled = false;
    GeneratorComponent pulses = make(Waveform::Pulse, 2.0, 100.0);
    pulses.dutyCycle = 0.2;
    pulses.bandLimit = BandLimit::Fourier;
    pulses.harmonics = 12;
    GeneratorComponent step = make(Waveform::Step, -1.0);
    step.eventSeconds = 0.3;
    GeneratorChannel a;
    a.name = "Gauche";
    a.unit = "";
    a.offset = -0.1;
    a.components = {sweep, noise};
    GeneratorChannel b;
    b.name = "Droite";
    b.unit = "A";
    b.components = {pulses, step};
    r.channels = {a, b};
    const Json j = toJson(r);
    CHECK(toJson(generatorRecipeFromJson(j)) == j);
    const auto text = j.dump();
    const auto parsed = Json::parse(text);
    REQUIRE(parsed.has_value());
    CHECK(toJson(generatorRecipeFromJson(*parsed)) == j);

    // Nothing usable: the default recipe.
    const GeneratorRecipe empty = generatorRecipeFromJson(Json::object());
    CHECK(toJson(empty) == toJson(defaultGeneratorRecipe()));
    CHECK(empty.channels.size() == 1 && empty.channels[0].name == "Voie 1" && empty.channels[0].unit == "V");

    // Wrong values: defaults, or brought within the bounds; unknown waveforms dropped.
    const auto bad = Json::parse(R"({"sampleRate": -5, "duration": "long", "seed": -3, "channels": [
        {"name": "", "components": [{"waveform": "nope"}, {"waveform": "square", "duty": 3, "amplitude": 1e30,
         "harmonics": -4, "frequency": -10}]}, 12]})");
    REQUIRE(bad.has_value());
    const GeneratorRecipe b2 = generatorRecipeFromJson(*bad);
    CHECK(b2.sampleRateHz == 48000.0);
    CHECK(b2.durationSeconds == 1.0);
    CHECK(b2.seed == 0);
    REQUIRE(b2.channels.size() == 1);
    CHECK(b2.channels[0].name == "Voie 1");
    REQUIRE(b2.channels[0].components.size() == 1);
    const GeneratorComponent &c = b2.channels[0].components[0];
    CHECK(c.waveform == Waveform::Square);
    CHECK(c.dutyCycle == 0.999);
    CHECK(c.amplitude == 1e15);
    CHECK(c.harmonics == 0);
    CHECK(c.frequencyHz == 0.0);

    // At most 16 channels of 32 components.
    Json many = Json::object();
    Json channels = Json::array();
    for (int i = 0; i < 20; ++i)
    {
        Json ch = Json::object();
        Json list = Json::array();
        for (int k = 0; k < 40; ++k) list.push_back(toJson(make(Waveform::Sine, 1.0)));
        ch["components"] = std::move(list);
        channels.push_back(std::move(ch));
    }
    many["channels"] = std::move(channels);
    const GeneratorRecipe capped = generatorRecipeFromJson(many);
    CHECK(capped.channels.size() == kMaximumGeneratorChannels);
    CHECK(capped.channels[15].components.size() == kMaximumGeneratorComponents);
    CHECK(capped.channels[15].name == "Voie 16");
}

TEST_CASE("generator: what cannot be generated is said before, by channel and component after", "[analysis]")
{
    GeneratorRecipe r = defaultGeneratorRecipe();
    r.sampleRateHz = 1e6;
    r.durationSeconds = 60.0;
    CHECK(contains(checkGeneratorRecipe(r), "60 000 000 échantillons : au plus 50 000 000"));
    r.durationSeconds = 30.0;
    r.channels.push_back(defaultGeneratorChannel(1));
    CHECK(contains(checkGeneratorRecipe(r), "(30 000 000 par voie)"));
    const GeneratorResult tooBig = generateSignal(r);
    CHECK(!tooBig.ok() && !tooBig.cancelled && tooBig.signal.frameCount() == 0);
    r.durationSeconds = 0.0;
    CHECK(checkGeneratorRecipe(r) == "La durée doit être positive.");

    GeneratorRecipe w;
    w.sampleRateHz = 48000.0;
    w.durationSeconds = 0.01;
    GeneratorChannel tension;
    tension.name = "Tension";
    tension.components = {make(Waveform::Sine, 1.0, 30000.0)};
    w.channels = {tension};
    const GeneratorResult folded = generateSignal(w);
    REQUIRE(folded.ok());
    REQUIRE(folded.warnings.size() == 1);
    CHECK(folded.warnings[0].starts_with("Tension, composante 1 (Sinus) — La fréquence dépasse fs/2"));

    GeneratorComponent wrong = make(Waveform::Square, 1.0, 100.0);
    wrong.dutyCycle = 0.0; // only through the structure: JSON brings it within (0, 1)
    w.channels[0].components.push_back(wrong);
    const GeneratorResult failed = generateSignal(w);
    CHECK(!failed.ok());
    CHECK(failed.error.starts_with("Tension, composante 2 (Carré) — Le rapport cyclique"));
}

TEST_CASE("generator: progress by component, and a stop on request", "[analysis]")
{
    GeneratorRecipe r = defaultGeneratorRecipe();
    r.channels[0].components.push_back(make(Waveform::Triangle, 0.5, 300.0));
    r.channels.push_back(defaultGeneratorChannel(1));
    std::atomic<bool> cancelled{false};
    std::atomic<int> permille{0};
    const GeneratorResult g = generateSignal(r, {&cancelled, &permille});
    CHECK(g.ok());
    CHECK(permille.load() == 1000);
    cancelled = true;
    const GeneratorResult stopped = generateSignal(r, {&cancelled, &permille});
    CHECK(stopped.cancelled);
    CHECK(stopped.error == "Génération annulée.");
}

TEST_CASE("generator: usual values of a new component, texts, ready-made examples", "[analysis]")
{
    const GeneratorComponent sweep = defaultGeneratorComponent(Waveform::Sweep, 48000.0, 2.0);
    CHECK(sweep.frequencyHz == 20.0 && sweep.endFrequencyHz == 20000.0 && sweep.sweepLaw == ChirpLaw::Logarithmic);
    const GeneratorComponent slow = defaultGeneratorComponent(Waveform::Sweep, 1000.0);
    CHECK(slow.endFrequencyHz == 200.0 && slow.frequencyHz == 2.0);
    CHECK(defaultGeneratorComponent(Waveform::Sine, 1000.0).frequencyHz == 50.0);
    CHECK(defaultGeneratorComponent(Waveform::Sine, 48000.0).frequencyHz == 1000.0);
    CHECK(defaultGeneratorComponent(Waveform::Step, 48000.0, 2.0).eventSeconds == 0.2);
    CHECK(defaultGeneratorComponent(Waveform::Pulse).dutyCycle == 0.1);
    CHECK(defaultGeneratorComponent(Waveform::Noise).amplitude == 0.1);

    // Another waveform keeps what both share.
    GeneratorComponent tone = make(Waveform::Square, 2.0, 440.0);
    tone.dutyCycle = 0.3;
    tone.phaseDegrees = 45.0;
    tone.bandLimit = BandLimit::Fourier;
    const GeneratorComponent pulses = withWaveform(tone, Waveform::Pulse);
    CHECK(pulses.waveform == Waveform::Pulse && pulses.amplitude == 2.0 && pulses.frequencyHz == 440.0);
    CHECK(pulses.dutyCycle == 0.3 && pulses.phaseDegrees == 45.0 && pulses.bandLimit == BandLimit::Fourier);
    const GeneratorComponent triangle = withWaveform(tone, Waveform::Triangle);
    CHECK(triangle.frequencyHz == 440.0 && triangle.dutyCycle == 0.5 && triangle.bandLimit == BandLimit::Fourier);
    const GeneratorComponent swept = withWaveform(tone, Waveform::Sweep);
    CHECK(swept.amplitude == 2.0 && swept.frequencyHz == 20.0 && swept.bandLimit == BandLimit::Sampled);
    const GeneratorComponent stepped = withWaveform(tone, Waveform::Step, 1000.0, 3.0);
    CHECK(stepped.eventSeconds == 0.3 && stepped.amplitude == 2.0);
    GeneratorComponent late = stepped;
    late.eventSeconds = 1.25;
    CHECK(withWaveform(late, Waveform::Impulse).eventSeconds == 1.25);
    CHECK(withWaveform(tone, Waveform::Square).dutyCycle == 0.3);

    CHECK(componentText(make(Waveform::Sine, 1.0), "V") == "Sinus 1 kHz, 1 V crête");
    CHECK(componentText(defaultGeneratorComponent(Waveform::Noise), "V") == "Bruit gaussien, écart type 100 mV");
    CHECK(componentText(sweep, "V") == "Balayage 20 Hz → 20 kHz (log.), 1 V crête");
    GeneratorComponent current = make(Waveform::Sine, 14.14, 50.0);
    current.phaseDegrees = -30.0;
    CHECK(componentText(current, "A") == "Sinus 50 Hz, 14,14 A crête, phase \xE2\x88\x92" "30°");
    CHECK(componentText(defaultGeneratorComponent(Waveform::Impulse, 48000.0, 0.1), "V") == "Dirac de 1 V à 10 ms");
    CHECK(componentText(defaultGeneratorComponent(Waveform::Pulse), "V") == "Impulsions 1 kHz, 1 V, rapport cyclique 10 %");
    GeneratorComponent off = make(Waveform::Dc, 2.0);
    off.enabled = false;
    CHECK(componentText(off, "") == "Continu 2 (désactivée)");

    Waveform w;
    for (Waveform each : {Waveform::Sine, Waveform::Square, Waveform::Triangle, Waveform::Sawtooth, Waveform::Pulse,
                          Waveform::Dc, Waveform::Step, Waveform::Impulse, Waveform::Noise, Waveform::Sweep,
                          Waveform::FourierSeries, Waveform::Drawn, Waveform::Recurrence, Waveform::Formula,
                          Waveform::Modulation})
    {
        CHECK(waveformFromKey(waveformKey(each), w) && w == each);
        CHECK(generatorWaveformName(each) != "?");
    }
    CHECK(!waveformFromKey("dent", w));

    const auto &examples = generatorExamples();
    CHECK(examples.size() >= 8);
    for (const GeneratorExample &e : examples)
    {
        CHECK(!e.name.empty() && !e.tip.empty());
        CHECK(checkGeneratorRecipe(e.recipe).empty());
        CHECK(toJson(generatorRecipeFromJson(toJson(e.recipe))) == toJson(e.recipe));
        const GeneratorResult g = generateSignal(e.recipe);
        CHECK(g.ok());
        CHECK(g.warnings.empty());
    }
}

namespace
{
// The samples of one component alone, on one channel.
std::vector<double> alone(const GeneratorComponent &component, double rate = 48000.0, double duration = 0.02,
                          std::vector<std::string> *warnings = nullptr, std::string *error = nullptr)
{
    GeneratorRecipe r;
    r.sampleRateHz = rate;
    r.durationSeconds = duration;
    GeneratorChannel channel;
    channel.components = {component};
    r.channels = {channel};
    GeneratorResult g = generateSignal(r);
    if (warnings) *warnings = g.warnings;
    if (error) *error = g.error;
    if (!g.ok()) return {};
    return g.signal.data->channels[0];
}

double largestGap(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size() || a.empty()) return 1e300;
    double gap = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) gap = std::max(gap, std::abs(a[i] - b[i]));
    return gap;
}
} // namespace

TEST_CASE("generator: a Fourier series from formulas of n, in its three forms", "[analysis]")
{
    GeneratorComponent series = defaultGeneratorComponent(Waveform::FourierSeries);
    CHECK(series.harmonics == 15 && series.bandLimit == BandLimit::Fourier);
    CHECK(series.variables == "E = 1" && series.formulaB == "4*E/(pi*n)*odd(n)");
    series.frequencyHz = 100.0;
    // The square wave of the generator, by its own series of 15 terms: the same samples.
    GeneratorComponent square = make(Waveform::Square, 1.0, 100.0);
    square.bandLimit = BandLimit::Fourier;
    square.harmonics = 15;
    const std::vector<double> reference = alone(square);
    REQUIRE(reference.size() == 960);
    CHECK(largestGap(alone(series), reference) < 1e-9);
    CHECK(componentText(series, "V") == "Série de Fourier 100 Hz, 15 harmoniques : b\xE2\x82\x99 = 4*E/(pi*n)*odd(n) (E = 1)");

    // Cₙ = (aₙ − j bₙ)/2: Re Cₙ = 0, Im Cₙ = −2E/(πn) for odd n; or |Cₙ| = 2E/(πn), arg Cₙ = −90°.
    GeneratorComponent cartesian = series;
    cartesian.seriesForm = SeriesForm::Cartesian;
    cartesian.formulaA = "0";
    cartesian.formulaB = "-2*E/(pi*n)*odd(n)";
    CHECK(largestGap(alone(cartesian), reference) < 1e-9);
    GeneratorComponent polar = series;
    polar.seriesForm = SeriesForm::Polar;
    polar.formulaA = "2*E/(pi*n)*odd(n)";
    polar.formulaB = "-90";
    CHECK(largestGap(alone(polar), reference) < 1e-9);
    CHECK(seriesTermName(SeriesForm::Polar, 2) == "arg C\xE2\x82\x99");

    // Written in another form: the same signal, from formulas made for that form.
    // Written short (the number in front scaled), and back as they were.
    const GeneratorComponent asCartesian = withSeriesForm(series, SeriesForm::Cartesian);
    CHECK(asCartesian.formulaA == "0" && asCartesian.formulaB == "-2*E/(pi*n)*odd(n)");
    CHECK(largestGap(alone(asCartesian), reference) < 1e-9);
    const GeneratorComponent asPolar = withSeriesForm(asCartesian, SeriesForm::Polar);
    CHECK(asPolar.formulaA == "2*E/(pi*n)*odd(n)" && asPolar.formulaB == "-90"); // bₙ ≥ 0: a constant phase
    CHECK(largestGap(alone(asPolar), reference) < 1e-9);
    const GeneratorComponent back = withSeriesForm(asPolar, SeriesForm::SineCosine);
    CHECK(back.formulaA == "0" && back.formulaB == series.formulaB);
    CHECK(withSeriesForm(withSeriesForm(asPolar, SeriesForm::Cartesian), SeriesForm::Polar).formulaA == asPolar.formulaA);
    GeneratorComponent both = series;
    both.formulaA = "1/n^2";
    both.formula0 = "0,5";
    const GeneratorComponent bothPolar = withSeriesForm(both, SeriesForm::Polar);
    CHECK(bothPolar.formulaA == "sqrt((1/n^2)^2 + (4*E/(pi*n)*odd(n))^2)/2");
    CHECK(bothPolar.formulaB == "deg(atan2(-(4*E/(pi*n)*odd(n)); 1/n^2))");
    CHECK(largestGap(alone(bothPolar), alone(both)) < 1e-9);
    const GeneratorComponent bothCartesian = withSeriesForm(bothPolar, SeriesForm::Cartesian);
    CHECK(bothCartesian.formulaA == "0.5/n^2" && bothCartesian.formulaB == "-2*E/(pi*n)*odd(n)");
    CHECK(largestGap(alone(bothCartesian), alone(both)) < 1e-9);
    const GeneratorComponent bothBack = withSeriesForm(bothCartesian, SeriesForm::SineCosine);
    CHECK(bothBack.formulaA == "1/n^2" && bothBack.formulaB == both.formulaB && bothBack.formula0 == "0,5");

    // Signs that change with n (triangle), a sum, a factor without number, the models: there and back.
    for (const char *model : {"square", "triangle", "sawtooth", "pulse", "fullwave", "halfwave"})
    {
        GeneratorComponent m = series;
        REQUIRE(applySeriesModel(m, model, 2.0, 0.3));
        for (SeriesForm form : {SeriesForm::Cartesian, SeriesForm::Polar})
        {
            const GeneratorComponent there = withSeriesForm(m, form);
            CHECK(largestGap(alone(there), alone(m)) < 1e-9);
            const GeneratorComponent again = withSeriesForm(there, SeriesForm::SineCosine);
            CHECK(again.formulaA == m.formulaA && again.formulaB == m.formulaB);
        }
    }
    GeneratorComponent full = series;
    REQUIRE(applySeriesModel(full, "fullwave"));
    const GeneratorComponent fullPolar = withSeriesForm(full, SeriesForm::Polar);
    CHECK(fullPolar.formulaA == "2*E/(pi*(4*n^2 - 1))" && fullPolar.formulaB == "180"); // aₙ < 0 for every n
    GeneratorComponent triangle = series;
    REQUIRE(applySeriesModel(triangle, "triangle"));
    CHECK(withSeriesForm(triangle, SeriesForm::Polar).formulaB == "-90*sign(8*E/(pi^2*n^2)*sin(n*pi/2))");
    GeneratorComponent other = series;
    other.formulaA = "1 + 1/n";
    other.formulaB = "E/(pi*n)";
    const GeneratorComponent otherCartesian = withSeriesForm(other, SeriesForm::Cartesian);
    CHECK(otherCartesian.formulaA == "(1 + 1/n)/2" && otherCartesian.formulaB == "-E/(pi*n)/2");
    const GeneratorComponent otherBack = withSeriesForm(otherCartesian, SeriesForm::SineCosine);
    CHECK(otherBack.formulaA == other.formulaA && otherBack.formulaB == other.formulaB);
    // A divisor at the end, a number in front of it, what the lot before wrote: short, and back.
    GeneratorComponent half = series;
    half.formulaB = "E/2";
    const GeneratorComponent halfCartesian = withSeriesForm(half, SeriesForm::Cartesian);
    CHECK(halfCartesian.formulaB == "-E/4");
    CHECK(withSeriesForm(halfCartesian, SeriesForm::SineCosine).formulaB == "E/2");
    const GeneratorComponent halfPolar = withSeriesForm(halfCartesian, SeriesForm::Polar);
    CHECK(halfPolar.formulaA == "E/4" && halfPolar.formulaB == "-90");
    CHECK(withSeriesForm(halfPolar, SeriesForm::SineCosine).formulaB == "E/2");
    half.formulaB = "3*E/2";
    const GeneratorComponent threeHalves = withSeriesForm(half, SeriesForm::Cartesian);
    CHECK(largestGap(alone(threeHalves), alone(half)) < 1e-9);
    CHECK(withSeriesForm(threeHalves, SeriesForm::SineCosine).formulaB == "3*E/2");
    GeneratorComponent older = series;
    older.seriesForm = SeriesForm::Cartesian;
    older.formulaB = "-(4*E/(pi*n)*odd(n))/2";
    CHECK(withSeriesForm(older, SeriesForm::SineCosine).formulaB == "4*E/(pi*n)*odd(n)");
    CHECK(withSeriesForm(older, SeriesForm::Polar).formulaA == "2*E/(pi*n)*odd(n)");
    GeneratorComponent compared = series; // a comparison as a coefficient (1 or 0) keeps its parentheses
    compared.formulaA = "n < 3";
    compared.formulaB = "0";
    const GeneratorComponent comparedPolar = withSeriesForm(compared, SeriesForm::Polar);
    CHECK(comparedPolar.formulaA == "(n < 3)/2" && comparedPolar.formulaB == "0");
    CHECK(withSeriesForm(comparedPolar, SeriesForm::SineCosine).formulaA == "n < 3");
    CHECK(largestGap(alone(comparedPolar), alone(compared)) < 1e-9);
    GeneratorComponent turned = series;
    turned.seriesForm = SeriesForm::Polar;
    turned.formulaA = "1/n";
    turned.formulaB = "45"; // not a multiple of 90°: cosd and sind stay
    const GeneratorComponent turnedBack = withSeriesForm(turned, SeriesForm::SineCosine);
    CHECK(turnedBack.formulaA == "2/n*cosd(45)" && turnedBack.formulaB == "-2/n*sind(45)");
    CHECK(largestGap(alone(turnedBack), alone(turned)) < 1e-9);

    // As typed in a course: the name of the term before its formula, a leading "=", an empty a₀.
    GeneratorComponent typed = series;
    typed.formulaB = "b_n = 4*E/(pi*n)*odd(n)";
    typed.formula0 = "";
    typed.formulaA = "= 0";
    CHECK(largestGap(alone(typed), reference) < 1e-9);

    // Every harmonic below fs/2 (harmonics 0), and the variables built in.
    GeneratorComponent all = series;
    all.harmonics = 0;
    all.variables = "E = T0 * f0";
    CHECK(seriesHarmonics(all, 48000.0) == 239);
    square.harmonics = 0;
    CHECK(largestGap(alone(all), alone(square)) < 1e-9);
}

TEST_CASE("generator: a waveform made its series, the models of the coefficients", "[analysis]")
{
    // Square, triangle, sawtooth and pulses as series: the same signal as the band-limited waveform.
    for (Waveform w : {Waveform::Square, Waveform::Triangle, Waveform::Sawtooth, Waveform::Pulse})
    {
        GeneratorComponent wave = make(w, 2.0, 440.0);
        wave.dutyCycle = 0.3;
        const GeneratorComponent series = withWaveform(wave, Waveform::FourierSeries, 48000.0, 0.02);
        CHECK(series.waveform == Waveform::FourierSeries);
        CHECK(series.frequencyHz == 440.0 && series.harmonics == 0 && series.bandLimit == BandLimit::Fourier);
        CHECK(series.variables.starts_with("E = 2"));
        wave.bandLimit = BandLimit::Fourier;
        CHECK(largestGap(alone(series), alone(wave)) < 1e-9);
        // And back: the frequency stays.
        CHECK(withWaveform(series, Waveform::Sine).frequencyHz == 440.0);
    }

    GeneratorComponent c = defaultGeneratorComponent(Waveform::FourierSeries);
    REQUIRE(applySeriesModel(c, "fullwave", 325.27));
    SeriesCoefficients k = seriesCoefficients(c, 48000.0, 3);
    REQUIRE(k.ok());
    const double pi = std::numbers::pi;
    CHECK_NEAR(k.dc, 2.0 * 325.27 / pi, 1e-12, 0.0);
    CHECK_NEAR(k.a[0], -4.0 * 325.27 / (3.0 * pi), 1e-12, 0.0);
    CHECK(k.b[2] == 0.0);
    REQUIRE(applySeriesModel(c, "halfwave"));
    k = seriesCoefficients(c, 48000.0, 4);
    REQUIRE(k.ok());
    CHECK_NEAR(k.dc, 1.0 / pi, 1e-12, 0.0);
    CHECK(k.b[0] == 0.5 && k.b[1] == 0.0);
    CHECK(k.a[0] == 0.0 && k.a[2] == 0.0);
    CHECK_NEAR(k.a[1], -2.0 / (3.0 * pi), 1e-12, 0.0);
    REQUIRE(applySeriesModel(c, "pulse", 1.0, 0.25));
    CHECK(c.variables == "E = 1 ; D = 0,25");
    k = seriesCoefficients(c, 48000.0, 1);
    CHECK(k.ok() && k.dc == 0.25);
    CHECK(!applySeriesModel(c, "spline"));
}

TEST_CASE("generator: a Fourier series says what is wrong in which term, and what fs/2 cuts", "[analysis]")
{
    GeneratorComponent c = defaultGeneratorComponent(Waveform::FourierSeries);
    std::string error;
    c.formulaB = "4*D/(pi*n)";
    CHECK(alone(c, 48000.0, 0.01, nullptr, &error).empty());
    CHECK(error.starts_with("Voie 1, composante 1 (Série de Fourier) — b\xE2\x82\x99 : Nom inconnu « D »"));
    c.formulaB = "0";
    c.formulaA = "1/(n-1)";
    alone(c, 48000.0, 0.01, nullptr, &error);
    CHECK(contains(error, "a\xE2\x82\x99 n'est pas un nombre fini pour n = 1 (une division par zéro ? if(n == 1; …; …) l'évite)"));
    c.formulaA = "if(n == 1; 0; 1/(n-1))";
    CHECK(!alone(c, 48000.0, 0.01, nullptr, &error).empty());
    c.formula0 = "1/n";
    alone(c, 48000.0, 0.01, nullptr, &error);
    CHECK(contains(error, "a\xE2\x82\x80 ne dépend pas de n"));
    c.formula0 = "0";
    c.variables = "E = ";
    alone(c, 48000.0, 0.01, nullptr, &error);
    CHECK(contains(error, "Variables : « E = » sans valeur"));
    c.variables = "E = n";
    alone(c, 48000.0, 0.01, nullptr, &error);
    CHECK(contains(error, "Variables, E : la valeur n'est pas un nombre fini"));

    // At 10 kHz and 48 kHz, harmonics 3 to 5 are at or above fs/2: left out, or folded back.
    GeneratorComponent high = defaultGeneratorComponent(Waveform::FourierSeries);
    high.frequencyHz = 10000.0;
    high.harmonics = 5;
    std::vector<std::string> warnings;
    CHECK(!alone(high, 48000.0, 0.01, &warnings).empty());
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "Harmoniques 3 à 5 au-delà de fs/2 laissés de côté"));
    high.bandLimit = BandLimit::Sampled;
    alone(high, 48000.0, 0.01, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "Les harmoniques de rang 3 et plus dépassent fs/2 : ils se replient"));
    high.frequencyHz = 0.0;
    alone(high, 48000.0, 0.01, nullptr, &error);
    CHECK(contains(error, "La fondamentale f0 doit être positive"));

    // Kept with the recipe: its form, variables and formulas.
    GeneratorComponent polar = defaultGeneratorComponent(Waveform::FourierSeries);
    polar.seriesForm = SeriesForm::Polar;
    polar.variables = "A = 2";
    polar.formulaA = "A/n";
    polar.formulaB = "-90*odd(n)";
    const qacq::model::Json j = toJson(polar);
    CHECK(j["waveform"].toString() == "fourier" && j["form"].toString() == "polar" && j["an"].toString() == "A/n");
    const GeneratorComponent back = generatorComponentFromJson(j);
    CHECK(back.seriesForm == SeriesForm::Polar && back.variables == "A = 2" && back.formulaB == "-90*odd(n)");
    CHECK(toJson(back) == j);
    CHECK(!toJson(make(Waveform::Sine, 1.0)).contains("an"));
}

TEST_CASE("generator: a drawn motif, its points joined by steps, segments or a smooth curve", "[analysis]")
{
    // Within bounds, in the order of x, a jump kept, at most two points at one x.
    const std::vector<MotifPoint> raw = {{0.5, 0.2}, {1.5, 3.0}, {0.25, -2.0}, {0.5, 0.4}, {0.5, 0.6}, {std::nan(""), 0.0}};
    const std::vector<MotifPoint> kept = normalizedMotif(raw);
    REQUIRE(kept.size() == 4);
    CHECK((kept[0] == MotifPoint{0.25, -1.0}));
    CHECK((kept[1] == MotifPoint{0.5, 0.2}));
    CHECK((kept[2] == MotifPoint{0.5, 0.6}));
    CHECK((kept[3] == MotifPoint{1.0, 1.0}));

    const std::vector<MotifPoint> p = {{0.0, 0.0}, {0.25, 1.0}, {0.5, 0.0}, {0.75, -1.0}};
    const MotifShape steps(p, MotifInterpolation::Steps, true);
    CHECK(steps(0.1) == 0.0);
    CHECK(steps(0.25) == 1.0); // from a point on, its value
    CHECK(steps(0.9) == -1.0);
    CHECK(steps(1.3) == 1.0);   // periodic
    CHECK(steps(-0.2) == -1.0); // −0.2 is 0.8
    const MotifShape linear(p, MotifInterpolation::Linear, true);
    CHECK_NEAR(linear(0.125), 0.5, 1e-15, 0.0);
    CHECK_NEAR(linear(0.875), -0.5, 1e-15, 0.0); // the last point joined to the first one of the next period
    const MotifShape smooth(p, MotifInterpolation::Smooth, true);
    for (const MotifPoint &q : p) CHECK(smooth(q.x) == q.y); // through its points
    // Never beyond its points: a plateau stays flat, a peak is not overshot.
    const std::vector<MotifPoint> plateau = {{0.0, 0.0}, {0.2, 1.0}, {0.4, 1.0}, {0.6, 0.0}, {0.8, -0.5}};
    const MotifShape flat(plateau, MotifInterpolation::Smooth, true);
    double low = 1e9, high = -1e9;
    for (int i = 0; i < 10000; ++i)
    {
        const double v = flat(i / 10000.0);
        low = std::min(low, v);
        high = std::max(high, v);
    }
    CHECK(high <= 1.0);
    CHECK(low >= -0.5);
    CHECK(flat(0.3) == 1.0);
    // Once over the signal: before the first point its value, after the last one its value.
    const std::vector<MotifPoint> ramp = {{0.2, -1.0}, {0.8, 1.0}};
    const MotifShape once(ramp, MotifInterpolation::Linear, false);
    CHECK(once(0.1) == -1.0);
    CHECK(once(0.9) == 1.0);
    CHECK_NEAR(once(0.5), 0.0, 0.0, 1e-15);
    CHECK_NEAR(once.mean(), 0.0, 0.0, 1e-15);
    // A jump: two points at one x, the value after it from there on.
    const std::vector<MotifPoint> saw = {{0.0, 0.0}, {0.5, 1.0}, {0.5, -1.0}};
    const MotifShape tooth(saw, MotifInterpolation::Linear, true);
    CHECK_NEAR(tooth(0.4999), 0.9998, 1e-12, 0.0);
    CHECK(tooth(0.5) == -1.0);
    CHECK(MotifShape({}, MotifInterpolation::Smooth, true)(0.3) == 0.0);
    const std::vector<MotifPoint> one = {{0.4, 0.7}};
    CHECK(MotifShape(one, MotifInterpolation::Smooth, true)(0.9) == 0.7);

    // Its exact Fourier series against the integral of its values.
    const std::vector<MotifPoint> joined = {{0.05, 0.3}, {0.2, -0.6}, {0.45, 0.9}, {0.7, 0.5}, {0.9, -0.8}};
    const std::vector<MotifPoint> jumping = {{0.05, 0.3}, {0.2, -0.6}, {0.45, 0.9}, {0.45, -0.2}, {0.7, 0.5}, {0.9, -0.8}};
    for (MotifInterpolation interpolation : {MotifInterpolation::Steps, MotifInterpolation::Linear, MotifInterpolation::Smooth})
    {
        const bool continuous = interpolation != MotifInterpolation::Steps;
        const MotifShape shape(continuous ? joined : jumping, interpolation, true);
        const qacq::dsp::FourierSeries f = shape.fourierSeries(12);
        REQUIRE(f.cosine.size() == 12);
        const int count = 100000;
        const double tolerance = continuous ? 1e-8 : 2e-5; // the integral by the midpoint rule
        double mean = 0.0;
        for (int i = 0; i < count; ++i) mean += shape((i + 0.5) / count);
        CHECK_NEAR(f.dc, mean / count, 0.0, tolerance);
        for (int k : {1, 2, 5, 12})
        {
            double c = 0.0, d = 0.0;
            for (int i = 0; i < count; ++i)
            {
                const double u = (i + 0.5) / count, v = shape(u);
                c += v * std::cos(2.0 * std::numbers::pi * k * u);
                d += v * std::sin(2.0 * std::numbers::pi * k * u);
            }
            CHECK_NEAR(f.cosine[static_cast<std::size_t>(k - 1)], 2.0 * c / count, 0.0, tolerance);
            CHECK_NEAR(f.sine[static_cast<std::size_t>(k - 1)], 2.0 * d / count, 0.0, tolerance);
        }
    }
}

TEST_CASE("generator: a drawn motif gives the samples of the waveform it was made from", "[analysis]")
{
    for (Waveform w : {Waveform::Square, Waveform::Pulse, Waveform::Triangle, Waveform::Sawtooth})
    {
        GeneratorComponent wave = make(w, 2.0, 1300.0);
        wave.dutyCycle = 0.3;
        wave.phaseDegrees = 40.0;
        const GeneratorComponent drawn = withWaveform(wave, Waveform::Drawn);
        CHECK(drawn.waveform == Waveform::Drawn && drawn.amplitude == 2.0);
        CHECK(drawn.frequencyHz == 1300.0 && drawn.phaseDegrees == 40.0);
        CHECK(largestGap(alone(drawn), alone(wave)) < 1e-12);
        // By its series: the same as the band-limited waveform.
        wave.bandLimit = BandLimit::Fourier;
        const GeneratorComponent limited = withWaveform(wave, Waveform::Drawn);
        CHECK(limited.bandLimit == BandLimit::Fourier);
        CHECK(largestGap(alone(limited), alone(wave)) < 1e-9);
        // And back: frequency and phase stay.
        const GeneratorComponent again = withWaveform(drawn, Waveform::Sine);
        CHECK(again.frequencyHz == 1300.0 && again.phaseDegrees == 40.0);
    }
    // A sine: 16 points, smooth, close to it.
    const GeneratorComponent sine = withWaveform(make(Waveform::Sine, 1.0, 1000.0), Waveform::Drawn);
    CHECK(sine.motif.size() == 16);
    CHECK(sine.interpolation == MotifInterpolation::Smooth);
    CHECK(largestGap(alone(sine), alone(make(Waveform::Sine, 1.0, 1000.0))) < 0.006);

    // By its series: the sum of its first harmonics, from its phase, times its amplitude.
    GeneratorComponent c = defaultGeneratorComponent(Waveform::Drawn);
    c.motif = normalizedMotif({{0.05, 0.3}, {0.2, -0.6}, {0.45, 0.9}, {0.45, -0.2}, {0.7, 0.5}, {0.9, -0.8}});
    c.interpolation = MotifInterpolation::Smooth;
    c.amplitude = 2.0;
    c.frequencyHz = 440.0;
    c.phaseDegrees = 30.0;
    c.bandLimit = BandLimit::Fourier;
    c.harmonics = 20;
    const std::vector<double> x = alone(c);
    REQUIRE(x.size() == 960);
    const qacq::dsp::FourierSeries f = MotifShape(c.motif, c.interpolation, true).fourierSeries(20);
    for (std::size_t i : {0u, 17u, 500u, 959u})
    {
        const double u = 440.0 * static_cast<double>(i) / 48000.0 + 30.0 / 360.0;
        double v = f.dc;
        for (std::size_t k = 1; k <= 20; ++k)
        {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) * u;
            v += f.cosine[k - 1] * std::cos(angle) + f.sine[k - 1] * std::sin(angle);
        }
        CHECK_NEAR(x[i], 2.0 * v, 0.0, 1e-9);
    }

    // Once over the whole signal: frequency and phase do not count.
    GeneratorComponent ramp = defaultGeneratorComponent(Waveform::Drawn, 1000.0, 1.0);
    ramp.motif = {{0.0, -1.0}, {1.0, 1.0}};
    ramp.motifOnce = true;
    ramp.phaseDegrees = 90.0;
    const std::vector<double> line = alone(ramp, 1000.0, 1.0);
    REQUIRE(line.size() == 1000);
    CHECK(line[0] == -1.0);
    CHECK_NEAR(line[500], 0.0, 0.0, 1e-12);
    CHECK_NEAR(line[999], 0.998, 1e-12, 0.0);
    CHECK(withWaveform(ramp, Waveform::Sine, 1000.0).frequencyHz == 50.0); // its own, not the motif's

    // What fs/2 does to it.
    GeneratorComponent fast = defaultGeneratorComponent(Waveform::Drawn);
    fast.frequencyHz = 30000.0;
    std::vector<std::string> warnings;
    std::string error;
    alone(fast, 48000.0, 0.02, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "dépasse fs/2"));
    fast.bandLimit = BandLimit::Fourier;
    const std::vector<double> flat = alone(fast, 48000.0, 0.02, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "seul le terme constant"));
    REQUIRE(!flat.empty());
    CHECK_NEAR(flat[100], MotifShape(fast.motif, fast.interpolation, true).mean(), 0.0, 1e-12);
    fast.frequencyHz = 0.0;
    CHECK(alone(fast, 48000.0, 0.02, &warnings, &error).empty());
    CHECK(contains(error, "Motif dessiné") && contains(error, "fréquence doit être positive"));
}

TEST_CASE("generator: a drawn motif as text, as JSON, by strokes of the mouse, from models", "[analysis]")
{
    const GeneratorComponent drawn = defaultGeneratorComponent(Waveform::Drawn);
    CHECK(drawn.motif.size() == 4);
    CHECK(drawn.interpolation == MotifInterpolation::Linear);
    CHECK(!drawn.motifOnce);
    CHECK(componentText(drawn, "V") == "Motif dessiné 1 kHz, 4 points (segments), 1 V crête");
    GeneratorComponent short_ = drawn;
    short_.motifOnce = true;
    short_.interpolation = MotifInterpolation::Steps;
    short_.motif.resize(1);
    CHECK(componentText(short_, "V") == "Motif dessiné sur toute la durée, 1 point (paliers), 1 V crête");
    GeneratorComponent phased = drawn;
    phased.phaseDegrees = 90.0;
    phased.bandLimit = BandLimit::Fourier;
    CHECK(componentText(phased, "V") == "Motif dessiné 1 kHz, 4 points (segments), 1 V crête, phase 90°, série de Fourier");

    // Text: 4 decimals at most, a comma; read back with either decimal mark, the points unchanged on error.
    CHECK(motifText(drawn.motif) == "0 -1 ; 0,1 1 ; 0,5 1 ; 0,6 -1");
    CHECK(motifText(std::vector<MotifPoint>{{1.0 / 3.0, -2.0 / 3.0}}) == "0,3333 -0,6667");
    std::vector<MotifPoint> typed;
    std::string error;
    CHECK(parseMotif("0 0 ; 0,25 1\n0.75 \xE2\x88\x92" "1 ;", typed, error));
    CHECK((typed == std::vector<MotifPoint>{{0.0, 0.0}, {0.25, 1.0}, {0.75, -1.0}}));
    CHECK(!parseMotif("0 0 ; 0,25", typed, error));
    CHECK(contains(error, "Point 2") && contains(error, "deux nombres"));
    CHECK(typed.size() == 3);
    CHECK(!parseMotif("1,5 0", typed, error));
    CHECK(contains(error, "entre 0 et 1"));
    CHECK(!parseMotif("0 -2", typed, error));
    CHECK(contains(error, "entre -1 et 1"));
    CHECK(!parseMotif("0 x", typed, error));
    CHECK(contains(error, "« x »"));
    CHECK(parseMotif("", typed, error));
    CHECK(typed.empty());

    // JSON: back as it was; points of any spelling read within their bounds.
    GeneratorComponent stored = drawn;
    stored.interpolation = MotifInterpolation::Smooth;
    stored.motifOnce = true;
    const Json j = toJson(stored);
    CHECK(j["motif"].size() == 4);
    CHECK(j["interpolation"].toString() == "smooth");
    const GeneratorComponent back = generatorComponentFromJson(j);
    CHECK(back.motif == stored.motif);
    CHECK(back.interpolation == MotifInterpolation::Smooth);
    CHECK(back.motifOnce);
    const auto parsed = Json::parse(R"({"waveform": "drawn", "motif": [[0.5, 2], [0.1, 0], {"x": 0.3, "y": -0.5}, "x", [1]], "interpolation": "steps"})");
    REQUIRE(parsed);
    const GeneratorComponent read = generatorComponentFromJson(*parsed);
    REQUIRE(read.motif.size() == 3);
    CHECK((read.motif[0] == MotifPoint{0.1, 0.0}));
    CHECK((read.motif[1] == MotifPoint{0.3, -0.5}));
    CHECK((read.motif[2] == MotifPoint{0.5, 1.0}));
    CHECK(read.interpolation == MotifInterpolation::Steps);
    CHECK(!read.motifOnce);
    CHECK(!toJson(make(Waveform::Sine, 1.0)).contains("motif"));

    // Strokes: where one passes it replaces the motif, as a function of x whichever way it goes.
    const std::vector<MotifPoint> base = {{0.0, -1.0}, {0.1, 1.0}, {0.5, 1.0}, {0.6, -1.0}};
    const std::vector<MotifPoint> straight = withStroke(base, std::vector<MotifPoint>{{0.7, -0.5}, {0.75, -0.5}, {0.9, -0.5}});
    REQUIRE(straight.size() == 6);
    CHECK_NEAR(straight[4].x, 0.7, 0.0, 1e-3);
    CHECK_NEAR(straight[5].x, 0.9, 0.0, 1e-3);
    CHECK(straight[4].y == -0.5 && straight[5].y == -0.5);
    const std::vector<MotifPoint> across = withStroke(base, std::vector<MotifPoint>{{0.55, 0.2}, {0.05, 0.2}});
    REQUIRE(across.size() == 4);
    CHECK(across[0] == base[0]);
    CHECK(across[1].y == 0.2 && across[2].y == 0.2);
    CHECK(across[3] == base[3]);
    // A curve: simplified, within its tolerance.
    std::vector<MotifPoint> wave;
    for (int i = 0; i <= 400; ++i)
        wave.push_back({i / 400.0, 0.8 * std::sin(2.0 * std::numbers::pi * i / 400.0)});
    const std::vector<MotifPoint> curve = withStroke({}, wave);
    CHECK(curve.size() > 10);
    CHECK(curve.size() < 80);
    const MotifShape shape(curve, MotifInterpolation::Linear, false);
    double gap = 0.0;
    for (int i = 0; i <= 1000; ++i)
        gap = std::max(gap, std::abs(shape(i / 1000.0) - 0.8 * std::sin(2.0 * std::numbers::pi * i / 1000.0)));
    CHECK(gap < 0.01);
    // Up within a column, then on: a jump.
    const std::vector<MotifPoint> jump = withStroke(base, std::vector<MotifPoint>{{0.3, -0.8}, {0.3, 0.8}, {0.4, 0.8}});
    REQUIRE(jump.size() == 7);
    CHECK(jump[2].x == jump[3].x);
    CHECK(jump[2].y == -0.8);
    CHECK(jump[3].y == 0.8);
    CHECK(withStroke(base, std::vector<MotifPoint>{{0.3, 0.0}}).size() == 5); // one point: added

    // Models.
    GeneratorComponent m = defaultGeneratorComponent(Waveform::Drawn);
    for (const char *key : {"ramp", "stairs", "trapezoid", "rc", "heartbeat"})
    {
        REQUIRE(applyMotifModel(m, key));
        CHECK(m.motif.size() >= 2);
    }
    CHECK(!applyMotifModel(m, "spline"));
    REQUIRE(applyMotifModel(m, "stairs"));
    CHECK(m.interpolation == MotifInterpolation::Steps);
    CHECK(m.motif.size() == 4);
    REQUIRE(applyMotifModel(m, "rc"));
    const MotifShape rc(m.motif, m.interpolation, true);
    const double v = std::tanh(2.5);
    CHECK_NEAR(rc(0.5), v, 1e-12, 0.0);
    CHECK_NEAR(rc(0.0), -v, 1e-12, 0.0);
    double top = -1e9;
    for (int i = 0; i < 10000; ++i) top = std::max(top, rc(i / 10000.0));
    CHECK(top <= v + 1e-12);
}

TEST_CASE("generator: a recurrence, y[n] from the values before it and an input", "[analysis]")
{
    // The oscillator: the same sine, of any amplitude, frequency and phase.
    const GeneratorComponent oscillator = defaultGeneratorComponent(Waveform::Recurrence);
    CHECK(oscillator.recurrence == "2*cos(w)*y[n-1] - y[n-2]");
    CHECK(checkRecurrence(oscillator, 48000.0).empty());
    CHECK(largestGap(alone(oscillator), alone(make(Waveform::Sine, 1.0, 1000.0))) < 1e-11);
    GeneratorComponent sine = make(Waveform::Sine, 2.0, 440.0);
    sine.phaseDegrees = 30.0;
    const GeneratorComponent made = withWaveform(sine, Waveform::Recurrence);
    CHECK(made.waveform == Waveform::Recurrence);
    CHECK(contains(made.variables, "phi = rad(30)"));
    CHECK(largestGap(alone(made, 48000.0, 1.0), alone(sine, 48000.0, 1.0)) < 1e-9);

    // A first-order low-pass of a step: 1 − a^(n+1); written in every way.
    GeneratorComponent lowpass = defaultGeneratorComponent(Waveform::Recurrence);
    REQUIRE(applyRecurrenceModel(lowpass, "lowpass"));
    CHECK(lowpass.recurrenceInput == RecurrenceInput::Step);
    const double a = std::exp(-1.0 / (48000.0 * 0.001));
    for (const char *typed : {"a*y[n-1] + (1 - a)*x[n]", "y[n] = a*y(n-1) + (1 - a)*x(n)", "y = a*y_{n-1} + (1-a)*x_{n}",
                              "a y[n \xE2\x88\x92 1] + (1 \xE2\x88\x92 a) x[n]", "= a*y1 + (1 - a)*x"})
    {
        lowpass.recurrence = typed;
        const std::vector<double> y = alone(lowpass);
        REQUIRE(y.size() == 960);
        CHECK_NEAR(y[0], 1.0 - a, 1e-12, 0.0);
        CHECK_NEAR(y[99], 1.0 - std::pow(a, 100.0), 1e-12, 0.0);
    }

    // Inputs: a Dirac, nothing, a noise (that of a noise component at its place), the components above.
    GeneratorComponent echo = defaultGeneratorComponent(Waveform::Recurrence);
    echo.variables.clear();
    echo.initialValues.clear();
    echo.recurrence = "x[n] + 0,5*x[n-2]";
    echo.recurrenceInput = RecurrenceInput::Impulse;
    std::vector<double> y = alone(echo);
    CHECK(y[0] == 1.0);
    CHECK(y[1] == 0.0);
    CHECK(y[2] == 0.5);
    CHECK(y[3] == 0.0);
    echo.recurrenceInput = RecurrenceInput::None;
    y = alone(echo);
    CHECK(*std::max_element(y.begin(), y.end()) == 0.0);
    echo.recurrence = "x[n]";
    echo.recurrenceInput = RecurrenceInput::Noise;
    GeneratorComponent noise = make(Waveform::Noise, 1.0);
    CHECK(largestGap(alone(echo), alone(noise)) == 0.0);

    // Above: y[n] filters what is above it and replaces it; what is below adds.
    GeneratorRecipe r;
    r.sampleRateHz = 48000.0;
    r.durationSeconds = 0.02;
    GeneratorComponent square = make(Waveform::Square, 1.0, 200.0);
    GeneratorComponent filter = lowpass;
    filter.recurrence = "a*y[n-1] + (1 - a)*x[n]";
    filter.recurrenceInput = RecurrenceInput::Above;
    GeneratorChannel channel;
    channel.components = {square, filter, make(Waveform::Dc, 0.25)};
    r.channels = {channel};
    GeneratorResult g = generateSignal(r);
    REQUIRE(g.ok());
    const std::vector<double> &out = g.signal.data->channels[0];
    const std::vector<double> in = alone(square);
    double previous = 0.0, gap = 0.0;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        previous = a * previous + (1.0 - a) * in[i];
        gap = std::max(gap, std::abs(out[i] - (previous + 0.25)));
    }
    CHECK(gap < 1e-12);
    // Its first values, as generated.
    REQUIRE(g.traces.size() == 1);
    CHECK(g.traces[0].channel == 0);
    CHECK(g.traces[0].component == 1);
    REQUIRE(g.traces[0].x.size() == 12);
    CHECK(g.traces[0].x[0] == 1.0);
    CHECK_NEAR(g.traces[0].y[0], 1.0 - a, 1e-12, 0.0);
    // Nothing above: its input is 0.
    channel.components = {filter};
    r.channels = {channel};
    g = generateSignal(r);
    REQUIRE(g.ok());
    CHECK(g.signal.data->channels[0][500] == 0.0);
}

TEST_CASE("generator: a recurrence says what is wrong, and where it diverges", "[analysis]")
{
    GeneratorComponent c = defaultGeneratorComponent(Waveform::Recurrence);
    const auto problem = [&c](const std::string &formula) {
        GeneratorComponent d = c;
        d.recurrence = formula;
        return checkRecurrence(d, 48000.0);
    };
    CHECK(contains(problem("b*y[n-1]"), "y[n] : ") && contains(problem("b*y[n-1]"), "« b »"));
    CHECK(contains(problem("0,5*y[n]"), "ne dépend que des valeurs d'avant"));
    CHECK(contains(problem("y[n+1]"), "ne dépend que des valeurs d'avant"));
    CHECK(contains(problem("y[n-17]"), "au plus 16"));
    CHECK(problem("y[n-16] + x[n-16]").empty());
    CHECK(problem("y = 2").empty());
    CHECK(!recurrenceUsesInput(c, 48000.0)); // the oscillator
    GeneratorComponent reads = c;
    reads.recurrence = "0,5*y[n-1] + x[n-3]";
    CHECK(recurrenceUsesInput(reads, 48000.0));
    reads.recurrence = "x + y1";
    CHECK(recurrenceUsesInput(reads, 48000.0));
    GeneratorComponent d = c;
    d.initialValues = "x[-1] = 1";
    CHECK(contains(checkRecurrence(d, 48000.0), "Valeurs initiales"));
    d.initialValues = "z = 1";
    CHECK(contains(checkRecurrence(d, 48000.0), "« z » n'est pas une valeur y[-1]"));
    d.initialValues = "y[-1] = n";
    CHECK(contains(checkRecurrence(d, 48000.0), "pas un nombre fini"));
    d = c;
    d.variables = "k = n";
    CHECK(contains(checkRecurrence(d, 48000.0), "Variables"));
    d.variables = "x = 1";
    CHECK(contains(checkRecurrence(d, 48000.0), "Variables"));

    // Divergence: 2^(n+1) is no longer finite at n = 1023.
    d = c;
    d.variables.clear();
    d.recurrence = "2*y[n-1]";
    d.initialValues = "y[-1] = 1";
    std::string error;
    CHECK(alone(d, 48000.0, 0.1, nullptr, &error).empty());
    CHECK(contains(error, "Récurrence") && contains(error, "n = 1 023") && contains(error, "diverge"));
    d.recurrence = "1/x[n]";
    CHECK(alone(d, 48000.0, 0.1, nullptr, &error).empty());
    CHECK(contains(error, "n = 0"));

    // Models, text, JSON.
    for (const char *key : {"oscillator", "lowpass", "resonator", "average", "walk", "logistic"})
    {
        GeneratorComponent m = defaultGeneratorComponent(Waveform::Recurrence);
        REQUIRE(applyRecurrenceModel(m, key));
        CHECK(checkRecurrence(m, 48000.0).empty());
        std::string why;
        const std::vector<double> y = alone(m, 48000.0, 0.02, nullptr, &why);
        CHECK(why.empty());
        CHECK(y.size() == 960);
    }
    GeneratorComponent logistic = defaultGeneratorComponent(Waveform::Recurrence);
    REQUIRE(applyRecurrenceModel(logistic, "logistic"));
    const std::vector<double> chaos = alone(logistic);
    CHECK_NEAR(chaos[0], 3.9 * 0.2 * 0.8, 1e-12, 0.0);
    CHECK(*std::min_element(chaos.begin(), chaos.end()) > 0.0);
    CHECK(*std::max_element(chaos.begin(), chaos.end()) < 1.0);
    CHECK(!applyRecurrenceModel(logistic, "fibonacci"));
    CHECK(componentText(logistic, "V") == "Récurrence y[n] = r*y[n-1]*(1 - y[n-1]) (r = 3,9), y[-1] = 0,2");
    GeneratorComponent driven = defaultGeneratorComponent(Waveform::Recurrence);
    REQUIRE(applyRecurrenceModel(driven, "average"));
    CHECK(componentText(driven, "V") == "Récurrence y[n] = (x[n] + x[n-1] + x[n-2] + x[n-3])/4, entrée : bruit blanc");
    driven.recurrenceInput = RecurrenceInput::Above;
    const GeneratorComponent back = generatorComponentFromJson(toJson(driven));
    CHECK(back.recurrence == driven.recurrence);
    CHECK(back.recurrenceInput == RecurrenceInput::Above);
    CHECK(toJson(driven)["input"].toString() == "above");
    CHECK(!toJson(make(Waveform::Sine, 1.0)).contains("recurrence"));
}

TEST_CASE("generator: an operation between channels, made after the channels it reads", "[analysis]")
{
    GeneratorRecipe r;
    r.sampleRateHz = 10000.0;
    r.durationSeconds = 0.1;
    GeneratorComponent op = defaultGeneratorComponent(Waveform::Formula);
    op.expression = "y = v2*v3";
    GeneratorChannel product, a, b;
    product.name = "Produit";
    product.components = {op};
    a.name = "A";
    a.offset = 0.5;
    a.components = {make(Waveform::Sine, 2.0, 50.0)};
    b.name = "B";
    b.components = {make(Waveform::Square, 1.0, 20.0)};
    r.channels = {product, a, b}; // the first one reads those after it
    GeneratorResult g = generateSignal(r);
    REQUIRE(g.ok());
    std::vector<std::vector<double>> ch = g.signal.data->channels;
    REQUIRE(ch.size() == 3);
    double gap = 0.0;
    for (std::size_t i = 0; i < ch[0].size(); ++i) gap = std::max(gap, std::abs(ch[0][i] - ch[1][i] * ch[2][i]));
    CHECK(gap < 1e-12); // the channel A with its offset
    CHECK(g.signal.channels[0].name == "Produit");
    CHECK(contains(g.signal.notes, "Produit : Opération entre voies : v2*v3."));

    // x: the components above, replaced; otherwise added. Uppercase names, variables, t.
    GeneratorComponent square = defaultGeneratorComponent(Waveform::Formula);
    square.expression = "k*x^2";
    square.variables = "k = 3";
    r.channels[0].components = {make(Waveform::Sine, 1.0, 100.0), square};
    g = generateSignal(r);
    REQUIRE(g.ok());
    ch = g.signal.data->channels;
    for (std::size_t i = 0; i < ch[0].size(); i += 7)
        CHECK_NEAR(ch[0][i], 3.0 * std::pow(std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(i) / 10000.0), 2), 0.0, 1e-9);
    GeneratorComponent added = defaultGeneratorComponent(Waveform::Formula);
    added.expression = "V2 + sin(2*pi*50*t)";
    r.channels[0].components = {make(Waveform::Dc, 1.0), added};
    g = generateSignal(r);
    REQUIRE(g.ok());
    ch = g.signal.data->channels;
    for (std::size_t i = 0; i < ch[0].size(); i += 13)
        CHECK_NEAR(ch[0][i], 1.0 + ch[1][i] + std::sin(2.0 * std::numbers::pi * 50.0 * static_cast<double>(i) / 10000.0), 0.0, 1e-9);

    // What it reads; what is wrong, at once and when generating.
    OperationCheck check = checkOperation(r, 0, 1);
    CHECK(check.error.empty());
    CHECK((check.channels == std::vector<std::size_t>{1}));
    CHECK(!check.readsAbove);
    CHECK(checkOperation(r, 1, 0).channels.empty()); // not an operation
    r.channels[0].components[1].expression = "v1 + w";
    CHECK(contains(checkOperation(r, 0, 1).error, "y : Nom inconnu"));
    r.channels[0].components[1].expression = "v5";
    CHECK(checkOperation(r, 0, 1).error == "y : la voie 5 n'existe pas (le signal a 3 voies).");
    r.channels[0].components[1].expression = "v1/2";
    CHECK(contains(checkOperation(r, 0, 1).error, "est la voie elle-même"));
    g = generateSignal(r);
    CHECK(!g.ok());
    CHECK(contains(g.error, "Produit, composante 2 (Opération entre voies) — y : « v1 » est la voie elle-même"));
    r.channels[0].components[1].expression = "v2";
    GeneratorComponent back = defaultGeneratorComponent(Waveform::Formula);
    back.expression = "-v1";
    r.channels[1].components.push_back(back);
    CHECK(contains(checkOperation(r, 0, 1).error, "Voies 1 → 2 → 1 : une voie ne peut pas dépendre d'elle-même"));
    g = generateSignal(r);
    CHECK(contains(g.error, "Voies 1 → 2 → 1"));
    r.channels[1].components.back().enabled = false; // a component left out does not read
    CHECK(checkOperation(r, 0, 1).error.empty());
    CHECK(generateSignal(r).ok());

    // A division by zero: 0 there, and said.
    r.channels[0].components = {defaultGeneratorComponent(Waveform::Formula)};
    r.channels[0].components[0].expression = "1/v2";
    r.channels[1].offset = 0.0;
    g = generateSignal(r);
    REQUIRE(g.ok());
    REQUIRE(g.warnings.size() == 1);
    // (The sine is exactly 0 at each whole period: 0, 20, 40, 60 and 80 ms.)
    CHECK(contains(g.warnings[0], "y n'est pas un nombre fini en 5 échantillons (le premier à t = 0 s"));
    CHECK(contains(g.warnings[0], "ils valent 0."));
    CHECK(g.signal.data->channels[0][0] == 0.0);
    CHECK(g.signal.data->channels[0][200] == 0.0);

    // New operations, ready-made ones.
    CHECK(defaultOperation(0, 1) == "x");
    CHECK(defaultOperation(1, 2) == "v1 + x");
    CHECK(defaultOperation(2, 3) == "v1 + v2");
    CHECK(defaultOperation(0, 3) == "v2 + v3");
    const std::vector<OperationModel> models = operationModels(2, 3);
    REQUIRE(models.size() == 9);
    CHECK((models[0].key == "sum" && models[0].formula == "v1 + v2"));
    CHECK(models[2].formula == "v1*v2");
    CHECK(models[5].formula == "sqrt(v1^2 + v2^2)");
    const std::vector<OperationModel> single = operationModels(0, 1);
    REQUIRE(single.size() == 3);
    CHECK(single[0].formula == "abs(x)");
    CHECK(operationModels(1, 2)[0].formula == "v1 + x");
    GeneratorComponent model = defaultGeneratorComponent(Waveform::Formula);
    CHECK(applyOperationModel(model, "ratio", 0, 3));
    CHECK(model.expression == "v2/v3");
    CHECK(!applyOperationModel(model, "log", 0, 3));

    // Text and JSON.
    model.variables = "k = 2";
    CHECK(componentText(model, "V") == "Opération entre voies : v2/v3 (k = 2)");
    const GeneratorComponent read = generatorComponentFromJson(toJson(model));
    CHECK(read.waveform == Waveform::Formula);
    CHECK(read.expression == "v2/v3");
    CHECK(read.variables == "k = 2");
    CHECK(!toJson(make(Waveform::Sine, 1.0)).contains("expression"));
}

TEST_CASE("generator: a channel read by another is not removed; the others are renumbered", "[analysis]")
{
    GeneratorRecipe r;
    r.channels = {defaultGeneratorChannel(0), defaultGeneratorChannel(1), defaultGeneratorChannel(2), defaultGeneratorChannel(3)};
    GeneratorComponent op = defaultGeneratorComponent(Waveform::Formula);
    op.expression = "v1*v3 + V3 - fv3 + v3x";
    r.channels[3].components = {op};
    GeneratorComponent carrier = defaultGeneratorComponent(Waveform::Modulation);
    carrier.message = "v4";
    r.channels[1].components.push_back(carrier);
    CHECK((channelReaders(r, 0) == std::vector<std::size_t>{3}));
    CHECK((channelReaders(r, 2) == std::vector<std::size_t>{3}));
    CHECK((channelReaders(r, 3) == std::vector<std::size_t>{1}));
    CHECK(channelReaders(r, 1).empty());

    std::string why;
    GeneratorRecipe kept = r;
    CHECK(!removeGeneratorChannel(kept, 0, &why));
    CHECK(why == "La voie 1 est lue par la voie 4 (v1 dans son opération ou son message) : changez d'abord sa formule.");
    CHECK(kept.channels.size() == 4);
    CHECK(removeGeneratorChannel(kept, 1, &why)); // nobody reads the channel 2
    REQUIRE(kept.channels.size() == 3);
    CHECK(kept.channels[2].components[0].expression == "v1*v2 + V2 - fv3 + v3x");
    GeneratorRecipe one;
    one.channels = {defaultGeneratorChannel(0)};
    CHECK(!removeGeneratorChannel(one, 0, &why));
    CHECK(why == "Le signal garde au moins une voie.");
    GeneratorRecipe two = r;
    two.channels[1].components.pop_back();
    two.channels[0].components = {op};
    two.channels[0].components[0].expression = "v4";
    CHECK(!removeGeneratorChannel(two, 3, &why));
    CHECK(why == "La voie 4 est lue par la voie 1 (v4 dans son opération ou son message) : changez d'abord sa formule.");
    two.channels[1].components = {op};
    two.channels[1].components[0].expression = "2*v3";
    CHECK(!removeGeneratorChannel(two, 2, &why));
    CHECK(why == "La voie 3 est lue par les voies 2 et 4 (v3 dans leurs opérations ou messages) : changez d'abord leurs formules.");
}

TEST_CASE("generator: a modulated carrier, as the module Modulations makes it", "[analysis]")
{
    // The same samples as qacq::dsp::modulate, in AM, FM and PM.
    for (CarrierModulation kind : {CarrierModulation::Am, CarrierModulation::Fm, CarrierModulation::Pm})
    {
        GeneratorComponent carrier = defaultGeneratorComponent(Waveform::Modulation);
        carrier.modulation = kind;
        carrier.amplitude = 2.0;
        carrier.frequencyHz = 6000.0;
        carrier.phaseDegrees = 30.0;
        carrier.amIndex = 0.7;
        carrier.deviationHz = 1500.0;
        carrier.phaseDeviationDegrees = 60.0;
        carrier.message = "m = sin(2*pi*fm*t)";
        carrier.variables = "fm = 500";
        std::vector<std::string> warnings;
        const std::vector<double> y = alone(carrier, 48000.0, 0.02, &warnings);
        CHECK(warnings.empty());
        qacq::dsp::ModulationSpec spec;
        spec.type = kind == CarrierModulation::Am   ? qacq::dsp::Modulation::Am
                    : kind == CarrierModulation::Fm ? qacq::dsp::Modulation::Fm
                                                    : qacq::dsp::Modulation::Pm;
        spec.sampleRateHz = 48000.0;
        spec.sampleCount = 960;
        spec.carrierFrequencyHz = 6000.0;
        spec.carrierAmplitude = 2.0;
        spec.carrierPhaseDegrees = 30.0;
        spec.messageFrequencyHz = 500.0;
        spec.amIndex = 0.7;
        spec.frequencyDeviationHz = 1500.0;
        spec.phaseDeviationRad = 60.0 * std::numbers::pi / 180.0;
        const qacq::dsp::ModulatedSignal m = qacq::dsp::modulate(spec);
        REQUIRE(m.ok());
        CHECK(largestGap(y, m.passband) < 1e-9);
    }

    // The message from the components above (replaced), or from another channel.
    GeneratorComponent carrier = defaultGeneratorComponent(Waveform::Modulation, 48000.0, 0.02);
    CHECK(carrier.frequencyHz == 10000.0);
    CHECK(carrier.variables == "fm = 1000");
    CHECK(carrier.deviationHz == 2000.0);
    carrier.message = "sin(2*pi*500*t)";
    const std::vector<double> reference = alone(carrier);
    GeneratorComponent above = carrier;
    above.message = "x";
    GeneratorRecipe r;
    r.sampleRateHz = 48000.0;
    r.durationSeconds = 0.02;
    GeneratorChannel message, modulated;
    message.components = {make(Waveform::Sine, 1.0, 500.0)};
    modulated.components = {make(Waveform::Sine, 1.0, 500.0), above};
    r.channels = {modulated};
    GeneratorResult g = generateSignal(r);
    REQUIRE(g.ok());
    CHECK(largestGap(g.signal.data->channels[0], reference) < 1e-9);
    GeneratorComponent other = carrier;
    other.message = "v1";
    modulated.components = {other};
    r.channels = {message, modulated};
    g = generateSignal(r);
    REQUIRE(g.ok());
    CHECK(largestGap(g.signal.data->channels[1], reference) < 1e-9);
    CHECK((checkOperation(r, 1, 0).channels == std::vector<std::size_t>{0}));
    r.channels[1].components[0].message = "v2";
    CHECK(contains(checkOperation(r, 1, 0).error, "Message : « v2 » est la voie elle-même"));

    // What is said: overmodulation, an instantaneous frequency beyond fs/2, a carrier beyond fs/2, a message
    // that is not a number.
    std::vector<std::string> warnings;
    GeneratorComponent deep = carrier;
    deep.amIndex = 1.5;
    alone(deep, 48000.0, 0.02, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "surmodulation : m × crête du message = 1,5 > 1"));
    GeneratorComponent wide = carrier;
    wide.modulation = CarrierModulation::Fm;
    wide.deviationHz = 20000.0;
    alone(wide, 48000.0, 0.02, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "la fréquence instantanée va de"));
    GeneratorComponent high = carrier;
    high.frequencyHz = 30000.0;
    alone(high, 48000.0, 0.02, &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(contains(warnings[0], "dépasse fs/2"));
    GeneratorComponent undefined = carrier;
    undefined.message = "1/t";
    alone(undefined, 48000.0, 0.02, &warnings);
    REQUIRE(!warnings.empty());
    CHECK(contains(warnings[0], "Le message n'est pas un nombre fini en 1 échantillon"));
    std::string error;
    GeneratorComponent wrong = carrier;
    wrong.message = "sin(2*pi*f*t)";
    CHECK(alone(wrong, 48000.0, 0.02, nullptr, &error).empty());
    CHECK(contains(error, "Porteuse modulée) — Message : Nom inconnu « f »"));

    // Ready-made messages; text, JSON, another waveform.
    const std::vector<OperationModel> messages = messageModels(1, 3);
    REQUIRE(messages.size() == 5);
    CHECK((messages[3].key == "v1" && messages[3].formula == "v1" && messages[4].key == "v3"));
    GeneratorComponent m = defaultGeneratorComponent(Waveform::Modulation);
    m.variables.clear();
    CHECK(applyMessageModel(m, "above", 1, 3));
    CHECK(m.message == "x");
    CHECK(applyMessageModel(m, "square", 1, 3));
    CHECK(m.message == "sign(sin(2*pi*fm*t))");
    CHECK(m.variables == "fm = 1000");
    m.variables = "a = 2";
    CHECK(applyMessageModel(m, "sine", 1, 3));
    CHECK(m.variables == "a = 2 ; fm = 1000");
    m.variables = "fm = 300";
    CHECK(applyMessageModel(m, "sine", 1, 3));
    CHECK(m.variables == "fm = 300");
    CHECK(applyMessageModel(m, "v3", 1, 3));
    CHECK(m.message == "v3");
    CHECK(!applyMessageModel(m, "v2", 1, 3));
    CHECK(componentText(carrier, "V") == "Porteuse 10 kHz, 1 V crête, modulée en amplitude (m = 0,5) par sin(2*pi*500*t) (fm = 1000)");
    GeneratorComponent fm = carrier;
    fm.modulation = CarrierModulation::Fm;
    fm.phaseDegrees = 90.0;
    CHECK(componentText(fm, "V") == "Porteuse 10 kHz, 1 V crête, phase 90°, modulée en fréquence (Δf = 2 kHz) par sin(2*pi*500*t) (fm = 1000)");
    const GeneratorComponent read = generatorComponentFromJson(toJson(fm));
    CHECK(read.modulation == CarrierModulation::Fm);
    CHECK(read.message == fm.message);
    CHECK(read.deviationHz == 2000.0);
    CHECK(toJson(fm)["modulation"].toString() == "fm");
    const GeneratorComponent fromSine = withWaveform(make(Waveform::Sine, 3.0, 2000.0), Waveform::Modulation);
    CHECK(fromSine.amplitude == 3.0);
    CHECK(fromSine.frequencyHz == 2000.0);
    CHECK(fromSine.variables == "fm = 200");
    CHECK(fromSine.deviationHz == 400.0);
    CHECK(withWaveform(fm, Waveform::Sine).frequencyHz == 10000.0);
}
