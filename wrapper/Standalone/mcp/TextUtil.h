#pragma once

// Line grammar and JSON emission for the command channel.
//
// Commands come in as shell-style token lines (`--set-param 3 0.75`) and go
// out as one JSON object per line. That asymmetry is SynthEditCL's, and it is
// deliberate: a JSON *parser* in the app would be a dependency and an attack
// surface for a channel whose entire input vocabulary is a few dozen verbs,
// while JSON *output* is what the MCP host wants to read. Quoting rules match
// `--script -`, so a line that works there works here.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace gmpi
{
namespace standalone
{
namespace mcp
{

/// Splits on whitespace, honouring double quotes so a value containing spaces
/// survives ("Audio Out"). A backslash escapes the next character inside
/// quotes, which is the only way to get a literal quote through.
inline std::vector<std::string> tokenize(std::string_view line)
{
    std::vector<std::string> out;
    std::string current;
    bool inQuotes = false;
    bool have = false;   // distinguishes "" (an empty argument) from no argument

    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];

        if (inQuotes && c == '\\' && i + 1 < line.size())
        {
            current += line[++i];
            continue;
        }
        if (c == '"')
        {
            inQuotes = !inQuotes;
            have = true;
            continue;
        }
        if (!inQuotes && (c == ' ' || c == '\t'))
        {
            if (have || !current.empty())
                out.push_back(current);
            current.clear();
            have = false;
            continue;
        }
        current += c;
    }

    if (have || !current.empty())
        out.push_back(current);

    return out;
}

inline std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

/// A number JSON can actually carry.
///
/// NaN and the infinities have no JSON literal, and printing them bare emits
/// `nan` / `inf`, which is not JSON at all - the client's parse throws and the
/// whole response is lost rather than the one bad field. A plugin that reports
/// a broken value is exactly when the tooling most needs to keep working, so
/// they become null and stay visible.
inline std::string jsonNumber(double v)
{
    if (v != v || v > 1e308 || v < -1e308)
        return "null";

    char buf[40];
    snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

/// Builds one JSON object, in order, without a DOM. Every response is a flat
/// bag of fields, so this is all the structure that is ever needed.
class JsonObject
{
public:
    JsonObject() { out_ = "{"; }

    JsonObject& str(std::string_view key, std::string_view value)
    {
        sep();
        out_ += '"'; out_ += jsonEscape(key); out_ += "\":\"";
        out_ += jsonEscape(value);
        out_ += '"';
        return *this;
    }

    JsonObject& num(std::string_view key, double value)
    {
        sep();
        out_ += '"'; out_ += jsonEscape(key); out_ += "\":";
        out_ += jsonNumber(value);
        return *this;
    }

    JsonObject& boolean(std::string_view key, bool value)
    {
        sep();
        out_ += '"'; out_ += jsonEscape(key); out_ += "\":";
        out_ += value ? "true" : "false";
        return *this;
    }

    /// For arrays and nested objects the caller has already rendered.
    JsonObject& raw(std::string_view key, std::string_view json)
    {
        sep();
        out_ += '"'; out_ += jsonEscape(key); out_ += "\":";
        out_ += json;
        return *this;
    }

    std::string done()
    {
        out_ += '}';
        return out_;
    }

private:
    void sep()
    {
        if (out_.size() > 1)
            out_ += ',';
    }

    std::string out_;
};

inline std::string okLine(std::string_view cmd)
{
    return JsonObject().str("cmd", cmd).boolean("ok", true).done();
}

inline std::string errorLine(std::string_view cmd, std::string_view what)
{
    return JsonObject().str("cmd", cmd).boolean("ok", false).str("error", what).done();
}

/// strtod that insists on consuming the WHOLE token.
///
/// Bare strtod("x") returns 0.0 and reports nothing, so a typo'd parameter
/// value would silently set the parameter to zero - a wrong answer that looks
/// like a right one, which is the worst failure this channel can have.
inline bool parseDouble(const std::string& s, double& out)
{
    if (s.empty())
        return false;

    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size())
        return false;

    out = v;
    return true;
}

inline bool parseInt(const std::string& s, int& out)
{
    double v = 0.0;
    if (!parseDouble(s, v))
        return false;
    if (v < -2147483648.0 || v > 2147483647.0)
        return false;

    out = static_cast<int>(v);
    return true;
}

/// "x,y" as one token, which is how the pointer verbs take coordinates -
/// matching SynthEditCL's `--drag 3984,4010 3984,3950`.
inline bool parsePoint(const std::string& s, float& x, float& y)
{
    const size_t comma = s.find(',');
    if (comma == std::string::npos)
        return false;

    double dx = 0.0, dy = 0.0;
    if (!parseDouble(s.substr(0, comma), dx) || !parseDouble(s.substr(comma + 1), dy))
        return false;

    x = static_cast<float>(dx);
    y = static_cast<float>(dy);
    return true;
}

} // namespace mcp
} // namespace standalone
} // namespace gmpi
