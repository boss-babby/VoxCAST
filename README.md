# VoxCast

Real-time voice dictation with AI-enhanced transcription. Press a key anywhere on
your OS, speak, and clean text appears at your caret.

---

## Architecture

```
                      ┌──────────────────────────────────────────┐
   Global hotkey ───▶ │            AppController                 │
   (CGEventTap /      │   owns the thread topology, nothing else │
    WH_KEYBOARD_LL)   │   in the app spawns threads              │
                      └───┬───────────┬───────────────┬──────────┘
                          │           │               │
        ┌─────────────────▼──┐   ┌────▼──────────┐  ┌─▼──────────────────┐
        │  AUDIO RT THREAD   │   │  UI THREAD    │  │  NET THREAD POOL   │
        │  CoreAudio/WASAPI  │   │  vsync-driven │  │  libcurl multi     │
        │  16k mono float    │   │  60–120 fps   │  │  async + cancel    │
        └───┬────────┬───────┘   └────┬──────────┘  └─┬──────────────────┘
            │        │                │               │
   SpscRingBuffer   Vad          PopupView       GeminiProvider
   (lock-free)   (energy+ZCR)   Skia/Metal|D3D   ├─ stage 1 transcribe
            │                        │           └─ stage 2 enhance
            └──────── int16 PCM ─────┴───────────────▶  │
                                                        ▼
                              PipelineResult ──▶ InputInjector (SendInput/CGEventPost)
                                              ├─▶ Clipboard (always, as fallback)
                                              └─▶ SQLite history + FTS5 search
```

Layering rule: **nothing above `platform/` may include an OS header.** All OS
behaviour is reached through the eight interfaces in `platform/Platform.h`.

```
native/
├── app/          AppController, Settings, main
├── ai/           ITranscriptionProvider, GeminiProvider, PromptBuilder, VoiceCommands
├── audio/        RingBuffer (SPSC lock-free), Vad, AudioAnalysis (RMS/FFT)
├── db/           HistoryStore (SQLite + FTS5), Dictionary
├── net/          HttpClient (libcurl multi), SseParser
├── platform/     Platform.h  +  mac/*.mm  +  win/*.cpp
├── ui/           Theme, Animation, SkiaRenderer, PopupView, Dashboard
├── assets/       fonts (Inter, JetBrains Mono), icons, Info.plist
└── tests/        Catch2: vad, spring, json parsing, voice commands
```

---

## Build

### macOS (14+)

```bash
brew install cmake ninja curl
# Skia, once (~20 min):
git clone https://skia.googlesource.com/skia.git ~/skia && cd ~/skia
python3 tools/git-sync-deps
bin/gn gen out/Release --args='is_official_build=true skia_use_metal=true skia_use_system_freetype2=false'
ninja -C out/Release skia

cd native
cmake -B build -G Ninja -DSKIA_ROOT=$HOME/skia -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/VoxCast.app/Contents/MacOS/VoxCast
```

Grant **Microphone**, **Accessibility** and **Input Monitoring** in
System Settings → Privacy & Security on first launch.

### Windows (10 1903+ / 11)

```powershell
vcpkg install curl:x64-windows
# Skia with D3D backend:
bin\gn gen out\Release --args="is_official_build=true skia_use_direct3d=true"
ninja -C out\Release skia

cd native
cmake -B build -DSKIA_ROOT=C:/skia -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

`-DSKIA_ROOT` may be omitted; the build then falls back to the software
reference canvas so the app still runs end-to-end (no GPU blur/bloom).

### Tests

```bash
cmake --build build --target voxcast_tests && ctest --test-dir build --output-on-failure
```

---

## Configuration

`~/Library/Application Support/VoxCast/config.json` (macOS) ·
`%APPDATA%\VoxCast\config.json` (Windows)

```jsonc
{
  "hotkey":   { "display": "Right Ctrl", "mode": "both", "modifierOnly": true },
  "popup":    { "anchor": "bottomCenter", "marginPx": 96, "snapToEdges": true },
  "audio":    { "deviceId": "default", "sampleRate": 16000, "vadSilenceStopMs": 900 },
  "provider": {
    "endpointBase":    "https://generativelanguage.googleapis.com/v1beta",
    "transcribeModel": "gemini-transcribe-latest",
    "enhanceModel":    "gemini-flash-latest",
    "streamPartials":  true,
    "temperature":     0.15
  },
  "enhancement": {
    "mode": "auto",
    "fixDisfluencies": true, "autoPunctuate": true,
    "inferStructure": true,  "applyVoiceCommands": true,
    "useCustomDictionary": true, "preserveProfanity": true
  },
  "theme": "auto"
}
```

**The API key is never written to this file.** It lives in the macOS Keychain
(`com.voxcast.app / gemini_api_key`) or the Windows Credential Manager, reached
through `ISecretStore`.

---

## Demo script (60 seconds)

1. Launch VoxCast — a violet dot appears in the menu bar, breathing slowly.
2. Focus a Slack message box. **Hold Right Ctrl.** The capsule springs up from
   the bottom of the screen in 220 ms; 18 gradient bars start tracking your voice.
3. Say: *"hey team um the deploy is green comma ship it period"*
   Ghost captions stream in under the waveform as partials arrive.
4. Release. Bars morph into a travelling "thinking" pulse (~700 ms).
5. Chat mode is auto-selected from the focused bundle id. Text is typed at your
   caret: **"hey team, the deploy is green, ship it"** — filler removed,
   punctuation applied, casual register preserved. Checkmark draws on, capsule
   fades out after 900 ms.
6. Focus VS Code, hold the key again, say *"const user id equals await get user
   open paren id close paren semicolon"* → `const userId = await getUser(id);`
7. Open the Dashboard from the tray → both transcriptions are in History with
   the source app, mode, word count and round-trip latency.

---

## Performance budget

| Metric | Target | Where enforced |
|---|---|---|
| Hotkey → popup visible + capturing | < 100 ms | window pre-created & hidden, never constructed on the hot path |
| End of speech → text injected | < 1.2 s (short utterance) | streaming partials + `gemini-flash-latest` stage 2 |
| Idle CPU (breathing animation) | < 3 % | GPU-only draw, no per-frame allocation, `rand()` banned on the render thread |
| Audio thread allocations | 0 | `SpscRingBuffer`, pre-sized scratch vectors |
| History memory | O(page) | SQLite + virtualised list |
