// Struct mapping in C++: the TLD_* macros, the msgpack-shaped call surface, and a check
// that this side agrees byte-for-byte with the Zig and Python mappings.
//
//   zig build tld-map
//
// The interesting assertions are the last two: the golden frame in lib/testdata was
// produced by the Zig mapping, and this file both decodes it and reproduces it exactly.
// If that ever stops holding, a C++ producer and a Zig consumer have stopped agreeing.

#include "qnnt_tld.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

enum class Mode : int { idle = 0, running = 1, faulted = 2 };
TLD_ADD_ENUM(Mode) // optional -- enums work undeclared; this is for msgpack parity

// An unsigned underlying type, whose top value does not fit in int64.
enum class Flags : std::uint64_t { none = 0, all = 0xFFFFFFFFFFFFFFFFull };

struct Reading {
    std::uint8_t channel{};
    double value{};
    bool valid{true};
    TLD_DEFINE_MAP(channel, value, valid)
};

struct Point {
    double x{}, y{};
    TLD_DEFINE_MAP(x, y)
};

struct Sample {
    std::uint64_t id{};
    std::string name;
    Mode mode{Mode::idle};
    std::vector<Reading> readings;
    std::vector<std::uint8_t> raw;
    std::optional<std::string> note;
    Point origin;
    TLD_DEFINE_MAP(id, name, mode, readings, raw, note, origin)
};

// Positional: two doubles, no keys on the wire.
struct Vec2 {
    double x{}, y{};
    TLD_DEFINE(x, y)
};

// A base class and explicit short keys.
struct Header {
    int serial{};
    TLD_DEFINE_MAP(serial)
};
struct Tagged : Header {
    std::uint8_t channel{};
    double value{};
    TLD_DEFINE_MAP(TLD_BASE(Header), TLD_NVP("ch", channel), TLD_NVP("v", value))
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
    const std::vector<std::uint8_t> bytes = qnnt::tld::pack(canonical());
    const Sample got = qnnt::tld::unpack<Sample>(bytes);
    assert(got.id == 99 && got.name == "sensor-a" && got.mode == Mode::faulted);
    assert(got.readings.size() == 2 && got.readings[1].valid == false);
    assert((got.raw == std::vector<std::uint8_t>{0x00, 0xFF, 0x0A}));
    assert(!got.note.has_value());
    assert(got.origin.x == 1.5 && got.origin.y == -2.5);
    printf("  round trip: %zu bytes\n", bytes.size());

    // 2. positional is smaller than named for the same data
    const auto named = qnnt::tld::pack(Point{1.0, 2.0}).size();
    const auto positional = qnnt::tld::pack(Vec2{1.0, 2.0}).size();
    assert(positional < named);
    printf("  named %zu bytes, positional %zu bytes\n", named, positional);

    // 3. enums, including an unsigned 64-bit one
    assert(qnnt::tld::unpack<Flags>(qnnt::tld::pack(Flags::all)) == Flags::all);
    assert(qnnt::tld::unpack<Mode>(qnnt::tld::pack(Mode::running)) == Mode::running);

    // 4. base class and renamed keys land on the wire as intended
    Tagged sent;
    sent.serial = 42;
    sent.channel = 3;
    sent.value = 9.5;
    std::stringstream ss;
    qnnt::tld::pack(ss, sent); // the msgpack call shape
    const std::string blob = ss.str();
    const Tagged back = qnnt::tld::unpack(blob.data(), blob.size()).get().as<Tagged>();
    assert(back.serial == 42 && back.channel == 3 && back.value == 9.5);

    std::string keys;
    for (const auto& field : qnnt::tld::decode(
             qnnt::tld::Bytes{reinterpret_cast<const std::uint8_t*>(blob.data()),
                              blob.size()})
             .object()) {
        keys += std::string{field.key} + " ";
    }
    assert(keys == "Header ch v ");
    printf("  keys with TLD_BASE and TLD_NVP: %s\n", keys.c_str());

    // 5. the golden frame: does C++ read what Zig wrote?
    std::ifstream in("lib/testdata/interop.tld", std::ios::binary);
    if (!in) {
        printf("  golden frame not found (run from the project root) -- skipped\n");
        return 0;
    }
    const std::vector<std::uint8_t> golden((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
    const Sample from_zig = qnnt::tld::unpack<Sample>(golden);
    assert(from_zig.id == 99 && from_zig.name == "sensor-a");
    assert(from_zig.mode == Mode::faulted && from_zig.readings.size() == 2);
    assert(from_zig.origin.y == -2.5);

    // 6. and does C++ write exactly the same bytes?
    assert(bytes == golden);
    printf("  golden frame: decoded, and re-encoded byte-for-byte identically\n");

    printf("  all C++ struct-mapping checks passed\n");
    return 0;
}
