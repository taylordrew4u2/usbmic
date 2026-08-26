#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace mma {

/// Minimal self-contained JSON value type, used only for session.json (§6.2).
/// Written in-tree rather than vendoring nlohmann::json because this sandbox
/// has no network access to fetch a third-party header reliably; the schema
/// needed here is small and fully covered by this implementation. See README
/// for this judgment call.
class JsonValue
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type (Type::Null) {}
    JsonValue (bool b) : type (Type::Bool), boolValue (b) {}
    JsonValue (double n) : type (Type::Number), numberValue (n) {}
    JsonValue (int n) : type (Type::Number), numberValue (n) {}
    JsonValue (const char* s) : type (Type::String), stringValue (s) {}
    JsonValue (std::string s) : type (Type::String), stringValue (std::move (s)) {}

    static JsonValue makeArray() { JsonValue v; v.type = Type::Array; return v; }
    static JsonValue makeObject() { JsonValue v; v.type = Type::Object; return v; }

    Type getType() const { return type; }
    bool isNull() const { return type == Type::Null; }

    void push_back (JsonValue v) { arrayValue.push_back (std::move (v)); }

    JsonValue& operator[] (const std::string& key)
    {
        type = Type::Object;
        for (auto& kv : objectValue)
            if (kv.first == key)
                return kv.second;
        objectValue.emplace_back (key, JsonValue());
        return objectValue.back().second;
    }

    const JsonValue* find (const std::string& key) const
    {
        for (auto& kv : objectValue)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    bool asBool (bool def = false) const { return type == Type::Bool ? boolValue : def; }
    double asDouble (double def = 0.0) const { return type == Type::Number ? numberValue : def; }
    int asInt (int def = 0) const { return type == Type::Number ? static_cast<int> (numberValue) : def; }
    std::string asString (const std::string& def = {}) const { return type == Type::String ? stringValue : def; }
    const std::vector<JsonValue>& asArray() const { return arrayValue; }

    std::string dump (int indent = 2) const { std::string out; dumpImpl (out, indent, 0); return out; }

    static JsonValue parse (const std::string& text)
    {
        size_t pos = 0;
        skipWhitespace (text, pos);
        JsonValue v = parseValue (text, pos);
        return v;
    }

private:
    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::vector<std::pair<std::string, JsonValue>> objectValue;

    static void skipWhitespace (const std::string& s, size_t& pos)
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    static std::string parseString (const std::string& s, size_t& pos)
    {
        std::string out;
        ++pos; // opening quote
        while (pos < s.size() && s[pos] != '"')
        {
            char c = s[pos];
            if (c == '\\' && pos + 1 < s.size())
            {
                char next = s[pos + 1];
                switch (next)
                {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    default: out += next; break;
                }
                pos += 2;
            }
            else
            {
                out += c;
                ++pos;
            }
        }
        ++pos; // closing quote
        return out;
    }

    static JsonValue parseValue (const std::string& s, size_t& pos)
    {
        skipWhitespace (s, pos);
        if (pos >= s.size())
            return JsonValue();

        char c = s[pos];
        if (c == '"')
            return JsonValue (parseString (s, pos));

        if (c == '{')
        {
            JsonValue obj = JsonValue::makeObject();
            ++pos;
            skipWhitespace (s, pos);
            if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
            while (pos < s.size())
            {
                skipWhitespace (s, pos);
                std::string key = parseString (s, pos);
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ':') ++pos;
                JsonValue value = parseValue (s, pos);
                obj[key] = value;
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == '}') { ++pos; break; }
                break;
            }
            return obj;
        }

        if (c == '[')
        {
            JsonValue arr = JsonValue::makeArray();
            ++pos;
            skipWhitespace (s, pos);
            if (pos < s.size() && s[pos] == ']') { ++pos; return arr; }
            while (pos < s.size())
            {
                JsonValue value = parseValue (s, pos);
                arr.push_back (value);
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == ']') { ++pos; break; }
                break;
            }
            return arr;
        }

        if (s.compare (pos, 4, "true") == 0) { pos += 4; return JsonValue (true); }
        if (s.compare (pos, 5, "false") == 0) { pos += 5; return JsonValue (false); }
        if (s.compare (pos, 4, "null") == 0) { pos += 4; return JsonValue(); }

        // Number.
        size_t start = pos;
        while (pos < s.size() && (isdigit (static_cast<unsigned char> (s[pos])) || s[pos] == '-' || s[pos] == '+'
                                   || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E'))
            ++pos;
        if (pos == start)
            throw std::runtime_error ("JsonValue::parse: unexpected character");
        return JsonValue (std::stod (s.substr (start, pos - start)));
    }

    static void appendIndent (std::string& out, int indent, int depth)
    {
        if (indent > 0)
            out.append (static_cast<size_t> (indent * depth), ' ');
    }

    static void escapeInto (std::string& out, const std::string& s)
    {
        out += '"';
        for (char c : s)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default: out += c; break;
            }
        }
        out += '"';
    }

    void dumpImpl (std::string& out, int indent, int depth) const
    {
        const bool pretty = indent > 0;
        switch (type)
        {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += boolValue ? "true" : "false"; break;
            case Type::Number:
            {
                std::ostringstream oss;
                if (numberValue == static_cast<long long> (numberValue))
                    oss << static_cast<long long> (numberValue);
                else
                    oss << numberValue;
                out += oss.str();
                break;
            }
            case Type::String: escapeInto (out, stringValue); break;
            case Type::Array:
            {
                out += '[';
                if (pretty && ! arrayValue.empty()) out += '\n';
                for (size_t i = 0; i < arrayValue.size(); ++i)
                {
                    appendIndent (out, indent, depth + 1);
                    arrayValue[i].dumpImpl (out, indent, depth + 1);
                    if (i + 1 < arrayValue.size()) out += ',';
                    if (pretty) out += '\n';
                }
                if (pretty && ! arrayValue.empty()) appendIndent (out, indent, depth);
                out += ']';
                break;
            }
            case Type::Object:
            {
                out += '{';
                if (pretty && ! objectValue.empty()) out += '\n';
                for (size_t i = 0; i < objectValue.size(); ++i)
                {
                    appendIndent (out, indent, depth + 1);
                    escapeInto (out, objectValue[i].first);
                    out += pretty ? ": " : ":";
                    objectValue[i].second.dumpImpl (out, indent, depth + 1);
                    if (i + 1 < objectValue.size()) out += ',';
                    if (pretty) out += '\n';
                }
                if (pretty && ! objectValue.empty()) appendIndent (out, indent, depth);
                out += '}';
                break;
            }
        }
    }
};

} // namespace mma
