#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// HttpClient — HTTP 客户端（基于 httplib）
// ============================================================================
#include <string>
#include <map>
#include <sstream>

namespace newrpa {
namespace net {

struct HttpResponse {
    int         status = 0;
    std::string body;
    std::string error;
    std::map<std::string, std::string> headers;
    double      elapsedMs = 0;
};

class NEWRPA_API HttpClient {
public:
    HttpClient() = default;

    // 统一入口
    HttpResponse Get(const std::string& url,
                     const std::map<std::string, std::string>& headers = {});
    HttpResponse Post(const std::string& url,
                       const std::string& body,
                       const std::string& contentType = "application/json",
                       const std::map<std::string, std::string>& headers = {});

    // 便捷
    std::string GetJson(const std::string& url);

    // 下载文件
    bool DownloadToFile(const std::string& url,
                        const std::string& localPath);

private:
    int connectTimeoutSec_ = 10;
};

} // namespace net
} // namespace newrpa
