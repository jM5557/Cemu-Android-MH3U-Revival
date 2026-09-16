#!/usr/bin/env bash
#
# Build the Cemu Android port (with the MH3U + custom texture patches) on Arch Linux.
#
#   ./build-android-arch.sh              # debug APK
#   ./build-android-arch.sh release      # release APK (debug-signed unless you set a keystore)
#   ./build-android-arch.sh clean        # wipe build outputs, keep the SDK and vcpkg cache
#
# Run from the root of your Cemu checkout, or set CEMU_SRC to point at it.
#
# The script is idempotent: it detects what is already installed and only does the missing
# parts. Re-running after a failure is safe.
#
# Disk: budget ~30 GB. The NDK is ~5 GB, vcpkg builds boost and friends from source into
# dependencies/vcpkg (~10 GB with buildtrees), and Gradle caches another few GB in ~/.gradle.
# The first build takes 30-90 minutes depending on your machine; later ones are minutes.

set -euo pipefail

CEMU_SRC="${CEMU_SRC:-$(pwd)}"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
BUILD_VARIANT="${1:-debug}"

RED=$'\e[31m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
info()  { printf '%s==>%s %s\n' "$GREEN$BOLD" "$RESET$BOLD" "$*$RESET"; }
warn()  { printf '%s==>%s %s\n' "$YELLOW$BOLD" "$RESET$BOLD" "$*$RESET"; }
die()   { printf '%s==> error:%s %s\n' "$RED$BOLD" "$RESET$BOLD" "$*$RESET" >&2; exit 1; }

# ---------------------------------------------------------------------------------------------
# 0. sanity checks
# ---------------------------------------------------------------------------------------------

command -v pacman >/dev/null || die "this script is for Arch Linux (no pacman found)"

[[ -f "$CEMU_SRC/CMakeLists.txt" && -d "$CEMU_SRC/src/android" ]] \
  || die "$CEMU_SRC does not look like a Cemu checkout (set CEMU_SRC=/path/to/Cemu)"

GRADLE_DIR="$CEMU_SRC/src/android"
GRADLE_CONFIG="$GRADLE_DIR/app/build.gradle.kts"
[[ -f "$GRADLE_CONFIG" ]] || die "missing $GRADLE_CONFIG"

if [[ "$BUILD_VARIANT" == "clean" ]]; then
  info "Cleaning build outputs"
  rm -rf "$CEMU_SRC/src/android/app/build" "$CEMU_SRC/src/android/.gradle" "$CEMU_SRC/build"
  info "Done. vcpkg cache and SDK left in place."
  exit 0
fi

case "$BUILD_VARIANT" in
  debug|release) ;;
  *) die "unknown variant '$BUILD_VARIANT' (expected: debug, release, clean)" ;;
esac

# ---------------------------------------------------------------------------------------------
# 1. version pins, read from the project rather than hardcoded
#
# These change when you rebase onto a newer android-port, so parse them out instead of
# guessing. A stale hardcoded NDK version produces a confusing Gradle error.
# ---------------------------------------------------------------------------------------------

parse_gradle() { sed -nE "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"?([^\"]+)\"?.*/\1/p" "$GRADLE_CONFIG" | head -1; }

NDK_VERSION="$(parse_gradle ndkVersion)"
COMPILE_SDK="$(parse_gradle compileSdk)"
[[ -n "$NDK_VERSION" ]]  || die "could not read ndkVersion from $GRADLE_CONFIG"
[[ -n "$COMPILE_SDK" ]]  || die "could not read compileSdk from $GRADLE_CONFIG"

info "Project wants: NDK $NDK_VERSION, compileSdk $COMPILE_SDK"

# ---------------------------------------------------------------------------------------------
# 2. system packages
#
# Notes on the less obvious ones:
#   jdk21-openjdk  Gradle 9 / AGP need JDK 21. Arch's default jdk-openjdk tracks the newest
#                  release, which AGP rejects. We pin 21 via JAVA_HOME below and deliberately
#                  do NOT run archlinux-java set, so your system default is untouched.
#   gettext        msgunfmt, used to turn bin/resources/*/cemu.mo into the .po files the
#                  kotlinx-gettext Gradle plugin consumes. Without it the app builds but ships
#                  with English only.
#   ninja/cmake    vcpkg and the NDK toolchain both want them.
#   unzip/zip/curl vcpkg bootstrap and sdkmanager.
#   base-devel     several vcpkg ports (boost, libzip) need autotools and a host compiler.
# ---------------------------------------------------------------------------------------------

PACKAGES=(base-devel git cmake ninja pkgconf curl zip unzip tar gettext jdk21-openjdk python perl)

MISSING=()
for pkg in "${PACKAGES[@]}"; do
  pacman -Qq "$pkg" &>/dev/null || MISSING+=("$pkg")
done

if (( ${#MISSING[@]} )); then
  info "Installing: ${MISSING[*]}"
  sudo pacman -S --needed --noconfirm "${MISSING[@]}"
else
  info "System packages already present"
fi

JAVA_HOME="/usr/lib/jvm/java-21-openjdk"
[[ -x "$JAVA_HOME/bin/java" ]] || die "JDK 21 not found at $JAVA_HOME after install"
export JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"
info "Using JDK: $("$JAVA_HOME/bin/java" -version 2>&1 | head -1)"

# ---------------------------------------------------------------------------------------------
# 3. Android SDK
#
# Installed from Google's command-line tools zip rather than the AUR: no AUR helper assumed,
# no mismatch with whatever Android Studio may already have, and sdkmanager handles licences.
# If you already have Android Studio, set ANDROID_SDK_ROOT to its SDK and this step is skipped.
# ---------------------------------------------------------------------------------------------

export ANDROID_SDK_ROOT
export ANDROID_HOME="$ANDROID_SDK_ROOT"   # some tooling still reads the old name
SDKMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"

if [[ ! -x "$SDKMANAGER" ]]; then
  info "Bootstrapping Android command-line tools into $ANDROID_SDK_ROOT"
  CMDLINE_TOOLS_URL="https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"
  TMP_ZIP="$(mktemp -d)/cmdline-tools.zip"
  curl -fL --progress-bar -o "$TMP_ZIP" "$CMDLINE_TOOLS_URL" \
    || die "download failed. Grab the Linux command-line tools from
         https://developer.android.com/studio#command-line-tools-only
         and unzip so that $SDKMANAGER exists, then re-run."
  mkdir -p "$ANDROID_SDK_ROOT/cmdline-tools"
  unzip -q "$TMP_ZIP" -d "$ANDROID_SDK_ROOT/cmdline-tools"
  # The zip contains a top-level 'cmdline-tools' dir; sdkmanager insists on being at
  # cmdline-tools/latest/ or it cannot locate its own package.
  mv "$ANDROID_SDK_ROOT/cmdline-tools/cmdline-tools" "$ANDROID_SDK_ROOT/cmdline-tools/latest"
  rm -rf "$(dirname "$TMP_ZIP")"
  [[ -x "$SDKMANAGER" ]] || die "sdkmanager still not at $SDKMANAGER after unpacking"
else
  info "Android SDK found at $ANDROID_SDK_ROOT"
fi

info "Accepting SDK licences"
yes 2>/dev/null | "$SDKMANAGER" --licenses >/dev/null || true

# Pick the newest CMake the SDK offers. AGP prefers an SDK-managed CMake over the one on PATH,
# and silently falling back can produce a version mismatch against the "3.25.0+" constraint.
SDK_CMAKE="$("$SDKMANAGER" --list 2>/dev/null \
  | grep -oE '^\s+cmake;[0-9.]+' | tr -d ' ' | sort -V | tail -1 || true)"

SDK_PACKAGES=(
  "platform-tools"
  "platforms;android-${COMPILE_SDK}"
  "build-tools;${COMPILE_SDK}.0.0"
  "ndk;${NDK_VERSION}"
)
[[ -n "$SDK_CMAKE" ]] && SDK_PACKAGES+=("$SDK_CMAKE")

info "Installing SDK components: ${SDK_PACKAGES[*]}"
"$SDKMANAGER" --install "${SDK_PACKAGES[@]}" \
  || die "sdkmanager failed. If it complains about build-tools;${COMPILE_SDK}.0.0, run
         '$SDKMANAGER --list | grep build-tools' and install the closest available version."

NDK_PATH="$ANDROID_SDK_ROOT/ndk/$NDK_VERSION"
[[ -d "$NDK_PATH" ]] || die "NDK $NDK_VERSION missing at $NDK_PATH"
export ANDROID_NDK_HOME="$NDK_PATH"
export ANDROID_NDK_ROOT="$NDK_PATH"

# ---------------------------------------------------------------------------------------------
# 4. submodules and vcpkg
#
# vcpkg is a submodule at dependencies/vcpkg and builds every native dependency from source
# for arm64-v8a. VCPKG_ROOT must point at it or the CMake toolchain picks up a system vcpkg
# with a different baseline and the build fails in confusing ways.
# ---------------------------------------------------------------------------------------------

cd "$CEMU_SRC"

if [[ ! -f dependencies/vcpkg/bootstrap-vcpkg.sh ]]; then
  info "Fetching submodules (this pulls vcpkg, imgui, ZArchive, cubeb, ...)"
  git submodule update --init --recursive
else
  info "Submodules present; syncing"
  git submodule update --init --recursive --depth 1 2>/dev/null || \
    git submodule update --init --recursive
fi

export VCPKG_ROOT="$CEMU_SRC/dependencies/vcpkg"
export VCPKG_FORCE_DOWNLOADED_BINARIES=true

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  info "Bootstrapping vcpkg"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

# ---------------------------------------------------------------------------------------------
# 5. translations
#
# Mirrors the .po generation from the project's own release workflow. Optional: skipped
# cleanly if the catalogs are missing, and the app just ships English-only.
# ---------------------------------------------------------------------------------------------

if compgen -G "bin/resources/*/cemu.mo" >/dev/null; then
  info "Generating translation catalogs"
  for mo_file in bin/resources/*/cemu.mo; do
    language_code="$(basename "$(dirname "$mo_file")")"
    po_file="src/android/app/src/main/assets/translations/$language_code/cemu.po"
    mkdir -p "$(dirname "$po_file")"
    msgunfmt "$mo_file" -o "$po_file" 2>/dev/null \
      || warn "  could not convert $mo_file, skipping $language_code"
  done
else
  warn "No bin/resources/*/cemu.mo found; building English-only"
fi

# ---------------------------------------------------------------------------------------------
# 6. build
# ---------------------------------------------------------------------------------------------

cd "$GRADLE_DIR"
chmod +x ./gradlew

if [[ "$BUILD_VARIANT" == "release" ]]; then
  GRADLE_TASK="assembleRelease"
  if [[ -n "${ANDROID_STORE_FILE:-}" ]]; then
    info "Release build, signing with $ANDROID_STORE_FILE"
    [[ -n "${ANDROID_KEY_STORE_PASSWORD:-}" ]] || die "ANDROID_STORE_FILE set but ANDROID_KEY_STORE_PASSWORD is not"
    [[ -n "${ANDROID_KEY_ALIAS:-}" ]]          || die "ANDROID_STORE_FILE set but ANDROID_KEY_ALIAS is not"
  else
    warn "Release build with no ANDROID_STORE_FILE: the APK will be debug-signed."
    warn "That installs fine for personal use but cannot be distributed or upgraded in place."
  fi
else
  GRADLE_TASK="assembleDebug"
fi

info "Running ./gradlew $GRADLE_TASK"
warn "First run compiles all of vcpkg. Expect 30-90 minutes and a lot of output."

# --no-daemon keeps a failed build from leaving a 2 GB JVM resident. Drop it for faster
# iteration once things are building reliably.
./gradlew "$GRADLE_TASK" --no-daemon

# ---------------------------------------------------------------------------------------------
# 7. report
# ---------------------------------------------------------------------------------------------

APK="$(find "$GRADLE_DIR/app/build/outputs/apk" -name '*.apk' -newermt '-2 hours' 2>/dev/null | sort | tail -1 || true)"

if [[ -n "$APK" ]]; then
  info "Built: $APK"
  printf '     %s\n' "$(du -h "$APK" | cut -f1)"
  echo
  echo "Install to a connected device with:"
  echo "  $ANDROID_SDK_ROOT/platform-tools/adb install -r \"$APK\""
  echo
  echo "Watch the emulator's log with:"
  echo "  $ANDROID_SDK_ROOT/platform-tools/adb logcat -s Cemu"
else
  warn "Build reported success but no APK found under app/build/outputs/apk"
  exit 1
fi
