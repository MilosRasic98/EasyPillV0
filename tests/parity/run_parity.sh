#!/usr/bin/env bash
# Prove the firmware and the simulator answer the same scheduler questions the
# same way.
#
# The simulator is how the box gets demonstrated and tuned with no hardware on
# the desk, so the C++ core and the Python simulator must not drift apart.
# Nothing else in the tree notices when they do. This does.
#
# Both halves print the same report for the same grid of questions. The diff is
# the whole assertion: no expected-output file to update, no assertions to keep
# in step, and a new question only has to be added to both writers.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${EASYPILL_TEST_OUT:-${TMPDIR:-/tmp}}"
BIN="$OUT/easypill_parity"

CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null 2>&1 || { echo "error: no $CXX on PATH"; exit 127; }

"$CXX" -std=c++17 -Wall -Wextra -O1 \
  -I "$ROOT/firmware/lib/easypill_core" \
  -o "$BIN" \
  "$ROOT/tests/parity/parity_driver.cpp" \
  "$ROOT"/firmware/lib/easypill_core/*.cpp || exit 1

"$BIN"                                    > "$OUT/parity_cpp.txt" || exit 1
python3 -I "$ROOT/tests/parity/parity_py.py" > "$OUT/parity_py.txt" || exit 1

lines=$(wc -l < "$OUT/parity_cpp.txt" | tr -d ' ')
if diff -q "$OUT/parity_cpp.txt" "$OUT/parity_py.txt" >/dev/null; then
  echo "$lines answers, firmware and simulator agree"
  exit 0
fi

echo "FIRMWARE AND SIMULATOR DISAGREE on $(diff "$OUT/parity_cpp.txt" "$OUT/parity_py.txt" | grep -c '^<') of $lines answers"
echo "  < firmware (easypill_core, the reference)   > simulator (easypill_sim.py)"
echo
diff "$OUT/parity_cpp.txt" "$OUT/parity_py.txt" | head -40
echo
echo "full reports: $OUT/parity_cpp.txt  $OUT/parity_py.txt"
exit 1
