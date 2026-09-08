// ============================================================================
//  VoxCast — ai/Modes.cpp
//  Pure, dependency-free mode helpers. Kept out of GeminiProvider.cpp so that
//  tests (and any future provider) can link them without pulling in HTTP.
// ============================================================================
#include "ITranscriptionProvider.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace vox::ai {

const char* toString(EnhancementMode m) {
    switch (m) {
        case EnhancementMode::Code:  return "Code";
        case EnhancementMode::Email: return "Email";
        case EnhancementMode::Chat:  return "Chat";
        case EnhancementMode::Notes: return "Notes";
        case EnhancementMode::Raw:   return "Raw";
        default:                     return "Auto";
    }
}

EnhancementMode modeFromFocusedApp(const std::string& id) {
    // Substring match against bundle ids (macOS) and executable names (Windows).
    static const std::vector<std::pair<const char*, EnhancementMode>> kMap = {
        {"vscode",          EnhancementMode::Code},
        {"code.exe",        EnhancementMode::Code},
        {"jetbrains",       EnhancementMode::Code},
        {"idea",            EnhancementMode::Code},
        {"iterm",           EnhancementMode::Code},
        {"terminal",        EnhancementMode::Code},
        {"windowsterminal", EnhancementMode::Code},
        {"powershell",      EnhancementMode::Code},
        {"xcode",           EnhancementMode::Code},
        {"sublime",         EnhancementMode::Code},
        {"neovim",          EnhancementMode::Code},

        {"mail",            EnhancementMode::Email},
        {"outlook",         EnhancementMode::Email},
        {"winword",         EnhancementMode::Email},
        {"word",            EnhancementMode::Email},
        {"docs.google",     EnhancementMode::Email},
        {"superhuman",      EnhancementMode::Email},
        {"sparkmail",       EnhancementMode::Email},

        {"slack",           EnhancementMode::Chat},
        {"discord",         EnhancementMode::Chat},
        {"messages",        EnhancementMode::Chat},
        {"teams",           EnhancementMode::Chat},
        {"whatsapp",        EnhancementMode::Chat},
        {"telegram",        EnhancementMode::Chat},
        {"signal",          EnhancementMode::Chat},

        {"notion",          EnhancementMode::Notes},
        {"obsidian",        EnhancementMode::Notes},
        {"bear",            EnhancementMode::Notes},
        {"craft",           EnhancementMode::Notes},
    };

    std::string lower;
    lower.reserve(id.size());
    for (char ch : id) lower += char(std::tolower((unsigned char)ch));

    for (const auto& [needle, mode] : kMap)
        if (lower.find(needle) != std::string::npos) return mode;

    return EnhancementMode::Notes;   // safest neutral default
}

} // namespace vox::ai
