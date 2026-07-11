cmake_minimum_required(VERSION 3.24)

project(
  GaussianSceneWorkbenchNative
  VERSION 0.3.0
  DESCRIPTION "Native desktop workspace for Gaussian scene reconstruction"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(GSW_RELEASE_DATE "2026-07-12" CACHE STRING "Release date shown by the application")

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Widgets OpenGL OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(GaussianSceneWorkbench WIN32
  src/AppTheme.cpp
  src/AppTheme.h
  src/MainWindow.cpp
  src/MainWindow.h
  src/NativeViewport.cpp
  src/NativeViewport.h
  src/PlyPointCloudLoader.cpp
  src/PlyPointCloudLoader.h
  src/ProcessSupervisor.cpp
  src/ProcessSupervisor.h
  src/SceneEditModel.cpp
  src/SceneEditModel.h
  src/TrainingDialog.cpp
  src/TrainingDialog.h
  src/WorkspaceDocument.cpp
  src/WorkspaceDocument.h
  src/main.cpp
)

target_include_directories(GaussianSceneWorkbench PRIVATE src)
target_link_libraries(GaussianSceneWorkbench PRIVATE
  Qt6::Core
  Qt6::Concurrent
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
)
target_compile_definitions(GaussianSceneWorkbench PRIVATE
  GSW_VERSION="${PROJECT_VERSION}"
  GSW_RELEASE_DATE="${GSW_RELEASE_DATE}"
)

if(MSVC)
  target_compile_options(GaussianSceneWorkbench PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(GaussianSceneWorkbench PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(GaussianSceneWorkbench PROPERTIES
  OUTPUT_NAME "Gaussian Scene Workbench"
  WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS GaussianSceneWorkbench
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 6.8 QUIET COMPONENTS Test)
  if(TARGET Qt6::Test)
    qt_add_executable(gsw_native_tests
      tests/WorkspaceDocumentTests.cpp
      src/PlyPointCloudLoader.cpp
      src/PlyPointCloudLoader.h
      src/SceneEditModel.cpp
      src/SceneEditModel.h
      src/WorkspaceDocument.cpp
      src/WorkspaceDocument.h
    )
    target_include_directories(gsw_native_tests PRIVATE src)
    target_link_libraries(gsw_native_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    if(MSVC)
      target_compile_options(gsw_native_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME workspace_document COMMAND gsw_native_tests)
  endif()
endif()
