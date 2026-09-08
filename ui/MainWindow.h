#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <array>
#include "Animation.h"

namespace vox::ui {

enum class DictationState {
    Idle,
    Listening,
    Processing,
    Success,
    Error
};

struct AppConfig {
    std::string apiKey;
    std::string hotkeyKey{"Left Ctrl"};
    std::string hotkeyDisplay{"Left Ctrl"};
    uint32_t    hotkeyVk{VK_LCONTROL};
    bool        hotkeyIsMod{true};
    std::string hotkeyMode{"hold"};         // "hold" or "toggle"
    std::string injectionMode{"live"};      // "live", "instant", or "typing"
    std::string transcribeModel{"gemini-3.5-transcribe"};
    std::string endpointBase{"https://generativelanguage.googleapis.com/v1beta"};
    std::string audioDeviceId{"default"};
    std::string silenceSensitivity{"normal"};// "normal", "high", "low"
    bool        showFloatingOverlay{true};
};

struct HistoryItem {
    std::string timestamp;
    std::string duration;
    std::string text;
};

struct VisualizerBar {
    anim::Spring<float> spring{320.f, 22.f, 1.f};
    float jitter{0.f};
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool create(HINSTANCE hInstance, const AppConfig& initialConfig);
    void show();
    void hide();
    void toggleVisibility();
    bool isVisible() const;
    HWND hwnd() const { return hwnd_; }

    void setDictationState(DictationState state, const std::string& message);
    void setAudioLevel(float level); // 0.0f - 1.0f
    void setTranscript(const std::string& text, bool appendHistory = true, const std::string& durationStr = "");
    void setApiTestResult(bool ok, long httpCode, long latencyMs, const std::string& message);
    void setAudioDevices(const std::vector<std::pair<std::string, std::string>>& devices);
    void updateRecordingStats(float durationSec, size_t samples, float peak, float rms);
    void updateActiveHotkeyDisplay(const std::string& display, uint32_t vk);

    std::function<void()> onStartDictate;
    std::function<void()> onStopDictate;
    std::function<void(const AppConfig&)> onSaveConfig;
    std::function<void(const std::string& apiKey, const std::string& model, const std::string& endpoint)> onTestApiConnection;
    std::function<void()> onPlayLastAudio;
    std::function<void()> onOpenAudioFolder;
    std::function<void(uint32_t vk, const std::string& name, bool isMod)> onHotkeyChanged;
    std::function<void()> onQuit;

    const AppConfig& getConfig() const { return config_; }

private:
    static LRESULT CALLBACK wndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void onTimerTick(float dt);
    void render(HDC hdc, int width, int height);

    void switchTab(int tabIndex);
    void syncUiFromConfig();
    AppConfig readConfigFromUi();

    // Mouse interaction hit tests
    void onMouseDown(int x, int y);
    void onMouseMove(int x, int y);

    HWND hwnd_{nullptr};
    HINSTANCE hInst_{nullptr};
    ULONG_PTR gdiplusToken_{0};

    // State
    AppConfig config_;
    DictationState dictState_{DictationState::Idle};
    std::string statusMessage_{"Ready — Hold Left Ctrl or click Start Dictating"};
    float currentAudioLevel_{0.0f};
    float displayedAudioLevel_{0.0f};
    bool detectingKey_{false};
    bool showKeyPlaintext_{false};
    std::string copyNotification_{};
    float copyNotificationTimer_{0.f};

    // Diagnostics
    std::string lastAudioStats_{"Last Recording: None captured yet in this session."};
    std::string apiTestResult_{"Ready to test connection"};
    bool apiTestOk_{true};
    long apiLatencyMs_{0};

    int activeTab_{0};
    int hoveredTab_{-1};
    int hoveredBtn_{-1};
    int selectedHistoryIdx_{-1};

    // Animation drivers
    float time_{0.0f};
    float ripplePhase_{0.0f};
    std::array<VisualizerBar, 28> visualizerBars_;

    std::vector<HistoryItem> history_;
    std::vector<std::pair<std::string, std::string>> audioDevices_;

    // Embedded child controls (for text input)
    HWND edtTranscript_{nullptr};
    HWND edtApiKey_{nullptr};
    HWND edtEndpoint_{nullptr};
    HBRUSH hBrushDarkInput_{nullptr};
    HFONT hFontEditor_{nullptr};
    HFONT hFontApiKey_{nullptr};

    std::mutex stateMutex_;
};

} // namespace vox::ui
