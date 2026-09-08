// ============================================================================
//  VoxCast — app/AppController.cpp
//  Wires hotkey → audio → popup → Gemini → injection → clipboard → history.
//  Owns the thread topology; nothing else in the app spawns threads.
// ============================================================================
#include "AppController.h"

#include <spdlog/spdlog.h>
#include <chrono>

namespace vox::app {

using namespace std::chrono;
using vox::ui::PopupState;

AppController::AppController(Config cfg) : cfg_(std::move(cfg)) {
    secrets_ = platform::ISecretStore::create();
    audio_   = platform::IAudioEngine::create();
    hotkey_  = platform::IHotkeyService::create();
    injector_= platform::IInputInjector::create();
    clipboard_= platform::IClipboard::create();
    ax_      = platform::IAccessibility::create();
    tray_    = platform::ITray::create();
    history_ = std::make_unique<db::HistoryStore>(cfg_.dataDir + "/voxcast.db");

    ai::ProviderConfig pc;
    pc.transcribeModel = cfg_.transcribeModel;
    pc.enhanceModel    = cfg_.enhanceModel;
    if (auto k = secrets_->load("gemini_api_key")) pc.apiKey = *k;
    provider_ = ai::makeGeminiProvider(pc);
    provider_->setDictionary(history_->loadDictionary());
}

AppController::~AppController() { shutdown(); }

// ---------------------------------------------------------------------------
bool AppController::start() {
    if (!hotkey_->hasAccessibilityPermission()) {
        hotkey_->requestAccessibilityPermission();
        tray_->notify("VoxCast needs permission",
                      "Enable Accessibility so VoxCast can hear your hotkey and type for you.");
    }

    platform::WindowDesc d;
    d.width = 480; d.height = 160;      // canvas is larger than the capsule
    d.borderless = true; d.alwaysOnTop = true; d.nativeBlur = true;
    d.showInTaskbar = false;
    popupWindow_ = platform::IPlatformWindow::create(d);
    renderer_    = std::make_unique<ui::SkiaRenderer>(popupWindow_->nativeSurface(),
                                                      popupWindow_->backingScale());
    popup_ = std::make_unique<ui::PopupView>();
    popup_->setTypeface(renderer_->uiTypeface());
    popup_->onDismissed = [this] { popupWindow_->hide(); };

    popupWindow_->onDisplayLink = [this] { renderFrame(); };
    popupWindow_->onKey = [this](platform::KeyEvent e) {
        if (e.down && e.keyCode == kEscape) cancel();
    };

    hotkey_->bind(cfg_.binding, [this](platform::HotkeyEdge edge) { onHotkey(edge); });

    tray_->setMenu({
        {"Start Dictation", [this]{ beginDictation(); }},
        {"Open Dashboard",  [this]{ if (onOpenDashboard) onOpenDashboard(); }},
        {"Pause VoxCast",   [this]{ setPaused(!paused_); }},
        {"Quit",            [this]{ if (onQuit) onQuit(); }},
    });
    tray_->setState(platform::TrayState::Idle);
    spdlog::info("VoxCast ready — hotkey: {}", cfg_.binding.display);
    return true;
}

void AppController::shutdown() {
    if (audio_ && audio_->running()) audio_->stop();
    if (hotkey_) hotkey_->unbind();
    if (provider_) provider_->abort();
}

// ---------------------------------------------------------------------------
void AppController::onHotkey(platform::HotkeyEdge edge) {
    if (paused_) return;
    const auto mode = cfg_.binding.mode;

    if (edge == platform::HotkeyEdge::Down) {
        pressT0_ = steady_clock::now();
        if (session_ == SessionState::Idle) beginDictation();
        else if (mode != platform::HotkeyMode::HoldToTalk) finishDictation();
    } else { // Up
        const auto held = duration_cast<milliseconds>(steady_clock::now() - pressT0_).count();
        // < 320 ms = a tap → latch into toggle mode; longer = push-to-talk release.
        const bool tap = held < 320;
        if (mode == platform::HotkeyMode::Toggle) return;
        if (mode == platform::HotkeyMode::Both && tap) { toggleLatched_ = true; return; }
        if (session_ == SessionState::Listening) finishDictation();
    }
}

// ---------------------------------------------------------------------------
void AppController::beginDictation() {
    if (session_ != SessionState::Idle) return;
    session_ = SessionState::Listening;
    cancelToken_ = std::make_shared<ai::CancellationToken>();

    // Resolve target app *now*, before our popup can steal focus.
    focus_ = ax_->focused();
    ai::EnhancementOptions opt = cfg_.enhancement;
    if (opt.mode == ai::EnhancementMode::Auto)
        opt.mode = ai::modeFromFocusedApp(focus_.bundleIdOrExe);
    activeOptions_ = opt;

    // --- Show popup first: perceived latency is what ships ------------------
    if (cfg_.popupAnchor == PopupAnchor::Cursor) popupWindow_->positionNearCursor(-52);
    else                                        popupWindow_->centerBottomOnActiveScreen(96);
    popupWindow_->show();
    popup_->setPartialTranscript({});
    popup_->setState(PopupState::Listening);
    tray_->setState(platform::TrayState::Listening);

    provider_->beginStream(opt, [this](const ai::TranscriptSegment& s) {
        std::lock_guard l(uiMutex_);
        pendingPartial_ = s.text;            // consumed on the render thread
    });

    vad_.reset();
    audio_->start(cfg_.inputDeviceId, [this](const float* mono, size_t frames) {
        onAudioFrames(mono, frames);         // REAL-TIME THREAD
    });
}

// --- real-time audio thread -------------------------------------------------
void AppController::onAudioFrames(const float* mono, size_t frames) {
    levelRing_.write(mono, frames);          // lock-free → waveform

    // int16 conversion straight into the network staging buffer
    pcmScratch_.resize(frames);
    for (size_t i = 0; i < frames; ++i)
        pcmScratch_[i] = int16_t(std::clamp(mono[i], -1.f, 1.f) * 32767.f);
    provider_->pushAudio(pcmScratch_.data(), frames);

    // VAD in 20 ms frames
    vadCarry_.insert(vadCarry_.end(), mono, mono + frames);
    const size_t N = size_t(vad_.config().frameSamples);
    while (vadCarry_.size() >= N) {
        auto r = vad_.process(vadCarry_.data(), N);
        vadCarry_.erase(vadCarry_.begin(), vadCarry_.begin() + long(N));
        rmsAtomic_.store(r.rms, std::memory_order_relaxed);
        if (r.endpoint && (toggleLatched_ || cfg_.binding.mode == platform::HotkeyMode::Toggle))
            endpointRequested_.store(true, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
void AppController::finishDictation() {
    if (session_ != SessionState::Listening) return;
    session_ = SessionState::Processing;
    toggleLatched_ = false;
    audio_->stop();
    popup_->setState(PopupState::Processing);
    tray_->setState(platform::TrayState::Processing);

    provider_->endStream([this](ai::PipelineResult r) {
        // NETWORK THREAD — marshal to UI thread via the pending slot.
        std::lock_guard l(uiMutex_);
        pendingResult_ = std::move(r);
        hasResult_ = true;
    }, cancelToken_);
}

void AppController::deliver(const ai::PipelineResult& r) {
    session_ = SessionState::Idle;
    if (!r.ok || r.enhancedText.empty()) {
        popup_->setState(PopupState::Cancelled);
        tray_->setState(r.error == "cancelled" ? platform::TrayState::Idle
                                               : platform::TrayState::Error);
        return;
    }

    // Clipboard first: it is the guaranteed fallback and must never race.
    clipboard_->setText(r.enhancedText);

    bool injected = false;
    if (focus_.hasEditableFocus) injected = injector_->typeText(r.enhancedText);
    if (!injected && cfg_.allowPasteFallback) injected = injector_->pasteFromClipboard();

    popup_->setResultLabel(injected
        ? std::to_string(r.wordCount) + " words · " + std::string(ai::toString(r.appliedMode))
        : "Copied — press \u2318V to paste");
    popup_->setState(PopupState::Result);
    tray_->setState(platform::TrayState::Idle);

    history_->insert(db::HistoryEntry{
        /*id*/ 0, r.rawTranscript, r.enhancedText, focus_.appName,
        ai::toString(r.appliedMode), r.wordCount,
        r.transcribeMs + r.enhanceMs, system_clock::now()
    });
    if (!injected)
        tray_->notify("Copied to clipboard", "No text field was focused.");
}

// ---------------------------------------------------------------------------
void AppController::cancel() {
    if (session_ == SessionState::Idle) return;
    if (cancelToken_) cancelToken_->cancel();
    audio_->stop();
    provider_->abort();
    session_ = SessionState::Idle;
    popup_->setState(PopupState::Cancelled);
    tray_->setState(platform::TrayState::Idle);
}

void AppController::setPaused(bool p) {
    paused_ = p;
    if (p) cancel();
    tray_->setState(p ? platform::TrayState::Paused : platform::TrayState::Idle);
}

// ---------------------------------------------------------------------------
// UI THREAD — one call per vsync.
// ---------------------------------------------------------------------------
void AppController::renderFrame() {
    const auto now = steady_clock::now();
    float dt = lastFrame_.time_since_epoch().count()
             ? duration<float>(now - lastFrame_).count() : 1.f / 60.f;
    lastFrame_ = now;

    // Drain cross-thread state.
    {
        std::lock_guard l(uiMutex_);
        if (!pendingPartial_.empty()) {
            popup_->setPartialTranscript(pendingPartial_);
            pendingPartial_.clear();
        }
        if (hasResult_) { hasResult_ = false; deliver(pendingResult_); }
    }
    if (endpointRequested_.exchange(false, std::memory_order_acquire))
        finishDictation();

    // Drain audio for the visualiser (peek: never steals from the encoder).
    float buf[1024];
    size_t n = levelRing_.read(buf, 1024);
    if (n) {
        double acc = 0.0;
        for (size_t i = 0; i < n; ++i) acc += double(buf[i]) * buf[i];
        popup_->pushAudioLevel(float(std::sqrt(acc / double(n))));
    } else {
        popup_->pushAudioLevel(rmsAtomic_.load(std::memory_order_relaxed));
    }

    popup_->update(dt);
    renderer_->beginFrame();
    popup_->draw(renderer_->canvas(), renderer_->widthDp(), renderer_->heightDp());
    renderer_->endFrame();       // present on vsync
}

} // namespace vox::app
