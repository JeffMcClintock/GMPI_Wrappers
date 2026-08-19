// Compiled directly into each consuming AUv3 app extension (the way
// wrapperAu2.cpp is compiled into each .component).
//
// The extension's principal class - GmpiAUViewController - is looked up BY
// NAME from the appex Info.plist at load time, so nothing in the extension
// references it by symbol, and the linker would otherwise leave the whole
// wrapper out of the static library. This file, being an ordinary source of
// the appex target, forces both Objective-C classes to link.

#import "AU3_ViewController.h"
#import "AU3_Wrapper.h"

extern "C" int gmpi_au3_forceLinkViewController(void);

extern "C" int gmpi_au3_forceLink(void)
{
	return gmpi_au3_forceLinkViewController()
		+ (int)(intptr_t)[GmpiAudioUnit class];
}

// A constructor so the references above are reachable without anything having
// to call gmpi_au3_forceLink() by hand.
__attribute__((constructor)) static void gmpi_au3_keepClasses(void)
{
	(void)gmpi_au3_forceLink();
}
