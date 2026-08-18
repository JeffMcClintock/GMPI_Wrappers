#pragma once

// The plugin's patch, kept across runs.
//
// A DAW does this for the VST3 and CLAP builds of the same plugin without
// anybody thinking about it: it calls getState when the project is saved and
// setState when it is reopened. A standalone has no DAW, so until now it had
// nothing - dial in a sound, quit, and it was gone. This is the missing half,
// and it is deliberately the SAME state through the SAME format, so the patch a
// standalone leaves behind is the patch a wrapper would have handed a host.
//
// AUTOMATIC, and only automatic. There is no Save button and no file chooser:
// one session, restored on launch, written as you go. Named presets are a
// separate feature and are deliberately not here - see the note at the bottom.
//
// WHAT IT IS NOT. Window size and position are the shell's business, not the
// plugin's, and are not kept here. Neither are the audio device settings, which
// have their own file and had it first (StandaloneSettings.h).
//
// WHEN IT WRITES. Both of the moments that can lose work, because a standalone
// is killed at least as often as it is quit:
//
//   * shortly after an edit, once the edits stop (a debounce), so an hour of
//     tweaking survives a crash or a `kill -9`
//   * at quit, from the one teardown sequence in StandaloneApp.cpp
//
// It never writes a file it did not have to: a run that changed nothing leaves
// the previous file untouched, byte for byte and timestamp included, and a
// plugin with no stateful parameters never creates one at all.
//
// WHEN IT WILL NOT LOAD. A saved patch is worth something; a running app is
// worth more. Every failure here ends with the app up and the plugin at its
// defaults, and the file that could not be read moved aside rather than
// deleted, so it is still there to look at afterwards. The precedent is
// GMPI_Wrappers 562ad2a, "never let a bad preset abort the host" - a lost patch
// must never be a lost session.
//
// The failure a return code cannot cover is a crash INSIDE the load: the SDK's
// preset readers assert on a value they cannot use rather than returning, and
// no catch(...) covers an assert. Two answers, in this order.
//
// The document is CHECKED FIRST - parametersAreReadable, below - so the file
// that would have done it never reaches the readers. A well-formed <Preset>
// with this plugin's name on it and `val="abc"` against a number is the case
// that matters, because in a debug build it is a process with no window, no
// command channel and a modal dialog nobody can see, and in a release build it
// is a garbage patch that silently zeroes the parameter.
//
// And a breadcrumb file behind that, for whatever was not anticipated: written
// before the load, removed after, so a launch that finds one knows the last
// launch died trying and skips the file instead of dying the same way forever.
//
// PORTABLE. No platform header reaches this file, and none needs to: where the
// files live is Settings::siblingFile's answer, and what is worth saving is
// StandaloneHost's. The three shells do not know this feature exists.
//
// THREADING. Everything here runs on the main thread, inside the app's tick or
// its teardown. That is not a convenience: StandaloneHost::captureState reads
// the CONTROLLER's parameter store precisely because the processor's is written
// by the audio thread, and a save must never touch the audio thread or race a
// device being closed.

#include <filesystem>
#include <string>

namespace gmpi
{
namespace standalone
{

class PlatformShell;
class Settings;
class StandaloneHost;

class SessionState
{
public:
    // `settings` is asked only where its folder is; nothing is read from it and
    // no key is written to it, so standalone.conf is not touched by this feature
    // at all. The patch is a large binary-ish document and that file is a short
    // list of device names - two shapes with no business in one file.
    SessionState(StandaloneHost& host, const Settings& settings);

    // Load the saved patch, or start at the plugin's defaults.
    //
    // CALL BEFORE startAudio() - the host writes the DSP's parameter store
    // directly, which is only safe while no audio callback can run.
    //
    // Call it before the window is sized, too. The editor's preferred size can
    // depend on its patch, and measuring it on the defaults and then restoring
    // would give a window fitted to a patch the user never had.
    //
    // Silent when there is nothing to load, which is the ordinary first run.
    // Anything else it has to say goes to shell.reportStatus - a line for
    // whoever is reading a log, never a dialog: nothing here is worth stopping
    // a startup for.
    void restore(PlatformShell& shell);

    // A parameter was edited. Cheap - it sets a flag and restarts the debounce.
    void markDirty();

    // Called once per tick from the app's loop. Writes the file when the edits
    // have stopped for long enough, or when they have been going on long enough
    // that waiting for a gap would risk losing the lot.
    //
    // RESIDUAL: that write is synchronous file I/O on the UI THREAD, into the
    // user's roaming profile - which on a managed Windows box can be folder
    // redirection to a network share, or a OneDrive folder that syncs on close.
    // A slow one stalls the frame the debounce happened to expire on. Accepted
    // for now because the debounce is what keeps it rare (one write per gap in
    // the editing, never one per frame) and a patch is a few kilobytes; the
    // answer if it ever bites is a writer thread, which needs a snapshot of the
    // document and a story for a save still in flight at quit.
    void pump(int elapsedMs, PlatformShell& shell);

    // Write now, if there is anything new to write. Called from the teardown,
    // which is what catches the values only the DSP authored - those never mark
    // the session dirty, because a meter would keep it permanently so.
    void saveNow(PlatformShell& shell);

    // Save the current patch and then move it aside, so that whatever replaces
    // it is not the only thing left. What File > Revert to Plugin Defaults
    // calls before it reverts: revert is the one action here that deliberately
    // throws a sound away, and it is the one that most needs an undo.
    //
    // Saves FIRST rather than just renaming whatever is on disk, because
    // session.xml can be up to a debounce behind the live patch, and the sound
    // the user is reverting is the live one.
    void keepCurrentAside(PlatformShell& shell);

private:
    // Wraps a captured <Preset> in this app's identity attributes. Prefixed
    // names, because that element belongs to the SDK and an unprefixed name on
    // it is the SDK's to define - `pluginId` in particular already means
    // something to both preset readers.
    std::string wrapWithIdentity(const std::string& presetXml) const;

    // True when the document's root names THIS plugin. `why` is filled in when
    // it does not, and with anything else worth saying (a version that differs)
    // when it does.
    bool identityMatches(const std::string& document, std::string& why) const;

    // True when the SDK's readers can be handed this document without one of
    // them ASSERTING on it. `why` is filled in when they cannot.
    //
    // The one class of bad file a return code could not have caught, and the
    // reason it needs catching HERE rather than in the reader: setFromXml and
    // the <Param> loops around it treat a value of the wrong kind as a defect
    // in whatever wrote the preset, and say so with assert(). That is the right
    // answer for a chunk handed over by a DAW, which came from code; it is the
    // wrong answer for a file, which came from a disk that anybody can edit.
    // Changing the readers would change what every DAW wrapper does with a
    // corrupt project, so the file's reader carries the check instead - and
    // asks the shared predicate the asserts themselves are written in terms of
    // (GmpiParameter::acceptsXmlText, via StandaloneHost::acceptsParameterText)
    // so that the two cannot come apart.
    //
    // Everything it rejects goes into the same quarantine as a truncated file:
    // one line, the file kept as session.previous.xml, the app up on defaults.
    bool parametersAreReadable(const std::string& document, std::string& why) const;

    // session.xml -> session.previous.xml, over any previous occupant. Says
    // nothing itself: the two callers are a failure and a deliberate revert,
    // and the same sentence would be wrong for one of them.
    //
    // Returns false when there was a file and it would not move. There being no
    // file at all is not a failure - there is then nothing to lose - and
    // `moved` is what tells the two apart.
    //
    // Raises mustRewrite_ on both of those outcomes and on NEITHER of the
    // failure ones, because that flag is what forces a write at the next save:
    // owed when the folder has been left without a session, and refused when the
    // file is still sitting there unmoved, since the write would land on it.
    bool moveAside(bool& moved, std::string& why);

    // Moves the state file aside because it could not be USED, and reports why.
    // Never deletes: whatever went wrong, the patch is the thing the user spent
    // time on, and a file they can still send someone beats a clean folder.
    //
    // Nor overwrites, which is the same promise by the back door and is what
    // moveAside leaving mustRewrite_ down on failure buys: a file that would not
    // move is a file this run will not write over either. Only the user editing
    // something replaces it after that, which is what editing is for.
    void quarantine(const std::string& why, PlatformShell& shell);

    // Write-then-rename, the same shape as Settings::save(), so a crash or a
    // full disk mid-write leaves the previous patch intact rather than half a
    // document that the next launch would quarantine.
    bool writeDocument(const std::string& document, PlatformShell& shell);

    // What is on disk right now, as this object last wrote or read it. A save
    // that would produce these same bytes does not happen.
    void takeBaseline();

    StandaloneHost& host_;

    std::filesystem::path statePath_;     // session.xml
    std::filesystem::path previousPath_;  // session.previous.xml
    std::filesystem::path loadingPath_;   // session.loading - the breadcrumb

    // What identityMatches tests, captured once: the plugin's own name and the
    // version it declares. The version is RECORDED AND REPORTED, never gated on
    // - pluginInfo::version is author-declared and optional, so a match proves
    // nothing and a mismatch is usually just a routine release.
    std::string pluginName_;
    std::string pluginVersion_;

    // False for a plugin with nothing worth saving, and then this whole class
    // does nothing: no file is read, none is written, and none is created.
    bool enabled_ = false;

    std::string baseline_;

    // Set when a file was moved aside, or when there was none to move, so the
    // next save happens even though nothing was edited. Without it a startup
    // that rejected a corrupt file and then changed nothing would leave the
    // folder with no session.xml at all.
    //
    // NOT set when the move failed - see moveAside. The file is still there in
    // that case, and forcing a write would put this session on top of it.
    bool mustRewrite_ = false;

    bool dirty_ = false;
    int quietMs_ = 0;      // since the last edit
    int dirtyAgeMs_ = 0;   // since the first edit that is still unsaved

    // TICK TIME, NOT WALL TIME. Both counters are fed whatever the app's loop
    // reports as elapsed, and on the two platforms that drive the tick from a
    // Ticker that is the NOMINAL interval rather than a measured one (see
    // Ticker::onTimer) - so a WM_TIMER arriving every 25ms still counts 16.
    // Measured on Windows, "2 seconds" of these lands nearer three of real
    // ones. Tolerated deliberately: this is a debounce, it needs an order of
    // magnitude and not a deadline, and taking a clock of its own here would
    // mean this file disagreeing with every other pump in the app about what
    // time it is.

    // Long enough that dragging a knob does not write a file per frame, short
    // enough that letting go and being killed loses nothing anybody would
    // notice. A quarter of a second would be safer and would also mean a save
    // in the middle of every gesture.
    static constexpr int kQuietMs = 2000;

    // And a ceiling on that, because a slow continuous edit - an automation
    // curve being drawn, a knob being crept round - never produces a gap, and
    // "we were about to save" is no use to somebody whose app just died.
    static constexpr int kCeilingMs = 30000;

    // A patch is parameters, not media. Eight megabytes is far past anything a
    // plugin should be keeping in one and still small enough to read on the
    // startup path without anybody noticing. Past it, the file is assumed to be
    // something other than ours and is moved aside unread.
    static constexpr std::uintmax_t kMaxBytes = 8u * 1024u * 1024u;
};

} // namespace standalone
} // namespace gmpi

// DEFERRED, AND WHY.
//
// Named presets - File > Save Preset / Load Preset - are the obvious next
// thing and are deliberately absent, for a reason that is about the FORMAT
// rather than about the menu. The shared <Preset> document cannot currently
// carry a preset's name or its category: both writers have those lines behind
// `#if 0` (Hosting/controller_holder.cpp, Hosting/processor_holder.cpp). So a
// Save Preset built today would produce files that no browser could ever label,
// and every one of them would have to be renamed to mean anything. The name
// belongs in the format, the format is shared with the DAW wrappers, and that
// decision belongs where the DAW-side preset story is made.
//
// A file chooser is the other half of the cost, and it is three backends with
// three different completion timings - one of them a portal that silently does
// nothing on a Linux box without xdg-desktop-portal installed. Session state
// needs no chooser at all, which is why it ships first.
