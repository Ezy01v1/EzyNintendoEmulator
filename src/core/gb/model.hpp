#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ezy::gb {

// The hardware being emulated. Cgb means a CGB-E (the last Game Boy Color revision).
enum class Model : std::uint8_t { Dmg, Cgb };

// What the user asked for. Auto picks Cgb for every ROM: a DMG game then runs in the
// CGB's DMG compatibility mode, with colours, just like on a real Game Boy Color.
enum class ModelChoice : std::uint8_t { Auto, Dmg, Cgb };

struct Options {
    ModelChoice model = ModelChoice::Auto;
    // A dump of the real CGB boot ROM (optional). Without it the boot is simulated.
    std::optional<std::vector<std::uint8_t>> boot_rom;
};

[[nodiscard]] constexpr Model resolve_model(ModelChoice choice) {
    return choice == ModelChoice::Dmg ? Model::Dmg : Model::Cgb;
}

}  // namespace ezy::gb
