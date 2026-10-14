// vst3_class_ids_test - the VST3 class IDs the wrapper's factory reports.
//
// Checks that an ID given in plugin.gmpi.xml (vst3ProcessorId/vst3ControllerId)
// becomes exactly the cid FUID::fromRegistryString gives - what se_vst3 does with
// the same string from factory.se.xml - and that the hashed fallback is distinct
// per plugin and per role, and prints the same GUID string on every platform.
//
// No CMake needed (paths relative to C:\SE or the parent of the repos):
//
//   g++ -std=c++20 -I GMPI -I GMPI/Core -I GMPI_Wrappers/wrapper/common -I SDKs/VST3_SDK
//       GMPI_Wrappers/tests/vst3_class_ids_test.cpp GMPI/Hosting/xml_spec_reader.cpp
//       GMPI_Wrappers/wrapper/common/tinyXml2/tinyxml2.cpp
//       SDKs/VST3_SDK/pluginterfaces/base/funknown.cpp -o vst3_class_ids_test
//
// (one command line; MSVC: the same with cl /std:c++20 /EHsc /DNOMINMAX, plus ole32.lib.)
// Exit code 0 = pass.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../wrapper/VST3/Vst3ClassIds.h"

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		++failures;
}

std::string registryString(const Steinberg::FUID& id)
{
	char s[40]{};
	id.toRegistryString(s);
	return s;
}

// What MyVstPluginFactory::getClassInfo writes into PClassInfo::cid, read back as a FUID.
Steinberg::FUID reportedCid(const Steinberg::FUID& id)
{
	Steinberg::TUID cid{};
	id.toTUID(cid);
	return Steinberg::FUID::fromTUID(cid);
}

gmpi::hosting::pluginInfo parseOne(const char* xml)
{
	std::vector<gmpi::hosting::pluginInfo> plugins;
	gmpi::hosting::readpluginXml(xml, plugins);
	return plugins.empty() ? gmpi::hosting::pluginInfo{} : plugins.front();
}
} // namespace

int main()
{
	// Values in the form SynthEdit's exporter stores (FUID::toRegistryString, upper case).
	const char* processorStr = "{5A3D1F07-9C2B-4E61-8F04-2B7C9D11E3A6}";
	const char* controllerStr = "{C0FFEE12-3456-4789-ABCD-0123456789EF}";

	// Explicit IDs.
	{
		const auto plugin = parseOne(R"XML(<?xml version="1.0"?>
<Plugin id="Acme: Explicit" name="Explicit" vendor="Acme" version="1.2.3.0"
        vst3ProcessorId="{5A3D1F07-9C2B-4E61-8F04-2B7C9D11E3A6}"
        vst3ControllerId="{C0FFEE12-3456-4789-ABCD-0123456789EF}"/>)XML");

		check(plugin.vst3ProcessorId == processorStr, "parser reads vst3ProcessorId");
		check(plugin.vst3ControllerId == controllerStr, "parser reads vst3ControllerId");
		check(plugin.version == "1.2.3.0", "parser reads version");

		Steinberg::FUID expectedP, expectedC;
		expectedP.fromRegistryString(processorStr);
		expectedC.fromRegistryString(controllerStr);

		const auto ids = wrapper::vst3ClassIds(plugin);
		check(reportedCid(ids.processor) == expectedP, "processor cid == FUID::fromRegistryString(vst3ProcessorId)");
		check(reportedCid(ids.controller) == expectedC, "controller cid == FUID::fromRegistryString(vst3ControllerId)");
		check(registryString(ids.processor) == processorStr, "processor cid prints back as the same registry string");
		check(registryString(ids.controller) == controllerStr, "controller cid prints back as the same registry string");

		// The VST2 ID is summed from these; they must follow the string, not the platform's TUID layout.
		unsigned char bytes[16]{};
		wrapper::canonicalBytes(ids.processor, bytes);
		const unsigned char expectedBytes[16] = { 0x5A,0x3D,0x1F,0x07, 0x9C,0x2B, 0x4E,0x61, 0x8F,0x04, 0x2B,0x7C,0x9D,0x11,0xE3,0xA6 };
		check(std::memcmp(bytes, expectedBytes, 16) == 0, "canonicalBytes follow the registry string's order");
	}

	// Lower-case hex is the same ID.
	{
		auto plugin = parseOne(R"XML(<Plugin id="Acme: Lower" name="Lower" vst3ProcessorId="{5a3d1f07-9c2b-4e61-8f04-2b7c9d11e3a6}"/>)XML");
		Steinberg::FUID expected;
		expected.fromRegistryString(processorStr);
		check(wrapper::vst3ClassIds(plugin).processor == expected, "lower-case registry string gives the same cid");
	}

	// No explicit IDs: hashed fallback.
	{
		const auto a = parseOne(R"XML(<Plugin id="Acme: Alpha" name="Alpha"/>)XML");
		const auto b = parseOne(R"XML(<Plugin id="Acme: Beta" name="Beta"/>)XML");
		check(a.vst3ProcessorId.empty() && a.vst3ControllerId.empty(), "absent attributes leave the fields empty");

		const auto idsA = wrapper::vst3ClassIds(a);
		const auto idsB = wrapper::vst3ClassIds(b);
		check(idsA.processor != idsB.processor, "fallback: different plugin ids give different processor cids");
		check(idsA.controller != idsB.controller, "fallback: different plugin ids give different controller cids");
		check(idsA.processor != idsA.controller, "fallback: processor and controller cids differ");
		check(wrapper::vst3ClassIds(a).processor == idsA.processor, "fallback is deterministic");

		// Pinned: the hash fallback must print identically on Windows, macOS and Linux.
		const auto p = registryString(idsA.processor);
		const auto c = registryString(idsA.controller);
		std::printf("      fallback for \"Acme: Alpha\": processor %s controller %s\n", p.c_str(), c.c_str());
		check(p == "{FEAC5688-7420-8CD7-9E87-E5BFB7D08304}", "fallback processor GUID string is the pinned value");
		check(c == "{FC6C62CC-F289-865F-836F-056DFAF488D4}", "fallback controller GUID string is the pinned value");
		check(p[15] == '8' && (c[20] == '8' || c[20] == '9' || c[20] == 'A' || c[20] == 'B'), "fallback is tagged as an RFC 9562 version-8 UUID");

		// Non-ASCII ids hash the same whether plain char is signed or not.
		const auto u = wrapper::vst3ClassIds(parseOne("<Plugin id=\"Acme: Caf\xC3\xA9\" name=\"Cafe\"/>"));
		std::printf("      fallback for \"Acme: Caf\\u00e9\": processor %s\n", registryString(u.processor).c_str());
		check(registryString(u.processor) == "{9F66EB2A-CACF-8BBB-98E5-22179D6C8356}", "non-ASCII fallback GUID string is the pinned value");
	}

	// Malformed explicit IDs are rejected (the factory then falls back, and asserts in a debug build).
	{
		using wrapper::detail::isRegistryString;
		check(isRegistryString(processorStr), "well-formed registry string accepted");
		check(!isRegistryString("5A3D1F07-9C2B-4E61-8F04-2B7C9D11E3A6"), "missing braces rejected");
		check(!isRegistryString("{5A3D1F07-9C2B-4E61-8F04-2B7C9D11E3AZ}"), "non-hex digit rejected");
		check(!isRegistryString("{5A3D1F079-C2B-4E61-8F04-2B7C9D11E3A6}"), "misplaced dash rejected");
		check(!isRegistryString("{5A3D1F07-9C2B-4E61-8F04-2B7C9D11E3A}"), "short string rejected");
	}

	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
