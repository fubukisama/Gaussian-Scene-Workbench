# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(GswNativeUpdateUiAcceptance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Test LinguistTools)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(CTest)

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
add_library(gsw_update_ui_language_resources OBJECT)
set_target_properties(gsw_update_ui_language_resources PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(gsw_update_ui_language_resources PRIVATE Qt6::Core)
qt_add_resources(gsw_update_ui_language_resources "update_ui_languages"
  PREFIX "/i18n" BASE "${i18n_dir}" FILES ${catalogs})
foreach(language zh_CN ja)
  find_file(qtbase_${language} NAMES "qtbase_${language}.qm"
    HINTS "${Qt6_DIR}/../../../share/qt6/translations" "${Qt6_DIR}/../../../translations" REQUIRED)
  set_source_files_properties("${qtbase_${language}}" PROPERTIES QT_RESOURCE_ALIAS "qtbase_${language}.qm")
  qt_add_resources(gsw_update_ui_language_resources "update_ui_qtbase_${language}"
    PREFIX "/i18n" FILES "${qtbase_${language}}")
endforeach()

add_library(gsw_update_ui_shared STATIC "${native_root}/src/AppLanguage.cpp"
  "${native_root}/src/AppLanguage.h" "${native_root}/src/NativeInstallation.cpp"
  "${native_root}/src/NativeInstallation.h" "${native_root}/src/NativeStartupHealth.cpp"
  "${native_root}/src/NativeStartupHealth.h")
target_include_directories(gsw_update_ui_shared PUBLIC "${native_root}/src")
target_link_libraries(gsw_update_ui_shared PUBLIC Qt6::Core Qt6::Widgets)
if(WIN32)
  target_link_libraries(gsw_update_ui_shared PRIVATE Shell32 Ole32 Propsys User32 Uuid bcrypt advapi32)
endif()
# Both variants are compiled. Only the fixture library may honor the explicit
# fixture-application locator; the production library has no such definition.
add_library(gsw_update_ui_production STATIC "${native_root}/src/NativeUpdateUi.cpp")
add_library(gsw_update_ui_fixture STATIC "${native_root}/src/NativeUpdateUi.cpp")
target_compile_definitions(gsw_update_ui_fixture PRIVATE GSW_UPDATER_UI_FIXTURE_BUILD=1)
foreach(target gsw_update_ui_production gsw_update_ui_fixture)
  target_link_libraries(${target} PUBLIC gsw_update_ui_shared Qt6::Widgets)
endforeach()

qt_add_executable(gsw_update_ui_helper_fixture "${native_root}/tests/NativeUpdateUiHelperFixture.cpp")
target_link_libraries(gsw_update_ui_helper_fixture PRIVATE Qt6::Core)
set_target_properties(gsw_update_ui_helper_fixture PROPERTIES OUTPUT_NAME "GSW Update UI Helper Fixture")
qt_add_executable(gsw_update_ui_tests "${native_root}/tests/NativeUpdateUiTests.cpp")
target_link_libraries(gsw_update_ui_tests PRIVATE gsw_update_ui_fixture
  gsw_update_ui_language_resources Qt6::Test Qt6::Widgets)
add_dependencies(gsw_update_ui_tests gsw_update_ui_helper_fixture gsw_update_ui_production)
foreach(target gsw_update_ui_shared gsw_update_ui_production gsw_update_ui_fixture
    gsw_update_ui_helper_fixture gsw_update_ui_tests)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endif()
endforeach()
add_test(NAME update_ui_public_boundary COMMAND gsw_update_ui_tests -o -,txt)
set_tests_properties(update_ui_public_boundary PROPERTIES TIMEOUT 120
  ENVIRONMENT "GSW_UPDATE_UI_HELPER_FIXTURE=$<TARGET_FILE:gsw_update_ui_helper_fixture>;GSW_UPDATE_UI_EVIDENCE=${CMAKE_CURRENT_BINARY_DIR}/ui-evidence;QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1"
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
