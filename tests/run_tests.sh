#!/bin/sh
# Compiles and runs every example, comparing output to tests/expected/<name>.txt
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
fail=0
for f in examples/*.simp; do
    base=$(basename "$f" .simp)
    # plasma needs SDL2 linked and opens a window — tools/build_plasma.sh
    case "$base" in plasma|raytracer|raytracer_pure) continue ;; esac
    if ! ./simplec "$f" -o "build/$base" >/dev/null; then
        echo "FAIL $base (compile error)"
        fail=1
        continue
    fi
    # an example with a tests/stdin/<name>.txt file reads it as its stdin
    if [ -f "tests/stdin/$base.txt" ]; then
        "./build/$base" < "tests/stdin/$base.txt" > "build/$base.out"
    else
        "./build/$base" < /dev/null > "build/$base.out"
    fi
    if diff -u "tests/expected/$base.txt" "build/$base.out" >/dev/null 2>&1; then
        echo "PASS $base"
    else
        echo "FAIL $base (output mismatch)"
        diff -u "tests/expected/$base.txt" "build/$base.out" | head -20
        fail=1
    fi
done
exit $fail
