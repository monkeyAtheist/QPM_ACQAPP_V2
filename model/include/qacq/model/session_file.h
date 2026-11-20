#pragma once

// Session file (.qacq): the signals of a session and the settings of the modules, in one file.
//
//   offset 0   "QACQSESS"                  8 bytes
//          8   format version              uint32, little endian (1)
//         12   header length H             uint64, little endian
//         20   header                      H bytes of UTF-8 JSON, readable in any text editor
//    20 + H    padding to a multiple of 8  zero bytes
//          D   samples                     channel after channel, little endian
//
// The header lists the signals (name, origin, rate, channels with names and units, discontinuities,
// notes, provenance) and, for each channel, where its samples are: offset from D, encoding ("f64le", or
// "f32le" when every value of the channel is exactly a float, as raw ADC counts are), byte count and
// CRC-32. A damaged channel is reported, never read back as wrong values.
// Writing goes to "<file>.tmp" first, then replaces the file: a failed save never destroys the previous one.

#include "qacq/model/json.h"
#include "qacq/model/signal.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace qacq::model
{
inline constexpr std::uint32_t kSessionFormatVersion = 1;

struct SessionContent
{
    std::vector<SignalPtr> signalList; // not "signals": a Qt macro
    Json settings = Json::object(); // settings of the modules, free form
    std::string application;        // writer, e.g. "QTacquisitionApp 2.0.0" (read back for information)
    std::string savedUtc;           // set by the caller
};

struct SessionWriteOptions
{
    bool float32WhenExact = true; // halves the size of integer-valued and float32 acquisitions, losslessly
};

// Empty on success, else the reason (in French).
std::string writeSession(std::ostream &out, const SessionContent &content, const SessionWriteOptions &options = {});
std::string saveSession(const std::filesystem::path &file, const SessionContent &content,
                        const SessionWriteOptions &options = {});

std::optional<SessionContent> readSession(std::istream &in, std::string *error = nullptr);
std::optional<SessionContent> loadSession(const std::filesystem::path &file, std::string *error = nullptr);

// Metadata of a signal as stored in the header (without the sample locations), and back. Also used by
// the clipboard and the "signal information" view.
Json signalToJson(const Signal &signal);
bool signalFromJson(const Json &json, Signal &signal, std::string *error = nullptr); // data not set
} // namespace qacq::model
