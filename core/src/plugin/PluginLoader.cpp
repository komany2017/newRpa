// ============================================================================
// PluginLoader 实现
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include <filesystem>
#include <sstream>
#include <mutex>

namespace newrpa {
namespace plugin {

PluginLoader::~PluginLoader() {
    std::lock_guard<std::mutex> lk(mtx_);
#if defined(_WIN32)
    for (auto& kv : entries_) {
        if (kv.second.handle) FreeLibrary((HMODULE)kv.second.handle);
    }
#else
    for (auto& kv : entries_) {
        if (kv.second.handle) dlclose(kv.second.handle);
    }
#endif
    entries_.clear();
}

void PluginLoader::ScanDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        spdlog::warn("[Plugin] Scan dir not found: {}", dir);
        return;
    }
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto& p = entry.path();
        bool isDll = p.extension() == ".dll";
        bool isSo  = p.extension() == ".so";
        if (!isDll && !isSo) continue;
        if (Load(p.string()))
            spdlog::info("[Plugin] Loaded: {}", p.filename().string());
    }
}

bool PluginLoader::Load(const std::string& dllPath) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(
        std::wstring(dllPath.begin(), dllPath.end()).c_str());
    if (!h) {
        spdlog::error("[Plugin] LoadLibrary failed: {}", dllPath);
        return false;
    }
    auto createFn = (pfn_create_plugin)
        GetProcAddress(h, "CreatePlugin");
    auto destroyFn = (pfn_destroy_plugin)
        GetProcAddress(h, "DestroyPlugin");
#else
    void* h = dlopen(dllPath.c_str(), RTLD_NOW);
    if (!h) {
        spdlog::error("[Plugin] dlopen failed: {}", dllPath);
        return false;
    }
    auto createFn = (pfn_create_plugin) dlsym(h, "CreatePlugin");
    auto destroyFn = (pfn_destroy_plugin) dlsym(h, "DestroyPlugin");
#endif
    if (!createFn || !destroyFn) {
        spdlog::error("[Plugin] Missing CreatePlugin/DestroyPlugin: {}", dllPath);
#if defined(_WIN32)
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        return false;
    }

    // 调一次 create + GetId 取插件 id
    auto* tmp = createFn();
    if (!tmp) {
        spdlog::error("[Plugin] CreatePlugin returned null: {}", dllPath);
#if defined(_WIN32)
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        return false;
    }
    std::string id = tmp->GetId();
    destroyFn(tmp);

    std::lock_guard<std::mutex> lk(mtx_);
    PluginEntry pe;
    pe.dllPath  = dllPath;
    pe.id       = id;
    pe.handle   = h;
    pe.create   = createFn;
    pe.destroy  = destroyFn;
    entries_[id] = std::move(pe);
    return true;
}

std::unique_ptr<IBankPlugin> PluginLoader::Create(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);

    // 1) 精确匹配插件 id（如 "newrpa.bcb"）
    auto it = entries_.find(id);
    if (it != entries_.end()) goto found;

    // 2) 短名匹配（config.json 里 bankName 是 "BCB"，插件 id 是 "newrpa.bcb"）
    for (auto& kv : entries_) {
        // 前缀匹配 "newrpa.BCB" / 大小写不敏感
        std::string lowerId = kv.first;
        std::string lowerKey = id;
        for (auto& c : lowerId) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : lowerKey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerId == lowerKey ||
            lowerId.size() > lowerKey.size() &&
                lowerId.compare(lowerId.size() - lowerKey.size(), lowerKey.size(), lowerKey) == 0) {
            it = entries_.find(kv.first);
            if (it != entries_.end()) goto found;
        }
    }
    return nullptr;

found:
    auto* raw = it->second.create();
    if (!raw) return nullptr;
    std::unique_ptr<IBankPlugin> ptr(raw);
    return ptr;
}

std::vector<std::string> PluginLoader::ListIds() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (auto& kv : entries_) ids.push_back(kv.first);
    return ids;
}

} // namespace plugin
} // namespace newrpa
