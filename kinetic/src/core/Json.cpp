#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace kinetic {

Json::Json(const Json& o) {
    switch (o.type()) {
    case Type::Array: v_ = std::make_shared<JsonArray>(*std::get<std::shared_ptr<JsonArray>>(o.v_)); break;
    case Type::Object: v_ = std::make_shared<JsonObject>(*std::get<std::shared_ptr<JsonObject>>(o.v_)); break;
    default: v_ = o.v_; break;
    }
}
Json& Json::operator=(const Json& o) {
    if (this != &o) { Json tmp(o); *this = std::move(tmp); }
    return *this;
}

static const char* typeName(Json::Type t) {
    switch (t) {
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return "bool";
    case Json::Type::Number: return "number";
    case Json::Type::String: return "string";
    case Json::Type::Array: return "array";
    case Json::Type::Object: return "object";
    }
    return "?";
}
#define KJ_EXPECT(cond, want) \
    if (!(cond)) throw JsonError(std::string("JSON: expected ") + want + ", got " + typeName(type()))

bool Json::asBool() const { KJ_EXPECT(isBool(), "bool"); return std::get<bool>(v_); }
double Json::asNumber() const { KJ_EXPECT(isNumber(), "number"); return std::get<double>(v_); }
int64_t Json::asInt() const { return static_cast<int64_t>(std::llround(asNumber())); }
const std::string& Json::asString() const { KJ_EXPECT(isString(), "string"); return std::get<std::string>(v_); }
const JsonArray& Json::asArray() const { KJ_EXPECT(isArray(), "array"); return *std::get<std::shared_ptr<JsonArray>>(v_); }
JsonArray& Json::asArray() { KJ_EXPECT(isArray(), "array"); return *std::get<std::shared_ptr<JsonArray>>(v_); }
const JsonObject& Json::asObject() const { KJ_EXPECT(isObject(), "object"); return *std::get<std::shared_ptr<JsonObject>>(v_); }
JsonObject& Json::asObject() { KJ_EXPECT(isObject(), "object"); return *std::get<std::shared_ptr<JsonObject>>(v_); }

const Json* Json::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    for (auto& kv : asObject()) if (kv.first == key) return &kv.second;
    return nullptr;
}
Json* Json::findMut(const std::string& key) {
    if (!isObject()) return nullptr;
    for (auto& kv : asObject()) if (kv.first == key) return &kv.second;
    return nullptr;
}
bool Json::has(const std::string& key) const { return find(key) != nullptr; }
const Json& Json::operator[](const std::string& key) const {
    const Json* j = find(key);
    if (!j) throw JsonError("JSON: missing key '" + key + "'");
    return *j;
}
Json& Json::set(const std::string& key, Json value) {
    if (isNull()) *this = object();
    auto& obj = asObject();
    for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(value); return *this; }
    obj.emplace_back(key, std::move(value));
    return *this;
}
double Json::num(const std::string& k, double def) const { auto* j = find(k); return j && j->isNumber() ? j->asNumber() : def; }
int64_t Json::integer(const std::string& k, int64_t def) const { auto* j = find(k); return j && j->isNumber() ? j->asInt() : def; }
bool Json::boolean(const std::string& k, bool def) const { auto* j = find(k); return j && j->isBool() ? j->asBool() : def; }
std::string Json::str(const std::string& k, const std::string& def) const { auto* j = find(k); return j && j->isString() ? j->asString() : def; }

void Json::push(Json v) { if (isNull()) *this = array(); asArray().push_back(std::move(v)); }
size_t Json::size() const { return isArray() ? asArray().size() : isObject() ? asObject().size() : 0; }
const Json& Json::at(size_t i) const { return asArray().at(i); }

bool Json::operator==(const Json& o) const {
    if (type() != o.type()) return false;
    switch (type()) {
    case Type::Null: return true;
    case Type::Bool: return asBool() == o.asBool();
    case Type::Number: return asNumber() == o.asNumber();
    case Type::String: return asString() == o.asString();
    case Type::Array: return asArray() == o.asArray();
    case Type::Object: return asObject() == o.asObject();
    }
    return false;
}

// ---------------------------------------------------------------- writer
static void escapeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
}

static void formatNumber(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "null"; return; }
    if (std::fabs(d) < 9.0e15 && d == std::floor(d)) {
        char buf[32]; std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d)); out += buf; return;
    }
    // Shortest representation that round-trips.
    char buf[40];
    for (int prec = 6; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    out += buf;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += asBool() ? "true" : "false"; break;
    case Type::Number: formatNumber(out, asNumber()); break;
    case Type::String: escapeString(out, asString()); break;
    case Type::Array: {
        const auto& a = asArray();
        if (a.empty()) { out += "[]"; break; }
        // Keep short numeric arrays on one line for readability.
        bool flat = a.size() <= 8;
        for (auto& e : a) if (!e.isNumber()) { flat = false; break; }
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += flat || indent < 0 ? (indent < 0 ? "," : ", ") : ",";
            if (!flat) nl(depth + 1);
            a[i].dumpTo(out, indent, depth + 1);
        }
        if (!flat) nl(depth);
        out += ']';
        break;
    }
    case Type::Object: {
        const auto& o = asObject();
        if (o.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < o.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            escapeString(out, o[i].first);
            out += indent < 0 ? ":" : ": ";
            o[i].second.dumpTo(out, indent, depth + 1);
        }
        nl(depth);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- parser
namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw JsonError("JSON parse error at offset " + std::to_string(i) + ": " + msg);
    }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        return false;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    uint32_t hex4() {
        if (i + 4 > s.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("bad hex digit");
        }
        return v;
    }
    std::string str() {
        if (s[i] != '"') fail("expected string");
        ++i;
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) fail("bad escape");
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (!(i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')) fail("lone surrogate");
                    i += 2;
                    uint32_t lo = hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
        return out;
    }
    Json value() {
        ws();
        if (i >= s.size()) fail("unexpected end");
        if (++depth > 256) fail("nesting too deep");
        Json result;
        char c = s[i];
        if (c == '{') {
            ++i;
            result = Json::object();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; }
            else {
                while (true) {
                    ws();
                    std::string key = str();
                    ws();
                    if (i >= s.size() || s[i] != ':') fail("expected ':'");
                    ++i;
                    result.asObject().emplace_back(std::move(key), value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            ++i;
            result = Json::array();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; }
            else {
                while (true) {
                    result.asArray().push_back(value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            result = Json(str());
        } else if (lit("true")) result = Json(true);
        else if (lit("false")) result = Json(false);
        else if (lit("null")) result = Json(nullptr);
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
            std::string tok = s.substr(start, i - start);
            char* end = nullptr;
            double d = std::strtod(tok.c_str(), &end);
            if (end != tok.c_str() + tok.size()) fail("bad number '" + tok + "'");
            result = Json(d);
        } else fail(std::string("unexpected character '") + c + "'");
        --depth;
        return result;
    }
};
} // namespace

Json Json::parse(const std::string& text) {
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing characters");
    return v;
}

} // namespace kinetic
