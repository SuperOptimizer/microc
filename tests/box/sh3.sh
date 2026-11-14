x="a b  c"
set -- $x; echo $#
set -- "$x"; echo $#
set --; echo $# "$@" end
set -- ""; echo $# 
e=""; set -- $e; echo $#; set -- "$e"; echo $#
set -- a b; set -- "$@" c; echo "$*" "$#"
echo "pre${x}post" pre${x}post
y='*'; echo "$y" 
cd /usr; echo $PWD; cd - >/dev/null; 
echo ${1+"$@"}
unset q; echo "${q-unset}" "${q:-null}"; q=; echo "${q-unset}" "${q:-null}" "${q+set}"
export FOO=bar; sh -c 'echo $FOO'
BAR=baz sh -c 'echo $BAR'; echo "[$BAR]"
echo `echo \`echo nested\``
echo "$(echo "quoted $(echo inner)")"
a=x; a=y echo $a
cat < /dev/null; echo rc=$?
echo out 1>&2 2>/dev/null
exec 3>&1; echo fd3 >&3; exec 3>&-
printf '%s\n' a b c | (read x; read y; echo $y)
if false; then :; fi; echo if=$?
case "a b" in "a b") echo m1;; esac
case x in [a-z]) echo m2;; esac
case abc in a*c) echo m3;; esac
case '*' in '*') echo m4;; esac
p='a*'; case abc in $p) echo m5;; esac; case abc in "$p") echo no;; *) echo m6;; esac
echo $(( 010 + 1 )) $((-3 * -2)) $((1 && 0 || 1)) $((5 > 3)) $((~0))
i=5; echo $((i += 2)) $i $((i++ )) 2>/dev/null || echo arith
echo "a
b" | while IFS= read -r line; do echo "<$line>"; done
f() { echo $1; shift; [ $# -gt 0 ] && f "$@"; }; f 1 2 3
type f | head -1 >/dev/null; command -v echo
echo 'single'"double"plain\ space
echo ${#x} ${x#* } "${x##* }"
[ -n "$x" ] && [ -z "" ] && echo ok
test 5 -eq 5 -o 1 -eq 2 && echo ok2
[ ! -e /nonexistent ] && echo ok3
[ "(" = "(" ] && echo ok4
echo -n no newline; echo
echo "tab\there"
printf '%b\n' 'x\ty'
printf "%s %s\n" a b c d
: > /dev/null
echo ${HOME:+home} ~ > /dev/null
