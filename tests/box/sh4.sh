# kbuild-like idioms
set -e
tmp=$W/tmp.$$
trap "rm -f $tmp" EXIT
{ echo "#define A 1"; echo "#define B 2"; } > $tmp
if [ -r $tmp ] && cmp -s $tmp $tmp; then echo same; fi
cat $tmp | sed -n 's/#define \(.\) \(.\)/\1=\2/p'
info() { printf "  %-7s %s\n" "${1}" "${2}"; }
info LD vmlinux
vmlinux_link() {
	local output=${1}
	local objs
	shift
	objs="a.o b.o"
	echo ld -o ${output} ${objs} "$@"
}
vmlinux_link out x y
is_enabled() { grep -q "^$1=y" /dev/null; }
if is_enabled CONFIG_X; then echo en; else echo dis; fi
for f in a b; do
	case "$f" in
	a) continue ;;
	esac
	echo $f
done
VERSION=5 PATCHLEVEL=15
echo $(expr $VERSION \* 65536 + $PATCHLEVEL \* 256)
x=`echo a b | cut -d' ' -f2`; echo $x
[ -z "$KBUILD_VERBOSE" ] || echo verbose
set -- -o out -l dep file1
while [ $# -gt 0 ]; do
	arg="$1"; shift
	case "$arg" in
	"-o") out="$1"; shift;;
	"-l") dep="$1"; shift;;
	*) echo "arg $arg";;
	esac
done
echo $out $dep
while getopts "ab:c" o -a -b val -c rest; do echo "opt $o ${OPTARG-}"; done; echo $OPTIND
echo "multi \
line" 'and
newline'
if [ "a" != "b" ] ; then
	echo neq
fi
cat <<EOF2 | tr a-z A-Z
upper $x
EOF2
echo $(( 5 * 65536 + 15 * 256 + 167 ))
long="$(echo one
echo two)"
echo "$long"
empty=
for w in $empty; do echo never; done
[ "$empty" ] || echo emptyok
echo a${empty}b "a${empty}b"
arr="1 2 3"; sum=0; for n in $arr; do sum=$((sum+n)); done; echo $sum
echo "${arr% *}|${arr#* }|${arr%% *}|${arr##* }"
basename /a/b/c.txt .txt; dirname /a/b/c.txt
echo "it's" "say \"hi\"" 'a"b'
f() { for a; do echo "arg:$a"; done; }; f "x y" z
printf '%s\n' "$@" end
# the word of ${x:+word} and ${x:-word} splits into fields unless quoted
cnt() { echo $#; }
A=x; unset B
cnt ${A:+-accel $A}
cnt "${A:+-accel $A}"
cnt ${B:-one two  three}
cnt ${B:-"one two" three}
cnt ${A:+'a b'} ${B:+nothing}
v=${B:-p q}; echo "$v"
exit 7
