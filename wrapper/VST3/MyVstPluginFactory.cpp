#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstcomponent.h"
#include "MyVstPluginFactory.h"
#include "Controller_VST3.h"
#include "Processor_VST3.h"
#include "Common.h"
//#include "wrapper/common/tinyXml2/tinyxml2.h"
#include "wrapper/common/dynamic_linking.h"
#include "Hosting/gmpi_factory.h"

#if 0
#include "BundleInfo.h"
#include "FileFinder.h"
#include "FileFinder.h"
#include "GmpiApiAudio.h"
#include "GmpiSdkCommon.h"
#endif

#if defined( _WIN32 )
extern HINSTANCE ghInst;
#elif __APPLE__
	#include <dlfcn.h>
	#include <CoreFoundation/CoreFoundation.h>

/*
// Hacky, access functions meant only for VSTGUI.
namespace VSTGUI
{
    static void CreateVSTGUIBundleRef();
    static void ReleaseVSTGUIBundleRef();
}
*/

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
/*
//------------------------------------------------------------------------
void ReleaseVSTGUIBundleRef ()
{
	openCount--;
	if (gBundleRef)
		CFRelease (gBundleRef);
	if (openCount == 0)
		gBundleRef = 0;
}
 */
#endif

#if __APPPLE__
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

extern "C"
gmpi::ReturnCode MP_GetFactory( void** returnInterface );

#if 0
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory ()
{
	return MyVstPluginFactory::GetInstance();
}
#endif

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
	strncpy8 (info->email , plugin->vendorEmail.c_str(), PFactoryInfo::kEmailSize);
	info->flags = PFactoryInfo::kUnicode;
	return kResultOk;
}

/** Returns the number of exported classes by this factory. 
If you are using the CPluginFactory implementation provided by the SDK, it returns the number of classes you registered with CPluginFactory::registerClass. */
int32 MyVstPluginFactory::countClasses ()
{
	initialize();

	return static_cast<int32>(2);
}

uint32 hashString(const std::string& s)
{
	uint32_t hash = 5381;
	{
		for (auto c : s)
		{
			hash = ((hash << 5) + hash) + c; // magic * 33 + c
		}
	}

	return hash;
}
	
void textIdtoUuid(const std::string& id, bool isController, Steinberg::TUID& ret) // else Processor
{
	// hash the id
	const auto hash = hashString(id);// ^ (isController ? 0xff : 0);

	//union helper_t
	//{
	//	char plain[16] = { "PluginGMPI     " }; // UUID version 4 because 'G' is 0x47
	//	Steinberg::TUID tuid;
	//};

	//helper_t helper;
	memcpy(ret, "PluginGMPI     ", 16);
	ret[11] = isController ? 'C' : 'P';

	memcpy(ret + 12, &hash, sizeof(hash));

//	helper.plain[6] = 0x40; // UUID version 4
}

int32_t MyVstPluginFactory::getVst2Id64(int32_t pluginIndex) // generated from hash of GUID. Not compatible w 32-bit VSTs.
{
	initialize();

	if (pluginIndex != 0)
	{
		return kInvalidArgument;
	}

	Steinberg::PClassInfoW info;
	getClassInfoUnicode(pluginIndex, &info);

	// generate an VST2 ID from VST3 GUIID.
	// see also CContainer::VstUniqueID()
	unsigned char vst2ID[4]{};
	int i2 = 0;
	for (int i = 0; i < sizeof(info.cid); ++i)
	{
		vst2ID[i2] = vst2ID[i2] + info.cid[i];
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

	Steinberg::TUID procUUid{};
	Steinberg::TUID ctrlUUid{};
	textIdtoUuid(plugin->id, false, procUUid);
	textIdtoUuid(plugin->id,  true, ctrlUUid);

	switch(classIndex)
	{
	case 0:
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		memcpy(info->cid, &procUUid/*(pluginInfo_.processorId.toTUID()*/, sizeof(TUID));
		break;
	case 1:
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		memcpy(info->cid, &ctrlUUid/*(pluginInfo_.controllerId.toTUID()*/, sizeof(TUID));
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

	std::string version{ "1.0.0" }; // for now

	const int pluginIndex = index / 2;
	const int classIndex = index % 2;

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kResultFalse;

	Steinberg::TUID procUUid{};
	Steinberg::TUID ctrlUUid{};
	textIdtoUuid(plugin->id, false, procUUid);
	textIdtoUuid(plugin->id, true, ctrlUUid);

	info->cardinality = PClassInfo::kManyInstances;

	strncpy8 (info->name, plugin->name.c_str(), PClassInfo::kNameSize );
	strncpy8 (info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize );
	strncpy8 (info->vendor, plugin->vendorName.c_str(), PClassInfo2::kVendorSize );
	strncpy8 (info->version, /*pluginInfo_.version_*/version.c_str(), PClassInfo2::kVersionSize );

	const auto subCategories = calcSubCategories(*plugin);

	switch(classIndex)
	{
	case 0:
		info->classFlags = Vst::kDistributable;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		memcpy (info->cid, &procUUid/*(pluginInfo_.processorId.toTUID())*/, sizeof (TUID));
		break;
	case 1:
		info->classFlags = 0;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		memcpy (info->cid, &ctrlUUid/*(pluginInfo_.controllerId.toTUID())*/, sizeof (TUID));
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

	std::string version{ "1.0.0" }; // for now

	const int pluginIndex = index / 2;
	const int classIndex = index % 2;

	auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo(pluginIndex);
	if (!plugin)
		return kResultFalse;

	Steinberg::TUID procUUid{};
	Steinberg::TUID ctrlUUid{};
	textIdtoUuid(plugin->id, false, procUUid);
	textIdtoUuid(plugin->id, true, ctrlUUid);
	const auto subCategories = calcSubCategories(*plugin);

	switch(classIndex)
	{
	case 0:
		info->classFlags = Vst::kDistributable;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstAudioEffectClass, PClassInfo::kCategorySize );
		memcpy(info->cid, &procUUid/*(pluginInfo_.processorId.toTUID())*/, sizeof(TUID));
		break;
	case 1:
		info->classFlags = 0;
		strncpy8 (info->subCategories, subCategories.c_str(), PClassInfo2::kSubCategoriesSize );
		strncpy8 (info->category, kVstComponentControllerClass, PClassInfo::kCategorySize );
		memcpy(info->cid, &ctrlUUid/*(pluginInfo_.controllerId.toTUID())*/, sizeof(TUID));
		break;
	}

	info->cardinality = PClassInfo::kManyInstances;

	str8ToStr16 (info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize );
	str8ToStr16 (info->version, /*pluginInfo_.version_*/version.c_str(), PClassInfo2::kVersionSize );
	str8ToStr16 (info->vendor, plugin->vendorName.c_str(), PClassInfo2::kVendorSize );
	str8ToStr16 (info->name, plugin->name.c_str(), PClassInfo::kNameSize );

	return kResultOk;
}
	
/** Receives information about host*/
tresult MyVstPluginFactory::setHostContext (FUnknown* context)
{
	return kNotImplemented;
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

		Steinberg::TUID procUUid{};
		Steinberg::TUID ctrlUUid{};
		textIdtoUuid(plugin->id, false, procUUid);
		textIdtoUuid(plugin->id, true, ctrlUUid);

		if (/*interfaceId == IComponent::iid ||*/ classId == Steinberg::FUID(procUUid))
		{
			auto i = new wrapper::Processor_VST3(*plugin);
			i->setControllerClass(ctrlUUid); // associate with controller.
			instance = (IAudioProcessor*)i;
			break;
		}
		else if(classId == Steinberg::FUID(ctrlUUid))
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
	std::string pluginPath;
	wrapper::gmpi_dynamic_linking::DLL_HANDLE hinstLib{};
	wrapper::gmpi_dynamic_linking::MP_GetDllHandle(&hinstLib);

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

