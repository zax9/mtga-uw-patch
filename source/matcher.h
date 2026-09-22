#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mtga {
enum class MatchStatus { Patchable, AlreadyCompatible, Unsupported };
struct MatchResult {
    MatchStatus status = MatchStatus::Unsupported;
    std::string reason = "Unrecognized assembly.";
    std::size_t upper_offset = std::numeric_limits<std::size_t>::max();
    float original_upper = 0, lower = 0, desired_upper = 2.4f;
    bool has_strong_name = false, has_authenticode = false;
    std::string assembly_name, assembly_version;
    std::uint32_t method_token = 0;
};
// Parses the supplied PE/CLI bytes as data. Never loads or executes the assembly.
MatchResult inspect(const std::vector<std::uint8_t>& image);
}
