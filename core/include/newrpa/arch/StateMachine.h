#pragma once
// ============================================================================
// StateMachine<State, Event> — 登录流程显式化
//
// 当前 LoginQuery() 里 "验证码→自动→失败→鸣笛→成功/超时" 是散落的
// 布尔判断。状态机把迁移关系变成一张表，并留下可打印的状态轨迹。
//
// State / Event 用 enum class（支持 operator< 与 static_cast<int>）。
// ============================================================================

#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace newrpa {
namespace arch {

enum class LoginState {
    Idle,
    CredentialsFilled,
    Captcha,
    AutoSolving,
    HumanRequired,
    LoggedIn,
    Timeout,
    Failed
};

enum class LoginEvent {
    FillDone,
    CaptchaFound,
    AutoSucceeded,
    AutoFailed,
    HumanDone,
    TimeUp,
    Error
};

template <class State, class Event>
class StateMachine {
public:
    using TransitionKey = std::pair<State, Event>;

    explicit StateMachine(State initial) : current_(initial) {
        trace_.push_back("init=" + Name(initial));
    }

    void Add(State from, Event ev, State to, std::string label = "") {
        table_[TransitionKey(from, ev)] = Transition{to, std::move(label)};
    }

    // 设置可读名字（如 "Idle" / "LoggedIn"），便于 Trace 输出
    void SetNameFn(std::function<std::string(State)> fn) { nameFn_ = std::move(fn); }

    bool Fire(Event ev) {
        auto it = table_.find(TransitionKey(current_, ev));
        if (it == table_.end()) return false;
        State prev = current_;
        current_ = it->second.to;
        trace_.push_back(Name(prev) + " --" + it->second.label + "--> " +
                         Name(current_));
        return true;
    }

    State current() const noexcept { return current_; }

    std::vector<std::string> Trace() const { return trace_; }

    std::string TraceJoined(const std::string& sep = "\n") const {
        std::string out;
        for (const auto& line : trace_) {
            if (!out.empty()) out += sep;
            out += line;
        }
        return out;
    }

    void ClearTrace() { trace_.clear(); }

private:
    struct Transition {
        State       to;
        std::string label;
    };

    std::string Name(State s) const {
        if (nameFn_) return nameFn_(s);
        if constexpr (std::is_enum_v<State>)
            return std::to_string(static_cast<int>(s));
        else
            return "state";
    }

    std::map<TransitionKey, Transition> table_;
    State                              current_;
    std::function<std::string(State)>  nameFn_;
    std::vector<std::string>           trace_;
};

// 预置登录状态机：把当前 LoginQuery 的分支固化成表
inline StateMachine<LoginState, LoginEvent> MakeLoginMachine() {
    using S = LoginState;
    using E = LoginEvent;
    StateMachine<S, E> m(S::Idle);
    m.SetNameFn([](S s) -> std::string {
        switch (s) {
        case S::Idle:               return "Idle";
        case S::CredentialsFilled:  return "CredentialsFilled";
        case S::Captcha:            return "Captcha";
        case S::AutoSolving:        return "AutoSolving";
        case S::HumanRequired:      return "HumanRequired";
        case S::LoggedIn:           return "LoggedIn";
        case S::Timeout:            return "Timeout";
        case S::Failed:             return "Failed";
        }
        return "?";
    });
    m.Add(S::Idle,              E::FillDone,       S::CredentialsFilled, "填完账号密码");
    m.Add(S::CredentialsFilled, E::CaptchaFound,   S::Captcha,           "检测到验证码");
    m.Add(S::CredentialsFilled, E::AutoSucceeded,  S::LoggedIn,          "直接进入首页");
    m.Add(S::Captcha,           E::AutoSucceeded,  S::LoggedIn,          "自动识别通过");
    m.Add(S::Captcha,           E::AutoFailed,     S::AutoSolving,       "转自动策略");
    m.Add(S::AutoSolving,       E::AutoSucceeded,  S::LoggedIn,          "自动通过");
    m.Add(S::AutoSolving,       E::AutoFailed,     S::HumanRequired,     "鸣笛转人工");
    m.Add(S::HumanRequired,     E::HumanDone,      S::LoggedIn,          "人工完成");
    m.Add(S::HumanRequired,     E::TimeUp,         S::Timeout,           "超5分钟");
    m.Add(S::HumanRequired,     E::Error,          S::Failed,            "人工介入失败");
    return m;
}

using LoginStateMachine = StateMachine<LoginState, LoginEvent>;

} // namespace arch
} // namespace newrpa
