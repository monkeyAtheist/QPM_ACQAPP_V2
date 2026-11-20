#pragma once

// Example firmware for a microcontroller board that sends exactly the format the acquisition expects:
// an Arduino sketch (analogRead on A0, A1...) or portable C (a function that builds a frame, for STM32,
// ESP32, PIC...). The checksum routine is generated from the same CRC parameters the decoder uses, as a
// bitwise loop with no table: small and easy to check against the catalogue value.

#include "qacq/acquisition/settings.h"

#include <string>

namespace qacq::acquisition
{
enum class FirmwareTarget
{
    Arduino,  // complete sketch: setup(), loop(), timing with micros()
    PortableC // frame builder in plain C99 + a usage example in comments
};

// channels and sampleRateHz describe what the board samples; baudRate is used by the Arduino sketch.
std::string firmwareExample(const DecoderSettings &format, FirmwareTarget target, std::size_t channels,
                            double sampleRateHz, int baudRate);
} // namespace qacq::acquisition
