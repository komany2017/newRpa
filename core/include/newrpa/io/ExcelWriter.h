#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// ExcelWriter — 写交易流水到 Excel/CSV
//
// 默认 CSV （不依赖任何外部库），设置 USE_LIBXL 宏时走 libxl。
// ============================================================================
#include "newrpa/plugin/IBankPlugin.h"
#include <string>
#include <vector>

namespace newrpa {
namespace io {

NEWRPA_API std::string WriteTransactionsCsv(
    const std::string& path,
    const std::vector<plugin::Transaction>& rows);

NEWRPA_API std::string WriteCurBalCsv(
    const std::string& path,
    const std::vector<std::map<std::string, std::string>>& rows);

} // namespace io
} // namespace newrpa
