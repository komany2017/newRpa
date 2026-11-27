#pragma once
// ============================================================================
// Result<T> — C++17 轻量 Either，统一新架构的错误通道
//
// 设计目标：取代工程里混用的 "" / false / 空 vector 三种隐式失败信号。
// 失败必须带着 code（机器可读）和 message（人可读），并标记是否可重试。
//
// 用法：
//   auto r = Login();
//   if (!r) { spdlog::error("{}: {}", r.error().code, r.error().message); }
//   auto rows = r.value();
//
// header-only，无第三方依赖，不改变现有 DLL 导出符号。
// ============================================================================

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace newrpa {
namespace arch {

// 统一错误对象
struct Error {
    std::string code;          // 机器可读，如 "captcha.timeout" / "cdp.timeout"
    std::string message;       // 人可读中文说明
    bool        retryable = false; // 责任链/重试装饰器据此决定是否重试

    Error() = default;
    Error(std::string c, std::string m, bool r = false)
        : code(std::move(c)), message(std::move(m)), retryable(r) {}

    static Error Retryable(std::string c, std::string m) {
        return Error(std::move(c), std::move(m), true);
    }
};

// void 用占位类型（C++17 没有 std::expected）
struct Void {};

// 访问失败结果时抛出
class BadResultAccess : public std::runtime_error {
public:
    BadResultAccess()
        : std::runtime_error("bad Result access: no value available") {}
};

template <class T>
class Result {
public:
    using value_type = T;

    // 默认构造 = 失败态（便于作为成员/返回值占位）
    Result() = default;

    static Result Ok(T value) {
        Result r;
        r.ok_ = true;
        r.value_.emplace(std::move(value));
        return r;
    }

    static Result Fail(Error e) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(e);
        return r;
    }

    static Result Fail(std::string code, std::string message,
                       bool retryable = false) {
        return Fail(Error(std::move(code), std::move(message), retryable));
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    T& value() {
        if (!ok_) throw BadResultAccess();
        return *value_;
    }
    const T& value() const {
        if (!ok_) throw BadResultAccess();
        return *value_;
    }

    // 失败时返回默认值，不抛异常
    T value_or(T fallback) const {
        return ok_ ? *value_ : std::move(fallback);
    }

    const Error& error() const noexcept { return error_; }

private:
    bool                ok_ = false;
    std::optional<T>    value_;
    Error               error_;
};

using ResultVoid = Result<Void>;

inline ResultVoid MakeOk() { return ResultVoid::Ok(Void{}); }

inline ResultVoid MakeFail(std::string code, std::string message,
                           bool retryable = false) {
    return ResultVoid::Fail(std::move(code), std::move(message), retryable);
}

// 把一个可能抛异常的函数包装成 Result，异常翻译成 Error
template <class F>
auto CatchToResult(F&& f) -> Result<decltype(f())> {
    using R = decltype(f());
    try {
        return R::Ok(f());
    } catch (const std::exception& e) {
        return R::Fail(Error("exception", e.what(), false));
    } catch (...) {
        return R::Fail(Error("exception", "unknown exception", false));
    }
}

} // namespace arch
} // namespace newrpa
