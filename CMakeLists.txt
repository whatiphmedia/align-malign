cmake_minimum_required(VERSION 3.22)
project(AlignMalign VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ALIGNMALIGN_BUILD_TESTS "Build the engine test" ON)

# ---------------------------------------------------------------- engine (framework free)
add_library(am_engine INTERFACE)
target_include_directories(am_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/engine/include)

if(ALIGNMALIGN_BUILD_TESTS)
  add_executable(am_engine_test engine/tests/test_engine.cpp)
  target_link_libraries(am_engine_test PRIVATE am_engine)
  add_executable(am_solver_test engine/tests/test_solver.cpp)
  target_link_libraries(am_solver_test PRIVATE am_engine)
  if(MSVC)
    target_compile_definitions(am_solver_test PRIVATE _USE_MATH_DEFINES)
  endif()
  add_executable(am_groups_test engine/tests/test_groups.cpp)
  target_link_libraries(am_groups_test PRIVATE am_engine)
  if(MSVC)
    target_compile_definitions(am_groups_test PRIVATE _USE_MATH_DEFINES)
  endif()
  add_executable(am_latency_test engine/tests/test_latency.cpp)
  target_link_libraries(am_latency_test PRIVATE am_engine)
  if(MSVC)
    target_compile_definitions(am_latency_test PRIVATE _USE_MATH_DEFINES)
  endif()
  add_executable(am_convolver_test engine/tests/test_convolver.cpp)
  target_link_libraries(am_convolver_test PRIVATE am_engine)
  if(MSVC)
    target_compile_definitions(am_convolver_test PRIVATE _USE_MATH_DEFINES)
  endif()
  if(NOT MSVC)
    target_link_libraries(am_engine_test PRIVATE pthread)
  endif()
  if(MSVC)
    target_compile_definitions(am_engine_test PRIVATE _USE_MATH_DEFINES)
  endif()
endif()

# ---------------------------------------------------------------- WebView2 (Windows UI)
# JUCE's FindWebView2 looks in JUCE_WEBVIEW2_PACKAGE_LOCATION; fetch the NuGet package there if nuget is available.
if(WIN32)
  set(AM_NUGET_DIR "${CMAKE_BINARY_DIR}/_deps/Nuget")
  if(NOT EXISTS "${AM_NUGET_DIR}")
    find_program(NUGET_EXE nuget)
    if(NUGET_EXE)
      execute_process(COMMAND "${NUGET_EXE}" install Microsoft.Web.WebView2 -OutputDirectory "${AM_NUGET_DIR}")
    endif()
  endif()
  set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${AM_NUGET_DIR}" CACHE PATH "Where the Microsoft.Web.WebView2 package lives")
endif()

# ---------------------------------------------------------------- JUCE
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(AlignMalign
  COMPANY_NAME "What Iph Media"
  COMPANY_WEBSITE "https://www.krackills.com"
  BUNDLE_ID com.whatiphmedia.alignmalign
  PLUGIN_MANUFACTURER_CODE Wiph
  PLUGIN_CODE AlMg
  PRODUCT_NAME "Auto Malign"
  DESCRIPTION "Auto Malign: align to anything, then malign it"
  FORMATS VST3 Standalone
  VST3_CATEGORIES Fx Tools
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  NEEDS_WEBVIEW2 TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(AlignMalign)

juce_add_binary_data(AlignMalignUI SOURCES plugin/ui/index.html live/AutoMalign/__init__.py)
set_target_properties(AlignMalignUI PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(AlignMalign PRIVATE
  plugin/Source/PluginProcessor.cpp
  plugin/Source/PluginEditor.cpp)

target_compile_definitions(AlignMalign PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
  _USE_MATH_DEFINES)

target_link_libraries(AlignMalign
  PRIVATE
    AlignMalignUI
    am_engine
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
