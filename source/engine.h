#pragma once
#include "matcher.h"
#include <filesystem>
#include <string>
namespace mtga {
enum class Action { Status, Install, Restore };
struct Report {
    bool success = false;
    MatchStatus status = MatchStatus::Unsupported;
    std::string headline;
    std::string detail;
    bool can_install = false;
    bool can_restore = false;
};
Report execute(Action action, const std::filesystem::path& game_path);
}
