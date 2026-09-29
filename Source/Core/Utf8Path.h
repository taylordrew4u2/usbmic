#pragma once
#include <filesystem>
#include <string>

namespace mma {

/// Every path in Core is carried as a UTF-8 std::string (it comes from
/// juce::String::toStdString, from session.json, from the settings file).
///
/// Handing one of those straight to std::fstream, std::filesystem or a narrow
/// Win32 call is correct everywhere except Windows, where a narrow path is read
/// in the ANSI code page: "C:\Users\Zo\xC3\xAB" becomes "C:\Users\ZoÃ«", a folder
/// that does not exist, and every take under it fails to start. Convert here,
/// once, and pass the path object on.
inline std::filesystem::path pathFromUtf8 (const std::string& utf8)
{
    return std::filesystem::u8path (utf8);
}

/// The inverse, for a path that came back from the filesystem (a directory
/// listing, a parent_path) and is going back into a std::string. path::string()
/// on Windows converts to the ANSI code page and throws on anything it cannot
/// represent.
inline std::string utf8FromPath (const std::filesystem::path& path)
{
    const auto u8 = path.u8string(); // std::string in C++17, std::u8string in C++20
    return std::string (u8.begin(), u8.end());
}

} // namespace mma
