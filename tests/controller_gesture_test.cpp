// controller_gesture_test - what gmpi_controller_holder tells the wrapper when a GUI edits parameters.
//
// A GUI's Grab pin (or setParameter with Field::Grab) must open and close one host gesture around the
// edits in between; an edit to a parameter the DAW has no tag for (private) must reach the processor
// and, when it is saved, mark the host project modified - never a performEdit with tag -1.
//
// No CMake needed (paths relative to C:\SE or the parent of the repos):
//
//   g++ -std=c++20 -I GMPI -I GMPI/Core -I gmpi_ui -I GMPI_Wrappers/wrapper/common
//       GMPI_Wrappers/tests/controller_gesture_test.cpp GMPI/Hosting/controller_holder.cpp
//       GMPI/Hosting/xml_spec_reader.cpp GMPI/Hosting/message_queues.cpp
//       GMPI_Wrappers/wrapper/common/tinyXml2/tinyxml2.cpp -o controller_gesture_test
//
// (one command line; MSVC: the same with cl /std:c++20 /EHsc /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS.)
// Exit code 0 = pass.

#include <cstdio>
#include <string>
#include <vector>
#include "Hosting/controller_holder.h"

namespace
{
int failures = 0;

void check(bool ok, const char* what, const std::string& got = {})
{
	std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, ok || got.empty() ? "" : "  -- got: ", ok ? "" : got.c_str());
	if (!ok)
		++failures;
}

const char* xml = R"XML(
<Plugin id="GMPI GestureTest" name="GestureTest" vendor="GMPI Tests">
  <Parameters>
    <Parameter id="0" name="Cutoff" datatype="float"/>
    <Parameter id="1" name="Hidden" datatype="float" private="true"/>
    <Parameter id="2" name="Scratch" datatype="float" private="true" persistant="false"/>
  </Parameters>
  <GUI>
    <Pin name="Cutoff" datatype="float" parameterId="0"/>
    <Pin name="Mouse Down" datatype="bool" parameterId="0" parameterField="Grab"/>
    <Pin name="Hidden" datatype="float" parameterId="1"/>
    <Pin name="Scratch" datatype="float" parameterId="2"/>
  </GUI>
</Plugin>
)XML";

enum Pins { CutoffValue, CutoffGrab, HiddenValue, ScratchValue };

template<typename T>
void setPin(gmpi::hosting::gmpi_controller_holder& holder, int pin, T value)
{
	const auto* p = reinterpret_cast<const std::byte*>(&value);
	holder.setPinFromUi(pin, 0, { p, p + sizeof(value) });
}
} // namespace

int main()
{
	std::vector<gmpi::hosting::pluginInfo> plugins;
	readpluginXml(xml, plugins);
	if (plugins.empty())
	{
		std::printf("FAIL  plugin XML did not parse\n");
		return 1;
	}

	gmpi::hosting::gmpi_controller_holder holder;
	std::string log;
	holder.notifyDaw = [&](gmpi::hosting::GmpiParameter* p) { log += "edit " + std::to_string(p->info->dawTag) + (p->isGrabbed ? " (grabbed);" : " (ungrabbed);"); };
	holder.notifyDawGrab = [&](gmpi::hosting::GmpiParameter* p, bool grabbed) { log += (grabbed ? "begin " : "end ") + std::to_string(p->info->dawTag) + ";"; };
	holder.sendNonNativeParameterToProcessor = [&](gmpi::hosting::GmpiParameter* p) { log += "to-processor " + std::to_string(p->info->id) + ";"; };
	holder.notifyDawStateChanged = [&]() { log += "dirty;"; };
	holder.init(plugins[0]);

	// A drag: grab, two moves, release.
	setPin(holder, CutoffGrab, true);
	setPin(holder, CutoffValue, 0.25f);
	setPin(holder, CutoffValue, 0.5f);
	setPin(holder, CutoffGrab, false);
	check(log == "begin 0;edit 0 (grabbed);edit 0 (grabbed);end 0;", "a GUI drag is one gesture: begin, edits while grabbed, end", log);

	// A click with no Grab pin wired still reaches the DAW as a self-contained edit.
	log.clear();
	setPin(holder, CutoffValue, 0.75f);
	check(log == "edit 0 (ungrabbed);", "an edit with no grab is reported ungrabbed (the wrapper brackets it)", log);

	// Repeating a grab state sends nothing.
	log.clear();
	setPin(holder, CutoffGrab, true);
	setPin(holder, CutoffGrab, true);
	setPin(holder, CutoffGrab, false);
	setPin(holder, CutoffGrab, false);
	check(log == "begin 0;end 0;", "a repeated grab or release is not a second gesture", log);

	// A private, saved parameter: no DAW tag, so it goes to the processor and marks the project modified.
	log.clear();
	setPin(holder, HiddenValue, 0.5f);
	check(log == "to-processor 1;dirty;", "a private saved parameter reaches the processor and dirties the project", log);

	// A private parameter that is not saved changes nothing the host stores.
	log.clear();
	setPin(holder, ScratchValue, 0.5f);
	check(log == "to-processor 2;", "a private unsaved parameter reaches the processor, no dirty", log);

	// An unchanged value sends nothing.
	log.clear();
	setPin(holder, HiddenValue, 0.5f);
	check(log.empty(), "an unchanged value sends nothing", log);

	// Grab through setParameter (an editor that talks to its host directly), then the last editor goes away mid-drag.
	log.clear();
	const bool down = true;
	holder.setParameter(0, gmpi::Field::Grab, 0, sizeof(down), reinterpret_cast<const uint8_t*>(&down));
	holder.unRegisterGui(nullptr);
	check(log == "begin 0;end 0;", "setParameter(Grab) opens a gesture; losing the last editor closes it", log);

	// Grabbing a parameter the DAW can't see opens no host gesture.
	log.clear();
	holder.setParameter(1, gmpi::Field::Grab, 0, sizeof(down), reinterpret_cast<const uint8_t*>(&down));
	check(log.empty(), "grabbing a private parameter opens no host gesture", log);

	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
