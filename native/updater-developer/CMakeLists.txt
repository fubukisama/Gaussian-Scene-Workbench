# SPDX-License-Identifier: GPL-3.0-or-later
# Developer-only standalone build: intentionally no install() rule and no
# dependency from native/, updater/, or the public runtime/package targets.
cmake_minimum_required(VERSION 3.24)
project(GswNativeDeveloperPublisher LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  message(FATAL_ERROR "The native developer publisher supports Windows only")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
add_library(gsw_developer_miniz STATIC ../third_party/miniz/miniz.c)
target_include_directories(gsw_developer_miniz PUBLIC ../third_party/miniz)
add_library(gsw_developer_activation_core STATIC
  ../src/NativeActivationCore.cpp ../src/NativeActivationCore.h)
target_include_directories(gsw_developer_activation_core PUBLIC ../src
  PRIVATE ../third_party/nlohmann)
target_link_libraries(gsw_developer_activation_core PUBLIC Qt6::Core PRIVATE gsw_developer_miniz)
add_executable(gsw_native_developer_publisher
  ../src/NativeDeveloperPublisherMain.cpp ../src/NativeDeveloperPublisher.cpp)
target_include_directories(gsw_native_developer_publisher PRIVATE
  ../third_party/nlohmann ../third_party/miniz)
target_link_libraries(gsw_native_developer_publisher PRIVATE gsw_developer_activation_core)
set_target_properties(gsw_native_developer_publisher PROPERTIES
  OUTPUT_NAME "GSW Native Developer Publisher")
foreach(target gsw_developer_activation_core gsw_native_developer_publisher)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endif()
endforeach()
