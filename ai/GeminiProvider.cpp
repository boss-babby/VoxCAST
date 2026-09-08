// ============================================================================
//  VoxCast — ai/GeminiProvider.cpp
//  Stage 1: streaming speech-to-text.  Stage 2: constrained editorial pass.
//  Networking is libcurl multi on a dedicated thread; the UI thread never
//  touches a socket.
// ============================================================================
#include "ITranscriptionProvider.h"
#include "PromptBuilder.h"
#include "VoiceCommands.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <curl/curl.h>

#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>

using json = nlohmann::json;
using clock_t_ = std::chrono::steady_clock;

namespace vox::ai {

namespace {

size_t writeCb(char* ptr, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, sz * nm);
    return sz * nm;
}

/// Server-sent-events splitter for `:streamGenerateContent?alt=sse`.
struct SseAccumulator {
    std::string buf;
    std::function<void(const json&)> onEvent;
    size_t feed(char* ptr, size_t n) {
        buf.append(ptr, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.rfind("data: ", 0) != 0) continue;
            std::string payload = line.substr(6);
            if (payload == "[DONE]") continue;
            try { onEvent(json::parse(payload)); }
            catch (const std::exception& e) { spdlog::warn("SSE parse: {}", e.what()); }
        }
        return n;
    }
};
size_t sseCb(char* ptr, size_t sz, size_t nm, void* ud) {
    return static_cast<SseAccumulator*>(ud)->feed(ptr, sz * nm);
}

std::string base64(const std::vector<int16_t>& pcm) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* b = reinterpret_cast<const unsigned char*>(pcm.data());
    size_t len = pcm.size() * sizeof(int16_t);
    std::string out; out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = b[i] << 16;
        if (i + 1 < len) v |= b[i + 1] << 8;
        if (i + 2 < len) v |= b[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? T[v & 63]        : '=';
    }
    return out;
}

/// Extracts concatenated text parts from a Gemini candidate chunk.
std::string extractText(const json& j) {
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

} // namespace

// ===========================================================================
class GeminiProvider final : public ITranscriptionProvider {
public:
    explicit GeminiProvider(ProviderConfig cfg) : cfg_(std::move(cfg)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~GeminiProvider() override { abort(); }

    const char* name() const override { return "Gemini"; }
    void setConfig(const ProviderConfig& c) override { std::lock_guard l(m_); cfg_ = c; }
    void setDictionary(std::vector<DictionaryTerm> d) override { std::lock_guard l(m_); dict_ = std::move(d); }

    bool beginStream(const EnhancementOptions& opt, PartialFn onPartial) override {
        std::lock_guard l(m_);
        if (cfg_.apiKey.empty()) { spdlog::error("Gemini: missing API key"); return false; }
        opts_ = opt; partial_ = std::move(onPartial);
        pcm_.clear(); pcm_.reserve(size_t(cfg_.sampleRate) * 30);
        rawPartial_.clear();
        t0_ = clock_t_::now();
        return true;
    }

    void pushAudio(const int16_t* pcm, size_t frames) override {
        std::lock_guard l(m_);
        pcm_.insert(pcm_.end(), pcm, pcm + frames);
        // Emit a partial roughly every 900 ms of audio for perceived latency.
        const size_t chunk = size_t(cfg_.sampleRate * 0.9);
        if (cfg_.streamPartials && pcm_.size() - lastPartialAt_ >= chunk) {
            lastPartialAt_ = pcm_.size();
            std::vector<int16_t> snapshot = pcm_;
            auto opts = opts_; auto cb = partial_;
            std::thread([this, snapshot = std::move(snapshot), cb]() mutable {
                std::string txt = transcribeBlocking(snapshot, /*partial*/true);
                if (!txt.empty() && cb) cb(TranscriptSegment{txt, false, 0.f, 0.0, 0.0});
            }).detach();
        }
    }

    void endStream(DoneFn done, std::shared_ptr<CancellationToken> tok) override {
        std::vector<int16_t> audio;
        EnhancementOptions opt;
        { std::lock_guard l(m_); audio.swap(pcm_); opt = opts_; }

        std::thread([this, audio = std::move(audio), opt, done = std::move(done), tok]() mutable {
            PipelineResult r;
            auto tA = clock_t_::now();

            if (tok && tok->cancelled()) { r.error = "cancelled"; done(r); return; }

            // ---- Stage 1 ---------------------------------------------------
            r.rawTranscript = transcribeBlocking(audio, /*partial*/false);
            r.transcribeMs  = std::chrono::duration<double, std::milli>(clock_t_::now() - tA).count();
            if (r.rawTranscript.empty()) { r.error = "empty transcript"; done(r); return; }
            if (tok && tok->cancelled()) { r.error = "cancelled"; done(r); return; }

            // ---- Local voice-command pass (deterministic, pre-LLM) ---------
            std::string staged = r.rawTranscript;
            if (opt.applyVoiceCommands) staged = applyVoiceCommands(staged);

            // ---- Stage 2 ---------------------------------------------------
            auto tB = clock_t_::now();
            if (opt.mode == EnhancementMode::Raw) {
                r.enhancedText = staged;
            } else {
                r.enhancedText = enhanceBlocking(staged, opt);
                if (r.enhancedText.empty()) r.enhancedText = staged;  // graceful degrade
            }
            r.enhanceMs   = std::chrono::duration<double, std::milli>(clock_t_::now() - tB).count();
            r.appliedMode = opt.mode;
            r.wordCount   = int(std::count(r.enhancedText.begin(), r.enhancedText.end(), ' ')) + 1;
            r.ok          = true;
            spdlog::info("pipeline ok: stt={:.0f}ms enhance={:.0f}ms words={}",
                         r.transcribeMs, r.enhanceMs, r.wordCount);
            done(std::move(r));
        }).detach();
    }

    void abort() override {
        std::lock_guard l(m_);
        aborted_.store(true);
        pcm_.clear();
    }

    bool testConnection(std::string& msg) override {
        ProviderConfig cfg; { std::lock_guard l(m_); cfg = cfg_; }
        if (cfg.apiKey.empty()) { msg = "No API key set."; return false; }
        json body = {{"contents", json::array({
            {{"parts", json::array({ {{"text","ping"}} })}} })}};
        std::string resp;
        long code = post(cfg.endpointBase + "/models/" + cfg.enhanceModel +
                         ":generateContent", cfg.apiKey, body.dump(), resp);
        if (code == 200) { msg = "Connected — " + cfg.enhanceModel + " reachable."; return true; }
        msg = "HTTP " + std::to_string(code) + ": " + resp.substr(0, 180);
        return false;
    }

private:
    // -----------------------------------------------------------------------
    std::string transcribeBlocking(const std::vector<int16_t>& pcm, bool partial) {
        ProviderConfig cfg; std::vector<DictionaryTerm> dict;
        { std::lock_guard l(m_); cfg = cfg_; dict = dict_; }
        if (pcm.empty()) return {};

        json body = {
            {"contents", json::array({{
                {"role", "user"},
                {"parts", json::array({
                    {{"text", prompt::transcriptionInstruction(dict, partial)}},
                    {{"inline_data", {
                        {"mime_type", "audio/l16;rate=" + std::to_string(cfg.sampleRate)},
                        {"data", base64(pcm)}}}}
                })}
            }})},
            {"generationConfig", {{"temperature", 0.0}, {"candidateCount", 1}}}
        };

        std::string resp;
        long code = post(cfg.endpointBase + "/models/" + cfg.transcribeModel +
                         ":generateContent", cfg.apiKey, body.dump(), resp);
        if (code != 200) { spdlog::error("STT HTTP {}: {}", code, resp.substr(0,200)); return {}; }
        try { return trim(extractText(json::parse(resp))); }
        catch (const std::exception& e) { spdlog::error("STT parse: {}", e.what()); return {}; }
    }

    // -----------------------------------------------------------------------
    std::string enhanceBlocking(const std::string& raw, const EnhancementOptions& opt) {
        ProviderConfig cfg; std::vector<DictionaryTerm> dict;
        { std::lock_guard l(m_); cfg = cfg_; dict = dict_; }

        const std::string sys = opt.promptOverride.empty()
            ? prompt::buildEnhancementPrompt(opt, dict)
            : prompt::interpolate(opt.promptOverride, opt, dict);

        json body = {
            {"system_instruction", {{"parts", json::array({ {{"text", sys}} })}}},
            {"contents", json::array({{
                {"role","user"},
                {"parts", json::array({ {{"text", "<transcript>\n" + raw + "\n</transcript>"}} })}
            }})},
            {"generationConfig", {
                {"temperature", cfg.temperature},
                {"topP", 0.9},
                {"maxOutputTokens", 2048},
                {"responseMimeType", "text/plain"}}}
        };

        std::string resp;
        long code = post(cfg.endpointBase + "/models/" + cfg.enhanceModel +
                         ":generateContent", cfg.apiKey, body.dump(), resp);
        if (code != 200) { spdlog::error("enhance HTTP {}: {}", code, resp.substr(0,200)); return {}; }
        try { return prompt::stripFences(trim(extractText(json::parse(resp)))); }
        catch (const std::exception& e) { spdlog::error("enhance parse: {}", e.what()); return {}; }
    }

    // -----------------------------------------------------------------------
    long post(const std::string& url, const std::string& key,
              const std::string& body, std::string& out) {
        CURL* c = curl_easy_init();
        if (!c) return -1;
        curl_slist* h = nullptr;
        h = curl_slist_append(h, "Content-Type: application/json");
        h = curl_slist_append(h, ("x-goog-api-key: " + key).c_str());
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body.size()));
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, long(cfg_.requestTimeoutMs));
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "gzip");
        CURLcode rc = curl_easy_perform(c);
        long code = -1;
        if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        else out = curl_easy_strerror(rc);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
        return code;
    }

    static std::string trim(std::string s) {
        const char* ws = " \t\r\n";
        auto b = s.find_first_not_of(ws);
        if (b == std::string::npos) return {};
        return s.substr(b, s.find_last_not_of(ws) - b + 1);
    }

    std::mutex m_;
    ProviderConfig cfg_;
    EnhancementOptions opts_;
    std::vector<DictionaryTerm> dict_;
    std::vector<int16_t> pcm_;
    size_t lastPartialAt_{0};
    std::string rawPartial_;
    PartialFn partial_;
    std::atomic<bool> aborted_{false};
    clock_t_::time_point t0_;
};

std::unique_ptr<ITranscriptionProvider> makeGeminiProvider(const ProviderConfig& cfg) {
    return std::make_unique<GeminiProvider>(cfg);
}

// toString() and modeFromFocusedApp() live in ai/Modes.cpp (no HTTP dependency).

} // namespace vox::ai
