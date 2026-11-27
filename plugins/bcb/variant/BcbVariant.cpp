// P4：查询版/演示版差异实现，路由、tag、回单模式与旧代码逐字一致。
#include "BcbVariant.h"
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace newrpa {
namespace bcb {
namespace variant {

// ---------------------------------------------------------------------------
// 查询版：origin 不带端口；日期必须 YYYYMMDD；回单默认逐条下载
// ---------------------------------------------------------------------------
std::vector<std::string>
BcbQueryVariant::ExportBalance(strategy::IBcbAutomationHost& h,
                               const std::string& today) {
    return h.HostDoQueryExport("AM/AM1/TAM1A01", today, today, "01_余额", false);
}

std::vector<std::string>
BcbQueryVariant::ExportTransactions(strategy::IBcbAutomationHost& h,
                                    const plugin::DateRange& range,
                                    const std::string& tag) {
    return h.HostDoQueryExport("AM/AM2/TAM2A01", range.begin, range.end, tag, false);
}

std::vector<std::string>
BcbQueryVariant::ExportReceipts(strategy::IBcbAutomationHost& h,
                                const plugin::DateRange& range) {
    // one=逐条（PDF 与 data.json 行一一对应）；batch=整批 ZIP
    std::string mode = "one";
    if (const char* e = std::getenv("BCB_SLIP_MODE")) mode = e;
    if (mode != "batch") {
        auto saved = h.HostDoQueryExportPerRow(
            "AM/AM3/NAM3001", range.begin, range.end, "04_电子回单");
        if (!saved.empty()) return saved;
        spdlog::warn("[BCB] 逐条下载未取得文件，回退批量下载");
    }
    return h.HostDoQueryExport("AM/AM3/NAM3001", range.begin, range.end,
                               "04_电子回单", true);
}

// ---------------------------------------------------------------------------
// 演示版：:9081 路由直连 + 原生下载
// ---------------------------------------------------------------------------
std::vector<std::string>
BcbDemoVariant::ExportBalance(strategy::IBcbAutomationHost& h,
                              const std::string&) {
    return h.HostDoExport("AM/AM1/TAM1A01", "01_余额", false);
}

std::vector<std::string>
BcbDemoVariant::ExportTransactions(strategy::IBcbAutomationHost& h,
                                   const plugin::DateRange&,
                                   const std::string& tag) {
    return h.HostDoExport("AM/AM2/TAM2A01", tag, false);
}

std::vector<std::string>
BcbDemoVariant::ExportReceipts(strategy::IBcbAutomationHost& h,
                               const plugin::DateRange&) {
    return h.HostDoExport("AM/AM3/NAM3001", "04_电子回单", true);
}

std::unique_ptr<BcbVariant> MakeVariantForLoginUrl(const std::string& loginUrl) {
    if (loginUrl.find("version=query") != std::string::npos)
        return std::unique_ptr<BcbVariant>(new BcbQueryVariant());
    return std::unique_ptr<BcbVariant>(new BcbDemoVariant());
}

} // namespace variant
} // namespace bcb
} // namespace newrpa
