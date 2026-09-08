// ============================================================================
//  VoxCast — app/main.cpp
//
//  Modes:
//    (no args)      run the tray app with the floating dictation overlay
//    --overlay-demo drive the overlay through a scripted session (no mic/net);
//                   used to verify the real window path headlessly under Wine
//    --selftest     exercise platform services and report a pass/fail matrix
//    --version
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform/Platform.h"
#include "net/HttpClient.h"
#include "ui/PopupView.h"
#include "ui/SkiaRenderer.h"
#include "ui/Theme.h"
#include "ui/MainWindow.h"

#include <shellapi.h>
#include <mmsystem.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <mutex>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>

using namespace vox;
using clk = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
static void attachConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outType = (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;

    if (outType == FILE_TYPE_PIPE || outType == FILE_TYPE_DISK || outType == FILE_TYPE_CHAR) {
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hOut), _O_TEXT);
        if (fd >= 0) {
            FILE* fp = _fdopen(fd, "w");
            if (fp) {
                *stdout = *fp;
                setvbuf(stdout, nullptr, _IONBF, 0);
            }
        }
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr != INVALID_HANDLE_VALUE && hErr != nullptr) {
            int fde = _open_osfhandle(reinterpret_cast<intptr_t>(hErr), _O_TEXT);
            if (fde >= 0) {
                FILE* fpe = _fdopen(fde, "w");
                if (fpe) {
                    *stderr = *fpe;
                    setvbuf(stderr, nullptr, _IONBF, 0);
                }
            }
        }
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

static void hideConsole() {
    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        ShowWindow(hConsole, SW_HIDE);
    }
    FreeConsole();
}

// ---------------------------------------------------------------------------
static int selftest() {
    std::printf("\n VoxCast platform self-test (Windows)\n");
    std::printf(" ---------------------------------------------------------------\n");
    int pass = 0, fail = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = {}) {
        std::printf("  %s %-34s %s\n", ok ? "[ok]  " : "[FAIL]", name, detail.c_str());
        std::fflush(stdout);
        ok ? ++pass : ++fail;
    };

    // Clipboard round-trip -------------------------------------------------
    {
        auto cb = platform::IClipboard::create();
        const std::string probe = "VoxCast ✓ clipboard — café 🎙️";
        bool set = cb->setText(probe);
        auto got = cb->getText();
        check("clipboard round-trip (unicode)",
              set && got && *got == probe,
              got ? ("\"" + got->substr(0, 28) + "\"") : "<none>");
    }

    // Secret store (DPAPI) --------------------------------------------------
    {
        auto ss = platform::ISecretStore::create();
        const std::string secret = "AIzaSyD-test-key-0123456789";
        bool stored = ss->store("selftest_key", secret);
        auto back = ss->load("selftest_key");
        bool ok = stored && back && *back == secret;
        ss->erase("selftest_key");
        check("DPAPI secret store round-trip", ok,
              ok ? "encrypted at rest" : "");
    }

    // Accessibility ---------------------------------------------------------
    {
        auto ax = platform::IAccessibility::create();
        auto f = ax->focused();
        check("focus query", true,
              f.appName.empty() ? "<no foreground window>" : f.appName);
    }

    // Audio engine ----------------------------------------------------------
    {
        auto au = platform::IAudioEngine::create();
        auto devs = au->enumerateDevices();
        int frames = 0;
        au->start("default", [&](const float*, size_t n) { frames += int(n); });
        Sleep(120);
        au->stop();
        check("audio engine start/stop", frames > 0,
              std::to_string(frames) + " frames, " +
              std::to_string(devs.size()) + " device(s)");
    }

    // VAD + ring buffer are covered by voxcast_tests.exe.

    // Renderer --------------------------------------------------------------
    {
        ui::SkiaRenderer r(nullptr, 2.f);
        ui::PopupView popup;
        popup.setTypeface(r.uiTypeface());
        popup.setState(ui::PopupState::Listening);
        for (int i = 0; i < 30; ++i) {
            popup.pushAudioLevel(0.4f + 0.3f * std::sin(float(i) * 0.4f));
            popup.update(1.f / 60.f);
        }
        r.beginFrame();
        popup.draw(r.canvas(), r.widthDp(), r.heightDp());
        r.endFrame();
        const uint32_t* px = r.pixels();
        size_t lit = 0;
        for (int i = 0; i < r.pixelWidth() * r.pixelHeight(); ++i)
            if ((px[i] >> 24) > 8) ++lit;
        check("skia renderer produces pixels", lit > 1000,
              std::to_string(lit) + " non-transparent px at " +
              std::to_string(r.pixelWidth()) + "x" + std::to_string(r.pixelHeight()));
    }

    // Layered window --------------------------------------------------------
    {
        platform::WindowDesc d;
        d.width = 320; d.height = 72;
        auto win = platform::IPlatformWindow::create(d);
        bool made = win && win->nativeSurface() != nullptr;
        if (made) {
            win->centerBottomOnActiveScreen(80);
            win->show();
            ui::SkiaRenderer r(win->nativeSurface(), 1.f);
            r.attach(win.get());
            ui::PopupView popup;
            popup.setTypeface(r.uiTypeface());
            popup.setState(ui::PopupState::Idle);
            for (int i = 0; i < 10; ++i) {
                popup.update(1.f / 60.f);
                r.beginFrame();
                popup.draw(r.canvas(), r.widthDp(), r.heightDp());
                r.endFrame();
            }
            win->hide();
        }
        check("layered overlay window", made, made ? "WS_EX_LAYERED + topmost" : "");
    }

    // Hotkey hook -----------------------------------------------------------
    {
        auto hk = platform::IHotkeyService::create();
        platform::HotkeyBinding b;
        b.keyCode = VK_RCONTROL;
        b.isModifierOnly = true;
        b.display = "Right Ctrl";
        bool bound = hk->bind(b, [](platform::HotkeyEdge) {});
        hk->unbind();
        check("low-level keyboard hook", bound, "WH_KEYBOARD_LL");
    }

    std::printf(" ---------------------------------------------------------------\n");
    std::printf(" %s  %d passed, %d failed\n\n",
                fail == 0 ? "PASSED" : "FAILED", pass, fail);
    std::fflush(stdout);
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
static int overlayDemo(int seconds) {
    std::printf("VoxCast overlay demo — scripted session, %ds\n", seconds);

    platform::WindowDesc d;
    d.width = 320; d.height = 72;
    d.borderless = true; d.alwaysOnTop = true; d.nativeBlur = false;
    d.showInTaskbar = false;
    auto win = platform::IPlatformWindow::create(d);
    if (!win) { std::printf("window creation failed\n"); return 1; }

    ui::SkiaRenderer renderer(win->nativeSurface(), 1.f);
    renderer.attach(win.get());

    ui::PopupView popup;
    popup.setTypeface(renderer.uiTypeface());
    popup.setState(ui::PopupState::Idle);

    win->centerBottomOnActiveScreen(120);
    win->show();

    const auto t0 = clk::now();
    auto last = t0;
    int frames = 0;
    int phase = -1;

    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) return 0;
        }

        const auto now = clk::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        const float t = std::chrono::duration<float>(now - t0).count();
        if (t > float(seconds)) break;

        // Scripted state machine.
        int p = (t < 1.0f) ? 0 : (t < 3.2f) ? 1 : (t < 4.4f) ? 2 : 3;
        if (p != phase) {
            phase = p;
            switch (p) {
                case 0: popup.setState(ui::PopupState::Idle); break;
                case 1: popup.setState(ui::PopupState::Listening); break;
                case 2: popup.setPartialTranscript({});
                        popup.setState(ui::PopupState::Processing); break;
                default: popup.setResultLabel("9 words - Chat");
                         popup.setState(ui::PopupState::Result); break;
            }
        }
        if (p == 1) {
            if (t > 1.8f) popup.setPartialTranscript("hey team um the deploy is green");
            float env = std::pow(std::fabs(std::sin(t * 13.f)), 2.0f);
            popup.pushAudioLevel(0.06f + 0.11f * env * (0.6f + 0.4f * std::sin(t * 1.9f)));
        }

        popup.update(dt);
        renderer.beginFrame();
        popup.draw(renderer.canvas(), renderer.widthDp(), renderer.heightDp());
        renderer.endFrame();
        ++frames;

        Sleep(16);   // ~60 Hz; the shipping build blocks on DwmFlush/vsync
    }

    const float elapsed = std::chrono::duration<float>(clk::now() - t0).count();
    std::printf("presented %d frames in %.2fs (%.1f fps average)\n",
                frames, elapsed, float(frames) / elapsed);
    win->hide();
    return 0;
}

// ---------------------------------------------------------------------------
static uint32_t parseKey(const std::string& name, bool& isModifierOnly, std::string& displayName) {
    std::string s = name;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());

    isModifierOnly = true;
    if (s == "leftctrl" || s == "lctrl" || s == "leftcontrol" || s == "ctrl" || s == "lcontrol") {
        displayName = "Left Ctrl";
        return VK_LCONTROL;
    }
    if (s == "rightctrl" || s == "rctrl" || s == "rightcontrol" || s == "rcontrol") {
        displayName = "Right Ctrl";
        return VK_RCONTROL;
    }
    if (s == "leftshift" || s == "lshift") {
        displayName = "Left Shift";
        return VK_LSHIFT;
    }
    if (s == "rightshift" || s == "rshift") {
        displayName = "Right Shift";
        return VK_RSHIFT;
    }
    if (s == "leftalt" || s == "lalt" || s == "alt") {
        displayName = "Left Alt";
        return VK_LMENU;
    }
    if (s == "rightalt" || s == "ralt" || s == "altgr") {
        displayName = "Right Alt";
        return VK_RMENU;
    }
    isModifierOnly = false;
    if (s == "capslock" || s == "caps") {
        displayName = "Caps Lock";
        return VK_CAPITAL;
    }
    if (s == "space") { displayName = "Space"; return VK_SPACE; }
    if (s == "tab") { displayName = "Tab"; return VK_TAB; }
    if (s == "grave" || s == "tilde" || s == "`" || s == "~") { displayName = "Tilde (~)"; return VK_OEM_3; }

    if (s.size() >= 2 && s[0] == 'f') {
        try {
            int num = std::stoi(s.substr(1));
            if (num >= 1 && num <= 24) {
                displayName = "F" + std::to_string(num);
                return VK_F1 + (num - 1);
            }
        } catch (...) {}
    }

    try {
        uint32_t val = 0;
        if (s.rfind("0x", 0) == 0) val = std::stoul(s, nullptr, 16);
        else val = std::stoul(s, nullptr, 10);
        if (val > 0) {
            displayName = "VK 0x" + std::to_string(val);
            return val;
        }
    } catch (...) {}

    displayName = "Left Ctrl";
    isModifierOnly = true;
    return VK_LCONTROL;
}

static std::vector<uint8_t> pcmToWav(const std::vector<int16_t>& pcm, int sampleRate = 16000) {
    uint32_t dataSize = uint32_t(pcm.size() * sizeof(int16_t));
    uint32_t fileSize = 36 + dataSize;
    std::vector<uint8_t> wav;
    wav.reserve(44 + dataSize);

    auto writeBytes = [&](const void* p, size_t n) {
        const auto* b = reinterpret_cast<const uint8_t*>(p);
        wav.insert(wav.end(), b, b + n);
    };
    auto writeU32 = [&](uint32_t v) { writeBytes(&v, 4); };
    auto writeU16 = [&](uint16_t v) { writeBytes(&v, 2); };

    writeBytes("RIFF", 4);
    writeU32(fileSize);
    writeBytes("WAVE", 4);

    writeBytes("fmt ", 4);
    writeU32(16);                       // Subchunk1Size
    writeU16(1);                        // AudioFormat (PCM)
    writeU16(1);                        // NumChannels (mono)
    writeU32(uint32_t(sampleRate));     // SampleRate
    writeU32(uint32_t(sampleRate * 2)); // ByteRate
    writeU16(2);                        // BlockAlign
    writeU16(16);                       // BitsPerSample

    writeBytes("data", 4);
    writeU32(dataSize);
    writeBytes(pcm.data(), dataSize);

    return wav;
}

static std::string base64Encode(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* b = bytes.data();
    size_t len = bytes.size();
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

static std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string w;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!w.empty()) {
                words.push_back(w);
                w.clear();
            }
        } else {
            w += c;
        }
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

static std::string requestTranscription(
    std::shared_ptr<net::IHttpClient> httpClient,
    const std::string& endpointBase,
    const std::string& transcribeModel,
    const std::string& apiKey,
    const std::vector<int16_t>& pcm,
    int timeoutMs = 7000)
{
    if (pcm.empty() || apiKey.empty()) return "";
    std::vector<uint8_t> wav = pcmToWav(pcm, 16000);
    std::string b64 = base64Encode(wav);

    nlohmann::json body = {
        {"contents", nlohmann::json::array({{
            {"role", "user"},
            {"parts", nlohmann::json::array({
                {{"text", "Generate a transcript of the speech. Output only the exact transcribed words."}},
                {{"inlineData", {
                    {"mimeType", "audio/wav"},
                    {"data", b64}
                }}}
            })}
        }})},
        {"generationConfig", {
            {"temperature", 0.0}
        }}
    };

    net::Request req;
    req.url = endpointBase + "/models/" + transcribeModel + ":generateContent?key=" + apiKey;
    req.method = "POST";
    req.headers["Content-Type"] = "application/json";
    req.headers["x-goog-api-key"] = apiKey;
    req.body = body.dump();
    req.timeoutMs = timeoutMs;

    auto resp = httpClient->send(req);
    std::string text;
    if (resp.ok()) {
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (j.contains("candidates") && !j["candidates"].empty()) {
                auto& c = j["candidates"][0];
                if (c.contains("content") && c["content"].contains("parts")) {
                    for (auto& p : c["content"]["parts"]) {
                        if (p.contains("text") && p["text"].is_string()) {
                            text += p["text"].get<std::string>();
                        } else if (p.contains("audioTranscription")) {
                            const auto& at = p["audioTranscription"];
                            if (at.is_string()) {
                                text += at.get<std::string>();
                            } else if (at.is_object() && at.contains("text") && at["text"].is_string()) {
                                text += at["text"].get<std::string>();
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ')) text.erase(text.begin());
    return text;
}

struct LiveStreamState {
    std::vector<std::string> committedWords;
    std::atomic<bool> workerBusy{false};
    std::chrono::steady_clock::time_point lastChunkTime;
    size_t lastChunkSampleCount{0};
    std::mutex mtx;

    void reset() {
        std::lock_guard l(mtx);
        committedWords.clear();
        workerBusy.store(false);
        lastChunkTime = std::chrono::steady_clock::now();
        lastChunkSampleCount = 0;
    }
};

// ---------------------------------------------------------------------------
static void saveConfigToFile(const ui::AppConfig& cfg, const std::string& path = "config.json") {
    nlohmann::json j;
    std::ifstream in(path);
    if (in.is_open()) {
        try { in >> j; } catch (...) {}
        in.close();
    }
    j["apiKey"] = cfg.apiKey;
    j["injectionMode"] = cfg.injectionMode;
    j["hotkey"]["key"] = cfg.hotkeyDisplay;
    j["hotkey"]["display"] = cfg.hotkeyDisplay;
    j["hotkey"]["keyCode"] = cfg.hotkeyVk;
    j["hotkey"]["mode"] = cfg.hotkeyMode;
    j["provider"]["endpointBase"] = cfg.endpointBase;
    j["provider"]["transcribeModel"] = cfg.transcribeModel;
    j["provider"]["temperature"] = 0.0;
    j["audio"]["deviceId"] = cfg.audioDeviceId;
    j["audio"]["silenceSensitivity"] = cfg.silenceSensitivity;
    j["popup"]["showFloatingOverlay"] = cfg.showFloatingOverlay;

    std::ofstream out(path);
    if (out.is_open()) {
        out << j.dump(2);
        out.close();
    }
}

static int runApp(const std::string& cliHotkey) {
    platform::HotkeyBinding hkBinding;
    bool isMod = true;
    std::string disp = "Left Ctrl";
    uint32_t vk = VK_LCONTROL;

    std::string apiKey;
    std::string transcribeModel = "gemini-3.5-transcribe-live";
    std::string endpointBase = "https://generativelanguage.googleapis.com/v1beta";
    std::string injectionMode = "live";
    std::string hotkeyMode = "hold";
    std::string silenceSensitivity = "normal";
    bool showFloatingOverlay = true;

    char envKey[1024]{};
    if (GetEnvironmentVariableA("GEMINI_API_KEY", envKey, 1024) > 0) {
        apiKey = envKey;
    }

    std::string chosenKey = cliHotkey;
    std::vector<std::string> searchPaths = { "config.json" };
    char appdata[MAX_PATH]{};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) {
        searchPaths.push_back(std::string(appdata) + "\\VoxCast\\config.json");
    }
    for (const auto& path : searchPaths) {
        std::ifstream f(path);
        if (f.is_open()) {
            try {
                nlohmann::json j;
                f >> j;
                if (j.contains("apiKey") && j["apiKey"].is_string() && !j["apiKey"].get<std::string>().empty()) {
                    apiKey = j["apiKey"].get<std::string>();
                }
                if (j.contains("injectionMode") && j["injectionMode"].is_string()) {
                    injectionMode = j["injectionMode"].get<std::string>();
                } else if (j.contains("injection") && j["injection"].is_string()) {
                    injectionMode = j["injection"].get<std::string>();
                }
                if (j.contains("provider")) {
                    auto& p = j["provider"];
                    if (apiKey.empty() && p.contains("apiKey") && p["apiKey"].is_string()) {
                        apiKey = p["apiKey"].get<std::string>();
                    }
                    if (p.contains("transcribeModel") && p["transcribeModel"].is_string() && !p["transcribeModel"].get<std::string>().empty()) {
                        transcribeModel = p["transcribeModel"].get<std::string>();
                    }
                    if (p.contains("endpointBase") && p["endpointBase"].is_string() && !p["endpointBase"].get<std::string>().empty()) {
                        endpointBase = p["endpointBase"].get<std::string>();
                    }
                }
                if (chosenKey.empty() && j.contains("hotkey")) {
                    auto& h = j["hotkey"];
                    if (h.contains("key") && h["key"].is_string()) {
                        chosenKey = h["key"].get<std::string>();
                    } else if (h.contains("display") && h["display"].is_string()) {
                        chosenKey = h["display"].get<std::string>();
                    } else if (h.contains("keyCode") && h["keyCode"].is_number_integer()) {
                        vk = h["keyCode"].get<uint32_t>();
                        disp = "VK " + std::to_string(vk);
                        chosenKey = "";
                    }
                    if (h.contains("mode") && h["mode"].is_string()) {
                        hotkeyMode = h["mode"].get<std::string>();
                    }
                }
                if (j.contains("popup") && j["popup"].contains("showFloatingOverlay")) {
                    showFloatingOverlay = j["popup"]["showFloatingOverlay"].get<bool>();
                }
                if (j.contains("audio") && j["audio"].contains("silenceSensitivity")) {
                    silenceSensitivity = j["audio"]["silenceSensitivity"].get<std::string>();
                }
                break;
            } catch (...) {}
        }
    }

    if (transcribeModel == "gemini-2.5-flash" || transcribeModel == "gemini-1.5-flash" || transcribeModel.empty()) {
        transcribeModel = "gemini-3.5-transcribe-live";
    }

    if (apiKey.empty()) {
        auto ss = platform::ISecretStore::create();
        if (auto k = ss->load("gemini_api_key")) apiKey = *k;
    }

    if (!chosenKey.empty()) {
        vk = parseKey(chosenKey, isMod, disp);
    }

    hkBinding.keyCode = vk;
    hkBinding.display = disp;
    hkBinding.isModifierOnly = isMod;
    hkBinding.mode = platform::HotkeyMode::Both;

    // Create Main Management UI Window
    ui::AppConfig uiCfg;
    uiCfg.apiKey = apiKey;
    uiCfg.hotkeyKey = disp;
    uiCfg.hotkeyDisplay = disp;
    uiCfg.hotkeyVk = vk;
    uiCfg.hotkeyIsMod = isMod;
    uiCfg.hotkeyMode = hotkeyMode;
    uiCfg.injectionMode = injectionMode;
    uiCfg.transcribeModel = transcribeModel;
    uiCfg.endpointBase = endpointBase;
    uiCfg.silenceSensitivity = silenceSensitivity;
    uiCfg.showFloatingOverlay = showFloatingOverlay;

    auto mainWindow = std::make_shared<ui::MainWindow>();
    if (!mainWindow->create(GetModuleHandleW(nullptr), uiCfg)) {
        std::printf("Failed to create main management UI window\n");
    }
    mainWindow->show();

    auto audio = platform::IAudioEngine::create();
    auto audioDevs = audio->enumerateDevices();
    std::vector<std::pair<std::string, std::string>> devList;
    for (const auto& d : audioDevs) {
        devList.push_back({d.id, d.name});
    }
    mainWindow->setAudioDevices(devList);

    auto tray = platform::ITray::create();
    tray->setState(platform::TrayState::Idle);

    platform::WindowDesc d;
    d.width = 320; d.height = 72;
    d.borderless = true; d.alwaysOnTop = true; d.nativeBlur = false;
    d.showInTaskbar = false;
    auto win = platform::IPlatformWindow::create(d);
    if (!win) { std::printf("Failed to create overlay window\n"); return 1; }

    ui::SkiaRenderer renderer(win->nativeSurface(), 1.f);
    renderer.attach(win.get());

    ui::PopupView popup;
    popup.setTypeface(renderer.uiTypeface());
    popup.setState(ui::PopupState::Idle);

    std::shared_ptr<net::IHttpClient> httpClient = net::IHttpClient::create();

    std::atomic<float> micLevel{0.0f};
    std::atomic<bool> isListening{false};
    std::atomic<bool> isResult{false};
    float resultTimer = 0.0f;
    std::vector<int16_t> pcmCaptured;
    std::mutex pcmMtx;
    LiveStreamState liveState;

    auto startListening = [&]() {
        if (isListening.exchange(true)) return;
        isResult = false;
        liveState.reset();
        {
            std::lock_guard l(pcmMtx);
            pcmCaptured.clear();
            pcmCaptured.reserve(16000 * 30);
        }

        tray->setState(platform::TrayState::Listening);
        mainWindow->setDictationState(ui::DictationState::Listening, "Listening... speak now");

        if (showFloatingOverlay) {
            popup.setState(ui::PopupState::Listening);
            popup.setPartialTranscript("Listening... speak now");
            win->centerBottomOnActiveScreen(80);
            win->show();
        }

        audio->start("default", [&](const float* samples, size_t n) {
            float acc = 0.f;
            {
                std::lock_guard l(pcmMtx);
                for (size_t i = 0; i < n; ++i) {
                    float s = std::clamp(samples[i], -1.0f, 1.0f);
                    pcmCaptured.push_back(int16_t(s * 32767.0f));
                    acc += samples[i] * samples[i];
                }
            }
            float rms = (n > 0) ? std::sqrt(acc / float(n)) : 0.f;
            micLevel.store(rms, std::memory_order_relaxed);
            mainWindow->setAudioLevel(std::clamp(rms * 4.5f, 0.0f, 1.0f));
        });
    };

    auto stopListeningAndProcess = [&]() {
        if (!isListening.exchange(false)) return;
        micLevel.store(0.f);

        tray->setState(platform::TrayState::Processing);
        mainWindow->setDictationState(ui::DictationState::Processing, "Transcribing via " + transcribeModel + "...");
        mainWindow->setAudioLevel(0.0f);

        if (showFloatingOverlay) {
            popup.setState(ui::PopupState::Processing);
            popup.setPartialTranscript({});
        }

        if (apiKey.empty()) {
            audio->stop();
            popup.setResultLabel("Set apiKey in config.json or UI");
            popup.setState(ui::PopupState::Result);
            tray->setState(platform::TrayState::Idle);
            mainWindow->setDictationState(ui::DictationState::Error, "No API key configured. Enter key in Settings tab.");
            isResult = true;
            resultTimer = 2.4f;
            return;
        }

        std::thread([&audio, httpClient, apiKey, endpointBase, transcribeModel, injectionMode, &popup, &isResult, &resultTimer, &tray, mainWindow, silenceSensitivity, &pcmMtx, &pcmCaptured, &liveState]() {
            audio->stop();

            int spin = 0;
            while (liveState.workerBusy.load() && spin++ < 20) {
                Sleep(25);
            }

            std::vector<int16_t> audioSnapshot;
            {
                std::lock_guard l(pcmMtx);
                audioSnapshot.swap(pcmCaptured);
            }

            size_t totalSamples = audioSnapshot.size();
            float durationSec = float(totalSamples) / 16000.0f;

            bool hadLiveWords = false;
            {
                std::lock_guard l(liveState.mtx);
                hadLiveWords = !liveState.committedWords.empty();
            }

            if (totalSamples < 5600 && !hadLiveWords) { // < 0.35s
                popup.setResultLabel("Held too briefly (< 0.4s)");
                popup.setState(ui::PopupState::Result);
                tray->setState(platform::TrayState::Idle);
                mainWindow->setDictationState(ui::DictationState::Idle, "Held too briefly (< 0.4s)");
                isResult = true;
                resultTimer = 1.3f;
                return;
            }

            // Compute audio statistics for silence detection
            int16_t maxSample = 0;
            double sumSq = 0.0;
            int activeSpeechSamples = 0;
            for (int16_t s : audioSnapshot) {
                int16_t absS = std::abs(s);
                if (absS > maxSample) maxSample = absS;
                double norm = double(s) / 32768.0;
                sumSq += norm * norm;
                if (absS > 650) activeSpeechSamples++;
            }
            float peakNorm = float(maxSample) / 32767.0f;
            float rms = float(std::sqrt(sumSq / double(totalSamples)));

            mainWindow->updateRecordingStats(durationSec, totalSamples, peakNorm, rms);

            // Always export last_recording.wav so user can play anytime in the UI
            std::vector<uint8_t> wavBytes = pcmToWav(audioSnapshot, 16000);
            {
                std::ofstream wavOut("last_recording.wav", std::ios::binary);
                if (wavOut.is_open()) {
                    wavOut.write(reinterpret_cast<const char*>(wavBytes.data()), wavBytes.size());
                    wavOut.close();
                }
            }

            // Silence thresholds based on sensitivity setting
            float minPeak = 0.030f;
            float minRms = 0.005f;
            int minActive = 800;
            if (silenceSensitivity == "high") {
                minPeak = 0.015f;
                minRms = 0.0025f;
                minActive = 400;
            } else if (silenceSensitivity == "low") {
                minPeak = 0.050f;
                minRms = 0.010f;
                minActive = 1200;
            }

            if (!hadLiveWords && (peakNorm < minPeak || rms < minRms || activeSpeechSamples < minActive)) {
                popup.setResultLabel("No speech detected");
                popup.setState(ui::PopupState::Result);
                tray->setState(platform::TrayState::Idle);
                mainWindow->setDictationState(ui::DictationState::Idle, "No speech detected (Skipped Gemini API call)");
                isResult = true;
                resultTimer = 1.8f;
                return;
            }

            std::string text = requestTranscription(httpClient, endpointBase, transcribeModel, apiKey, audioSnapshot, 10000);

            if (injectionMode == "live") {
                auto inj = platform::IInputInjector::create();
                std::string finalText = text;
                if (finalText.empty() && hadLiveWords) {
                    std::lock_guard l(liveState.mtx);
                    for (size_t i = 0; i < liveState.committedWords.size(); ++i) {
                        if (i > 0) finalText += " ";
                        finalText += liveState.committedWords[i];
                    }
                }

                if (!finalText.empty()) {
                    std::vector<std::string> finalWords = splitWords(finalText);
                    std::lock_guard l(liveState.mtx);
                    size_t matchCount = 0;
                    size_t maxMatch = std::min(liveState.committedWords.size(), finalWords.size());
                    while (matchCount < maxMatch && liveState.committedWords[matchCount] == finalWords[matchCount]) {
                        matchCount++;
                    }

                    size_t wordsToRemove = liveState.committedWords.size() - matchCount;
                    if (wordsToRemove > 0 && wordsToRemove <= 3) {
                        int bs = 0;
                        for (size_t w = matchCount; w < liveState.committedWords.size(); ++w) {
                            bs += int(liveState.committedWords[w].size());
                            if (w > 0) bs += 1;
                        }
                        for (int b = 0; b < bs; ++b) {
                            inj->sendKey(VK_BACK, 0);
                        }
                    }

                    std::string toAppend;
                    for (size_t w = matchCount; w < finalWords.size(); ++w) {
                        if (w == 0 && matchCount == 0 && liveState.committedWords.empty()) {
                            toAppend += finalWords[w];
                        } else {
                            toAppend += " " + finalWords[w];
                        }
                    }
                    if (!toAppend.empty()) {
                        inj->typeText(toAppend);
                    }

                    std::string preview = finalText;
                    if (preview.size() > 24) preview = preview.substr(0, 21) + "...";
                    popup.setResultLabel(preview);

                    std::ostringstream durSs;
                    durSs << std::fixed << std::setprecision(1) << durationSec << "s";
                    mainWindow->setTranscript(finalText, true, durSs.str());
                    mainWindow->setDictationState(ui::DictationState::Success, "Live Dictated directly into active app!");
                } else {
                    popup.setResultLabel("Empty transcript");
                    mainWindow->setDictationState(ui::DictationState::Idle, "Empty transcript returned by Gemini");
                }
            } else {
                if (!text.empty()) {
                    auto cb = platform::IClipboard::create();
                    cb->setText(text);
                    auto inj = platform::IInputInjector::create();

                    if (injectionMode == "typing" || injectionMode == "type") {
                        inj->typeText(text);
                    } else {
                        inj->pasteFromClipboard();
                    }

                    std::string preview = text;
                    if (preview.size() > 24) preview = preview.substr(0, 21) + "...";
                    popup.setResultLabel(preview);

                    std::ostringstream durSs;
                    durSs << std::fixed << std::setprecision(1) << durationSec << "s";
                    mainWindow->setTranscript(text, true, durSs.str());
                    mainWindow->setDictationState(ui::DictationState::Success, "Transcribed & Pasted into active app!");
                } else {
                    popup.setResultLabel("Empty transcript");
                    mainWindow->setDictationState(ui::DictationState::Idle, "Empty transcript returned by Gemini");
                }
            }

            popup.setState(ui::PopupState::Result);
            tray->setState(platform::TrayState::Idle);
            isResult = true;
            resultTimer = 1.8f;
        }).detach();
    };

    // Tray Menu
    tray->setMenu({
        {"Open VoxCast Studio", [&] { mainWindow->show(); }},
        {"Start / Stop Dictate", [&] { if (isListening.load()) stopListeningAndProcess(); else startListening(); }},
        {"Minimize to Tray", [&] { mainWindow->hide(); }},
        {"Quit VoxCast", [] { PostQuitMessage(0); }}
    });

    // MainWindow Callbacks
    mainWindow->onStartDictate = [&]() {
        startListening();
    };
    mainWindow->onStopDictate = [&]() {
        stopListeningAndProcess();
    };
    mainWindow->onPlayLastAudio = []() {
        PlaySoundA("last_recording.wav", nullptr, SND_ASYNC | SND_FILENAME);
    };
    mainWindow->onOpenAudioFolder = []() {
        ShellExecuteA(nullptr, "open", ".", nullptr, nullptr, SW_SHOW);
    };
    mainWindow->onSaveConfig = [&](const ui::AppConfig& newCfg) {
        apiKey = newCfg.apiKey;
        injectionMode = newCfg.injectionMode;
        transcribeModel = newCfg.transcribeModel;
        endpointBase = newCfg.endpointBase;
        silenceSensitivity = newCfg.silenceSensitivity;
        showFloatingOverlay = newCfg.showFloatingOverlay;
        hotkeyMode = newCfg.hotkeyMode;
        saveConfigToFile(newCfg);
    };
    mainWindow->onTestApiConnection = [httpClient, mainWindow](const std::string& key, const std::string& model, const std::string& endpoint) {
        std::thread([httpClient, mainWindow, key, model, endpoint]() {
            net::Request req;
            req.url = endpoint + "/models/" + model + "?key=" + key;
            req.method = "GET";
            req.headers["Content-Type"] = "application/json";
            req.timeoutMs = 8000;

            auto t0 = clk::now();
            auto resp = httpClient->send(req);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();

            if (resp.ok()) {
                mainWindow->setApiTestResult(true, resp.status, ms, "Connected to " + model + " successfully!");
            } else {
                net::Request req2;
                req2.url = endpoint + "/models/" + model + ":generateContent?key=" + key;
                req2.method = "POST";
                req2.headers["Content-Type"] = "application/json";
                req2.body = "{\"contents\":[{\"parts\":[{\"text\":\"ping\"}]}]}";
                req2.timeoutMs = 8000;

                auto t1 = clk::now();
                auto resp2 = httpClient->send(req2);
                auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t1).count();

                if (resp2.ok()) {
                    mainWindow->setApiTestResult(true, resp2.status, ms2, "Connected to " + model + " successfully!");
                } else {
                    std::string err = resp2.body;
                    if (err.size() > 50) err = err.substr(0, 47) + "...";
                    mainWindow->setApiTestResult(false, resp2.status, ms2, err.empty() ? "Connection failed" : err);
                }
            }
        }).detach();
    };
    mainWindow->onQuit = []() {
        PostQuitMessage(0);
    };

    auto hk = platform::IHotkeyService::create();
    auto registerHook = [&]() {
        return hk->bind(hkBinding, [&](platform::HotkeyEdge edge) {
            if (hotkeyMode == "toggle") {
                if (edge == platform::HotkeyEdge::Down) {
                    if (isListening.load()) stopListeningAndProcess();
                    else startListening();
                }
            } else {
                if (edge == platform::HotkeyEdge::Down) {
                    startListening();
                } else {
                    stopListeningAndProcess();
                }
            }
        });
    };
    bool bound = registerHook();

    mainWindow->onHotkeyChanged = [&](uint32_t newVk, const std::string& newName, bool newIsMod) {
        vk = newVk;
        disp = newName;
        isMod = newIsMod;
        hkBinding.keyCode = newVk;
        hkBinding.display = newName;
        hkBinding.isModifierOnly = newIsMod;
        hkBinding.mode = platform::HotkeyMode::Both;
        hk->unbind();
        registerHook();

        ui::AppConfig c = mainWindow->getConfig();
        c.hotkeyVk = newVk;
        c.hotkeyDisplay = newName;
        c.hotkeyIsMod = newIsMod;
        saveConfigToFile(c);
    };

    auto lastTick = clk::now();
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Active physical key state watchdog for hold mode:
        if (hotkeyMode == "hold" && isListening.load(std::memory_order_relaxed)) {
            bool physicallyDown = false;
            if (vk == VK_LCONTROL) {
                physicallyDown = ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) ||
                                 ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            } else if (vk == VK_RCONTROL) {
                physicallyDown = ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0) ||
                                 ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            } else if (vk == VK_CONTROL) {
                physicallyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            } else {
                physicallyDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            }

            if (!physicallyDown) {
                stopListeningAndProcess();
            }
        }

        const auto now = clk::now();
        const float dt = std::chrono::duration<float>(now - lastTick).count();
        lastTick = now;

        if (isListening.load(std::memory_order_relaxed)) {
            float lvl = micLevel.load(std::memory_order_relaxed);
            if (showFloatingOverlay) popup.pushAudioLevel(std::clamp(lvl * 6.0f + 0.04f, 0.05f, 0.95f));

            if (injectionMode == "live" && !apiKey.empty() && !liveState.workerBusy.load()) {
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - liveState.lastChunkTime).count();
                if (elapsedMs >= 750) {
                    std::vector<int16_t> liveSnapshot;
                    {
                        std::lock_guard l(pcmMtx);
                        liveSnapshot = pcmCaptured;
                    }
                    size_t curSamples = liveSnapshot.size();
                    if (curSamples >= 12000 && curSamples > liveState.lastChunkSampleCount + 4000) {
                        liveState.workerBusy.store(true);
                        liveState.lastChunkTime = now;
                        liveState.lastChunkSampleCount = curSamples;

                        std::thread([httpClient, endpointBase, transcribeModel, apiKey, liveSnapshot, &liveState, &popup, mainWindow, &isListening]() {
                            std::string raw = requestTranscription(httpClient, endpointBase, transcribeModel, apiKey, liveSnapshot, 6000);
                            if (!raw.empty() && isListening.load(std::memory_order_relaxed)) {
                                std::vector<std::string> newWords = splitWords(raw);
                                if (!newWords.empty()) {
                                    std::lock_guard l(liveState.mtx);
                                    if (isListening.load(std::memory_order_relaxed)) {
                                        size_t matchCount = 0;
                                        size_t maxMatch = std::min(liveState.committedWords.size(), newWords.size());
                                        while (matchCount < maxMatch && liveState.committedWords[matchCount] == newWords[matchCount]) {
                                            matchCount++;
                                        }

                                        auto inj = platform::IInputInjector::create();
                                        size_t wordsToRemove = liveState.committedWords.size() - matchCount;
                                        if (wordsToRemove > 0 && wordsToRemove <= 3) {
                                            int bs = 0;
                                            for (size_t w = matchCount; w < liveState.committedWords.size(); ++w) {
                                                bs += int(liveState.committedWords[w].size());
                                                if (w > 0) bs += 1;
                                            }
                                            for (int b = 0; b < bs; ++b) {
                                                inj->sendKey(VK_BACK, 0);
                                            }
                                        }

                                        std::string toAppend;
                                        for (size_t w = matchCount; w < newWords.size(); ++w) {
                                            if (w == 0 && matchCount == 0 && liveState.committedWords.empty()) {
                                                toAppend += newWords[w];
                                            } else {
                                                toAppend += " " + newWords[w];
                                            }
                                        }

                                        if (!toAppend.empty()) {
                                            inj->typeText(toAppend);
                                        }

                                        liveState.committedWords = newWords;

                                        std::string preview = raw;
                                        if (preview.size() > 24) preview = preview.substr(0, 21) + "...";
                                        popup.setPartialTranscript(preview);
                                        mainWindow->setTranscript(raw, false);
                                    }
                                }
                            }
                            liveState.workerBusy.store(false);
                        }).detach();
                    }
                }
            }
        } else if (isResult) {
            resultTimer -= dt;
            if (resultTimer <= 0.0f) {
                isResult = false;
                if (showFloatingOverlay) win->hide();
                popup.setState(ui::PopupState::Idle);
            }
        }

        if (showFloatingOverlay && win->visible()) {
            popup.update(dt);
            renderer.beginFrame();
            popup.draw(renderer.canvas(), renderer.widthDp(), renderer.heightDp());
            renderer.endFrame();
        }

        Sleep(16);
    }
    return 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    theme::Theme::get().setMode(theme::Mode::Auto);

    std::string cliHotkey;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--selftest"))     { attachConsole(); return selftest(); }
        if (!std::strcmp(argv[i], "--version"))      { attachConsole(); std::printf("VoxCast 1.0.0\n"); std::fflush(stdout); return 0; }
        if (!std::strcmp(argv[i], "--overlay-demo")) {
            attachConsole();
            int secs = (i + 1 < argc) ? std::atoi(argv[i + 1]) : 6;
            return overlayDemo(secs > 0 ? secs : 6);
        }
        if (!std::strcmp(argv[i], "--hotkey") && i + 1 < argc) {
            cliHotkey = argv[++i];
        }
    }

    // Normal GUI app launch: ensure console window is completely hidden and detached
    hideConsole();

    return runApp(cliHotkey);
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return main(__argc, __argv);
}
#endif
