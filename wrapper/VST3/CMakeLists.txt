cmake_minimum_required(VERSION 3.15)

project(VST3_Wrapper)

set(CMAKE_CXX_STANDARD 20)

add_definitions(-D_UNICODE)

if (MSVC)
    # Floating Point Model: Fast (/fp:fast)
    # Buffer Security Check: No (/GS-)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast /GS-")
endif()

set(srcs
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

MyVstPluginFactory.h
MyVstPluginFactory.cpp
Processor_VST3.h
Processor_VST3.cpp
Controller_VST3.h
Controller_VST3.cpp
VST3EditorBase.h
VST3EditorBase.cpp
${ADAPTORS_COMMON_FOLDER}/my_input_stream.h
${ADAPTORS_COMMON_FOLDER}/my_input_stream.cpp
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp

${ADAPTORS_COMMON_FOLDER}/dynamic_linking.h
${ADAPTORS_COMMON_FOLDER}/dynamic_linking.cpp
${ADAPTORS_COMMON_FOLDER}/BundleInfo.h
${ADAPTORS_COMMON_FOLDER}/BundleInfo.cpp
${ADAPTORS_COMMON_FOLDER}/conversion.h
${ADAPTORS_COMMON_FOLDER}/conversion.cpp
${ADAPTORS_COMMON_FOLDER}/FileFinder.h
${ADAPTORS_COMMON_FOLDER}/FileFinder.cpp
${ADAPTORS_COMMON_FOLDER}/FileWatcher.h
${ADAPTORS_COMMON_FOLDER}/FileWatcher.cpp
${ADAPTORS_COMMON_FOLDER}/it_enum_list.h
${ADAPTORS_COMMON_FOLDER}/it_enum_list.cpp

${gmpi_ui_folder}/backends/DrawingFrameCommon.h
${gmpi_ui_folder}/backends/DrawingFrameCommon.cpp
${gmpi_ui_folder}/backends/DrawingFrameWin.h
${gmpi_ui_folder}/backends/DrawingFrameWin.cpp
${gmpi_ui_folder}/backends/DirectXGfx.h
${gmpi_ui_folder}/backends/DirectXGfx.cpp
${gmpi_ui_folder}/helpers/Timer.h
${gmpi_ui_folder}/helpers/Timer.cpp
)

IF(WIN32)
set(srcs ${srcs}
SEVSTGUIEditorWin.h
SEVSTGUIEditorWin.cpp
${VST3_SDK}/public.sdk/source/main/dllmain.cpp
)
ENDIF()

if(APPLE)
set(srcs ${srcs}
SEVSTGUIEditorMac.h
SEVSTGUIEditorMac.cpp
${gmpi_ui_folder}/backends/DrawingFrameMac.mm
${gmpi_ui_folder}/backends/CocoaGfx.h
${gmpi_ui_folder}/backends/Gfx_base.h
${gmpi_ui_folder}/backends/Bezier.h
${VST3_SDK}/public.sdk/source/main/macmain.cpp
)
ENDIF()

set(vst_gui_minimal_srcs
    ${VST3_SDK}/public.sdk/source/common/pluginview.h
    ${VST3_SDK}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
    
    ${VST3_SDK}/public.sdk/source/vst/vstbus.h
    ${VST3_SDK}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.h
    ${VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp

    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.h
    ${VST3_SDK}/public.sdk/source/vst/vsteditcontroller.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp

    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.h
    ${VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.h
    ${VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.h
    ${VST3_SDK}/public.sdk/source/vst/vstparameters.cpp 
)

include_directories(
${VST3_SDK}
${DSP_CORE}
${gmpi_sdk_folder}
${gmpi_sdk_folder}/Core
${gmpi_ui_folder}
${gmpi_ui_folder}/helpers
${ADAPTORS_COMMON_FOLDER}
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
endif()

include (GenerateExportHeader)

add_library(${PROJECT_NAME} STATIC ${sdk_srcs} ${srcs} ${vst_gui_minimal_srcs})

# 'DEBUG' is a steinberg thing, ('_DEBUG' is standard C++)
target_compile_definitions(${PROJECT_NAME} PRIVATE 
  $<$<CONFIG:Debug>:_DEBUG>
  $<$<CONFIG:Debug>:DEBUG=1>
  $<$<CONFIG:Release>:DEBUG=0>
  $<$<CONFIG:Release>:NDEBUG>
)

TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREFOUNDATION_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COCOA_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${CORETEXT_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${OPENGL_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${IMAGEIO_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${COREGRAPHICS_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${ACCELERATE_LIBRARY} )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} ${QUARTZCORE_LIBRARY} )

TARGET_LINK_LIBRARIES( ${PROJECT_NAME} base )
TARGET_LINK_LIBRARIES( ${PROJECT_NAME} pluginterfaces )

if(CMAKE_HOST_WIN32)
target_link_libraries(${PROJECT_NAME} Dwmapi dxguid d3d11 d2d1 dwrite windowscodecs)
endif()

target_compile_definitions (${PROJECT_NAME} PRIVATE -D_UNICODE -DUNICODE)

if(APPLE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION vst3)
endif()

# required by Linux build
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

########################### VST3 SDK #######################################

list(APPEND CMAKE_MODULE_PATH "${VST3_SDK}/cmake/modules")

include(SMTG_VST3_SDK)

#---Add base libraries---------------------------
set(VST_SDK TRUE) # used for Base module which provides only a subset of Base for VST-SDK
add_subdirectory(${VST3_SDK}/pluginterfaces ${CMAKE_CURRENT_BINARY_DIR}/pluginterfaces)
add_subdirectory(${VST3_SDK}/base           ${CMAKE_CURRENT_BINARY_DIR}/base)

SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES FOLDER "wrappers")
SET_TARGET_PROPERTIES(base PROPERTIES FOLDER "wrappers")
SET_TARGET_PROPERTIES(pluginterfaces PROPERTIES FOLDER "wrappers")
