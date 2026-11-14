cd "$W"
printf 'alice 30 x\nbob 25 y\ncarol 35 x\n' > p
awk '{ print $2, $1 }' p
awk '$2 > 28 { n++; s += $2 } END { print n, s, s/n }' p
awk 'BEGIN { x = "a" "b"; print x, length(x), substr("hello", 2, 3), index("hello", "ll"), toupper("abc") }'
awk 'BEGIN { n = split("a:b:c", arr, ":"); for (i = 1; i <= n; i++) printf "%s-", arr[i]; print "" }'
awk 'BEGIN { s = "foo bar foo"; gsub(/foo/, "[&]", s); print s; sub(/bar/, "baz", s); print s }'
awk '/^b/ { print NR ": " $0 }' p
awk '{ a[$3]++ } END { for (k in a) print k, a[k] | "sort" }' p
awk 'BEGIN { printf "%5d|%-5s|%05.2f|%x|%c|%s\n", 42, "ab", 3.14159, 255, 65, "str" }'
awk 'BEGIN { if (1 > 2) print "no"; else print "yes"; i = 0; while (i < 3) i++; print i; do i--; while (i > 0); print i }'
awk 'function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) } BEGIN { print fact(10) }'
awk 'function f(a, i) { a["x"] = 1; i = 5 } BEGIN { f(arr); print arr["x"], ("x" in arr), ("y" in arr), i == "" }'
awk 'BEGIN { print 1/4, 2^10, 7 % 3, -3 + 1, 1e6, 0.1 + 0.2, 100000 * 100000, int(3.9), "3x" + 1 }'
awk 'BEGIN { print length("héllo") > "/dev/null"; print "to stderr" > "/dev/stderr" }' 2>&1
awk '{ $2 = "X"; print; print NF }' p | head -2
awk 'NR == 2, NR == 3 { print "r", $1 }' p
awk 'BEGIN { while ((getline line < "p") > 0) n++; print n; close("p") }'
awk 'BEGIN { "echo hi" | getline x; print x }'
awk -v v=7 'BEGIN { print v + 1; print substr("abc", 0), substr("abc", 2); print match("foobar", /o+/), RSTART, RLENGTH }'
awk 'BEGIN { a["k"] = 1; delete a["k"]; print length(a); x = sprintf("%d-%s", 3, "z"); print x; print tolower("ABC") ~ /^abc$/ }'
awk '{ print ($2 >= 30) ? "old" : "young" }' p
awk 'END { print NR, $0 }' p
awk '!seen[$3]++' p
awk 'BEGIN { print "a" > "o1"; print "b" >> "o1"; close("o1"); while ((getline l < "o1") > 0) print "got", l }'
awk 'BEGIN { exit 3 } END { print "no" }'; echo rc=$?
awk '{ exit 4 } END { print "end" }' p; echo rc=$?
awk 'BEGIN { x["a"]=1; x["b"]=2; n=0; for (k in x) n += x[k]; print n; print 1==1.0, "a" < "b", "10" < "9", 10 < 9 }'
awk '$1 ~ /^[a-b]/ && $2 != 25 { print $1 }' p
awk 'BEGIN { FS = "," } { print $2 }' <<E
a,b,c
E
awk '{ sum = 0; for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) sum += $i; print sum }' p
awk 'BEGIN { printf("%s %s\n", "paren", "form"); print substr("hello world", 7) }'
awk -f "$T/gen.awk" "$T/caps"
echo 'a b c' | awk '{ print NF; NF = 2; print; $5 = "e"; print; print NF }'
awk 'BEGIN { s = "a.b.c"; n = gsub(/\./, "/", s); print n, s; print index(s, "/"), length() }'
awk 'BEGIN { printf "%s\n", 017; print 1 " " 2; print 1 + 2 " " 3; x = 5; x += 2; x *= 3; print x--, x, ++x }'
