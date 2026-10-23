find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(TC3_PCSX2_BUILD "${PROJECT_SOURCE_DIR}/.local/core-build" CACHE PATH "Configured pinned PCSX2 core build")
if(NOT EXISTS "${TC3_PCSX2_SOURCE}/pcsx2/R5900.h")
  message(FATAL_ERROR "Prepare the pinned PCSX2 source before building the native block proof")
endif()
if(NOT EXISTS "${TC3_PCSX2_BUILD}/compile_commands.json")
  message(FATAL_ERROR "Configure/build the pinned core with CMAKE_EXPORT_COMPILE_COMMANDS=ON first")
endif()
add_library(tc3_native_blocks STATIC native_block_provider.cpp)
target_include_directories(tc3_native_blocks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${TC3_PCSX2_SOURCE}")
target_compile_features(tc3_native_blocks PUBLIC cxx_std_20)
set(original_object "${CMAKE_CURRENT_BINARY_DIR}/original_scalar.o")
add_custom_command(OUTPUT "${original_object}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_original_scalar.py"
    --core-build "${TC3_PCSX2_BUILD}" --source "${TC3_PCSX2_SOURCE}" --output "${original_object}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/build_original_scalar.py"
    "${TC3_PCSX2_SOURCE}/pcsx2/R5900OpcodeImpl.cpp"
    "${TC3_PCSX2_BUILD}/compile_commands.json" VERBATIM)
set_source_files_properties("${original_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
add_executable(tc3-native-block-tests "${PROJECT_SOURCE_DIR}/tests/native_block_tests.cpp" "${original_object}")
target_link_libraries(tc3-native-block-tests PRIVATE tc3_native_blocks)
target_link_options(tc3-native-block-tests PRIVATE -Wl,--gc-sections)
add_test(NAME native-block-original-scalar-semantics COMMAND tc3-native-block-tests)
