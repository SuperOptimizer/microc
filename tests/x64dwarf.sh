#!/bin/sh
# tests/x64dwarf.sh STAGE: -g debug information read back by gdb and llvm-dwarfdump --verify (x86-64 host).
# Each tests/x64dwarf/*.c marks lines with "/* BPn */"; gdb stops there and prints the variables of .exp's lines.
S=${1:-build/stage0}; D=${X64_DIR:-/tmp/mcc-x64dwarf}; mkdir -p "$D"; pass=0; fail=0
command -v gdb > /dev/null || { echo "x64dwarf: no gdb, skipped"; exit 0; }
for c in tests/x64dwarf/*.c; do
  n=$(basename "$c" .c)
  for o in -O0 -O2; do
    if ! "$S/mcc" -mglibc -g $o "$c" -o "$D/$n$o" 2> "$D/$n$o.err"; then echo "FAIL $n $o: compile"; fail=$((fail + 1)); continue; fi
    if command -v llvm-dwarfdump > /dev/null && ! llvm-dwarfdump --verify "$D/$n$o" > "$D/$n$o.verify" 2>&1; then
      echo "FAIL $n $o: llvm-dwarfdump --verify"; fail=$((fail + 1)); continue
    fi
    : > "$D/$n.gdb"; : > "$D/$n$o.out"
    while read -r bp rest; do
      line=$(grep -n "/\* $bp \*/" "$c" | cut -d: -f1)
      var=${rest%%=*}
      printf 'break %s:%s\ncommands\nsilent\nprintf "%s %s=%%d\\n", %s\ncontinue\nend\n' "$n.c" "$line" "$bp" "$var" "$var" >> "$D/$n.gdb"
    done < "tests/x64dwarf/$n.exp"
    printf 'run\nquit\n' >> "$D/$n.gdb"
    (cd tests/x64dwarf && gdb -q -batch -nx -x "$D/$n.gdb" "$D/$n$o") 2>&1 | grep '^BP' > "$D/$n$o.out"
    if diff "tests/x64dwarf/$n.exp" "$D/$n$o.out" > /dev/null; then pass=$((pass + 1)); else echo "FAIL $n $o: gdb"; diff "tests/x64dwarf/$n.exp" "$D/$n$o.out"; fail=$((fail + 1)); fi
  done
done
echo "$pass passed, $fail failed"
