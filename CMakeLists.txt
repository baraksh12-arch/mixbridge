cmake_minimum_required(VERSION 3.22)
project(MixBridge VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Platform detection
# ============================================================================

if(APPLE)
    set(MIXBRIDGE_MACOS TRUE)
    enable_language(OBJCXX)
elseif(WIN32)
    set(MIXBRIDGE_WINDOWS TRUE)
endif()

# ============================================================================
# JUCE - assumes JUCE is cloned alongside this project
# Set JUCE_DIR if JUCE is in a different location:
#   cmake -DJUCE_DIR=/path/to/JUCE ..
# ============================================================================

if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
endif()

if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${JUCE_DIR}.\n"
        "Clone JUCE: git clone https://github.com/juce-framework/JUCE.git\n"
        "Or set -DJUCE_DIR=/path/to/JUCE")
endif()

add_subdirectory(${JUCE_DIR} JUCE EXCLUDE_FROM_ALL)

# ============================================================================
# Plugin definition
# ============================================================================

juce_add_plugin(MixBridge
    # Plugin identity
    COMPANY_NAME                "MixBridge Audio"
    COMPANY_WEBSITE             "https://mixbridge.audio"
    COMPANY_EMAIL               "dev@mixbridge.audio"
    PLUGIN_MANUFACTURER_CODE    "MXBR"
    PLUGIN_CODE                 "MxBr"

    # Plugin type
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    # Formats to build
    FORMATS                     VST3 AU Standalone

    # Bundle identifiers
    VST3_CATEGORIES             "Analyzer" "Network"
    AU_MAIN_TYPE                kAudioUnitType_Effect
    BUNDLE_ID                   "audio.mixbridge.plugin"

    # Plugin metadata
    PLUGIN_NAME                 "MixBridge"
    PRODUCT_NAME                "MixBridge"
    VERSION                     "1.0.0"

    # Icon
    ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_512.png"
    ICON_SMALL                  "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon_64.png"

    # Copy plugin to system folder after build (dev convenience)
    COPY_PLUGIN_AFTER_BUILD     TRUE

    # macOS deployment target
    HARDENED_RUNTIME_ENABLED    TRUE
    MICROPHONE_PERMISSION_ENABLED FALSE
    NETWORK_MULTICAST_ENABLED   TRUE
)

# ============================================================================
# Source files
# ============================================================================

target_sources(MixBridge
    PRIVATE
        # Core
        Plugin/Source/Core/MixBridgeProcessor.cpp
        Plugin/Source/Core/MixBridgeProcessor.h

        # UI
        Plugin/Source/UI/MixBridgeEditor.cpp
        Plugin/Source/UI/MixBridgeEditor.h

        # Network
        Plugin/Source/Network/NetworkStreamer.h
        Plugin/Source/Network/UDPSocket.h

        # Discovery
        Plugin/Source/Discovery/BonjourDiscovery.h

        # Shared - Protocol
        Shared/Protocol/MixBridgeProtocol.h

        # Shared - Ring Buffer
        Shared/RingBuffer/LockFreeRingBuffer.h

        # Shared - Common
        Shared/Common/HighResClock.h
        Shared/Common/NetworkStats.h
)

# ============================================================================
# Include paths
# ============================================================================

target_include_directories(MixBridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Plugin/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Shared
)

# ============================================================================
# JUCE modules
# ============================================================================

target_compile_definitions(MixBridge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(MixBridge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_plugin_client
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ============================================================================
# Platform-specific settings
# ============================================================================

if(MIXBRIDGE_MACOS)
    # Bonjour is built-in on macOS via dns_sd
    target_link_libraries(MixBridge PRIVATE
        "-framework Foundation"
        "-framework CoreServices"
    )

    # Network entitlement for LAN access
    set_target_properties(MixBridge PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
            "${CMAKE_CURRENT_SOURCE_DIR}/Plugin/CMake/MixBridge.entitlements"
    )

    # Apple Silicon + Intel universal binary
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")

elseif(MIXBRIDGE_WINDOWS)
    # Windows networking
    target_link_libraries(MixBridge PRIVATE ws2_32)

    # Try to find Bonjour SDK (Apple's Bonjour for Windows)
    find_path(BONJOUR_INCLUDE_DIR dns_sd.h
        PATHS
            "C:/Program Files/Bonjour SDK/Include"
            "C:/Program Files (x86)/Bonjour SDK/Include"
    )
    find_library(BONJOUR_LIB dnssd
        PATHS
            "C:/Program Files/Bonjour SDK/Lib/x64"
            "C:/Program Files (x86)/Bonjour SDK/Lib/Win32"
    )
    if(BONJOUR_INCLUDE_DIR AND BONJOUR_LIB)
        target_include_directories(MixBridge PRIVATE ${BONJOUR_INCLUDE_DIR})
        target_link_libraries(MixBridge PRIVATE ${BONJOUR_LIB})
        target_compile_definitions(MixBridge PRIVATE MIXBRIDGE_HAS_BONJOUR=1)
        message(STATUS "Bonjour SDK found: ${BONJOUR_LIB}")
    else()
        target_compile_definitions(MixBridge PRIVATE MIXBRIDGE_HAS_BONJOUR=0)
        message(STATUS "Bonjour SDK not found - using UDP broadcast fallback discovery")
    endif()

    # VS compiler flags
    if(MSVC)
        target_compile_options(MixBridge PRIVATE
            /W4
            /WX-
            /permissive-
            /Zc:__cplusplus
            /arch:AVX2    # SIMD optimizations
        )
    endif()
endif()

# ============================================================================
# Compiler warnings (GCC/Clang)
# ============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(MixBridge PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter      # JUCE has many
        -Wno-gnu-zero-variadic-macro-arguments
        -ffast-math                # Safe for audio DSP
    )
endif()

# ============================================================================
# Tests (optional, built separately)
# ============================================================================

option(MIXBRIDGE_BUILD_TESTS "Build unit tests" OFF)

if(MIXBRIDGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ============================================================================
# Install targets
# ============================================================================

if(MIXBRIDGE_MACOS)
    install(
        TARGETS MixBridge_VST3
        DESTINATION "/Library/Audio/Plug-Ins/VST3"
    )
    install(
        TARGETS MixBridge_AU
        DESTINATION "/Library/Audio/Plug-Ins/Components"
    )
endif()

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "MixBridge Configuration:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  JUCE dir:     ${JUCE_DIR}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
if(MIXBRIDGE_MACOS)
    message(STATUS "  Platform:     macOS (Universal)")
    message(STATUS "  Deploy target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
elseif(MIXBRIDGE_WINDOWS)
    message(STATUS "  Platform:     Windows")
endif()
message(STATUS "")
