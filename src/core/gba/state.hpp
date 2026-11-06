#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace ezy::gba {

namespace state_detail {
template <typename T>
inline constexpr bool kRaw = std::is_trivially_copyable_v<T> &&
                             (std::has_unique_object_representations_v<T> || std::is_floating_point_v<T>);
template <typename T>
struct IsArray : std::false_type {};
template <typename T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};
}  // namespace state_detail

// Save states: every component has one `template <class Ar> void serialize(Ar& ar)` that
// lists its state as `ar(member)`. The same function saves (StateWriter) and loads
// (StateReader), so the two can never disagree on the layout.
//
// Values without padding are copied as raw bytes; a struct with padding (whose bytes
// would be garbage) must have its own serialize(), which the archivers call. Values are
// copied as raw bytes (little-endian, as the bus already assumes), so a state
// is only meant to be loaded by the same build: Gba checks a version number for that.

class StateWriter {
public:
    static constexpr bool kLoading = false;

    template <typename T>
    void operator()(T& value) {
        if constexpr (state_detail::kRaw<T>) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            data_.insert(data_.end(), bytes, bytes + sizeof(T));
        } else if constexpr (state_detail::IsArray<T>::value) {
            for (auto& element : value) (*this)(element);
        } else {
            value.serialize(*this);  // a struct with padding: list its members
        }
    }
    void operator()(std::vector<std::uint8_t>& bytes) {
        auto size = static_cast<std::uint32_t>(bytes.size());
        (*this)(size);
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }

private:
    std::vector<std::uint8_t> data_;
};

// Reading past the end, or a vector whose stored size doesn't match, marks the reader
// failed; the values read after that are left as they were.
class StateReader {
public:
    static constexpr bool kLoading = true;

    explicit StateReader(std::span<const std::uint8_t> data) : data_(data) {}

    template <typename T>
    void operator()(T& value) {
        if constexpr (state_detail::kRaw<T>) {
            if (!take(&value, sizeof(T))) failed_ = true;
        } else if constexpr (state_detail::IsArray<T>::value) {
            for (auto& element : value) (*this)(element);
        } else {
            value.serialize(*this);
        }
    }
    // Vectors keep their size: a state from a different configuration (e.g. another
    // save chip size) fails instead of resizing.
    void operator()(std::vector<std::uint8_t>& bytes) {
        std::uint32_t size = 0;
        (*this)(size);
        if (failed_ || size != bytes.size() || !take(bytes.data(), size)) failed_ = true;
    }

    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] bool at_end() const { return pos_ == data_.size(); }

private:
    bool take(void* out, std::size_t size) {
        if (failed_ || data_.size() - pos_ < size) return false;
        if (size > 0) std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace ezy::gba
