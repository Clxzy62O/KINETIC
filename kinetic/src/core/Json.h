#pragma once
// Minimal, dependency-free JSON DOM. Object key order is preserved so saved projects diff cleanly.
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kinetic {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v_(b) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(int64_t i) : v_(static_cast<double>(i)) {}
    Json(uint64_t i) : v_(static_cast<double>(i)) {}
    Json(double d) : v_(d) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(JsonArray a) : v_(std::make_shared<JsonArray>(std::move(a))) {}
    Json(JsonObject o) : v_(std::make_shared<JsonObject>(std::move(o))) {}

    static Json array() { return Json(JsonArray{}); }
    static Json object() { return Json(JsonObject{}); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool() const;
    double asNumber() const;
    int64_t asInt() const;
    const std::string& asString() const;
    const JsonArray& asArray() const;
    JsonArray& asArray();
    const JsonObject& asObject() const;
    JsonObject& asObject();

    // Object access
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const; // throws if missing
    Json& set(const std::string& key, Json value);        // insert or replace, returns *this
    // Typed getters with defaults (for forward-compatible loading)
    double num(const std::string& key, double def) const;
    int64_t integer(const std::string& key, int64_t def) const;
    bool boolean(const std::string& key, bool def) const;
    std::string str(const std::string& key, const std::string& def) const;
    const Json* find(const std::string& key) const;
    Json* findMut(const std::string& key);

    // Array helpers
    void push(Json v);
    size_t size() const;
    const Json& at(size_t i) const;

    std::string dump(int indent = -1) const;
    static Json parse(const std::string& text);

    bool operator==(const Json& o) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>> v_;
    // Arrays/objects are copy-on-write-free shared; ensure deep copies on copy for value semantics.
public:
    Json(const Json& o);
    Json& operator=(const Json& o);
    Json(Json&&) noexcept = default;
    Json& operator=(Json&&) noexcept = default;
};

} // namespace kinetic
