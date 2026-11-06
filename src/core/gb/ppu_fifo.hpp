#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ezy::gb {

// Pieces of the pixel-FIFO renderer (Pan Docs, "Pixel FIFO").

// A background/window pixel waiting in the BG FIFO.
struct FifoBgPixel {
    std::uint8_t color = 0;       // colour index 0-3, before palettes
    std::uint8_t palette = 0;     // CGB palette 0-7
    bool priority = false;        // CGB map attribute bit 7: BG over sprites
};

// A sprite pixel waiting in the OBJ FIFO.
struct FifoObjPixel {
    std::uint8_t color = 0;       // 0 = transparent
    std::uint8_t attributes = 0;  // OAM byte 3 (palette, priority, ...)
    std::uint8_t oam_index = 0;   // for CGB priority between overlapping sprites
};

// A small fixed-size FIFO. The BG FIFO holds up to 16 pixels, the OBJ FIFO 8.
template <class T, std::size_t N>
class PixelFifo {
public:
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    void clear() { head_ = size_ = 0; }
    void push(const T& pixel) {
        items_[(head_ + size_) % N] = pixel;
        ++size_;
    }
    T pop() {
        const T pixel = items_[head_];
        head_ = (head_ + 1) % N;
        --size_;
        return pixel;
    }
    // Entry `i` counted from the front (0 = next to pop). i < size().
    T& at(std::size_t i) { return items_[(head_ + i) % N]; }

    template <class Ar>
    void serialize(Ar& ar) {
        for (auto& item : items_) {
            ar(item.color);
            if constexpr (requires { item.palette; }) {
                ar(item.palette), ar(item.priority);
            } else {
                ar(item.attributes), ar(item.oam_index);
            }
        }
        ar(head_), ar(size_);
    }

private:
    std::array<T, N> items_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

// A sprite selected by the mode 2 OAM scan for the current line.
struct LineSprite {
    std::uint8_t y = 0, x = 0, tile = 0, attributes = 0, oam_index = 0;
    bool fetched = false;

    template <class Ar>
    void serialize(Ar& ar) {
        ar(y), ar(x), ar(tile), ar(attributes), ar(oam_index), ar(fetched);
    }
};

}  // namespace ezy::gb
