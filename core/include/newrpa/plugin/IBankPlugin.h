#pragma once
// ============================================================================
// IBankPlugin — 银行插件统一接口
// PluginLoader — 动态加载插件 DLL
//
// 每个银行一个 DLL，导出 CreatePlugin / DestroyPlugin 两个 C 函数。
// DLL 里实现 IBankPlugin 的一个子类。
// ============================================================================

#include "newrpa/NewRpaExport.h"
#include "newrpa/arch/Result.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/chrome/ChromeBrowser.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <ctime>

namespace newrpa {
// ----------------------------------------------------------------------------
// Result 风格的登录结果 / 错误码（P5 统一接口；原 PluginEx.h 已删除）
// ----------------------------------------------------------------------------
namespace arch {
struct LoginOutcome {
    std::string raw;              // "ok" / "already logged in" / 原始错误信息
    bool        loggedIn = false; // 是否为成功状态
};
namespace BankErr {
inline const char* LoginFailed   = "bank.login_failed";
inline const char* LoginNoCap    = "bank.login_no_cap";
inline const char* RangeMissing  = "bank.range_missing";
inline const char* QueryFailed   = "bank.query_failed";
inline const char* SlipFailed    = "bank.slip_failed";
inline const char* NoPlugin      = "bank.no_plugin";
inline const char* UnknownAction = "command.unknown";
} // namespace BankErr
} // namespace arch

namespace plugin {

// 银行配置（从旧项目的 BankObject 简化过来）
struct NEWRPA_API BankConfig {
    std::string bankName;     // "ICBC" / "TZB" / ...
    std::string displayName;  // 工行 / 台州银行
    std::string loginUrl;
    std::string operatorId;   // 操作员号
    std::string account;      // 账号
    std::string password;     // 密码（可能加密存储）
    std::string certPassword; // 证书/UKey 密码
    std::string ukeyType;     // 0=无, 1= USB Key, 2=二代 Key
    std::string ukeyPort;
    std::string vid;          // 虚拟 ID（部分银行需要）
    std::string pid;          // 平台 ID
    std::string route;
    std::string dev;
    std::string section;      // 分行号
    std::string company;      // 企业名
    std::string routeType;
    std::string bankType;
    std::string note;         // 备注
    std::string chromeProfileDir; // 按银行覆盖 Chrome profile（空 = 用全局 chromeProfileDir）
    int         chromePort = 0;   // 按银行覆盖 CDP 端口（0 = 用全局 chromePort）
};

// 单笔流水/交易记录（和旧项目 Huidan 对应）
struct NEWRPA_API Transaction {
    std::string sn;           // 流水号
    std::string account;      // 本账号
    std::string opAccount;    // 对方账号
    std::string opAccountName;// 对方户名
    std::string amount;       // 金额
    std::string direction;    // "收"/"付"
    std::string transDate;    // 交易日期 YYYY-MM-DD HH:MM:SS
    std::string balanceAfter; // 交易后余额
    std::string remark;       // 备注
    std::string currency;     // 币种 "CNY" / ...
};

// 下载回单的时间段
struct NEWRPA_API DateRange {
    std::string begin;  // YYYY-MM-DD
    std::string end;    // YYYY-MM-DD
};

// 插件主接口
class NEWRPA_API IBankPlugin {
public:
    virtual ~IBankPlugin() = default;

    // --- 基础 ---

    // 插件名，例 "newrpa.icbc"
    virtual const char* GetId() const = 0;
    // 显示名，例 "中国工商银行"
    virtual const char* GetDisplayName() const = 0;

    // 注入运行时依赖
    virtual void SetSession(std::shared_ptr<cdp::CDPSession> session,
                            std::shared_ptr<chrome::ChromeBrowser> browser) = 0;

    // 注入银行配置
    virtual void SetConfig(const BankConfig& cfg) = 0;

    // 探测能力（支持哪些银行功能），返回位图
    virtual uint32_t Capabilities() const = 0;
    //   位图常量
    static constexpr uint32_t CAP_LOGIN         = 1 << 0;
    static constexpr uint32_t CAP_CUR_BALANCE   = 1 << 1;
    static constexpr uint32_t CAP_CUR_TRANS     = 1 << 2;
    static constexpr uint32_t CAP_YES_TRANS      = 1 << 3;
    static constexpr uint32_t CAP_SLIP          = 1 << 4;
    static constexpr uint32_t CAP_PAYMENT       = 1 << 5;
    static constexpr uint32_t CAP_APPROVAL      = 1 << 6;
    static constexpr uint32_t CAP_QUERY         = 1 << 7;

    // --- 核心业务 ---

    // 登录网银：raw 保留原始字符串以维持日志文本，loggedIn 是结构化成功标记
    virtual arch::Result<arch::LoginOutcome> Login() = 0;

    // 查询当日余额。返回 {account, balance, currency, time}
    virtual arch::Result<std::vector<std::map<std::string, std::string>>>
    GetCurBal() = 0;

    // 查询当日交易流水
    virtual arch::Result<std::vector<Transaction>> GetCurTran() = 0;

    // 查询历史流水（指定时间段）
    virtual arch::Result<std::vector<Transaction>>
    GetHisTran(const DateRange& range) = 0;

    // 下载电子回单（返回下载的文件路径）
    virtual arch::Result<std::vector<std::string>>
    DownBankSlip(const DateRange& range) = 0;

    // 批量付款：amount 金额、sn 流水号、对方账号 paccno
    virtual arch::Result<std::string> Payment(
        const std::string& transAmount,
        const std::string& transNums,
        const std::string& paccno,
        const std::string& filename = "",
        const std::string& sn = "") = 0;

    // 审批（按流水号）
    virtual arch::Result<std::string> Approval(const std::string& no) = 0;

    // 查询交易状态
    virtual arch::Result<std::string> Query(const std::string& no) = 0;

    // 关闭，释放资源（析构也会调一次，幂等）
    virtual void Close() = 0;

    // 遍历所有账号跑完整流程（历史/当日/余额/回单，历史增量）。
    // 默认不支持；支持的插件（如 BCB）重写此方法。
    virtual arch::Result<std::string> RunAllAccounts() {
        return arch::Result<std::string>::Ok(
            std::string("all-accounts not supported by ") + GetId());
    }

    // 混合模式：CDP 登录后，改用纯 HTTP 直连接口下载数据。
    // 默认不支持；支持的插件（如 BCB）重写此方法。
    virtual arch::Result<std::string> HttpDirect() {
        return arch::Result<std::string>::Ok(
            std::string("http-direct not supported by ") + GetId());
    }

    // --- 工具 ---

    // 触发 UKey / 证书处理（部分银行需要）
    virtual arch::Result<bool> TriggerUKey() {
        return arch::Result<bool>::Ok(true);
    }
};

// 插件 DLL 导出约定
// 每个插件 DLL 必须有这两个 C 函数
extern "C" {
using pfn_create_plugin  = IBankPlugin* (*)(void);
using pfn_destroy_plugin = void (*)(IBankPlugin*);
}

// ----------------------------------------------------------------------------
// PluginLoader — 动态加载 DLL
// ----------------------------------------------------------------------------
class NEWRPA_API PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader();

    // 从目录扫描所有插件 DLL（默认 ./plugins）
    void ScanDir(const std::string& dir);

    // 直接加载一个 DLL
    bool Load(const std::string& dllPath);

    // 创建指定 id 的插件实例（所有权归调用方）
    std::unique_ptr<IBankPlugin> Create(const std::string& id);

    // 列出已加载插件 id
    std::vector<std::string> ListIds() const;

private:
    struct PluginEntry {
        std::string dllPath;
        std::string id;
        void* handle         = nullptr; // OS 级 DLL handle
        pfn_create_plugin    create  = nullptr;
        pfn_destroy_plugin   destroy = nullptr;
    };

    std::unordered_map<std::string, PluginEntry> entries_;
    mutable std::mutex mtx_;
};

} // namespace plugin
} // namespace newrpa
