#include "StandaloneSettings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>

// The one UTF-8 conversion, shared with windows/MidiDriverWin.cpp. Inside the
// arm rather than at the top of the file because only Windows has anything to
// convert: path::native() is a wide string only here.
#include "helpers/unicode_conversion.h"
#endif

namespace gmpi
{
namespace standalone
{

namespace
{

// The separator inside a stored list. ASCII 31 (unit separator) is the one
// character guaranteed not to appear in a device name or an ALSA client id,
// which a comma or a colon very much can ("Scarlett 2i2 USB, MIDI 1").
constexpr char kListSeparator = '\x1f';

std::filesystem::path configRoot()
{
    // BACKLOG E55 -- ONE OVERRIDE, CHECKED BEFORE THE PER-PLATFORM ARMS.
    //
    // The three arms below are not equivalent: mac reads $HOME and linux reads
    // $XDG_CONFIG_HOME, so a test run on either can point the config root at a
    // scratch dir and leave the developer's own folder untouched -- S23
    // measured exactly that, byte-identical before and after. Windows calls
    // SHGetKnownFolderPath and consults nothing, not even %APPDATA%, so a
    // Windows reproduction has to back up the real folder and restore it.
    //
    // That asymmetry is not theoretical. It cost a session on 2026-08-28: E53
    // could not be re-measured on the windows box because a concurrent session
    // held that one folder, and two processes cannot both own it.
    //
    // So the seam is ONE rule rather than three, and it is checked FIRST --
    // before the platform arms, so its behaviour does not depend on which
    // platform's fallbacks happen to be set. The name matches the existing
    // GMPI_STANDALONE_IPC_DIR (mcp/IpcServer.h), which redirects the command
    // channel's socket directory for the same reason.
    //
    // Deliberately NOT created if missing: a typo in the variable should fail
    // loudly where the caller opens the file, not silently start a fresh
    // config tree somewhere unexpected.
    if (const char* override_ = std::getenv("GMPI_STANDALONE_CONFIG_DIR"); override_ && *override_)
        return std::filesystem::path(override_);

#ifdef _WIN32
    if (PWSTR appData{}; SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
    {
        std::filesystem::path p{ appData };
        CoTaskMemFree(appData);
        return p;
    }
    return std::filesystem::temp_directory_path();
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support";
    return std::filesystem::temp_directory_path();
#else
    // The XDG basedir spec: honour XDG_CONFIG_HOME, fall back to ~/.config.
    // Both can be unset in a service or a bare test harness, hence the last
    // resort - a settings file in /tmp still beats crashing on a null path.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg);
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config";
    return std::filesystem::temp_directory_path();
#endif
}

// A plugin name goes into a path, so anything a filesystem would object to
// (or that would let a crafted name escape the config folder) is replaced.
std::string sanitiseFolderName(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
        out += ok ? c : '_';
    }

    // Leading dots would make the folder hidden on unix and trailing spaces
    // are illegal on Windows; an empty name would put the file in the root.
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
        out.erase(out.begin());
    while (!out.empty() && out.back() == ' ')
        out.pop_back();

    return out.empty() ? std::string{ "GMPI Standalone" } : out;
}

} // namespace

Settings::Settings(const std::string& pluginName)
{
    const auto dir = configRoot() / sanitiseFolderName(pluginName);
    path_ = dir / "standalone.conf";
}

void Settings::load()
{
    values_.clear();

    std::ifstream file(path_);
    if (!file)
        return; // first run: defaults everywhere, not an error

    std::string line;
    while (std::getline(file, line))
    {
        // Tolerate a file written on the other kind of platform. The app is
        // cross-platform and its config folder can live on a synced drive.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty() || line[0] == '#')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Settings::save() const
{
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec)
        return;

    // Write-then-rename, so a crash or a full disk mid-write leaves the
    // previous settings intact rather than a half file that load() would
    // read as "no audio device configured".
    std::filesystem::path tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file)
            return;

        file << "# GMPI standalone settings. Rewritten by the app; comments are not preserved.\n";
        for (const auto& [key, value] : values_)
            file << key << '=' << value << '\n';

        if (!file)
            return;
    }

    std::filesystem::rename(tmp, path_, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

std::string Settings::filePath() const
{
#ifdef _WIN32
    // Neither path::string() nor path::u8string(). MSVC's string() narrows
    // through the system ANSI codepage, refuses a best-fit substitution, and
    // THROWS a std::system_error when a character has no representation there -
    // so on a machine whose locale does not cover the account name (a Cyrillic
    // or CJK user on a western-locale install) merely asking where the settings
    // live would be an exception. u8string() fixes that case but not the class:
    // it throws the identical ERROR_NO_UNICODE_TRANSLATION on an unpaired
    // surrogate, and a Windows filename is only WTF-16, so a lone surrogate is
    // a legal name the STL refuses to encode. to_utf8 is WideCharToMultiByte
    // WITHOUT WC_ERR_INVALID_CHARS, so the API substitutes U+FFFD there rather
    // than failing, which is the answer this getter wants: its callers are the
    // ones reporting some other problem, and a path shown with a replacement
    // character in it beats no path.
    return gmpi::unicode::to_utf8(path_.native());
#else
    // A POSIX path is stored as the bytes the OS gave us, which is already the
    // UTF-8 asked for; u8string() would relabel these same bytes.
    return path_.native();
#endif
}

std::filesystem::path Settings::siblingFile(const char* fileName) const
{
    return path_.parent_path() / fileName;
}

std::string Settings::getString(const std::string& key, const std::string& fallback) const
{
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Settings::getInt(const std::string& key, int fallback) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
        return fallback;

    try
    {
        size_t consumed = 0;
        const int v = std::stoi(it->second, &consumed);
        // Reject "48000abc": a partially-numeric value is a corrupt file, and
        // silently taking the numeric prefix hides that.
        return consumed == it->second.size() ? v : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

bool Settings::getBool(const std::string& key, bool fallback) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
        return fallback;

    return it->second == "1" || it->second == "true";
}

void Settings::setString(const std::string& key, const std::string& value)
{
    values_[key] = value;
}

void Settings::setInt(const std::string& key, int value)
{
    values_[key] = std::to_string(value);
}

void Settings::setBool(const std::string& key, bool value)
{
    values_[key] = value ? "1" : "0";
}

std::vector<std::string> Settings::getStringList(const std::string& key) const
{
    std::vector<std::string> out;

    const auto joined = getString(key);
    if (joined.empty())
        return out;

    std::string item;
    std::istringstream stream(joined);
    while (std::getline(stream, item, kListSeparator))
    {
        if (!item.empty())
            out.push_back(item);
    }

    return out;
}

void Settings::setStringList(const std::string& key, const std::vector<std::string>& values)
{
    std::string joined;
    for (const auto& v : values)
    {
        if (!joined.empty())
            joined += kListSeparator;
        joined += v;
    }

    setString(key, joined);
}

} // namespace standalone
} // namespace gmpi
