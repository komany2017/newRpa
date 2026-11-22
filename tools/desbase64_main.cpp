// ============================================================================
// desbase64.exe — DesBase64 命令行工具（Base64 编解码）
//
// 用法:
//   desbase64 -e "文本"                 文本 → Base64
//   desbase64 -d "aGVsbG8="              Base64 → 文本
//   desbase64 -ef in.bin                文件编码（结果打印到屏幕）
//   desbase64 -ef in.bin -o out.txt     文件编码（写文件）
//   desbase64 -df in.txt -o out.bin     文件解码（写二进制文件）
//   echo hello | desbase64 -e           从管道读输入
//
// 注意: 参数按原始字节处理（不转码），GBK/UTF-8 文本都能原样往返。
// ============================================================================

#include "DesBase64.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void Usage() {
    std::printf(
        "desbase64 — Base64 编解码工具\n\n"
        "用法:\n"
        "  desbase64 -e \"文本\"               文本 -> Base64\n"
        "  desbase64 -d \"aGVsbG8=\"           Base64 -> 文本\n"
        "  desbase64 -ef in.bin [-o out.txt]  文件编码\n"
        "  desbase64 -df in.txt [-o out.bin]  文件解码\n"
        "  echo hello | desbase64 -e          从标准输入读\n");
}

static bool ReadAllStdin(std::string& out) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
    // 去掉管道常见的结尾换行（echo 会带 \r\n）
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return true;
}

static bool ReadFileBytes(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "打不开文件: %s\n", path.c_str());
        return false;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

static bool WriteOutput(const std::string& path, const void* data, size_t len,
                        bool binary) {
    if (path.empty()) {
        if (binary) _setmode(_fileno(stdout), _O_BINARY);
        fwrite(data, 1, len, stdout);
        if (binary) {
            fflush(stdout);
        } else {
            std::fputc('\n', stdout);
        }
        return true;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "写文件失败: %s\n", path.c_str());
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    std::fclose(f);
    if (ok) std::fprintf(stderr, "已写入 %s (%zu 字节)\n", path.c_str(), len);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 1;
    }

    bool encode = false, decode = false, fileMode = false;
    std::string input, outPath;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-e" || a == "--encode") {
            encode = true;
        } else if (a == "-d" || a == "--decode") {
            decode = true;
        } else if (a == "-ef" || a == "--encode-file") {
            encode = true;
            fileMode = true;
        } else if (a == "-df" || a == "--decode-file") {
            decode = true;
            fileMode = true;
        } else if (a == "-o" || a == "--out") {
            if (i + 1 < argc) outPath = argv[++i];
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            Usage();
            return 1;
        } else {
            input = a;   // 位置参数：待处理的文本 / 文件名
        }
    }

    if (encode == decode) {
        std::fprintf(stderr, "请指定 -e（编码）或 -d（解码）\n");
        Usage();
        return 1;
    }

    // 1. 取输入
    std::string data;
    if (fileMode) {
        if (input.empty()) {
            std::fprintf(stderr, "文件模式需要文件名\n");
            return 1;
        }
        if (!ReadFileBytes(input, data)) return 1;
    } else if (!input.empty()) {
        data = input;
    } else {
        ReadAllStdin(data);   // 管道 / 交互输入
    }

    if (data.empty()) {
        std::fprintf(stderr, "输入为空\n");
        return 1;
    }

    // 2. 编 / 解码
    if (encode) {
        std::string out(((data.size() + 2) / 3) * 4 + 1, '\0');
        int n = DesBase64::BASE64_Encode(
            reinterpret_cast<const unsigned char*>(data.data()),
            static_cast<int>(data.size()), &out[0]);
        if (n <= 0) {
            std::fprintf(stderr, "编码失败\n");
            return 1;
        }
        return WriteOutput(outPath, out.data(),
                           static_cast<size_t>(n), false) ? 0 : 1;
    }

    // 解码：多分配 3 字节余量（不足 4 字符组时有零头）
    std::string out(data.size() / 4 * 3 + 4, '\0');
    int n = DesBase64::BASE64_Decode(data.c_str(),
                                     static_cast<int>(data.size()), &out[0]);
    if (n < 0) {
        std::fprintf(stderr, "解码失败（错误码 %d：非法 Base64 字符串）\n", n);
        return 1;
    }
    // 输出二进制到 stdout，避免 Windows 文本模式把 \n 改成 \r\n
    return WriteOutput(outPath, out.data(), static_cast<size_t>(n), true) ? 0 : 1;
}
