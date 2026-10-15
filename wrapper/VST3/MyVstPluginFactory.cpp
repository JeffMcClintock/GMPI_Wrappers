#ifdef _WIN32
#undef  WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#undef  NOMINMAX
#define NOMINMAX
#include "Windows.h"
#endif

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
//#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstcomponent.h"
#include "MyVstPluginFactory.h"
#include "Controller_VST3.h"
#include "Processor_VST3.h"
#include "Hosting/gmpi_factory.h"
#include "Vst3ClassIds.h"

#if defined( _WIN32 )
extern HINSTANCE ghInst;
#elif __APPLE__
	#include <dlfcn.h>
	#include <CoreFoundation/CoreFoundation.h>

#if 0
// Copied from: public.sdk/source/vst/vstguieditor.cpp
//void* gBundleRef = 0;
//static int openCount = 0;
//CFBundleRef getBundleRef(){return (CFBundleRef) gBundleRef;};
//------------------------------------------------------------------------
CFBundleRef CreatePluginBundleRef ()
{
    CFBundleRef rBundleRef = 0;
    
	Dl_info info;
	if (dladdr ((const void*)CreatePluginBundleRef, &info))
	{
		if (info.dli_fname)
		{
			Steinberg::String name;
			name.assign (info.dli_fname);
			for (int i = 0; i < 3; i++)
			{
				int delPos = name.findLast ('/');
				if (delPos == -1)
				{
					fprintf (stdout, "Could not determine bundle location.\n");
					return 0; // unexpected
				}
				name.remove (delPos, name.length () - delPos);
			}
			CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation (0, (const UInt8*)name.text8 (), name.length (), true);
			if (bundleUrl)
			{
				rBundleRef = CFBundleCreate (0, bundleUrl);
				CFRelease (bundleUrl);
			}
		}
	}
    
    return rBundleRef;
}

void ReleasePluginBundleRef (CFBundleRef bundleRef)
{
	CFRelease (bundleRef);
}
#endif
#endif

#if 0 // was misspelt __APPPLE__
 // divert these functions from main executable to VST3 Wrapper lib functions
bool bundleEntry_internal(CFBundleRef ref)
{
	return bundleEntry(ref);
}
bool bundleExit_internal(void)
{
	return bundleExit();
}
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

bool InitModule() { return true; }
bool DeinitModule() { return true; }

std::string calcSubCategories(gmpi::hosting::pluginInfo const& plugin)
{
	if (countPins(plugin, gmpi::PinDirection::In, gmpi::PinDatatype::Midi) > 0)
	{
		return "Instrument|Synth";
	}
	return "Fx";
}

// The plugin's own version, as declared by the `version` attribute of <Plugin>
// in its metadata XML and read by the SDK's parser. What the DAW is told, and
// the authoritative answer.
//
// gmpi_plugin.cmake stamps the macOS bundle and the Windows VERSIONINFO
// resource from that same attribute, so a host and Explorer normally agree
// about what is installed - but CMake finds the element by a text search rather
// than through this parser, and gmpi::hosting::pluginInfo::version lists the
// cases where the two can differ. When they do, this is the one that is right.
//
// Read through a guard because pluginInfo::version arrived in the SDK after
// this wrapper existed, and the two are separate repositories that are
// routinely at different revisions. An older SDK gets "1.0.0", which is what
// this factory reported unconditionally before, so nothing regresses.
namespace
{
std::string pluginVersion(gmpi::hosting::pluginInfo const& plugin)
{
#ifdef GMPI_HOSTING_PLUGININFO_HAS_VERSION
	if (!plugin.version.empty())
		return plugin.version;
#else
	(void)plugin;
#endif
	return "1.0.0";
}
}

MyVstPluginFactory* MyVstPluginFactory::GetInstance()
{
	static MyVstPluginFactory singleton;
	return &singleton;
}

/** Fill a PFactoryInfo structure with information about the Plug-in vendor. */
tresult MyVstPluginFactory::getFactoryInfo (PFactoryInfo* info)
{
	initialize();

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo();
	if(!plugin)
		return kResultFalse;

	strncpy8 (info->vendor, plugin->vendorName.c_str() , PFactoryInfo::kNameSize);
	strncpy8 (info->url   , plugin->vendorUrl.c_str()  , PFactoryInfo::kURLSize);
	// VST3 writes the email as a link.
	const auto email = plugin->vendorEmail.empty() || plugin->vendorEmail.rfind("mailto:", 0) == 0 ? plugin->vendorEmail : "mailto:" + plugin->vendorEmail;
	strncpy8 (info->email , email.c_str(), PFactoryInfo::kEmailSize);
	info->flags = PFactoryInfo::kUnicode;
	return kResultOk;
}

/** Returns the number of exported classes by this factory. 
If you are using the CPluginFactory implementation provided by the SDK, it returns the number of classes you registered with CPluginFactory::registerClass. */
int32 MyVstPluginFactory::countClasses ()
{
	initialize();

	return static_cast<int32>(gmpi::hosting::factory::getInstance().getPlugincount() * 2); // a processor and a controller each
}

int32_t MyVstPluginFactory::getVst2Id64(int32_t pluginIndex) // generated from hash of GUID. Not compatible w 32-bit VSTs.
{
	initialize();

	if (pluginIndex != 0)
	{
		return kInvalidArgument;
	}

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kInvalidArgument;

	// generate an VST2 ID from the processor's VST3 GUID, in canonical byte order so it is the same on every platform.
	// see also CContainer::VstUniqueID()
	unsigned char cid[16]{};
	wrapper::canonicalBytes(wrapper::vst3ClassIds(*plugin).processor, cid);

	unsigned char vst2ID[4]{};
	int i2 = 0;
	for (int i = 0; i < sizeof(cid); ++i)
	{
		vst2ID[i2] = vst2ID[i2] + cid[i];
		i2 = (i2 + 1) & 0x03;
	}

	for (auto& c : vst2ID)
	{
		c = 0x20 + (std::min)(0x7e, c & 0x7f); // make printable
	}

	return *((int32_t*)vst2ID);
}


/** Fill a PClassInfo structure with information about the class at the specified index. */
tresult MyVstPluginFactory::getClassInfo (int32 index, PClassInfo* info)
{
	initialize();

	const auto numClasses = gmpi::hosting::factory::getInstance().getPlugincount() * 2;

	if (index < 0 || index >= numClasses)
	{
		return kInvalidArgument;
	}

	const int pluginIndex = index / 2;
	const int classIndex = index % 2;

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kResultFalse;

	const auto ids = wrapper::vst3ClassIds(*plugin);

	switch(classIndex)
	{
	case 0:
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		ids.processor.toTUID(info->cid);
		break;
	case 1:
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		ids.controller.toTUID(info->cid);
		break;
	}

	info->cardinality = PClassInfo::kManyInstances;

	strncpy8 (info->name, plugin->name.c_str(), PClassInfo::kNameSize );

	return kResultOk;
}

/** Returns the class info (version 2) for a given index. */
tresult MyVstPluginFactory::getClassInfo2 (int32 index, PClassInfo2* info)
{
	initialize();

	const auto numClasses = gmpi::hosting::factory::getInstance().getPlugincount() * 2;

	if (index < 0 || index >= numClasses)
	{
		return kInvalidArgument;
	}

	const int pluginIndex = index / 2;
	const int classIndex = index % 2;

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kResultFalse;

	const auto version = pluginVersion(*plugin);

	const auto ids = wrapper::vst3ClassIds(*plugin);

	info->cardinality = PClassInfo::kManyInstances;

	strncpy8 (info->name, plugin->name.c_str(), PClassInfo::kNameSize );
	strncpy8 (info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize );
	strncpy8 (info->vendor, plugin->vendorName.c_str(), PClassInfo2::kVendorSize );
	strncpy8 (info->version, version.c_str(), PClassInfo2::kVersionSize );

	const auto subCategories = calcSubCategories(*plugin);

	switch(classIndex)
	{
	case 0:
		info->classFlags = Vst::kDistributable;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		ids.processor.toTUID(info->cid);
		break;
	case 1:
		info->classFlags = 0;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		ids.controller.toTUID(info->cid);
		break;
	}

	return kResultOk;
}

/** Returns the unicode class info for a given index. */
tresult MyVstPluginFactory::getClassInfoUnicode (int32 index, PClassInfoW* info)
{
	initialize();
 
	const auto numClasses = gmpi::hosting::factory::getInstance().getPlugincount() * 2;

	if (index < 0 || index >= numClasses)
	{
		return kInvalidArgument;
	}

	const int pluginIndex = index / 2;
	const int classIndex = index % 2;

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kResultFalse;

	const auto version = pluginVersion(*plugin);

	const auto ids = wrapper::vst3ClassIds(*plugin);
	const auto subCategories = calcSubCategories(*plugin);

	switch(classIndex)
	{
	case 0:
		info->classFlags = Vst::kDistributable;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		ids.processor.toTUID(info->cid);
		break;
	case 1:
		info->classFlags = 0;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		ids.controller.toTUID(info->cid);
		break;
	}

	info->cardinality = PClassInfo::kManyInstances;

	str8ToStr16 (info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize );
	str8ToStr16 (info->version, version.c_str(), PClassInfo2::kVersionSize );
	str8ToStr16 (info->vendor, plugin->vendorName.c_str(), PClassInfo2::kVendorSize );
	str8ToStr16 (info->name, plugin->name.c_str(), PClassInfo::kNameSize );

	return kResultOk;
}
	
namespace
{
// Borrowed, not owned. The host guarantees this outlives the module, and the
// factory is a singleton with no defined teardown point at which we could
// safely release it.
Steinberg::FUnknown* g_hostContext{};
}

/** Receives information about host*/
tresult MyVstPluginFactory::setHostContext (FUnknown* context)
{
	g_hostContext = context;
	return kResultOk;
}

FUnknown* MyVstPluginFactory::getHostContext()
{
	return g_hostContext;
}

/** Create a new class instance. */
tresult MyVstPluginFactory::createInstance (FIDString cid, FIDString iid, void** obj)
{
	FUID classId = (FUID) *(TUID*)cid;
	FUID interfaceId = (FUID) *(TUID*)iid;

	initialize();

	FUnknown* instance{};

	const auto numPlugins = gmpi::hosting::factory::getInstance().getPlugincount();

	for (int pluginIndex = 0 ; pluginIndex < numPlugins ; ++pluginIndex)
	{
		auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);

		const auto ids = wrapper::vst3ClassIds(*plugin);

		if (/*interfaceId == IComponent::iid ||*/ classId == ids.processor)
		{
			auto i = new wrapper::Processor_VST3(*plugin);
			i->setControllerClass(ids.controller); // associate with controller.
			instance = (IAudioProcessor*)i;
			break;
		}
		else if(classId == ids.controller)
		{
			instance = static_cast<Steinberg::Vst::IEditController*>(new wrapper::Controller_VST3(*plugin));
			break;
		}
	}

	if (instance)
	{
		if (instance->queryInterface (iid, obj) == kResultOk)
		{
			instance->release ();
			return kResultOk;
		}
		else
			instance->release ();
	}

	*obj = 0;
	return kNoInterface;
}

void MyVstPluginFactory::initialize()
{
	static auto callOnce = initializeFactory();
}

void MyVstPluginFactory::RegisterXml(const std::string& pluginPath, const char* xml)
{
	//gmpi::hosting::readpluginXml(xml, plugins);

	//for(auto& p : plugins)
	//{
	//	p.pluginPath = pluginPath;
	//}
}

typedef gmpi::ReturnCode (*MP_DllEntry)(void**);

bool MyVstPluginFactory::initializeFactory()
{
#if 0
    std::string pluginPath;
	wrapper::gmpi_dynamic_linking::DLL_HANDLE hinstLib{};
	wrapper::gmpi_dynamic_linking::MP _GetDllHandle(&hinstLib);

	if (!hinstLib)
	{
		return true;
	}

#if _WIN32
	// Use XML data to get list of plugins
	auto hInst = (HINSTANCE)hinstLib;
	HRSRC hRsrc = ::FindResource(hInst,
		MAKEINTRESOURCE(1), // ID
		L"GMPXML");			// type GMPI XML

	if (hRsrc)
	{
		const BYTE* lpRsrc = (BYTE*)LoadResource(hInst, hRsrc);

		if (lpRsrc)
		{
			const BYTE* locked_mem = (BYTE*)LockResource((HANDLE)lpRsrc);
			const std::string xmlFile((char*)locked_mem);

			// cleanup
			UnlockResource((HANDLE)lpRsrc);
			FreeResource((HANDLE)lpRsrc);
//			wrapper::gmpi_dynamic_linking::MP_DllUnload(hinstLib);

			RegisterXml(pluginPath, xmlFile.c_str());

			return true;
		}
	}
#else
  // not needed for built-in XML  #error implement this for mac
#endif
#endif

	return true;
}

//------------------------------------------------------------------------
tresult PLUGIN_API MyVstPluginFactory::queryInterface (FIDString iid, void** obj)
{
	QUERY_INTERFACE (iid, obj, IPluginFactory::iid, IPluginFactory)
	QUERY_INTERFACE (iid, obj, IPluginFactory2::iid, IPluginFactory2)
	QUERY_INTERFACE (iid, obj, IPluginFactory3::iid, IPluginFactory3)
	QUERY_INTERFACE (iid, obj, FUnknown::iid, FUnknown)
	*obj = 0;
	return kNoInterface;
}

bool MyVstPluginFactory::GetOutputsAsStereoPairs()
{
	return true; // for now.  pluginInfo_.outputsAsStereoPairs;
}

std::string MyVstPluginFactory::getVendorName()
{
	return gmpi::hosting::factory::getInstance().getPluginInfo()->vendorName;
}

