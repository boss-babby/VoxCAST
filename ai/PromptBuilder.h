#pragma once
#include "ITranscriptionProvider.h"
#include <string>
#include <vector>

namespace vox::ai::prompt {

/// Mode-specific editorial rules (Code / Email / Chat / Notes).
std::string modeSection(EnhancementMode);
/// Custom-vocabulary block injected into every request.
std::string dictionarySection(const std::vector<DictionaryTerm>&);
/// Full stage-2 system instruction.
std::string buildEnhancementPrompt(const EnhancementOptions&, const std::vector<DictionaryTerm>&);
/// Stage-1 verbatim transcription instruction.
std::string transcriptionInstruction(const std::vector<DictionaryTerm>&, bool partial);
/// Expands {{MODE}} / {{MODE_RULES}} / {{DICTIONARY}} / {{INVARIANTS}} in a
/// user-edited template from Settings → Modes.
std::string interpolate(const std::string& tmpl, const EnhancementOptions&,
                        const std::vector<DictionaryTerm>&);
/// Defensive: models occasionally wrap output in ``` fences.
std::string stripFences(std::string);

} // namespace vox::ai::prompt
