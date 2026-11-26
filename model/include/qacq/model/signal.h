#pragma once

// A signal of the session: one or more channels sampled together at a constant rate.
//
// Signals are immutable values shared through std::shared_ptr<const Signal>, and their samples are
// shared again between versions (std::shared_ptr<const SampleData>): renaming a signal of ten million
// samples copies a few strings, and an undo history of fifty steps costs nothing more than the data that
// really changed.

#include "qacq/model/json.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qacq::io
{
struct SampleBlock;
}

namespace qacq::model
{
enum class Origin
{
    Acquired,  // from a serial port, a socket, an instrument, the sound card
    Generated, // generator, modulator
    Imported,  // CSV, WAV, an old session
    Derived    // result of a processing step on another signal (filter, demodulation...)
};

std::string_view originKey(Origin origin) noexcept;  // "acquired"... (files)
std::string_view originName(Origin origin) noexcept; // "Acquis"... (interface)
bool originFromKey(std::string_view key, Origin &origin) noexcept;

enum class Representation
{
    Real,
    IQ // complex baseband: channels taken in pairs (I, Q)
};

struct ChannelInfo
{
    std::string name;
    std::string unit; // SI symbol as shown ("V", "A", "m/s²"), empty for a bare number
};

// The samples, channel by channel (each channel contiguous: what the processing functions take).
struct SampleData
{
    std::vector<std::vector<double>> channels;
    // Frame indices before which samples are missing or the record restarts (from the decoders).
    std::vector<std::size_t> discontinuities;

    std::size_t channelCount() const noexcept { return channels.size(); }
    std::size_t frameCount() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
    std::size_t byteSize() const noexcept { return channelCount() * frameCount() * sizeof(double); }
};

struct Signal
{
    std::uint64_t id = 0; // given by the document, unique in a session
    std::string name;
    Origin origin = Origin::Generated;
    Representation representation = Representation::Real;
    double sampleRateHz = 1.0;
    double startSeconds = 0.0; // time of the first sample
    std::string createdUtc;    // ISO 8601, set by the caller ("2026-11-20T11:17:03Z")
    std::string notes;
    std::vector<ChannelInfo> channels;
    std::shared_ptr<const SampleData> data;
    // Where it comes from: acquisition settings, or {"operation", "source", "parameters"} for a derived
    // signal. Free form, kept in the session file.
    Json provenance;

    std::size_t channelCount() const noexcept { return channels.size(); }
    std::size_t frameCount() const noexcept { return data ? data->frameCount() : 0; }
    double durationSeconds() const noexcept
    {
        return sampleRateHz > 0.0 ? static_cast<double>(frameCount()) / sampleRateHz : 0.0;
    }
    std::span<const double> channel(std::size_t index) const noexcept;
};

using SignalPtr = std::shared_ptr<const Signal>;

// Empty when the signal is consistent, else the reason (in French): channel count of the data and of the
// descriptions, equal channel lengths, positive finite rate, discontinuities in range, I/Q pairs.
std::string validateSignal(const Signal &signal);

// Channel-major copy of decoded frames (interleaved). The discontinuities are kept.
std::shared_ptr<SampleData> deinterleave(const io::SampleBlock &block);
std::shared_ptr<SampleData> deinterleave(std::span<const double> interleaved, std::size_t channels);

// The signal with only those of its channels, in the order given (each named once): the others, and their
// samples, are left out (the samples kept are copied; id, name, timing, provenance stay). An I/Q signal that no
// longer is made of whole pairs (I then Q, as they were) becomes a real one. What goes is said at the end of
// its notes ("Voie retirée : sin."). False, the signal unchanged, with why (in French): no channel named, one
// that does not exist or named twice.
bool keepChannels(Signal &signal, std::span<const std::size_t> channels, std::string *error = nullptr);

// Default names "Voie 1", "Voie 2"... ("I", "Q" for an I/Q pair).
std::vector<ChannelInfo> defaultChannels(std::size_t count, std::string_view unit = {},
                                         Representation representation = Representation::Real);
} // namespace qacq::model
