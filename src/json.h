// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  json.h - 极简 JSON 读写（够用即可：会话配置的持久化）
//  字符串一律存 UTF-8；对象内部保持插入顺序，字段顺序稳定，便于人工查看。
// ===========================================================================
#pragma once

#include <string>
#include <vector>
#include <utility>

class Json {
public:
    enum class T { Null, Bool, Num, Str, Arr, Obj };

    T           type = T::Null;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;
    std::vector<Json>                          arr;
    std::vector<std::pair<std::string, Json>>  obj;

    Json() = default;

    static Json Obj();
    static Json Arr();
    static Json Str(const std::string& s);
    static Json Num(double d);
    static Json Bool(bool v);

    bool IsNull() const { return type == T::Null; }

    // ---- 对象写入（不存在则追加，存在则覆盖）----
    Json& Set(const std::string& key, const Json& v);
    Json& Set(const std::string& key, const std::string& v);
    Json& Set(const std::string& key, const char* v);       // 必须显式提供：
    Json& Set(const std::string& key, const wchar_t* v);    // 否则 Set(k,"x") 会走 bool 重载
    Json& Set(const std::string& key, int v);
    Json& Set(const std::string& key, double v);
    Json& Set(const std::string& key, bool v);

    // ---- 对象读取（缺失或类型不符时返回默认值）----
    const Json* Find(const std::string& key) const;
    std::string GetStr(const std::string& key, const std::string& def = std::string()) const;
    int         GetInt(const std::string& key, int def = 0) const;
    double      GetNum(const std::string& key, double def = 0.0) const;
    bool        GetBool(const std::string& key, bool def = false) const;
    const std::vector<Json>* GetArr(const std::string& key) const;

    std::string Dump(int indent = 2) const;
    static bool Parse(const std::string& text, Json& out, std::string* err = nullptr);

private:
    void DumpTo(std::string& out, int indent, int depth) const;
};
