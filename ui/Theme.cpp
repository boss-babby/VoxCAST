#include "Theme.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace vox::theme {

Theme& Theme::get() {
    static Theme t;
    return t;
}

void Theme::setMode(Mode m) {
    mode_ = m;
    if (m == Mode::Auto) {
#if defined(_WIN32)
        // AppsUseLightTheme == 0 means the shell is in dark mode.
        DWORD light = 1, cb = sizeof(light);
        if (RegGetValueW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &cb) != ERROR_SUCCESS)
            light = 0;
        palette_ = light ? &kLight : &kDark;
#else
        palette_ = &kDark;   // macOS resolves via NSApp.effectiveAppearance
#endif
        return;
    }
    palette_ = (m == Mode::Light) ? &kLight : &kDark;
}

} // namespace vox::theme
