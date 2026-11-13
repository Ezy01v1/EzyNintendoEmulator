#include <doctest/doctest.h>
#include "common/state.hpp"
#include "n64/rdp_hidden_bits.hpp"
#include <limits>
#include <map>

using ezy::n64::RdpHiddenBits;

TEST_CASE("RDP direct hidden bits preserve the sparse map across writes and fill ranges") {
    RdpHiddenBits bits;
    std::map<std::uint32_t, std::uint8_t> reference;
    std::uint32_t random = 0x7195AB34;
    for (unsigned i = 0; i < 4096; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const auto index = random % RdpHiddenBits::kHalfwords;
        const auto value = static_cast<std::uint8_t>((random >> 19) & 3);
        bits.write(index, value);
        if (value) reference[static_cast<std::uint32_t>(index)] = value;
        else reference.erase(static_cast<std::uint32_t>(index));
        CHECK(bits.read(index) == value);
        if ((i & 3) == 0) {
            const auto first = index - index % 32;
            bits.clear(first, first + 32);
            reference.erase(reference.lower_bound(static_cast<std::uint32_t>(first)),
                            reference.lower_bound(static_cast<std::uint32_t>(first + 32)));
            CHECK(bits.read(index) == 0);
        }
        REQUIRE(bits.count() == reference.size());
    }
    ezy::StateWriter old_format, actual;
    auto count = static_cast<std::uint32_t>(reference.size());
    old_format(count);
    for (auto [index, value] : reference) { old_format(index); old_format(value); }
    bits.serialize(actual);
    const auto saved = actual.take();
    CHECK(saved == old_format.take());
    RdpHiddenBits restored;
    ezy::StateReader reader(saved);
    restored.serialize(reader);
    REQUIRE_FALSE(reader.failed());
    REQUIRE(reader.at_end());
    for (auto [index, value] : reference) CHECK(restored.read(index) == value);
    restored.clear();
    CHECK(restored.count() == 0);
    for (auto [index, value] : reference) { (void)value; CHECK(restored.read(index) == 0); }
}

TEST_CASE("RDP hidden bits bound the last RDRAM halfword and clipped fill ranges") {
    RdpHiddenBits bits;
    bits.write(0, 7);
    bits.write(RdpHiddenBits::kHalfwords - 1, 2);
    bits.write(RdpHiddenBits::kHalfwords, 3);
    CHECK(bits.count() == 2);
    CHECK(bits.read(0) == 3);
    CHECK(bits.read(RdpHiddenBits::kHalfwords) == 0);
    bits.clear(RdpHiddenBits::kHalfwords, std::numeric_limits<std::size_t>::max());
    CHECK(bits.count() == 2);
    bits.clear(RdpHiddenBits::kHalfwords - 1, std::numeric_limits<std::size_t>::max());
    CHECK(bits.count() == 1);
    CHECK(bits.read(RdpHiddenBits::kHalfwords - 1) == 0);
    bits.clear(20, 10);
    CHECK(bits.read(0) == 3);
    bits.clear(0, 1);
    CHECK(bits.count() == 0);
}

TEST_CASE("RDP hidden bits reject malformed sparse states without unbounded iteration") {
    for (unsigned variant = 0; variant < 4; ++variant) {
        RdpHiddenBits bits;
        ezy::StateWriter writer;
        std::uint32_t count = variant == 0 ? 0xFFFFFFFFU : 1;
        std::uint32_t index = variant == 1 ? static_cast<std::uint32_t>(RdpHiddenBits::kHalfwords) : 0;
        std::uint8_t value = variant == 2 ? 4 : 1;
        writer(count);
        if (variant != 3) { writer(index); writer(value); }
        const auto saved = writer.take();
        ezy::StateReader reader(saved);
        bits.serialize(reader);
        CHECK(reader.failed());
    }
}

TEST_CASE("RDP hidden bits retain legacy duplicate entries and canonical sparse ordering") {
    RdpHiddenBits bits;
    ezy::StateWriter writer;
    std::uint32_t count = 4, high = static_cast<std::uint32_t>(RdpHiddenBits::kHalfwords - 1), low = 0;
    std::uint8_t three = 3, zero = 0, one = 1, two = 2;
    writer(count);
    writer(high); writer(three);
    writer(high); writer(zero);
    writer(low); writer(one);
    writer(high); writer(two);
    const auto saved = writer.take();
    ezy::StateReader reader(saved);
    bits.serialize(reader);
    REQUIRE_FALSE(reader.failed());
    REQUIRE(reader.at_end());
    CHECK(bits.count() == 2);
    CHECK(bits.read(low) == 1);
    CHECK(bits.read(high) == 2);
    ezy::StateWriter canonical, actual;
    count = 2;
    canonical(count); canonical(low); canonical(one); canonical(high); canonical(two);
    bits.serialize(actual);
    CHECK(actual.take() == canonical.take());
}
