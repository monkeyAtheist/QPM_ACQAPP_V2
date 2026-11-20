#pragma once

// Software trigger, as on an oscilloscope, on one channel of the stream.
//
// The trigger watches the samples as they arrive and reports captures: windows of `length` frames with
// `preTrigger` of them before the trigger instant. A capture is reported once all its frames have
// arrived; the caller copies them from the recorder. Hysteresis: a rising edge counts only after the
// signal went below level - hysteresis (noise around the level does not re-trigger). The instant is
// interpolated between the two samples around the crossing, so successive captures can be aligned to a
// fraction of a sample.
// Modes: Normal (only on a trigger), Auto (also a free capture after autoTimeout frames without one),
// Single (one capture, then disarmed until arm()).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qacq::acquisition
{
enum class TriggerEdge
{
    Rising,
    Falling,
    Either
};

enum class TriggerMode
{
    Auto,
    Normal,
    Single
};

struct TriggerSettings
{
    std::size_t channel = 0;
    double level = 0.0;
    double hysteresis = 0.0; // absolute, in the unit of the channel
    TriggerEdge edge = TriggerEdge::Rising;
    TriggerMode mode = TriggerMode::Auto;
    std::size_t length = 1000;   // frames per capture
    double preTrigger = 0.25;    // fraction of the capture before the trigger instant, 0..1
    std::size_t holdoff = 0;     // frames after a capture's trigger before the next one may start
    std::size_t autoTimeout = 0; // Auto: frames without a trigger before a free capture (0: one capture length)
};

struct TriggerCapture
{
    std::uint64_t firstFrame = 0;
    std::size_t length = 0;
    std::uint64_t triggerFrame = 0; // first sample past the crossing
    double triggerFraction = 0.0;   // crossing at triggerFrame - 1 + fraction
    bool forced = false;            // Auto mode, no trigger
    bool rising = true;
};

class Trigger
{
public:
    explicit Trigger(const TriggerSettings &settings = {}) { configure(settings); }

    void configure(const TriggerSettings &settings);
    const TriggerSettings &settings() const noexcept { return m_settings; }
    void arm();                    // re-arms a single capture (and restarts the auto timeout)
    bool armed() const noexcept { return m_armed; }

    // Samples of the trigger channel, frames [firstFrame, firstFrame + samples.size()), in order and without
    // gap (after a gap, call reset()). Returns the captures completed by these samples.
    std::vector<TriggerCapture> process(std::span<const double> samples, std::uint64_t firstFrame);
    void reset(); // forget the stream state (keeps the settings and the armed state)

private:
    std::size_t preFrames() const noexcept;

    TriggerSettings m_settings;
    bool m_armed = true;
    bool m_haveLast = false;
    double m_last = 0.0;
    bool m_belowArmed = false; // rising edge: went below level - hysteresis
    bool m_aboveArmed = false; // falling edge: went above level + hysteresis
    std::uint64_t m_streamStart = 0;    // first frame seen since the reset
    std::uint64_t m_nextAllowed = 0;    // no trigger before this frame (holdoff, no overlap)
    std::uint64_t m_lastCaptureEnd = 0; // auto timeout counts from here
    bool m_started = false;
    std::vector<TriggerCapture> m_pending; // triggered, waiting for their post-trigger frames
};
} // namespace qacq::acquisition
