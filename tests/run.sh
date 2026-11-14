#!/bin/sh
# Test harness. usage: tests/run.sh path/to/mcc
MCC=$1
OUT=build/test
mkdir -p "$OUT"
pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $1"; }

# expect_exit name want command...
expect_exit() {
    name=$1; want=$2; shift 2
    "$@" >/dev/null 2>&1
    got=$?
    if [ "$got" = "$want" ]; then ok; else bad "$name (exit $got, want $want)"; fi
}

# --- Mach-O executable writer ---
"$MCC" --test-exe "$OUT/exe42" && expect_exit exe42 42 "$OUT/exe42"
"$MCC" --test-exe "$OUT/exe42d" data && expect_exit exe42-data 42 "$OUT/exe42d"
expect_exit exe42-codesign 0 codesign -v "$OUT/exe42"
expect_exit exe42d-codesign 0 codesign -v "$OUT/exe42d"

# --- Linker, fed with objects from the host compiler ---
if [ -d tests/ld ]; then
    for f in a b lib1 lib2 lib3; do
        cc -c -O1 -fcommon -o "$OUT/ld_$f.o" "tests/ld/$f.c" || bad "ld (host cc $f)"
    done
    rm -f "$OUT/ld_lib.a"
    ar rcs "$OUT/ld_lib.a" "$OUT/ld_lib1.o" "$OUT/ld_lib2.o" "$OUT/ld_lib3.o" 2>/dev/null
    "$MCC" ld -o "$OUT/ld1" "$OUT/ld_a.o" "$OUT/ld_b.o" "$OUT/ld_lib.a" && expect_exit ld-objects 42 "$OUT/ld1"
    # round trip: rewrite each object with our writer, link the copies
    for f in a b; do "$MCC" ld --rewrite -o "$OUT/ld_$f.rw.o" "$OUT/ld_$f.o"; done
    "$MCC" ld -o "$OUT/ld2" "$OUT/ld_a.rw.o" "$OUT/ld_b.rw.o" "$OUT/ld_lib.a" && expect_exit ld-rewritten 42 "$OUT/ld2"
    expect_exit ld-rewritten-nm 0 nm "$OUT/ld_a.rw.o"
    # our objects must also be acceptable to the system linker
    expect_exit ld-host-accepts 0 cc -o "$OUT/ld3" "$OUT/ld_a.rw.o" "$OUT/ld_b.rw.o" "$OUT/ld_lib1.o" "$OUT/ld_lib2.o"
    expect_exit ld-host-linked 42 "$OUT/ld3"
    expect_exit ld-undefined 1 "$MCC" ld -o "$OUT/ld4" "$OUT/ld_a.o"
fi

# --- Assembler: encodings must match the host assembler ---
for src in tests/as/*.s; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .s)
    "$MCC" as -o "$OUT/as_$name.o" "$src" && cc -c -o "$OUT/as_$name.ref.o" "$src"
    otool -t "$OUT/as_$name.o" | tail -n +2 >"$OUT/as_$name.txt"
    otool -t "$OUT/as_$name.ref.o" | tail -n +2 >"$OUT/as_$name.ref.txt"
    if cmp -s "$OUT/as_$name.txt" "$OUT/as_$name.ref.txt"; then ok; else bad "as-$name"; fi
done

# --- Preprocessor: tests/pp/NAME.c with NAME.expect (verified against the host cpp) ---
for src in tests/pp/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    "$MCC" -E "$src" >"$OUT/pp_$name.out" 2>&1
    if cmp -s "$OUT/pp_$name.out" "tests/pp/$name.expect"; then ok; else bad "pp-$name"; fi
done

# --- C execute tests: tests/c/NAME.c with NAME.expect (stdout, then "exit N") ---
for src in tests/c/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    if ! "$MCC" -o "$OUT/$name" "$src" 2>"$OUT/$name.err"; then
        bad "$name (compile)"; continue
    fi
    { "$OUT/$name"; echo "exit $?"; } >"$OUT/$name.out" 2>/dev/null
    if cmp -s "$OUT/$name.out" "tests/c/$name.expect"; then ok; else bad "$name (output)"; fi
done

# --- Link-time optimisation: every C test again, then several files at once ---
for src in tests/c/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    if ! "$MCC" -flto -o "$OUT/${name}_lto" "$src" 2>"$OUT/$name.err"; then
        bad "$name (lto compile)"; continue
    fi
    { "$OUT/${name}_lto"; echo "exit $?"; } >"$OUT/${name}_lto.out" 2>/dev/null
    if cmp -s "$OUT/${name}_lto.out" "tests/c/$name.expect"; then ok; else bad "$name (lto output)"; fi
done
# --- Every C test again, optimised for size ---
for src in tests/c/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    if ! "$MCC" -Os -o "$OUT/${name}_os" "$src" 2>"$OUT/$name.err"; then
        bad "$name (-Os compile)"; continue
    fi
    { "$OUT/${name}_os"; echo "exit $?"; } >"$OUT/${name}_os.out" 2>/dev/null
    if cmp -s "$OUT/${name}_os.out" "tests/c/$name.expect"; then ok; else bad "$name (-Os output)"; fi
done
# --- Every C test again at -O3 (vectorisation, more optimisation rounds) ---
for src in tests/c/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    for flags in "-O3" "-O3 -flto" "-O3 -ffast-math"; do
        if ! "$MCC" $flags -o "$OUT/${name}_o3" "$src" 2>"$OUT/$name.err"; then
            bad "$name ($flags compile)"; continue
        fi
        { "$OUT/${name}_o3"; echo "exit $?"; } >"$OUT/${name}_o3.out" 2>/dev/null
        if cmp -s "$OUT/${name}_o3.out" "tests/c/$name.expect"; then ok; else bad "$name ($flags output)"; fi
    done
done
# --- Every C test again with a profile: instrumented, then rebuilt from the counts ---
for src in tests/c/*.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    rm -f "$OUT/$name.prof"
    if ! "$MCC" -fprofile-generate -o "$OUT/${name}_pg" "$src" 2>"$OUT/$name.err"; then
        bad "$name (profile-generate compile)"; continue
    fi
    { MCC_PROFILE="$OUT/$name.prof" "$OUT/${name}_pg"; echo "exit $?"; } >"$OUT/${name}_pg.out" 2>/dev/null
    if cmp -s "$OUT/${name}_pg.out" "tests/c/$name.expect"; then ok; else bad "$name (instrumented output)"; fi
    [ -f "$OUT/$name.prof" ] || continue # the program did not leave through exit
    for flags in "" "-flto"; do
        if ! "$MCC" $flags "-fprofile-use=$OUT/$name.prof" -o "$OUT/${name}_pu" "$src" 2>"$OUT/$name.err"; then
            bad "$name (profile-use $flags compile)"; continue
        fi
        { "$OUT/${name}_pu"; echo "exit $?"; } >"$OUT/${name}_pu.out" 2>/dev/null
        if cmp -s "$OUT/${name}_pu.out" "tests/c/$name.expect"; then ok; else bad "$name (profile-use $flags output)"; fi
    done
done
if [ -d tests/lto ]; then
    # in one go
    "$MCC" -flto -o "$OUT/lto1" tests/lto/a.c tests/lto/b.c tests/lto/c.c && "$OUT/lto1" >"$OUT/lto1.out"
    if cmp -s "$OUT/lto1.out" tests/lto/expect; then ok; else bad "lto (one command)"; fi
    # through object files and an archive, mixed with an ordinary object
    for f in a c; do "$MCC" -flto -c -o "$OUT/lto_$f.o" "tests/lto/$f.c"; done
    "$MCC" -c -o "$OUT/lto_b.o" tests/lto/b.c
    rm -f "$OUT/lto_c.a"
    "$MCC" ar "$OUT/lto_c.a" "$OUT/lto_c.o"
    "$MCC" -o "$OUT/lto2" "$OUT/lto_a.o" "$OUT/lto_b.o" "$OUT/lto_c.a" && "$OUT/lto2" >"$OUT/lto2.out"
    if cmp -s "$OUT/lto2.out" tests/lto/expect; then ok; else bad "lto (objects)"; fi
    # functions nothing refers to are left out
    if nm "$OUT/lto1" | grep -q unused; then bad "lto (dead functions kept)"; else ok; fi
fi

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
