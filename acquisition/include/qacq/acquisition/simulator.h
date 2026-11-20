#pragma once

// A simulated device: what a microcontroller board or an instrument would send, byte for byte, so the
// whole chain (source, decoder, pipe, recorder, display) runs without hardware: in the tests, in
// demonstrations, and to try a frame format before writing the firmware.
//
// Each channel is a waveform of the generator plus optional noise. The bytes follow the decoder settings
// of the acquisition: text lines, raw binary, framed binary (with a counter and a checksum) or SCPI
// answers. Defects can be injected: damaged bytes (one bit flipped) and lost frames (the counter still
// advances, as on a real link). Everything is reproducible from the seed.

#include "qacq/acquisition/settings.h"
#include "qacq/dsp/random.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::acquisition
{
class SimulatedDevice
{
public:
    SimulatedDevice(const SimulatedSettings &settings, const DecoderSettings &output, double sampleRateHz);

    std::size_t channelCount() const noexcept { return m_settings.channels.size(); }
    double sampleRateHz() const noexcept { return m_rate; }

    // Bytes of the next `frames` frames.
    std::vector<std::uint8_t> produce(std::size_t frames);
    // Bytes for `seconds` of the simulated clock (the fraction of a frame left is kept for next time).
    std::vector<std::uint8_t> produceFor(double seconds);
    std::uint64_t framesProduced() const noexcept { return m_next; }
    std::uint64_t framesLost() const noexcept { return m_lost; }

    // Exact value of a channel at a frame (before noise and defects), for the tests.
    double cleanValue(std::size_t channel, std::uint64_t frame) const;

    // SCPI instrument: answer to a command line (without its newline); empty when the command expects
    // no answer. *IDN?, *RST, SYST:ERR?, MEAS? / READ? (one frame), :WAV:DATA? / CURV? (block of
    // `waveformPoints` frames in the block format), anything else: error -113 queued.
    std::string answer(std::string_view command);
    std::size_t waveformPoints = 1000;

private:
    double value(std::size_t channel, std::uint64_t frame);
    void appendFrame(std::uint64_t frame, std::vector<std::uint8_t> &out);
    void damage(std::vector<std::uint8_t> &bytes, std::size_t from);

    SimulatedSettings m_settings;
    DecoderSettings m_output;
    double m_rate;
    double m_clockRate; // rate of the simulated board's own clock (rateErrorPpm)
    dsp::Random m_noise;
    dsp::Random m_defects;
    std::uint64_t m_next = 0;
    std::uint64_t m_lost = 0;
    double m_carry = 0.0;
    std::vector<std::string> m_errors;
};
} // namespace qacq::acquisition
