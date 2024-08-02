#include "MyVstPluginFactory.h"

#if __APPLE__
// inlude the bare-minimum from the VST3 SDK
// stuff that must be in the main executable, not a lib.

#include "public.sdk\source\main\macmain.cpp"

#if 0
#include <CoreFoundation/CoreFoundation.h>
#include "pluginterfaces/base/fplatform.h"

bool bundleEntry_internal (CFBundleRef ref);
bool bundleExit_internal (void);


extern "C" {
/** bundleEntry and bundleExit must be provided by the plug-in! */
SMTG_EXPORT_SYMBOL bool bundleEntry (CFBundleRef r)
{
    return bundleEntry_internal(r);
}

SMTG_EXPORT_SYMBOL bool bundleExit (void)
{
    return bundleExit_internal();
}

}
#endif
#endif

// need to export the factory symbol from the main plugin DLL, because the VST3_Wrapper static library can't export symbols on mac.
SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory()
{
	return MyVstPluginFactory::GetInstance();
}

extern "C"
{
static int moduleCounter{ 0 }; // counting for InitDll/ExitDll pairs

SMTG_EXPORT_SYMBOL bool InitDll()
{
	if (++moduleCounter == 1)
		return true; // InitModule();
	return true;
}

SMTG_EXPORT_SYMBOL bool ExitDll()
{
	if (--moduleCounter == 0)
		return true; // DeinitModule();
	if (moduleCounter < 0)
		return false;
	return true;
}
}
