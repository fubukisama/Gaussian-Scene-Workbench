# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(GswNativeRecovery LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets LinguistTools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
option(GSW_UPDATER_FIXTURE_BUILD "Build distinct isolated Recovery product" OFF)
option(GSW_RECOVERY_UI_TESTS "Build private real-Helper Recovery UI acceptance" OFF)
if(GSW_RECOVERY_UI_TESTS AND NOT GSW_UPDATER_FIXTURE_BUILD)
  message(FATAL_ERROR "Recovery UI acceptance is a distinct fixture product only")
endif()
set(native_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(repo_root "${native_root}/..")
file(GLOB i18n_sources CONFIGURE_DEPENDS "${native_root}/src/*.cpp" "${native_root}/src/*.h")
set(i18n_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n")
file(MAKE_DIRECTORY "${i18n_dir}")
set(catalogs)
foreach(language zh_CN en_US ja_JP)
  list(APPEND catalogs "${i18n_dir}/workbench_${language}.qm")
endforeach()
add_custom_command(OUTPUT ${catalogs}
  COMMAND "${Python3_EXECUTABLE}" -I -B "${repo_root}/scripts/native_i18n.py" --emit-ts "${i18n_dir}"
  COMMAND Qt6::lrelease "${i18n_dir}/workbench_zh_CN.ts" -qm "${i18n_dir}/workbench_zh_CN.qm"
  COMMAND Qt6::lrelease "${i18n_dir}/workbench_en_US.ts" -qm "${i18n_dir}/workbench_en_US.qm"
  COMMAND Qt6::lrelease "${i18n_dir}/workbench_ja_JP.ts" -qm "${i18n_dir}/workbench_ja_JP.qm"
  DEPENDS ${i18n_sources} "${native_root}/i18n/catalog.json" "${repo_root}/scripts/native_i18n.py"
  VERBATIM)
set_source_files_properties(${catalogs} PROPERTIES GENERATED TRUE)
add_library(gsw_recovery_languages OBJECT)
target_link_libraries(gsw_recovery_languages PRIVATE Qt6::Core)
qt_add_resources(gsw_recovery_languages "recovery_languages"
  PREFIX "/i18n" BASE "${i18n_dir}" FILES ${catalogs})
foreach(language zh_CN ja)
  find_file(recovery_qtbase_${language} NAMES "qtbase_${language}.qm"
    HINTS "${Qt6_DIR}/../../../share/qt6/translations" "${Qt6_DIR}/../../../translations" REQUIRED)
  set_source_files_properties("${recovery_qtbase_${language}}" PROPERTIES QT_RESOURCE_ALIAS "qtbase_${language}.qm")
  qt_add_resources(gsw_recovery_languages "recovery_qtbase_${language}"
    PREFIX "/i18n" FILES "${recovery_qtbase_${language}}")
endforeach()
add_library(gsw_recovery_shared STATIC
  ../src/NativeRecovery.cpp ../src/NativeRecovery.h
  ../src/AppLanguage.cpp ../src/AppLanguage.h
  ../src/NativeInstallation.cpp ../src/NativeInstallation.h
  ../src/NativeStartupHealth.cpp ../src/NativeStartupHealth.h)
target_include_directories(gsw_recovery_shared PUBLIC ../src)
target_link_libraries(gsw_recovery_shared PUBLIC Qt6::Core Qt6::Widgets)
if(WIN32)
  target_link_libraries(gsw_recovery_shared PRIVATE Shell32 Ole32 Propsys User32 Uuid bcrypt advapi32)
endif()
qt_add_executable(gsw_native_recovery WIN32 ../src/NativeRecoveryMain.cpp)
target_link_libraries(gsw_native_recovery PRIVATE gsw_recovery_shared gsw_recovery_languages)
if(GSW_UPDATER_FIXTURE_BUILD)
  target_compile_definitions(gsw_recovery_shared PRIVATE GSW_UPDATER_FIXTURE_BUILD=1)
  target_compile_definitions(gsw_native_recovery PRIVATE GSW_UPDATER_FIXTURE_BUILD=1
    GSW_APP_USER_MODEL_ID="Fubukisama.GaussianSceneWorkbench.UpdateFixture.Recovery")
  set_target_properties(gsw_native_recovery PROPERTIES OUTPUT_NAME "GSW Update Fixture Recovery")
else()
  target_compile_definitions(gsw_native_recovery PRIVATE
    GSW_APP_USER_MODEL_ID="Fubukisama.GaussianSceneWorkbench.Native.Recovery")
  set_target_properties(gsw_native_recovery PROPERTIES OUTPUT_NAME "Gaussian Scene Workbench Recovery")
endif()
if(WIN32)
  target_sources(gsw_native_recovery PRIVATE ../assets/gsw-app-icon.rc)
endif()
foreach(target gsw_recovery_shared gsw_native_recovery)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endif()
endforeach()
if(GSW_RECOVERY_UI_TESTS)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  qt_add_executable(gsw_native_recovery_ui_tests ../tests/NativeRecoveryUiTests.cpp)
  target_link_libraries(gsw_native_recovery_ui_tests PRIVATE
    gsw_recovery_shared gsw_recovery_languages Qt6::Test Qt6::Widgets)
  target_compile_definitions(gsw_native_recovery_ui_tests PRIVATE
    GSW_RECOVERY_QT_TEST=1 GSW_UPDATER_FIXTURE_BUILD=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(gsw_native_recovery_ui_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endif()
endif()
