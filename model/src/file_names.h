#pragma once

#include <filesystem>
#include <string>

namespace qacq::model::detail
{
// File name for the messages, in UTF-8 whatever the system (path::string() may fail on Windows).
inline std::string displayName(const std::filesystem::path &file)
{
    const std::u8string name = file.filename().u8string();
    return std::string(reinterpret_cast<const char *>(name.data()), name.size());
}
} // namespace qacq::model::detail
