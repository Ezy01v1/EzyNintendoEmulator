#include "n64/rsp_vector.hpp"

#include <algorithm>

namespace ezy::n64 {

void RspVector::access_memory(std::uint32_t word, std::uint32_t base, std::span<std::uint8_t, 0x1000> dmem) {
    const unsigned operation = (word >> 11) & 31;
    if (operation > 11) return;
    const bool store = (word >> 26) == 0x3A;  // SWC2
    const unsigned element = (word >> 7) & 15, reg = (word >> 16) & 31;
    const int offset = static_cast<int>(word & 63) - static_cast<int>(word & 64);
    constexpr std::array scale{1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16};
    const std::uint32_t address = base + static_cast<std::uint32_t>(offset * scale[operation]);
    const unsigned within_quad = address & 15;
    const std::uint32_t aligned8 = address & ~7U;
    const unsigned within8 = address & 7;
    auto& vector = registers_[reg];

    const auto read = [dmem](std::uint32_t addr) { return dmem[addr & 0xFFF]; };
    const auto write = [dmem](std::uint32_t addr, unsigned value) {
        dmem[addr & 0xFFF] = static_cast<std::uint8_t>(value);
    };
    const auto transfer = [&](unsigned start, unsigned count, std::uint32_t addr) {
        for (unsigned i = 0; i < count; ++i) {
            if (store) write(addr + i, byte(vector, start + i));
            else set_byte(vector, start + i, read(addr + i));
        }
    };

    // Simple loads truncate at the end of the vector, whereas stores wrap.
    if (operation <= 4) {  // BV, SV, LV, DV, QV
        const unsigned width = operation == 4 ? 16 - within_quad : 1U << operation;
        transfer(element, store ? width : std::min(width, 16 - element), address);
        return;
    }
    if (operation == 5) {  // RV transfers the other side of the aligned quadword.
        if (store) {
            transfer(element + 16 - within_quad, within_quad, address & ~15U);
        } else if (within_quad > element) {
            const unsigned count = within_quad - element;
            transfer(16 - count, count, address & ~15U);
        }
        return;
    }

    if (!store) {
        const unsigned start = within8 - element;
        switch (operation) {
            case 6: case 7: case 8: {  // LPV, LUV, LHV
                const unsigned stride = operation == 8 ? 2 : 1;
                const unsigned shift = operation == 6 ? 8 : 7;
                for (unsigned i = 0; i < vector.size(); ++i) {
                    vector[i] = static_cast<std::uint16_t>(read(aligned8 + ((start + i * stride) & 15)) << shift);
                }
                break;
            }
            case 9: {  // LFV
                Vector packed{};
                for (unsigned i = 0; i < packed.size(); ++i) {
                    const unsigned index = start + (i & 3) * 4 + (i & 4) * 2;
                    packed[i] = static_cast<std::uint16_t>(read(aligned8 + (index & 15)) << 7);
                }
                for (unsigned i = element; i < std::min(element + 8, 16U); ++i) {
                    set_byte(vector, i, byte(packed, i));
                }
                break;
            }
            case 11: {  // LTV transposes a row into a group of eight vector registers.
                const unsigned first = element + (address & 8);
                for (unsigned i = 0; i < 8; ++i) {
                    auto& row = registers_[(reg & ~7U) + (((element / 2) + i) & 7)];
                    const unsigned pos = first + 2 * i;
                    row[i] = static_cast<std::uint16_t>((read(aligned8 + (pos & 15)) << 8) |
                                                         read(aligned8 + ((pos + 1) & 15)));
                }
                break;
            }
            default: break;  // LWV has no effect on the N64 RSP.
        }
        return;
    }

    switch (operation) {
        case 6: case 7:  // SPV, SUV exchange which half uses each shift.
            for (unsigned i = 0; i < 8; ++i) {
                const unsigned pos = (element + i) & 15;
                const bool high_byte = (pos < 8) == (operation == 6);
                write(address + i, vector[pos & 7] >> (high_byte ? 8 : 7));
            }
            break;
        case 8:  // SHV
            for (unsigned i = 0; i < 8; ++i) {
                const unsigned pos = element + 2 * i;
                const unsigned value = (unsigned{byte(vector, pos)} << 1) | (byte(vector, pos + 1) >> 7);
                write(aligned8 + ((within8 + 2 * i) & 15), value);
            }
            break;
        case 9: {  // SFV uses a sparse lane map; other elements write zeros.
            constexpr std::array first_lane{0, 6, -1, -1, 1, 7, -1, -1, 4, -1, -1, 3, 5, -1, -1, 0};
            const int first = first_lane[element];
            for (unsigned i = 0; i < 4; ++i) {
                unsigned value = 0;
                if (first >= 0) {
                    const auto lane = static_cast<unsigned>(first);
                    value = vector[(lane & 4) | ((lane + i) & 3)] >> 7;
                }
                write(aligned8 + ((within8 + 4 * i) & 15), value);
            }
            break;
        }
        case 10:  // SWV
            for (unsigned i = 0; i < 16; ++i) write(aligned8 + ((within8 + i) & 15), byte(vector, element + i));
            break;
        case 11: {  // STV
            const unsigned first = element & ~1U;
            for (unsigned i = 0; i < 8; ++i) {
                const auto& row = registers_[(reg & ~7U) + i];
                const unsigned pos = 2 * i - first;
                write(aligned8 + ((within8 + pos) & 15), byte(row, pos));
                write(aligned8 + ((within8 + pos + 1) & 15), byte(row, pos + 1));
            }
            break;
        }
    }
}

}  // namespace ezy::n64
