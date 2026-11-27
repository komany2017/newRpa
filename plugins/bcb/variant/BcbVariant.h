#pragma once
// ============================================================================
// 北京银行查询版 / 演示版变体（P4）
//
// BCBPlugin 是模板方法宿主：登录前置检查、读取 data.json、CSV 落盘、回单索引
// 等公共步骤固定不变；四个版本差异点下沉到两个子类，业务方法不再解析 loginUrl。
// ============================================================================

#include "../strategy/BcbStrategy.h"
#include "newrpa/plugin/IBankPlugin.h"

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace bcb {
namespace variant {

class BcbVariant {
public:
    virtual ~BcbVariant() = default;
    virtual const char* name() const = 0;

    // 登录流程差异：查询版走账号+密码+顶象滑块
    virtual bool UseQueryLogin() const = 0;

    // 三类导出的差异步骤；公共的登录检查 / data.json 解析 / 落盘仍在宿主中
    virtual std::vector<std::string>
    ExportBalance(strategy::IBcbAutomationHost& h,
                  const std::string& today) = 0;

    virtual std::vector<std::string>
    ExportTransactions(strategy::IBcbAutomationHost& h,
                       const plugin::DateRange& range,
                       const std::string& tag) = 0;

    virtual std::vector<std::string>
    ExportReceipts(strategy::IBcbAutomationHost& h,
                   const plugin::DateRange& range) = 0;
};

class BcbQueryVariant : public BcbVariant {
public:
    const char* name() const override { return "query"; }
    bool UseQueryLogin() const override { return true; }
    std::vector<std::string> ExportBalance(strategy::IBcbAutomationHost& h,
                                           const std::string& today) override;
    std::vector<std::string>
    ExportTransactions(strategy::IBcbAutomationHost& h,
                       const plugin::DateRange& range,
                       const std::string& tag) override;
    std::vector<std::string>
    ExportReceipts(strategy::IBcbAutomationHost& h,
                   const plugin::DateRange& range) override;
};

class BcbDemoVariant : public BcbVariant {
public:
    const char* name() const override { return "demo"; }
    bool UseQueryLogin() const override { return false; }
    std::vector<std::string> ExportBalance(strategy::IBcbAutomationHost& h,
                                           const std::string& today) override;
    std::vector<std::string>
    ExportTransactions(strategy::IBcbAutomationHost& h,
                       const plugin::DateRange& range,
                       const std::string& tag) override;
    std::vector<std::string>
    ExportReceipts(strategy::IBcbAutomationHost& h,
                   const plugin::DateRange& range) override;
};

// 按 loginUrl 决策一次；空串视为演示版（与旧 Login 默认 URL 一致）
std::unique_ptr<BcbVariant> MakeVariantForLoginUrl(const std::string& loginUrl);

} // namespace variant
} // namespace bcb
} // namespace newrpa
