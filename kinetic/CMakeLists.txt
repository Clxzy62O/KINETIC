cmake_minimum_required(VERSION 3.20)
project(Kinetic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KINETIC_SANITIZE "Build with ASan/UBSan" OFF)
option(KINETIC_BUILD_TESTS "Build tests" ON)
option(KINETIC_BUILD_UI "Build the Qt 6 desktop application" OFF)

find_package(Threads REQUIRED)

add_library(kinetic_engine STATIC
  src/core/Json.cpp
  src/core/Platform.cpp
  src/anim/Keyframes.cpp
  src/project/Model.cpp
  src/project/Velocity.cpp
  src/edit/Commands.cpp
  src/edit/Edit.cpp
  src/edit/BeatSync.cpp
  src/io/Serialize.cpp
  src/io/Storage.cpp
  src/io/Preset.cpp
  src/media/Process.cpp
  src/media/Probe.cpp
  src/media/Decoder.cpp
  src/media/FrameCache.cpp
  src/media/AudioDecode.cpp
  src/render/Image.cpp
  src/render/Effects.cpp
  src/render/Compositor.cpp
  src/render/Evaluator.cpp
  src/audio/Fft.cpp
  src/audio/BeatDetect.cpp
  src/export/Exporter.cpp
)
target_include_directories(kinetic_engine PUBLIC src)
target_link_libraries(kinetic_engine PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(kinetic_engine PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus)
  target_compile_definitions(kinetic_engine PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(kinetic_engine PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(KINETIC_SANITIZE)
  target_compile_options(kinetic_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(kinetic_engine PUBLIC -fsanitize=address,undefined)
endif()

add_executable(kinetic-cli apps/kinetic-cli/main.cpp apps/kinetic-cli/Demo.cpp)
target_link_libraries(kinetic-cli PRIVATE kinetic_engine)

if(KINETIC_BUILD_TESTS)
  enable_testing()
  add_executable(kinetic_tests
    tests/main.cpp
    tests/test_core.cpp
    tests/test_timeline.cpp
    tests/test_io.cpp
    tests/test_audio.cpp
    tests/test_render.cpp
    tests/test_integration.cpp
  )
  target_link_libraries(kinetic_tests PRIVATE kinetic_engine)
  add_test(NAME kinetic_tests COMMAND kinetic_tests)
endif()

if(KINETIC_BUILD_UI)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
  find_package(Qt6 6.4 QUIET COMPONENTS Multimedia)
  qt_standard_project_setup()
  qt_add_executable(kinetic WIN32 MACOSX_BUNDLE
    apps/kinetic/main.cpp
    apps/kinetic/MainWindow.h apps/kinetic/MainWindow.cpp
    apps/kinetic/TimelineWidget.h apps/kinetic/TimelineWidget.cpp
    apps/kinetic/Panels.h apps/kinetic/Panels.cpp
    apps/kinetic/Document.h
    apps/kinetic-cli/Demo.cpp)
  target_include_directories(kinetic PRIVATE apps/kinetic apps/kinetic-cli)
  target_link_libraries(kinetic PRIVATE kinetic_engine Qt6::Widgets)
  if(TARGET Qt6::Multimedia)
    target_link_libraries(kinetic PRIVATE Qt6::Multimedia)
    target_compile_definitions(kinetic PRIVATE KINETIC_HAS_AUDIO=1)
  else()
    message(STATUS "Qt6 Multimedia not found: building without audio preview")
  endif()
endif()
