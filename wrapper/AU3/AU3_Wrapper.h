#pragma once

/*
  AudioUnit V3 (app-extension) wrapper for GMPI plugins - macOS and iOS.

  Where the AU2 wrapper subclasses ausdk::AUBase and ships as a .component the
  host dlopens, this one subclasses AUAudioUnit and ships inside an .appex the
  system loads out-of-process. The GMPI side is identical: the same
  gmpi_processor / gmpi_controller_holder pair from GMPI/Hosting that every
  other wrapper drives, discovered through the same MP_GetFactory the plugin
  links in.
*/

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>

#ifdef __cplusplus
namespace gmpi { namespace hosting { class gmpi_controller_holder; } }
#endif

@interface GmpiAudioUnit : AUAudioUnit

#ifdef __cplusplus
// The editor host for the view controller. The AU2 wrapper smuggles this same
// pointer to its Cocoa view through private property 64000 because view and
// unit meet across a C API; here both live in this extension process, so it is
// just a method.
- (gmpi::hosting::gmpi_controller_holder*)gmpiController;
#endif

@end
