#!/usr/bin/env bash
# Build and run the easypill_core host tests.
#
# easypill_core is deliberately free of Arduino.h, so the whole of the
# scheduling, adherence and settings logic can be compiled and exercised with
# nothing but a host compiler -- which is the only way to test any of this
# without the board.
set -euo pipefail

# Resolve everything from the script's own location so this works from any cwd.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
CORE_DIR="${PROJECT_DIR}/firmware/lib/easypill_core"

CXX="${CXX:-/usr/bin/clang++}"
OUT_DIR="${EASYPILL_TEST_OUT:-/tmp/easypill_core_tests}"
BIN="${OUT_DIR}/core_tests"

mkdir -p "${OUT_DIR}"

SOURCES=("${SCRIPT_DIR}/core_tests.cpp")
while IFS= read -r src; do
  SOURCES+=("${src}")
done < <(find "${CORE_DIR}" -maxdepth 1 -name '*.cpp' | sort)

"${CXX}" -std=c++17 -Wall -Wextra -Wno-unused-parameter -O1 -g \
  -I "${CORE_DIR}" \
  -o "${BIN}" \
  "${SOURCES[@]}"

"${BIN}"
