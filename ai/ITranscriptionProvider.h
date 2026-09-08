// ============================================================================
//  VoxCast — ai/ITranscriptionProvider.h
//  Provider-agnostic contract for the two-stage pipeline. Gemini is the
//  default implementation; anything satisfying this interface (Whisper,
//  Deepgram, on-device) can be dropped in via ProviderRegistry.
// ============================================================================
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vox::ai {

// --------------------------------------------------------------------------
enum class EnhancementMode { Auto, Code, Email, Chat, Notes, Raw };

const char* toString(EnhancementMode);
EnhancementMode modeFromFocusedApp(const std::string& bundleIdOrExe);

// --------------------------------------------------------------------------
struct DictionaryTerm {
    std::string term;            // "Kubernetes"
    std::string pronunciation;   // "koo-ber-net-eez"  (optional hint)
    bool        caseSensitive{true};
};

struct EnhancementOptions {
    bool fixDisfluencies      = true;
    bool autoPunctuate        = true;
    bool inferStructure       = true;   // paragraphs / lists
    bool applyVoiceCommands   = true;   // "new paragraph", "scratch that"…
    bool useCustomDictionary  = true;
    bool preserveProfanity    = true;   // never silently censor the user
    EnhancementMode mode      = EnhancementMode::Auto;
    std::string     promptOverride;     // user-edited template, may be empty
};

struct ProviderConfig {
    std::string endpointBase   = "https://generativelanguage.googleapis.com/v1beta";
    std::string transcribeModel= "gemini-transcribe-latest";
    std::string enhanceModel   = "gemini-flash-latest";
    std::string apiKey;                 // injected from ISecretStore, never on disk
    int   sampleRate           = 16000;
    int   requestTimeoutMs     = 15000;
    bool  streamPartials       = true;
    float temperature          = 0.15f; // low: we are editing, not authoring
};

// --------------------------------------------------------------------------
struct TranscriptSegment {
    std::string text;
    bool  isFinal{false};
    float confidence{0.f};
    double startSec{0.0}, endSec{0.0};
};

struct PipelineResult {
    std::string rawTranscript;
    std::string enhancedText;
    EnhancementMode appliedMode{EnhancementMode::Notes};
    int    wordCount{0};
    double transcribeMs{0.0}, enhanceMs{0.0};
    bool   ok{false};
    std::string error;
};

/// Cooperative cancellation shared with the UI (Esc cancels in-flight work).
class CancellationToken {
public:
    void cancel() { flag_.store(true, std::memory_order_release); }
    void reset()  { flag_.store(false, std::memory_order_release); }
    bool cancelled() const { return flag_.load(std::memory_order_acquire); }
private:
    std::atomic<bool> flag_{false};
};

// --------------------------------------------------------------------------
class ITranscriptionProvider {
public:
    using PartialFn = std::function<void(const TranscriptSegment&)>;
    using DoneFn    = std::function<void(PipelineResult)>;

    virtual ~ITranscriptionProvider() = default;
    virtual const char* name() const = 0;

    /// Opens a streaming session. Audio is pushed in as it is captured.
    virtual bool beginStream(const EnhancementOptions&, PartialFn) = 0;
    virtual void pushAudio(const int16_t* pcm16, size_t frames) = 0;
    /// Closes stage 1, runs stage 2, invokes DoneFn on the network thread.
    virtual void endStream(DoneFn, std::shared_ptr<CancellationToken>) = 0;
    virtual void abort() = 0;

    /// Settings → "Test connection".
    virtual bool testConnection(std::string& outMessage) = 0;

    virtual void setConfig(const ProviderConfig&) = 0;
    virtual void setDictionary(std::vector<DictionaryTerm>) = 0;
};

std::unique_ptr<ITranscriptionProvider> makeGeminiProvider(const ProviderConfig&);

} // namespace vox::ai
