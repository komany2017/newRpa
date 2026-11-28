#pragma once
// ============================================================================
// Xlsx.h — 零依赖 XLSX 读写（单头文件）
//
// 特性：
//   * 内置 ZIP 读/写 与 raw DEFLATE 解压，不依赖 zlib / libxlsxwriter / COM
//   * 读取：共享字符串、内联字符串、公式缓存值、数字/布尔/错误、样式日期
//   * 写入：STRORE 方式打包（Excel/WPS/LibreOffice 均可直接打开）
//   * 从文件加载后另存：原样保留 styles/workbook/rels 等部件，只替换工作表数据
//   * 也可从零新建工作簿
//
// 用法：
//   newrpa::io::Xlsx wb;
//   std::string err;
//   if (!wb.Load("余额.xlsx", &err)) { /* err */ }
//   for (size_t r = 0; r < wb.rowCount(); ++r)
//       std::cout << wb.getString(r, 0) << "\n";
//   wb.setString(0, 0, "户名");
//   wb.Save("out.xlsx", &err);
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace newrpa {
namespace io {
namespace xlsx_detail {

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------
inline std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

inline bool writeAll(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

inline bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isWs(s[a])) ++a;
    while (b > a && isWs(s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string xmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t e = s.find(';', i + 1);
        if (e == std::string::npos) { out += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                if (ent[1] == 'x' || ent[1] == 'X')
                    cp = std::strtoul(ent.c_str() + 2, nullptr, 16);
                else
                    cp = std::strtoul(ent.c_str() + 1, nullptr, 10);
            } catch (...) { cp = 0; }
            if (cp == 0) { out += '?'; }
            else if (cp < 0x80) { out += static_cast<char>(cp); }
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else { out.append(s, i, e - i + 1); }
        i = e;
    }
    return out;
}

inline std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
                    continue;
                out += static_cast<char>(c);
        }
    }
    return out;
}

// 找到 <tag ...> 或 <tag .../> 的起始位置（避免匹配 <tags>）
inline size_t findTag(const std::string& s, const std::string& tag, size_t from = 0) {
    const std::string open = "<" + tag;
    size_t p = from;
    while ((p = s.find(open, p)) != std::string::npos) {
        size_t q = p + open.size();
        if (q >= s.size() || isWs(s[q]) || s[q] == '>' || s[q] == '/') return p;
        p += open.size();
    }
    return std::string::npos;
}

inline std::string attr(const std::string& tag, const std::string& name) {
    const std::string pat = name + "=";
    size_t p = 0;
    while ((p = tag.find(pat, p)) != std::string::npos) {
        if (p == 0 || isWs(tag[p - 1])) {
            size_t q = p + pat.size();
            while (q < tag.size() && isWs(tag[q])) ++q;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                char quote = tag[q++];
                size_t e = tag.find(quote, q);
                if (e != std::string::npos) return tag.substr(q, e - q);
            }
        }
        p += pat.size();
    }
    return std::string();
}

inline std::string tagContent(const std::string& body, const std::string& tag) {
    size_t b = findTag(body, tag);
    if (b == std::string::npos) return std::string();
    size_t e = body.find('>', b);
    if (e == std::string::npos) return std::string();
    size_t c = body.find("</" + tag + ">", e + 1);
    if (c == std::string::npos) return std::string();
    return xmlUnescape(body.substr(e + 1, c - e - 1));
}

inline bool cellRef(const std::string& ref, size_t& row, size_t& col) {
    row = 0; col = 0;
    size_t i = 0;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) {
        col = col * 26 + static_cast<size_t>(std::toupper(static_cast<unsigned char>(ref[i])) - 'A' + 1);
        ++i;
    }
    if (i == 0 || i == ref.size()) return false;
    col -= 1;
    for (; i < ref.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(ref[i]))) return false;
        row = row * 10 + static_cast<size_t>(ref[i] - '0');
    }
    if (row == 0) return false;
    row -= 1;
    return true;
}

inline std::string colName(size_t col0) {
    std::string s;
    size_t n = col0 + 1;
    while (n) {
        --n;
        s.insert(s.begin(), static_cast<char>('A' + n % 26));
        n /= 26;
    }
    return s;
}

inline std::string formatNumber(double v) {
    if (std::fabs(v - std::round(v)) < 1e-10 && std::fabs(v) < 1e15)
        return std::to_string(static_cast<long long>(std::round(v)));
    std::ostringstream os;
    os.precision(15);
    os << v;
    return os.str();
}

// Excel 序列号 -> "YYYY-MM-DD" 或 "YYYY-MM-DD HH:MM:SS"
inline std::string serialToDate(double serial) {
    if (!(serial > 0) || serial > 2958465) return std::string();
    long days = static_cast<long>(std::floor(serial));
    double frac = serial - days;
    long unixDays;
    if (days == 60) return "1900-02-29";
    unixDays = (days < 61) ? (days - 25568) : (days - 25569);
    // Hinnant civil_from_days
    long z = unixDays + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = static_cast<long>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));
    y += (m <= 2);
    std::ostringstream os;
    os << (y < 0 ? "-" : "") << (y < 0 ? -y : y) << '-'
       << (m < 10 ? "0" : "") << m << '-'
       << (d < 10 ? "0" : "") << d;
    if (frac > 1e-9) {
        long sec = static_cast<long>(std::floor(frac * 86400 + 0.5));
        long hh = sec / 3600, mm = (sec % 3600) / 60, ss = sec % 60;
        os << ' ' << (hh < 10 ? "0" : "") << hh << ':'
           << (mm < 10 ? "0" : "") << mm << ':'
           << (ss < 10 ? "0" : "") << ss;
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// CRC32 / raw DEFLATE inflate
// ---------------------------------------------------------------------------
inline uint32_t crc32(const std::string& data) {
    static uint32_t table[256] = {0};
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data)
        c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct BitReader {
    const std::string& src;
    size_t pos = 0; // bit position

    explicit BitReader(const std::string& s) : src(s) {}

    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) {
            size_t byte = pos >> 3;
            if (byte < src.size())
                v |= ((static_cast<unsigned char>(src[byte]) >> (pos & 7)) & 1u) << i;
            ++pos;
        }
        return v;
    }
    void align() { pos = (pos + 7) & ~static_cast<size_t>(7); }
};

struct Huffman {
    int counts[16] = {0};
    std::vector<int> symbols;

    void build(const uint8_t* lengths, int n) {
        std::fill(counts, counts + 16, 0);
        for (int i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        int offsets[16] = {0};
        int off = 0;
        for (int len = 1; len < 16; ++len) { offsets[len] = off; off += counts[len]; }
        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[static_cast<size_t>(offsets[lengths[i]]++)] = i;
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bits(1);
            int count = counts[len];
            if (code - first < count)
                return symbols[static_cast<size_t>(index + code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool inflateRaw(const std::string& src, std::string& out) {
    static const int lenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const int lenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const int distBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,
        193,257,385,513,769,1025,1537,2049,3073,4097,
        6145,8193,12289,16385,24577
    };
    static const int distExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,
        6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };

    BitReader br(src);
    out.clear();
    try {
        while (true) {
            int final = br.bits(1);
            int type = br.bits(2);
            if (type == 0) {
                br.align();
                size_t p = br.pos >> 3;
                if (p + 4 > src.size()) return false;
                int len = static_cast<unsigned char>(src[p]) |
                          (static_cast<unsigned char>(src[p + 1]) << 8);
                if (p + 4 + static_cast<size_t>(len) > src.size()) return false;
                out.append(src, p + 4, static_cast<size_t>(len));
                br.pos = (p + 4 + static_cast<size_t>(len)) << 3;
            } else if (type == 1 || type == 2) {
                Huffman lit, dist;
                if (type == 1) {
                    uint8_t l[288], d[32];
                    for (int i = 0; i < 144; ++i) l[i] = 8;
                    for (int i = 144; i < 256; ++i) l[i] = 9;
                    for (int i = 256; i < 280; ++i) l[i] = 7;
                    for (int i = 280; i < 288; ++i) l[i] = 8;
                    for (int i = 0; i < 32; ++i) d[i] = 5;
                    lit.build(l, 288);
                    dist.build(d, 32);
                } else {
                    int hlit = br.bits(5) + 257;
                    int hdist = br.bits(5) + 1;
                    int hclen = br.bits(4) + 4;
                    static const int order[19] = {
                        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                    };
                    uint8_t cl[19] = {0};
                    for (int i = 0; i < hclen; ++i)
                        cl[order[i]] = static_cast<uint8_t>(br.bits(3));
                    Huffman clh;
                    clh.build(cl, 19);
                    std::vector<uint8_t> lengths;
                    lengths.reserve(static_cast<size_t>(hlit + hdist));
                    while (static_cast<int>(lengths.size()) < hlit + hdist) {
                        int sym = clh.decode(br);
                        if (sym < 0) return false;
                        int n = 1;
                        uint8_t value = 0;
                        if (sym == 16) {
                            if (lengths.empty()) return false;
                            value = lengths.back();
                            n = br.bits(2) + 3;
                        } else if (sym == 17) {
                            n = br.bits(3) + 3;
                        } else if (sym == 18) {
                            n = br.bits(7) + 11;
                        } else if (sym <= 15) {
                            value = static_cast<uint8_t>(sym);
                        } else return false;
                        while (n-- > 0 && static_cast<int>(lengths.size()) < hlit + hdist)
                            lengths.push_back(value);
                    }
                    std::vector<uint8_t> dl(lengths.begin() + hlit, lengths.end());
                    if (dl.empty()) dl.push_back(0);
                    lit.build(lengths.data(), hlit);
                    dist.build(dl.data(), static_cast<int>(dl.size()));
                }
                while (true) {
                    int sym = lit.decode(br);
                    if (sym < 0) return false;
                    if (sym < 256) {
                        out += static_cast<char>(sym);
                    } else if (sym == 256) {
                        break;
                    } else {
                        sym -= 257;
                        if (sym < 0 || sym >= 29) return false;
                        int length = lenBase[sym] + br.bits(lenExtra[sym]);
                        int ds = dist.decode(br);
                        if (ds < 0 || ds >= 30) return false;
                        int distance = distBase[ds] + br.bits(distExtra[ds]);
                        if (distance <= 0 || static_cast<size_t>(distance) > out.size()) return false;
                        size_t start = out.size() - static_cast<size_t>(distance);
                        for (int i = 0; i < length; ++i) out += out[start + static_cast<size_t>(i)];
                    }
                }
            } else {
                return false; // reserved block type
            }
            if (final) break;
        }
    } catch (...) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ZIP 读 / 写（只支持 xlsx 常见的 32 位条目，写入用 STORE）
// ---------------------------------------------------------------------------
inline uint16_t rd16(const std::string& s, size_t p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(s[p]) |
           (static_cast<unsigned char>(s[p + 1]) << 8));
}
inline uint32_t rd32(const std::string& s, size_t p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[p]) |
           (static_cast<unsigned char>(s[p + 1]) << 8) |
           (static_cast<unsigned char>(s[p + 2]) << 16) |
           (static_cast<unsigned char>(s[p + 3]) << 24));
}
inline void wr16(std::string& s, uint16_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
inline void wr32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

inline bool unzip(const std::string& zip,
                  std::map<std::string, std::string>& parts,
                  std::string* err) {
    parts.clear();
    size_t eocd = std::string::npos;
    if (zip.size() >= 22) {
        size_t lower = zip.size() > 66000 ? zip.size() - 66000 : 0;
        for (size_t i = zip.size() - 22; ; --i) {
            if (rd32(zip, i) == 0x06054b50u) { eocd = i; break; }
            if (i == lower) break;
        }
    }
    if (eocd == std::string::npos) {
        if (err) *err = "Invalid xlsx: ZIP EOCD not found";
        return false;
    }
    int count = rd16(zip, eocd + 10);
    size_t cd = rd32(zip, eocd + 16);
    for (int k = 0; k < count; ++k) {
        if (cd + 46 > zip.size() || rd32(zip, cd) != 0x02014b50u) {
            if (err) *err = "Invalid xlsx: bad central directory";
            return false;
        }
        uint16_t method = rd16(zip, cd + 10);
        uint32_t compSize = rd32(zip, cd + 20);
        uint32_t rawSize = rd32(zip, cd + 24);
        uint16_t nameLen = rd16(zip, cd + 28);
        uint16_t extraLen = rd16(zip, cd + 30);
        uint16_t cmtLen = rd16(zip, cd + 32);
        uint32_t local = rd32(zip, cd + 42);
        if (cd + 46 + nameLen > zip.size()) return false;
        std::string name = zip.substr(cd + 46, nameLen);
        if (local + 30 > zip.size() || rd32(zip, local) != 0x04034b50u) {
            if (err) *err = "Invalid xlsx: bad local header";
            return false;
        }
        uint16_t lname = rd16(zip, local + 26);
        uint16_t lextra = rd16(zip, local + 28);
        size_t dataStart = local + 30 + lname + lextra;
        if (dataStart + compSize > zip.size()) {
            if (err) *err = "Invalid xlsx: truncated entry " + name;
            return false;
        }
        std::string packed = zip.substr(dataStart, compSize);
        std::string plain;
        if (method == 0) {
            plain = packed;
        } else if (method == 8) {
            if (!inflateRaw(packed, plain)) {
                if (err) *err = "Invalid xlsx: decompress failed: " + name;
                return false;
            }
        } else {
            if (err) *err = "Unsupported ZIP method " + std::to_string(method);
            return false;
        }
        (void)rawSize;
        parts[name] = plain;
        cd += 46 + nameLen + extraLen + cmtLen;
    }
    return true;
}

inline std::string zipOne(const std::string& name, const std::string& data,
                          uint32_t offset, std::string& central) {
    std::string local;
    uint32_t crc = crc32(data);
    uint32_t size = static_cast<uint32_t>(data.size());
    wr32(local, 0x04034b50u);
    wr16(local, 20);            // version needed
    wr16(local, 0x0800);        // UTF-8
    wr16(local, 0);             // STORE
    wr16(local, 0); wr16(local, 0);
    wr32(local, crc); wr32(local, size); wr32(local, size);
    wr16(local, static_cast<uint16_t>(name.size()));
    wr16(local, 0);
    local += name;
    local += data;

    central.clear();
    wr32(central, 0x02014b50u);
    wr16(central, 20); wr16(central, 20);
    wr16(central, 0x0800); wr16(central, 0);
    wr16(central, 0); wr16(central, 0);
    wr32(central, crc); wr32(central, size); wr32(central, size);
    wr16(central, static_cast<uint16_t>(name.size()));
    wr16(central, 0); wr16(central, 0);
    wr16(central, 0); wr16(central, 0);
    wr32(central, 0); wr32(central, offset);
    central += name;
    return local;
}

inline bool zipParts(const std::vector<std::pair<std::string, std::string>>& parts,
                     const std::string& path, std::string* err) {
    std::string body, central;
    uint32_t offset = 0;
    for (const auto& kv : parts) {
        std::string oneCentral;
        body += zipOne(kv.first, kv.second, offset, oneCentral);
        central += oneCentral;
        offset = static_cast<uint32_t>(body.size());
    }
    size_t cdOffset = body.size();
    body += central;
    std::string eocd;
    wr32(eocd, 0x06054b50u);
    wr16(eocd, 0); wr16(eocd, 0);
    wr16(eocd, static_cast<uint16_t>(parts.size()));
    wr16(eocd, static_cast<uint16_t>(parts.size()));
    wr32(eocd, static_cast<uint32_t>(central.size()));
    wr32(eocd, static_cast<uint32_t>(cdOffset));
    wr16(eocd, 0);
    body += eocd;
    if (!writeAll(path, body)) {
        if (err) *err = "Cannot write " + path;
        return false;
    }
    return true;
}

} // namespace xlsx_detail

// ===========================================================================
// Xlsx
// ===========================================================================
class Xlsx {
public:
    struct Cell {
        enum Type { Empty, String, Number, Bool, Formula, Error } type = Empty;
        std::string text;      // 显示文本 / 字符串值
        double number = 0;     // 数值（number 类型）
        bool boolean = false;
        int style = 0;
        std::string formula;   // type==Formula 时的公式
    };

private:
    struct Sheet {
        std::string name;
        std::string part;      // xl/worksheets/sheetN.xml
        std::vector<std::vector<Cell>> rows;
    };

    std::vector<Sheet> sheets_;
    std::map<std::string, std::string> parts_;
    std::vector<std::string> shared_;
    std::vector<bool> dateStyle_;
    size_t cur_ = 0;

public:
    // -----------------------------------------------------------------------
    // 加载 / 保存
    // -----------------------------------------------------------------------
    bool Load(const std::string& path, std::string* err = nullptr) {
        std::string bytes = xlsx_detail::readAll(path);
        if (bytes.empty()) {
            if (err) *err = "Cannot read xlsx: " + path;
            return false;
        }
        std::map<std::string, std::string> parts;
        if (!xlsx_detail::unzip(bytes, parts, err)) return false;
        parts_ = parts;
        sheets_.clear();
        shared_.clear();
        dateStyle_.clear();

        auto it = parts_.find("xl/sharedStrings.xml");
        if (it != parts_.end()) parseShared(it->second);
        it = parts_.find("xl/styles.xml");
        if (it != parts_.end()) parseStyles(it->second);

        auto wb = parts_.find("xl/workbook.xml");
        if (wb == parts_.end()) {
            if (err) *err = "Missing xl/workbook.xml";
            return false;
        }
        std::map<std::string, std::string> rels;
        auto relIt = parts_.find("xl/_rels/workbook.xml.rels");
        if (relIt != parts_.end()) {
            const std::string& r = relIt->second;
            size_t p = 0;
            while ((p = xlsx_detail::findTag(r, "Relationship", p)) != std::string::npos) {
                size_t e = r.find('>', p);
                if (e == std::string::npos) break;
                std::string tag = r.substr(p, e - p + 1);
                std::string id = xlsx_detail::attr(tag, "Id");
                std::string target = xlsx_detail::attr(tag, "Target");
                if (!id.empty() && !target.empty()) rels[id] = target;
                p = e + 1;
            }
        }
        const std::string& w = wb->second;
        size_t p = 0;
        while ((p = xlsx_detail::findTag(w, "sheet", p)) != std::string::npos) {
            size_t e = w.find('>', p);
            if (e == std::string::npos) break;
            std::string tag = w.substr(p, e - p + 1);
            std::string name = xlsx_detail::xmlUnescape(xlsx_detail::attr(tag, "name"));
            std::string rid = xlsx_detail::attr(tag, "r:id");
            if (rid.empty()) rid = xlsx_detail::attr(tag, "id");
            std::string target;
            auto ri = rels.find(rid);
            if (ri != rels.end()) target = ri->second;
            std::string part;
            if (!target.empty()) {
                if (target[0] == '/') part = target.substr(1);
                else if (target.compare(0, 3, "xl/") == 0) part = target;
                else part = "xl/" + target;
            }
            Sheet sh;
            sh.name = name.empty() ? ("Sheet" + std::to_string(sheets_.size() + 1)) : name;
            sh.part = part;
            auto pi = parts_.find(part);
            if (pi != parts_.end()) parseSheet(pi->second, sh.rows);
            sheets_.push_back(sh);
            p = e + 1;
        }
        if (sheets_.empty()) {
            if (err) *err = "No sheets found in workbook";
            return false;
        }
        cur_ = 0;
        return true;
    }

    bool Save(const std::string& path, std::string* err = nullptr) const {
        std::vector<std::pair<std::string, std::string>> out;
        if (parts_.empty()) {
            buildNewParts(out);
        } else {
            for (const auto& kv : parts_) {
                bool replaced = false;
                for (const Sheet& sh : sheets_) {
                    if (sh.part == kv.first && !sh.part.empty()) {
                        out.emplace_back(kv.first, buildSheetXml(sh));
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) out.emplace_back(kv.first, kv.second);
            }
            for (const Sheet& sh : sheets_) {
                bool found = false;
                for (const auto& kv : out) if (kv.first == sh.part) { found = true; break; }
                if (!found) out.emplace_back(sh.part, buildSheetXml(sh));
            }
        }
        return xlsx_detail::zipParts(out, path, err);
    }

    // -----------------------------------------------------------------------
    // 工作表
    // -----------------------------------------------------------------------
    size_t sheetCount() const { return sheets_.size(); }

    std::vector<std::string> sheetNames() const {
        std::vector<std::string> v;
        for (const Sheet& s : sheets_) v.push_back(s.name);
        return v;
    }

    bool selectSheet(size_t index) {
        if (index >= sheets_.size()) return false;
        cur_ = index;
        return true;
    }

    bool selectSheet(const std::string& name) {
        for (size_t i = 0; i < sheets_.size(); ++i)
            if (sheets_[i].name == name) { cur_ = i; return true; }
        return false;
    }

    size_t currentSheet() const { return cur_; }

    bool addSheet(const std::string& name) {
        Sheet sh;
        sh.name = safeSheetName(name);
        sh.part = "xl/worksheets/sheet" + std::to_string(sheets_.size() + 1) + ".xml";
        sheets_.push_back(sh);
        return true;
    }

    // -----------------------------------------------------------------------
    // 读取单元格（0-based）
    // -----------------------------------------------------------------------
    size_t rowCount() const {
        if (sheets_.empty()) return 0;
        return sheets_[cur_].rows.size();
    }

    size_t colCount(size_t row) const {
        if (sheets_.empty() || row >= sheets_[cur_].rows.size()) return 0;
        return sheets_[cur_].rows[row].size();
    }

    Cell getCell(size_t row, size_t col) const {
        static const Cell emptyCell;
        if (sheets_.empty() || row >= sheets_[cur_].rows.size()) return emptyCell;
        const auto& r = sheets_[cur_].rows[row];
        if (col >= r.size()) return emptyCell;
        return r[col];
    }

    std::string getString(size_t row, size_t col) const {
        return getCell(row, col).text;
    }

    double getNumber(size_t row, size_t col, double def = 0) const {
        Cell c = getCell(row, col);
        if (c.type == Cell::Number) return c.number;
        if (c.type == Cell::String || c.type == Cell::Formula) {
            // 兼容“文本数字”：去掉逗号、元、等号、百分号
            std::string s;
            for (char ch : c.text) {
                if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+')
                    s += ch;
                else if (ch == '%') { s += ch; break; }
            }
            if (!s.empty() && s != "-" && s != ".") {
                char* endp = nullptr;
                double v = std::strtod(s.c_str(), &endp);
                if (endp && *endp == '\0') return s.find('%') != std::string::npos ? v / 100.0 : v;
            }
        }
        return def;
    }

    // -----------------------------------------------------------------------
    // 写入单元格（0-based，自动扩展行列）
    // -----------------------------------------------------------------------
    void setCell(size_t row, size_t col, const Cell& c) {
        if (sheets_.empty()) addSheet("Sheet1");
        auto& rows = sheets_[cur_].rows;
        if (rows.size() <= row) rows.resize(row + 1);
        if (rows[row].size() <= col) rows[row].resize(col + 1);
        rows[row][col] = c;
    }

    void setString(size_t row, size_t col, const std::string& v) {
        Cell c; c.type = Cell::String; c.text = v;
        setCell(row, col, c);
    }

    void setNumber(size_t row, size_t col, double v) {
        Cell c; c.type = Cell::Number; c.number = v; c.text = xlsx_detail::formatNumber(v);
        setCell(row, col, c);
    }

    void setBool(size_t row, size_t col, bool v) {
        Cell c; c.type = Cell::Bool; c.boolean = v; c.text = v ? "TRUE" : "FALSE";
        setCell(row, col, c);
    }

private:
    // -----------------------------------------------------------------------
    // 解析
    // -----------------------------------------------------------------------
    void parseShared(const std::string& xml) {
        size_t p = 0;
        while ((p = xlsx_detail::findTag(xml, "si", p)) != std::string::npos) {
            size_t e = xml.find("</si>", p);
            if (e == std::string::npos) break;
            std::string block = xml.substr(p, e - p + 5);
            std::string text;
            size_t q = 0;
            while ((q = xlsx_detail::findTag(block, "t", q)) != std::string::npos) {
                size_t qe = block.find('>', q);
                if (qe == std::string::npos) break;
                size_t qc = block.find("</t>", qe + 1);
                if (qc == std::string::npos) break;
                text += xlsx_detail::xmlUnescape(block.substr(qe + 1, qc - qe - 1));
                q = qc + 4;
            }
            shared_.push_back(text);
            p = e + 5;
        }
    }

    void parseStyles(const std::string& xml) {
        std::map<int, std::string> custom;
        size_t p = 0;
        while ((p = xlsx_detail::findTag(xml, "numFmt", p)) != std::string::npos) {
            size_t e = xml.find('>', p);
            if (e == std::string::npos) break;
            std::string tag = xml.substr(p, e - p + 1);
            int id = std::atoi(xlsx_detail::attr(tag, "numFmtId").c_str());
            custom[id] = xlsx_detail::lower(xlsx_detail::xmlUnescape(xlsx_detail::attr(tag, "formatCode")));
            p = e + 1;
        }
        dateStyle_.clear();
        size_t xfSec = xlsx_detail::findTag(xml, "cellXfs");
        if (xfSec == std::string::npos) return;
        size_t xfEnd = xml.find("</cellXfs>", xfSec);
        if (xfEnd == std::string::npos) xfEnd = xml.size();
        p = xfSec;
        while ((p = xlsx_detail::findTag(xml, "xf", p)) != std::string::npos && p < xfEnd) {
            size_t e = xml.find('>', p);
            if (e == std::string::npos) break;
            if (xml[e - 1] != '/') { p = e + 1; continue; }
            std::string tag = xml.substr(p, e - p + 1);
            int id = std::atoi(xlsx_detail::attr(tag, "numFmtId").c_str());
            bool date = false;
            if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) ||
                (id >= 45 && id <= 47) || (id >= 50 && id <= 58)) date = true;
            auto ci = custom.find(id);
            if (ci != custom.end()) {
                std::string f = ci->second;
                std::string cleaned;
                bool inQuote = false, inBracket = false;
                for (char ch : f) {
                    if (ch == '"') { inQuote = !inQuote; continue; }
                    if (ch == '[') { inBracket = true; continue; }
                    if (ch == ']') { inBracket = false; continue; }
                    if (!inQuote && !inBracket) cleaned += ch;
                }
                if (cleaned.find('y') != std::string::npos ||
                    cleaned.find('d') != std::string::npos ||
                    cleaned.find("h:m") != std::string::npos ||
                    cleaned.find("m:s") != std::string::npos) date = true;
            }
            dateStyle_.push_back(date);
            p = e + 1;
        }
    }

    void parseSheet(const std::string& xml, std::vector<std::vector<Cell>>& rows) {
        rows.clear();
        size_t p = 0;
        size_t nextRow = 0;
        while ((p = xlsx_detail::findTag(xml, "c", p)) != std::string::npos) {
            size_t gt = xml.find('>', p);
            if (gt == std::string::npos) break;
            std::string tag = xml.substr(p, gt - p + 1);
            std::string ref = xlsx_detail::attr(tag, "r");
            std::string type = xlsx_detail::attr(tag, "t");
            int style = std::atoi(xlsx_detail::attr(tag, "s").c_str());
            if (gt > 0 && xml[gt - 1] == '/') { p = gt + 1; continue; }
            size_t close = xml.find("</c>", gt + 1);
            if (close == std::string::npos) break;
            std::string body = xml.substr(gt + 1, close - gt - 1);

            size_t row = nextRow, col = 0;
            if (!xlsx_detail::cellRef(ref, row, col)) {
                // 没有 r 属性：按行内顺序递增，行号用最近的 <row r="..">
                size_t rowTag = xml.rfind("<row", p);
                if (rowTag != std::string::npos) {
                    size_t rgt = xml.find('>', rowTag);
                    if (rgt != std::string::npos && rgt < p) {
                        std::string rt = xml.substr(rowTag, rgt - rowTag + 1);
                        std::string rv = xlsx_detail::attr(rt, "r");
                        if (!rv.empty()) row = static_cast<size_t>(std::atoi(rv.c_str()) - 1);
                        col = 0;
                        for (size_t q = rgt + 1; q < p; ++q)
                            if (xlsx_detail::findTag(xml.substr(0, q), "c", q - 1) != std::string::npos) ++col;
                    }
                }
                col = 0;
            }
            Cell cell;
            cell.style = style;
            std::string f = xlsx_detail::tagContent(body, "f");
            if (!f.empty()) { cell.formula = f; cell.type = Cell::Formula; }
            std::string v = xlsx_detail::tagContent(body, "v");
            if (type == "s") {
                size_t idx = static_cast<size_t>(std::strtoul(v.c_str(), nullptr, 10));
                cell.type = Cell::String;
                cell.text = (idx < shared_.size()) ? shared_[idx] : std::string();
            } else if (type == "inlineStr") {
                std::string text;
                size_t q = 0;
                while ((q = xlsx_detail::findTag(body, "t", q)) != std::string::npos) {
                    size_t qe = body.find('>', q);
                    if (qe == std::string::npos) break;
                    size_t qc = body.find("</t>", qe + 1);
                    if (qc == std::string::npos) break;
                    text += xlsx_detail::xmlUnescape(body.substr(qe + 1, qc - qe - 1));
                    q = qc + 4;
                }
                cell.type = Cell::String;
                cell.text = text;
            } else if (type == "b") {
                cell.type = Cell::Bool;
                cell.boolean = (v == "1");
                cell.text = cell.boolean ? "TRUE" : "FALSE";
            } else if (type == "e") {
                cell.type = Cell::Error;
                cell.text = v;
            } else if (type == "str") {
                cell.type = Cell::Formula;
                cell.text = v;
            } else {
                cell.type = Cell::Number;
                cell.number = std::strtod(v.c_str(), nullptr);
                bool isDate = (style >= 0 && static_cast<size_t>(style) < dateStyle_.size() &&
                               dateStyle_[static_cast<size_t>(style)]);
                if (isDate) {
                    cell.text = xlsx_detail::serialToDate(cell.number);
                } else {
                    cell.text = v;
                }
            }
            while (rows.size() <= row) rows.emplace_back();
            if (rows[row].size() <= col) rows[row].resize(col + 1);
            rows[row][col] = cell;
            nextRow = row;
            p = close + 4;
        }
    }

    // -----------------------------------------------------------------------
    // 生成 XML
    // -----------------------------------------------------------------------
    static std::string safeSheetName(std::string s) {
        for (char& c : s)
            if (std::string("[]:*?/\\").find(c) != std::string::npos) c = '_';
        if (s.size() > 31) s.resize(31);
        if (s.empty()) s = "Sheet";
        return s;
    }

    std::string buildSheetXml(const Sheet& sh) const {
        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<sheetData>";
        for (size_t r = 0; r < sh.rows.size(); ++r) {
            xml += "<row r=\"" + std::to_string(r + 1) + "\">";
            for (size_t c = 0; c < sh.rows[r].size(); ++c) {
                const Cell& cell = sh.rows[r][c];
                if (cell.type == Cell::Empty && cell.text.empty()) continue;
                std::string ref = xlsx_detail::colName(c) + std::to_string(r + 1);
                std::string sAttr = cell.style ? (" s=\"" + std::to_string(cell.style) + "\"") : std::string();
                xml += "<c r=\"" + ref + "\"" + sAttr;
                if (cell.type == Cell::String) {
                    xml += " t=\"inlineStr\"><is><t xml:space=\"preserve\">" +
                           xlsx_detail::xmlEscape(cell.text) + "</t></is></c>";
                } else if (cell.type == Cell::Bool) {
                    xml += " t=\"b\"><v>" + std::string(cell.boolean ? "1" : "0") + "</v></c>";
                } else if (cell.type == Cell::Error) {
                    xml += " t=\"e\"><v>" + xlsx_detail::xmlEscape(cell.text) + "</v></c>";
                } else if (cell.type == Cell::Formula) {
                    xml += "><f>" + xlsx_detail::xmlEscape(cell.formula) + "</f><v>" +
                           xlsx_detail::xmlEscape(cell.text) + "</v></c>";
                } else {
                    double v = cell.number;
                    if (!cell.text.empty() && v == 0) v = std::strtod(cell.text.c_str(), nullptr);
                    xml += "><v>" + xlsx_detail::formatNumber(v) + "</v></c>";
                }
            }
            xml += "</row>";
        }
        xml += "</sheetData></worksheet>";
        return xml;
    }

    void buildNewParts(std::vector<std::pair<std::string, std::string>>& out) const {
        const std::string nsMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
        const std::string nsRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
        const std::string nsPkgRel = "http://schemas.openxmlformats.org/package/2006/relationships";
        const std::string nsCT = "http://schemas.openxmlformats.org/package/2006/content-types";

        std::string contentTypes =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Types xmlns=\"" + nsCT + "\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";
        std::string wb =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<workbook xmlns=\"" + nsMain + "\" xmlns:r=\"" + nsRel + "\"><sheets>";
        std::string wbRel =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"" + nsPkgRel + "\">";
        for (size_t i = 0; i < sheets_.size(); ++i) {
            std::string n = std::to_string(i + 1);
            std::string sheetPart = "xl/worksheets/sheet" + n + ".xml";
            contentTypes += "<Override PartName=\"/" + sheetPart +
                "\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
            wb += "<sheet name=\"" + xlsx_detail::xmlEscape(safeSheetName(sheets_[i].name)) +
                  "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
            wbRel += "<Relationship Id=\"rId" + n + "\" Type=\"" + nsRel +
                     "/worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
        }
        contentTypes +=
            "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
            "</Types>";
        wb += "</sheets></workbook>";
        wbRel += "</Relationships>";

        std::string rootRel =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"" + nsPkgRel + "\">"
            "<Relationship Id=\"rId1\" Type=\"" + nsRel +
            "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";
        std::string styles =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<styleSheet xmlns=\"" + nsMain + "\">"
            "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
            "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
            "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
            "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
            "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
            "<cellXfs count=\"2\">"
            "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
            "<xf numFmtId=\"14\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
            "</cellXfs>"
            "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
            "</styleSheet>";

        out.emplace_back("[Content_Types].xml", contentTypes);
        out.emplace_back("_rels/.rels", rootRel);
        out.emplace_back("xl/workbook.xml", wb);
        out.emplace_back("xl/_rels/workbook.xml.rels", wbRel);
        out.emplace_back("xl/styles.xml", styles);
        for (size_t i = 0; i < sheets_.size(); ++i) {
            Sheet sh = sheets_[i];
            sh.part = "xl/worksheets/sheet" + std::to_string(i + 1) + ".xml";
            out.emplace_back(sh.part, buildSheetXml(sh));
        }
    }
};

} // namespace io
} // namespace newrpa
