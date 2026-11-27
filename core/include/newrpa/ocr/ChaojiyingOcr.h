#pragma once
// ============================================================================
// ChaojiyingOcr — 超级鹰打码（验证码识别），公共模块（所有银行插件共用）
//
// 接口: http://upload.chaojiying.net/Upload/Processing.php（multipart 表单）
//   - 密码必须传 pass2 = MD5(明文密码)；传明文 pass 会被判 -10023 认证失败
//     （实测：同一账号 pass2 成功、pass 明文失败）
//   - codetype 常用值: 1004=4位英文数字 1005=5位英文数字 1902=常见验证码
//
// 凭据来源（优先级高 → 低，代码/仓库里绝不放账号密码，值也绝不进日志）：
//   1. 环境变量 CJY_USER / CJY_PASS / CJY_SOFTID / CJY_URL / CJY_CODETYPE
//   2. config.json 顶层 "ocr" 段 { "user", "pass", "softid", "url", "codetype" }
//
// 插件用法：
//   #include "newrpa/ocr/ChaojiyingOcr.h"
//   std::string code, err;
//   if (newrpa::ocr::GlobalOcrReady() &&
//       newrpa::ocr::ChaojiyingOcr(newrpa::ocr::GlobalOcrCred())
//           .OcrFromDataUrl(imgSrc, "1902", code, err)) { ... }
// ============================================================================
#include "newrpa/NewRpaExport.h"
#include <string>

namespace newrpa {
namespace ocr {

struct NEWRPA_API OcrCred {
    std::string user;
    std::string pass;
    std::string softId;
    std::string url      = "http://upload.chaojiying.net/Upload/Processing.php";
    std::string codeType = "1004";
    bool Valid() const {
        return !user.empty() && !pass.empty() && !softId.empty();
    }
};

// 解析凭据：环境变量优先，没设的项回退到 config.json 的 ocr 段
NEWRPA_API OcrCred ResolveOcrCred(const OcrCred& fromConfig);

// 全局凭据（main.cpp 加载 config.json 后设置一次，所有银行插件共享）
NEWRPA_API void SetGlobalOcr(const OcrCred& cred);
NEWRPA_API bool GlobalOcrReady();
NEWRPA_API OcrCred GlobalOcrCred();

class NEWRPA_API ChaojiyingOcr {
public:
    explicit ChaojiyingOcr(OcrCred cred) : cred_(std::move(cred)) {}

    // 本地图片文件识别（如登录页截图验证码 file:xxx.png）
    bool OcrFromFile(const std::string& path, const std::string& codeType,
                     std::string& out, std::string& err);

    // 页面内联图片：data URL（data:image/png;base64,xxx）或纯 base64。
    // MIME 决定临时文件扩展名（默认 .jpg），上传完即删临时文件。
    bool OcrFromDataUrl(const std::string& src, const std::string& codeType,
                        std::string& out, std::string& err);

private:
    // multipart 上传（user + pass2=MD5(pass) + softid + codetype + userfile）
    bool Upload(const std::string& filename, const std::string& bytes,
                const std::string& codeType, std::string& out, std::string& err);

    OcrCred cred_;
};

} // namespace ocr
} // namespace newrpa
