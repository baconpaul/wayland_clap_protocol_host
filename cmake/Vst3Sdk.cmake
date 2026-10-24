# The VST 3 SDK (MIT since 3.8), for vst3_equiv: fetched, or a local checkout with VST3_SDK_DIR.
#
# Only the sources which the host and the plugin need are built, as static libraries, rather than
# the SDK's own CMake project with its examples, VSTGUI and validator:
#   vst3sdk-base     pluginterfaces and base
#   vst3sdk-plugin   public.sdk for a plugin: the factory, SingleComponentEffect
#   vst3sdk-hosting  public.sdk for a host: loading modules, HostApplication

set(VST3_SDK_DIR "" CACHE PATH "A local VST 3 SDK checkout (else fetched)")
set(VST3_SDK_GIT_TAG "v3.8.0_build_66" CACHE STRING "The VST 3 SDK version to fetch")

if(VST3_SDK_DIR)
  set(vst3sdk_dir "${VST3_SDK_DIR}")
else()
  include(FetchContent)
  FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG "${VST3_SDK_GIT_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES base pluginterfaces public.sdk
    SOURCE_SUBDIR do-not-add) # only fetches: we build the sources below
  FetchContent_MakeAvailable(vst3sdk)
  set(vst3sdk_dir "${vst3sdk_SOURCE_DIR}")
endif()

if(NOT EXISTS "${vst3sdk_dir}/pluginterfaces/base/funknown.h")
  message(FATAL_ERROR "No VST 3 SDK in ${vst3sdk_dir}")
endif()

set(sdk "${vst3sdk_dir}")
set(vst "${sdk}/public.sdk/source/vst")

add_library(vst3sdk-base STATIC
  "${sdk}/pluginterfaces/base/conststringtable.cpp"
  "${sdk}/pluginterfaces/base/coreiids.cpp"
  "${sdk}/pluginterfaces/base/funknown.cpp"
  "${sdk}/pluginterfaces/base/ustring.cpp"
  "${sdk}/base/source/baseiids.cpp"
  "${sdk}/base/source/fbuffer.cpp"
  "${sdk}/base/source/fdebug.cpp"
  "${sdk}/base/source/fdynlib.cpp"
  "${sdk}/base/source/fobject.cpp"
  "${sdk}/base/source/fstreamer.cpp"
  "${sdk}/base/source/fstring.cpp"
  "${sdk}/base/source/timer.cpp"
  "${sdk}/base/source/updatehandler.cpp"
  "${sdk}/base/thread/source/fcondition.cpp"
  "${sdk}/base/thread/source/flock.cpp"
  "${sdk}/public.sdk/source/common/commoniids.cpp"
  "${vst}/vstinitiids.cpp")
target_include_directories(vst3sdk-base SYSTEM PUBLIC "${sdk}")
# The SDK wants one of them
target_compile_definitions(vst3sdk-base PUBLIC $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>)
set_target_properties(vst3sdk-base PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
target_compile_options(vst3sdk-base PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(vst3sdk-base PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_library(vst3sdk-plugin STATIC
  "${sdk}/public.sdk/source/common/pluginview.cpp"
  "${sdk}/public.sdk/source/main/moduleinit.cpp"
  "${sdk}/public.sdk/source/main/pluginfactory.cpp"
  "${vst}/vstbus.cpp"
  "${vst}/vstcomponentbase.cpp"
  "${vst}/vstparameters.cpp"
  "${vst}/vstsinglecomponenteffect.cpp") # which includes vsteditcontroller.cpp, renamed
# Only the SDK's SMTG_EXPORT_SYMBOLs leave the plugin
set_target_properties(vst3sdk-plugin PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
target_compile_options(vst3sdk-plugin PRIVATE -w)
target_link_libraries(vst3sdk-plugin PUBLIC vst3sdk-base)
# ModuleEntry() and ModuleExit(), which nothing references: in a static library they would be left
# out, so they are compiled into the plugin
target_sources(vst3sdk-plugin INTERFACE "${sdk}/public.sdk/source/main/linuxmain.cpp")

add_library(vst3sdk-hosting STATIC
  "${vst}/hosting/hostclasses.cpp"
  "${vst}/hosting/module.cpp"
  "${vst}/hosting/module_linux.cpp"
  "${vst}/hosting/pluginterfacesupport.cpp"
  "${vst}/utility/stringconvert.cpp"
  "${sdk}/public.sdk/source/common/memorystream.cpp"
  "${sdk}/public.sdk/source/common/commonstringconvert.cpp")
target_compile_options(vst3sdk-hosting PRIVATE -w)
target_link_libraries(vst3sdk-hosting PUBLIC vst3sdk-base)
