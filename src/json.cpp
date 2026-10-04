// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  json.cpp
// ===========================================================================
#include "json.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
//  构造
// ---------------------------------------------------------------------------
Json Json::Obj() { Json j; j.type = T::Obj; return j; }
Json Json::Arr() { Json j; j.type = T::Arr; return j; }

Json Json::Str(const std::string& s) {
    Json j; j.type = T::Str; j.str = s; return j;
}
Json Json::Num(double d) {
    Json j; j.type = T::Num; j.num = d; return j;
}
Json Json::Bool(bool v) {
    Json j; j.type = T::Bool; j.b = v; return j;
}

// ---------------------------------------------------------------------------
//  写入
// ---------------------------------------------------------------------------
Json& Json::Set(const std::string& key, const Json& v) {
    if (type != T::Obj) { type = T::Obj; }
    for (auto& kv : obj) {
        if (kv.first == key) { kv.second = v; return *this; }
    }
    obj.emplace_back(key, v);
    return *this;
}
Json& Json::Set(const std::string& key, const std::string& v) { return Set(key, Json::Str(v)); }
Json& Json::Set(const std::string& key, const char* v) {
    return Set(key, Json::Str(v ? std::string(v) : std::string()));
}
Json& Json::Set(const std::string& key, const wchar_t* v) {
    // 便捷重载：调用方传的是宽字符时按 UTF-8 存
    std::string u8;
    if (v) {
        int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            u8.resize((size_t)(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, v, -1, &u8[0], n, nullptr, nullptr);
        }
    }
    return Set(key, Json::Str(u8));
}
Json& Json::Set(const std::string& key, int v) { return Set(key, Json::Num((double)v)); }
Json& Json::Set(const std::string& key, double v) { return Set(key, Json::Num(v)); }
Json& Json::Set(const std::string& key, bool v) { return Set(key, Json::Bool(v)); }

// ---------------------------------------------------------------------------
//  读取
// ---------------------------------------------------------------------------
const Json* Json::Find(const std::string& key) const {
    if (type != T::Obj) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string Json::GetStr(const std::string& key, const std::string& def) const {
    const Json* j = Find(key);
    if (!j) return def;
    if (j->type == T::Str) return j->str;
    return def;
}

int Json::GetInt(const std::string& key, int def) const {
    const Json* j = Find(key);
    if (!j) return def;
    if (j->type == T::Num) return (int)llround(j->num);
    if (j->type == T::Bool) return j->b ? 1 : 0;
    return def;
}

double Json::GetNum(const std::string& key, double def) const {
    const Json* j = Find(key);
    if (!j) return def;
    if (j->type == T::Num) return j->num;
    return def;
}

bool Json::GetBool(const std::string& key, bool def) const {
    const Json* j = Find(key);
    if (!j) return def;
    if (j->type == T::Bool) return j->b;
    if (j->type == T::Num) return j->num != 0.0;
    return def;
}

const std::vector<Json>* Json::GetArr(const std::string& key) const {
    const Json* j = Find(key);
    if (!j || j->type != T::Arr) return nullptr;
    return &j->arr;
}

// ---------------------------------------------------------------------------
//  序列化
// ---------------------------------------------------------------------------
static void EscapeTo(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

static void NumTo(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "0"; return; }
    // 整数值直接输出成整数，避免 sessions.json 里出现 "port": 22.0
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", (long long)d);
        out += buf;
        return;
    }
    char buf[40];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.17g", d);
    out += buf;
}

void Json::DumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    const std::string pad    = pretty ? std::string((size_t)(indent * (depth + 1)), ' ') : std::string();
    const std::string padEnd = pretty ? std::string((size_t)(indent * depth), ' ')       : std::string();

    switch (type) {
    case T::Null: out += "null"; break;
    case T::Bool: out += b ? "true" : "false"; break;
    case T::Num:  NumTo(out, num); break;
    case T::Str:  EscapeTo(out, str); break;

    case T::Arr: {
        if (arr.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out += pad; }
            arr[i].DumpTo(out, indent, depth + 1);
        }
        if (pretty) { out += '\n'; out += padEnd; }
        out += ']';
        break;
    }

    case T::Obj: {
        if (obj.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < obj.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out += pad; }
            EscapeTo(out, obj[i].first);
            out += ':';
            if (pretty) out += ' ';
            obj[i].second.DumpTo(out, indent, depth + 1);
        }
        if (pretty) { out += '\n'; out += padEnd; }
        out += '}';
        break;
    }
    }
}

std::string Json::Dump(int indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
//  解析
// ---------------------------------------------------------------------------
namespace {

struct Parser {
    const char* p;
    const char* end;
    const char* begin = nullptr;
    std::string err;

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool Fail(const char* what) {
        if (err.empty()) {
            char buf[160];
            int off = begin ? (int)(p - begin) : -1;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (offset %d)", what, off);
            err = buf;
        }
        return false;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(uint32_t& v) {
        if (end - p < 4) return Fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = p[i];
            uint32_t d;
            if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return Fail("bad hex in \\u escape");
            v = (v << 4) | d;
        }
        p += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        if (p >= end || *p != '"') return Fail("expected string");
        ++p;
        out.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { ++p; return true; }
            if (c == '\\') {
                ++p;
                if (p >= end) return Fail("bad escape");
                char e = *p++;
                switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        const char* save = p;
                        p += 2;
                        uint32_t lo = 0;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p = save;   // 不是合法低位代理，原样处理
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("unknown escape");
                }
            } else {
                out += (char)c;
                ++p;
            }
        }
        return Fail("unterminated string");
    }

    bool ParseValue(Json& out) {
        SkipWs();
        if (p >= end) return Fail("unexpected end");

        char c = *p;
        if (c == '{') {
            ++p;
            out = Json::Obj();
            SkipWs();
            if (p < end && *p == '}') { ++p; return true; }
            while (true) {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (p >= end || *p != ':') return Fail("expected ':'");
                ++p;
                Json v;
                if (!ParseValue(v)) return false;
                out.obj.emplace_back(key, v);
                SkipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            out = Json::Arr();
            SkipWs();
            if (p < end && *p == ']') { ++p; return true; }
            while (true) {
                Json v;
                if (!ParseValue(v)) return false;
                out.arr.push_back(v);
                SkipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!ParseString(s)) return false;
            out = Json::Str(s);
            return true;
        }
        if (end - p >= 4 && strncmp(p, "true", 4) == 0)  { p += 4; out = Json::Bool(true);  return true; }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; out = Json::Bool(false); return true; }
        if (end - p >= 4 && strncmp(p, "null", 4) == 0)  { p += 4; out = Json();            return true; }

        // 数字
        {
            const char* start = p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            bool any = false;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                               *p == '-' || *p == '+')) { ++p; any = true; }
            if (!any) return Fail("invalid value");
            std::string tmp(start, (size_t)(p - start));
            out = Json::Num(strtod(tmp.c_str(), nullptr));
            return true;
        }
    }
};

} // namespace

bool Json::Parse(const std::string& text, Json& out, std::string* err) {
    // 跳过 UTF-8 BOM
    const char* begin = text.c_str();
    size_t len = text.size();
    if (len >= 3 && (unsigned char)begin[0] == 0xEF &&
        (unsigned char)begin[1] == 0xBB && (unsigned char)begin[2] == 0xBF) {
        begin += 3;
        len   -= 3;
    }

    Parser ps;
    ps.p     = begin;
    ps.end   = begin + len;
    ps.begin = begin;

    Json tmp;
    if (!ps.ParseValue(tmp)) {
        if (err) *err = ps.err;
        return false;
    }
    ps.SkipWs();
    if (ps.p != ps.end) {
        if (err) *err = "trailing garbage after value";
        return false;
    }
    out = tmp;
    return true;
}
