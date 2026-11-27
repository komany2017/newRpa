#pragma once
// ============================================================================
// RAII 小工具
//
// 针对现状："OnEvent/OffEvent 要手动配对，异常路径容易漏掉，
// 曾因栈展开时 Chrome 被杀掉导致 F5 调试中断"。
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/cdp/CDPSession.h"

#include <chrono>
#include <functional>
#include <string>

namespace newrpa {
namespace arch {

// CDP 事件订阅：构造时注册，析构时自动反注册（含异常路径）
class ScopedEvent {
public:
    ScopedEvent(cdp::CDPSession* session, const std::string& eventName,
                cdp::EventCallback cb)
        : session_(session), event_(eventName),
          handle_(session ? session->OnEvent(eventName, std::move(cb)) : 0) {}

    ~ScopedEvent() {
        if (session_ && handle_ != 0)
            session_->OffEvent(event_, handle_);
    }

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

    int handle() const noexcept { return handle_; }

private:
    cdp::CDPSession* session_;
    std::string      event_;
    int              handle_;
};

// 作用域计时器：析构时把耗时交给回调（可写日志/上下文）
class ScopedTimer {
public:
    using Sink = std::function<void(const std::string&, long long)>;

    explicit ScopedTimer(std::string label, Sink sink = nullptr)
        : label_(std::move(label)), sink_(std::move(sink)),
          start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        if (!sink_) return;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start_)
                      .count();
        sink_(label_, ms);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    long long ElapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_)
            .count();
    }

private:
    std::string                         label_;
    Sink                                sink_;
    std::chrono::steady_clock::time_point start_;
};

// 值域守卫：保存旧值，析构时恢复（用于临时改标志位）
template <class T>
class ScopedValue {
public:
    ScopedValue(T& ref, T temporary) : ref_(ref), old_(ref) {
        ref_ = std::move(temporary);
    }
    ~ScopedValue() { ref_ = old_; }

    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;

private:
    T& ref_;
    T  old_;
};

} // namespace arch
} // namespace newrpa
