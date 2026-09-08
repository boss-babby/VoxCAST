// ============================================================================
//  VoxCast — ui/MainWindow.cpp
//
//  Modern Dark-Themed Desktop Management UI with GDI+ & Smooth Animations
// ============================================================================
#include "MainWindow.h"
#include <propidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

using namespace Gdiplus;

namespace vox::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string toNarrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void addRoundRect(GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.AddArc(x, y, d, d, 180.f, 90.f);
    path.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    path.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

static void drawCard(Graphics& g, float x, float y, float w, float h, float r, Color fill, Color border, float borderWidth = 1.0f) {
    GraphicsPath path;
    addRoundRect(path, x, y, w, h, r);
    SolidBrush b(fill);
    g.FillPath(&b, &path);
    Pen p(border, borderWidth);
    g.DrawPath(&p, &path);
}

static void drawButton(Graphics& g, float x, float y, float w, float h, float r,
                       Color col1, Color col2, const wchar_t* text,
                       Font& font, Color textCol, bool isHovered, Color borderColor = Color(0, 0, 0, 0)) {
    GraphicsPath path;
    addRoundRect(path, x, y, w, h, r);
    Color c1 = isHovered ? Color(col1.GetA(), std::min(255, col1.GetR() + 25), std::min(255, col1.GetG() + 25), std::min(255, col1.GetB() + 25)) : col1;
    Color c2 = isHovered ? Color(col2.GetA(), std::min(255, col2.GetR() + 25), std::min(255, col2.GetG() + 25), std::min(255, col2.GetB() + 25)) : col2;
    LinearGradientBrush lgb(PointF(x, y), PointF(x, y + h), c1, c2);
    g.FillPath(&lgb, &path);

    Pen p(borderColor.GetA() > 0 ? borderColor : Color(isHovered ? 90 : 30, 255, 255, 255), 1.0f);
    g.DrawPath(&p, &path);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tb(textCol);
    RectF rf(x, y, w, h);
    g.DrawString(text, -1, &font, rf, &sf, &tb);
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr UINT_PTR TIMER_ANIM         = 1;
constexpr UINT WM_APP_UPDATE_STATE    = WM_APP + 1;
constexpr UINT WM_APP_SET_TRANSCRIPT  = WM_APP + 2;
constexpr UINT WM_APP_API_TEST_RESULT = WM_APP + 3;
constexpr UINT WM_APP_UPDATE_STATS    = WM_APP + 4;

constexpr int ID_EDT_TRANSCRIPT       = 801;
constexpr int ID_EDT_API_KEY          = 802;
constexpr int ID_EDT_ENDPOINT         = 803;

// Modern Palette
static const Color cBg(255, 13, 15, 24);             // #0d0f18
static const Color cHeader(255, 20, 23, 36);         // #141724
static const Color cCard(255, 21, 24, 39);           // #151827
static const Color cCardBorder(255, 36, 42, 66);     // #242a42
static const Color cCardHover(255, 28, 33, 53);      // #1c2135
static const Color cCardActive(255, 33, 39, 64);     // #212740
static const Color cInput(255, 16, 19, 31);          // #10131f
static const Color cInputBorder(255, 45, 52, 82);    // #2d3452

static const Color cIndigo1(255, 99, 102, 241);      // #6366f1
static const Color cIndigo2(255, 79, 70, 229);       // #4f46e5
static const Color cViolet(255, 139, 92, 246);       // #8b5cf6
static const Color cRed1(255, 239, 68, 68);          // #ef4444
static const Color cRed2(255, 220, 38, 38);          // #dc2626
static const Color cGreen1(255, 16, 185, 129);       // #10b981
static const Color cGreen2(255, 5, 150, 105);        // #059669
static const Color cAmber(255, 245, 158, 11);        // #f59e0b

static const Color cTextPrimary(255, 248, 250, 252); // #f8fafc
static const Color cTextSecondary(255, 148, 163, 184);// #94a3b8
static const Color cTextMuted(255, 100, 116, 139);   // #64748b

struct AsyncTranscriptMsg {
    std::string text;
    bool appendHistory;
    std::string duration;
};

struct AsyncApiTestMsg {
    bool ok;
    long httpCode;
    long latencyMs;
    std::string message;
};

// ---------------------------------------------------------------------------
MainWindow::MainWindow() {
    GdiplusStartupInput gsi;
    GdiplusStartup(&gdiplusToken_, &gsi, nullptr);

    for (size_t i = 0; i < visualizerBars_.size(); ++i) {
        visualizerBars_[i].spring.configure(360.f, 24.f, 1.f);
        visualizerBars_[i].spring.snap(0.06f);
    }
}

MainWindow::~MainWindow() {
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_ANIM);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hBrushDarkInput_) DeleteObject(hBrushDarkInput_);
    if (hFontEditor_) DeleteObject(hFontEditor_);
    if (hFontApiKey_) DeleteObject(hFontApiKey_);

    if (gdiplusToken_) {
        GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
}

bool MainWindow::create(HINSTANCE hInstance, const AppConfig& initialConfig) {
    hInst_ = hInstance;
    config_ = initialConfig;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::wndProcStatic;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"VoxCastStudioWindowClass";
    RegisterClassExW(&wc);

    int width = 940;
    int height = 720;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - width) / 2;
    int posY = (screenH - height) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"VoxCast Studio — AI Speech-to-Text",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        posX, posY, width, height,
        nullptr, nullptr, hInstance, this
    );

    if (!hwnd_) return false;

    // Modern Windows 10/11 dark titlebar
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));

    hBrushDarkInput_ = CreateSolidBrush(RGB(16, 19, 31));
    hFontEditor_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    hFontApiKey_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");

    // Child text editors (positioned inside custom drawn cards)
    edtTranscript_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        52, 324, 836, 224, hwnd_, (HMENU)ID_EDT_TRANSCRIPT, hInst_, nullptr);
    SendMessageW(edtTranscript_, WM_SETFONT, (WPARAM)hFontEditor_, TRUE);

    edtApiKey_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_PASSWORD | ES_AUTOHSCROLL,
        56, 218, 680, 26, hwnd_, (HMENU)ID_EDT_API_KEY, hInst_, nullptr);
    SendMessageW(edtApiKey_, WM_SETFONT, (WPARAM)hFontApiKey_, TRUE);

    edtEndpoint_ = CreateWindowExW(0, L"EDIT", L"https://generativelanguage.googleapis.com/v1beta",
        WS_CHILD | ES_AUTOHSCROLL,
        56, 552, 680, 26, hwnd_, (HMENU)ID_EDT_ENDPOINT, hInst_, nullptr);
    SendMessageW(edtEndpoint_, WM_SETFONT, (WPARAM)hFontEditor_, TRUE);

    syncUiFromConfig();
    switchTab(0);

    // 60 Hz timer for smooth spring physics and animations
    SetTimer(hwnd_, TIMER_ANIM, 16, nullptr);

    return true;
}

void MainWindow::show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
}

void MainWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::toggleVisibility() {
    if (isVisible()) hide();
    else show();
}

bool MainWindow::isVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void MainWindow::switchTab(int tabIndex) {
    activeTab_ = tabIndex;

    ShowWindow(edtTranscript_, (activeTab_ == 0) ? SW_SHOW : SW_HIDE);
    ShowWindow(edtApiKey_, (activeTab_ == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(edtEndpoint_, (activeTab_ == 1) ? SW_SHOW : SW_HIDE);

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::syncUiFromConfig() {
    if (edtApiKey_) SetWindowTextW(edtApiKey_, toWide(config_.apiKey).c_str());
    if (edtEndpoint_) SetWindowTextW(edtEndpoint_, toWide(config_.endpointBase).c_str());
}

AppConfig MainWindow::readConfigFromUi() {
    AppConfig c = config_;
    if (edtApiKey_) {
        wchar_t buf[1024]{};
        GetWindowTextW(edtApiKey_, buf, 1024);
        c.apiKey = toNarrow(buf);
    }
    if (edtEndpoint_) {
        wchar_t buf[1024]{};
        GetWindowTextW(edtEndpoint_, buf, 1024);
        c.endpointBase = toNarrow(buf);
    }
    return c;
}

void MainWindow::updateActiveHotkeyDisplay(const std::string& display, uint32_t vk) {
    config_.hotkeyDisplay = display;
    config_.hotkeyVk = vk;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::setDictationState(DictationState state, const std::string& message) {
    std::lock_guard l(stateMutex_);
    dictState_ = state;
    statusMessage_ = message;
    if (hwnd_) PostMessageW(hwnd_, WM_APP_UPDATE_STATE, 0, 0);
}

void MainWindow::setAudioLevel(float level) {
    currentAudioLevel_ = level;
}

void MainWindow::setTranscript(const std::string& text, bool appendHistory, const std::string& durationStr) {
    auto* msg = new AsyncTranscriptMsg{ text, appendHistory, durationStr };
    if (hwnd_) PostMessageW(hwnd_, WM_APP_SET_TRANSCRIPT, (WPARAM)msg, 0);
    else delete msg;
}

void MainWindow::setApiTestResult(bool ok, long httpCode, long latencyMs, const std::string& message) {
    auto* msg = new AsyncApiTestMsg{ ok, httpCode, latencyMs, message };
    if (hwnd_) PostMessageW(hwnd_, WM_APP_API_TEST_RESULT, (WPARAM)msg, 0);
    else delete msg;
}

void MainWindow::setAudioDevices(const std::vector<std::pair<std::string, std::string>>& devices) {
    audioDevices_ = devices;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::updateRecordingStats(float durationSec, size_t samples, float peak, float rms) {
    std::ostringstream ss;
    ss << "Last Audio: " << std::fixed << std::setprecision(2) << durationSec << "s ("
       << samples << " samples) • Peak: " << peak << " • RMS: " << rms;
    lastAudioStats_ = ss.str();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Animation & Physics Update (60 FPS)
// ---------------------------------------------------------------------------
void MainWindow::onTimerTick(float dt) {
    time_ += dt;
    ripplePhase_ = std::fmod(ripplePhase_ + dt * 1.5f, 1.0f);

    if (copyNotificationTimer_ > 0.f) {
        copyNotificationTimer_ -= dt;
        if (copyNotificationTimer_ <= 0.f) {
            copyNotification_.clear();
        }
    }

    // Audio level smoothing
    float targetLvl = currentAudioLevel_;
    if (targetLvl > displayedAudioLevel_) {
        displayedAudioLevel_ += (targetLvl - displayedAudioLevel_) * 0.45f;
    } else {
        displayedAudioLevel_ += (targetLvl - displayedAudioLevel_) * 0.15f;
    }

    // Update 28-Bar Visualizer Springs
    for (size_t i = 0; i < visualizerBars_.size(); ++i) {
        visualizerBars_[i].jitter = 0.5f + 0.5f * std::sin(time_ * (3.4f + 0.35f * i) + float(i));

        if (dictState_ == DictationState::Listening) {
            float x = (float(i) / float(visualizerBars_.size() - 1)) * 2.f - 1.f;
            float envelope = 0.60f + 0.40f * std::cos(x * 1.35f);
            float energy = displayedAudioLevel_ * envelope * (0.80f + 0.40f * visualizerBars_[i].jitter);
            visualizerBars_[i].spring.setTarget(std::clamp(energy, 0.08f, 1.0f));
        } else if (dictState_ == DictationState::Processing) {
            // Smooth thinking wave
            float phase = time_ * 4.0f - float(i) * 0.28f;
            float wave = 0.15f + 0.55f * std::pow(std::max(0.f, std::sin(phase)), 3.f);
            visualizerBars_[i].spring.setTarget(wave);
        } else {
            // Idle gentle breathing wave
            float b = anim::breathe(time_, 3.2f);
            float x = float(i) / float(visualizerBars_.size());
            float wave = 0.06f + 0.09f * (0.5f + 0.5f * std::sin(time_ * 1.8f + x * 4.f));
            visualizerBars_[i].spring.setTarget(anim::lerp(0.04f, wave, b));
        }

        visualizerBars_[i].spring.update(dt);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Main Render Loop (GDI+)
// ---------------------------------------------------------------------------
void MainWindow::render(HDC hdc, int width, int height) {
    Bitmap backBuffer(width, height);
    Graphics g(&backBuffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Fill deep dark background
    SolidBrush bBg(cBg);
    g.FillRectangle(&bBg, 0, 0, width, height);

    FontFamily ff(L"Segoe UI");
    Font fTitle(&ff, 15.f, FontStyleRegular, UnitPixel);
    Font fHeader(&ff, 13.5f, FontStyleRegular, UnitPixel);
    Font fRegular(&ff, 12.f, FontStyleRegular, UnitPixel);
    Font fBold(&ff, 12.f, FontStyleRegular, UnitPixel);
    Font fSmall(&ff, 10.5f, FontStyleRegular, UnitPixel);
    Font fMono(L"Consolas", 11.f, FontStyleRegular, UnitPixel);
    SolidBrush tbMuted(cTextMuted);

    // -----------------------------------------------------------------------
    // Top Header Bar
    // -----------------------------------------------------------------------
    SolidBrush bHeader(cHeader);
    g.FillRectangle(&bHeader, 0, 0, width, 68);
    Pen pHeaderBorder(cCardBorder, 1.0f);
    g.DrawLine(&pHeaderBorder, 0, 68, width, 68);

    // Glowing App Logo Icon
    drawCard(g, 28, 14, 40, 40, 10, cIndigo1, cViolet);
    {
        SolidBrush tb(Color(255, 255, 255, 255));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF rLogo(28, 14, 40, 40);
        g.DrawString(L"🎙️", -1, &fHeader, rLogo, &sf, &tb);
    }

    // Title & Subtitle
    SolidBrush tbTitle(cTextPrimary);
    g.DrawString(L"VoxCast Studio", -1, &fTitle, PointF(78, 15), &tbTitle);
    g.DrawString(L"AI Speech-to-Text • Powered by Gemini 3.5", -1, &fSmall, PointF(80, 38), &tbMuted);

    // Minimize to Tray header button
    bool isMinHover = (hoveredBtn_ == 99);
    drawButton(g, width - 150, 18, 120, 32, 8,
               isMinHover ? Color(255, 38, 44, 66) : Color(255, 26, 30, 46),
               isMinHover ? Color(255, 38, 44, 66) : Color(255, 26, 30, 46),
               L"📥 Hide to Tray", fSmall, cTextSecondary, isMinHover, cCardBorder);

    // Live Status Pill in Header
    Color pillBg = Color(255, 22, 34, 30);
    Color pillBorder = cGreen1;
    std::wstring pillText = L"● READY (HOLD LEFT CTRL)";

    if (dictState_ == DictationState::Listening) {
        pillBg = Color(255, 50, 18, 22);
        pillBorder = cRed1;
        pillText = L"● RECORDING VOICE";
    } else if (dictState_ == DictationState::Processing) {
        pillBg = Color(255, 48, 36, 16);
        pillBorder = cAmber;
        pillText = L"● TRANSCRIBING VIA GEMINI...";
    } else if (dictState_ == DictationState::Success) {
        pillBg = Color(255, 18, 45, 32);
        pillBorder = cGreen1;
        pillText = L"● TRANSCRIBED & PASTED";
    } else if (dictState_ == DictationState::Error) {
        pillBg = Color(255, 55, 18, 20);
        pillBorder = cRed1;
        pillText = L"● ERROR / RATE LIMIT";
    }

    drawCard(g, width - 420, 20, 250, 28, 14, pillBg, pillBorder, 1.0f);
    {
        SolidBrush tb(pillBorder);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF rPill(width - 420, 20, 250, 28);
        g.DrawString(pillText.c_str(), -1, &fSmall, rPill, &sf, &tb);
    }

    // -----------------------------------------------------------------------
    // Segmented Navigation Tab Bar
    // -----------------------------------------------------------------------
    const wchar_t* kTabs[] = {
        L"🎙️ Studio & Dictate",
        L"✨ AI & Models",
        L"⌨️ Hotkey & Typing",
        L"🎤 Audio & Device",
        L"📜 History Log"
    };

    float tabTrackX = 28.0f;
    float tabTrackY = 82.0f;
    float tabTrackW = width - 56.0f;
    float tabTrackH = 38.0f;
    drawCard(g, tabTrackX, tabTrackY, tabTrackW, tabTrackH, 8, Color(255, 18, 21, 33), cCardBorder);

    float tabItemW = (tabTrackW - 8.0f) / 5.0f;
    for (int i = 0; i < 5; ++i) {
        float tx = tabTrackX + 4.0f + i * tabItemW;
        float ty = tabTrackY + 4.0f;
        float tw = tabItemW;
        float th = tabTrackH - 8.0f;

        bool isActive = (activeTab_ == i);
        bool isHover = (hoveredTab_ == i);

        if (isActive) {
            drawButton(g, tx, ty, tw, th, 6, cIndigo1, cViolet, kTabs[i], fRegular, Color(255, 255, 255, 255), false, cIndigo2);
        } else {
            Color fill = isHover ? Color(255, 28, 33, 52) : Color(0, 0, 0, 0);
            Color txt = isHover ? cTextPrimary : cTextSecondary;
            drawButton(g, tx, ty, tw, th, 6, fill, fill, kTabs[i], fRegular, txt, false);
        }
    }

    // -----------------------------------------------------------------------
    // TAB 0: Studio & Dictate
    // -----------------------------------------------------------------------
    if (activeTab_ == 0) {
        // Hero Recording Card
        drawCard(g, 28, 138, width - 56, 126, 12, cCard, cCardBorder);

        // Concentric ripples when recording
        if (dictState_ == DictationState::Listening) {
            for (int r = 0; r < 3; ++r) {
                float phase = std::fmod(ripplePhase_ + float(r) * 0.33f, 1.0f);
                float exp = phase * 22.0f;
                int alpha = int((1.0f - phase) * 110.0f);
                GraphicsPath ring;
                addRoundRect(ring, 48 - exp, 154 - exp, 200 + exp * 2.f, 44 + exp * 2.f, 22.f + exp);
                Pen rp(Color(alpha, 239, 68, 68), 1.5f);
                g.DrawPath(&rp, &ring);
            }
        }

        // Hero Dictate Button
        bool isListening = (dictState_ == DictationState::Listening);
        bool isRecHover = (hoveredBtn_ == 1001);
        Color b1 = isListening ? cRed1 : cIndigo1;
        Color b2 = isListening ? cRed2 : cIndigo2;
        const wchar_t* btnLbl = isListening ? L"⏹️ Stop Recording" : L"🎙️ Start Dictating";
        drawButton(g, 48, 154, 200, 44, 10, b1, b2, btnLbl, fRegular, Color(255, 255, 255, 255), isRecHover);

        // Spring-Animated 28-Bar Visualizer
        float barStartX = 276.0f;
        float barY = 156.0f;
        float barMaxH = 40.0f;
        float barTotalW = (width - 56) - 300.0f;
        float barW = 11.0f;
        float barGap = (barTotalW - (28 * barW)) / 27.0f;

        for (size_t i = 0; i < visualizerBars_.size(); ++i) {
            float val = visualizerBars_[i].spring.value();
            float h = std::max(4.0f, val * barMaxH);
            float bx = barStartX + i * (barW + barGap);
            float by = barY + (barMaxH - h);

            GraphicsPath barPath;
            addRoundRect(barPath, bx, by, barW, h, barW * 0.5f);
            LinearGradientBrush barBrush(PointF(bx, barY), PointF(bx, barY + barMaxH),
                                        Color(255, 244, 63, 94),   // Top: Rose
                                        Color(255, 16, 185, 129)); // Bottom: Emerald
            g.FillPath(&barBrush, &barPath);
        }

        // Status banner under button
        SolidBrush tbStatus(cTextSecondary);
        g.DrawString(toWide(statusMessage_).c_str(), -1, &fSmall, PointF(48, 206), &tbStatus);

        // Mode badge on Tab 0 (Clickable toggle)
        bool isLive = (config_.injectionMode == "live");
        std::wstring modeBadge = isLive ? L"⚡ Mode: Live Dictation (Type directly as you speak) • Click to toggle"
                                        : (config_.injectionMode == "instant" ? L"🚀 Mode: Instant Paste on Release (Ctrl+V) • Click to toggle"
                                                                              : L"⌨️ Mode: Simulated Typing on Release • Click to toggle");
        Color mBadgeFill = isLive ? Color(255, 24, 32, 56) : Color(255, 18, 22, 36);
        Color mBadgeBorder = isLive ? cIndigo1 : cCardBorder;
        drawCard(g, 48, 226, width - 96, 26, 6, mBadgeFill, mBadgeBorder);
        SolidBrush tbMB(isLive ? Color(255, 199, 210, 254) : cTextSecondary);
        g.DrawString(modeBadge.c_str(), -1, &fSmall, PointF(60, 232), &tbMB);

        // Live Transcript Card Container
        drawCard(g, 28, 274, width - 56, 346, 12, cCard, cCardBorder);

        // Transcript Header Bar
        SolidBrush tbTransTitle(cTextPrimary);
        g.DrawString(L"Live Transcription Output", -1, &fHeader, PointF(48, 288), &tbTransTitle);

        // Badges: Word count & copy status
        if (!copyNotification_.empty()) {
            drawCard(g, width - 240, 284, 170, 24, 12, Color(255, 18, 48, 34), cGreen1);
            SolidBrush tbCopy(cGreen1);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF rCopy(width - 240, 284, 170, 24);
            g.DrawString(toWide(copyNotification_).c_str(), -1, &fSmall, rCopy, &sf, &tbCopy);
        }

        // Background for edit box
        drawCard(g, 44, 318, width - 88, 236, 8, cInput, cInputBorder);

        // Transcript Action Buttons
        bool isCopyHover = (hoveredBtn_ == 1002);
        drawButton(g, 48, 568, 170, 36, 8, cIndigo1, cIndigo2, L"📋 Copy Transcript", fRegular, Color(255, 255, 255, 255), isCopyHover);

        bool isPlayHover = (hoveredBtn_ == 1003);
        drawButton(g, 230, 568, 170, 36, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), L"🔊 Play Last Audio", fRegular, cTextPrimary, isPlayHover, cCardBorder);

        bool isClearHover = (hoveredBtn_ == 1004);
        drawButton(g, 412, 568, 100, 36, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), L"🗑️ Clear", fRegular, cTextSecondary, isClearHover, cCardBorder);
    }

    // -----------------------------------------------------------------------
    // TAB 1: AI & Models
    // -----------------------------------------------------------------------
    else if (activeTab_ == 1) {
        // API Key Card
        drawCard(g, 28, 138, width - 56, 140, 12, cCard, cCardBorder);
        SolidBrush tb(cTextPrimary);
        g.DrawString(L"Google Gemini API Key", -1, &fHeader, PointF(48, 152), &tb);
        SolidBrush tbMuted(cTextMuted);
        g.DrawString(L"Your key is encrypted with Windows DPAPI and used for direct speech recognition.", -1, &fSmall, PointF(48, 176), &tbMuted);

        // Input frame
        drawCard(g, 48, 204, 690, 38, 8, cInput, cInputBorder);

        // Show/Hide Key toggle button
        bool isEyeHover = (hoveredBtn_ == 2001);
        const wchar_t* eyeText = showKeyPlaintext_ ? L"Hide" : L"Show";
        drawButton(g, 750, 204, 70, 38, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), eyeText, fSmall, cTextPrimary, isEyeHover, cCardBorder);

        // Test API Button & Health Status Badge
        bool isTestHover = (hoveredBtn_ == 2002);
        drawButton(g, 48, 252, 170, 34, 8, cIndigo1, cIndigo2, L"⚡ Test Connection", fRegular, Color(255, 255, 255, 255), isTestHover);

        Color resBg = apiTestOk_ ? Color(255, 18, 48, 32) : Color(255, 52, 18, 22);
        Color resBorder = apiTestOk_ ? cGreen1 : cRed1;
        drawCard(g, 230, 253, width - 290, 32, 8, resBg, resBorder);
        SolidBrush tbRes(resBorder);
        g.DrawString(toWide(apiTestResult_).c_str(), -1, &fSmall, PointF(244, 261), &tbRes);

        // Model Selection Grid
        drawCard(g, 28, 306, width - 56, 210, 12, cCard, cCardBorder);
        g.DrawString(L"Select Speech-to-Text Model", -1, &fHeader, PointF(48, 322), &tb);

        struct ModelOption {
            std::string id;
            const wchar_t* title;
            const wchar_t* badge;
            const wchar_t* desc;
        };
        ModelOption models[2] = {
            { "gemini-3.5-transcribe-live", L"⚡ Gemini 3.5 Transcribe Live", L"LIVE STREAMING", L"Dedicated low-latency streaming speech recognition engine. Writes directly into your active window in real-time as you speak." },
            { "gemini-3.5-transcribe", L"✨ Gemini 3.5 Transcribe", L"HIGH ACCURACY", L"High-fidelity speech-to-text model for precision audio analysis, verbatim accuracy, and batch transcription." }
        };

        float cardW = (width - 56 - 40 - 20) / 2.0f;
        for (int m = 0; m < 2; ++m) {
            float mx = 48.0f + m * (cardW + 20.0f);
            float my = 356.0f;
            float mh = 140.0f;

            bool isSelected = (config_.transcribeModel == models[m].id);
            bool isHover = (hoveredBtn_ == 2100 + m);

            Color fill = isSelected ? Color(255, 28, 34, 58) : (isHover ? cCardHover : cInput);
            Color border = isSelected ? cIndigo1 : (isHover ? cIndigo2 : cCardBorder);
            drawCard(g, mx, my, cardW, mh, 10, fill, border, isSelected ? 1.5f : 1.0f);

            SolidBrush tbMTitle(cTextPrimary);
            g.DrawString(models[m].title, -1, &fRegular, PointF(mx + 14, my + 14), &tbMTitle);

            // Badge
            drawCard(g, mx + cardW - 128, my + 14, 114, 20, 5, isSelected ? cIndigo1 : Color(255, 30, 36, 54), Color(0, 0, 0, 0));
            SolidBrush tbB(cTextPrimary);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            RectF rB(mx + cardW - 128, my + 14, 114, 20);
            g.DrawString(models[m].badge, -1, &fSmall, rB, &sf, &tbB);

            // Desc
            SolidBrush tbDesc(cTextSecondary);
            RectF rD(mx + 14, my + 44, cardW - 28, 80);
            g.DrawString(models[m].desc, -1, &fSmall, rD, nullptr, &tbDesc);
        }

        // Endpoint Card
        drawCard(g, 28, 528, width - 56, 90, 12, cCard, cCardBorder);
        g.DrawString(L"API Base Endpoint", -1, &fHeader, PointF(48, 536), &tb);
        drawCard(g, 48, 548, width - 96, 36, 8, cInput, cInputBorder);
    }

    // -----------------------------------------------------------------------
    // TAB 2: Hotkey & Typing Settings
    // -----------------------------------------------------------------------
    else if (activeTab_ == 2) {
        // Hotkey Selection Card
        drawCard(g, 28, 138, width - 56, 220, 12, cCard, cCardBorder);
        SolidBrush tb(cTextPrimary);
        g.DrawString(L"Global Dictation Hotkey", -1, &fHeader, PointF(48, 152), &tb);

        // Active key badge
        std::wstring activeBadge = L"Active Key:  " + toWide(config_.hotkeyDisplay);
        drawCard(g, 48, 186, 220, 38, 8, Color(255, 28, 34, 58), cIndigo1, 1.2f);
        SolidBrush tbAct(cTextPrimary);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF rAct(48, 186, 220, 38);
        g.DrawString(activeBadge.c_str(), -1, &fRegular, rAct, &sf, &tbAct);

        // Detect Key Interactive Button
        bool isDetectHover = (hoveredBtn_ == 3001);
        std::wstring detectLbl = detectingKey_ ? L"👉 Press ANY key on your keyboard now... (Esc to cancel)" : L"🎯 Detect Key: Click & Press Any Key";
        Color detBg1 = detectingKey_ ? cRed1 : (isDetectHover ? cViolet : cIndigo1);
        Color detBg2 = detectingKey_ ? cRed2 : (isDetectHover ? cIndigo1 : cIndigo2);
        drawButton(g, 280, 186, 460, 38, 8, detBg1, detBg2, detectLbl.c_str(), fRegular, Color(255, 255, 255, 255), isDetectHover);

        // Preset chips grid
        const wchar_t* kChips[] = {
            L"Left Ctrl", L"Right Ctrl", L"Caps Lock", L"Left Alt", L"Right Alt",
            L"Left Shift", L"Right Shift", L"F1", L"F2", L"F9", L"Space", L"Tab", L"Tilde (~)"
        };
        float chipX = 48.0f;
        float chipY = 240.0f;
        for (int c = 0; c < 13; ++c) {
            float cw = 110.0f;
            float ch = 34.0f;
            if (c == 7) { chipX = 48.0f; chipY += 42.0f; }

            bool isSel = (config_.hotkeyDisplay == toNarrow(kChips[c]));
            bool isHov = (hoveredBtn_ == 3100 + c);

            Color fill = isSel ? Color(255, 28, 34, 58) : (isHov ? cCardHover : cInput);
            Color border = isSel ? cIndigo1 : (isHov ? cIndigo2 : cCardBorder);
            drawButton(g, chipX, chipY, cw, ch, 8, fill, fill, kChips[c], fRegular, isSel ? Color(255, 255, 255, 255) : cTextSecondary, isHov, border);
            chipX += cw + 10.0f;
        }

        // Behavior & Mode Card
        drawCard(g, 28, 372, width - 56, 240, 12, cCard, cCardBorder);
        g.DrawString(L"Dictation Behavior & Insertion", -1, &fHeader, PointF(48, 388), &tb);

        // Activation Mode Options
        float optW = (width - 56 - 60) / 2.0f;
        bool isHold = (config_.hotkeyMode == "hold");
        drawCard(g, 48, 416, optW, 56, 8, isHold ? Color(255, 28, 34, 58) : cInput, isHold ? cIndigo1 : cCardBorder, isHold ? 1.5f : 1.0f);
        g.DrawString(L"🔘 Hold to Speak (Recommended)", -1, &fRegular, PointF(64, 426), &tb);
        g.DrawString(L"Speak while holding hotkey, release to transcribe instantly.", -1, &fSmall, PointF(64, 448), &tbMuted);

        drawCard(g, 48 + optW + 20, 416, optW, 56, 8, !isHold ? Color(255, 28, 34, 58) : cInput, !isHold ? cIndigo1 : cCardBorder, !isHold ? 1.5f : 1.0f);
        g.DrawString(L"🔘 Toggle Mode", -1, &fRegular, PointF(64 + optW + 20, 426), &tb);
        g.DrawString(L"Press once to start recording, press again to stop.", -1, &fSmall, PointF(64 + optW + 20, 448), &tbMuted);

        // Injection Mode Options: 3 Options (Live, Instant, Typing)
        float optW3 = (width - 56 - 60) / 3.0f;
        float injY = 484.0f;
        float injH = 68.0f;

        bool isLive = (config_.injectionMode == "live");
        bool isInstant = (config_.injectionMode == "instant");
        bool isTyping = (config_.injectionMode == "typing" || config_.injectionMode == "type");

        // 1. Live Dictation
        drawCard(g, 48, injY, optW3, injH, 8, isLive ? Color(255, 28, 34, 58) : cInput, isLive ? cIndigo1 : cCardBorder, isLive ? 1.5f : 1.0f);
        SolidBrush tbLive(isLive ? Color(255, 199, 210, 254) : cTextPrimary);
        g.DrawString(L"⚡ Live Dictation (Type as you speak)", -1, &fRegular, PointF(60, injY + 10), &tbLive);
        g.DrawString(L"Gemini 3.5 streams & writes directly into your window in real time.", -1, &fSmall, RectF(60, injY + 30, optW3 - 20, 34), nullptr, &tbMuted);

        // 2. Instant Paste
        drawCard(g, 48 + optW3 + 15, injY, optW3, injH, 8, isInstant ? Color(255, 28, 34, 58) : cInput, isInstant ? cIndigo1 : cCardBorder, isInstant ? 1.5f : 1.0f);
        SolidBrush tbInst(isInstant ? Color(255, 199, 210, 254) : cTextPrimary);
        g.DrawString(L"🚀 Instant Paste (Ctrl+V)", -1, &fRegular, PointF(60 + optW3 + 15, injY + 10), &tbInst);
        g.DrawString(L"Transcribes on release and pastes whole text at cursor.", -1, &fSmall, RectF(60 + optW3 + 15, injY + 30, optW3 - 20, 34), nullptr, &tbMuted);

        // 3. Simulated Typing
        drawCard(g, 48 + (optW3 + 15) * 2, injY, optW3, injH, 8, isTyping ? Color(255, 28, 34, 58) : cInput, isTyping ? cIndigo1 : cCardBorder, isTyping ? 1.5f : 1.0f);
        SolidBrush tbType(isTyping ? Color(255, 199, 210, 254) : cTextPrimary);
        g.DrawString(L"⌨️ Simulated Typing", -1, &fRegular, PointF(60 + (optW3 + 15) * 2, injY + 10), &tbType);
        g.DrawString(L"Simulates typewriter keystrokes character-by-character.", -1, &fSmall, RectF(60 + (optW3 + 15) * 2, injY + 30, optW3 - 20, 34), nullptr, &tbMuted);

        // Floating overlay checkbox switch
        bool isOver = config_.showFloatingOverlay;
        drawCard(g, 48, 564, width - 96, 32, 8, isOver ? Color(255, 24, 30, 48) : cInput, isOver ? cIndigo1 : cCardBorder);
        g.DrawString(isOver ? L"☑️ Show floating status capsule overlay during global dictation" : L"⬜ Show floating status capsule overlay during global dictation", -1, &fRegular, PointF(64, 572), &tb);
    }

    // -----------------------------------------------------------------------
    // TAB 3: Audio & Device Settings
    // -----------------------------------------------------------------------
    else if (activeTab_ == 3) {
        // Device Card
        drawCard(g, 28, 138, width - 56, 170, 14, cCard, cCardBorder);
        SolidBrush tb(cTextPrimary);
        g.DrawString(L"Select Audio Recording Device", -1, &fHeader, PointF(48, 152), &tb);

        float devY = 186.0f;
        for (size_t d = 0; d < std::min((size_t)3, audioDevices_.size()); ++d) {
            bool isSel = (config_.audioDeviceId == audioDevices_[d].first) || (d == 0 && config_.audioDeviceId == "default");
            drawCard(g, 48, devY, width - 96, 36, 8, isSel ? Color(255, 28, 34, 58) : cInput, isSel ? cIndigo1 : cCardBorder);
            std::wstring label = (isSel ? L"✓ " : L"   ") + toWide(audioDevices_[d].second);
            g.DrawString(label.c_str(), -1, &fRegular, PointF(64, devY + 8), &tb);
            devY += 44.0f;
        }

        // Silence Gate Sensitivity Card
        drawCard(g, 28, 322, width - 56, 160, 12, cCard, cCardBorder);
        g.DrawString(L"Silence Detection & Noise Gate", -1, &fHeader, PointF(48, 338), &tb);

        float sensW = (width - 56 - 60) / 3.0f;
        struct SensOpt {
            std::string id;
            const wchar_t* title;
            const wchar_t* desc;
        };
        SensOpt sens[3] = {
            { "normal", L"🔘 Balanced (Default)", L"Peak 0.030 • Standard home or office" },
            { "high", L"🔘 High Sensitivity", L"Peak 0.015 • Whispering or quiet room" },
            { "low", L"🔘 Low Sensitivity", L"Peak 0.050 • Noisy room or loud typing" }
        };

        for (int s = 0; s < 3; ++s) {
            float sx = 48.0f + s * (sensW + 15.0f);
            float sy = 372.0f;
            bool isSel = (config_.silenceSensitivity == sens[s].id);
            drawCard(g, sx, sy, sensW, 80, 10, isSel ? Color(255, 28, 34, 58) : cInput, isSel ? cIndigo1 : cCardBorder, isSel ? 1.5f : 1.0f);
            g.DrawString(sens[s].title, -1, &fRegular, PointF(sx + 14, sy + 14), &tb);
            SolidBrush tbM(cTextMuted);
            g.DrawString(sens[s].desc, -1, &fSmall, PointF(sx + 14, sy + 44), &tbM);
        }

        // Audio Diagnostics Card
        drawCard(g, 28, 496, width - 56, 120, 12, cCard, cCardBorder);
        g.DrawString(L"Audio Diagnostics & Inspection", -1, &fHeader, PointF(48, 510), &tb);
        g.DrawString(toWide(lastAudioStats_).c_str(), -1, &fRegular, PointF(48, 540), &tb);

        bool isPlay3Hover = (hoveredBtn_ == 4001);
        drawButton(g, 48, 568, 220, 36, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), L"🔊 Play last_recording.wav", fRegular, cTextPrimary, isPlay3Hover, cCardBorder);

        bool isFolderHover = (hoveredBtn_ == 4002);
        drawButton(g, 280, 568, 200, 36, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), L"📁 Open App Directory", fRegular, cTextPrimary, isFolderHover, cCardBorder);
    }

    // -----------------------------------------------------------------------
    // TAB 4: History Log
    // -----------------------------------------------------------------------
    else if (activeTab_ == 4) {
        drawCard(g, 28, 138, width - 56, 478, 12, cCard, cCardBorder);
        SolidBrush tb(cTextPrimary);
        g.DrawString(L"Session Dictation History", -1, &fHeader, PointF(48, 152), &tb);

        if (history_.empty()) {
            SolidBrush tbEmpty(cTextMuted);
            g.DrawString(L"No dictations recorded in this session yet.", -1, &fRegular, PointF(48, 200), &tbEmpty);
        } else {
            float histY = 186.0f;
            for (size_t h = 0; h < std::min((size_t)7, history_.size()); ++h) {
                bool isSel = (selectedHistoryIdx_ == (int)h);
                drawCard(g, 48, histY, width - 96, 48, 8, isSel ? Color(255, 28, 34, 58) : cInput, isSel ? cIndigo1 : cCardBorder);

                std::wstring ts = toWide(history_[h].timestamp + " [" + history_[h].duration + "]  " + history_[h].text);
                g.DrawString(ts.c_str(), -1, &fMono, PointF(60, histY + 14), &tb);
                histY += 54.0f;
            }

            bool isCopyHHover = (hoveredBtn_ == 5001);
            drawButton(g, 48, 566, 180, 36, 8, cIndigo1, cIndigo2, L"📋 Copy Selected", fRegular, Color(255, 255, 255, 255), isCopyHHover);

            bool isClearHHover = (hoveredBtn_ == 5002);
            drawButton(g, 240, 566, 140, 36, 8, Color(255, 30, 35, 54), Color(255, 30, 35, 54), L"🗑️ Clear History", fRegular, cTextSecondary, isClearHHover, cCardBorder);
        }
    }

    // -----------------------------------------------------------------------
    // Persistent Footer Bar
    // -----------------------------------------------------------------------
    SolidBrush bFooter(Color(255, 17, 20, 32));
    g.FillRectangle(&bFooter, 0, height - 64, width, 64);
    Pen pFootBorder(cCardBorder, 1.0f);
    g.DrawLine(&pFootBorder, 0, height - 64, width, height - 64);

    // Save & Apply Settings Button (Emerald gradient)
    bool isSaveHover = (hoveredBtn_ == 9001);
    drawButton(g, 28, height - 50, 220, 38, 8, cGreen1, cGreen2, L"💾 Save & Apply Settings", fRegular, Color(255, 255, 255, 255), isSaveHover);

    // Footer Sync Status
    SolidBrush tbFoot(cTextMuted);
    g.DrawString(L"● All settings synchronized with config.json", -1, &fSmall, PointF(264, height - 38), &tbFoot);

    // Blit to screen
    Graphics screen(hdc);
    screen.DrawImage(&backBuffer, 0, 0);
}

// ---------------------------------------------------------------------------
// Mouse Hit Testing & Click Handling
// ---------------------------------------------------------------------------
void MainWindow::onMouseDown(int x, int y) {
    // 1. Tab switches
    if (y >= 82 && y <= 124) {
        float tabTrackW = 940 - 56;
        float itemW = (tabTrackW - 8.0f) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            float tx = 28.0f + 4.0f + i * itemW;
            if (x >= tx && x <= tx + itemW) {
                switchTab(i);
                return;
            }
        }
    }

    // 2. Hide to Tray button
    if (x >= 940 - 150 && x <= 940 - 30 && y >= 18 && y <= 50) {
        hide();
        return;
    }

    // 3. Save & Apply footer button
    if (x >= 28 && x <= 248 && y >= 720 - 50 && y <= 720 - 12) {
        config_ = readConfigFromUi();
        if (onSaveConfig) onSaveConfig(config_);
        copyNotification_ = "✓ Settings Saved & Applied!";
        copyNotificationTimer_ = 2.5f;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // TAB 0 Controls
    if (activeTab_ == 0) {
        // Hero Dictate Button
        if (x >= 48 && x <= 248 && y >= 154 && y <= 198) {
            if (dictState_ == DictationState::Listening) {
                if (onStopDictate) onStopDictate();
            } else {
                if (onStartDictate) onStartDictate();
            }
            return;
        }
        // Mode badge on Tab 0 (Clickable toggle)
        if (x >= 48 && x <= 940 - 48 && y >= 226 && y <= 252) {
            if (config_.injectionMode == "live") config_.injectionMode = "instant";
            else if (config_.injectionMode == "instant") config_.injectionMode = "typing";
            else config_.injectionMode = "live";
            if (onSaveConfig) onSaveConfig(config_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Copy Transcript
        if (x >= 48 && x <= 218 && y >= 568 && y <= 604) {
            int len = GetWindowTextLengthW(edtTranscript_);
            if (len > 0) {
                std::wstring w(len + 1, 0);
                GetWindowTextW(edtTranscript_, &w[0], len + 1);
                if (OpenClipboard(hwnd_)) {
                    EmptyClipboard();
                    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                    if (hGlob) {
                        memcpy(GlobalLock(hGlob), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(hGlob);
                        SetClipboardData(CF_UNICODETEXT, hGlob);
                    }
                    CloseClipboard();
                    copyNotification_ = "✓ Copied to Clipboard!";
                    copyNotificationTimer_ = 2.0f;
                }
            }
            return;
        }
        // Play Last Audio
        if (x >= 230 && x <= 400 && y >= 568 && y <= 604) {
            if (onPlayLastAudio) onPlayLastAudio();
            else PlaySoundA("last_recording.wav", nullptr, SND_ASYNC | SND_FILENAME);
            return;
        }
        // Clear
        if (x >= 412 && x <= 512 && y >= 568 && y <= 604) {
            SetWindowTextW(edtTranscript_, L"");
            return;
        }
    }

    // TAB 1 Controls (AI & Models)
    else if (activeTab_ == 1) {
        // Show/Hide Key toggle
        if (x >= 750 && x <= 820 && y >= 204 && y <= 242) {
            showKeyPlaintext_ = !showKeyPlaintext_;
            SendMessageW(edtApiKey_, EM_SETPASSWORDCHAR, showKeyPlaintext_ ? 0 : L'●', 0);
            InvalidateRect(edtApiKey_, nullptr, TRUE);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Test API Connection
        if (x >= 48 && x <= 228 && y >= 252 && y <= 288) {
            config_ = readConfigFromUi();
            apiTestResult_ = "Testing connection to Gemini API...";
            apiTestOk_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onTestApiConnection) {
                onTestApiConnection(config_.apiKey, config_.transcribeModel, config_.endpointBase);
            }
            return;
        }
        // Model Selection Cards (2 dedicated Gemini 3.5 models)
        float cardW = (940 - 56 - 40 - 20) / 2.0f;
        for (int m = 0; m < 2; ++m) {
            float mx = 48.0f + m * (cardW + 20.0f);
            if (x >= mx && x <= mx + cardW && y >= 356 && y <= 496) {
                if (m == 0) config_.transcribeModel = "gemini-3.5-transcribe-live";
                else config_.transcribeModel = "gemini-3.5-transcribe";
                if (onSaveConfig) onSaveConfig(config_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
    }

    // TAB 2 Controls (Hotkey & Typing)
    else if (activeTab_ == 2) {
        // Detect Key Button
        if (x >= 280 && x <= 740 && y >= 186 && y <= 224) {
            detectingKey_ = true;
            SetFocus(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Preset Chips
        const wchar_t* kChips[] = {
            L"Left Ctrl", L"Right Ctrl", L"Caps Lock", L"Left Alt", L"Right Alt",
            L"Left Shift", L"Right Shift", L"F1", L"F2", L"F9", L"Space", L"Tab", L"Tilde (~)"
        };
        float chipX = 48.0f;
        float chipY = 240.0f;
        for (int c = 0; c < 13; ++c) {
            float cw = 110.0f;
            float ch = 34.0f;
            if (c == 7) { chipX = 48.0f; chipY += 42.0f; }

            if (x >= chipX && x <= chipX + cw && y >= chipY && y <= chipY + ch) {
                std::string strName = toNarrow(kChips[c]);
                uint32_t vk = VK_LCONTROL;
                bool isMod = true;
                if (strName == "Left Ctrl") vk = VK_LCONTROL;
                else if (strName == "Right Ctrl") vk = VK_RCONTROL;
                else if (strName == "Caps Lock") { vk = VK_CAPITAL; isMod = false; }
                else if (strName == "Left Alt") vk = VK_LMENU;
                else if (strName == "Right Alt") vk = VK_RMENU;
                else if (strName == "Left Shift") vk = VK_LSHIFT;
                else if (strName == "Right Shift") vk = VK_RSHIFT;
                else if (strName == "Space") { vk = VK_SPACE; isMod = false; }
                else if (strName == "Tab") { vk = VK_TAB; isMod = false; }
                else if (strName == "Tilde (~)") { vk = VK_OEM_3; isMod = false; }
                else if (strName == "F1") { vk = VK_F1; isMod = false; }
                else if (strName == "F2") { vk = VK_F2; isMod = false; }
                else if (strName == "F9") { vk = VK_F9; isMod = false; }

                config_.hotkeyDisplay = strName;
                config_.hotkeyVk = vk;
                config_.hotkeyIsMod = isMod;
                if (onHotkeyChanged) onHotkeyChanged(vk, strName, isMod);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            chipX += cw + 10.0f;
        }

        // Hold vs Toggle mode
        float optW = (940 - 56 - 60) / 2.0f;
        if (y >= 416 && y <= 472) {
            if (x >= 48 && x <= 48 + optW) config_.hotkeyMode = "hold";
            else if (x >= 48 + optW + 20 && x <= 48 + optW * 2 + 20) config_.hotkeyMode = "toggle";
            if (onSaveConfig) onSaveConfig(config_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Injection Mode Options: Live, Instant, Typing
        float optW3 = (940 - 56 - 60) / 3.0f;
        if (y >= 484 && y <= 552) {
            if (x >= 48 && x <= 48 + optW3) config_.injectionMode = "live";
            else if (x >= 48 + optW3 + 15 && x <= 48 + (optW3 + 15) + optW3) config_.injectionMode = "instant";
            else if (x >= 48 + (optW3 + 15) * 2 && x <= 48 + (optW3 + 15) * 2 + optW3) config_.injectionMode = "typing";
            if (onSaveConfig) onSaveConfig(config_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Floating pill toggle
        if (x >= 48 && x <= 940 - 48 && y >= 564 && y <= 596) {
            config_.showFloatingOverlay = !config_.showFloatingOverlay;
            if (onSaveConfig) onSaveConfig(config_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // TAB 3 Controls (Audio & Device)
    else if (activeTab_ == 3) {
        // Sensitivity options
        float sensW = (940 - 56 - 60) / 3.0f;
        for (int s = 0; s < 3; ++s) {
            float sx = 48.0f + s * (sensW + 15.0f);
            if (x >= sx && x <= sx + sensW && y >= 372 && y <= 452) {
                if (s == 0) config_.silenceSensitivity = "normal";
                else if (s == 1) config_.silenceSensitivity = "high";
                else config_.silenceSensitivity = "low";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        // Play audio
        if (x >= 48 && x <= 268 && y >= 568 && y <= 604) {
            if (onPlayLastAudio) onPlayLastAudio();
            else PlaySoundA("last_recording.wav", nullptr, SND_ASYNC | SND_FILENAME);
            return;
        }
        // Open folder
        if (x >= 280 && x <= 480 && y >= 568 && y <= 604) {
            if (onOpenAudioFolder) onOpenAudioFolder();
            else ShellExecuteA(nullptr, "open", ".", nullptr, nullptr, SW_SHOW);
            return;
        }
    }

    // TAB 4 Controls (History)
    else if (activeTab_ == 4) {
        float histY = 186.0f;
        for (size_t h = 0; h < std::min((size_t)7, history_.size()); ++h) {
            if (x >= 48 && x <= 940 - 48 && y >= histY && y <= histY + 48) {
                selectedHistoryIdx_ = (int)h;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            histY += 54.0f;
        }
        // Copy selected
        if (x >= 48 && x <= 228 && y >= 566 && y <= 602) {
            if (selectedHistoryIdx_ >= 0 && selectedHistoryIdx_ < (int)history_.size()) {
                std::wstring textW = toWide(history_[selectedHistoryIdx_].text);
                if (OpenClipboard(hwnd_)) {
                    EmptyClipboard();
                    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (textW.size() + 1) * sizeof(wchar_t));
                    if (hGlob) {
                        memcpy(GlobalLock(hGlob), textW.c_str(), (textW.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(hGlob);
                        SetClipboardData(CF_UNICODETEXT, hGlob);
                    }
                    CloseClipboard();
                    copyNotification_ = "✓ History Entry Copied!";
                    copyNotificationTimer_ = 2.0f;
                }
            }
            return;
        }
        // Clear history
        if (x >= 240 && x <= 380 && y >= 566 && y <= 602) {
            history_.clear();
            selectedHistoryIdx_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
}

void MainWindow::onMouseMove(int x, int y) {
    int oldTab = hoveredTab_;
    int oldBtn = hoveredBtn_;
    hoveredTab_ = -1;
    hoveredBtn_ = -1;

    // Tabs
    if (y >= 82 && y <= 124) {
        float tabTrackW = 940 - 56;
        float itemW = (tabTrackW - 8.0f) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            float tx = 28.0f + 4.0f + i * itemW;
            if (x >= tx && x <= tx + itemW) {
                hoveredTab_ = i;
                break;
            }
        }
    }

    // Hide to Tray
    if (x >= 940 - 150 && x <= 940 - 30 && y >= 18 && y <= 50) hoveredBtn_ = 99;

    // Save & Apply
    if (x >= 28 && x <= 248 && y >= 720 - 50 && y <= 720 - 12) hoveredBtn_ = 9001;

    if (activeTab_ == 0) {
        if (x >= 48 && x <= 258 && y >= 156 && y <= 208) hoveredBtn_ = 1001;
        else if (x >= 48 && x <= 218 && y >= 562 && y <= 600) hoveredBtn_ = 1002;
        else if (x >= 230 && x <= 400 && y >= 562 && y <= 600) hoveredBtn_ = 1003;
        else if (x >= 412 && x <= 512 && y >= 562 && y <= 600) hoveredBtn_ = 1004;
    } else if (activeTab_ == 1) {
        if (x >= 750 && x <= 820 && y >= 204 && y <= 242) hoveredBtn_ = 2001;
        else if (x >= 48 && x <= 228 && y >= 252 && y <= 288) hoveredBtn_ = 2002;
        else {
            float cardW = (940 - 56 - 40 - 20) / 2.0f;
            for (int m = 0; m < 2; ++m) {
                float mx = 48.0f + m * (cardW + 20.0f);
                if (x >= mx && x <= mx + cardW && y >= 356 && y <= 496) {
                    hoveredBtn_ = 2100 + m;
                    break;
                }
            }
        }
    } else if (activeTab_ == 2) {
        if (x >= 280 && x <= 740 && y >= 186 && y <= 224) hoveredBtn_ = 3001;
    } else if (activeTab_ == 3) {
        if (x >= 48 && x <= 268 && y >= 568 && y <= 604) hoveredBtn_ = 4001;
        else if (x >= 280 && x <= 480 && y >= 568 && y <= 604) hoveredBtn_ = 4002;
    } else if (activeTab_ == 4) {
        if (x >= 48 && x <= 228 && y >= 566 && y <= 602) hoveredBtn_ = 5001;
        else if (x >= 240 && x <= 380 && y >= 566 && y <= 602) hoveredBtn_ = 5002;
    }

    if (hoveredTab_ != oldTab || hoveredBtn_ != oldBtn) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Window Procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWindow::wndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->wndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            render(hdc, rc.right, rc.bottom);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // Double buffered, no erase needed

        case WM_TIMER: {
            if (wp == TIMER_ANIM) {
                onTimerTick(0.016f);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = (int)LOWORD(lp);
            int y = (int)HIWORD(lp);
            onMouseDown(x, y);
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = (int)LOWORD(lp);
            int y = (int)HIWORD(lp);
            onMouseMove(x, y);
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, RGB(248, 250, 252));
            SetBkColor(hdc, RGB(16, 19, 31));
            return (LRESULT)hBrushDarkInput_;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (detectingKey_) {
                uint32_t vk = (uint32_t)wp;
                detectingKey_ = false;

                if (vk != VK_ESCAPE) {
                    std::string name;
                    bool isMod = false;
                    if (vk == VK_CONTROL || vk == VK_LCONTROL) { name = "Left Ctrl"; vk = VK_LCONTROL; isMod = true; }
                    else if (vk == VK_RCONTROL) { name = "Right Ctrl"; isMod = true; }
                    else if (vk == VK_MENU || vk == VK_LMENU) { name = "Left Alt"; vk = VK_LMENU; isMod = true; }
                    else if (vk == VK_RMENU) { name = "Right Alt"; isMod = true; }
                    else if (vk == VK_SHIFT || vk == VK_LSHIFT) { name = "Left Shift"; vk = VK_LSHIFT; isMod = true; }
                    else if (vk == VK_RSHIFT) { name = "Right Shift"; isMod = true; }
                    else if (vk == VK_CAPITAL) { name = "Caps Lock"; }
                    else if (vk == VK_SPACE) { name = "Space"; }
                    else if (vk == VK_TAB) { name = "Tab"; }
                    else if (vk == VK_OEM_3) { name = "Tilde (~)"; }
                    else if (vk >= VK_F1 && vk <= VK_F24) { name = "F" + std::to_string(vk - VK_F1 + 1); }
                    else {
                        name = "VK 0x" + std::to_string(vk);
                    }

                    config_.hotkeyDisplay = name;
                    config_.hotkeyVk = vk;
                    config_.hotkeyIsMod = isMod;
                    if (onHotkeyChanged) onHotkeyChanged(vk, name, isMod);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_APP_UPDATE_STATE: {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_APP_SET_TRANSCRIPT: {
            auto* p = reinterpret_cast<AsyncTranscriptMsg*>(wp);
            if (p) {
                std::wstring w = toWide(p->text);
                SetWindowTextW(edtTranscript_, w.c_str());

                if (p->appendHistory && !p->text.empty()) {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    wchar_t timeBuf[32]{};
                    swprintf_s(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

                    HistoryItem hi;
                    hi.timestamp = toNarrow(timeBuf);
                    hi.duration = p->duration;
                    hi.text = p->text;
                    history_.insert(history_.begin(), hi);
                }
                delete p;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_APP_API_TEST_RESULT: {
            auto* p = reinterpret_cast<AsyncApiTestMsg*>(wp);
            if (p) {
                apiTestOk_ = p->ok;
                apiLatencyMs_ = p->latencyMs;
                if (p->ok) {
                    apiTestResult_ = "✓ Connected in " + std::to_string(p->latencyMs) + "ms! Model ready.";
                } else {
                    apiTestResult_ = "✗ Error " + std::to_string(p->httpCode) + ": " + p->message;
                }
                delete p;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CLOSE: {
            hide();
            return 0;
        }

        case WM_DESTROY: {
            if (onQuit) onQuit();
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace vox::ui
