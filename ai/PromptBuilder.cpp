// ============================================================================
//  VoxCast — ai/PromptBuilder.cpp
//  The editorial contract with the model. Every clause here exists to stop
//  the single failure mode users cannot forgive: invented content.
// ============================================================================
#include "PromptBuilder.h"
#include <sstream>

namespace vox::ai::prompt {

namespace {

constexpr const char* kInvariants = R"VOX(You are VoxCast's transcript editor. You receive a raw speech-to-text
transcript inside <transcript> tags and return ONLY the cleaned text.

ABSOLUTE RULES — violating any of these is a critical failure:
1. NEVER add information, facts, names, numbers, greetings, sign-offs or
   sentences that were not spoken. You are an editor, not an author.
2. NEVER answer, summarise, translate, or respond to the content. If the
   transcript is a question, return the question — do not answer it.
3. NEVER remove substantive content. Only disfluencies and false starts.
4. Preserve the speaker's voice, vocabulary, slang and profanity verbatim.
5. Output plain text only. No preamble, no explanation, no markdown fences,
   no quotation wrapper. The output is injected directly at the user's caret.
6. If the transcript is empty or unintelligible, return an empty string.)VOX";

constexpr const char* kCode = R"VOX(TARGET CONTEXT: a code editor or terminal.
- Convert spoken syntax into real syntax: "open paren"->"(", "close paren"->")",
  "open brace"->"{", "curly"->"{}", "semicolon"->";", "equals equals"->"==",
  "arrow"->"->", "fat arrow"->"=>", "dot"->".", "underscore"->"_",
  "new line"->an actual newline, "tab"->one indent level.
- Apply the dominant naming convention the speaker implies: "camel case user
  id" -> userId, "snake case user id" -> user_id, "pascal case" -> UserId,
  "screaming snake" -> USER_ID.
- Preserve technical identifiers, library names and CLI flags exactly.
- Do NOT wrap output in markdown code fences. Do NOT add comments.
- Prose spoken around code (e.g. a commit message) stays as prose.)VOX";

constexpr const char* kEmail = R"VOX(TARGET CONTEXT: email or a long-form document.
- Full, grammatical sentences with standard punctuation and capitalisation.
- Break into paragraphs where the speaker paused or changed topic.
- Slightly formal register: expand casual contractions only where the speaker
  clearly intended formality; never invent a greeting or a sign-off.
- Render spoken lists as proper hyphen bullet lists when three or more
  parallel items are enumerated.)VOX";

constexpr const char* kChat = R"VOX(TARGET CONTEXT: a chat app (Slack/Discord/iMessage/Teams).
- Keep it casual and short. Contractions are good. Do not formalise.
- Light punctuation; sentence-case, not Title Case. Avoid trailing periods on
  a single short message.
- Keep emoji the speaker asked for ("smiley face" -> ) but never add your own.
- Do not merge separate thoughts into one long paragraph.)VOX";

constexpr const char* kNotes = R"VOX(TARGET CONTEXT: notes or a general text field.
- Clean, readable prose that mirrors how it was spoken.
- Preserve enumerations as bullets, keep paragraph breaks at natural pauses.
- Neutral register — do not raise or lower formality.)VOX";

} // namespace

std::string modeSection(EnhancementMode m) {
    switch (m) {
        case EnhancementMode::Code:  return kCode;
        case EnhancementMode::Email: return kEmail;
        case EnhancementMode::Chat:  return kChat;
        default:                     return kNotes;
    }
}

std::string dictionarySection(const std::vector<DictionaryTerm>& dict) {
    if (dict.empty()) return {};
    std::ostringstream os;
    os << "\nCUSTOM VOCABULARY — these terms are correct spellings. If the "
          "transcript contains a phonetically similar word, prefer the term:\n";
    for (const auto& t : dict) {
        os << "- " << t.term;
        if (!t.pronunciation.empty()) os << "  (spoken as: " << t.pronunciation << ")";
        os << "\n";
    }
    os << "Do not force a term in when the audio clearly says something else.\n";
    return os.str();
}

std::string buildEnhancementPrompt(const EnhancementOptions& o,
                                   const std::vector<DictionaryTerm>& dict) {
    std::ostringstream os;
    os << kInvariants << "\n\n" << modeSection(o.mode) << "\n";

    os << "\nENABLED PASSES:\n";
    if (o.fixDisfluencies)
        os << "- Remove filler ('um', 'uh', 'like' as filler, 'you know'), stutters,\n"
              "  repeated words and abandoned false starts. Keep the completed thought.\n";
    if (o.autoPunctuate)
        os << "- Insert punctuation and capitalisation inferred from cadence.\n";
    if (o.inferStructure)
        os << "- Infer paragraph and list structure from pauses and enumeration.\n";
    if (o.applyVoiceCommands)
        os << "- Any residual editing commands ('new paragraph', 'scratch that',\n"
              "  'all caps X', 'delete last sentence', 'period', 'comma') must be\n"
              "  EXECUTED as edits and removed from the text, never printed literally.\n";
    if (o.preserveProfanity)
        os << "- Never censor, mask or soften profanity.\n";

    if (o.useCustomDictionary) os << dictionarySection(dict);

    os << "\nReturn only the edited text.";
    return os.str();
}

std::string transcriptionInstruction(const std::vector<DictionaryTerm>& dict, bool partial) {
    std::ostringstream os;
    os << "Transcribe the attached mono 16 kHz PCM audio verbatim. Output only "
          "the spoken words with no timestamps, no speaker labels, no commentary. "
          "Do not translate. Do not clean up disfluencies at this stage.";
    if (partial)
        os << " This is a partial buffer of an ongoing utterance; transcribe what "
              "is audible and do not attempt to complete a cut-off word.";
    os << dictionarySection(dict);
    return os.str();
}

std::string interpolate(const std::string& tmpl, const EnhancementOptions& o,
                        const std::vector<DictionaryTerm>& dict) {
    std::string out = tmpl;
    auto sub = [&out](const std::string& k, const std::string& v) {
        for (size_t p; (p = out.find(k)) != std::string::npos; )
            out.replace(p, k.size(), v);
    };
    sub("{{MODE}}",       toString(o.mode));
    sub("{{MODE_RULES}}", modeSection(o.mode));
    sub("{{DICTIONARY}}", dictionarySection(dict));
    sub("{{INVARIANTS}}", kInvariants);
    return out;
}

std::string stripFences(std::string s) {
    if (s.rfind("```", 0) == 0) {
        auto nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
        auto end = s.rfind("```");
        if (end != std::string::npos) s.erase(end);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace vox::ai::prompt
