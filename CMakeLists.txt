cmake_minimum_required(VERSION 3.22)

# macOS: universal binary (Apple Silicon + Intel), runs on 10.13+
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
# Windows: static MSVC runtime, so no VC++ redistributable is needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(SunRiser VERSION 1.0.0)

option(SUNRISER_BUILD_TESTS "Build the offline render/GUI test app" OFF)

# Use a local JUCE checkout if present, otherwise download it
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# VST3 -> Ableton Live, FL Studio, Cubase, Bitwig, Studio One, Reaper...
# AU   -> Logic Pro, GarageBand (built automatically on macOS only)
juce_add_plugin(SunRiser
    COMPANY_NAME "Sunny Plugins"
    COMPANY_WEBSITE "https://github.com/sanresume22/sunriser"
    BUNDLE_ID com.sunnyplugins.sunriser
    PLUGIN_MANUFACTURER_CODE Snny
    PLUGIN_CODE SnRs
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "SunRiser"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(SUNRISER_SOURCES
    Source/RiserEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_sources(SunRiser PRIVATE ${SUNRISER_SOURCES})
target_compile_features(SunRiser PRIVATE cxx_std_17)

set(SUNRISER_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1)

target_compile_definitions(SunRiser PUBLIC ${SUNRISER_DEFS})

target_link_libraries(SunRiser
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(SUNRISER_BUILD_TESTS)
    juce_add_console_app(SunRiserTest PRODUCT_NAME "SunRiserTest")
    target_sources(SunRiserTest PRIVATE Tests/RenderTest.cpp ${SUNRISER_SOURCES})
    target_compile_features(SunRiserTest PRIVATE cxx_std_17)
    target_compile_definitions(SunRiserTest PRIVATE ${SUNRISER_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(SunRiserTest PRIVATE juce::juce_audio_utils juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
