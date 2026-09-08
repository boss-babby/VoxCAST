#pragma once
#include <string>

namespace vox::ai {
/// Executes inline spoken editing commands ("new paragraph", "scratch that",
/// "all caps kubernetes", "delete last three words", "open paren"…) and
/// returns the resulting text with the command phrases consumed.
/// Pure function — unit tested in tests/test_voice_commands.cpp.
std::string applyVoiceCommands(const std::string& input);
}
