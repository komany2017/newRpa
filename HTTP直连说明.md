# 北京银行演示版：CDP 登录 + HTTP 直连

## 一、抓包结论（2026-11-21，实地抓取）

用 CDP `Network` 域记录了完整登录 + 余额/交易流程（58 条记录，见 `build-mingw/bin/sniff_result.json` 生成逻辑），结论：

**演示版没有真正的会话型业务 API。** 所有数据都是静态资源：

| 数据 | 来源 | 是否需登录 |
|---|---|---|
| 余额/交易表格 | webpack 打包进 JS 的静态 JSON（`NBP04010` 等交易码） | 否 |
| 当日交易流水 | `/cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz`（GB2312 XML） | **否** |
| 历史交易流水 | `/cdn/demoYSB/AM/AM2/AM2DLZHistory.dlz`（GB2312 XML） | **否** |

- 服务端响应头有 `ETag`/`Last-Modified`（nginx 静态文件），无 Set-Cookie 会话
- 抓到的 Cookie 只有 BIGipServer 负载均衡和 sensorsdata 统计，无登录态 token
- 实测无 Cookie 直接 GET 静态 dlz 文件 → HTTP 200，数据完整

> 含义：这个演示版的"登录"是前端校验（密码固定 111111），数据本身公开可读。因此 HTTP 直连在这里退化为"直接下载静态文件"。真实网银不会有这种设计——真网银的查询接口需要会话 + 签名，那才是 HTTP 直连真正有价值的地方。

## 二、已实现的混合模式

命令：

```bat
cd E:\NewRpa-Next\build-mingw\bin
newrpa.exe run --bank BCB --action http-direct
```

流程：
1. **登录走 RPA**：`Login()` 用 CDP 驱动有界面 Chrome 完成真实登录（导航→填证书密码→填登录密码→点登录→跳转首页）
2. **取数走 HTTP**：从页面 URL 提取 origin，用 `HttpDownloader`（WinHTTP + schannel）纯 HTTP GET 静态数据文件，不经过浏览器点击/下载
3. 落盘到 `downloads/HTTP_当日交易_YYYYMMDD_HHMMSS.dlz`、`HTTP_历史交易_*.dlz`

实测：CDP 登录约 7 秒，HTTP 取数 0.16 秒；当日/历史各 16 笔，GB2312 XML 解析正常。

## 三、通用会话模式 `http-session`（新增）

这是更接近真实网银用法的版本：**RPA 登录 → 抓 Cookie 持久化 → 带 Cookie 按配置取数**，与具体银行解耦。

```bat
:: 首次：CDP 登录 + 抓 Cookie + 取数（实测 11 秒）
newrpa.exe run --bank BCB --action http-session

:: 之后：复用已保存 Cookie，跳过登录（实测 1 秒）
newrpa.exe run --bank BCB --action http-session --reuse-cookie
```

会话文件：`state/cookies_BCB.json`（本次抓到 5 条 Cookie）；该目录已在 `.gitignore` 中排除，不会进版本库。

**配置驱动**：`config.json` 根级 `httpTasks` 是任务列表，支持任意 URL / method / body / 自定义 header，占位符 `{origin}` `{bank}` `{date}` `{timestamp}`：

```json
"httpTasks": [
  {
    "name": "当日交易",
    "url": "{origin}/cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz",
    "saveAs": "HTTP_当日交易_{timestamp}.dlz",
    "method": "GET"
  }
]
```

未配置 `httpTasks` 时自动回退到 BCB 演示站的两个 dlz。实测 `--reuse-cookie` 下 Cookie 命中 5/5，两个文件 HTTP 200、各 16 笔；用临时配置改任务名/占位符也验证通过（生成 `TEST_BCB_20261121.dlz`）。

Cookie 注入规则（RFC 6265）：`domain` 支持精确匹配和前导点后缀匹配（`.bankofbeijing.com.cn` 命中 `cbank.bankofbeijing.com.cn`），`path` 前缀匹配且校验 `/` 边界，`secure` Cookie 只发 https。

## 四、关键代码

| 文件 | 改动 |
|---|---|
| `core/include/newrpa/net/HttpDownloader.h` | 新增：WinHTTP 单头文件客户端，原生 HTTPS（schannel，绕开 OpenSSL 兼容问题）、忽略自签证书、自定义 Header、GET/POST/下载 |
| `core/include/newrpa/plugin/IBankPlugin.h` | 新增 `virtual std::string HttpDirect()` 默认实现（不影响 ICBC） |
| `plugins/bcb/BCBPlugin.cpp/.h` | 实现 `HttpDirect()`：CDP 登录 + HTTP 直连下载 + 统计笔数 |
| `plugins/bcb/CMakeLists.txt` | BCB 插件链接 `winhttp` |
| `app/main.cpp` | 新增 `http-direct`、`sniff`、`http-session` 三个 action 与 `--reuse-cookie` 参数 |
| `core/include/newrpa/net/CookieJar.h` | 新增：CDP Cookie 解析、JSON 持久化、按 domain/path/secure 自动匹配注入 |
| `core/include/newrpa/net/HttpSession.h` | 新增：配置驱动任务列表、占位符展开、带 Cookie 批量下载 |
| `config.json` / `config.example.json` | 新增 `httpTasks` 任务列表 |
| `app/CMakeLists.txt` | app 目标链接 `winhttp`（main.cpp 使用 HttpSession） |

## 五、为什么用 WinHTTP 而不是修 HttpClient

- 原 `HttpClient` 用 httplib 且强制 `http://` 前缀，无法访问本项目需要的 HTTPS 站点
- 本环境 OpenSSL 3 对该站 TLS 握手失败（`SSL connection failed`），而 curl 用 Windows schannel 成功
- WinHTTP 正是基于 schannel，与 curl/Chrome 一致，且系统自带、零外部依赖

## 六、迁移到真实网银时的注意点

演示版的 HTTP 直连价值有限（数据公开）。真实网银要这么做，通常还需要：
1. **会话**：已由 `http-session` + `CookieJar` 完成（CDP 抓 Cookie → 持久化 → 自动注入）；真实网银还需额外处理 token 刷新与失效重登
2. **签名**：逆向前端 JS 的报文签名（常见国密 SM2/SM4 或 RSA），用 C++ 复刻
3. **加密**：部分银行请求体整体加密
4. **防爬**：设备指纹、动态 token、请求时序校验

届时的正确姿势仍是本项目的混合架构：**CDP 登录建会话 + 复用会话凭证做 HTTP 请求**，只是请求构造从"拼静态 URL"变成"构造带签名的业务报文"。

## 七、VSCode F5 调试配置

`.vscode/launch.json` 新增 4 个 HTTP 相关配置（F5 → 下拉框选择）：

| 配置名 | 用途 |
|---|---|
| ★ BCB HTTP 会话取数 (登录+抓Cookie+取数) | 完整路径：CDP 登录 + 抓新 Cookie + 按 httpTasks 取数 |
| BCB HTTP 会话取数 (复用Cookie, 跳过登录) | 用 `state/cookies_BCB.json`，最快（约 1 秒） |
| BCB HTTP 直连 (http-direct) | 只下载静态 dlz，不抓 Cookie |
| BCB 抓包 (sniff → sniff_result.json) | 旁路记录 XHR/Cookie，摸清接口用 |

断点建议：

- `app/main.cpp` 的 `http-session` 分支（抓 Cookie、读 httpTasks、注入）
- `core/include/newrpa/net/CookieJar.h` 的 `HeaderFor`（domain/path/secure 匹配）
- `core/include/newrpa/net/HttpSession.h` 的 `RunTasks` / `Do`（请求构造与落盘）
- `core/include/newrpa/net/HttpDownloader.h` 的 `Do`（WinHTTP 收发）

「★ 通用 (选银行+action)」的 action 下拉已加入 `http-session` / `http-direct` / `sniff`。所有配置保留 `symbolLoadInfo.loadAll=true` 和已修复的 gdb Python 环境（PATH 前置 `mingw64/opt/bin`、`PYTHONHOME`、清空 `PYTHONPATH`），`std::string` 可直接显示值。`preLaunchTask` 为 `Build + Copy DLLs`，F5 会自动编译并同步 DLL。

## 八、中文文件名乱码修复

现象：`downloads/` 下生成的 `01_余额_data.json` 等显示为 `01_浣欓_data.json`，但文件内容正常。

根因：MinGW 下 `std::ofstream(std::string)` 走 ANSI 代码页，把源码里的 UTF-8 中文路径按 GBK 解读，产生二次编码；而传 `std::filesystem::path` 会走 Unicode API（`_wfopen`），文件名正确。这也是 `HTTP_当日交易_*.dlz` 一直正常、只有 C++ 插件写出的 json 乱码的原因。

修复（统一把文件流参数改为 `fs::path`）：
- `plugins/bcb/BCBPlugin.cpp`：`WriteBinaryFile` / `LoadLastHisEnd` / `GetCurBal` / `GetHisTran` 的读写
- `core/include/newrpa/net/CookieJar.h`：`Save()` / `Load()`
- `core/include/newrpa/net/HttpDownloader.h`：`DownloadToFile()` 用 `ofstream(fs::path)` 替代 `fopen_s`
- `app/main.cpp`：`http-session` 读配置

验证：重编同步 DLL 后依次跑 `curbal` / `curtran` / `histran`，生成 `01_余额_data.json`、`02_当日交易_data.json`、`03_历史交易_data.json`，中文全部正常；下载文件名也均正常。
