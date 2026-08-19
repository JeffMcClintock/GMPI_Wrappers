#include <iostream>
#include <filesystem>
#include <fstream>
#include "tinyXml2/tinyxml2.h"
#include "../Hosting/dynamic_linking.h"
#include "GmpiApiCommon.h"
#include "GmpiSdkCommon.h"
#if __APPLE__
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

typedef int32_t(/*__stdcall*/ * MP_DllEntry)(void**);

std::string to4charId(std::string s)
{
    // convert to lower-case
    for (auto& c : s)
		c = tolower(c);

    // enforce 4 char minimum
    s += "!!!!";

    // populate an array with all alpha numberic characters in order of most frequently occuring first.
	// This is to make the resulting 4 char code more likely to be unique.
    const char unwanted[] =
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
        "etaoinshrdlcumwfgypbvkjxqz"
        "0123456789";

    for(auto c : unwanted)
    {
        if(s.size() <= 4)
			break;

        size_t pos;
        while((pos = s.find(c)) != std::string::npos && s.size() > 4)
        {
            s.erase(pos, 1);
        }
	}

	s[0] = toupper(s[0]); // must have at least one uppercase to be valid.

    return s;
}

std::string attributeOr(const char* attribute, std::string def)
{
    return attribute ? attribute : def;
}

// An AudioComponent's version is a single integer, and Apple packs it as
// 0xMMMMmmbb: SIXTEEN bits of major, then eight of minor and eight of bug-fix.
// So 0x00010000, decimal 65536, is 1.0.0. It is the only version macOS's
// AudioComponent registry carries, and what an AU host displays, so a plugin
// that never changes this number looks unchanged to the host no matter what
// shipped.
//
// The field widths are why the clamps below are not all the same. An earlier
// draft clamped all three to 255 - which agrees with the shifts, and would have
// pinned any plugin that ever reached major 256 at 255 forever. The format is
// Apple's, so the code follows the format rather than the other way round.
//
// Reads the leading numbers of the declared version and ignores any tail, so
// "2.1.0-beta" is 2.1.0 here while the bundle's own version strings keep the
// text the author wrote. Each field is clamped rather than allowed to carry
// into the one above it, which also keeps the running total from overflowing on
// a long run of digits.
constexpr uint32_t auVersionDefault = 65536; // 1.0.0

uint32_t auVersionInteger(const std::string& version)
{
    // Index 0 is the 16-bit major; 1 and 2 are the two 8-bit fields.
    unsigned int part[3]{};
    const unsigned int limit[3]{ 65535, 255, 255 };
    int index{};

    for (auto c = version.begin(); c != version.end() && index < 3; ++c)
    {
        if (*c >= '0' && *c <= '9')
        {
            part[index] = part[index] * 10 + static_cast<unsigned int>(*c - '0');

            if (part[index] > limit[index])
                part[index] = limit[index];
        }
        else if (*c == '.')
        {
            ++index;
        }
        else
        {
            break; // a non-numeric tail ends the version proper.
        }
    }

    const uint32_t packed = (part[0] << 16) | (part[1] << 8) | part[2];

    // A version that begins with anything but a digit - "v1.2", "beta" - parses
    // to nothing, and zero is the one value that must not be published: a host
    // reads an AudioComponent version of 0 as older than every build that came
    // before it, so an AU that shipped as 1.0.0 and then adopted a "v"-prefixed
    // version would appear to go BACKWARDS. Before this attribute existed this
    // tool wrote 65536 unconditionally, so falling back to it leaves such a
    // plugin exactly where it already was rather than behind itself.
    //
    // A plugin that deliberately declares 0.0.0 is caught by the same test and
    // published as 1.0.0. It is not distinguishable from a parse failure here,
    // and of the two readings this is the safe one - the other publishes a
    // version every host treats as a downgrade.
    return packed == 0 ? auVersionDefault : packed;
}

// Everything the emitters below need to describe one plugin as an
// AudioComponent, derived once from the built module's own XML so the AU2
// .component and the AU3 .appex cannot disagree about identity.
struct AuIdentity
{
    std::string name;
    std::string vendor_name;
    std::string vendor_code;   // 4-char manufacturer
    std::string plugin_code;   // 4-char subtype
    std::string version;       // author's text, e.g. "2.1.0-beta"
    const char* effect_code{}; // "aufx" / "aumf" / "aumu"
    bool isSynth{};
};

// The <dict> describing one AudioComponent - identical fields in the AU2
// bundle plist and the AU3 appex's NSExtensionAttributes, differing only in
// factoryFunction and indentation.
static void emitAudioComponentDict(const AuIdentity& id, const char* factoryFunction, const std::string& indent, std::ostream& out)
{
    out << indent << "<dict>\n";
    out << indent << "\t<key>description</key>\n";
    out << indent << "\t<string>" << id.name << "</string>\n";
    out << indent << "\t<key>factoryFunction</key>\n";
    out << indent << "\t<string>" << factoryFunction << "</string>\n";
    out << indent << "\t<key>manufacturer</key>\n";
    out << indent << "\t<string>" << id.vendor_code << "</string>\n";
    out << indent << "\t<key>name</key>\n";
    out << indent << "\t<string>" << id.vendor_name << ":" << id.name << "</string>\n";
    out << indent << "\t<key>sandboxSafe</key>\n";
    out << indent << "\t<true/>\n";
    out << indent << "\t<key>subtype</key>\n";
    out << indent << "\t<string>" << id.plugin_code << "</string>\n";

    if (id.isSynth)
    {
        out << indent << "\t<key>tags</key>\n";
        out << indent << "\t<array>\n";
        out << indent << "\t\t<string>Synthesizer</string>\n";
        out << indent << "\t</array>\n";
    }

    out << indent << "\t<key>type</key>\n";
    out << indent << "\t<string>" << id.effect_code << "</string>\n";
    out << indent << "\t<key>version</key>\n";
    out << indent << "\t<integer>" << auVersionInteger(id.version) << "</integer>\n";
    out << indent << "</dict>\n";
}

// The AUv3 app extension's Info.plist. The layout auval accepts is the one
// wrapper/AU3/appex-Info.plist.in documents; this emitter exists so a build
// can fill it from the plugin's own XML instead of hand-substituting.
// GmpiAUViewController is both the factory and the principal class - see
// AU3_ViewController.h.
static void emitAu3Plist(const AuIdentity& id, const std::string& exeName, const std::string& bundleId, std::ostream& out)
{
    out << R"XML(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
)XML";
    out << "\t<key>CFBundleDisplayName</key>\n";
    out << "\t<string>" << id.name << "</string>\n";
    out << "\t<key>CFBundleExecutable</key>\n";
    out << "\t<string>" << exeName << "</string>\n";
    out << "\t<key>CFBundleIdentifier</key>\n";
    out << "\t<string>" << bundleId << "</string>\n";
    out << "\t<key>CFBundleInfoDictionaryVersion</key>\n";
    out << "\t<string>6.0</string>\n";
    out << "\t<key>CFBundleName</key>\n";
    out << "\t<string>" << id.name << "</string>\n";
    out << "\t<key>CFBundlePackageType</key>\n";
    out << "\t<string>XPC!</string>\n";
    out << "\t<key>CFBundleShortVersionString</key>\n";
    out << "\t<string>" << id.version << "</string>\n";
    out << "\t<key>CFBundleVersion</key>\n";
    out << "\t<string>" << id.version << "</string>\n";
    out << "\t<key>NSExtension</key>\n";
    out << "\t<dict>\n";
    out << "\t\t<key>NSExtensionAttributes</key>\n";
    out << "\t\t<dict>\n";
    out << "\t\t\t<key>AudioComponents</key>\n";
    out << "\t\t\t<array>\n";

    emitAudioComponentDict(id, "GmpiAUViewController", "\t\t\t\t", out);

    out << "\t\t\t</array>\n";
    out << "\t\t</dict>\n";
    out << "\t\t<key>NSExtensionPointIdentifier</key>\n";
    out << "\t\t<string>com.apple.AudioUnit-UI</string>\n";
    out << "\t\t<key>NSExtensionPrincipalClass</key>\n";
    out << "\t\t<string>GmpiAUViewController</string>\n";
    out << "\t</dict>\n";
    out << "</dict>\n";
    out << "</plist>\n";
}

struct PlistOptions
{
    bool au3{};
    std::string exeName;  // CFBundleExecutable
    std::string bundleId; // CFBundleIdentifier (au3 only)
};

int scanDll(wrapper::gmpi_dynamic_linking::DLL_HANDLE dllHandle, const PlistOptions& options, std::ostream& out)
{
    const std::string exeName = options.exeName;
    MP_DllEntry dll_entry_point{};
    const char* gmpi_dll_entrypoint_name = "MP_GetFactory";
#ifdef _WIN32
    auto r = wrapper::gmpi_dynamic_linking::MP_DllSymbol(dllHandle, gmpi_dll_entrypoint_name, (void**)&dll_entry_point);
#else
    dll_entry_point = (MP_DllEntry) CFBundleGetFunctionPointerForName((CFBundleRef)dllHandle, CFSTR("MP_GetFactory"));
    int r = 0;
#endif
    
    if (!dll_entry_point || r != 0)
    {
        std::cerr << "ERROR: Can't locate entry point\n";
        return 2;
    }

    { // restrict scope of 'vst_factory' and 'gmpi_factory' so smart pointers RIAA before dll is unloaded

        // Instansiate factory and query sub-plugins.
        gmpi::shared_ptr<gmpi::api::IPluginFactory> gmpi_factory;
        {
            gmpi::shared_ptr<gmpi::api::IUnknown> com_object;
            r = dll_entry_point(com_object.put_void());

            gmpi_factory = com_object.as<gmpi::api::IPluginFactory>();
        }

        if (!gmpi_factory)
        {
            std::cerr << "ERROR: Can't locate factory object\n";
            return 2;
        }

        int index = 0;
        gmpi::ReturnString s;

        if (gmpi::ReturnCode::Ok != gmpi_factory->getPluginInformation(index++, &s)) // FULL XML
            return 2;

        tinyxml2::XMLDocument doc;
        doc.Parse(s.c_str());

        if (doc.Error())
        {
            std::cerr << "Module XML Error: " << doc.ErrorName() << " " << doc.Value();
            return 2;
        }

        auto pluginList = doc.FirstChildElement("PluginList");

        if (pluginList == 0)
        {
            std::cerr << "Module XML Error: No 'PluginList' element.";
            return 2;
        }
        bool reportedDuplicateModule = false;

        auto PluginElement = pluginList->FirstChildElement("Plugin");

        if (!PluginElement)
        {
            std::cerr << "Module XML Error: No 'Plugin' element.";
            return 2;
        }

        auto gmpi_plugin_id = attributeOr(PluginElement->Attribute("id"), "GMPI Plugin");
        std::string plugin_id;
        std::string vendor_code;
        std::string vendor_name;

        if(auto p = gmpi_plugin_id.find(':') ; p != std::string::npos)
        {
            vendor_code = to4charId(gmpi_plugin_id.substr(0, p));
			vendor_name = vendor_code;
            plugin_id = gmpi_plugin_id.substr(gmpi_plugin_id.find_first_not_of(' ', p + 1));
        }
        else
        {
			plugin_id = gmpi_plugin_id;
        }

        auto plugin_code = to4charId(plugin_id);
        auto name = attributeOr(PluginElement->Attribute("name"), "GMPI Plugin");

        // The one place a plugin's version is written, and read here out of the
        // XML this tool pulled from the BUILT MODULE - literally the document
        // the VST3 factory parses at load, so the AU and the VST3 cannot report
        // different versions of the same plugin.
        //
        // gmpi_plugin.cmake reads that attribute too, to stamp the Windows
        // VERSIONINFO resource, but by searching the sources as text rather than
        // by loading the module; gmpi::hosting::pluginInfo::version says where
        // that can differ. "1.0.0" when the plugin declares none - the version
        // this tool's AudioComponent integer hardcoded before, and
        // gmpi::hosting::defaultPluginVersion.
        auto version = attributeOr(PluginElement->Attribute("version"), "1.0.0");
        if (version.empty())
            version = "1.0.0";

        if (auto v = PluginElement->Attribute("vendor"); v)
        {
            vendor_name = v;

            if (vendor_code.empty())
                vendor_code = to4charId(vendor_name);
        }

        if (vendor_name.empty())
        {
            vendor_name = "GMPI";
        }
        if (vendor_code.empty())
        {
            vendor_code = "Gmpi";
        }

        bool hasMidi{};
        bool hasInputs{};
        bool hasOutputs{};

		// categorise plugin type.
        if (auto plugin_class = PluginElement->FirstChildElement("Audio"); plugin_class)
        {
            for (auto pin = plugin_class->FirstChildElement(); pin; pin = pin->NextSiblingElement())
            {
                if (strcmp(pin->Value(), "Pin") == 0)
                {
                    auto pin_direction = pin->Attribute("direction");
					const bool isOut = pin_direction && pin_direction[0] == 'o';
                    const bool isAudio = nullptr != pin->Attribute("rate");
					auto datatype = pin->Attribute("datatype");
                    const bool isMidi = nullptr != datatype && datatype[0] == 'm';

					hasMidi |= isMidi;
					hasInputs |= isAudio && !isOut;
					hasOutputs |= isAudio && isOut;
                }
                //else
                //{
                //    if (strcmp(node->Value(), "Group") == 0)
                //    {
                //        assert(false); // used? if so check auto pin numbering.
                //        ScanPinXml(node, pinlist, plugin_sub_type); // recursion
                //    }
                //}
            }
        }

        bool isSynth{};
		const char* effect_code = "aufx"; // effect by default
        if (hasMidi)
        {
            if(hasInputs)
                effect_code = "aumf"; // midi effect
            else
            {
                effect_code = "aumu"; // instrument
                isSynth = true;
            }
        }

        if (options.au3)
        {
            const AuIdentity identity{ name, vendor_name, vendor_code, plugin_code, version, effect_code, isSynth };
            emitAu3Plist(identity, options.exeName, options.bundleId, out);
            return 0;
        }

        // emit the Info.plist file

out << R"XML(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AudioComponents</key>
	<array>
		<dict>
			<key>description</key>
)XML";
            out << "\t\t\t<string>" << name << "</string>";

            out << R"XML(
			<key>factoryFunction</key>
			<string>AU2_WrapperFactory</string>
			<key>manufacturer</key>
)XML";
            out << "\t\t\t<string>" << vendor_code << "</string>\n";
            out << "\t\t\t<key>name</key>\n";
            out << "\t\t\t<string>" << vendor_name << ":" << name << "</string>";
out << R"XML(
			<key>sandboxSafe</key>
			<true/>
			<key>subtype</key>
)XML";
out << "\t\t\t<string>" << plugin_code << "</string>";
out << R"XML(
)XML";

if (isSynth)
{
    out << "\t\t\t<key>tags</key>\n";
    out << "\t\t\t<array>\n";
    out << "\t\t\t\t<string>Synthesizer</string>\n";
    out << "\t\t\t</array>\n";
}

out << "\t\t\t<key>type</key>\n";
out << "\t\t\t<string>" << effect_code << "</string>\n";
out << "\t\t\t<key>version</key>\n";
out << "\t\t\t<integer>" << auVersionInteger(version) << "</integer>\n";
out << R"XML(		</dict>
	</array>)XML";

		//out << "\t<key>CFBundleName</key>\n";
		//out << "\t<string>" << exeName << "</string>\n";

out << R"XML(
	<key>BuildMachineOSBuild</key>
	<string>24G84</string>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
)XML";
out << "\t<string>" << exeName << "</string>";

#if 0         // build warning about bundle ID not matching
out << R"XML(
	<key>CFBundleIdentifier</key>
)XML";
out << "\t<string>com." << vendor_code << "." << plugin_id << "</string>";
#endif
        
out << R"XML(
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
)XML";

// The bundle's own version, as opposed to the AudioComponent's integer above.
// This plist REPLACES the one CMake generated, so the MACOSX_BUNDLE_* version
// properties gmpi_plugin.cmake sets on the target do not survive into a
// .component - these three keys are the only bundle version an AU ends up with,
// and before this the first was empty and the other two were absent entirely.
// Written verbatim, so a version with a tail ("2.1.0-beta") reads the way the
// author wrote it; note that Apple wants period-separated integers in
// CFBundleShortVersionString and CFBundleVersion, so such a version would fail
// App Store validation.
out << "\t<key>CFBundleLongVersionString</key>\n";
out << "\t<string>" << version << "</string>\n";
out << "\t<key>CFBundleShortVersionString</key>\n";
out << "\t<string>" << version << "</string>\n";
out << "\t<key>CFBundleVersion</key>\n";
out << "\t<string>" << version << "</string>\n";

out << R"XML(	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>MacOSX</string>
	</array>
	<key>CSResourcesFileMapped</key>
	<true/>
	<key>DTCompiler</key>
	<string>com.apple.compilers.llvm.clang.1_0</string>
	<key>DTPlatformBuild</key>
	<string>24F74</string>
	<key>DTPlatformName</key>
	<string>macosx</string>
	<key>DTPlatformVersion</key>
	<string>15.5</string>
	<key>DTSDKBuild</key>
	<string>24F74</string>
	<key>DTSDKName</key>
	<string>macosx15.5</string>
	<key>DTXcode</key>
	<string>1640</string>
	<key>DTXcodeBuild</key>
	<string>16F6</string>
	<key>LSMinimumSystemVersion</key>
	<string>10.15</string>
</dict>
</plist>
)XML";

    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: plist_util <plugin_path> <output path> [--au3 <executable_name> <bundle_identifier>]\n";
        return 2;
    }

    const std::filesystem::path pluginPath(argv[1]);
    const std::filesystem::path outputPath(argv[2]);

    PlistOptions options;
    options.exeName = pluginPath.stem().string() + "_AU";

    if (argc >= 6 && std::string(argv[3]) == "--au3")
    {
        options.au3 = true;
        options.exeName = argv[4];
        options.bundleId = argv[5];
    }
    else if (argc > 3)
    {
        std::cerr << "Usage: plist_util <plugin_path> <output path> [--au3 <executable_name> <bundle_identifier>]\n";
        return 2;
    }

    wrapper::gmpi_dynamic_linking::DLL_HANDLE dllHandle{};
    
#ifdef _WIN32
    auto r = wrapper::gmpi_dynamic_linking::MP_DllLoad(&dllHandle, pluginPath.c_str());
#else
    int r = 0;
    // int32_t r = MP_DllLoad( &dllHandle, load_filename.c_str() );

    // Create a path to the bundle
    CFStringRef pluginPathStringRef = CFStringCreateWithCString(NULL,
        pluginPath.c_str(), kCFStringEncodingASCII);

    CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
    pluginPathStringRef, kCFURLPOSIXPathStyle, true);
    if(bundleUrl == NULL) {
        printf("Couldn't make URL reference for plugin\n");
        return 1;
    }

    // Open the bundle
    dllHandle = (wrapper::gmpi_dynamic_linking::DLL_HANDLE) CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    CFRelease(bundleUrl);
    CFRelease(pluginPathStringRef);

    if(dllHandle == 0) {
        printf("Couldn't create bundle reference\n");
        return 1;
    }
#endif

    if (!dllHandle || r != 0)
    {
        std::cerr << "ERROR: Can't load plugin\n";
        return 2;
    }

	// open a filestream for the output file.
	std::ofstream ofs(outputPath);

    r = scanDll(dllHandle, options, ofs);

    wrapper::gmpi_dynamic_linking::MP_DllUnload(dllHandle);

    return r;
}
