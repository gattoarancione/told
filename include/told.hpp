// TOLD (Typo Lenght Data) v1.0 -- header-only C++ wrapper over the C ABI in told.h.
//
// It adds the three things the C interface cannot express, and nothing else:
//
//   * lifetimes: the encoder handle and every container's header are closed by
//     destructors, so a leaked encoder or a begin without its matching end is not a
//     mistake you can make here;
//   * iteration: containers are ranges, so `for (auto field : frame.object())` replaces
//     the manual have/next loop;
//   * types: accessors return values and throw on mismatch, rather than filling an out
//     parameter and returning a status to check.
//
// Zero-copy still holds all the way up: `asString`/`asBuffer`/`raw` are views into the
// buffer you passed to `decode`, valid exactly as long as it is. Nothing is copied.
//
// Requires C++20 (std::span). Throws on error, so if you build with -fno-exceptions use
// the C API directly.

#ifndef TOLD_HPP
#define TOLD_HPP

#include "told.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace told {

using Status = told_status;
using Bytes = std::span<const std::uint8_t>;

enum class Type : std::uint8_t {
    null = TOLD_NULL,
    boolean = TOLD_BOOL,
    tinyuint = TOLD_TINYUINT,
    int8 = TOLD_INT8,
    int16 = TOLD_INT16,
    int32 = TOLD_INT32,
    int64 = TOLD_INT64,
    float32 = TOLD_FLOAT32,
    float64 = TOLD_FLOAT64,
    string = TOLD_STRING,
    array = TOLD_ARRAY,
    object = TOLD_OBJECT,
    buffer = TOLD_BUFFER,
    extended = TOLD_EXTENDED,
};

/// Carries the status so callers can branch on the cause, not just the message.
class Error : public std::runtime_error {
public:
    explicit Error(Status status)
        : std::runtime_error(std::string{"told: "} + told_status_name(status)), status_{status} {}
    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Status status_;
};

inline void check(Status status) {
    if (status != TOLD_OK) throw Error{status};
}

class ArrayIterator;
class ObjectIterator;
class ArrayRange;
class ObjectRange;

/// A decoded frame. Copyable and cheap: it is a type, a mod and two borrowed spans.
class Frame {
public:
    Frame() = default;
    explicit Frame(const told_frame& raw) noexcept : f_{raw} {}

    [[nodiscard]] Type type() const noexcept { return static_cast<Type>(f_.type); }
    [[nodiscard]] std::uint8_t mod() const noexcept { return f_.mod; }
    [[nodiscard]] std::size_t size() const noexcept { return f_.raw_len; }
    [[nodiscard]] bool isNull() const noexcept { return type() == Type::null; }

    /// The whole frame, prefix included -- what to write to forward it verbatim.
    [[nodiscard]] Bytes raw() const noexcept { return {f_.raw, f_.raw_len}; }

    /// The extension type code, present only on EXTENDED frames.
    [[nodiscard]] std::optional<std::uint8_t> extensionCode() const noexcept {
        if (f_.ext_code < 0) return std::nullopt;
        return static_cast<std::uint8_t>(f_.ext_code);
    }

    [[nodiscard]] bool asBool() const {
        bool out{};
        check(told_as_bool(&f_, &out));
        return out;
    }

    [[nodiscard]] std::int64_t asInt() const {
        std::int64_t out{};
        check(told_as_i64(&f_, &out));
        return out;
    }

    [[nodiscard]] std::uint64_t asUint() const {
        std::uint64_t out{};
        check(told_as_u64(&f_, &out));
        return out;
    }

    [[nodiscard]] double asFloat() const {
        double out{};
        check(told_as_f64(&f_, &out));
        return out;
    }

    /// UTF-8 validated, and a view into the decoded buffer.
    [[nodiscard]] std::string_view asString() const {
        const std::uint8_t* ptr{};
        std::size_t len{};
        check(told_as_string(&f_, &ptr, &len));
        return {reinterpret_cast<const char*>(ptr), len};
    }

    /// Opaque bytes, no encoding contract -- also a view, not a copy.
    [[nodiscard]] Bytes asBuffer() const {
        const std::uint8_t* ptr{};
        std::size_t len{};
        check(told_as_buffer(&f_, &ptr, &len));
        return {ptr, len};
    }

    [[nodiscard]] ArrayRange array() const;
    [[nodiscard]] ObjectRange object() const;

    [[nodiscard]] const told_frame& native() const noexcept { return f_; }

private:
    friend class ArrayIterator;
    friend class ObjectIterator;
    told_frame f_{};
};

struct Field {
    std::string_view key;
    Frame value;
};

/// Input iterator over an ARRAY's children. Elements are decoded on demand, so walking
/// a large array costs nothing until it is read.
class ArrayIterator {
public:
    using value_type = Frame;
    using difference_type = std::ptrdiff_t;

    ArrayIterator() = default; // end sentinel
    explicit ArrayIterator(told_iter it) : it_{it} { advance(); }

    const Frame& operator*() const noexcept { return current_; }
    const Frame* operator->() const noexcept { return &current_; }
    ArrayIterator& operator++() {
        advance();
        return *this;
    }
    void operator++(int) { advance(); }
    bool operator==(const ArrayIterator& other) const noexcept { return have_ == other.have_; }

private:
    void advance() {
        bool have = false;
        check(told_array_next(&it_, &current_.f_, &have));
        have_ = have;
    }

    told_iter it_{};
    Frame current_{};
    bool have_ = false;
};

/// Input iterator over an OBJECT's key/value pairs, in wire order.
class ObjectIterator {
public:
    using value_type = Field;
    using difference_type = std::ptrdiff_t;

    ObjectIterator() = default; // end sentinel
    explicit ObjectIterator(told_iter it) : it_{it} { advance(); }

    const Field& operator*() const noexcept { return current_; }
    const Field* operator->() const noexcept { return &current_; }
    ObjectIterator& operator++() {
        advance();
        return *this;
    }
    void operator++(int) { advance(); }
    bool operator==(const ObjectIterator& other) const noexcept { return have_ == other.have_; }

private:
    void advance() {
        const std::uint8_t* key{};
        std::size_t key_len{};
        bool have = false;
        check(told_object_next(&it_, &key, &key_len, &current_.value.f_, &have));
        current_.key = {reinterpret_cast<const char*>(key), have ? key_len : 0};
        have_ = have;
    }

    told_iter it_{};
    Field current_{};
    bool have_ = false;
};

class ArrayRange {
public:
    explicit ArrayRange(told_iter it) noexcept : it_{it} {}
    [[nodiscard]] ArrayIterator begin() const { return ArrayIterator{it_}; }
    [[nodiscard]] ArrayIterator end() const noexcept { return {}; }

private:
    told_iter it_;
};

class ObjectRange {
public:
    explicit ObjectRange(told_iter it) noexcept : it_{it} {}
    [[nodiscard]] ObjectIterator begin() const { return ObjectIterator{it_}; }
    [[nodiscard]] ObjectIterator end() const noexcept { return {}; }

private:
    told_iter it_;
};

inline ArrayRange Frame::array() const {
    told_iter it{};
    check(told_array_iter(&f_, &it));
    return ArrayRange{it};
}

inline ObjectRange Frame::object() const {
    told_iter it{};
    check(told_object_iter(&f_, &it));
    return ObjectRange{it};
}

/// Decode the frame at the start of `bytes`, which may extend past it.
[[nodiscard]] inline Frame decode(Bytes bytes) {
    told_frame frame{};
    check(told_decode(bytes.data(), bytes.size(), &frame));
    return Frame{frame};
}

/// Size of the frame at the start of `bytes`, or nullopt when the prefix is too short to
/// tell — the distinction a stream reader needs, expressed in the return type rather than
/// as a status to remember.
[[nodiscard]] inline std::optional<std::size_t> frameSize(Bytes bytes) {
    std::size_t size{};
    const Status status = told_frame_size(bytes.data(), bytes.size(), &size);
    if (status == TOLD_INCOMPLETE) return std::nullopt;
    check(status);
    return size;
}

/// Walk the whole tree: nested frames, UTF-8 in strings, object shape. Use on untrusted
/// input; `decode` only checks the outermost frame.
inline void validate(Bytes bytes, std::size_t max_depth = 32) {
    check(told_validate(bytes.data(), bytes.size(), max_depth));
}

/// Owns the encode buffer. Move-only, because two owners would double-free the handle.
class Encoder {
public:
    Encoder() : handle_{told_encoder_create()} {
        if (handle_ == nullptr) throw std::bad_alloc{};
    }

    ~Encoder() { told_encoder_destroy(handle_); }

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    Encoder(Encoder&& other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}
    Encoder& operator=(Encoder&& other) noexcept {
        if (this != &other) {
            told_encoder_destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    /// Closes its container when it goes out of scope, so a begin cannot be left open.
    class Scope {
    public:
        Scope(Encoder& enc, std::size_t mark, bool is_object) noexcept
            : enc_{&enc}, mark_{mark}, is_object_{is_object} {}

        ~Scope() {
            // A throwing destructor would turn a failed close into a terminate during
            // unwinding, so this one swallows. Call `close()` first where it matters;
            // the only realistic failure is allocation.
            if (enc_ != nullptr) closeQuietly();
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept
            : enc_{std::exchange(other.enc_, nullptr)}, mark_{other.mark_}, is_object_{other.is_object_} {}
        Scope& operator=(Scope&&) = delete;

        /// Close now and report failure. Safe to call once; the destructor then does
        /// nothing.
        void close() {
            Encoder* enc = std::exchange(enc_, nullptr);
            if (enc == nullptr) return;
            check(is_object_ ? told_end_object(enc->handle_, mark_)
                             : told_end_array(enc->handle_, mark_));
        }

    private:
        void closeQuietly() noexcept {
            if (is_object_) {
                (void)told_end_object(enc_->handle_, mark_);
            } else {
                (void)told_end_array(enc_->handle_, mark_);
            }
            enc_ = nullptr;
        }

        Encoder* enc_;
        std::size_t mark_;
        bool is_object_;
    };

    [[nodiscard]] Scope beginArray() {
        std::size_t mark{};
        check(told_begin_array(handle_, &mark));
        return Scope{*this, mark, false};
    }

    [[nodiscard]] Scope beginObject() {
        std::size_t mark{};
        check(told_begin_object(handle_, &mark));
        return Scope{*this, mark, true};
    }

    // Deliberately NOT a set of `write` overloads. A string literal binds to `bool`
    // ahead of `std::string_view` -- pointer-to-bool is a standard conversion and beats
    // a user-defined one -- so `write("text")` would silently encode `true`. That is a
    // data-corruption bug with no diagnostic, and it happened here before these names
    // existed. Integer literals have the matching problem in reverse, being ambiguous
    // between the signed, unsigned and floating overloads. Explicit names cost a few
    // characters and remove the whole class of error.
    void writeNull() { check(told_write_null(handle_)); }
    void writeBool(bool value) { check(told_write_bool(handle_, value)); }
    void writeUint(std::uint64_t value) { check(told_write_u64(handle_, value)); }
    void writeInt(std::int64_t value) { check(told_write_i64(handle_, value)); }
    void writeFloat32(float value) { check(told_write_f32(handle_, value)); }
    void writeFloat(double value) { check(told_write_f64(handle_, value)); }

    void writeString(std::string_view text) {
        check(told_write_string(handle_, reinterpret_cast<const std::uint8_t*>(text.data()),
                                    text.size()));
    }

    /// Named for readability at call sites; an object key is just a STRING frame.
    void key(std::string_view name) { writeString(name); }

    void writeBuffer(Bytes bytes) {
        check(told_write_buffer(handle_, bytes.data(), bytes.size()));
    }

    void writeExtended(std::uint8_t code, Bytes payload) {
        check(told_write_extended(handle_, code, payload.data(), payload.size()));
    }

    /// Borrowed: copy before the next write, which may reallocate.
    [[nodiscard]] Bytes bytes() const noexcept {
        std::size_t len{};
        const std::uint8_t* ptr = told_encoder_bytes(handle_, &len);
        return {ptr, len};
    }

    /// Drop the contents, keep the capacity, for reusing one encoder per message.
    void reset() noexcept { told_encoder_reset(handle_); }

private:
    friend class Scope;
    told_encoder* handle_;
};

// ---------------------------------------------------------------- struct mapping
//
// Packing a C++ object straight to TOLD and back, the way MSGPACK_DEFINE does for msgpack.
// C++ has no reflection to lean on, so a class lists its own members once:
//
//     struct Reading {
//         std::uint8_t channel;
//         double value;
//         bool valid;
//         TOLD_DEFINE_MAP(channel, value, valid)   // object, names as keys
//     };
//
//     struct Point {
//         double x, y;
//         TOLD_DEFINE(x, y)                        // array, positional and compact
//     };
//
// then packs and unpacks with no further ceremony:
//
//     std::vector<std::uint8_t> bytes = told::pack(reading);
//     auto back = told::unpack<Reading>(bytes);
//
// _MAP costs the key bytes and tolerates fields being added or reordered; _DEFINE is
// smaller and positional, so both ends must agree on the order. Same choice, and the same
// trade, as msgpack's two macros.
//
// Built-in support covers bool, every integer and floating type, enums (as their
// underlying integer, which is what the Zig and Python sides emit too), std::string and
// std::string_view, std::vector<std::uint8_t> as a binary buffer, std::vector<T> and
// std::array<T,N> as arrays, std::optional<T> as null-or-value, std::map and
// std::unordered_map keyed by string as objects, and any type carrying either macro.
//
// Unknown keys in a frame are ignored, so a producer may add a field without breaking
// consumers built earlier. A key the frame does not carry leaves that member as the
// constructor left it.

namespace detail {

/// Resolves TOLD_BASE to a reference of the right constness, so the same macro text works
/// in both the const and non-const toldFields() overloads -- packing needs const, unpacking
/// needs mutable, and a plain static_cast could only satisfy one of them.
template <class Base, class Derived>
constexpr Base& toldBase(Derived& d) {
    return static_cast<Base&>(d);
}
template <class Base, class Derived>
constexpr const Base& toldBase(const Derived& d) {
    return static_cast<const Base&>(d);
}

} // namespace detail

/// Include a base class's members, as MSGPACK_BASE does. In the _MAP form the key is the
/// base's name; in the array form it is one nested element.
///
///     struct Derived : Base {
///         int extra;
///         TOLD_DEFINE_MAP(TOLD_BASE(Base), extra)
///     };
#define TOLD_BASE(Base) ::told::detail::toldBase<Base>(*this)

/// Give a member a wire key different from its C++ name, as MSGPACK_NVP does. Useful for
/// keeping keys short on a slow link without renaming the member.
///
///     TOLD_DEFINE_MAP(TOLD_NVP("ch", channel), TOLD_NVP("v", value))
#define TOLD_NVP(key, member) member

// Enums need no declaration here: any enum, scoped or plain, packs as its underlying
// integer already, chosen by that type's signedness. This macro exists so code being
// ported from msgpack -- where MSGPACK_ADD_ENUM is mandatory, because msgpack refuses an
// undeclared enum -- keeps compiling with the line left in place. It checks that its
// argument really is an enum and does nothing else.
//
// Use it at namespace scope, as MSGPACK_ADD_ENUM is used:
//
//     enum class Mode : int { idle, running };
//     TOLD_ADD_ENUM(Mode)
//
// The representation is the integer in every language here, so a frame written by C++ is
// read by Zig and Python without a shared name table. Zig additionally accepts a string,
// for frames written by hand.
#define TOLD_ADD_ENUM(EnumType)                                                       \
    static_assert(::std::is_enum_v<EnumType>,                                         \
                  "TOLD_ADD_ENUM: " #EnumType " is not an enum");

// The stringised member list is taken as a parameter rather than produced inside a shared
// body, because # only refuses to expand its operand in the macro that the user actually
// invoked. Forwarding __VA_ARGS__ through an alias and stringising it there captures the
// *expanded* text instead -- which loses TOLD_NVP's explicit key and turns TOLD_BASE into
// its own implementation detail. Each spelling below therefore stringises for itself
// and hands the text down.
#define TOLD_FIELDS_IMPL(named, names_text, ...)                                      \
    auto toldFields() { return ::std::forward_as_tuple(__VA_ARGS__); }                 \
    auto toldFields() const { return ::std::forward_as_tuple(__VA_ARGS__); }           \
    static constexpr bool told_named_fields = (named);                                 \
    static constexpr ::std::string_view toldFieldNames() { return names_text; }

#define TOLD_DEFINE(...) TOLD_FIELDS_IMPL(false, #__VA_ARGS__, __VA_ARGS__)
#define TOLD_DEFINE_ARRAY(...) TOLD_FIELDS_IMPL(false, #__VA_ARGS__, __VA_ARGS__)
#define TOLD_DEFINE_MAP(...) TOLD_FIELDS_IMPL(true, #__VA_ARGS__, __VA_ARGS__)

namespace detail {

/// The i-th name out of a stringised macro argument list ("a, b, c"), trimmed.
///
/// This is why the macros stringise their arguments: it is the only way to recover member
/// names in C++ without a reflection proposal that is not here yet. Evaluated at compile
/// time, so no parsing happens at run time.
constexpr std::string_view unwrapFieldName(std::string_view piece) {
    // TOLD_NVP("key", member) -> key. The explicit key wins over the member's own name.
    if (piece.starts_with("TOLD_NVP(")) {
        const std::size_t open = piece.find('"');
        if (open != std::string_view::npos) {
            const std::size_t close = piece.find('"', open + 1);
            if (close != std::string_view::npos) {
                return piece.substr(open + 1, close - open - 1);
            }
        }
        return piece;
    }
    // TOLD_BASE(Base) -> Base, so a base class does not land under a key spelling out the
    // macro call.
    if (piece.starts_with("TOLD_BASE(")) {
        const std::size_t open = piece.find('(');
        const std::size_t close = piece.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
            std::string_view inner = piece.substr(open + 1, close - open - 1);
            // Drop any qualification: ns::Base is keyed as Base.
            const std::size_t scope = inner.rfind(':');
            if (scope != std::string_view::npos) inner = inner.substr(scope + 1);
            return inner;
        }
    }
    return piece;
}

constexpr std::string_view nthFieldName(std::string_view all, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t i = 0;; ++i) {
        // Split on the first comma at depth zero: TOLD_NVP("k", m) contains one of its own.
        std::size_t comma = std::string_view::npos;
        int depth = 0;
        for (std::size_t j = start; j < all.size(); ++j) {
            const char c = all[j];
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == ',' && depth == 0) { comma = j; break; }
        }
        std::string_view piece = comma == std::string_view::npos
                                     ? all.substr(start)
                                     : all.substr(start, comma - start);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
            piece.remove_prefix(1);
        }
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
            piece.remove_suffix(1);
        }
        if (i == index) return unwrapFieldName(piece);
        if (comma == std::string_view::npos) return {};
        start = comma + 1;
    }
}

template <class T>
concept HasToldFields = requires(const T& value) { value.toldFields(); };

template <class T>
struct IsByteVector : std::false_type {};
template <class A>
struct IsByteVector<std::vector<std::uint8_t, A>> : std::true_type {};

template <class T>
struct IsVector : std::false_type {};
template <class E, class A>
struct IsVector<std::vector<E, A>> : std::true_type {};

template <class T>
struct IsStdArray : std::false_type {};
template <class E, std::size_t N>
struct IsStdArray<std::array<E, N>> : std::true_type {};

template <class T>
struct IsOptional : std::false_type {};
template <class E>
struct IsOptional<std::optional<E>> : std::true_type {};

template <class T>
struct IsStringMap : std::false_type {};
template <class V, class C, class A>
struct IsStringMap<std::map<std::string, V, C, A>> : std::true_type {};
template <class V, class H, class E, class A>
struct IsStringMap<std::unordered_map<std::string, V, H, E, A>> : std::true_type {};

} // namespace detail

/// Specialise this to teach the mapping about a type whose definition you cannot touch --
/// Qt's QString, a vendor's vector, a legacy struct that must not grow members. msgpack
/// calls the same idea an adaptor.
///
///     template <>
///     struct told::Adaptor<QString> {
///         static void pack(Encoder& enc, const QString& v) { enc.writeString(...); }
///         static void unpack(const Frame& f, QString& out) { out = ...; }
///     };
///
/// An adaptor is consulted before every built-in rule, so it can also override how a type
/// this header already knows about is encoded.
template <class T>
struct Adaptor;

template <class T>
void packValue(Encoder& enc, const T& value);

template <class T>
void unpackValue(const Frame& frame, T& out);

namespace detail {

template <class T>
concept HasPackAdaptor = requires(Encoder& enc, const T& value) {
    ::told::Adaptor<T>::pack(enc, value);
};

template <class T>
concept HasUnpackAdaptor = requires(const Frame& frame, T& out) {
    ::told::Adaptor<T>::unpack(frame, out);
};

} // namespace detail

namespace detail {

template <class T, std::size_t... I>
void packFields(Encoder& enc, const T& value, std::index_sequence<I...>) {
    auto fields = value.toldFields();
    if constexpr (T::told_named_fields) {
        constexpr std::string_view names = T::toldFieldNames();
        ((enc.writeString(nthFieldName(names, I)),
          packValue(enc, std::get<I>(fields))),
         ...);
    } else {
        (packValue(enc, std::get<I>(fields)), ...);
    }
}

template <class T, std::size_t... I>
void unpackNamedFields(const Frame& frame, T& out, std::index_sequence<I...>) {
    auto fields = out.toldFields();
    constexpr std::string_view names = T::toldFieldNames();
    for (const Field& field : frame.object()) {
        // A key matching no member is skipped rather than rejected, so a producer can add
        // one without every consumer being rebuilt first.
        ((field.key == nthFieldName(names, I)
              ? (unpackValue(field.value, std::get<I>(fields)), void())
              : void()),
         ...);
    }
}

template <class T, std::size_t... I>
void unpackPositionalFields(const Frame& frame, T& out, std::index_sequence<I...>) {
    auto fields = out.toldFields();
    auto range = frame.array();
    auto it = range.begin();
    const auto end = range.end();
    ((it == end ? throw Error(TOLD_TRUNCATED)
                : (unpackValue(*it, std::get<I>(fields)), ++it, void())),
     ...);
}

} // namespace detail

/// Encode any supported value, including a type carrying one of the macros.
template <class T>
void packValue(Encoder& enc, const T& value) {
    using U = std::remove_cvref_t<T>;

    if constexpr (detail::HasPackAdaptor<U>) {
        Adaptor<U>::pack(enc, value);
    } else if constexpr (std::is_same_v<U, bool>) {
        enc.writeBool(value);
    } else if constexpr (std::is_enum_v<U>) {
        // The underlying integer, matching what the Zig and Python sides write. A name
        // would need a table C++ cannot generate on its own.
        //
        // Via the underlying type, not straight to int64: an `enum class F : uint64_t`
        // with a value above INT64_MAX would otherwise be cast to a negative number and
        // come back wrong.
        using Underlying = std::underlying_type_t<U>;
        if constexpr (std::is_signed_v<Underlying>) {
            const auto raw = static_cast<std::int64_t>(value);
            if (raw >= 0) {
                enc.writeUint(static_cast<std::uint64_t>(raw));
            } else {
                enc.writeInt(raw);
            }
        } else {
            enc.writeUint(static_cast<std::uint64_t>(value));
        }
    } else if constexpr (std::is_integral_v<U>) {
        // Non-negative takes the unsigned encoding regardless of the C++ type's
        // signedness: it is a byte shorter for small values, reads back fine through
        // asInt, and matches what Zig and Python emit for the same field.
        if constexpr (std::is_signed_v<U>) {
            const auto raw = static_cast<std::int64_t>(value);
            if (raw >= 0) {
                enc.writeUint(static_cast<std::uint64_t>(raw));
            } else {
                enc.writeInt(raw);
            }
        } else {
            enc.writeUint(static_cast<std::uint64_t>(value));
        }
    } else if constexpr (std::is_same_v<U, float>) {
        enc.writeFloat32(value);
    } else if constexpr (std::is_floating_point_v<U>) {
        enc.writeFloat(static_cast<double>(value));
    } else if constexpr (std::is_same_v<U, std::string> ||
                         std::is_same_v<U, std::string_view>) {
        enc.writeString(value);
    } else if constexpr (detail::IsByteVector<U>::value) {
        // Bytes, not text: a vector<uint8_t> is the C++ way of saying binary.
        enc.writeBuffer(Bytes{value.data(), value.size()});
    } else if constexpr (detail::IsOptional<U>::value) {
        if (value.has_value()) {
            packValue(enc, *value);
        } else {
            enc.writeNull();
        }
    } else if constexpr (detail::IsVector<U>::value || detail::IsStdArray<U>::value) {
        auto scope = enc.beginArray();
        for (const auto& item : value) packValue(enc, item);
        scope.close();
    } else if constexpr (detail::IsStringMap<U>::value) {
        auto scope = enc.beginObject();
        for (const auto& [key, item] : value) {
            enc.writeString(key);
            packValue(enc, item);
        }
        scope.close();
    } else if constexpr (detail::HasToldFields<U>) {
        constexpr std::size_t count =
            std::tuple_size_v<decltype(std::declval<const U&>().toldFields())>;
        if constexpr (U::told_named_fields) {
            auto scope = enc.beginObject();
            detail::packFields(enc, value, std::make_index_sequence<count>{});
            scope.close();
        } else {
            auto scope = enc.beginArray();
            detail::packFields(enc, value, std::make_index_sequence<count>{});
            scope.close();
        }
    } else {
        static_assert(detail::HasToldFields<U>,
                      "told: type has no mapping. Add TOLD_DEFINE or "
                      "TOLD_DEFINE_MAP to it, specialise told::Adaptor for it, or "
                      "pack its members individually.");
    }
}

/// Decode into an existing value.
template <class T>
void unpackValue(const Frame& frame, T& out) {
    using U = std::remove_cvref_t<T>;

    if constexpr (detail::HasUnpackAdaptor<U>) {
        Adaptor<U>::unpack(frame, out);
    } else if constexpr (std::is_same_v<U, bool>) {
        out = frame.asBool();
    } else if constexpr (std::is_enum_v<U>) {
        using Underlying = std::underlying_type_t<U>;
        if constexpr (std::is_signed_v<Underlying>) {
            out = static_cast<U>(static_cast<Underlying>(frame.asInt()));
        } else {
            out = static_cast<U>(static_cast<Underlying>(frame.asUint()));
        }
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_signed_v<U>) {
            out = static_cast<U>(frame.asInt());
        } else {
            out = static_cast<U>(frame.asUint());
        }
    } else if constexpr (std::is_floating_point_v<U>) {
        out = static_cast<U>(frame.asFloat());
    } else if constexpr (std::is_same_v<U, std::string>) {
        // Either framing is accepted. A string is what pack writes for text, but a
        // producer in another language may have chosen a buffer for the same field, and
        // refusing that would be pedantry rather than safety.
        if (frame.type() == Type::string) {
            const std::string_view text = frame.asString();
            out.assign(text);
        } else {
            const Bytes raw = frame.asBuffer();
            out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        }
    } else if constexpr (detail::IsByteVector<U>::value) {
        if (frame.type() == Type::string) {
            const std::string_view text = frame.asString();
            const auto* first = reinterpret_cast<const std::uint8_t*>(text.data());
            out.assign(first, first + text.size());
        } else {
            const Bytes raw = frame.asBuffer();
            out.assign(raw.begin(), raw.end());
        }
    } else if constexpr (detail::IsOptional<U>::value) {
        if (frame.isNull()) {
            out.reset();
        } else {
            typename U::value_type inner{};
            unpackValue(frame, inner);
            out = std::move(inner);
        }
    } else if constexpr (detail::IsVector<U>::value) {
        out.clear();
        for (const Frame& item : frame.array()) {
            typename U::value_type element{};
            unpackValue(item, element);
            out.push_back(std::move(element));
        }
    } else if constexpr (detail::IsStdArray<U>::value) {
        std::size_t index = 0;
        for (const Frame& item : frame.array()) {
            if (index >= out.size()) throw Error(TOLD_MALFORMED_CONTAINER);
            unpackValue(item, out[index++]);
        }
        if (index != out.size()) throw Error(TOLD_TRUNCATED);
    } else if constexpr (detail::IsStringMap<U>::value) {
        out.clear();
        for (const Field& field : frame.object()) {
            typename U::mapped_type element{};
            unpackValue(field.value, element);
            out.emplace(std::string{field.key}, std::move(element));
        }
    } else if constexpr (detail::HasToldFields<U>) {
        constexpr std::size_t count =
            std::tuple_size_v<decltype(std::declval<const U&>().toldFields())>;
        if constexpr (U::told_named_fields) {
            detail::unpackNamedFields(frame, out, std::make_index_sequence<count>{});
        } else {
            detail::unpackPositionalFields(frame, out, std::make_index_sequence<count>{});
        }
    } else {
        static_assert(detail::HasToldFields<U>,
                      "told: type has no mapping. Add TOLD_DEFINE, TOLD_DEFINE_MAP, or a "
                      "told::Adaptor specialisation.");
    }
}

/// Pack a value into a fresh byte vector.
template <class T>
[[nodiscard]] std::vector<std::uint8_t> pack(const T& value) {
    Encoder enc;
    packValue(enc, value);
    const Bytes out = enc.bytes();
    return std::vector<std::uint8_t>{out.begin(), out.end()};
}

/// Pack into a caller-owned encoder, for putting a value inside a larger frame.
template <class T>
void packInto(Encoder& enc, const T& value) {
    packValue(enc, value);
}

/// Decode the frame at the start of `bytes` as a `T`.
template <class T>
[[nodiscard]] T unpack(Bytes bytes) {
    T out{};
    unpackValue(decode(bytes), out);
    return out;
}

template <class T>
[[nodiscard]] T unpack(const std::vector<std::uint8_t>& bytes) {
    return unpack<T>(Bytes{bytes.data(), bytes.size()});
}

template <class T>
[[nodiscard]] T unpack(const Frame& frame) {
    T out{};
    unpackValue(frame, out);
    return out;
}

/// What `unpack` yields, mirroring msgpack::object closely enough that call sites written
/// as `unpack(p, n).get().as<T>()` port by changing the namespace and nothing else.
class Object {
public:
    explicit Object(Frame frame) noexcept : f_{frame} {}

    template <class T>
    [[nodiscard]] T as() const {
        T out{};
        unpackValue(f_, out);
        return out;
    }

    template <class T>
    void convert(T& out) const {
        unpackValue(f_, out);
    }

    [[nodiscard]] const Frame& frame() const noexcept { return f_; }

private:
    Frame f_;
};

/// Mirrors msgpack::object_handle: owns the bytes the frame views, so the Object stays
/// valid after the caller's buffer is gone. msgpack owns a zone for the same reason.
class ObjectHandle {
public:
    explicit ObjectHandle(Bytes bytes) : owned_{bytes.begin(), bytes.end()} {}

    [[nodiscard]] Object get() const {
        return Object{decode(Bytes{owned_.data(), owned_.size()})};
    }

    [[nodiscard]] Bytes bytes() const noexcept { return {owned_.data(), owned_.size()}; }

private:
    std::vector<std::uint8_t> owned_;
};

/// `unpack(ptr, len)` in the shape msgpack uses. Prefer `unpack<T>(bytes)` above in new
/// code -- it skips the copy this one makes to own its buffer.
[[nodiscard]] inline ObjectHandle unpack(const void* data, std::size_t len) {
    return ObjectHandle{Bytes{static_cast<const std::uint8_t*>(data), len}};
}

/// `pack(stream, value)` in the shape msgpack uses, for call sites that already have a
/// std::ostream (a stringstream, usually) to hand.
template <class T>
void pack(std::ostream& out, const T& value) {
    const std::vector<std::uint8_t> bytes = pack(value);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] inline std::string_view version() { return told_version(); }

// ---------------------------------------------------------------- porting from msgpack
//
// The macros above are named so that moving a class over from msgpack is a
// search and replace of MSGPACK_ for TOLD_ and MSGPACK:: for told:: :
//
//     MSGPACK_DEFINE(a, b)        ->  TOLD_DEFINE(a, b)          array, positional
//     MSGPACK_DEFINE_ARRAY(a, b)  ->  TOLD_DEFINE_ARRAY(a, b)    the same thing, spelled out
//     MSGPACK_DEFINE_MAP(a, b)    ->  TOLD_DEFINE_MAP(a, b)      object, keys are names
//     MSGPACK_ADD_ENUM(E)         ->  TOLD_ADD_ENUM(E)           optional here; see above
//     MSGPACK_BASE(B)             ->  TOLD_BASE(B)
//     MSGPACK_NVP("k", m)         ->  TOLD_NVP("k", m)
//     MSGPACK::pack(ss, obj)      ->  told::pack(ss, obj)
//     MSGPACK::unpack(p, n)       ->  told::unpack(p, n)   .get().as<T>() as before

} // namespace told

#endif // TOLD_HPP
