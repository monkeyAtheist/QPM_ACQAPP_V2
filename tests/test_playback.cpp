#include "qacq_test.h"

#include "qacq/analysis/playback.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace qacq::analysis;

namespace
{
qacq::model::Signal makeSignal(std::vector<std::vector<double>> channels, double rate)
{
    qacq::model::Signal s;
    s.sampleRateHz = rate;
    auto data = std::make_shared<qacq::model::SampleData>();
    data->channels = std::move(channels);
    s.channels = qacq::model::defaultChannels(data->channels.size(), "V");
    s.data = data;
    return s;
}

std::vector<double> tone(double frequency, double rate, std::size_t n, double amplitude = 1.0)
{
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate);
    return x;
}

double rms(const std::vector<double> &x)
{
    double sum = 0.0;
    for (double v : x) sum += v * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
}

bool contains(const std::vector<std::string> &list, const std::string &part)
{
    return std::any_of(list.begin(), list.end(), [&](const std::string &s) { return s.find(part) != std::string::npos; });
}
} // namespace

TEST_CASE("playback: resampling keeps what the lower rate carries, and nothing beyond it", "[analysis]")
{
    // 1 kHz at 10 kHz brought to 48 kHz: the same sine (the edges aside, or periodic).
    const std::vector<double> x = tone(1000.0, 10000.0, 10000);
    const std::vector<double> y = resample(x, 10000.0, 48000.0);
    REQUIRE(y.size() == 48000);
    double gap = 0.0;
    for (std::size_t j = 2000; j < 46000; ++j)
        gap = std::max(gap, std::abs(y[j] - std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(j) / 48000.0)));
    CHECK(gap < 1e-3);
    const std::vector<double> loop = resample(x, 10000.0, 48000.0, true);
    gap = 0.0;
    for (std::size_t j = 0; j < loop.size(); ++j)
        gap = std::max(gap, std::abs(loop[j] - std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(j) / 48000.0)));
    CHECK(gap < 1e-3);
    CHECK(resample(x, 10000.0, 10000.0) == x);
    CHECK(resample({}, 10000.0, 48000.0).empty());

    // Down from 192 kHz to 48 kHz: 30 kHz is gone (nothing folds back to 18 kHz), 1 kHz stays.
    const std::vector<double> high = resample(tone(30000.0, 192000.0, 19200), 192000.0, 48000.0, true);
    const std::vector<double> low = resample(tone(1000.0, 192000.0, 19200), 192000.0, 48000.0, true);
    REQUIRE(high.size() == 4800);
    CHECK(rms(high) < 1e-3);
    CHECK_NEAR(rms(low), std::sqrt(0.5), 0.0, 1e-3);
}

TEST_CASE("playback: channels heard, level, clipping, what is said, sample formats", "[analysis]")
{
    const qacq::model::Signal s = makeSignal({tone(440.0, 48000.0, 4800, 2.0), tone(880.0, 48000.0, 4800, 0.5)}, 48000.0);
    PlaybackRequest r;
    r.channels = {0};
    r.outputRateHz = 48000.0;
    r.outputChannels = 2;
    PlaybackBuffer b = preparePlayback(s, r);
    REQUIRE(b.ok());
    CHECK(b.frames == 4800);
    CHECK(b.samples.size() == 9600);
    CHECK_NEAR(b.seconds, 0.1, 1e-12, 0.0);
    const auto peakOf = [&b](std::size_t channel, std::size_t channels) {
        double peak = 0.0;
        for (std::size_t f = 0; f < b.frames; ++f) peak = std::max(peak, std::abs(static_cast<double>(b.samples[f * channels + channel])));
        return peak;
    };
    const double top = std::pow(10.0, -1.0 / 20.0);
    CHECK_NEAR(peakOf(0, 2), top, 0.0, 1e-6); // normalized: the peak at −1 dBFS
    CHECK_NEAR(b.gainDb, -1.0 - 20.0 * std::log10(2.0), 0.0, 1e-6);
    CHECK(b.samples[200] == b.samples[201]); // mono on both sides
    CHECK(b.warnings.empty());

    // Stereo: the first channel on the left, the second on the right, the same gain.
    r.channels = {0, 1};
    b = preparePlayback(s, r);
    REQUIRE(b.ok());
    CHECK_NEAR(peakOf(0, 2), top, 0.0, 1e-6);
    CHECK_NEAR(peakOf(1, 2), top / 4.0, 0.0, 1e-6);
    // A card of one channel: their mean.
    r.outputChannels = 1;
    b = preparePlayback(s, r);
    REQUIRE(b.ok());
    CHECK(b.samples.size() == 4800);
    const PlaybackBuffer both = [&] {
        PlaybackRequest two = r;
        two.outputChannels = 2;
        return preparePlayback(s, two);
    }();
    CHECK_NEAR(static_cast<double>(b.samples[123]),
               0.5 * (static_cast<double>(both.samples[246]) + static_cast<double>(both.samples[247])), 0.0, 1e-6);

    // As it is: ±1 is full scale, beyond it clipped, and said.
    r.channels = {0};
    r.outputChannels = 2;
    r.normalized = false;
    b = preparePlayback(s, r);
    REQUIRE(b.ok());
    CHECK(b.clipped > 0);
    CHECK(peakOf(0, 2) == 1.0);
    CHECK(contains(b.warnings, "écrêtés"));
    const qacq::model::Signal quiet = makeSignal({tone(440.0, 48000.0, 480, 0.001)}, 48000.0);
    CHECK(contains(preparePlayback(quiet, r).warnings, "à peine audible"));

    // The start only of what is too long; what is beyond the card; what cannot be heard.
    r.normalized = true;
    r.maximumSeconds = 0.05;
    b = preparePlayback(s, r);
    CHECK(b.frames == 2400);
    CHECK(contains(b.warnings, "premières minutes"));
    r.maximumSeconds = 600.0;
    const qacq::model::Signal ultrasound = makeSignal({tone(100000.0, 1e6, 100000)}, 1e6);
    CHECK(contains(preparePlayback(ultrasound, r).warnings, "au-delà de"));
    r.channels = {5};
    CHECK(!preparePlayback(s, r).ok());
    r.channels = {0, 1, 0};
    CHECK(!preparePlayback(s, r).ok());
    r.channels = {0};
    CHECK(!preparePlayback(makeSignal({{}}, 48000.0), r).ok());

    // Stopped (another one asked meanwhile): nothing, and said so.
    const std::atomic<bool> stop{true};
    qacq::model::Progress stopping;
    stopping.cancelled = &stop;
    const PlaybackBuffer dropped = preparePlayback(ultrasound, r, stopping);
    CHECK(dropped.cancelled);
    CHECK(!dropped.ok());
    CHECK(resample(tone(1000.0, 10000.0, 10000), 10000.0, 48000.0, false, stopping).empty());

    // Sample formats, little endian.
    const std::vector<float> v = {0.0f, 1.0f, -1.0f, 0.5f};
    CHECK((encodeSamples(v, SampleFormat::UInt8) == std::vector<std::uint8_t>{128, 255, 1, 192}));
    CHECK((encodeSamples(v, SampleFormat::Int16) == std::vector<std::uint8_t>{0, 0, 0xFF, 0x7F, 0x01, 0x80, 0x00, 0x40}));
    const std::vector<std::uint8_t> i32 = encodeSamples(v, SampleFormat::Int32);
    CHECK((std::vector<std::uint8_t>(i32.begin() + 4, i32.begin() + 12) == std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0x80}));
    const std::vector<std::uint8_t> f32 = encodeSamples(v, SampleFormat::Float32);
    CHECK((std::vector<std::uint8_t>(f32.begin() + 4, f32.begin() + 8) == std::vector<std::uint8_t>{0x00, 0x00, 0x80, 0x3F}));
    CHECK(bytesPerSample(SampleFormat::Int16) == 2);
}
