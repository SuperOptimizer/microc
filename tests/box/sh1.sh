echo hello world
x=1; y="a b"; echo $x "$y" '$x' "$x$y" ${x}z
echo ${u:-default} ${x:+set} ${u-} "${u:=assigned}" $u
p=/usr/local/lib/libfoo.so.1
echo ${p##*/} ${p%/*} ${p#/} ${p%%.*} ${#p}
set -- a "b c" d
echo $# "$@" $*
for i in "$@"; do echo "[$i]"; done
for i; do printf '<%s>' "$i"; done; echo
shift; echo $1
if [ "$x" = 1 ]; then echo yes; elif true; then echo no; else echo never; fi
i=0; while [ $i -lt 3 ]; do i=$((i+1)); echo i=$i; done
until [ $i -le 0 ]; do i=$((i - 1)); done; echo $i
case foo.c in *.h) echo h;; *.c|*.S) echo c;; *) echo other;; esac
f() { local a=$1; echo "f:$a:$#"; return 3; }
f q r; echo rc=$?
echo $(echo sub; echo two) `echo bq`
echo "$(printf 'a\nb\n')"
true && echo and; false || echo or; ! false && echo not
echo $((1+2*3)) $(( (1+2)*3 )) $((7/2)) $((7%4)) $((1<<4)) $((x==1?10:20)) $((0x10))
cat <<E1
heredoc $x $(echo cs)
  \$x "q"
E1
cat <<'E2'
raw $x
E2
cat <<-E3
	tabbed
	E3
echo a | tr a b 2>/dev/null || echo piped
{ echo g1; echo g2; } > /dev/null
(exit 4); echo sub=$?
echo "a\tb" 'c\nd'
printf '%s=%d %5s|%-5s|%03d %x\n' k 5 r l 7 255
IFS=: ; set -- $PATH_T; IFS=' 	
'
v="x:y:z"; IFS=:; for q in $v; do echo q=$q; done; unset IFS
echo "${v}" ${v%:*}
a=1 b=2; echo $a$b
test -d / && echo dir; [ -f /nonexist ] || echo nofile; [ 3 -gt 2 -a 1 -lt 2 ] && echo cmp
echo * | wc -w | tr -d ' ' >/dev/null
echo "$#" "$?" "$0" > /dev/null
x=abc; echo ${x%c} ${x#a} "${x}"'q'"r"
echo \$ \" \\ "\$" "\\" '\\'
n=5; while :; do n=$((n-1)); [ $n -gt 2 ] && continue; break; done; echo n=$n
for a in 1 2; do for b in x y; do [ $b = y ] && continue 2; echo $a$b; done; done
g() { echo "$@"; }; g "a  b" c
eval 'echo ev $x'
z=$(false; echo $?); echo z=$z
read l1 l2 <<E4
one two three
E4
echo "$l1|$l2"
echo one two three | while read a b; do echo "$b $a"; done
echo end
