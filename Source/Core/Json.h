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

    /// How many key/value pairs this object holds. Zero for anything that is
    /// not an object. This parser is deliberately lenient -- it returns a value
    /// rather than throwing on input it cannot make sense of -- so a caller that
    /// needs to tell "a file I could read" from "a file I could not" has to ask
    /// whether anything actually came out of it.
    size_t getMemberCount() const { return objectValue.size(); }

    /// Members that actually carry a value. A member whose value is a literal
    /// null counts as nothing read. (A member the input was cut inside is no
    /// longer stored at all -- see parse().)
    size_t getValuedMemberCount() const
    {
        size_t count = 0;

        for (const auto& kv : objectValue)
            if (kv.second.getType() != Type::Null)
                ++count;

        return count;
    }

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

    /// Input that stops part way -- a file cut short by a power cut mid-write --
    /// still yields every member that finished before the cut, but never the
    /// one the input stopped inside: half a string, a number that may have
    /// had more digits, or an object missing its end would all read as real
    /// values. `truncated`, when given, says whether that happened.
    static JsonValue parse (const std::string& text, bool* truncated = nullptr)
    {
        size_t pos = 0;
        bool cut = false;
        skipWhitespace (text, pos);
        JsonValue v = parseValue (text, pos, cut);
        if (truncated != nullptr) *truncated = cut;
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

    static std::string parseString (const std::string& s, size_t& pos, bool& cut)
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
        // No closing quote: the input ended inside this string.
        if (pos >= s.size()) { cut = true; return out; }
        ++pos; // closing quote
        return out;
    }

    // Sets `cut` when the input ends before this value does. A cut member or
    // element is dropped by its container rather than stored, and the cut is
    // passed up so every enclosing container knows it ended early too. A cut
    // array keeps the elements it finished -- a list cut short is a shorter
    // list -- but a cut object inside a container is dropped whole, since a
    // record missing its tail (a port without its name) is not one to trust.
    static JsonValue parseValue (const std::string& s, size_t& pos, bool& cut)
    {
        skipWhitespace (s, pos);
        if (pos >= s.size())
        {
            cut = true;
            return JsonValue();
        }

        char c = s[pos];
        if (c == '"')
            return JsonValue (parseString (s, pos, cut));

        if (c == '{')
        {
            JsonValue obj = JsonValue::makeObject();
            ++pos;
            skipWhitespace (s, pos);
            if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
            while (pos < s.size())
            {
                skipWhitespace (s, pos);
                std::string key = parseString (s, pos, cut);
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ':') ++pos;
                JsonValue value = parseValue (s, pos, cut);
                if (cut)
                {
                    if (value.type == Type::Array) obj[key] = value;
                    return obj;
                }
                obj[key] = value;
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == '}') { ++pos; return obj; }
                break;
            }
            // Ran out of input before the closing brace.
            if (pos >= s.size()) cut = true;
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
                JsonValue value = parseValue (s, pos, cut);
                if (cut)
                {
                    if (value.type == Type::Array) arr.push_back (value);
                    return arr;
                }
                arr.push_back (value);
                skipWhitespace (s, pos);
                if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
                if (pos < s.size() && s[pos] == ']') { ++pos; return arr; }
                break;
            }
            // Ran out of input before the closing bracket.
            if (pos >= s.size()) cut = true;
            return arr;
        }

        if (s.compare (pos, 4, "true") == 0) { pos += 4; return JsonValue (true); }
        if (s.compare (pos, 5, "false") == 0) { pos += 5; return JsonValue (false); }
        if (s.compare (pos, 4, "null") == 0) { pos += 4; return JsonValue(); }

        // The start of true/false/null running into the end of the input.
        for (const char* word : { "true", "false", "null" })
            if (s.size() - pos < std::char_traits<char>::length (word) && s.compare (pos, std::string::npos, word, s.size() - pos) == 0)
            {
                pos = s.size();
                cut = true;
                return JsonValue();
            }

        // Number.
        size_t start = pos;
        while (pos < s.size() && (isdigit (static_cast<unsigned char> (s[pos])) || s[pos] == '-' || s[pos] == '+'
                                   || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E'))
            ++pos;
        if (pos == start)
            throw std::runtime_error ("JsonValue::parse: unexpected character");
        // A number that runs to the very end of the input may have lost digits.
        // Harmless for a document that is just a number, but inside a container
        // the end of input here means the cut fell in or right after it.
        if (pos >= s.size()) cut = true;
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
