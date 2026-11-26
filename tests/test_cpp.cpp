// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

// Unit tests for the C++ wrapper in told.hpp. The C tests already pin the bytes; these
// check what the wrapper adds on top: exceptions carrying the status, RAII scopes, ranges,
// the struct-mapping macros and adaptors, and readFrame over callables and istreams.
#include <told.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "harness.h"

// Records a failure unless `expr` throws told::Error carrying `want`.
#define CHECK_THROWS(expr, want)                                                      \
    do {                                                                              \
        bool thrown_ = false;                                                         \
        try {                                                                         \
            (void)(expr);                                                             \
        } catch (const told::Error& err_) {                                           \
            thrown_ = err_.status() == (want);                                        \
            if (!thrown_) printf("        threw %s\n", err_.what());                  \
        }                                                                             \
        if (!thrown_) {                                                               \
            harness_failures++;                                                       \
            printf("        %s:%d: CHECK_THROWS(%s, %s)\n", __FILE__, __LINE__, #expr, #want); \
        }                                                                             \
    } while (0)

namespace {

std::vector<std::uint8_t> copy(told::Bytes bytes) {
    return {bytes.begin(), bytes.end()};
}

// --- decoding ---------------------------------------------------------------------

void test_decode_and_typed_accessors() {
    const std::uint8_t bytes[] = {0x52, 0x01, 0x2C};
    const told::Frame f = told::decode(bytes);
    CHECK(f.type() == told::Type::int32);
    CHECK(f.asInt() == 300);
    CHECK(f.asUint() == 300);
    CHECK(f.size() == 3);
    CHECK(f.raw().data() == bytes); // zero-copy
}

void test_errors_throw_with_their_status() {
    const std::uint8_t reserved[] = {0xD0};
    CHECK_THROWS(told::decode(reserved), TOLD_RESERVED_TYPE);

    const std::uint8_t seven[] = {0x27};
    CHECK_THROWS(told::decode(seven).asString(), TOLD_TYPE_MISMATCH);
    CHECK_THROWS(told::decode(seven).array(), TOLD_TYPE_MISMATCH);

    const std::uint8_t bad_utf8[] = {0x92, 0xFF, 0xFE};
    CHECK_THROWS(told::decode(bad_utf8).asString(), TOLD_INVALID_UTF8);

    try {
        (void)told::decode(reserved);
    } catch (const told::Error& err) {
        CHECK(std::string{err.what()} == "told: reserved type");
    }
}

void test_frame_size_is_optional_while_incomplete() {
    const std::uint8_t long_buffer[] = {0xC9, 0x01, 0x2C};
    CHECK(!told::frameSize(told::Bytes{long_buffer, 2}).has_value());
    CHECK(told::frameSize(long_buffer) == 303u);
}

void test_validate_throws_on_buried_damage() {
    const std::uint8_t nested_bad[] = {0xB7, 0x92, 'o', 'k', 0xA3, 0x92, 0xFF, 0xFE};
    (void)told::decode(nested_bad); // the outer frame is fine
    CHECK_THROWS(told::validate(nested_bad), TOLD_INVALID_UTF8);
}

void test_extension_code_is_optional() {
    told::Encoder enc;
    const std::uint8_t payload[] = {1, 2};
    enc.writeExtended(0x42, payload);
    CHECK(told::decode(enc.bytes()).extensionCode() == std::optional<std::uint8_t>{0x42});
    const std::uint8_t seven[] = {0x27};
    CHECK(!told::decode(seven).extensionCode().has_value());
}

// --- encoding ---------------------------------------------------------------------

void test_scopes_close_their_containers() {
    told::Encoder enc;
    {
        auto obj = enc.beginObject();
        enc.key("z");
        enc.writeBool(true);
        enc.key("a");
        {
            auto arr = enc.beginArray();
            enc.writeUint(2);
        } // closed here
    } // and here
    const auto bytes = enc.bytes();
    EXPECT_BYTES(bytes.data(), bytes.size(), 0xB7, 0x91, 'z', 0x11, 0x91, 'a', 0xA1, 0x22);
}

void test_scope_close_is_explicit_and_once() {
    told::Encoder enc;
    auto arr = enc.beginArray();
    enc.writeNull();
    arr.close();
    arr.close(); // a second close does nothing, and neither will the destructor
    const auto bytes = enc.bytes();
    EXPECT_BYTES(bytes.data(), bytes.size(), 0xA1, 0x00);
}

void test_write_names_are_explicit() {
    told::Encoder enc;
    enc.writeString("text"); // must not bind to writeBool, which is why there is no write()
    enc.writeInt(-1);
    enc.writeFloat32(1.5f);
    enc.writeFloat(-0.0);
    const auto bytes = enc.bytes();
    EXPECT_BYTES(bytes.data(), bytes.size(), 0x94, 't', 'e', 'x', 't', 0x31, 0xFF, 0x74, 0x3F,
                 0xC0, 0x00, 0x00, 0x88, 0x80, 0, 0, 0, 0, 0, 0, 0);
}

void test_invalid_utf8_write_throws() {
    told::Encoder enc;
    CHECK_THROWS(enc.writeString("\xFF\xFE"), TOLD_INVALID_UTF8);
}

void test_encoder_moves_and_resets() {
    told::Encoder a;
    a.writeUint(1);
    told::Encoder b = std::move(a);
    CHECK(b.bytes().size() == 1);
    b.reset();
    CHECK(b.bytes().empty());
    b.writeUint(2);
    EXPECT_BYTES(b.bytes().data(), b.bytes().size(), 0x22);
}

// --- ranges -----------------------------------------------------------------------

void test_ranges_walk_arrays_and_objects() {
    told::Encoder enc;
    {
        auto obj = enc.beginObject();
        enc.key("n");
        {
            auto arr = enc.beginArray();
            enc.writeUint(1);
            enc.writeUint(2);
            enc.writeUint(3);
        }
        enc.key("s");
        enc.writeString("hi");
    }
    const auto bytes = copy(enc.bytes());
    const told::Frame root = told::decode(bytes);

    std::vector<std::string> keys;
    std::uint64_t sum = 0;
    for (const told::Field& field : root.object()) {
        keys.emplace_back(field.key);
        if (field.key == "n")
            for (const told::Frame& item : field.value.array()) sum += item.asUint();
        if (field.key == "s") CHECK(field.value.asString() == "hi");
    }
    CHECK((keys == std::vector<std::string>{"n", "s"}));
    CHECK(sum == 6);
}

void test_range_iteration_reports_malformed_containers() {
    const std::uint8_t int_key[] = {0xB2, 0x2A, 0x2B};
    CHECK_THROWS(told::decode(int_key).object().begin(), TOLD_MALFORMED_CONTAINER);
}

// --- struct mapping ---------------------------------------------------------------

enum class Mode : std::int32_t { idle = 0, running = 1, faulted = 2 };

struct Reading {
    std::uint8_t channel{};
    double value{};
    bool valid = true;
    TOLD_DEFINE_MAP(channel, value, valid)
    bool operator==(const Reading&) const = default;
};

struct Point {
    double x{}, y{};
    TOLD_DEFINE(x, y)
    bool operator==(const Point&) const = default;
};

struct Terse {
    std::uint8_t channel{};
    double value{};
    TOLD_DEFINE_MAP(TOLD_NVP("ch", channel), TOLD_NVP("v", value))
};

struct Record {
    std::string name;
    Mode mode{};
    std::vector<Reading> readings;
    std::vector<std::uint8_t> raw;
    std::optional<std::string> note;
    std::map<std::string, int> counts;
    Point origin;
    TOLD_DEFINE_MAP(name, mode, readings, raw, note, counts, origin)
    bool operator==(const Record&) const = default;
};

struct Base {
    int id{};
    TOLD_DEFINE_MAP(id)
};

struct Derived : Base {
    int extra{};
    TOLD_DEFINE_MAP(TOLD_BASE(Base), extra)
};

void test_struct_packs_like_the_c_descriptor_does() {
    const Reading r{1, 0.0, true};
    const auto bytes = told::pack(r);
    // The same bytes test_c.c pins for its descriptor-table version of this struct.
    EXPECT_BYTES(bytes.data(), bytes.size(), 0xB8, 0x17, 0x97, 'c', 'h', 'a', 'n', 'n', 'e',
                 'l', 0x21, 0x95, 'v', 'a', 'l', 'u', 'e', 0x80, 0x95, 'v', 'a', 'l', 'i',
                 'd', 0x11);
    CHECK(told::unpack<Reading>(bytes) == r);
}

void test_positional_struct_has_no_keys() {
    const auto bytes = told::pack(Point{1.5, 0.0});
    EXPECT_BYTES(bytes.data(), bytes.size(), 0xA8, 0x0A, 0x88, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0,
                 0x80);
    CHECK((told::unpack<Point>(bytes) == Point{1.5, 0.0}));
}

void test_nvp_sets_the_wire_key() {
    const auto bytes = told::pack(Terse{3, 0.0});
    EXPECT_BYTES(bytes.data(), bytes.size(), 0xB7, 0x92, 'c', 'h', 0x23, 0x91, 'v', 0x80);
}

void test_nested_record_round_trips() {
    Record rec;
    rec.name = "sensor-a";
    rec.mode = Mode::faulted;
    rec.readings = {{1, 3.5, true}, {2, -0.25, false}};
    rec.raw = {0x00, 0xFF, 0x0A};
    rec.note = std::nullopt;
    rec.counts = {{"a", 1}, {"b", -2}};
    rec.origin = {1.5, -2.5};
    const auto bytes = told::pack(rec);
    told::validate(bytes);
    CHECK(told::unpack<Record>(bytes) == rec);

    rec.note = "hello";
    CHECK(told::unpack<Record>(told::pack(rec)).note == std::optional<std::string>{"hello"});
}

void test_base_members_nest_under_the_base_name() {
    Derived d;
    d.id = 5;
    d.extra = 6;
    const auto bytes = told::pack(d);
    const told::Frame root = told::decode(bytes);
    auto it = root.object().begin();
    CHECK(it->key == "Base");
    const Derived back = told::unpack<Derived>(bytes);
    CHECK(back.id == 5 && back.extra == 6);
}

void test_unpack_ignores_unknown_keys_and_keeps_defaults() {
    told::Encoder enc;
    {
        auto obj = enc.beginObject();
        enc.key("value");
        enc.writeFloat(2.0);
        enc.key("added_later");
        enc.writeNull();
    }
    const Reading r = told::unpack<Reading>(copy(enc.bytes()));
    CHECK(r.channel == 0 && r.value == 2.0 && r.valid);
}

void test_unpack_refuses_out_of_range_and_wrong_types() {
    const auto big = told::pack(std::map<std::string, int>{{"channel", 300}});
    CHECK_THROWS(told::unpack<Reading>(big), TOLD_OUT_OF_RANGE);
    CHECK_THROWS(told::unpack<std::int8_t>(told::pack(-129)), TOLD_OUT_OF_RANGE);
    CHECK(told::unpack<std::int8_t>(told::pack(-128)) == -128);
    CHECK_THROWS(told::unpack<Mode>(told::pack(std::int64_t{1} << 40)), TOLD_OUT_OF_RANGE);
    CHECK_THROWS(told::unpack<std::string>(told::pack(7)), TOLD_TYPE_MISMATCH);
}

void test_msgpack_shaped_calls() {
    std::stringstream ss;
    told::pack(ss, Reading{4, 1.5, false});
    const std::string s = ss.str();
    const Reading r = told::unpack(s.data(), s.size()).get().as<Reading>();
    CHECK((r == Reading{4, 1.5, false}));
}

} // namespace

// A type the mapping cannot see into, taught through an adaptor.
struct Celsius {
    double degrees;
};

template <>
struct told::Adaptor<Celsius> {
    static void pack(told::Encoder& enc, const Celsius& c) { enc.writeFloat(c.degrees); }
    static void unpack(const told::Frame& f, Celsius& out) { out.degrees = f.asFloat(); }
};

namespace {

void test_adaptor_drives_a_foreign_type() {
    const auto bytes = told::pack(Celsius{21.5});
    CHECK(told::decode(bytes).type() == told::Type::float64);
    CHECK(told::unpack<Celsius>(bytes).degrees == 21.5);
}

// --- stream reading ---------------------------------------------------------------

std::string three_frames() {
    told::Encoder enc;
    {
        auto obj = enc.beginObject();
        enc.key("seq");
        enc.writeUint(4242);
    }
    enc.writeBuffer(std::vector<std::uint8_t>(300, 'x'));
    enc.writeUint(7);
    const auto bytes = enc.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void test_read_frame_from_an_istream() {
    std::istringstream in{three_frames() + "tail"};
    std::vector<std::uint8_t> buf(512);

    auto first = told::readFrame(in, buf);
    CHECK(first.has_value() && first->type() == told::Type::object);
    auto second = told::readFrame(in, buf);
    CHECK(second.has_value() && second->asBuffer().size() == 300);
    auto third = told::readFrame(in, buf);
    CHECK(third.has_value() && third->asUint() == 7);

    std::string rest;
    in >> rest;
    CHECK(rest == "tail");
}

void test_read_frame_returns_nullopt_at_the_end() {
    std::istringstream in{three_frames()};
    std::vector<std::uint8_t> buf(512);
    int frames = 0;
    while (auto frame = told::readFrame(in, buf)) {
        (void)frame;
        frames++;
    }
    CHECK(frames == 3);
}

void test_read_frame_from_a_callable_with_short_reads() {
    const std::string data = three_frames();
    std::size_t pos = 0, calls = 0;
    auto one_byte = [&](std::span<std::uint8_t> dst) -> std::ptrdiff_t {
        calls++;
        if (pos == data.size()) return 0;
        dst[0] = static_cast<std::uint8_t>(data[pos++]);
        return 1;
    };
    std::vector<std::uint8_t> buf(512);
    int frames = 0;
    while (told::readFrame(one_byte, buf)) frames++;
    CHECK(frames == 3);
    CHECK(calls == data.size() + 1);
}

void test_read_frame_throws_for_a_cut_stream_and_a_small_buffer() {
    const std::string data = three_frames();
    std::vector<std::uint8_t> buf(512);

    std::istringstream cut{data.substr(0, 3)};
    CHECK_THROWS(told::readFrame(cut, buf), TOLD_TRUNCATED);

    std::istringstream big{data.substr(8)}; // starts at the 300-byte BUFFER
    std::vector<std::uint8_t> small(64);
    CHECK_THROWS(told::readFrame(big, small), TOLD_FRAME_TOO_LARGE);
}

void test_read_frame_carries_the_callables_exception() {
    auto boom = [](std::span<std::uint8_t>) -> std::ptrdiff_t {
        throw std::runtime_error("peer gone");
    };
    std::vector<std::uint8_t> buf(64);
    bool carried = false;
    try {
        (void)told::readFrame(boom, buf);
    } catch (const std::runtime_error& err) {
        carried = std::string{err.what()} == "peer gone";
    }
    CHECK(carried);

    auto fails = [](std::span<std::uint8_t>) -> std::ptrdiff_t { return -1; };
    CHECK_THROWS(told::readFrame(fails, buf), TOLD_READ_FAILED);
}

} // namespace

int main() {
    printf("told C++ unit tests\n");

    RUN(test_decode_and_typed_accessors);
    RUN(test_errors_throw_with_their_status);
    RUN(test_frame_size_is_optional_while_incomplete);
    RUN(test_validate_throws_on_buried_damage);
    RUN(test_extension_code_is_optional);
    RUN(test_scopes_close_their_containers);
    RUN(test_scope_close_is_explicit_and_once);
    RUN(test_write_names_are_explicit);
    RUN(test_invalid_utf8_write_throws);
    RUN(test_encoder_moves_and_resets);
    RUN(test_ranges_walk_arrays_and_objects);
    RUN(test_range_iteration_reports_malformed_containers);
    RUN(test_struct_packs_like_the_c_descriptor_does);
    RUN(test_positional_struct_has_no_keys);
    RUN(test_nvp_sets_the_wire_key);
    RUN(test_nested_record_round_trips);
    RUN(test_base_members_nest_under_the_base_name);
    RUN(test_unpack_ignores_unknown_keys_and_keeps_defaults);
    RUN(test_unpack_refuses_out_of_range_and_wrong_types);
    RUN(test_msgpack_shaped_calls);
    RUN(test_adaptor_drives_a_foreign_type);
    RUN(test_read_frame_from_an_istream);
    RUN(test_read_frame_returns_nullopt_at_the_end);
    RUN(test_read_frame_from_a_callable_with_short_reads);
    RUN(test_read_frame_throws_for_a_cut_stream_and_a_small_buffer);
    RUN(test_read_frame_carries_the_callables_exception);

    return harness_report();
}
