// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

// The same round-trip as told_demo.c, through the C++ wrapper -- so the two can be read
// side by side to see what the wrapper actually buys.
#include <told.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    std::printf("TOLD version %s (C++ wrapper; must match the C demo byte for byte)\n", std::string{told::version()}.c_str());

    told::Encoder enc;
    const std::uint8_t raw_payload[] = {0x0A, 0xFF, 0x00};

    {
        // Containers close themselves: no marks to carry, no end to forget.
        auto obj = enc.beginObject();
        enc.key("seq");
        enc.writeUint(4242);
        enc.key("live");
        enc.writeBool(true);
        enc.key("node");
        enc.writeString("node-c");
        enc.key("raw");
        enc.writeBuffer(raw_payload);
        enc.key("tags");
        {
            auto arr = enc.beginArray();
            enc.writeUint(1);
            enc.writeFloat(1.5);
            enc.writeNull();
        }
    }

    const auto bytes = enc.bytes();
    std::printf("encoded %zu bytes\n", bytes.size());

    // Incompleteness is in the return type, not a status to remember.
    const auto partial = told::frameSize(bytes.first(1));
    std::printf("frameSize from 1 byte: %s\n", partial ? "value" : "nullopt (incomplete)");
    std::printf("frameSize from 2 bytes: %zu\n", told::frameSize(bytes.first(2)).value());

    told::validate(bytes);
    const auto frame = told::decode(bytes);
    std::printf("decoded type=0x%X size=%zu (raw %s inside the encoder buffer)\n",
                static_cast<unsigned>(frame.type()), frame.size(),
                frame.raw().data() >= bytes.data() ? "points" : "does NOT point");

    for (auto field : frame.object()) {
        std::printf("  %.*s = ", static_cast<int>(field.key.size()), field.key.data());
        switch (field.value.type()) {
        case told::Type::tinyuint:
        case told::Type::int8:
        case told::Type::int16:
        case told::Type::int32:
        case told::Type::int64:
            std::printf("%llu\n", static_cast<unsigned long long>(field.value.asUint()));
            break;
        case told::Type::boolean:
            std::printf("%s\n", field.value.asBool() ? "true" : "false");
            break;
        case told::Type::string: {
            const auto text = field.value.asString();
            std::printf("\"%.*s\"\n", static_cast<int>(text.size()), text.data());
            break;
        }
        case told::Type::buffer: {
            const auto buf = field.value.asBuffer();
            std::printf("<buffer %zuB:", buf.size());
            for (const auto byte : buf) std::printf(" %02x", byte);
            std::printf(">\n");
            break;
        }
        case told::Type::array: {
            std::printf("[");
            for (const auto& elem : field.value.array()) {
                if (elem.isNull()) {
                    std::printf(" null");
                } else if (elem.type() == told::Type::float64) {
                    std::printf(" %.1f", elem.asFloat());
                } else {
                    std::printf(" %llu", static_cast<unsigned long long>(elem.asUint()));
                }
            }
            std::printf(" ]\n");
            break;
        }
        default:
            std::printf("(type 0x%X)\n", static_cast<unsigned>(field.value.type()));
        }
    }

    // Errors arrive as typed exceptions carrying the status.
    const std::uint8_t reserved[] = {0xD0};
    try {
        (void)told::decode(reserved);
        std::printf("FAIL: reserved nibble decoded\n");
        return 1;
    } catch (const told::Error& err) {
        std::printf("reserved nibble 0xD0 -> caught \"%s\" (status %d)\n", err.what(),
                    static_cast<int>(err.status()));
    }

    const std::uint8_t bad_utf8[] = {0x92, 0xFF, 0xFE};
    try {
        (void)told::decode(bad_utf8).asString();
        std::printf("FAIL: invalid utf-8 accepted\n");
        return 1;
    } catch (const told::Error& err) {
        std::printf("invalid utf-8 -> caught \"%s\"\n", err.what());
    }

    // Move-only: the moved-from encoder must not double-free at scope exit.
    told::Encoder moved = std::move(enc);
    moved.reset();
    moved.writeUint(9);
    std::printf("after move+reset: %zu byte(s)\n", moved.bytes().size());

    std::printf("OK\n");
    return 0;
}
