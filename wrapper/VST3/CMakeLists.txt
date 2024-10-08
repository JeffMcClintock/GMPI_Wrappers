cmake_minimum_required(VERSION 3.15)

include(FetchContent)

project(VST3_Wrapper)

set(CMAKE_CXX_STANDARD 17)

add_definitions(-D_UNICODE)
add_definitions(-DSE_TARGET_PLUGIN)
add_definitions(-DSE_TARGET_VST3)
add_definitions(-DGMPI_VST3_WRAPPER)
#add_definitions(-DGMPI_HOST_POINTER_SUPPORT)

if (MSVC)
    # Floating Point Model: Fast (/fp:fast)
    # Buffer Security Check: No (/GS-)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast /GS-")
endif()

#factory.cpp

set(srcs
MyVstPluginFactory.h
MyVstPluginFactory.cpp
adelayprocessor.h
adelayprocessor.cpp
adelaycontroller.h
adelaycontroller.cpp
VST3EditorBase.h
VST3EditorBase.cpp
${ADAPTORS_COMMON_FOLDER}/StagingMemoryBuffer.h
${ADAPTORS_COMMON_FOLDER}/interThreadQue.h
${ADAPTORS_COMMON_FOLDER}/interThreadQue.cpp
${ADAPTORS_COMMON_FOLDER}/HostControls.h
${ADAPTORS_COMMON_FOLDER}/HostControls.cpp
${ADAPTORS_COMMON_FOLDER}/my_input_stream.h
${ADAPTORS_COMMON_FOLDER}/my_input_stream.cpp
${ADAPTORS_COMMON_FOLDER}/interThreadQue.cpp
${ADAPTORS_COMMON_FOLDER}/interThreadQue.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.h
${ADAPTORS_COMMON_FOLDER}/tinyXml2/tinyxml2.cpp

${ADAPTORS_COMMON_FOLDER}/dynamic_linking.h
${ADAPTORS_COMMON_FOLDER}/dynamic_linking.cpp
${ADAPTORS_COMMON_FOLDER}/MpParameter.h
${ADAPTORS_COMMON_FOLDER}/MpParameter.cpp
${ADAPTORS_COMMON_FOLDER}/GmpiResourceManager.h
${ADAPTORS_COMMON_FOLDER}/GmpiResourceManager.cpp
${ADAPTORS_COMMON_FOLDER}/platform_plugin.cpp
${ADAPTORS_COMMON_FOLDER}/Controller.h
${ADAPTORS_COMMON_FOLDER}/Controller.cpp
${ADAPTORS_COMMON_FOLDER}/MyTypeTraits.h
${ADAPTORS_COMMON_FOLDER}/MyTypeTraits.cpp
${ADAPTORS_COMMON_FOLDER}/RawConversions.h
${ADAPTORS_COMMON_FOLDER}/RawConversions.cpp
${ADAPTORS_COMMON_FOLDER}/BundleInfo.h
${ADAPTORS_COMMON_FOLDER}/BundleInfo.cpp
${ADAPTORS_COMMON_FOLDER}/ProcessorStateManager.h
${ADAPTORS_COMMON_FOLDER}/ProcessorStateManager.cpp
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

# adelay.def

IF(WIN32)
set(srcs ${srcs}
SEVSTGUIEditorWin.h
SEVSTGUIEditorWin.cpp
${VST3_SDK}/public.sdk/source/main/dllmain.cpp
)
ELSE()
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


#${CMAKE_CURRENT_SOURCE_DIR}
#${ADAPTORS_COMMON_FOLDER}
#${ADAPTORS_COMMON_FOLDER}/../Shared/

include_directories(
${VST3_SDK}
${DSP_CORE}
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
endif()

include (GenerateExportHeader)
#add_library(${PROJECT_NAME} MODULE ${sdk_srcs} ${srcs} ${vst_gui_minimal_srcs})
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

#TARGET_LINK_LIBRARIES( ${PROJECT_NAME} GMPIPluginlib )

if(CMAKE_HOST_WIN32)
target_link_libraries(${PROJECT_NAME} d3d11.lib d2d1 dwrite windowscodecs)
endif()

target_compile_definitions (${PROJECT_NAME} PRIVATE -D_UNICODE -DUNICODE)

if(APPLE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE TRUE)
set_target_properties(${PROJECT_NAME} PROPERTIES BUNDLE_EXTENSION vst3)
#  set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX )

  # Place module xml in bundle 'Resources' folder.
#  set(xml_path ${PROJECT_NAME}.xml)
#  target_sources(${PROJECT_NAME} PUBLIC ${xml_path})
#  set_source_files_properties(${xml_path} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

# windows dll extension
# set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".vst3")

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
