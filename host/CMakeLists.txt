find_package(PkgConfig REQUIRED)
pkg_check_modules(TC3_SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_package(Vulkan REQUIRED)
find_program(TC3_GLSLANG glslangValidator REQUIRED)
foreach(stage vert frag)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/menu_${stage}.h"
    COMMAND "${TC3_GLSLANG}" -V --vn "tc3_${stage}" -o "${CMAKE_CURRENT_BINARY_DIR}/menu_${stage}.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/menu.${stage}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/menu.${stage}" VERBATIM)
endforeach()
add_executable(tc3-native-host main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/menu_vert.h" "${CMAKE_CURRENT_BINARY_DIR}/menu_frag.h")
target_compile_features(tc3-native-host PRIVATE cxx_std_20)
target_include_directories(tc3-native-host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(tc3-native-host SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nuklear")
target_link_libraries(tc3-native-host PRIVATE PkgConfig::TC3_SDL3 Vulkan::Vulkan)
target_compile_options(tc3-native-host PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

option(TC3_ENABLE_GPU_TESTS "Run display-dependent Vulkan host smoke tests" OFF)
if(BUILD_TESTING AND TC3_ENABLE_GPU_TESTS)
  add_test(NAME host_vulkan_smoke COMMAND tc3-native-host --smoke-frames 12 --smoke-resize --smoke-ui --validation)
  set_tests_properties(host_vulkan_smoke PROPERTIES TIMEOUT 40 LABELS "gpu;integration")
endif()
