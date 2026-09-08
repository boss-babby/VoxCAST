// ============================================================================
//  VoxCast — ai/VoiceCommands.cpp
//  Deterministic, local, zero-latency editing commands. Runs BEFORE the LLM
//  pass so that destructive operations ("scratch that") can never be
//  hallucinated away or printed literally.
// ============================================================================
#include "VoiceCommands.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <vector>

namespace vox::ai {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return char(std::tolower(c)); });
    return s;
}

void deleteLastSentence(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
    auto pos = s.find_last_of(".!?\n");
    if (pos == std::string::npos) { s.clear(); return; }
    // Skip the terminator of the *previous* sentence.
    auto prev = s.find_last_of(".!?\n", pos ? pos - 1 : 0);
    s.erase(prev == std::string::npos ? 0 : prev + 1);
}

void deleteLastWords(std::string& s, int n) {
    for (int i = 0; i < n; ++i) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        auto pos = s.find_last_of(' ');
        if (pos == std::string::npos) { s.clear(); return; }
        s.erase(pos);
    }
}

struct Rule { std::regex re; std::string replacement; };

const std::vector<Rule>& literalRules() {
    static const std::vector<Rule> rules = {
        { std::regex(R"(\b(new paragraph|paragraph break)\b)", std::regex::icase), "\n\n" },
        { std::regex(R"(\b(new line|next line|line break)\b)", std::regex::icase), "\n"   },
        { std::regex(R"(\bnew bullet\b)",                      std::regex::icase), "\n- " },
        { std::regex(R"(\s*\b(full stop|period)\b)",           std::regex::icase), "."    },
        { std::regex(R"(\s*\bcomma\b)",                        std::regex::icase), ","    },
        { std::regex(R"(\s*\bquestion mark\b)",                std::regex::icase), "?"    },
        { std::regex(R"(\s*\bexclamation (mark|point)\b)",     std::regex::icase), "!"    },
        { std::regex(R"(\s*\bcolon\b)",                        std::regex::icase), ":"    },
        { std::regex(R"(\s*\bsemicolon\b)",                    std::regex::icase), ";"    },
        { std::regex(R"(\bopen paren(thesis)?\b\s*)",          std::regex::icase), "("    },
        { std::regex(R"(\s*\bclose paren(thesis)?\b)",         std::regex::icase), ")"    },
    };
    return rules;
}

} // namespace

std::string applyVoiceCommands(const std::string& input) {
    std::string s = input;

    // --- 1. Destructive commands, applied left-to-right ---------------------
    static const std::regex kScratch(
        R"(\b(scratch that|strike that|delete that|ignore that)\b)", std::regex::icase);
    static const std::regex kDelSentence(
        R"(\bdelete (the )?last sentence\b)", std::regex::icase);
    static const std::regex kDelWords(
        R"(\bdelete (the )?last (\w+) words?\b)", std::regex::icase);

    std::smatch m;
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 64) {
        changed = false;
        if (std::regex_search(s, m, kScratch)) {
            std::string head = s.substr(0, size_t(m.position()));
            std::string tail = s.substr(size_t(m.position() + m.length()));
            deleteLastSentence(head);
            s = head + tail; changed = true; continue;
        }
        if (std::regex_search(s, m, kDelSentence)) {
            std::string head = s.substr(0, size_t(m.position()));
            std::string tail = s.substr(size_t(m.position() + m.length()));
            deleteLastSentence(head);
            s = head + tail; changed = true; continue;
        }
        if (std::regex_search(s, m, kDelWords)) {
            static const std::vector<std::pair<const char*,int>> words = {
                {"one",1},{"two",2},{"three",3},{"four",4},{"five",5},
                {"six",6},{"seven",7},{"eight",8},{"nine",9},{"ten",10}};
            std::string numTok = lower(m[2].str());
            int n = 1;
            if (std::all_of(numTok.begin(), numTok.end(), ::isdigit)) n = std::stoi(numTok);
            else for (auto& [w, v] : words) if (numTok == w) n = v;
            std::string head = s.substr(0, size_t(m.position()));
            std::string tail = s.substr(size_t(m.position() + m.length()));
            deleteLastWords(head, n);
            s = head + tail; changed = true; continue;
        }
    }

    // --- 2. Case transforms -------------------------------------------------
    // Single pass: find "all caps <word>", uppercase the word, consume the
    // command phrase. (An earlier version ran a stripping regex first, which
    // silently ate the command before the uppercase pass could see it.)
    {
        static const std::regex kAllCaps(R"(\ball caps ([A-Za-z][\w'-]*))",
                                         std::regex::icase);
        std::string out, rest = s;
        std::smatch mm;
        while (std::regex_search(rest, mm, kAllCaps)) {
            out += rest.substr(0, size_t(mm.position()));
            std::string w = mm[1].str();
            std::transform(w.begin(), w.end(), w.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            out += w;
            rest = rest.substr(size_t(mm.position() + mm.length()));
        }
        s = out + rest;
    }

    // --- 3. Literal punctuation / structure substitutions -------------------
    for (const auto& r : literalRules()) s = std::regex_replace(s, r.re, r.replacement);

    // --- 4. Whitespace normalisation ---------------------------------------
    s = std::regex_replace(s, std::regex(R"([ \t]{2,})"), " ");
    s = std::regex_replace(s, std::regex(R"( +([,.!?;:]))"), "$1");
    s = std::regex_replace(s, std::regex(R"([ \t]*\n[ \t]*)"), "\n");
    s = std::regex_replace(s, std::regex(R"(\n{3,})"), "\n\n");
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\n')) s.pop_back();
    return s;
}

} // namespace vox::ai
