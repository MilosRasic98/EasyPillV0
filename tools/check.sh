#!/usr/bin/env bash
# Build everything and run the host tests: everything that can be checked
# without a board on the desk.
#
#   tools/check.sh            all four firmware targets + core unit tests
#   tools/check.sh core       just the host unit tests (fast, no toolchain)
#   tools/check.sh parity     just the firmware-vs-simulator scheduler diff
#   tools/check.sh c3         just the two C3 targets
#   tools/check.sh web        regenerate the embedded web assets and report sizes
#
# PlatformIO is not on PATH in a plain shell when it was installed by the VS Code
# extension, so find it rather than asking the user to fix their PATH.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware"

if command -v pio >/dev/null 2>&1; then
  PIO=pio
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  echo "error: cannot find pio. Install PlatformIO Core, or open this project once"
  echo "       in VS Code with the PlatformIO extension so it bootstraps itself."
  exit 127
fi

TARGET="${1:-all}"
FAILED=()
PASSED=()

run_step() {
  local name="$1"; shift
  printf '\n\033[1m=== %s ===\033[0m\n' "$name"
  if "$@"; then
    PASSED+=("$name")
  else
    FAILED+=("$name")
  fi
}

build_env() {
  ( cd "$FW" && "$PIO" run -e "$1" ) 2>&1 | grep -vE 'NotOpenSSLWarning|warnings\.warn'
  # grep eats the exit status, so ask PlatformIO again rather than guessing.
  return "${PIPESTATUS[0]}"
}

core_tests() {
  bash "$ROOT/tests/run_core_tests.sh"
}

parity() {
  bash "$ROOT/tests/parity/run_parity.sh"
}

web_assets() {
  python3 "$FW/tools/gen_web_assets.py" || return 1
  printf '\ngzipped payload the ESP32 has to serve:\n'
  local total=0
  for f in index.html app.css app.js favicon.svg; do
    if [ -f "$ROOT/web/$f" ]; then
      local n
      n=$(gzip -9 -c "$ROOT/web/$f" | wc -c | tr -d ' ')
      total=$((total + n))
      printf '  %-14s %7s bytes gz\n' "$f" "$n"
    fi
  done
  printf '  %-14s %7s bytes gz\n' TOTAL "$total"
}

case "$TARGET" in
  core) run_step "core unit tests" core_tests ;;
  parity) run_step "firmware/simulator parity" parity ;;
  web)  run_step "web assets" web_assets ;;
  c3)
    run_step "web assets" web_assets
    run_step "bringup-c3" build_env bringup-c3
    run_step "app-c3"     build_env app-c3
    ;;
  s3)
    run_step "web assets" web_assets
    run_step "bringup-s3" build_env bringup-s3
    run_step "app-s3"     build_env app-s3
    ;;
  all)
    run_step "core unit tests" core_tests
    run_step "firmware/simulator parity" parity
    run_step "web assets" web_assets
    run_step "bringup-c3" build_env bringup-c3
    run_step "bringup-s3" build_env bringup-s3
    run_step "app-c3"     build_env app-c3
    run_step "app-s3"     build_env app-s3
    ;;
  *)
    echo "usage: tools/check.sh [all|core|parity|web|c3|s3]"
    exit 2
    ;;
esac

printf '\n\033[1m--- summary ---\033[0m\n'
for p in "${PASSED[@]:-}"; do [ -n "$p" ] && printf '  \033[32mok\033[0m    %s\n' "$p"; done
for f in "${FAILED[@]:-}"; do [ -n "$f" ] && printf '  \033[31mFAIL\033[0m  %s\n' "$f"; done

[ "${#FAILED[@]}" -eq 0 ] || exit 1
echo "  everything passed"
