#pragma once
// ============================================================================
// SlipBundle.h — 电子回单 ZIP 解压 + 与 xxxx_data.json 行的「一一对应」索引
//
// 背景：北京银行电子回单下载得到的是一个 ZIP，里面按「表格行顺序」每个回单一个
//       PDF，文件名是银行内部回单号（如 8905225140384645120.pdf），页面表格里
//       并没有这个号，因此文件与 data.json 的行无法靠字段直接关联。
//
// 本模块负责把这条关联显式化：
//   1) 解压 ZIP 中所有 *.pdf 到指定目录（保留银行原始文件名）
//   2) 从 PDF 文本里抽出「记账日期」（PDF 里数字小写金额被银行子集字体编码成
//      非 ASCII 码位，不可靠；日期是 UTF-16BE 明文，可靠）
//   3) 按「日期优先 + 原始顺序兜底」把每个 PDF 绑定到 data.json 的某一行
//   4) 写出索引 JSON：{ items:[{seq,pdf,pdfDate,rowSeq,matched,row:{...}}],
//                       rowCount,pdfCount,matchedCount,allMatched }
//
// 说明：顺序本身是银行按表格行序生成的（实测 4 条回单日期 09-21/06-21/03-21/
//       03-11 与表格行顺序完全一致），日期校验用来发现「顺序被打破」的情况。
// ============================================================================

#include "newrpa/io/Xlsx.h"          // 复用 unzip / inflateRaw

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace newrpa {
namespace io {
namespace slip {

namespace fs = std::filesystem;

// MinGW 下 path::string() 走 ANSI 代码页，中文路径会抛
// "Cannot convert character sequence"，统一用 UTF-8 互转
inline fs::path U8Path(const std::string& s) {
#ifdef _WIN32
    // C++20 起已弃用 fs::u8path，等价于用 UTF-8 串直接构造 path
    return fs::path(reinterpret_cast<const char8_t*>(s.c_str()));
#else
    return fs::path(s);
#endif
}
inline std::string PathToU8(const fs::path& p) {
#ifdef _WIN32
    // C++20 起 u8string() 返回 std::u8string(char8_t)，转回 std::string
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return p.string();
#endif
}

struct SlipIndexResult {
    int rowCount = 0;
    int pdfCount = 0;
    int matchedCount = 0;
    bool allMatched = false;
    std::string indexPath;
    std::string pdfDir;
};

// ---------------------------------------------------------------- 文件读写
inline bool ReadAll(const std::string& path, std::string& out) {
    std::ifstream f(U8Path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

inline bool WriteAll(const std::string& path, const std::string& data) {
    fs::path p = U8Path(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

inline std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           Lower(s.substr(s.size() - suf.size())) == Lower(suf);
}

// ---------------------------------------------------------------- PDF 文本
// 把一个 PDF 内容流里的字面量字符串按 UTF-16BE 解出来。
// 北京银行回单 PDF 用 Identity-H 编码的 CID 字体，字面量字节就是 Unicode 码位
// 的大端 2 字节表示，因此直接 utf-16be 解码即可得到可读文本。
inline std::string DecodeUtf16Be(const std::string& b) {
    std::string out;
    out.reserve(b.size() / 2);
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        unsigned int u = (static_cast<unsigned char>(b[i]) << 8) |
                         static_cast<unsigned char>(b[i + 1]);
        if (u == 0) continue;
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

// 抽取一个内容流里所有 ( ... ) 字面量（处理 \( \) \\ 转义）
inline std::vector<std::string> LiteralsOfStream(const std::string& s) {
    std::vector<std::string> out;
    std::string buf;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '(') { ++i; continue; }
        ++i;
        buf.clear();
        int depth = 1;
        while (i < s.size() && depth > 0) {
            char c = s[i];
            if (c == '\\') {
                ++i;
                if (i >= s.size()) break;
                char e = s[i];
                switch (e) {
                    case 'n': buf.push_back('\n'); break;
                    case 'r': buf.push_back('\r'); break;
                    case 't': buf.push_back('\t'); break;
                    case 'b': buf.push_back('\b'); break;
                    case 'f': buf.push_back('\f'); break;
                    default:
                        if (e >= '0' && e <= '7') {       // 八进制 \ddd
                            int v = 0, k = 0;
                            while (k < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7') {
                                v = v * 8 + (s[i] - '0');
                                ++i; ++k;
                            }
                            buf.push_back(static_cast<char>(v & 0xFF));
                            continue;
                        }
                        buf.push_back(e);
                        break;
                }
                ++i;
                continue;
            }
            if (c == '(') { ++depth; buf.push_back(c); ++i; continue; }
            if (c == ')') { --depth; if (depth == 0) { ++i; break; } buf.push_back(c); ++i; continue; }
            buf.push_back(c);
            ++i;
        }
        out.push_back(buf);
    }
    return out;
}

// 返回 PDF 的可读文本（失败/无文本返回空串）
inline std::string PdfText(const std::string& pdf) {
    std::string all;
    size_t pos = 0;
    while (true) {
        size_t s = pdf.find("stream", pos);
        if (s == std::string::npos) break;
        // 排除 "endstream" 里的 "stream"
        if (s >= 3 && pdf.compare(s - 3, 3, "end") == 0) { pos = s + 6; continue; }
        size_t d = s + 6;
        if (d < pdf.size() && pdf[d] == '\r') ++d;
        if (d < pdf.size() && pdf[d] == '\n') ++d;
        size_t e = pdf.find("endstream", d);
        if (e == std::string::npos) break;
        std::string chunk = pdf.substr(d, e - d);
        pos = e + 9;

        std::string plain;
        bool ok = false;
        // PDF FlateDecode = zlib 包装（2 字节头 + raw deflate + adler32）
        if (chunk.size() > 2 && static_cast<unsigned char>(chunk[0]) == 0x78)
            ok = xlsx_detail::inflateRaw(chunk.substr(2), plain);
        if (!ok) ok = xlsx_detail::inflateRaw(chunk, plain);
        if (!ok || plain.empty()) continue;

        for (auto& lit : LiteralsOfStream(plain)) all += DecodeUtf16Be(lit);
    }
    return all;
}

// 从文本里取第一个 YYYY-MM-DD；没有则返回空
inline std::string FirstDate(const std::string& text) {
    for (size_t i = 0; i + 10 <= text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        if (!(std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
              std::isdigit(static_cast<unsigned char>(text[i + 2])) &&
              std::isdigit(static_cast<unsigned char>(text[i + 3])))) continue;
        if (text[i + 4] != '-' || text[i + 7] != '-') continue;
        bool tail = true;
        for (int k = 5; k < 10; ++k)
            if (k != 7 && !std::isdigit(static_cast<unsigned char>(text[i + k]))) tail = false;
        if (!tail) continue;
        std::string d = text.substr(i, 10);
        int mm = std::stoi(d.substr(5, 2));
        int dd = std::stoi(d.substr(8, 2));
        if (mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31) return d;
    }
    return "";
}

// ---------------------------------------------------------------- 表格归一
// 把 [[表头],[数据]...] 归一成「每行列数 == 表头列数」。
// 电子回单页的 el-table 有勾选列：表头循环会跳过空表头，数据行却保留该单元格，
// 于是出现 表头 11 列 / 数据 12 值。这里把多出来的那个「勾选列」整列丢掉。
inline void NormalizeTable(std::vector<std::string>& header,
                           std::vector<std::vector<std::string>>& rows) {
    if (header.empty()) return;
    size_t hlen = header.size();
    // 情况一：每行都比表头多 1，且多出来的首格全为空 -> 丢首列
    bool allPlusOne = true;
    for (auto& r : rows) {
        if (r.size() != hlen + 1 || (!r.empty() && !r[0].empty())) { allPlusOne = false; break; }
    }
    if (allPlusOne) {
        for (auto& r : rows) r.erase(r.begin());
        return;
    }
    // 情况二：表头首格为空、数据首格全为空 -> 丢首列（表头也丢）
    if (!header[0].empty()) return;
    bool allEmptyFirst = true;
    for (auto& r : rows) {
        if (r.empty() || !r[0].empty()) { allEmptyFirst = false; break; }
    }
    if (!allEmptyFirst) return;
    header.erase(header.begin());
    for (auto& r : rows) if (!r.empty()) r.erase(r.begin());
}

// 按表头名取日期（记账日期 / 交易时间 / 日期），截到 YYYY-MM-DD
inline std::string RowDate(const std::vector<std::string>& header,
                           const std::vector<std::string>& row) {
    static const char* keys[] = {"记账日期", "交易时间", "交易日期", "日期"};
    for (const char* k : keys) {
        for (size_t i = 0; i < header.size() && i < row.size(); ++i) {
            if (header[i] == k) {
                std::string d = FirstDate(row[i]);
                if (!d.empty()) return d;
            }
        }
    }
    return "";
}

// ---------------------------------------------------------------- 排序
inline bool NaturalLess(const std::string& a, const std::string& b) {
    auto stem = [](const std::string& s) {
        size_t p = s.find_last_of('.');
        return p == std::string::npos ? s : s.substr(0, p);
    };
    std::string sa = stem(a), sb = stem(b);
    bool na = !sa.empty(), nb = !sb.empty();
    for (char c : sa) if (!std::isdigit(static_cast<unsigned char>(c))) na = false;
    for (char c : sb) if (!std::isdigit(static_cast<unsigned char>(c))) nb = false;
    if (na && nb) {
        if (sa.size() != sb.size()) return sa.size() < sb.size();
        return sa < sb;
    }
    return a < b;
}

// ---------------------------------------------------------------- 主流程
// zipPath      : 银行下载回来的 ZIP
// dataJsonPath : 同批下载的 xxxx_data.json
// outPdfDir    : PDF 解压目标目录
// indexPath    : 索引 JSON 输出路径
inline bool BuildIndexFromBlobs(std::vector<std::string> names,
                                const std::map<std::string, std::string>& blobs,
                                const std::string& source,
                                const std::string& dataJsonPath,
                                const std::string& outPdfDir,
                                const std::string& indexPath,
                                SlipIndexResult* res,
                                std::string* err);

inline bool BuildIndex(const std::string& zipPath,
                       const std::string& dataJsonPath,
                       const std::string& outPdfDir,
                       const std::string& indexPath,
                       SlipIndexResult* res,
                       std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    std::string zipBytes;
    if (!ReadAll(zipPath, zipBytes)) return fail("cannot read zip: " + zipPath);

    std::map<std::string, std::string> parts;
    std::string zerr;
    if (!xlsx_detail::unzip(zipBytes, parts, &zerr)) return fail("unzip failed: " + zerr);

    // 收集 PDF，按回单号自然序（等同银行生成顺序）
    std::vector<std::string> names;
    for (auto& kv : parts)
        if (EndsWith(kv.first, ".pdf")) names.push_back(kv.first);
    std::sort(names.begin(), names.end(), NaturalLess);
    if (names.empty()) return fail("no .pdf inside zip (entries=" + std::to_string(parts.size()) + ")");
    return BuildIndexFromBlobs(std::move(names), parts,
                               PathToU8(U8Path(zipPath).filename()),
                               dataJsonPath, outPdfDir, indexPath, res, err);
}

// 核心：names + blobs 就绪（来自 ZIP 解压或单个 PDF 文件）后统一建索引
inline bool BuildIndexFromBlobs(std::vector<std::string> names,
                                const std::map<std::string, std::string>& blobs,
                                const std::string& source,
                                const std::string& dataJsonPath,
                                const std::string& outPdfDir,
                                const std::string& indexPath,
                                SlipIndexResult* res,
                                std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    std::error_code ec;
    fs::create_directories(outPdfDir, ec);
    for (auto& n : names) {
        std::string base = PathToU8(U8Path(n).filename());
        if (!WriteAll(PathToU8(U8Path(outPdfDir) / U8Path(base)), blobs.at(n)))
            return fail("write pdf failed: " + base);
    }

    // 读 data.json
    std::string js;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    if (ReadAll(dataJsonPath, js)) {
        try {
            auto arr = nlohmann::json::parse(js);
            if (arr.is_array() && !arr.empty()) {
                auto& t = arr[0];
                if (t.contains("data") && t["data"].is_array() && !t["data"].empty()) {
                    for (auto& h : t["data"][0]) header.push_back(h.get<std::string>());
                    for (size_t i = 1; i < t["data"].size(); ++i) {
                        std::vector<std::string> r;
                        for (auto& c : t["data"][i]) r.push_back(c.is_string() ? c.get<std::string>() : c.dump());
                        rows.push_back(std::move(r));
                    }
                }
            }
        } catch (...) { /* 容忍：data.json 缺失/损坏时仍导出 PDF，只是不建映射 */ }
    }
    NormalizeTable(header, rows);

    // 每个 PDF 的文本与日期（先算，后面用于判定「字段值是否出现在 PDF 正文里」）
    std::vector<std::string> pdfDates, pdfTexts;
    pdfDates.reserve(names.size());
    pdfTexts.reserve(names.size());
    for (auto& n : names) {
        std::string t = PdfText(blobs.at(n));
        pdfDates.push_back(FirstDate(t));
        pdfTexts.push_back(std::move(t));
    }

    // 可选：同名 xxxx_fields.json（页面 Vue 模型全字段）。
    // 不假设字段名：自动挑一个满足「行内唯一 + 每个值恰好命中一个 PDF 且互不撞车」
    // 的字段作为匹配键 —— 流水号/凭证号/日期都可能胜出；账号因每行相同会被排除。
    std::vector<std::string> rowIds;
    std::string idField;
    std::vector<int> pdfToRow(names.size(), -1);
    {
        std::string fp = dataJsonPath;
        const std::string suf = "_data.json";
        if (fp.size() >= suf.size() &&
            fp.compare(fp.size() - suf.size(), suf.size(), suf) == 0)
            fp = fp.substr(0, fp.size() - suf.size()) + "_fields.json";
        std::string fj;
        if (ReadAll(fp, fj)) {
            std::vector<std::map<std::string, std::string>> frows;
            try {
                auto arr = nlohmann::json::parse(fj);
                if (arr.is_array()) {
                    const nlohmann::json* best = nullptr;
                    int bestN = -1;
                    for (auto& t : arr) {
                        if (!t.contains("rows") || !t["rows"].is_array()) continue;
                        int rc = t.value("rowCount", 0);
                        if (rc > bestN) { bestN = rc; best = &t; }
                    }
                    if (best) {
                        for (auto& r : (*best)["rows"]) {
                            std::map<std::string, std::string> m;
                            for (auto it = r.begin(); it != r.end(); ++it)
                                if (it.value().is_string()) m[it.key()] = it.value().get<std::string>();
                            frows.push_back(std::move(m));
                        }
                    }
                }
            } catch (...) { /* fields.json 不可用时退回日期/顺序匹配 */ }

            if (frows.size() == names.size() && !frows.empty()) {
                static const char* prefer[] = {"流水号", "交易流水号", "凭证号", "单据号",
                                               "serialNo", "serialNumber", "flowNo",
                                               "jyh", "seqNo"};
                auto rank = [](const std::string& k) {
                    for (size_t i = 0; i < sizeof(prefer) / sizeof(prefer[0]); ++i)
                        if (k == prefer[i]) return static_cast<int>(i);
                    return 100;
                };
                std::vector<std::string> keys;
                for (auto& r : frows)
                    for (auto& kv : r)
                        if (std::find(keys.begin(), keys.end(), kv.first) == keys.end())
                            keys.push_back(kv.first);
                std::stable_sort(keys.begin(), keys.end(),
                                 [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });

                for (auto& k : keys) {
                    std::vector<std::string> vals;
                    bool allOk = true;
                    for (auto& r : frows) {
                        auto it = r.find(k);
                        if (it == r.end() || it->second.size() < 6) { allOk = false; break; }
                        vals.push_back(it->second);
                    }
                    if (!allOk) continue;
                    std::set<std::string> uniq(vals.begin(), vals.end());
                    if (uniq.size() != vals.size()) continue;      // 行内必须唯一

                    std::vector<int> hit(vals.size(), -1);
                    std::vector<int> cand(names.size(), -1);
                    std::set<int> usedPdf;
                    bool bij = true;
                    for (size_t r = 0; r < vals.size() && bij; ++r) {
                        int h = -1, cnt = 0;
                        for (size_t j = 0; j < pdfTexts.size(); ++j)
                            if (pdfTexts[j].find(vals[r]) != std::string::npos) { h = static_cast<int>(j); ++cnt; }
                        if (cnt != 1 || usedPdf.count(h)) { bij = false; break; }
                        hit[r] = h;
                        usedPdf.insert(h);
                        cand[h] = static_cast<int>(r);
                    }
                    if (!bij) continue;
                    rowIds = vals;
                    idField = k;
                    pdfToRow = cand;        // 每个 PDF 精确对应的行号
                    break;
                }
            }
        }
    }

    // 绑定：流水号命中 > 同位置同日期 > 同日期未占用 > 原始顺序
    std::vector<bool> used(rows.size(), false);
    nlohmann::json items = nlohmann::json::array();
    int matched = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& d = pdfDates[i];
        int pick = -1;
        std::string how = "order";
        if (i < pdfToRow.size() && pdfToRow[i] >= 0 &&
            static_cast<size_t>(pdfToRow[i]) < rows.size()) {
            pick = pdfToRow[i];
            how = idField.empty() ? "order" : idField;
        }
        if (pick < 0 && i < rows.size() && !used[i] && !d.empty() &&
            RowDate(header, rows[i]) == d) { pick = static_cast<int>(i); how = "date"; }
        if (pick < 0 && !d.empty()) {
            for (size_t j = 0; j < rows.size(); ++j) {
                if (!used[j] && RowDate(header, rows[j]) == d) {
                    pick = static_cast<int>(j); how = "date"; break;
                }
            }
        }
        if (pick < 0 && i < rows.size() && !used[i]) pick = static_cast<int>(i);

        bool ok = false;
        nlohmann::json item;
        item["seq"] = static_cast<int>(i + 1);
        item["pdf"] = PathToU8(U8Path(names[i]).filename());
        item["pdfDate"] = d;
        item["rowSeq"] = pick >= 0 ? pick + 1 : 0;
        item["matchBy"] = pick >= 0 ? how : "none";
        if (pick >= 0) {
            used[pick] = true;
            std::string rd = RowDate(header, rows[pick]);
            if (!idField.empty() && pick < static_cast<int>(rowIds.size()))
                item["rowId"] = rowIds[pick];
            ok = (idField.size() && how == idField) || (!d.empty() && d == rd);
            nlohmann::json ro = nlohmann::json::object();
            for (size_t c = 0; c < header.size() && c < rows[pick].size(); ++c)
                ro[header[c]] = rows[pick][c];
            item["row"] = ro;
            item["rowDate"] = rd;
        }
        item["matched"] = ok;
        if (ok) ++matched;
        items.push_back(std::move(item));
    }

    nlohmann::json doc;
    std::string dtag = PathToU8(U8Path(dataJsonPath).stem());
    if (dtag.size() > 5 && dtag.compare(dtag.size() - 5, 5, "_data") == 0)
        dtag.resize(dtag.size() - 5);
    doc["dataTag"] = dtag;
    doc["source"] = source;
    doc["pdfDir"] = PathToU8(U8Path(outPdfDir).filename());
    doc["rowCount"] = static_cast<int>(rows.size());
    doc["pdfCount"] = static_cast<int>(names.size());
    doc["matchedCount"] = matched;
    doc["idField"] = idField;
    doc["allMatched"] = (matched == static_cast<int>(names.size()) &&
                         names.size() == rows.size());
    doc["items"] = items;

    if (!WriteAll(indexPath, doc.dump(2))) return fail("write index failed: " + indexPath);

    if (res) {
        res->rowCount = static_cast<int>(rows.size());
        res->pdfCount = static_cast<int>(names.size());
        res->matchedCount = matched;
        res->allMatched = doc["allMatched"].get<bool>();
        res->indexPath = indexPath;
        res->pdfDir = outPdfDir;
    }
    return true;
}

// 逐条下载场景：文件已经是单个 PDF（不是 ZIP），直接索引并收拢到 outPdfDir
inline bool BuildIndexFromPdfs(const std::vector<std::string>& pdfPaths,
                               const std::string& dataJsonPath,
                               const std::string& outPdfDir,
                               const std::string& indexPath,
                               SlipIndexResult* res, std::string* err) {
    std::vector<std::string> names;
    std::map<std::string, std::string> blobs;
    for (auto& p : pdfPaths) {
        std::string b;
        if (!ReadAll(p, b)) continue;
        std::string base = PathToU8(U8Path(p).filename());
        names.push_back(base);
        blobs[base] = std::move(b);
    }
    if (names.empty()) { if (err) *err = "no .pdf files"; return false; }
    std::sort(names.begin(), names.end(), NaturalLess);
    return BuildIndexFromBlobs(std::move(names), blobs, "per-row pdf",
                               dataJsonPath, outPdfDir, indexPath, res, err);
}

// 便捷版：从一批下载产物里建索引（ZIP 优先，其次单个 PDF）
inline bool BuildFromDownloads(const std::vector<std::string>& files,
                               const std::string& downloadDir,
                               const std::string& dataTag,
                               SlipIndexResult* res,
                               std::string* err) {
    fs::path dir = U8Path(downloadDir);
    std::string dj = PathToU8(dir / U8Path(dataTag + "_data.json"));
    std::string od = PathToU8(dir / U8Path(dataTag + "_pdf"));
    std::string ix = PathToU8(dir / U8Path(dataTag + "_index.json"));

    std::vector<std::string> zips, pdfs;
    for (auto& f : files) {
        if (EndsWith(f, ".zip")) zips.push_back(f);
        else if (EndsWith(f, ".pdf")) pdfs.push_back(f);
    }
    // 一个 ZIP（整批打包）或 多个 ZIP（逐条下载，每个 1 张回单）都合并处理
    if (!zips.empty()) {
        std::map<std::string, std::string> all;
        std::vector<std::string> names;
        for (auto& z : zips) {
            std::string bytes, zerr;
            if (!ReadAll(z, bytes)) continue;
            std::map<std::string, std::string> parts;
            if (!xlsx_detail::unzip(bytes, parts, &zerr)) {
                continue;
            }
            for (auto& kv : parts) {
                if (!EndsWith(kv.first, ".pdf")) continue;
                std::string n = kv.first;
                int dup = 2;
                while (all.count(n)) n = kv.first + "_" + std::to_string(dup++);
                all[n] = kv.second;
                names.push_back(n);
            }
        }
        if (!names.empty()) {
            std::sort(names.begin(), names.end(), NaturalLess);
            std::string label = zips.size() == 1 ? PathToU8(U8Path(zips[0]).filename())
                                                 : (std::to_string(zips.size()) + " zips");
            return BuildIndexFromBlobs(std::move(names), all, label, dj, od, ix, res, err);
        }
    }
    if (!pdfs.empty()) return BuildIndexFromPdfs(pdfs, dj, od, ix, res, err);
    if (err) *err = "no .zip / .pdf in downloads";
    return false;
}

} // namespace slip
} // namespace io
} // namespace newrpa
