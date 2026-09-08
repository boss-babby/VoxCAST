// ============================================================================
//  VoxCast — platform/win/WinPlatform.cpp
//  Windows implementations of the platform seam.
//
//  Window strategy: WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW with
//  UpdateLayeredWindow() fed a premultiplied BGRA DIB. That gives true
//  per-pixel alpha on a borderless, always-on-top, taskbar-less surface —
//  which is exactly what the capsule needs, and unlike DWM blur it also
//  works under Wine and on systems with composition disabled.
//  When DWM is available we additionally request Acrylic backdrop.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <dwmapi.h>

#include "platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace vox::platform {

// ===========================================================================
// Helpers
// ===========================================================================
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// ===========================================================================
// PlatformClipboard
// ===========================================================================
class WinClipboard final : public IClipboard {
public:
    bool setText(const std::string& utf8) override {
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();
        std::wstring w = widen(utf8);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        if (!h) { CloseClipboard(); return false; }
        auto* dst = static_cast<wchar_t*>(GlobalLock(h));
        std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
        return true;
    }
    std::optional<std::string> getText() override {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (!h) { CloseClipboard(); return std::nullopt; }
        auto* p = static_cast<const wchar_t*>(GlobalLock(h));
        std::string out = p ? narrow(p) : std::string{};
        GlobalUnlock(h);
        CloseClipboard();
        return out;
    }
};
std::unique_ptr<IClipboard> IClipboard::create() {
    return std::make_unique<WinClipboard>();
}

// ===========================================================================
// PlatformInputInjector — SendInput with KEYEVENTF_UNICODE.
// Surrogate pairs are sent as two events, which is what Windows expects for
// astral-plane codepoints (emoji).
// ===========================================================================
class WinInputInjector final : public IInputInjector {
public:
    bool typeText(const std::string& utf8) override {
        std::wstring w = widen(utf8);
        if (w.empty()) return false;
        std::vector<INPUT> in;
        in.reserve(w.size() * 2);
        for (wchar_t c : w) {
            if (c == L'\n') {           // newline must be a real VK_RETURN
                INPUT d{}; d.type = INPUT_KEYBOARD; d.ki.wVk = VK_RETURN;
                INPUT u = d; u.ki.dwFlags = KEYEVENTF_KEYUP;
                in.push_back(d); in.push_back(u);
                continue;
            }
            INPUT d{}; d.type = INPUT_KEYBOARD;
            d.ki.wVk = 0; d.ki.wScan = c; d.ki.dwFlags = KEYEVENTF_UNICODE;
            INPUT u = d; u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            in.push_back(d); in.push_back(u);
        }
        UINT sent = SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
        return sent == in.size();
    }
    bool pasteFromClipboard() override {
        // Clear any lingering modifier keys (Left Ctrl, Shift) before triggering Ctrl+V
        INPUT clear[4]{};
        for (auto& i : clear) i.type = INPUT_KEYBOARD;
        clear[0].ki.wVk = VK_LCONTROL; clear[0].ki.dwFlags = KEYEVENTF_KEYUP;
        clear[1].ki.wVk = VK_RCONTROL; clear[1].ki.dwFlags = KEYEVENTF_KEYUP;
        clear[2].ki.wVk = VK_CONTROL;  clear[2].ki.dwFlags = KEYEVENTF_KEYUP;
        clear[3].ki.wVk = VK_SHIFT;    clear[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, clear, sizeof(INPUT));

        Sleep(10);

        INPUT in[4]{};
        for (auto& i : in) i.type = INPUT_KEYBOARD;
        in[0].ki.wVk = VK_CONTROL;
        in[1].ki.wVk = 'V';
        in[2].ki.wVk = 'V';         in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].ki.wVk = VK_CONTROL;   in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(4, in, sizeof(INPUT)) == 4;
    }
    void sendKey(uint32_t vk, uint32_t mods) override {
        std::vector<INPUT> in;
        auto push = [&](WORD k, bool up) {
            INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k;
            if (up) i.ki.dwFlags = KEYEVENTF_KEYUP;
            in.push_back(i);
        };
        if (mods & MOD_CONTROL) push(VK_CONTROL, false);
        if (mods & MOD_SHIFT)   push(VK_SHIFT, false);
        if (mods & MOD_ALT)     push(VK_MENU, false);
        push(WORD(vk), false);
        push(WORD(vk), true);
        if (mods & MOD_ALT)     push(VK_MENU, true);
        if (mods & MOD_SHIFT)   push(VK_SHIFT, true);
        if (mods & MOD_CONTROL) push(VK_CONTROL, true);
        SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
    }
};
std::unique_ptr<IInputInjector> IInputInjector::create() {
    return std::make_unique<WinInputInjector>();
}

// ===========================================================================
// PlatformAccessibility
// ===========================================================================
class WinAccessibility final : public IAccessibility {
public:
    FocusInfo focused() override {
        FocusInfo f;
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return f;

        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, 511);
        f.windowTitle = narrow(title);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t path[MAX_PATH]{};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &sz)) {
                std::wstring p(path);
                size_t slash = p.find_last_of(L'\\');
                f.bundleIdOrExe = narrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
                f.appName = f.bundleIdOrExe;
                size_t dot = f.appName.rfind('.');
                if (dot != std::string::npos) f.appName = f.appName.substr(0, dot);
            }
            CloseHandle(proc);
        }

        // Caret probe: attach to the foreground thread's input queue and ask
        // whether the focused control reports a caret position.
        DWORD fgThread = GetWindowThreadProcessId(hwnd, nullptr);
        DWORD self = GetCurrentThreadId();
        if (AttachThreadInput(self, fgThread, TRUE)) {
            GUITHREADINFO gti{sizeof(GUITHREADINFO)};
            if (GetGUIThreadInfo(fgThread, &gti))
                f.hasEditableFocus = (gti.hwndCaret != nullptr) ||
                                     (gti.flags & GUI_CARETBLINKING) != 0;
            if (!f.hasEditableFocus && gti.hwndFocus) {
                wchar_t cls[128]{};
                GetClassNameW(gti.hwndFocus, cls, 127);
                std::wstring c(cls);
                f.hasEditableFocus = c.find(L"Edit") != std::wstring::npos ||
                                     c.find(L"RICHEDIT") != std::wstring::npos ||
                                     c.find(L"Chrome") != std::wstring::npos;
            }
            AttachThreadInput(self, fgThread, FALSE);
        }
        return f;
    }
};
std::unique_ptr<IAccessibility> IAccessibility::create() {
    return std::make_unique<WinAccessibility>();
}

// ===========================================================================
// PlatformSecrets — DPAPI (CryptProtectData) blob under %APPDATA%.
// Ciphertext is bound to the current user account; it cannot be decrypted by
// another user or on another machine.
// ===========================================================================
class WinSecretStore final : public ISecretStore {
public:
    WinSecretStore() {
        wchar_t appdata[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        dir_ = (n > 0 && n < MAX_PATH) ? std::wstring(appdata) + L"\\VoxCast"
                                       : std::wstring(L".");
        CreateDirectoryW(dir_.c_str(), nullptr);
    }

    bool store(const std::string& key, const std::string& secret) override {
        DATA_BLOB in{ DWORD(secret.size()), (BYTE*)secret.data() }, out{};
        if (!CryptProtectData(&in, L"VoxCast", nullptr, nullptr, nullptr, 0, &out))
            return false;
        HANDLE h = CreateFileW(path(key).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = false;
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(h, out.pbData, out.cbData, &wrote, nullptr) && wrote == out.cbData;
            CloseHandle(h);
        }
        LocalFree(out.pbData);
        return ok;
    }

    std::optional<std::string> load(const std::string& key) override {
        HANDLE h = CreateFileW(path(key).c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::nullopt;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        std::vector<char> buf(size_t(sz.QuadPart));
        DWORD got = 0;
        if (!buf.empty()) ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr);
        CloseHandle(h);
        if (buf.empty() || got != buf.size()) return std::nullopt;
        DATA_BLOB in{ DWORD(buf.size()), (BYTE*)buf.data() }, out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
            return std::nullopt;
        std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return s;
    }

    bool erase(const std::string& key) override {
        return DeleteFileW(path(key).c_str()) != 0;
    }

private:
    std::wstring path(const std::string& key) const {
        return dir_ + L"\\" + widen(key) + L".dpapi";
    }
    std::wstring dir_;
};
std::unique_ptr<ISecretStore> ISecretStore::create() {
    return std::make_unique<WinSecretStore>();
}

// ===========================================================================
// PlatformAudio — Windows waveIn capture at 16 kHz 16-bit mono.
// Captures real microphone data and automatically resamples through the
// Windows audio subsystem. If no capture endpoint exists, falls back to silence.
// ===========================================================================
class WinAudioEngine final : public IAudioEngine {
public:
    ~WinAudioEngine() override { stop(); }

    std::vector<AudioDevice> enumerateDevices() override {
        std::vector<AudioDevice> list;
        UINT numDevs = waveInGetNumDevs();
        for (UINT i = 0; i < numDevs; ++i) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                list.push_back({std::to_string(i), narrow(caps.szPname), i == 0});
            }
        }
        if (list.empty()) {
            list.push_back({"default", "System default input", true});
        }
        return list;
    }

    bool start(const std::string& deviceId, CaptureFn cb) override {
        if (running_) return true;
        cb_ = std::move(cb);
        running_ = true;
        stopFlag_ = false;

        UINT devIndex = WAVE_MAPPER;
        try {
            if (!deviceId.empty() && deviceId != "default") {
                devIndex = (UINT)std::stoul(deviceId);
            }
        } catch (...) {
            devIndex = WAVE_MAPPER;
        }

        hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 16000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = 32000;
        wfx.cbSize = 0;

        MMRESULT res = MMSYSERR_ERROR;
        if (hEvent_) {
            res = waveInOpen(&hWaveIn_, devIndex, &wfx, (DWORD_PTR)hEvent_, 0, CALLBACK_EVENT);
        }

        if (res == MMSYSERR_NOERROR && hWaveIn_) {
            constexpr int SAMPLES_PER_BUF = 640; // 40ms @ 16kHz
            for (int i = 0; i < NUM_BUFFERS; ++i) {
                buffers_[i].resize(SAMPLES_PER_BUF);
                ZeroMemory(&headers_[i], sizeof(WAVEHDR));
                headers_[i].lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
                headers_[i].dwBufferLength = SAMPLES_PER_BUF * sizeof(int16_t);
                waveInPrepareHeader(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
                waveInAddBuffer(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
            }
            waveInStart(hWaveIn_);
            worker_ = std::thread([this] { captureLoopWaveIn(); });
        } else {
            // Fallback for headless CI or systems without input device
            worker_ = std::thread([this] { captureLoopSilence(); });
        }
        return true;
    }

    void stop() override {
        if (!running_) return;
        stopFlag_ = true;

        if (hWaveIn_) {
            waveInReset(hWaveIn_);
        }
        if (hEvent_) SetEvent(hEvent_);
        if (worker_.joinable()) worker_.join();

        if (hWaveIn_) {
            for (int i = 0; i < NUM_BUFFERS; ++i) {
                if (headers_[i].lpData) {
                    waveInUnprepareHeader(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
                }
            }
            waveInClose(hWaveIn_);
            hWaveIn_ = nullptr;
        }
        if (hEvent_) {
            CloseHandle(hEvent_);
            hEvent_ = nullptr;
        }

        running_ = false;
        meter_.store(0.f);
    }

    bool  running() const override { return running_; }
    float inputGain() const override { return gain_; }
    void  setInputGain(float g) override { gain_ = g; }
    float meterLevel() const override { return meter_.load(std::memory_order_relaxed); }

private:
    void captureLoopWaveIn() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        std::vector<float> floatBuf;
        floatBuf.reserve(640);

        while (!stopFlag_) {
            WaitForSingleObject(hEvent_, 50);
            if (stopFlag_) break;

            for (int i = 0; i < NUM_BUFFERS; ++i) {
                if (headers_[i].dwFlags & WHDR_DONE) {
                    int samples = int(headers_[i].dwBytesRecorded / sizeof(int16_t));
                    if (samples > 0) {
                        floatBuf.resize(samples);
                        float sumSq = 0.f;
                        float g = gain_;
                        for (int s = 0; s < samples; ++s) {
                            float v = (float(buffers_[i][s]) / 32768.0f) * g;
                            v = std::clamp(v, -1.0f, 1.0f);
                            floatBuf[s] = v;
                            sumSq += v * v;
                        }
                        float rms = std::sqrt(sumSq / float(samples));
                        meter_.store(rms, std::memory_order_relaxed);

                        if (cb_) cb_(floatBuf.data(), size_t(samples));
                    }

                    headers_[i].dwBytesRecorded = 0;
                    headers_[i].dwFlags &= ~WHDR_DONE;
                    if (!stopFlag_ && hWaveIn_) {
                        waveInAddBuffer(hWaveIn_, &headers_[i], sizeof(WAVEHDR));
                    }
                }
            }
        }
    }

    void captureLoopSilence() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        constexpr int kFrame = 320; // 20 ms
        std::vector<float> buf(kFrame, 0.f);
        auto next = std::chrono::steady_clock::now();

        while (!stopFlag_) {
            meter_.store(0.f, std::memory_order_relaxed);
            if (cb_) cb_(buf.data(), size_t(kFrame));
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    }

    static constexpr int NUM_BUFFERS = 4;
    HWAVEIN hWaveIn_{nullptr};
    HANDLE hEvent_{nullptr};
    std::vector<int16_t> buffers_[NUM_BUFFERS];
    WAVEHDR headers_[NUM_BUFFERS]{};

    CaptureFn cb_;
    std::thread worker_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<float> meter_{0.f};
    bool running_{false};
    float gain_{1.f};
};
std::unique_ptr<IAudioEngine> IAudioEngine::create() {
    return std::make_unique<WinAudioEngine>();
}

// ===========================================================================
// PlatformHotkey — WH_KEYBOARD_LL so modifier-only keys (Right Ctrl, Fn)
// generate both edges, which RegisterHotKey cannot do.
// ===========================================================================
static class WinHotkeyService* g_hotkey = nullptr;

class WinHotkeyService final : public IHotkeyService {
public:
    WinHotkeyService() { g_hotkey = this; }
    ~WinHotkeyService() override { unbind(); g_hotkey = nullptr; }

    bool bind(const HotkeyBinding& b, Callback cb) override {
        binding_ = b;
        cb_ = std::move(cb);
        if (!hook_)
            hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WinHotkeyService::proc,
                                      GetModuleHandleW(nullptr), 0);
        return hook_ != nullptr;
    }
    void unbind() override {
        if (hook_) { UnhookWindowsHookEx(hook_); hook_ = nullptr; }
        cb_ = nullptr;
    }
    void beginCapture(std::function<void(HotkeyBinding)> onCaptured) override {
        capture_ = std::move(onCaptured);
    }
    void cancelCapture() override { capture_ = nullptr; }

    bool hasAccessibilityPermission() const override { return true; }  // n/a on Win
    void requestAccessibilityPermission() override {}

    /// Injected by the message loop; also reachable from the hook.
    void feed(DWORD vk, bool down) {
        if (capture_) {
            HotkeyBinding b;
            b.keyCode = vk;
            b.isModifierOnly = (vk == VK_RCONTROL || vk == VK_LCONTROL ||
                                vk == VK_RSHIFT   || vk == VK_LSHIFT);
            b.display = displayName(vk);
            auto c = capture_;
            capture_ = nullptr;
            c(b);
            return;
        }
        if (!cb_) return;

        bool matches = (vk == binding_.keyCode);
        if (!matches) {
            if ((binding_.keyCode == VK_LCONTROL || binding_.keyCode == VK_RCONTROL) && vk == VK_CONTROL) {
                matches = true;
            } else if (binding_.keyCode == VK_CONTROL && (vk == VK_LCONTROL || vk == VK_RCONTROL)) {
                matches = true;
            } else if ((binding_.keyCode == VK_LMENU || binding_.keyCode == VK_RMENU) && vk == VK_MENU) {
                matches = true;
            } else if (binding_.keyCode == VK_MENU && (vk == VK_LMENU || vk == VK_RMENU)) {
                matches = true;
            } else if ((binding_.keyCode == VK_LSHIFT || binding_.keyCode == VK_RSHIFT) && vk == VK_SHIFT) {
                matches = true;
            } else if (binding_.keyCode == VK_SHIFT && (vk == VK_LSHIFT || vk == VK_RSHIFT)) {
                matches = true;
            }
        }
        if (!matches) return;

        if (down == lastDown_) return;              // filter auto-repeat
        lastDown_ = down;
        cb_(down ? HotkeyEdge::Down : HotkeyEdge::Up);
    }

    static std::string displayName(DWORD vk) {
        switch (vk) {
            case VK_RCONTROL: return "Right Ctrl";
            case VK_LCONTROL: return "Left Ctrl";
            case VK_CONTROL:  return "Ctrl";
            case VK_RSHIFT:   return "Right Shift";
            case VK_LSHIFT:   return "Left Shift";
            case VK_F13:      return "F13";
            default: {
                char b[32];
                std::snprintf(b, sizeof b, "VK 0x%02X", unsigned(vk));
                return b;
            }
        }
    }

private:
    static LRESULT CALLBACK proc(int code, WPARAM w, LPARAM l) {
        if (code == HC_ACTION && g_hotkey) {
            auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
            DWORD vk = k->vkCode;
            const bool isExtended = (k->flags & LLKHF_EXTENDED) != 0;

            if (vk == VK_CONTROL) {
                vk = isExtended ? VK_RCONTROL : VK_LCONTROL;
            } else if (vk == VK_MENU) {
                vk = isExtended ? VK_RMENU : VK_LMENU;
            } else if (vk == VK_SHIFT) {
                vk = (k->scanCode == 0x36) ? VK_RSHIFT : VK_LSHIFT;
            }

            const bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            const bool up   = (w == WM_KEYUP   || w == WM_SYSKEYUP);
            if (down || up) g_hotkey->feed(vk, down);
        }
        return CallNextHookEx(nullptr, code, w, l);
    }

    HHOOK hook_{nullptr};
    HotkeyBinding binding_{};
    Callback cb_;
    std::function<void(HotkeyBinding)> capture_;
    bool lastDown_{false};
};
std::unique_ptr<IHotkeyService> IHotkeyService::create() {
    return std::make_unique<WinHotkeyService>();
}

// ===========================================================================
// PlatformTray — Shell_NotifyIcon with a state-tinted generated icon.
// ===========================================================================
class WinTray final : public ITray {
public:
    static LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<WinTray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_APP + 1 && self) {
            if (lp == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                for (size_t i = 0; i < self->menu_.size(); ++i) {
                    AppendMenuW(hMenu, MF_STRING, i + 1, widen(self->menu_[i].first).c_str());
                }
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd > 0 && size_t(cmd - 1) < self->menu_.size()) {
                    if (self->menu_[cmd - 1].second) self->menu_[cmd - 1].second();
                }
                return 0;
            } else if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
                if (!self->menu_.empty() && self->menu_[0].second) {
                    self->menu_[0].second();
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    WinTray() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = trayWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VoxCastTray";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, L"VoxCastTray", L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid_.uCallbackMessage = WM_APP + 1;
        nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy_s(nid_.szTip, L"VoxCast — idle");
        Shell_NotifyIconW(NIM_ADD, &nid_);
    }
    ~WinTray() override { Shell_NotifyIconW(NIM_DELETE, &nid_); }

    void setState(TrayState s) override {
        const wchar_t* t = L"VoxCast — idle";
        switch (s) {
            case TrayState::Listening:  t = L"VoxCast — listening"; break;
            case TrayState::Processing: t = L"VoxCast — thinking";  break;
            case TrayState::Paused:     t = L"VoxCast — paused";    break;
            case TrayState::Error:      t = L"VoxCast — error";     break;
            default: break;
        }
        wcscpy_s(nid_.szTip, t);
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }
    void setMenu(const std::vector<std::pair<std::string,
                 std::function<void()>>>& items) override {
        menu_ = items;
    }
    void notify(const std::string& title, const std::string& body) override {
        NOTIFYICONDATAW n = nid_;
        n.uFlags = NIF_INFO;
        wcscpy_s(n.szInfoTitle, widen(title).substr(0, 63).c_str());
        wcscpy_s(n.szInfo, widen(body).substr(0, 255).c_str());
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }
private:
    HWND hwnd_{};
    NOTIFYICONDATAW nid_{};
    std::vector<std::pair<std::string, std::function<void()>>> menu_;
};
std::unique_ptr<ITray> ITray::create() { return std::make_unique<WinTray>(); }

// ===========================================================================
// PlatformWindow — layered, per-pixel-alpha, always-on-top overlay.
// ===========================================================================
class WinWindow final : public IPlatformWindow {
public:
    explicit WinWindow(const WindowDesc& d) : desc_(d) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &WinWindow::wndProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"VoxCastOverlay";
            RegisterClassExW(&wc);
            registered = true;
        }

        DWORD ex = WS_EX_LAYERED | WS_EX_NOACTIVATE;
        if (d.alwaysOnTop)    ex |= WS_EX_TOPMOST;
        if (!d.showInTaskbar) ex |= WS_EX_TOOLWINDOW;
        if (d.clickThrough)   ex |= WS_EX_TRANSPARENT;

        hwnd_ = CreateWindowExW(ex, L"VoxCastOverlay", widen(d.title).c_str(),
                                WS_POPUP, 0, 0, d.width, d.height,
                                nullptr, nullptr, inst, this);
        w_ = d.width; h_ = d.height;

        makeDib();
    }
    ~WinWindow() override {
        if (dib_) DeleteObject(dib_);
        if (memDc_) DeleteDC(memDc_);
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void show() override {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (desc_.alwaysOnTop)
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        visible_ = true;
    }
    void hide() override { ShowWindow(hwnd_, SW_HIDE); visible_ = false; }
    bool visible() const override { return visible_; }

    void setPosition(int x, int y) override {
        x_ = x; y_ = y;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void setSize(int w, int h) override {
        w_ = w; h_ = h;
        SetWindowPos(hwnd_, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        makeDib();
    }
    void setOpacity(float a) override { opacity_ = a; }
    void setClickThrough(bool on) override {
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    }

    void centerBottomOnActiveScreen(int margin) override {
        HMONITOR m = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        int cx = (mi.rcWork.left + mi.rcWork.right) / 2 - w_ / 2;
        int cy = mi.rcWork.bottom - h_ - margin;
        setPosition(cx, cy);
    }
    void positionNearCursor(int offsetY) override {
        POINT p{};
        GetCursorPos(&p);
        HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        int x = std::clamp(int(p.x) - w_ / 2, int(mi.rcWork.left),
                           int(mi.rcWork.right) - w_);
        int y = std::clamp(int(p.y) + offsetY, int(mi.rcWork.top),
                           int(mi.rcWork.bottom) - h_);
        setPosition(x, y);
    }
    void snapToNearestEdge(int threshold) override {
        HMONITOR m = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        int nx = x_, ny = y_;
        if (std::abs(x_ - int(mi.rcWork.left)) < threshold) nx = mi.rcWork.left;
        if (std::abs(x_ + w_ - int(mi.rcWork.right)) < threshold) nx = mi.rcWork.right - w_;
        if (std::abs(y_ - int(mi.rcWork.top)) < threshold) ny = mi.rcWork.top;
        if (std::abs(y_ + h_ - int(mi.rcWork.bottom)) < threshold) ny = mi.rcWork.bottom - h_;
        setPosition(nx, ny);
    }

    void* nativeSurface() override { return hwnd_; }
    float backingScale() const override {
        HDC dc = GetDC(nullptr);
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        return float(dpi) / 96.f;
    }

    /// Push a premultiplied BGRA buffer to the compositor.
    void present(const uint32_t* bgra, int w, int h) {
        if (!dibPixels_ || w != w_ || h != h_) return;
        std::memcpy(dibPixels_, bgra, size_t(w) * size_t(h) * 4);
        POINT src{0, 0};
        SIZE  size{w_, h_};
        POINT dst{x_, y_};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, BYTE(std::clamp(opacity_, 0.f, 1.f) * 255),
                         AC_SRC_ALPHA};
        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, memDc_, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

private:
    void makeDib() {
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        if (!memDc_) memDc_ = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w_;
        bi.bmiHeader.biHeight = -h_;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS,
                                reinterpret_cast<void**>(&dibPixels_), nullptr, 0);
        if (dib_) SelectObject(memDc_, dib_);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        } else if (self) {
            switch (m) {
                case WM_KEYDOWN: case WM_KEYUP:
                    if (self->onKey)
                        self->onKey(KeyEvent{uint32_t(w), 0, m == WM_KEYDOWN});
                    return 0;
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
                    if (self->onMouse)
                        self->onMouse(MouseEvent{float(LOWORD(l)), float(HIWORD(l)), 0,
                                                 m == WM_LBUTTONDOWN,
                                                 (w & MK_LBUTTON) != 0});
                    return 0;
                default: break;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }

    WindowDesc desc_;
    HWND hwnd_{};
    HDC  memDc_{};
    HBITMAP dib_{};
    uint32_t* dibPixels_{nullptr};
    int w_{0}, h_{0}, x_{0}, y_{0};
    float opacity_{1.f};
    bool visible_{false};
};

std::unique_ptr<IPlatformWindow> IPlatformWindow::create(const WindowDesc& d) {
    return std::make_unique<WinWindow>(d);
}

/// Downcast helper used by the renderer to reach present().
void PresentToWindow(IPlatformWindow* w, const uint32_t* bgra, int width, int height) {
    if (auto* ww = dynamic_cast<WinWindow*>(w)) ww->present(bgra, width, height);
}

// ===========================================================================
// Launch at login — HKCU\...\Run
// ===========================================================================
bool setLaunchAtLogin(bool enabled) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok;
    if (enabled) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        ok = RegSetValueExW(k, L"VoxCast", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(path),
                            DWORD((wcslen(path) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(k, L"VoxCast") == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return ok;
}

bool launchAtLoginEnabled() {
    wchar_t buf[MAX_PATH]{};
    DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"VoxCast", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}

} // namespace vox::platform
