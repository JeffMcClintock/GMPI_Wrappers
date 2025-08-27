cmake_minimum_required(VERSION 3.15)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)

project(AU_Wrapper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-D_UNICODE)
add_definitions(-DSE_TARGET_PLUGIN)
add_definitions(-DSE_TARGET_AU)
    
set(srcs
CocoaNamespaceMacros.h
SeAuVersion.h
SeAuEditor.mm
AudioUnit.cpp
AudioUnit.h
SEVSTGUIEditorMac.h
SEVSTGUIEditorMac.cpp
)

set(au_sdk_srcs
${AU_SDK}/AUBase.cpp
${AU_SDK}/AUBase.h
${AU_SDK}/AUBuffer.cpp
${AU_SDK}/AUBuffer.h
${AU_SDK}/AUBufferAllocator.cpp
${AU_SDK}/AUEffectBase.cpp
${AU_SDK}/AUEffectBase.h
${AU_SDK}/AUInputElement.cpp
${AU_SDK}/AUInputElement.h
${AU_SDK}/AUMIDIBase.cpp
${AU_SDK}/AUMIDIBase.h
${AU_SDK}/AUMIDIUtility.h
${AU_SDK}/AUOutputElement.cpp
${AU_SDK}/AUOutputElement.h
${AU_SDK}/AUPlugInDispatch.cpp
${AU_SDK}/AUPlugInDispatch.h
${AU_SDK}/AUScopeElement.cpp
${AU_SDK}/AUScopeElement.h
${AU_SDK}/AUSilentTimeout.h
${AU_SDK}/AUUtility.h
${AU_SDK}/AudioUnitSDK.h
${AU_SDK}/ComponentBase.cpp
${AU_SDK}/ComponentBase.h
${AU_SDK}/MusicDeviceBase.cpp
${AU_SDK}/MusicDeviceBase.h
)

include_directories(
${DSP_CORE}
${AU_SDK}
${gmpi_sdk_folder}/Core
${gmpi_ui_folder}
${gmpi_ui_folder}/helpers
)

source_group(se_sdk FILES ${sdk_srcs})

if(APPLE)
FIND_LIBRARY(COREFOUNDATION_LIBRARY CoreFoundation )
MARK_AS_ADVANCED (COREFOUNDATION_LIBRARY)
FIND_LIBRARY(QUARTZCORE_LIBRARY QuartzCore )
MARK_AS_ADVANCED (QUARTZCORE_LIBRARY)
FIND_LIBRARY(COCOA_LIBRARY Cocoa )
MARK_AS_ADVANCED (COCOA_LIBRARY)
FIND_LIBRARY(CORETEXT_LIBRARY CoreText )
MARK_AS_ADVANCED (CORETEXT_LIBRARY)
FIND_LIBRARY(OPENGL_LIBRARY OpenGL )
MARK_AS_ADVANCED (OPENGL_LIBRARY)
FIND_LIBRARY(IMAGEIO_LIBRARY ImageIO )
MARK_AS_ADVANCED (IMAGEIO_LIBRARY)
FIND_LIBRARY(COREGRAPHICS_LIBRARY CoreGraphics )
MARK_AS_ADVANCED (COREGRAPHICS_LIBRARY)
FIND_LIBRARY(ACCELERATE_LIBRARY Accelerate )
MARK_AS_ADVANCED (ACCELERATE_LIBRARY)
FIND_LIBRARY(AUDIOUNIT_LIBRARY AudioUnit )
MARK_AS_ADVANCED (AUDIOUNIT_LIBRARY)
FIND_LIBRARY(AUDIOTOOLBOX_LIBRARY AudioToolbox )
MARK_AS_ADVANCED (AUDIOTOOLBOX_LIBRARY)
endif()

include (GenerateExportHeader)
add_library(${PROJECT_NAME} MODULE ${sdk_srcs} ${srcs} ${au_sdk_srcs})

TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREFOUNDATION_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COCOA_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${CORETEXT_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${OPENGL_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${IMAGEIO_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREGRAPHICS_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${ACCELERATE_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${QUARTZCORE_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${AUDIOUNIT_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${AUDIOTOOLBOX_LIBRARY} )

TARGET_LINK_LIBRARIES( ${PROJECT_NAME} SynthEditLib )

target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Release>:NDEBUG>
  -D_UNICODE
  -DUNICODE
)

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
  set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION component)

  # generate debug symbols
  target_compile_options(${PROJECT_NAME} PRIVATE
      $<$<CONFIG:Debug>:-g>
      -Wno-deprecated-declarations
      -Wno-deprecated-enum-enum-conversion
)
  
  set_target_properties(${PROJECT_NAME} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT
    $<$<CONFIG:Debug>:dwarf-with-dsym>
  )

endif()

set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".au")
