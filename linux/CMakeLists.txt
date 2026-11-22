# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "flutter_tts")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "flutter_tts_plugin")

# Sources that don't depend on Flutter; shared with the unit tests.
list(APPEND FLUTTER_TTS_CORE_SOURCES
  "speechd_api.cc"
  "tts_engine.cc"
  "tts_util.cc"
)

list(APPEND PLUGIN_SOURCES
  "flutter_tts_plugin.cc"
  ${FLUTTER_TTS_CORE_SOURCES}
)

add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt.
apply_standard_settings(${PLUGIN_NAME})
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)
# libspeechd (Speech Dispatcher) is loaded at runtime with dlopen(), so it is
# neither a build-time nor a hard runtime dependency. See speechd_api.h.
target_link_libraries(${PLUGIN_NAME} PRIVATE ${CMAKE_DL_LIBS})

# List of absolute paths to libraries that should be bundled with the plugin.
set(flutter_tts_bundled_libraries
  ""
  PARENT_SCOPE
)

# === Tests ===
# Built when the application sets include_flutter_tts_tests (the example app
# does). Run with: build/linux/<arch>/<mode>/plugins/flutter_tts/flutter_tts_test
# The engine tests talk to a real speech-dispatcher and are skipped when none
# is reachable.
if (${include_${PROJECT_NAME}_tests})
if(${CMAKE_VERSION} VERSION_LESS "3.11.0")
message("Unit tests require CMake 3.11.0 or later")
else()
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(${TEST_RUNNER}
  test/tts_util_test.cc
  test/tts_engine_test.cc
  ${FLUTTER_TTS_CORE_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_17)
target_include_directories(${TEST_RUNNER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::GTK ${CMAKE_DL_LIBS})
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)

include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})

endif()  # CMake version check
endif()  # include_${PROJECT_NAME}_tests
