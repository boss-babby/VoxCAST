// ============================================================================
//  Parsing of real Gemini response envelopes, including the malformed and
//  hostile shapes that have actually shown up in production.
// ============================================================================
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <nlohmann/json.hpp>
#include "ai/PromptBuilder.h"
#include "ai/ITranscriptionProvider.h"

using json = nlohmann::json;
using namespace vox::ai;

// Mirrors the extractText() helper in GeminiProvider.cpp.
static std::string extractText(const json& j) {
    std::string out;
    if (!j.contains("candidates")) return out;
    for (const auto& c : j["candidates"]) {
        if (!c.contains("content") || !c["content"].contains("parts")) continue;
        for (const auto& p : c["content"]["parts"]) {
            if (p.contains("text") && p["text"].is_string()) {
                out += p["text"].get<std::string>();
            } else if (p.contains("audioTranscription")) {
                const auto& at = p["audioTranscription"];
                if (at.is_string()) {
                    out += at.get<std::string>();
                } else if (at.is_object() && at.contains("text") && at["text"].is_string()) {
                    out += at["text"].get<std::string>();
                }
            }
        }
    }
    return out;
}

TEST_CASE("parses audioTranscription response from gemini-3.5-transcribe", "[json]") {
    auto j = json::parse(R"({
      "candidates":[{"content":{"parts":[{"audioTranscription":{"text":"Hello, can you hear me?"}}],
                     "role":"model"},"finishReason":"STOP","index":0}],
      "usageMetadata":{"promptTokenCount":226,"totalTokenCount":226}
    })");
    REQUIRE(extractText(j) == "Hello, can you hear me?");
}

TEST_CASE("parses a standard single-candidate response", "[json]") {
    auto j = json::parse(R"({
      "candidates":[{"content":{"parts":[{"text":"hey team, the deploy is green"}],
                     "role":"model"},"finishReason":"STOP","index":0}],
      "usageMetadata":{"promptTokenCount":42,"candidatesTokenCount":9}
    })");
    REQUIRE(extractText(j) == "hey team, the deploy is green");
    REQUIRE(j["usageMetadata"]["promptTokenCount"].get<int>() == 42);
}

TEST_CASE("concatenates multi-part responses", "[json]") {
    auto j = json::parse(R"({"candidates":[{"content":{"parts":[
        {"text":"const userId = "},{"text":"await getUser(id);"}]}}]})");
    REQUIRE(extractText(j) == "const userId = await getUser(id);");
}

TEST_CASE("empty candidates yields empty string, not a throw", "[json]") {
    REQUIRE(extractText(json::parse(R"({"candidates":[]})")).empty());
    REQUIRE(extractText(json::parse(R"({})")).empty());
    REQUIRE(extractText(json::parse(R"({"candidates":[{"finishReason":"SAFETY"}]})")).empty());
}

TEST_CASE("a parts entry without text is skipped", "[json]") {
    auto j = json::parse(R"({"candidates":[{"content":{"parts":[
        {"inlineData":{"mimeType":"audio/l16"}},{"text":"ok"}]}}]})");
    REQUIRE(extractText(j) == "ok");
}

TEST_CASE("error envelope is detected", "[json]") {
    auto j = json::parse(R"({"error":{"code":429,
        "message":"Resource has been exhausted","status":"RESOURCE_EXHAUSTED"}})");
    REQUIRE(j.contains("error"));
    REQUIRE(j["error"]["code"].get<int>() == 429);
    REQUIRE(extractText(j).empty());
}

TEST_CASE("unicode and emoji survive a round trip", "[json]") {
    const std::string s = "café — naïve 🎙️ 日本語";
    json j = {{"candidates", json::array({
        {{"content", {{"parts", json::array({ {{"text", s}} })}}}} })}};
    REQUIRE(extractText(json::parse(j.dump())) == s);
}

TEST_CASE("malformed json throws and is catchable", "[json]") {
    REQUIRE_THROWS(json::parse("{not json"));
}

TEST_CASE("SSE data lines split correctly", "[json]") {
    const std::string stream =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hey \"}]}}]}\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"team\"}]}}]}\n"
        "data: [DONE]\n";
    std::string acc, buf = stream;
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (line.rfind("data: ", 0) != 0) continue;
        std::string payload = line.substr(6);
        if (payload == "[DONE]") continue;
        acc += extractText(json::parse(payload));
    }
    REQUIRE(acc == "hey team");
}

TEST_CASE("stripFences removes markdown code fences", "[json]") {
    REQUIRE(prompt::stripFences("```cpp\nint x = 1;\n```") == "int x = 1;");
    REQUIRE(prompt::stripFences("plain text") == "plain text");
    REQUIRE(prompt::stripFences("```\nhello\n```") == "hello");
}

TEST_CASE("transcription request body has the expected shape", "[json]") {
    json body = {
        {"contents", json::array({{
            {"role", "user"},
            {"parts", json::array({
                {{"text", "transcribe"}},
                {{"inline_data", {{"mime_type","audio/l16;rate=16000"},{"data","AAAA"}}}}
            })}
        }})},
        {"generationConfig", {{"temperature", 0.0}, {"candidateCount", 1}}}
    };
    REQUIRE(body["contents"][0]["parts"].size() == 2);
    REQUIRE(body["contents"][0]["parts"][1]["inline_data"]["mime_type"]
            == "audio/l16;rate=16000");
    REQUIRE(body["generationConfig"]["temperature"].get<double>()
            == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("mode is inferred from the focused application id", "[json][modes]") {
    REQUIRE(modeFromFocusedApp("com.microsoft.VSCode")   == EnhancementMode::Code);
    REQUIRE(modeFromFocusedApp("Code.exe")               == EnhancementMode::Code);
    REQUIRE(modeFromFocusedApp("com.tinyspeck.slackmacgap") == EnhancementMode::Chat);
    REQUIRE(modeFromFocusedApp("OUTLOOK.EXE")            == EnhancementMode::Email);
    REQUIRE(modeFromFocusedApp("md.obsidian")            == EnhancementMode::Notes);
    REQUIRE(modeFromFocusedApp("some.unknown.app")       == EnhancementMode::Notes);
}

TEST_CASE("enhancement prompt carries the anti-fabrication invariants", "[prompt]") {
    EnhancementOptions o;
    o.mode = EnhancementMode::Chat;
    std::vector<DictionaryTerm> dict{{"Kubernetes", "koo-ber-net-eez", true}};
    const std::string p = prompt::buildEnhancementPrompt(o, dict);
    REQUIRE(p.find("NEVER add information") != std::string::npos);
    REQUIRE(p.find("editor, not an author") != std::string::npos);
    REQUIRE(p.find("Kubernetes") != std::string::npos);
    REQUIRE(p.find("koo-ber-net-eez") != std::string::npos);
    REQUIRE(p.find("casual") != std::string::npos);   // chat mode section present
}

TEST_CASE("prompt template interpolation expands placeholders", "[prompt]") {
    EnhancementOptions o;
    o.mode = EnhancementMode::Code;
    const std::string out = prompt::interpolate("MODE={{MODE}}\n{{DICTIONARY}}", o, {});
    REQUIRE(out.find("MODE=Code") != std::string::npos);
    REQUIRE(out.find("{{MODE}}") == std::string::npos);
}
