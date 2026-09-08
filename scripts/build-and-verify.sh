#!/usr/bin/env bash
# ============================================================================
#  VoxCast — cross-compile for Windows x64 and verify under Wine.
#  Reproduces the full CI pipeline on any Debian/Ubuntu box with no Windows
#  machine involved. Verified on Debian 12 with GCC 12 and Wine 8.0.
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${HERE}/build-win"
OUT="${HERE}/artifacts"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine64}"
export WINEARCH=win64
export WINEDEBUG=-all

step() { printf '\n\033[1;35m==> %s\033[0m\n' "$*"; }

# --- 0. toolchain ----------------------------------------------------------
step "Checking toolchain"
for t in cmake x86_64-w64-mingw32-g++-posix wine; do
  command -v "$t" >/dev/null || {
    echo "missing: $t"
    echo "  sudo apt-get install -y cmake ninja-build mingw-w64 wine wine64 xvfb"
    exit 1
  }
  printf '  %-34s %s\n' "$t" "$(command -v $t)"
done
# The -posix thread model is mandatory: win32 has no std::thread.
x86_64-w64-mingw32-g++-posix -v 2>&1 | grep -q 'Thread model: posix' \
  || { echo "ERROR: compiler is not the posix-threads variant"; exit 1; }

# --- 1. configure + build --------------------------------------------------
step "Configuring (mini-Skia software backend; pass SKIA_ROOT for GPU)"
cmake -B "$BUILD" -S "$HERE" \
      -DCMAKE_TOOLCHAIN_FILE="${HERE}/cmake/mingw-w64-x86_64.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      ${SKIA_ROOT:+-DSKIA_ROOT="$SKIA_ROOT"}

step "Building"
cmake --build "$BUILD" -j"$(nproc)"

mkdir -p "$OUT"
cp "$BUILD"/*.exe "$OUT/" 2>/dev/null || true
ls -la "$OUT"/*.exe

# --- 2. virtual display (the overlay needs a compositor target) ------------
if ! xdpyinfo -display :99 >/dev/null 2>&1; then
  step "Starting Xvfb :99"
  Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
  sleep 2
fi
export DISPLAY=:99

# --- 3. unit tests ---------------------------------------------------------
step "Unit tests (VAD · spring/bezier · voice commands · JSON)"
wine "$BUILD/voxcast_tests.exe"

# --- 4. platform self-test -------------------------------------------------
step "Platform self-test (clipboard · DPAPI · focus · audio · renderer · window · hook)"
wine "$BUILD/VoxCast.exe" --selftest

# --- 5. offscreen render of the real PopupView -----------------------------
step "Rendering PopupView stills"
mkdir -p "$OUT/frames"
wine "$BUILD/render_frames.exe" "$(winepath -w "$OUT/frames" 2>/dev/null || echo "$OUT/frames")" \
     --fps 30 --scale 2

# --- 6. live overlay window + screenshot -----------------------------------
step "Live Win32 layered overlay (9s scripted session)"
wine "$BUILD/VoxCast.exe" --overlay-demo 9 &
DEMO=$!
sleep 3; import -window root -screen "$OUT/frames/live-listening.png" 2>/dev/null || true
sleep 2; import -window root -screen "$OUT/frames/live-processing.png" 2>/dev/null || true
wait $DEMO || true

# --- 7. ctest --------------------------------------------------------------
step "ctest (through the Wine crosscompiling emulator)"
( cd "$BUILD" && ctest --output-on-failure )

step "Done — artifacts in $OUT"
