#!/bin/sh
# Bounds-check elimination must never remove a check that is actually
# needed. Each program here indexes out of range in a way the range
# analysis cannot rule out; every one must still trap at runtime.
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/safety
fail=0
for f in tests/safety/*.simp; do
    base=$(basename "$f" .simp)
    if ! ./simplec "$f" -o "build/safety/$base" >/dev/null 2>&1; then
        echo "FAIL $base (compile)"
        fail=1
        continue
    fi
    out=$("./build/safety/$base" 2>&1 | tail -1)
    code=$?
    case "$out" in
        *"out of bounds"*) echo "TRAPS $base" ;;
        *) echo "FAIL  $base — expected a bounds trap, got: $out (exit $code)"
           fail=1 ;;
    esac
done
exit $fail
