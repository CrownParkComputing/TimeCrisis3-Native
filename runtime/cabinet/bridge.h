// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
using TC3CabinetNativeImport = int (*)();
// CPU thread only. Disabled unless both explicit experimental environment
// variables TC3_CABINET_ROLE and TC3_CABINET_DIR are supplied.
TC3CabinetNativeImport tc3CabinetImportHLE(const std::string& library,std::uint16_t index);
void tc3CabinetRegisterExports(std::uint32_t table);
void tc3CabinetReleaseExports(std::uint32_t table);
