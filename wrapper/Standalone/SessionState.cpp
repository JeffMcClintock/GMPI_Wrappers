#include "SessionState.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "StandaloneApp.h"
#include "StandaloneHost.h"
#include "StandaloneSettings.h"

#include "tinyXml2/tinyxml2.h"

namespace gmpi
{
namespace standalone
{

namespace
{

// The three filenames, as constants rather than literals at the call sites, so
// that the name a message says and the name a path is built from cannot drift.
// ASCII, and printed as-is: a message must never try to render the FOLDER,
// which contains the user's account name and cannot be narrowed without either
// throwing or substituting (see Settings::filePath for that whole trap). The
// folder is standalone.conf's, and saying so is enough to find it.
constexpr const char* kStateFile    = "session.xml";
constexpr const char* kPreviousFile = "session.previous.xml";
constexpr const char* kLoadingFile  = "session.loading";

// The identity this app stamps on a document it wrote, and tests before reading
// one back.
//
// PREFIXED, because the element is the SDK's. `<Preset>` is the format the VST3,
// CLAP and AU wrappers exchange with their hosts, and an unprefixed attribute on
// it is the SDK's to define - `pluginId` already is defined, and writing one
// flips both preset readers into their MIDI-learn format. Unknown attributes are
// ignored by both readers, so a document carrying these still loads anywhere a
// preset loads.
constexpr const char* kAttrPlugin  = "standalonePlugin";
constexpr const char* kAttrVersion = "standalonePluginVersion";

} // namespace

SessionState::SessionState(StandaloneHost& host, const Settings& settings)
    : host_(host)
    , statePath_(settings.siblingFile(kStateFile))
    , previousPath_(settings.siblingFile(kPreviousFile))
    , loadingPath_(settings.siblingFile(kLoadingFile))
    , pluginName_(host.pluginName())
    , pluginVersion_(host.pluginInfo() ? host.pluginInfo()->version : std::string{})
{
}

std::string SessionState::wrapWithIdentity(const std::string& presetXml) const
{
    tinyxml2::XMLDocument doc;
    if (tinyxml2::XML_SUCCESS != doc.Parse(presetXml.c_str(), presetXml.size()))
        return {};

    auto* preset = doc.FirstChildElement("Preset");
    if (!preset)
        return {};

    preset->SetAttribute(kAttrPlugin, pluginName_.c_str());
    preset->SetAttribute(kAttrVersion, pluginVersion_.c_str());

    tinyxml2::XMLPrinter printer;
    doc.Accept(&printer);

    return printer.CStr();
}

bool SessionState::identityMatches(const std::string& document, std::string& why) const
{
    why.clear();

    tinyxml2::XMLDocument doc;
    if (tinyxml2::XML_SUCCESS != doc.Parse(document.c_str(), document.size()))
    {
        why = "it is not valid XML";
        return false;
    }

    auto* preset = doc.FirstChildElement("Preset");
    if (!preset)
    {
        why = "it holds no <Preset> element";
        return false;
    }

    const char* name{};
    if (tinyxml2::XML_SUCCESS != preset->QueryStringAttribute(kAttrPlugin, &name) || !name)
    {
        // Every file this app writes carries the attribute, so one without it
        // came from somewhere else - a hand-copied DAW preset, most likely.
        // Refused rather than tried: a preset for another plugin parses
        // perfectly and applies its parameter ids to whatever they happen to
        // mean here, which is a worse outcome than starting at the defaults.
        why = "it does not say which plugin wrote it";
        return false;
    }

    if (pluginName_ != name)
    {
        why = std::string("it was written by '") + name + "'";
        return false;
    }

    const char* version{};
    if (tinyxml2::XML_SUCCESS == preset->QueryStringAttribute(kAttrVersion, &version)
        && version && pluginVersion_ != version)
    {
        // REPORTED, NEVER ACTED ON, and the returned value is still true.
        //
        // pluginInfo::version is a string the plugin's author declares and may
        // not declare at all, so a match proves nothing about whether the
        // parameters still line up, and a mismatch is usually just a release
        // the user installed. Refusing a patch on it would throw away good
        // patches at every update. What genuinely defends against a parameter
        // that changed type is a datatype tag per <Param>, and that belongs in
        // the shared format beside the shared writer, where the DAW wrappers
        // get it too.
        why = std::string("it was saved by version ") + version
            + ", and this is version " + pluginVersion_;
    }

    return true;
}

bool SessionState::parametersAreReadable(const std::string& document, std::string& why) const
{
    why.clear();

    tinyxml2::XMLDocument doc;
    if (tinyxml2::XML_SUCCESS != doc.Parse(document.c_str(), document.size()))
    {
        why = "it is not valid XML";
        return false;
    }

    auto* preset = doc.FirstChildElement("Preset");
    if (!preset)
    {
        why = "it holds no <Preset> element";
        return false;
    }

    // Direct children of <Preset>, which is what both readers walk. Every one
    // of them, not just the stateful ones the controller's reader keeps: the
    // processor's reader has no such filter and sees the whole document.
    for (auto* param = preset->FirstChildElement("Param"); param; param = param->NextSiblingElement("Param"))
    {
        // Missing, or not an integer. Either way the readers leave their handle
        // at -1 and assert on it, so neither reaches a parameter and neither
        // can be let through. A literal id="-1" is refused with them: the
        // readers cannot tell it apart from the attribute being absent.
        int id = -1;
        if (tinyxml2::XML_SUCCESS != param->QueryIntAttribute("id", &id) || id == -1)
        {
            why = "one of its parameters has no usable id";
            return false;
        }

        // No `val` at all is FINE and is not an error: both readers skip such a
        // <Param> before they touch the parameter. It is text of the wrong kind
        // that has to be stopped - "abc" against a number, most of all, which
        // is an assert in a debug build and a silent zero in a release one.
        if (const char* text = param->Attribute("val"); text && !host_.acceptsParameterText(id, text))
        {
            why = "the value it gives parameter " + std::to_string(id)
                + " is not one this plugin can read";
            return false;
        }
    }

    return true;
}

bool SessionState::moveAside(bool& moved, std::string& why)
{
    moved = false;
    why.clear();

    std::error_code ec;

    if (!std::filesystem::exists(statePath_, ec))
    {
        // Nothing to move, and nothing lost by not moving it. A fresh file is
        // owed all the same: the folder has no session in it, and a run that
        // then changed nothing would leave it that way.
        mustRewrite_ = true;
        return true;
    }

    // rename() onto an existing file is not portable enough to rely on; the
    // previous occupant is removed first so the move is a plain create.
    std::filesystem::remove(previousPath_, ec);
    std::filesystem::rename(statePath_, previousPath_, ec);

    if (ec)
    {
        // It would not move, so it is still lying at statePath_ with its
        // contents intact - and mustRewrite_ STAYS DOWN, which is the whole of
        // what "never deletes" is worth. Raised, it would force a write at the
        // next save even though nothing was edited, and that write would land
        // on the one file this call has just failed to preserve: the app would
        // report that it could not keep the patch and then destroy it in the
        // same run.
        //
        // Left down, saveNow compares against the baseline instead, and the
        // baseline is what the plugin is holding right now - so a run that
        // rejected this file and changed nothing rewrites nothing, and the file
        // is still there to look at afterwards. It is overwritten only once the
        // user has edited something, which is not this code losing a patch but
        // the user replacing one.
        why = ec.message();
        return false;
    }

    moved = true;

    // Moved out from under the folder, so the next save owes it a file even if
    // this run changes nothing.
    mustRewrite_ = true;
    return true;
}

void SessionState::quarantine(const std::string& why, PlatformShell& shell)
{
    bool moved = false;
    std::string moveError;

    if (!moveAside(moved, moveError))
    {
        shell.reportStatus(std::string("Session state: ") + kStateFile + " was not loaded - " + why
                         + " - and it could not be moved aside (" + moveError + ").");
        return;
    }

    if (!moved)
    {
        // The breadcrumb path can reach here with the state file already gone.
        // Still worth saying why the patch is not loading.
        shell.reportStatus(std::string("Session state: ") + kStateFile + " was not loaded - " + why + ".");
        return;
    }

    shell.reportStatus(std::string("Session state: ") + kStateFile + " was not loaded - " + why
                     + ". It has been kept as " + kPreviousFile + ", beside standalone.conf.");
}

void SessionState::keepCurrentAside(PlatformShell& shell)
{
    if (!enabled_)
        return; // no patch, so nothing to keep and no file to keep it in

    // The live patch, not whatever the last debounce happened to leave behind.
    saveNow(shell);

    bool moved = false;
    std::string moveError;

    if (!moveAside(moved, moveError))
    {
        shell.reportStatus(std::string("Session state: the previous patch could not be kept aside (")
                         + moveError + ").");
        return;
    }

    if (moved)
    {
        shell.reportStatus(std::string("Session state: the previous patch has been kept as ")
                         + kPreviousFile + ", beside standalone.conf.");
    }
}

void SessionState::takeBaseline()
{
    // What a save would produce RIGHT NOW, rather than the bytes just read off
    // the disk. The two can differ in formatting alone, and comparing against
    // this is what makes "nothing changed" mean nothing was written - including
    // the file's timestamp.
    baseline_ = wrapWithIdentity(host_.captureState());
}

void SessionState::restore(PlatformShell& shell)
{
    enabled_ = host_.hasStatefulParameters();
    if (!enabled_)
    {
        // A plugin with no patch: no file is read, written, or created.
        //
        // RESIDUAL: this returns in front of the breadcrumb check below, so a
        // session.loading left behind by an earlier BUILD of the same plugin -
        // one that still had stateful parameters - is never cleared and sits in
        // the folder for good. Harmless while the plugin stays patchless (this
        // class does nothing at all then), and cleared by the first launch of
        // one that is not, since that launch reaches the check. Clearing it here
        // would mean this branch touching the disk, which is the one thing it
        // promises not to do.
        return;
    }

    std::error_code ec;

    // A breadcrumb from the previous run means that run did not come back from
    // reading this file. The failure a return code cannot catch is an abort
    // inside the parse - a parameter that changed datatype under a reused id
    // trips an assert in GmpiParameter::setFromXml, and no catch(...) covers
    // that - so without this the app would die on every launch from now on, and
    // the only cure would be a user who knew to delete a file they have never
    // heard of.
    //
    // RESIDUAL, and it is a false positive rather than a missed one: the
    // breadcrumb says "somebody is loading", not "the previous run died". Two
    // copies of the same plugin launched together - a second instance started
    // while the first is still in restore() - and the second sees the first's
    // crumb and quarantines a file that is perfectly good. Costs a patch, not a
    // session, and only in a race that a person has to arrange; making it exact
    // means an owner in the file (a pid, a lock) and a story for a stale one,
    // which is more machinery than the fault deserves.
    if (std::filesystem::exists(loadingPath_, ec))
    {
        quarantine("loading it did not let the previous run finish starting", shell);
        std::filesystem::remove(loadingPath_, ec);
        takeBaseline();
        return;
    }

    if (!std::filesystem::exists(statePath_, ec))
    {
        // First run, or a folder somebody cleaned out. SILENT: there is nothing
        // wrong, and an app that announced its own defaults every launch would
        // be reporting the ordinary case.
        takeBaseline();
        return;
    }

    if (const auto size = std::filesystem::file_size(statePath_, ec); ec || size > kMaxBytes)
    {
        quarantine(ec ? "its size could not be read (" + ec.message() + ")"
                      : "it is far larger than a patch can be", shell);
        takeBaseline();
        return;
    }

    std::string document;
    {
        std::ifstream file(statePath_, std::ios::binary);
        if (!file)
        {
            quarantine("it could not be opened for reading", shell);
            takeBaseline();
            return;
        }

        document.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

        if (file.bad())
        {
            quarantine("reading it failed part way through", shell);
            takeBaseline();
            return;
        }
    }

    if (std::string why; !identityMatches(document, why))
    {
        quarantine(why, shell);
        takeBaseline();
        return;
    }
    else if (!why.empty())
    {
        shell.reportStatus("Session state: " + why + ". Loading it anyway.");
    }

    // And then what is INSIDE it, before any of it reaches the SDK. A file can
    // be perfectly well-formed XML, carry this plugin's name, and name a real
    // parameter, and still hold "abc" where a number belongs - at which point
    // the reader asserts rather than returning, and a debug build stops dead
    // with a modal dialog and no window ever appearing. Checked here so that
    // such a file gets the same treatment, and the same sentence, as a
    // truncated one.
    if (std::string why; !parametersAreReadable(document, why))
    {
        quarantine(why, shell);
        takeBaseline();
        return;
    }

    // Arm the breadcrumb, and note what happens when arming FAILS: the restore
    // goes ahead regardless. A protection that could not be put in place must
    // not become a reason to withhold the thing it protects - a read-only or
    // full config folder would otherwise cost the user their patch to defend
    // against a crash that has not happened.
    {
        std::ofstream crumb(loadingPath_, std::ios::binary | std::ios::trunc);
    }

    bool restored = false;
    try
    {
        restored = host_.restoreState(document);
    }
    catch (...)
    {
        // 562ad2a's rule, on this side of the fence: a patch we cannot read is a
        // lost patch and must never be a lost session. The store may be part
        // restored, which is untidy but not unsafe - every parameter in it holds
        // a value it accepted - and the file is moved aside below, so the next
        // launch starts clean.
        restored = false;
    }

    std::filesystem::remove(loadingPath_, ec);

    if (!restored)
        quarantine("the plugin would not take it", shell);

    takeBaseline();
}

void SessionState::markDirty()
{
    if (!dirty_)
    {
        dirty_ = true;
        dirtyAgeMs_ = 0; // the ceiling runs from the first unsaved edit
    }

    quietMs_ = 0;
}

void SessionState::pump(int elapsedMs, PlatformShell& shell)
{
    if (!enabled_ || !dirty_)
        return;

    quietMs_ += elapsedMs;
    dirtyAgeMs_ += elapsedMs;

    if (quietMs_ >= kQuietMs || dirtyAgeMs_ >= kCeilingMs)
        saveNow(shell);
}

void SessionState::saveNow(PlatformShell& shell)
{
    // First, and unconditionally: every path below is "this save has happened",
    // including the ones that decline to write. Leaving the flag up after a
    // no-op save would make the next tick try again, and the one after that.
    dirty_ = false;
    quietMs_ = 0;
    dirtyAgeMs_ = 0;

    if (!enabled_)
        return;

    std::string document;
    try
    {
        // The plugin may maintain part of its state lazily (a chunk parameter
        // refreshed on demand); this is the "imminent save" warning that makes
        // captureState() read CURRENT bytes. See StandaloneHost::syncPluginState.
        host_.syncPluginState();

        document = wrapWithIdentity(host_.captureState());
    }
    catch (...)
    {
        document.clear();
    }

    if (document.empty())
    {
        // Only reachable if the shared preset writer produced something that is
        // not a <Preset> document, which would be a defect in it rather than in
        // the patch. Reported rather than written: a file with no identity on it
        // is one the next launch would quarantine.
        shell.reportStatus(std::string("Session state: the plugin's patch could not be read, so ")
                         + kStateFile + " was not written.");
        return;
    }

    if (!mustRewrite_ && document == baseline_)
        return; // nothing has changed since the last write; do not touch the file

    if (writeDocument(document, shell))
    {
        baseline_ = document;
        mustRewrite_ = false;
    }
}

bool SessionState::writeDocument(const std::string& document, PlatformShell& shell)
{
    std::error_code ec;
    std::filesystem::create_directories(statePath_.parent_path(), ec);
    if (ec)
    {
        shell.reportStatus(std::string("Session state: the settings folder could not be created, so ")
                         + kStateFile + " was not written (" + ec.message() + ").");
        return false;
    }

    std::filesystem::path tmp = statePath_;
    tmp += ".tmp";

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            shell.reportStatus(std::string("Session state: ") + kStateFile + " could not be opened for writing.");
            return false;
        }

        file.write(document.data(), static_cast<std::streamsize>(document.size()));

        if (!file)
        {
            shell.reportStatus(std::string("Session state: writing ") + kStateFile + " failed part way through.");
            return false;
        }
    }

    std::filesystem::rename(tmp, statePath_, ec);
    if (ec)
    {
        // The half-written temporary is removed rather than left lying about;
        // the previous session.xml is untouched, which is the whole point of
        // writing through a temporary in the first place.
        std::filesystem::remove(tmp, ec);
        shell.reportStatus(std::string("Session state: ") + kStateFile + " could not be replaced.");
        return false;
    }

    return true;
}

} // namespace standalone
} // namespace gmpi
