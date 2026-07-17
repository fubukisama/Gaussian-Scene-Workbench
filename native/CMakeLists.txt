cmake_minimum_required(VERSION 3.24)

project(
  GaussianSceneWorkbenchNative
  VERSION 0.3.1
  DESCRIPTION "Native desktop workspace for Gaussian scene reconstruction"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(GSW_RELEASE_DATE "2026-07-16" CACHE STRING "Release date shown by the application")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/build_manifest.json" GSW_BUILD_MANIFEST_JSON)
string(JSON GSW_APP_USER_MODEL_ID GET "${GSW_BUILD_MANIFEST_JSON}" appUserModelId)
string(LENGTH "${GSW_APP_USER_MODEL_ID}" GSW_APP_USER_MODEL_ID_LENGTH)
if(NOT GSW_APP_USER_MODEL_ID MATCHES "^[A-Za-z0-9]+(\\.[A-Za-z0-9]+)+$" OR
   GSW_APP_USER_MODEL_ID_LENGTH GREATER 128)
  message(FATAL_ERROR "build_manifest.json contains an invalid appUserModelId")
endif()
set(GSW_MSVC_SHOWINCLUDES_PREFIX "" CACHE STRING
  "Locale-correct MSVC /showIncludes prefix supplied by the build bootstrap"
)
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja" AND GSW_MSVC_SHOWINCLUDES_PREFIX)
  set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${GSW_MSVC_SHOWINCLUDES_PREFIX}")
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "${GSW_MSVC_SHOWINCLUDES_PREFIX}")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Widgets OpenGL OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(GaussianSceneWorkbench WIN32
  resources.qrc
  src/AppTheme.cpp
  src/AppTheme.h
  src/BackendLocator.cpp
  src/BackendLocator.h
  src/CameraTrajectory.cpp
  src/CameraTrajectory.h
  src/ColmapSupport.cpp
  src/ColmapSupport.h
  src/DatasetImportDialog.cpp
  src/DatasetImportDialog.h
  src/DatasetImportPlan.cpp
  src/DatasetImportPlan.h
  src/ImportEnvironmentProbe.cpp
  src/ImportEnvironmentProbe.h
  src/MainWindow.cpp
  src/MainWindow.h
  src/NativeViewport.cpp
  src/NativeViewport.h
  src/PlyPointCloudLoader.cpp
  src/PlyPointCloudLoader.h
  src/ProcessSupervisor.cpp
  src/ProcessSupervisor.h
  src/ReconstructionDialog.cpp
  src/ReconstructionDialog.h
  src/SceneEditModel.cpp
  src/SceneEditModel.h
  src/ScreenSpaceSelection.cpp
  src/ScreenSpaceSelection.h
  src/TrainingDialog.cpp
  src/TrainingDialog.h
  src/TrainingOutputLocator.cpp
  src/TrainingOutputLocator.h
  src/WorkspaceDocument.cpp
  src/WorkspaceDocument.h
  src/main.cpp
)

if(WIN32)
  target_sources(GaussianSceneWorkbench PRIVATE assets/gsw-app-icon.rc)
  target_link_libraries(GaussianSceneWorkbench PRIVATE Shell32)
endif()

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
  GSW_APP_USER_MODEL_ID="${GSW_APP_USER_MODEL_ID}"
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
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  add_test(
    NAME native_application_launch
    COMMAND GaussianSceneWorkbench --smoke-test
  )
  set_tests_properties(native_application_launch PROPERTIES
    TIMEOUT 15
    ENVIRONMENT
      "QT_OPENGL=software;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins;QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH="
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  )

    qt_add_executable(gsw_native_tests
      tests/WorkspaceDocumentTests.cpp
      src/CameraTrajectory.cpp
      src/CameraTrajectory.h
      src/ColmapSupport.cpp
      src/ColmapSupport.h
      src/PlyPointCloudLoader.cpp
      src/PlyPointCloudLoader.h
      src/SceneEditModel.cpp
      src/SceneEditModel.h
      src/ScreenSpaceSelection.cpp
      src/ScreenSpaceSelection.h
      src/TrainingOutputLocator.cpp
      src/TrainingOutputLocator.h
      src/WorkspaceDocument.cpp
      src/WorkspaceDocument.h
    )
    target_include_directories(gsw_native_tests PRIVATE src)
    target_link_libraries(gsw_native_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    if(MSVC)
      target_compile_options(gsw_native_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME workspace_document COMMAND gsw_native_tests)

    qt_add_executable(gsw_dataset_import_plan_tests
      tests/DatasetImportPlanTests.cpp
      src/DatasetImportPlan.cpp
      src/DatasetImportPlan.h
    )
    target_include_directories(gsw_dataset_import_plan_tests PRIVATE src)
    target_link_libraries(gsw_dataset_import_plan_tests PRIVATE Qt6::Core Qt6::Test)
    if(MSVC)
      target_compile_options(gsw_dataset_import_plan_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME dataset_import_plan COMMAND gsw_dataset_import_plan_tests)

    add_executable(gsw_process_output_fixture
      tests/helpers/ProcessOutputFixture.cpp
    )
    target_link_libraries(gsw_process_output_fixture PRIVATE Qt6::Core)

    qt_add_executable(gsw_process_supervisor_tests
      tests/ProcessSupervisorTests.cpp
      src/ProcessSupervisor.cpp
      src/ProcessSupervisor.h
    )
    target_include_directories(gsw_process_supervisor_tests PRIVATE src)
    target_link_libraries(gsw_process_supervisor_tests PRIVATE Qt6::Core Qt6::Test)
    add_dependencies(gsw_process_supervisor_tests gsw_process_output_fixture)
    if(MSVC)
      target_compile_options(gsw_process_output_fixture PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
      target_compile_options(gsw_process_supervisor_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME process_supervisor COMMAND gsw_process_supervisor_tests)

    qt_add_executable(gsw_workspace_scene_format_tests
      tests/WorkspaceSceneFormatTests.cpp
      src/WorkspaceDocument.cpp
      src/WorkspaceDocument.h
    )
    target_include_directories(gsw_workspace_scene_format_tests PRIVATE src)
    target_link_libraries(gsw_workspace_scene_format_tests PRIVATE Qt6::Core Qt6::Test)
    if(MSVC)
      target_compile_options(gsw_workspace_scene_format_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME workspace_scene_format COMMAND gsw_workspace_scene_format_tests)

    qt_add_executable(gsw_backend_locator_tests
      tests/BackendLocatorTests.cpp
      src/BackendLocator.cpp
      src/BackendLocator.h
    )
    target_include_directories(gsw_backend_locator_tests PRIVATE src)
    target_link_libraries(gsw_backend_locator_tests PRIVATE Qt6::Core Qt6::Test)
    if(MSVC)
      target_compile_options(gsw_backend_locator_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
    add_test(NAME backend_locator COMMAND gsw_backend_locator_tests)
endif()
