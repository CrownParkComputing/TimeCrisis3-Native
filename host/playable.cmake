# Included after the pinned upstream PCSX2 target is defined.
set(TC3_FRONTEND_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_executable(tc3-native "${TC3_FRONTEND_DIR}/playable.cpp")
target_compile_features(tc3-native PRIVATE cxx_std_20)
target_include_directories(tc3-native PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/pcsx2" "${CMAKE_BINARY_DIR}/common/include" "${TC3_FRONTEND_DIR}")
target_link_libraries(tc3-native PRIVATE PCSX2_FLAGS PCSX2)
if(EXISTS "${TC3_FRONTEND_DIR}/game_menu.cpp")
  target_sources(tc3-native PRIVATE "${TC3_FRONTEND_DIR}/game_menu.cpp")
  target_include_directories(tc3-native PRIVATE "${TC3_FRONTEND_DIR}/../third_party/nuklear")
  target_compile_definitions(tc3-native PRIVATE TC3_GAME_MENU=1)
endif()
add_custom_command(TARGET tc3-native POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/bin/resources" "$<TARGET_FILE_DIR:tc3-native>/resources")

# The frontend catches its own configuration/lifecycle errors. Upstream core
# remains built with its original no-exceptions flags and PCH configuration.
set_source_files_properties("${TC3_FRONTEND_DIR}/playable.cpp" PROPERTIES
  SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fexceptions")

# Specialize the patch loader for this product without changing the reference.
get_target_property(TC3_CORE_SOURCES PCSX2 SOURCES)
list(FILTER TC3_CORE_SOURCES EXCLUDE REGEX "(^|/)Patch\\.cpp$")
set_property(TARGET PCSX2 PROPERTY SOURCES "${TC3_CORE_SOURCES}")
file(READ "${CMAKE_SOURCE_DIR}/pcsx2/Patch.cpp" TC3_PATCH_SOURCE)
set(TC3_PATCH_ENTRY "\tbool verbose_if_changed)\n{\n")
string(FIND "${TC3_PATCH_SOURCE}" "${TC3_PATCH_ENTRY}" TC3_PATCH_ENTRY_POS)
if(TC3_PATCH_ENTRY_POS LESS 0)
  message(FATAL_ERROR "Patch-loader entry changed; review TC3 no-patch policy")
endif()
string(REPLACE "${TC3_PATCH_ENTRY}"
  "${TC3_PATCH_ENTRY}\tUnloadPatches();\n\treturn; // TC3 does not load game patches or cheats.\n"
  TC3_PATCH_SOURCE "${TC3_PATCH_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tc3_no_patches.cpp" "${TC3_PATCH_SOURCE}")
target_sources(PCSX2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tc3_no_patches.cpp")

# Upstream uses one fixed POSIX name for a short-lived GS mapping. Simultaneous
# cabinet startup can collide before shm_unlink. Give each core its own name.
get_target_property(TC3_CORE_SOURCES PCSX2 SOURCES)
list(FILTER TC3_CORE_SOURCES EXCLUDE REGEX "(^|/)GS/GS\\.cpp$")
set_property(TARGET PCSX2 PROPERTY SOURCES "${TC3_CORE_SOURCES}")
file(READ "${CMAKE_SOURCE_DIR}/pcsx2/GS/GS.cpp" TC3_GS_SOURCE)
set(TC3_GS_MAPPING "\tconst char* file_name = \"/GS.mem\";")
string(FIND "${TC3_GS_SOURCE}" "${TC3_GS_MAPPING}" TC3_GS_MAPPING_POS)
if(TC3_GS_MAPPING_POS LESS 0)
  message(FATAL_ERROR "GS mapping allocator changed; review cabinet isolation")
endif()
string(REPLACE "${TC3_GS_MAPPING}"
  "\tconst std::string mapping_name = HostSys::GetFileMappingName(\"TC3.GS.mem\");\n\tconst char* file_name = mapping_name.c_str();"
  TC3_GS_SOURCE "${TC3_GS_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tc3_gs.cpp"
  "#include \"common/HostSys.h\"\n${TC3_GS_SOURCE}")
target_sources(PCSX2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tc3_gs.cpp")

# Reprocess a retained clean image while paused; the stock idle path only
# re-presents its last processed frame and omits CAS entirely.
get_target_property(TC3_CORE_SOURCES PCSX2 SOURCES)
list(FILTER TC3_CORE_SOURCES EXCLUDE REGEX "(^|/)GS/Renderers/Common/(GSRenderer|GSDevice)\\.cpp$")
set_property(TARGET PCSX2 PROPERTY SOURCES "${TC3_CORE_SOURCES}")
file(READ "${CMAKE_SOURCE_DIR}/pcsx2/GS/Renderers/Common/GSRenderer.cpp" TC3_RENDERER_SOURCE)
file(READ "${TC3_FRONTEND_DIR}/../rendering/present_paused.inc" TC3_PAUSED_PRESENT)
set(TC3_PRESENT_START "void GSRenderer::PresentCurrentFrame()")
set(TC3_PRESENT_END "void GSTranslateWindowToDisplayCoordinates(")
string(FIND "${TC3_RENDERER_SOURCE}" "${TC3_PRESENT_START}" TC3_PRESENT_START_POS)
string(FIND "${TC3_RENDERER_SOURCE}" "${TC3_PRESENT_END}" TC3_PRESENT_END_POS)
if(TC3_PRESENT_START_POS LESS 0 OR TC3_PRESENT_END_POS LESS TC3_PRESENT_START_POS)
  message(FATAL_ERROR "Paused GS presentation boundary changed")
endif()
string(SUBSTRING "${TC3_RENDERER_SOURCE}" 0 ${TC3_PRESENT_START_POS} TC3_RENDERER_PREFIX)
string(SUBSTRING "${TC3_RENDERER_SOURCE}" ${TC3_PRESENT_END_POS} -1 TC3_RENDERER_SUFFIX)
set(TC3_RENDERER_SOURCE "${TC3_RENDERER_PREFIX}${TC3_PAUSED_PRESENT}\n${TC3_RENDERER_SUFFIX}")
# Inset the actual GS image and its authoritative pointer mapping together.
# This helper is used by active, paused and capture presentation paths.
set(TC3_BORDER_RETURN "\treturn ret;\n}\n\nstatic GSVector4i CalculateDrawSrcRect")
string(FIND "${TC3_RENDERER_SOURCE}" "${TC3_BORDER_RETURN}" TC3_BORDER_POS)
if(TC3_BORDER_POS LESS 0)
  message(FATAL_ERROR "GS display geometry changed; review lightgun border mapping")
endif()
string(REPLACE "${TC3_BORDER_RETURN}"
  "\treturn tc3::sinden::inset(ret);\n}\n\nstatic GSVector4i CalculateDrawSrcRect"
  TC3_RENDERER_SOURCE "${TC3_RENDERER_SOURCE}")
set(TC3_EFFECT_START "\tif (GSConfig.ShadeBoost)\n\t\tg_gs_device->ShadeBoost();")
string(FIND "${TC3_RENDERER_SOURCE}" "${TC3_EFFECT_START}" TC3_EFFECT_POS)
if(TC3_EFFECT_POS LESS 0)
  message(FATAL_ERROR "GS clean-image capture point changed")
endif()
string(REPLACE "${TC3_EFFECT_START}" "\ttc3::preview::Capture();\n\n${TC3_EFFECT_START}"
  TC3_RENDERER_SOURCE "${TC3_RENDERER_SOURCE}")
string(REPLACE "void GSRenderer::Reset(bool hardware_reset)\n{"
  "void GSRenderer::Reset(bool hardware_reset)\n{\n\ttc3::preview::Reset();"
  TC3_RENDERER_SOURCE "${TC3_RENDERER_SOURCE}")
set(TC3_PREVIEW_INCLUDE "#include \"${TC3_FRONTEND_DIR}/../rendering/paused_preview.h\"\n#include \"${TC3_FRONTEND_DIR}/../input/sinden.h\"\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tc3_renderer.cpp" "${TC3_PREVIEW_INCLUDE}${TC3_RENDERER_SOURCE}")
file(READ "${CMAKE_SOURCE_DIR}/pcsx2/GS/Renderers/Common/GSDevice.cpp" TC3_DEVICE_SOURCE)
set(TC3_DESTROY_ENTRY "void GSDevice::Destroy()\n{")
string(FIND "${TC3_DEVICE_SOURCE}" "${TC3_DESTROY_ENTRY}" TC3_DESTROY_POS)
if(TC3_DESTROY_POS LESS 0)
  message(FATAL_ERROR "GS preview resource destruction boundary changed")
endif()
string(REPLACE "${TC3_DESTROY_ENTRY}" "${TC3_DESTROY_ENTRY}\n\ttc3::preview::Reset();"
  TC3_DEVICE_SOURCE "${TC3_DEVICE_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tc3_device.cpp" "${TC3_PREVIEW_INCLUDE}${TC3_DEVICE_SOURCE}")
target_sources(PCSX2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tc3_renderer.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/tc3_device.cpp" "${TC3_FRONTEND_DIR}/../rendering/paused_preview.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${TC3_FRONTEND_DIR}/../rendering/present_paused.inc")

# The native cabinet adapter is dormant unless an explicit left/right role and
# local session directory are supplied. Keep the pinned oracle source intact.
if(EXISTS "${TC3_FRONTEND_DIR}/../runtime/cabinet/bridge.cpp")
  get_target_property(TC3_CORE_SOURCES PCSX2 SOURCES)
  list(FILTER TC3_CORE_SOURCES EXCLUDE REGEX "(^|/)IopBios\\.cpp$")
  set_property(TARGET PCSX2 PROPERTY SOURCES "${TC3_CORE_SOURCES}")
  file(READ "${CMAKE_SOURCE_DIR}/pcsx2/IopBios.cpp" TC3_IOP_SOURCE)
  set(TC3_REGISTER_ENTRY "\t\tint RegisterLibraryEntries_HLE()\n\t\t{\n")
  set(TC3_IMPORT_ENTRY "\tirxHLE irxImportHLE(const std::string& libname, u16 index)\n\t{\n")
  set(TC3_RELEASE_ENTRY "\t\tint ReleaseLibraryEntries_HLE()\n\t\t{\n")
  foreach(TC3_ENTRY IN ITEMS TC3_REGISTER_ENTRY TC3_IMPORT_ENTRY TC3_RELEASE_ENTRY)
    string(FIND "${TC3_IOP_SOURCE}" "${${TC3_ENTRY}}" TC3_ENTRY_POS)
    if(TC3_ENTRY_POS LESS 0)
      message(FATAL_ERROR "IOP adapter entry changed: ${TC3_ENTRY}")
    endif()
  endforeach()
  string(REPLACE "${TC3_REGISTER_ENTRY}"
    "${TC3_REGISTER_ENTRY}\t\t\ttc3CabinetRegisterExports(a0);\n" TC3_IOP_SOURCE "${TC3_IOP_SOURCE}")
  string(REPLACE "${TC3_RELEASE_ENTRY}"
    "${TC3_RELEASE_ENTRY}\t\t\ttc3CabinetReleaseExports(a0);\n" TC3_IOP_SOURCE "${TC3_IOP_SOURCE}")
  string(REPLACE "${TC3_IMPORT_ENTRY}"
    "${TC3_IMPORT_ENTRY}\t\tif (auto native = tc3CabinetImportHLE(libname, index)) return native;\n"
    TC3_IOP_SOURCE "${TC3_IOP_SOURCE}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tc3_iop_bios.cpp"
    "#include \"${TC3_FRONTEND_DIR}/../runtime/cabinet/bridge.h\"\n${TC3_IOP_SOURCE}")
  target_sources(PCSX2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tc3_iop_bios.cpp"
    "${TC3_FRONTEND_DIR}/../runtime/cabinet/bridge.cpp")
  set_source_files_properties("${TC3_FRONTEND_DIR}/../runtime/cabinet/bridge.cpp" TARGET_DIRECTORY PCSX2 PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fexceptions")
endif()
