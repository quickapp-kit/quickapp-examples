cmake_minimum_required(VERSION 3.24)
project(quickapp_examples LANGUAGES CXX)

set(QUICKAPP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-core" CACHE PATH "QuickApp Kit Core")
set(QUICKAPP_JS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-js" CACHE PATH "QuickApp Kit JS Runtime")

set(QUICKAPP_LVGL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUICKAPP_LVGL_BUILD_S02 ON CACHE BOOL "" FORCE)
set(QUICKAPP_LVGL_BUILD_S03 ON CACHE BOOL "" FORCE)
set(QUICKAPP_LVGL_BUILD_S04 ON CACHE BOOL "" FORCE)
set(QUICKAPP_LVGL_BUILD_S06 ON CACHE BOOL "" FORCE)
set(QUICKAPP_LVGL_BUILD_SIMULATOR ON CACHE BOOL "" FORCE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-lvgl" "${CMAKE_CURRENT_BINARY_DIR}/quickapp-runtime-lvgl" EXCLUDE_FROM_ALL)

add_executable(quickapp_case001_loader composition/case001_loader.cpp)
target_compile_features(quickapp_case001_loader PRIVATE cxx_std_20)
target_compile_options(quickapp_case001_loader PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(quickapp_case001_loader PRIVATE
  "${QUICKAPP_CORE_DIR}/include")
target_link_libraries(quickapp_case001_loader PRIVATE quickapp_core_package quickapp_core_foundation)

add_executable(quickapp_case001_js composition/case001_js.cpp)
target_compile_features(quickapp_case001_js PRIVATE cxx_std_20)
target_compile_options(quickapp_case001_js PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(quickapp_case001_js PRIVATE
  "${QUICKAPP_CORE_DIR}/include"
  "${QUICKAPP_JS_DIR}/include"
  "${QUICKAPP_JS_DIR}/providers/quickjs/include"
  "${QUICKAPP_JS_DIR}/include"
  "${QUICKAPP_JS_DIR}/providers/quickjs/include")
target_link_libraries(quickapp_case001_js PRIVATE
  quickapp_core_package quickapp_core_foundation
  quickapp_js_engine_quickjs quickapp_js_engine_api quickapp_js_executor
  quickapp_js_module_loader quickapp_js_static_facades)

add_executable(quickapp_case001_lvgl composition/case001_lvgl.cpp)
target_compile_features(quickapp_case001_lvgl PRIVATE cxx_std_20)
target_compile_options(quickapp_case001_lvgl PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(quickapp_case001_lvgl PRIVATE
  "${QUICKAPP_CORE_DIR}/include"
  "${QUICKAPP_JS_DIR}/include"
  "${QUICKAPP_JS_DIR}/providers/quickjs/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-lvgl/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../source/third_party/lvgl"
  "${CMAKE_CURRENT_SOURCE_DIR}/../source/third_party/lvgl/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-lvgl")
target_link_libraries(quickapp_case001_lvgl PRIVATE
  quickapp_core_package quickapp_core_render quickapp_core_features
  quickapp_js_engine_quickjs quickapp_js_engine_api quickapp_js_executor
  quickapp_js_module_loader quickapp_js_static_facades quickapp_js_page_host_control
  quickapp_js_vm_lifecycle quickapp_js_alpha_initial_binding quickapp_js_alpha_initial_render
  quickapp_js_event
  quickapp_js_alpha_page_stage
  quickapp_lvgl_mount_host quickapp_lvgl_feature_provider quickapp_lvgl_font_measure
  quickapp_lvgl_sdl_backends lvgl::lvgl PkgConfig::SDL3)
target_compile_definitions(quickapp_case001_lvgl PRIVATE CONFIG_LV_USE_SDL=1)

add_executable(quickapp_lvgl_simulator composition/case001_lvgl.cpp)
target_compile_features(quickapp_lvgl_simulator PRIVATE cxx_std_20)
target_compile_options(quickapp_lvgl_simulator PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(quickapp_lvgl_simulator PRIVATE
  "${QUICKAPP_CORE_DIR}/include"
  "${QUICKAPP_JS_DIR}/include"
  "${QUICKAPP_JS_DIR}/providers/quickjs/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-lvgl/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../source/third_party/lvgl"
  "${CMAKE_CURRENT_SOURCE_DIR}/../source/third_party/lvgl/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../quickapp-runtime-lvgl")
target_link_libraries(quickapp_lvgl_simulator PRIVATE
  quickapp_core_package quickapp_core_render quickapp_core_features
  quickapp_js_engine_quickjs quickapp_js_engine_api quickapp_js_executor
  quickapp_js_module_loader quickapp_js_static_facades quickapp_js_page_host_control
  quickapp_js_vm_lifecycle quickapp_js_alpha_initial_binding quickapp_js_alpha_initial_render
  quickapp_js_event
  quickapp_js_alpha_page_stage
  quickapp_lvgl_mount_host quickapp_lvgl_feature_provider quickapp_lvgl_font_measure
  quickapp_lvgl_sdl_backends lvgl::lvgl PkgConfig::SDL3)
target_compile_definitions(quickapp_lvgl_simulator PRIVATE
  CONFIG_LV_USE_SDL=1
  CONFIG_LV_SDL_DIRECT_EXIT=0
  QUICKAPP_EXAMPLES_INTERACTIVE_SIMULATOR=1)
