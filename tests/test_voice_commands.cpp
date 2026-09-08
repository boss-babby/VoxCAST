#include <catch2/catch_test_macros.hpp>
#include "ai/VoiceCommands.h"

using vox::ai::applyVoiceCommands;

TEST_CASE("new paragraph becomes a blank line", "[cmd]") {
    REQUIRE(applyVoiceCommands("first thought new paragraph second thought")
            == "first thought\n\nsecond thought");
}

TEST_CASE("scratch that deletes the previous sentence", "[cmd]") {
    auto out = applyVoiceCommands("Ship it Friday. Actually no. scratch that We ship Monday.");
    REQUIRE(out.find("Actually no") == std::string::npos);
    REQUIRE(out.find("We ship Monday") != std::string::npos);
    REQUIRE(out.find("Ship it Friday") != std::string::npos);
}

TEST_CASE("delete last N words", "[cmd]") {
    REQUIRE(applyVoiceCommands("alpha beta gamma delta delete last two words")
            == "alpha beta");
}

TEST_CASE("all caps uppercases only the next word", "[cmd]") {
    REQUIRE(applyVoiceCommands("this is all caps urgent and nothing else")
            == "this is URGENT and nothing else");
}

TEST_CASE("spoken punctuation attaches without a leading space", "[cmd]") {
    REQUIRE(applyVoiceCommands("hello there comma world period") == "hello there, world.");
}

TEST_CASE("paren commands produce real parens", "[cmd]") {
    REQUIRE(applyVoiceCommands("call open paren x close paren") == "call (x)");
}

TEST_CASE("command-free text is untouched apart from trimming", "[cmd]") {
    REQUIRE(applyVoiceCommands("  a perfectly ordinary sentence.  ")
            == "a perfectly ordinary sentence.");
}
