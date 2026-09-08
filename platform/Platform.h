// ============================================================================
//  VoxCast — platform/Platform.h
//  The single seam between portable app code and the OS. Every symbol below
//  has exactly two implementations: platform/mac/*.mm (Cocoa/CoreAudio/AX)
//  and platform/win/*.cpp (Win32/WASAPI/UIA). Nothing above this layer may
//  #include a platform header.
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace vox::platform {

// --------------------------------------------------------------------------
// PlatformAudio
// --------------------------------------------------------------------------
struct AudioDevice { std::string id, name; bool isDefault{false}; };

class IAudioEngine {
public:
    /// Called on the real-time capture thread. NEVER allocate/lock in here.
    using CaptureFn = std::function<void(const float* mono16k, size_t frames)>;

    virtual ~IAudioEngine() = default;
    virtual std::vector<AudioDevice> enumerateDevices() = 0;
    virtual bool  start(const std::string& deviceId, CaptureFn onFrames) = 0;
    virtual void  stop() = 0;
    virtual bool  running() const = 0;
    virtual float inputGain() const = 0;
    virtual void  setInputGain(float linear) = 0;
    /// Smoothed peak level for the settings meter (safe from UI thread).
    virtual float meterLevel() const = 0;

    static std::unique_ptr<IAudioEngine> create();  // WASAPI | CoreAudio
};

// --------------------------------------------------------------------------
// PlatformHotkey — low-level global capture (CGEventTap / WH_KEYBOARD_LL)
// so that modifier-only keys such as Fn and Right-Ctrl are usable.
// --------------------------------------------------------------------------
enum class HotkeyEdge { Down, Up };
enum class HotkeyMode { HoldToTalk, Toggle, Both };

struct HotkeyBinding {
    uint32_t keyCode{};              // virtual key / CGKeyCode
    uint32_t modifiers{};            // bitmask
    bool     isModifierOnly{false};  // Fn, Right Ctrl…
    HotkeyMode mode{HotkeyMode::Both};
    std::string display{"Right Ctrl"};
};

class IHotkeyService {
public:
    using Callback = std::function<void(HotkeyEdge)>;
    virtual ~IHotkeyService() = default;
    virtual bool bind(const HotkeyBinding&, Callback) = 0;
    virtual void unbind() = 0;
    /// Live capture widget in Settings uses this to read the next chord.
    virtual void beginCapture(std::function<void(HotkeyBinding)> onCaptured) = 0;
    virtual void cancelCapture() = 0;
    virtual bool hasAccessibilityPermission() const = 0;
    virtual void requestAccessibilityPermission() = 0;
    static std::unique_ptr<IHotkeyService> create();
};

// --------------------------------------------------------------------------
// PlatformInputInjector — synthetic typing with full Unicode/emoji support.
// --------------------------------------------------------------------------
class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    /// Types UTF-8 text at the caret. Returns false if no focused editable.
    virtual bool typeText(const std::string& utf8) = 0;
    /// Fallback path: synthesise Cmd/Ctrl+V after placing text on clipboard.
    virtual bool pasteFromClipboard() = 0;
    virtual void sendKey(uint32_t keyCode, uint32_t modifiers) = 0;
    static std::unique_ptr<IInputInjector> create();
};

// --------------------------------------------------------------------------
// PlatformClipboard
// --------------------------------------------------------------------------
class IClipboard {
public:
    virtual ~IClipboard() = default;
    virtual bool setText(const std::string& utf8) = 0;
    virtual std::optional<std::string> getText() = 0;
    static std::unique_ptr<IClipboard> create();
};

// --------------------------------------------------------------------------
// PlatformAccessibility — who has focus, and is there a caret?
// Drives automatic enhancement-mode selection.
// --------------------------------------------------------------------------
struct FocusInfo {
    std::string bundleIdOrExe;   // com.microsoft.VSCode | Code.exe
    std::string appName;
    std::string windowTitle;
    bool        hasEditableFocus{false};
};

class IAccessibility {
public:
    virtual ~IAccessibility() = default;
    virtual FocusInfo focused() = 0;
    static std::unique_ptr<IAccessibility> create();
};

// --------------------------------------------------------------------------
// PlatformWindow — native transparent, borderless, always-on-top surface with
// compositor blur (NSVisualEffectView / DWM Acrylic + DirectComposition) and
// a GPU swapchain handed to Skia (Metal / D3D11).
// --------------------------------------------------------------------------
struct WindowDesc {
    int   width{360}, height{78};
    bool  borderless{true};
    bool  alwaysOnTop{true};
    bool  clickThrough{false};
    bool  nativeBlur{true};
    bool  showInTaskbar{false};
    bool  acceptsFirstMouse{true};
    std::string title{"VoxCast"};
};

struct MouseEvent { float x, y; int button; bool down; bool drag; };
struct KeyEvent   { uint32_t keyCode; uint32_t modifiers; bool down; };

class IPlatformWindow {
public:
    virtual ~IPlatformWindow() = default;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool visible() const = 0;
    virtual void setPosition(int x, int y) = 0;
    virtual void setSize(int w, int h) = 0;
    virtual void setOpacity(float a) = 0;
    virtual void setClickThrough(bool) = 0;
    virtual void centerBottomOnActiveScreen(int marginPx) = 0;
    virtual void positionNearCursor(int offsetY) = 0;
    virtual void snapToNearestEdge(int thresholdPx) = 0;
    /// Native handle for the Skia GPU backend (CAMetalLayer* / HWND).
    virtual void* nativeSurface() = 0;
    virtual float backingScale() const = 0;

    std::function<void(MouseEvent)> onMouse;
    std::function<void(KeyEvent)>   onKey;
    std::function<void()>           onDisplayLink;   // vsync tick

    static std::unique_ptr<IPlatformWindow> create(const WindowDesc&);
};

// --------------------------------------------------------------------------
// PlatformSecrets — Keychain / DPAPI-backed Credential Manager.
// --------------------------------------------------------------------------
class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual bool store(const std::string& key, const std::string& secret) = 0;
    virtual std::optional<std::string> load(const std::string& key) = 0;
    virtual bool erase(const std::string& key) = 0;
    static std::unique_ptr<ISecretStore> create();
};

// --------------------------------------------------------------------------
// PlatformTray
// --------------------------------------------------------------------------
enum class TrayState { Idle, Listening, Processing, Paused, Error };

class ITray {
public:
    virtual ~ITray() = default;
    virtual void setState(TrayState) = 0;
    virtual void setMenu(const std::vector<std::pair<std::string, std::function<void()>>>&) = 0;
    virtual void notify(const std::string& title, const std::string& body) = 0;
    static std::unique_ptr<ITray> create();
};

/// Launch-at-login (LaunchAgent plist / HKCU Run key).
bool setLaunchAtLogin(bool enabled);
bool launchAtLoginEnabled();

} // namespace vox::platform
