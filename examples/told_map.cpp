// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

// Struct mapping in C++: the TOLD_* macros, the msgpack-shaped call surface, and a check
// that this side agrees byte-for-byte with the Zig and Python mappings.
//
//   zig build told-map
//
// The interesting assertions are the last two: the golden frame in src/testdata was
// produced by the Zig mapping, and this file both decodes it and reproduces it exactly.
// If that ever stops holding, a C++ producer and a Zig consumer have stopped agreeing.

#include "told.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

// CHECK() vanishes under NDEBUG, which a release build defines -- then every check below
// would compile away and this example would prove nothing while still exiting 0.
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);    \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

enum class Mode : int { idle = 0, running = 1, faulted = 2 };
TOLD_ADD_ENUM(Mode) // optional -- enums work undeclared; this is for msgpack parity

// An unsigned underlying type, whose top value does not fit in int64.
enum class Flags : std::uint64_t { none = 0, all = 0xFFFFFFFFFFFFFFFFull };

struct Reading {
    std::uint8_t channel{};
    double value{};
    bool valid{true};
    TOLD_DEFINE_MAP(channel, value, valid)
};

struct Point {
    double x{}, y{};
    TOLD_DEFINE_MAP(x, y)
};

struct Sample {
    std::uint64_t id{};
    std::string name;
    Mode mode{Mode::idle};
    std::vector<Reading> readings;
    std::vector<std::uint8_t> raw;
    std::optional<std::string> note;
    Point origin;
    TOLD_DEFINE_MAP(id, name, mode, readings, raw, note, origin)
};

// Positional: two doubles, no keys on the wire.
struct Vec2 {
    double x{}, y{};
    TOLD_DEFINE(x, y)
};

// A base class and explicit short keys.
struct Header {
    int serial{};
    TOLD_DEFINE_MAP(serial)
};
struct Tagged : Header {
    std::uint8_t channel{};
    double value{};
    TOLD_DEFINE_MAP(TOLD_BASE(Header), TOLD_NVP("ch", channel), TOLD_NVP("v", value))
};

// A type whose definition cannot be touched -- stand-in for QString and friends -- taught
// to the mapping from outside. This is the mechanism the Qt header uses.
struct Foreign {
    int a{};
    int b{};
};

template <>
struct told::Adaptor<Foreign> {
    static void pack(Encoder& enc, const Foreign& v) {
        // Deliberately not the default shape: two ints as one "a:b" string, to prove the
        // adaptor is really in charge.
        char text[32];
        std::snprintf(text, sizeof text, "%d:%d", v.a, v.b);
        enc.writeString(text);
    }
    static void unpack(const Frame& frame, Foreign& out) {
        const std::string text{frame.asString()};
        std::sscanf(text.c_str(), "%d:%d", &out.a, &out.b);
    }
};

static Sample canonical() {
    Sample s;
    s.id = 99;
    s.name = "sensor-a";
    s.mode = Mode::faulted;
    s.readings = {Reading{1, 3.5, true}, Reading{2, -0.25, false}};
    s.raw = {0x00, 0xFF, 0x0A};
    s.note.reset();
    s.origin = Point{1.5, -2.5};
    return s;
}

int main() {
    // 1. round trip
    const std::vector<std::uint8_t> bytes = told::pack(canonical());
    const Sample got = told::unpack<Sample>(bytes);
    CHECK(got.id == 99 && got.name == "sensor-a" && got.mode == Mode::faulted);
    CHECK(got.readings.size() == 2 && got.readings[1].valid == false);
    CHECK((got.raw == std::vector<std::uint8_t>{0x00, 0xFF, 0x0A}));
    CHECK(!got.note.has_value());
    CHECK(got.origin.x == 1.5 && got.origin.y == -2.5);
    printf("  round trip: %zu bytes\n", bytes.size());

    // 2. positional is smaller than named for the same data
    const auto named = told::pack(Point{1.0, 2.0}).size();
    const auto positional = told::pack(Vec2{1.0, 2.0}).size();
    CHECK(positional < named);
    printf("  named %zu bytes, positional %zu bytes\n", named, positional);

    // 3. enums, including an unsigned 64-bit one
    CHECK(told::unpack<Flags>(told::pack(Flags::all)) == Flags::all);
    CHECK(told::unpack<Mode>(told::pack(Mode::running)) == Mode::running);

    // 4. base class and renamed keys land on the wire as intended
    Tagged sent;
    sent.serial = 42;
    sent.channel = 3;
    sent.value = 9.5;
    std::stringstream ss;
    told::pack(ss, sent); // the msgpack call shape
    const std::string blob = ss.str();
    const Tagged back = told::unpack(blob.data(), blob.size()).get().as<Tagged>();
    CHECK(back.serial == 42 && back.channel == 3 && back.value == 9.5);

    std::string keys;
    for (const auto& field : told::decode(
             told::Bytes{reinterpret_cast<const std::uint8_t*>(blob.data()),
                              blob.size()})
             .object()) {
        keys += std::string{field.key} + " ";
    }
    CHECK(keys == "Header ch v ");
    printf("  keys with TOLD_BASE and TOLD_NVP: %s\n", keys.c_str());

    // 5. the golden frame: does C++ read what Zig wrote?
    std::ifstream in("src/testdata/interop.tld", std::ios::binary);
    if (!in) {
        printf("  golden frame not found (run from the project root) -- skipped\n");
        return 0;
    }
    const std::vector<std::uint8_t> golden((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
    const Sample from_zig = told::unpack<Sample>(golden);
    CHECK(from_zig.id == 99 && from_zig.name == "sensor-a");
    CHECK(from_zig.mode == Mode::faulted && from_zig.readings.size() == 2);
    CHECK(from_zig.origin.y == -2.5);

    // 6. and does C++ write exactly the same bytes?
    CHECK(bytes == golden);
    printf("  golden frame: decoded, and re-encoded byte-for-byte identically\n");

    // 7. the adaptor extension point
    const Foreign foreign{7, 9};
    const auto foreign_bytes = told::pack(foreign);
    CHECK(told::decode(told::Bytes{foreign_bytes.data(), foreign_bytes.size()})
               .type() == told::Type::string);
    const Foreign foreign_back = told::unpack<Foreign>(foreign_bytes);
    CHECK(foreign_back.a == 7 && foreign_back.b == 9);
    printf("  Adaptor specialisation drives encoding for a foreign type\n");

    printf("  all C++ struct-mapping checks passed\n");
    return 0;
}
