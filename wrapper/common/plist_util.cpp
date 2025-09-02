#include <iostream>
#include <filesystem>
#include <fstream>
#include "tinyXml2/tinyxml2.h"
#include "dynamic_linking.h"
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

int scanDll(wrapper::gmpi_dynamic_linking::DLL_HANDLE dllHandle, std::string exeName, std::ostream& out)
{
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
			<string>SEInstrumentBaseFactory</string>
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
out << "\t\t\t<string>" << effect_code << "</string>";
out << R"XML(
			<key>version</key>
			<integer>65536</integer>
		</dict>
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
	<key>CFBundleLongVersionString</key>
	<string></string>
	<key>CFBundlePackageType</key>
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
        std::cerr << "Usage: plist_util <plugin_path> <output path>\n";
        return 2;
    }

    const std::filesystem::path pluginPath(argv[1]);
    const std::filesystem::path outputPath(argv[2]);

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
        return;
    }

    // Open the bundle
    dllHandle = (wrapper::gmpi_dynamic_linking::DLL_HANDLE) CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    CFRelease(bundleUrl);
    CFRelease(pluginPathStringRef);

    if(dllHandle == 0) {
        printf("Couldn't create bundle reference\n");
        return;
    }
#endif

    if (!dllHandle || r != 0)
    {
        std::cerr << "ERROR: Can't load plugin\n";
        return 2;
    }

	// open a filestream for the output file.
	std::ofstream ofs(outputPath);

    r = scanDll(dllHandle, pluginPath.stem().string() + "_AU", ofs);

    wrapper::gmpi_dynamic_linking::MP_DllUnload(dllHandle);

    return r;
}
