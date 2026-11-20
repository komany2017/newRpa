#pragma once

// NewRpa 插件 DLL 导入/导出宏
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef NEWRPA_BUILD
#    define NEWRPA_API __declspec(dllexport)
#  elif defined(NEWRPA_USE_DLL)
#    define NEWRPA_API __declspec(dllimport)
#  else
#    define NEWRPA_API
#  endif
#else
#  define NEWRPA_API __attribute__((visibility("default")))
#endif

// 插件 DLL 导出函数名（C 约定）
#ifdef __cplusplus
extern "C" {
#endif

// 每个银行插件必须导出这两个函数
using pfn_create_plugin  = struct IBankPlugin* (*)(void);
using pfn_destroy_plugin = void (*)(struct IBankPlugin*);

#ifdef __cplusplus
}
#endif
