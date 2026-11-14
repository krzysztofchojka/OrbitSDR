#pragma once

// Tiny JSON reader (enough for the SatNOGS transmitter list): objects, arrays,
// strings, numbers, true / false / null. No external libraries needed.

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>

namespace orbit {

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    const JsonValue* get(const std::string& k) const {
        if (type != Object) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    std::string getString(const std::string& k, const std::string& def = "") const {
        const JsonValue* v = get(k);
        return (v && v->type == String) ? v->str : def;
    }
    double getNumber(const std::string& k, double def = 0.0) const {
        const JsonValue* v = get(k);
        return (v && v->type == Number) ? v->num : def;
    }
    bool getBool(const std::string& k, bool def = false) const {
        const JsonValue* v = get(k);
        return (v && v->type == Bool) ? v->b : def;
    }
    bool isNull(const std::string& k) const {
        const JsonValue* v = get(k);
        return !v || v->type == Null;
    }
};

class JsonParser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }

    static void putUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': break;
                    case 'b': case 'f': break;
                    case 'u': {
                        if (i + 4 > s.size()) return false;
                        unsigned cp = (unsigned)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        putUtf8(out, cp);
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }

    bool parseValue(JsonValue& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        bool ok = true;
        if (c == '{') {
            v.type = JsonValue::Object;
            i++; ws();
            if (i < s.size() && s[i] == '}') { i++; }
            else for (;;) {
                ws();
                std::string key;
                if (!parseString(key)) { ok = false; break; }
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                i++;
                JsonValue child;
                if (!parseValue(child)) { ok = false; break; }
                v.obj[key] = std::move(child);
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.type = JsonValue::Array;
            i++; ws();
            if (i < s.size() && s[i] == ']') { i++; }
            else for (;;) {
                JsonValue child;
                if (!parseValue(child)) { ok = false; break; }
                v.arr.push_back(std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v.type = JsonValue::String;
            ok = parseString(v.str);
        } else if (s.compare(i, 4, "true") == 0) { v.type = JsonValue::Bool; v.b = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { v.type = JsonValue::Bool; v.b = false; i += 5; }
        else if (s.compare(i, 4, "null") == 0) { v.type = JsonValue::Null; i += 4; }
        else {
            char* end = nullptr;
            v.type = JsonValue::Number;
            v.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) ok = false;
            else i = (size_t)(end - s.c_str());
        }
        depth--;
        return ok;
    }

public:
    explicit JsonParser(const std::string& text) : s(text) {}
    bool parse(JsonValue& out) { i = 0; depth = 0; return parseValue(out); }
};

inline bool parseJson(const std::string& text, JsonValue& out) {
    JsonParser p(text);
    return p.parse(out);
}

} // namespace orbit
