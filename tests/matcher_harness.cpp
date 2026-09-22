#include "matcher.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static const char* status_name(mtga::MatchStatus status) {
    switch (status) {
    case mtga::MatchStatus::Patchable: return "Patchable";
    case mtga::MatchStatus::AlreadyCompatible: return "AlreadyCompatible";
    default: return "Unsupported";
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("Provide one or more fixture paths.");
        for (int i = 1; i < argc; ++i) {
            const std::filesystem::path path(argv[i]);
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("Cannot read fixture: " + path.string());
            const std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(stream)),
                                                 std::istreambuf_iterator<char>());
            const auto result = mtga::inspect(image);
            bool roundtrip = true;
            if (result.status == mtga::MatchStatus::Patchable) {
                if (result.upper_offset > image.size() || image.size() - result.upper_offset < 4)
                    throw std::runtime_error("Accepted operand extends beyond image.");
                auto patched = image;
                static_assert(sizeof(float) == 4, "Four-byte IEEE float required.");
                std::uint32_t desired_bits = 0;
                std::memcpy(&desired_bits, &result.desired_upper, 4);
                for (std::size_t j = 0; j < 4; ++j)
                    patched[result.upper_offset + j] = static_cast<std::uint8_t>(desired_bits >> (j * 8));
                const auto patched_result = mtga::inspect(patched);
                roundtrip = patched_result.status == mtga::MatchStatus::AlreadyCompatible &&
                            patched_result.upper_offset == result.upper_offset &&
                            patched_result.original_upper == result.desired_upper &&
                            std::equal(image.begin(), image.begin() + result.upper_offset, patched.begin()) &&
                            std::equal(image.begin() + result.upper_offset + 4, image.end(),
                                       patched.begin() + result.upper_offset + 4);
                std::copy_n(image.begin() + result.upper_offset, 4,
                            patched.begin() + result.upper_offset);
                roundtrip = roundtrip && patched == image;
            }
            auto reason = result.reason;
            for (auto& c : reason) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            std::cout << path.filename().string() << '\t' << status_name(result.status) << '\t';
            if (result.upper_offset == std::numeric_limits<std::size_t>::max()) std::cout << -1;
            else std::cout << result.upper_offset;
            std::cout << '\t' << std::setprecision(9) << result.original_upper << '\t'
                      << result.lower << '\t' << (roundtrip ? 1 : 0) << '\t' << reason << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
