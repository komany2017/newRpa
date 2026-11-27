#pragma once
// ============================================================================
// BankPluginBase — 模板方法基类
//
// 现状：每个银行插件自己重写 DownBankSlip，流程顺序散落且各不相同。
// 目标：基类用 final 固定 "准备 → 抓行 → 逐条下载 → 绑定落盘" 骨架，
//       银行差异只在三个钩子里实现。
//
// 迁移方式：让 BCBPlugin 从 IBankPlugin 改为继承 BankPluginBase，
//           把现有 DumpVueRowsJson / DoQueryExportPerRow 包进钩子即可。
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/arch/Strategy.h"
#include "newrpa/plugin/IBankPlugin.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace newrpa {
namespace arch {

struct ExportContext {
    plugin::DateRange range;
    std::string       tag;     // 输出文件前缀，如 "04_电子回单"
    std::string       workDir; // 输出目录（调用方保证存在）
};

class BankPluginBase : public plugin::IBankPlugin {
public:
    // final：流程顺序对所有银行一致；差异只能通过钩子表达
    Result<std::vector<std::string>>
    DownBankSlip(const plugin::DateRange& range) final {
        ExportContext ec;
        ec.range = range;
        ec.tag = "receipt";
        ec.workDir = "downloads";

        if (!Prepare(ec))
            return Result<std::vector<std::string>>::Fail(
                "bank.prepare_failed", "回单导出准备失败");

        auto rows = CaptureRows(ec);
        if (rows.empty())
            return Result<std::vector<std::string>>::Ok(
                std::vector<std::string>{});

        auto files = DownloadReceipts(ec, rows);
        return Result<std::vector<std::string>>::Ok(
            BindAndSave(ec, rows, files));
    }

protected:
    // 钩子①：导航到页面、设日期、确认查询条件
    virtual bool Prepare(ExportContext& ec) = 0;

    // 钩子②：抓流水行（流水号/金额/账号/日期）
    virtual std::vector<plugin::Transaction> CaptureRows(ExportContext& ec) = 0;

    // 钩子③：逐条勾选→生成→下载，返回文件及对应流水号
    virtual std::vector<DownloadedFile>
    DownloadReceipts(ExportContext& ec,
                     const std::vector<plugin::Transaction>& rows) = 0;

    // 默认绑定：先按 sn 精确匹配，再按顺序兜底；输出可审计的绑定清单。
    // 生产环境可在子类里改为调用 SlipBundle（支持 ZIP / UTF-16BE CID 解码）。
    virtual std::vector<std::string>
    BindAndSave(ExportContext& ec,
                const std::vector<plugin::Transaction>& rows,
                const std::vector<DownloadedFile>& files) {
        using json = nlohmann::json;

        std::vector<bool> used(files.size(), false);
        json manifest = json::array();

        // 第一轮：按流水号精确匹配
        for (size_t i = 0; i < rows.size(); ++i) {
            const std::string& sn = rows[i].sn;
            for (size_t j = 0; j < files.size(); ++j) {
                if (used[j] || files[j].sn.empty() || files[j].sn != sn)
                    continue;
                used[j] = true;
                json item;
                item["row"] = i + 1;
                item["sn"] = sn;
                item["amount"] = rows[i].amount;
                item["account"] = rows[i].account;
                item["pdf"] = files[j].path;
                manifest.push_back(std::move(item));
                break;
            }
        }

        // 第二轮：剩余文件按顺序兜底（应对银行不返回流水号的场景）
        size_t next = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            bool matched = false;
            for (const auto& m : manifest)
                if (m.value("row", 0) == static_cast<int>(i + 1))
                    matched = true;
            if (matched) continue;
            while (next < files.size() && used[next]) ++next;
            if (next >= files.size()) break;
            used[next] = true;
            json item;
            item["row"] = i + 1;
            item["sn"] = rows[i].sn;
            item["amount"] = rows[i].amount;
            item["account"] = rows[i].account;
            item["pdf"] = files[next].path;
            manifest.push_back(std::move(item));
            ++next;
        }

        std::string manifestPath = ec.workDir + "/" + ec.tag + "_bindings.json";
        std::ofstream ofs(manifestPath, std::ios::binary);
        if (ofs) ofs << manifest.dump(2);

        std::vector<std::string> out;
        out.reserve(manifest.size());
        for (const auto& m : manifest)
            out.push_back(m.value("pdf", ""));
        return out;
    }
};

} // namespace arch
} // namespace newrpa
