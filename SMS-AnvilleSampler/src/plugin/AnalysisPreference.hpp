#pragma once

#include "IO/DefaultsFile.hpp"

namespace midichopper::plugin {

// Whether header sound detection is ON: a per-user UI preference, never project
// state. Call from the UI thread only.
class AnalysisPreference {
public:
    [[nodiscard]] static std::filesystem::path configPath() noexcept
    {
        return sms::io::DefaultsFile::configPath("SMS-AnvilleSampler");
    }

    [[nodiscard]] static bool read(const std::filesystem::path& path) noexcept
    {
        try {
            return sms::io::DefaultsFile::get(path, "analysis") == "on";
        } catch (...) { return false; }
    }

    [[nodiscard]] static bool write(const std::filesystem::path& path, const bool on) noexcept
    {
        try {
            return sms::io::DefaultsFile::set(path, "analysis", on ? "on" : "off");
        } catch (...) { return false; }
    }
};

} // namespace midichopper::plugin
