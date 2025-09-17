cmake_minimum_required(VERSION 3.15)

#set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version")
#set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)

project(AU_Wrapper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-D_UNICODE)

set(srcs
AU2_Wrapper.cpp
AU2_Wrapper.h

AudioUnitView.mm

${gmpi_sdk_folder}/Hosting/parse_enum.h
${gmpi_sdk_folder}/Hosting/xml_spec_reader.h
${gmpi_sdk_folder}/Hosting/xml_spec_reader.cpp
${gmpi_sdk_folder}/Hosting/controller_holder.h
${gmpi_sdk_folder}/Hosting/controller_holder.cpp
${gmpi_sdk_folder}/Hosting/processor_holder.h
${gmpi_sdk_folder}/Hosting/processor_holder.cpp
${gmpi_sdk_folder}/Hosting/message_queues.h
${gmpi_sdk_folder}/Hosting/message_queues.cpp
${gmpi_sdk_folder}/Hosting/gmpi_factory.h
${gmpi_sdk_folder}/Hosting/gmpi_factory.cpp

${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp

${gmpi_ui_folder}/backends/DrawingFrameCommon.h
${gmpi_ui_folder}/backends/DrawingFrameCommon.cpp
${gmpi_ui_folder}/helpers/Timer.h
${gmpi_ui_folder}/helpers/Timer.cpp
)

IF(APPLE)
set(srcs ${srcs}
${gmpi_ui_folder}/backends/DrawingFrameMac.h
${gmpi_ui_folder}/backends/DrawingFrameMac.mm
${gmpi_ui_folder}/backends/CocoaGfx.h
${gmpi_ui_folder}/backends/Gfx_base.h
${gmpi_ui_folder}/backends/Bezier.h
)
ENDIF()

set(AU_H "${AU_SDK}/include/AudioUnitSDK/")
set(AU_S "${AU_SDK}/src/AudioUnitSDK/")

set(au_sdk_srcs
${AU_H}AUBase.h
${AU_H}AUBuffer.h
${AU_H}AUEffectBase.h
${AU_H}AUInputElement.h
${AU_H}AUMIDIBase.h
${AU_H}AUMIDIUtility.h
${AU_H}AUOutputElement.h
${AU_H}AUPlugInDispatch.h
${AU_H}AUScopeElement.h
${AU_H}AUSilentTimeout.h
${AU_H}AUUtility.h
${AU_H}AudioUnitSDK.h
${AU_H}ComponentBase.h
${AU_H}MusicDeviceBase.h
${AU_S}AUBase.cpp
${AU_S}AUBuffer.cpp
${AU_S}AUBufferAllocator.cpp
${AU_S}AUEffectBase.cpp
${AU_S}AUInputElement.cpp
${AU_S}AUMIDIBase.cpp
${AU_S}AUOutputElement.cpp
${AU_S}AUPlugInDispatch.cpp
${AU_S}AUScopeElement.cpp
${AU_S}ComponentBase.cpp
${AU_S}MusicDeviceBase.cpp
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

add_library(${PROJECT_NAME} STATIC ${sdk_srcs} ${srcs} ${au_sdk_srcs})

# Include paths ONLY for the AU wrapper library
target_include_directories(${PROJECT_NAME} PRIVATE
    ${AU_SDK_H}
    ${gmpi_sdk_folder}
    ${gmpi_sdk_folder}/Core
    ${gmpi_ui_folder}
    ${gmpi_ui_folder}/helpers
    ${ADAPTORS_COMMON_FOLDER}
)

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
SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")

