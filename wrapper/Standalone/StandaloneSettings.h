#pragma once

// Persisted device selection for the standalone app.
//
// A flat key=value text file, because that is genuinely all this needs: a
// handful of device ids and three numbers. Pulling in XML (tinyxml2 is already
// here for the plugin's spec) or JSON would buy nothing and would mean the
// wrapper's simplest component had the largest dependency.
//
// Location follows each platform's convention, keyed on the plugin's own name
// so two standalones built from different plugins do not share settings:
//
//   Linux    $XDG_CONFIG_HOME/<plugin>/standalone.conf   (~/.config/... )
//   Windows  %APPDATA%\<plugin>\standalone.conf
//   macOS    ~/Library/Application Support/<plugin>/standalone.conf
//
// Unknown keys are preserved on save. That matters more than it looks: a
// newer build writing a key an older build does not understand must not have
// it silently deleted the next time the old build saves.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gmpi
{
namespace standalone
{

class Settings
{
public:
    // `pluginName` comes from the plugin's own XML, so the folder is named
    // after the product rather than after this wrapper.
    explicit Settings(const std::string& pluginName);

    void load();
    void save() const;

    // Where load()/save() read and write. Nothing calls this yet; it is here so
    // that whatever first needs to name the file - a "settings not saved"
    // message is useless without the path - inherits a settled answer instead
    // of narrowing path_ itself and meeting the trap below.
    //
    // UTF-8 on every platform, not the local narrow encoding: on Windows the
    // path is natively UTF-16 and nothing narrower than UTF-8 holds all of it,
    // because the path contains the user's account name and the machine's ANSI
    // codepage may have no representation for those characters at all. On POSIX
    // the native form is already bytes that are UTF-8 by convention, so there
    // this relabels rather than converts.
    //
    // Never throws: a unit it cannot encode becomes U+FFFD rather than an
    // exception, which matters because the callers this is for are already
    // reporting a failure when they reach for it.
    std::string filePath() const;

    // --- typed accessors -------------------------------------------------
    // Every getter takes the fallback used when the key is absent or garbage,
    // so a truncated or hand-edited file degrades to defaults rather than to
    // zeros (a sample rate of 0 would divide by zero in latency reporting).

    std::string getString(const std::string& key, const std::string& fallback = {}) const;
    int         getInt(const std::string& key, int fallback) const;
    bool        getBool(const std::string& key, bool fallback) const;

    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setBool(const std::string& key, bool value);

    // MIDI inputs are a list; stored as one line with '\x1f' between entries
    // rather than as indexed keys, so removing a device cannot leave an
    // orphaned "midiIn3=" behind.
    std::vector<std::string> getStringList(const std::string& key) const;
    void setStringList(const std::string& key, const std::vector<std::string>& values);

    // The keys this app uses. Named constants rather than string literals at
    // the call sites so a typo is a link error, not a silently-lost setting.
    static constexpr const char* keyAudioDevice   = "audioDevice";
    static constexpr const char* keySampleRate    = "sampleRate";
    static constexpr const char* keyBufferFrames  = "bufferFrames";
    static constexpr const char* keyMidiInputs    = "midiInputs";
    static constexpr const char* keyMidiInputsSet = "midiInputsConfigured";

private:
    std::filesystem::path path_;
    std::map<std::string, std::string> values_;
};

} // namespace standalone
} // namespace gmpi
