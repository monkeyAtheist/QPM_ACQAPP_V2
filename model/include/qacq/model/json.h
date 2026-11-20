#pragma once

// A small JSON value for the session files and the settings of the modules, independent of Qt so the
// whole model can be tested without it.
//
// - Objects keep the order of their members (the files stay readable and diff well).
// - Numbers are doubles, written with the shortest text that reads back to the same double
//   (std::to_chars): integers up to 2^53 come out as integers. NaN and infinities have no JSON form and
//   are written as null.
// - parse() follows RFC 8259 strictly (no comments, no trailing commas, no leading zeros), decodes
//   \uXXXX escapes including surrogate pairs into UTF-8, and refuses nesting deeper than 256 levels.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace qacq::model
{
class Json;
struct JsonMember;

class Json
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    using Array = std::vector<Json>;
    using Object = std::vector<JsonMember>;

    // Defined below, once JsonMember is complete (Clang and MSVC instantiate the variant eagerly).
    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(int value);
    Json(unsigned value);
    Json(long value);
    Json(unsigned long value);
    Json(long long value);
    Json(unsigned long long value);
    Json(double value);
    Json(const char *value);
    Json(std::string value);
    Json(std::string_view value);
    Json(Array value);
    Json(Object value);

    static Json array();
    static Json object();

    Type type() const noexcept { return static_cast<Type>(m_value.index()); }
    bool isNull() const noexcept { return type() == Type::Null; }
    bool isBool() const noexcept { return type() == Type::Bool; }
    bool isNumber() const noexcept { return type() == Type::Number; }
    bool isString() const noexcept { return type() == Type::String; }
    bool isArray() const noexcept { return type() == Type::Array; }
    bool isObject() const noexcept { return type() == Type::Object; }

    // Read access with a fallback when the type differs.
    bool toBool(bool fallback = false) const noexcept;
    double toNumber(double fallback = 0.0) const noexcept;
    std::string toString(std::string fallback = {}) const;
    const Array &items() const noexcept;    // empty unless an array
    const Object &members() const noexcept; // empty unless an object

    // Objects. The const form returns null for a missing key; the other one inserts it (a null value
    // becomes an empty object first).
    const Json &operator[](std::string_view key) const noexcept;
    Json &operator[](std::string_view key);
    bool contains(std::string_view key) const noexcept;
    bool erase(std::string_view key);

    // Arrays (push_back turns a null value into an empty array first). at() returns null out of range.
    void push_back(Json value);
    const Json &at(std::size_t index) const noexcept;

    std::size_t size() const noexcept; // members of an object, items of an array, else 0

    // indent < 0: compact on one line; otherwise pretty-printed with that many spaces per level.
    std::string dump(int indent = -1) const;
    static std::optional<Json> parse(std::string_view text, std::string *error = nullptr);

    bool operator==(const Json &other) const;
    bool operator!=(const Json &other) const { return !(*this == other); }

private:
    void write(std::string &out, int indent, int level) const;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_value;
};

struct JsonMember
{
    std::string key;
    Json value;

    bool operator==(const JsonMember &other) const { return key == other.key && value == other.value; }
};

inline Json::Json() : m_value(nullptr) {}
inline Json::Json(std::nullptr_t) : m_value(nullptr) {}
inline Json::Json(bool value) : m_value(value) {}
inline Json::Json(int value) : m_value(static_cast<double>(value)) {}
inline Json::Json(unsigned value) : m_value(static_cast<double>(value)) {}
inline Json::Json(long value) : m_value(static_cast<double>(value)) {}
inline Json::Json(unsigned long value) : m_value(static_cast<double>(value)) {}
inline Json::Json(long long value) : m_value(static_cast<double>(value)) {}
inline Json::Json(unsigned long long value) : m_value(static_cast<double>(value)) {}
inline Json::Json(double value) : m_value(value) {}
inline Json::Json(const char *value) : m_value(std::string(value)) {}
inline Json::Json(std::string value) : m_value(std::move(value)) {}
inline Json::Json(std::string_view value) : m_value(std::string(value)) {}

// Shortest round-trip text of a double ("0.1", "1e-07", "42"); "null" for NaN and infinities.
std::string formatNumber(double value);
} // namespace qacq::model
