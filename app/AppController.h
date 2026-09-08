#pragma once
#include "../ai/ITranscriptionProvider.h"
#include "../audio/RingBuffer.h"
#include "../audio/Vad.h"
#include "../db/HistoryStore.h"
#include "../platform/Platform.h"
#include "../ui/PopupView.h"
#include "../ui/SkiaRenderer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vox::app {

enum class PopupAnchor { Cursor, BottomCenter };
enum class SessionState { Idle, Listening, Processing };

struct Config {
    platform::HotkeyBinding binding{};
    ai::EnhancementOptions  enhancement{};
    PopupAnchor popupAnchor{PopupAnchor::BottomCenter};
    std::string inputDeviceId{"default"};
    std::string dataDir{"."};
    std::string transcribeModel{"gemini-transcribe-latest"};
    std::string enhanceModel{"gemini-flash-latest"};
    bool allowPasteFallback{true};
};

class AppController {
public:
    static constexpr uint32_t kEscape = 0x35;   // macOS kVK_Escape (Win: VK_ESCAPE)

    explicit AppController(Config);
    ~AppController();

    bool start();
    void shutdown();

    void beginDictation();
    void finishDictation();
    void cancel();
    void setPaused(bool);
    bool paused() const { return paused_; }

    db::HistoryStore& history() { return *history_; }
    ai::ITranscriptionProvider& provider() { return *provider_; }

    std::function<void()> onOpenDashboard, onQuit;

private:
    void onHotkey(platform::HotkeyEdge);
    void onAudioFrames(const float*, size_t);   // real-time thread
    void deliver(const ai::PipelineResult&);
    void renderFrame();                         // ui thread

    Config cfg_;
    std::unique_ptr<platform::ISecretStore>    secrets_;
    std::unique_ptr<platform::IAudioEngine>    audio_;
    std::unique_ptr<platform::IHotkeyService>  hotkey_;
    std::unique_ptr<platform::IInputInjector>  injector_;
    std::unique_ptr<platform::IClipboard>      clipboard_;
    std::unique_ptr<platform::IAccessibility>  ax_;
    std::unique_ptr<platform::ITray>           tray_;
    std::unique_ptr<platform::IPlatformWindow> popupWindow_;
    std::unique_ptr<ui::SkiaRenderer>          renderer_;
    std::unique_ptr<ui::PopupView>             popup_;
    std::unique_ptr<ai::ITranscriptionProvider> provider_;
    std::unique_ptr<db::HistoryStore>          history_;

    audio::SpscRingBuffer<float> levelRing_{1u << 15};
    audio::Vad  vad_;
    std::vector<float>   vadCarry_;
    std::vector<int16_t> pcmScratch_;

    std::mutex uiMutex_;
    std::string pendingPartial_;
    ai::PipelineResult pendingResult_;
    bool hasResult_{false};
    std::atomic<bool>  endpointRequested_{false};
    std::atomic<float> rmsAtomic_{0.f};

    std::shared_ptr<ai::CancellationToken> cancelToken_;
    platform::FocusInfo   focus_;
    ai::EnhancementOptions activeOptions_;
    SessionState session_{SessionState::Idle};
    bool paused_{false}, toggleLatched_{false};
    std::chrono::steady_clock::time_point pressT0_{}, lastFrame_{};
};

} // namespace vox::app
