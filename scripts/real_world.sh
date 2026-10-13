#!/usr/bin/env bash
# Runs real programs on top of oguzalloc (LD_PRELOAD) and compares run time with glibc.
# Usage: make preload && ./scripts/real_world.sh
set -euo pipefail
cd "$(dirname "$0")/.."
LIB="$PWD/build/liboguzalloc.so"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== Correctness: do real programs run on oguzalloc?"
LD_PRELOAD="$LIB" ls /usr/bin > /dev/null && echo "  ls      OK"
LD_PRELOAD="$LIB" git --version > /dev/null && echo "  git     OK"
printf '#include <stdio.h>\nint main(void){puts("ok");return 0;}\n' > "$TMP/h.c"
LD_PRELOAD="$LIB" gcc -O2 "$TMP/h.c" -o "$TMP/h" && "$TMP/h" > /dev/null && echo "  gcc     OK"
LD_PRELOAD="$LIB" python3 -c "
import json, threading
d = {str(i): [i] * 10 for i in range(200000)}
assert json.loads(json.dumps(d)) == d
ts = [threading.Thread(target=lambda: [list(range(1000)) for _ in range(500)]) for _ in range(16)]
[t.start() for t in ts]; [t.join() for t in ts]
" && echo "  python  OK (16 threads)"

echo "== Proof of binding: which library does git's malloc resolve to?"
LD_DEBUG=bindings LD_PRELOAD="$LIB" git --version 2>&1 \
    | grep -m1 "binding file git .*normal symbol .malloc'" | sed 's/^ *[0-9]*: */  /' || true

# Generate a large C file: compiling it is allocation-heavy for gcc.
python3 - > "$TMP/big.c" <<'EOF'
print('#include <string.h>')
for i in range(3000):
    print(f'struct s{i} {{ int a; char b[{i % 64 + 1}]; double c; }};')
    print(f'int f{i}(int x) {{ struct s{i} v; memset(&v, 0, sizeof v); v.a = x * {i};'
          f' for (int k = 0; k < x; k++) v.a += k ^ {i}; return v.a + (int)v.c; }}')
EOF
cat > "$TMP/py.py" <<'EOF'
import json
for _ in range(3):
    d = {str(i): {"a": [i, i + 1, "x" * (i % 50)], "b": str(i) * 3} for i in range(150000)}
    d2 = json.loads(json.dumps(d))
    l = sorted(d2.items(), key=lambda kv: kv[1]["b"])
EOF

measure() {   # $1 = label, rest = command. glibc and oguzalloc runs alternate.
    local label="$1"; shift
    local g=() m=()
    for _ in 1 2 3 4 5; do
        local s
        s=$(date +%s%N); "$@" > /dev/null; g+=("$(( $(date +%s%N) - s ))")
        s=$(date +%s%N); LD_PRELOAD="$LIB" "$@" > /dev/null; m+=("$(( $(date +%s%N) - s ))")
    done
    python3 -c "
import statistics as st, sys
g = [int(x) / 1e9 for x in '${g[*]}'.split()]; m = [int(x) / 1e9 for x in '${m[*]}'.split()]
print(f'  {sys.argv[1]:<22} glibc {st.median(g):6.2f}s   oguzalloc {st.median(m):6.2f}s   diff {(st.median(m)/st.median(g)-1)*100:+.1f}%')
" "$label"
}

echo "== Run time (median of 5 runs, negative = oguzalloc is faster)"
measure "gcc -O2 (3000 funcs)" gcc -O2 -c "$TMP/big.c" -o "$TMP/big.o"
PYTHONHASHSEED=0 measure "python json+sort" python3 "$TMP/py.py"
