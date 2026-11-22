#pragma once

// A signal made ready for a sound card: the channels heard (one: on every output channel; two: left and
// right), brought to the rate of the card (windowed-sinc interpolation, cut at 0.45 of the lower rate, so
// nothing folds back when the rate goes down), to its level (the peak at −1 dBFS, or as it is: ±1 in the
// unit of the signal is full scale, beyond it clipped), then encoded in the sample format of the card.
// Without Qt: qacq_app plays it through Qt Multimedia (AudioPlayer).

#include "qacq/model/progress.h"
#include "qacq/model/signal.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qacq::analysis
{
enum class SampleFormat
{
    UInt8,
    Int16,
    Int32,
    Float32
};

struct PlaybackRequest
{
    std::vector<std::size_t> channels{0}; // of the signal: one (mono) or two (left, right)
    double outputRateHz = 48000.0;
    std::size_t outputChannels = 2; // of the card
    bool normalized = true;         // the peak at −1 dBFS; otherwise as it is
    bool periodic = false;          // played in a loop: the resampling joins the end to the start
    double maximumSeconds = 600.0;  // beyond, the start only
};

struct PlaybackBuffer
{
    std::vector<float> samples; // interleaved, outputChannels a frame, within [−1, 1]
    std::size_t frames = 0;
    double seconds = 0.0;  // heard
    double gainDb = 0.0;   // applied (normalized)
    std::size_t clipped = 0; // samples beyond full scale (as it is), set to ±1
    std::vector<std::string> warnings;
    std::string error;
    bool cancelled = false;

    bool ok() const noexcept { return error.empty() && !cancelled; }
};

PlaybackBuffer preparePlayback(const model::Signal &signal, const PlaybackRequest &request,
                               const model::Progress &progress = {});

// One channel from one rate to another: windowed sinc (Kaiser, β = 8, 24 zero crossings on each side), cut
// at 0.45 of the lower rate. Periodic: the samples before the start are those of the end, and after it.
// Equal rates: a copy. round(n × out/in) samples; none if stopped (`progress`, looked at every 4 096).
std::vector<double> resample(std::span<const double> samples, double inputRateHz, double outputRateHz,
                             bool periodic = false, const model::Progress &progress = {});

// [−1, 1] in a sample format, little endian: 8 bits unsigned (128 = 0), 16 and 32 bits signed, 32-bit float.
std::size_t bytesPerSample(SampleFormat format) noexcept;
std::vector<std::uint8_t> encodeSamples(std::span<const float> samples, SampleFormat format);
} // namespace qacq::analysis
