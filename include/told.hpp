// TLD (Type-Length-Data) v1.0 -- header-only C++ wrapper over the C ABI in qnnt_tld.h.
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

#ifndef QNNT_TLD_HPP
#define QNNT_TLD_HPP

#include "qnnt_tld.h"

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace qnnt::tld {

using Status = qnnt_tld_status;
using Bytes = std::span<const std::uint8_t>;

enum class Type : std::uint8_t {
    null = QNNT_TLD_NULL,
    boolean = QNNT_TLD_BOOL,
    tinyuint = QNNT_TLD_TINYUINT,
    int8 = QNNT_TLD_INT8,
    int16 = QNNT_TLD_INT16,
    int32 = QNNT_TLD_INT32,
    int64 = QNNT_TLD_INT64,
    float32 = QNNT_TLD_FLOAT32,
    float64 = QNNT_TLD_FLOAT64,
    string = QNNT_TLD_STRING,
    array = QNNT_TLD_ARRAY,
    object = QNNT_TLD_OBJECT,
    buffer = QNNT_TLD_BUFFER,
    extended = QNNT_TLD_EXTENDED,
};

/// Carries the status so callers can branch on the cause, not just the message.
class Error : public std::runtime_error {
public:
    explicit Error(Status status)
        : std::runtime_error(std::string{"tld: "} + qnnt_tld_status_name(status)), status_{status} {}
    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Status status_;
};

inline void check(Status status) {
    if (status != QNNT_TLD_OK) throw Error{status};
}

class ArrayIterator;
class ObjectIterator;
class ArrayRange;
class ObjectRange;

/// A decoded frame. Copyable and cheap: it is a type, a mod and two borrowed spans.
class Frame {
public:
    Frame() = default;
    explicit Frame(const qnnt_tld_frame& raw) noexcept : f_{raw} {}

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
        check(qnnt_tld_as_bool(&f_, &out));
        return out;
    }

    [[nodiscard]] std::int64_t asInt() const {
        std::int64_t out{};
        check(qnnt_tld_as_i64(&f_, &out));
        return out;
    }

    [[nodiscard]] std::uint64_t asUint() const {
        std::uint64_t out{};
        check(qnnt_tld_as_u64(&f_, &out));
        return out;
    }

    [[nodiscard]] double asFloat() const {
        double out{};
        check(qnnt_tld_as_f64(&f_, &out));
        return out;
    }

    /// UTF-8 validated, and a view into the decoded buffer.
    [[nodiscard]] std::string_view asString() const {
        const std::uint8_t* ptr{};
        std::size_t len{};
        check(qnnt_tld_as_string(&f_, &ptr, &len));
        return {reinterpret_cast<const char*>(ptr), len};
    }

    /// Opaque bytes, no encoding contract -- also a view, not a copy.
    [[nodiscard]] Bytes asBuffer() const {
        const std::uint8_t* ptr{};
        std::size_t len{};
        check(qnnt_tld_as_buffer(&f_, &ptr, &len));
        return {ptr, len};
    }

    [[nodiscard]] ArrayRange array() const;
    [[nodiscard]] ObjectRange object() const;

    [[nodiscard]] const qnnt_tld_frame& native() const noexcept { return f_; }

private:
    friend class ArrayIterator;
    friend class ObjectIterator;
    qnnt_tld_frame f_{};
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
    explicit ArrayIterator(qnnt_tld_iter it) : it_{it} { advance(); }

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
        check(qnnt_tld_array_next(&it_, &current_.f_, &have));
        have_ = have;
    }

    qnnt_tld_iter it_{};
    Frame current_{};
    bool have_ = false;
};

/// Input iterator over an OBJECT's key/value pairs, in wire order.
class ObjectIterator {
public:
    using value_type = Field;
    using difference_type = std::ptrdiff_t;

    ObjectIterator() = default; // end sentinel
    explicit ObjectIterator(qnnt_tld_iter it) : it_{it} { advance(); }

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
        check(qnnt_tld_object_next(&it_, &key, &key_len, &current_.value.f_, &have));
        current_.key = {reinterpret_cast<const char*>(key), have ? key_len : 0};
        have_ = have;
    }

    qnnt_tld_iter it_{};
    Field current_{};
    bool have_ = false;
};

class ArrayRange {
public:
    explicit ArrayRange(qnnt_tld_iter it) noexcept : it_{it} {}
    [[nodiscard]] ArrayIterator begin() const { return ArrayIterator{it_}; }
    [[nodiscard]] ArrayIterator end() const noexcept { return {}; }

private:
    qnnt_tld_iter it_;
};

class ObjectRange {
public:
    explicit ObjectRange(qnnt_tld_iter it) noexcept : it_{it} {}
    [[nodiscard]] ObjectIterator begin() const { return ObjectIterator{it_}; }
    [[nodiscard]] ObjectIterator end() const noexcept { return {}; }

private:
    qnnt_tld_iter it_;
};

inline ArrayRange Frame::array() const {
    qnnt_tld_iter it{};
    check(qnnt_tld_array_iter(&f_, &it));
    return ArrayRange{it};
}

inline ObjectRange Frame::object() const {
    qnnt_tld_iter it{};
    check(qnnt_tld_object_iter(&f_, &it));
    return ObjectRange{it};
}

/// Decode the frame at the start of `bytes`, which may extend past it.
[[nodiscard]] inline Frame decode(Bytes bytes) {
    qnnt_tld_frame frame{};
    check(qnnt_tld_decode(bytes.data(), bytes.size(), &frame));
    return Frame{frame};
}

/// Size of the frame at the start of `bytes`, or nullopt when the prefix is too short to
/// tell — the distinction a stream reader needs, expressed in the return type rather than
/// as a status to remember.
[[nodiscard]] inline std::optional<std::size_t> frameSize(Bytes bytes) {
    std::size_t size{};
    const Status status = qnnt_tld_frame_size(bytes.data(), bytes.size(), &size);
    if (status == QNNT_TLD_INCOMPLETE) return std::nullopt;
    check(status);
    return size;
}

/// Walk the whole tree: nested frames, UTF-8 in strings, object shape. Use on untrusted
/// input; `decode` only checks the outermost frame.
inline void validate(Bytes bytes, std::size_t max_depth = 32) {
    check(qnnt_tld_validate(bytes.data(), bytes.size(), max_depth));
}

/// Owns the encode buffer. Move-only, because two owners would double-free the handle.
class Encoder {
public:
    Encoder() : handle_{qnnt_tld_encoder_create()} {
        if (handle_ == nullptr) throw std::bad_alloc{};
    }

    ~Encoder() { qnnt_tld_encoder_destroy(handle_); }

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    Encoder(Encoder&& other) noexcept : handle_{std::exchange(other.handle_, nullptr)} {}
    Encoder& operator=(Encoder&& other) noexcept {
        if (this != &other) {
            qnnt_tld_encoder_destroy(handle_);
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
            check(is_object_ ? qnnt_tld_end_object(enc->handle_, mark_)
                             : qnnt_tld_end_array(enc->handle_, mark_));
        }

    private:
        void closeQuietly() noexcept {
            if (is_object_) {
                (void)qnnt_tld_end_object(enc_->handle_, mark_);
            } else {
                (void)qnnt_tld_end_array(enc_->handle_, mark_);
            }
            enc_ = nullptr;
        }

        Encoder* enc_;
        std::size_t mark_;
        bool is_object_;
    };

    [[nodiscard]] Scope beginArray() {
        std::size_t mark{};
        check(qnnt_tld_begin_array(handle_, &mark));
        return Scope{*this, mark, false};
    }

    [[nodiscard]] Scope beginObject() {
        std::size_t mark{};
        check(qnnt_tld_begin_object(handle_, &mark));
        return Scope{*this, mark, true};
    }

    // Deliberately NOT a set of `write` overloads. A string literal binds to `bool`
    // ahead of `std::string_view` -- pointer-to-bool is a standard conversion and beats
    // a user-defined one -- so `write("text")` would silently encode `true`. That is a
    // data-corruption bug with no diagnostic, and it happened here before these names
    // existed. Integer literals have the matching problem in reverse, being ambiguous
    // between the signed, unsigned and floating overloads. Explicit names cost a few
    // characters and remove the whole class of error.
    void writeNull() { check(qnnt_tld_write_null(handle_)); }
    void writeBool(bool value) { check(qnnt_tld_write_bool(handle_, value)); }
    void writeUint(std::uint64_t value) { check(qnnt_tld_write_u64(handle_, value)); }
    void writeInt(std::int64_t value) { check(qnnt_tld_write_i64(handle_, value)); }
    void writeFloat32(float value) { check(qnnt_tld_write_f32(handle_, value)); }
    void writeFloat(double value) { check(qnnt_tld_write_f64(handle_, value)); }

    void writeString(std::string_view text) {
        check(qnnt_tld_write_string(handle_, reinterpret_cast<const std::uint8_t*>(text.data()),
                                    text.size()));
    }

    /// Named for readability at call sites; an object key is just a STRING frame.
    void key(std::string_view name) { writeString(name); }

    void writeBuffer(Bytes bytes) {
        check(qnnt_tld_write_buffer(handle_, bytes.data(), bytes.size()));
    }

    void writeExtended(std::uint8_t code, Bytes payload) {
        check(qnnt_tld_write_extended(handle_, code, payload.data(), payload.size()));
    }

    /// Borrowed: copy before the next write, which may reallocate.
    [[nodiscard]] Bytes bytes() const noexcept {
        std::size_t len{};
        const std::uint8_t* ptr = qnnt_tld_encoder_bytes(handle_, &len);
        return {ptr, len};
    }

    /// Drop the contents, keep the capacity, for reusing one encoder per message.
    void reset() noexcept { qnnt_tld_encoder_reset(handle_); }

private:
    friend class Scope;
    qnnt_tld_encoder* handle_;
};

[[nodiscard]] inline std::string_view version() { return qnnt_tld_version(); }

} // namespace qnnt::tld

#endif // QNNT_TLD_HPP
