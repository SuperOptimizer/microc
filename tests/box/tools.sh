cd "$W"
printf 'b 2\na 10\nc 1\na 10\n' > f1
sort f1; sort -r f1; sort -n -k2 f1; sort -u f1; sort f1 | uniq -c | sed 's/^ *//'
cut -d' ' -f2 f1; cut -c1 f1; echo a-b-c | cut -d- -f1,3; echo 0123456789 | cut -b -4
head -n 2 f1; head -1 f1; tail -n 1 f1; tail -n +3 f1; tail -2 f1
wc -l < f1 | tr -d ' '; wc -c f1 | sed 's/^ *//'
tr a-z A-Z < f1; tr -d '\n' < f1; echo; echo "a  b   c" | tr -s ' '; printf 'a\0b\n' | tr '\0' '\n'; echo hello | tr -d el; echo abc | tr '[:lower:]' '[:upper:]'
basename /x/y/z.c .c; basename z/; dirname /x/y/z.c; dirname z; dirname /; dirname a/b/
expr 5 \* 65536 + 15 \* 256 + 167; expr abc : 'a\(.*\)'; expr abcd : abc; expr 3 \> 2; expr length hello; expr 1 = 2; echo rc=$?
seq 3 | xargs; seq 5 | xargs -n2; echo "a 'b c' d" | xargs -n1; printf 'x\ny\n' | xargs -I{} echo "<{}>"
printf 'hello\n' | sha1sum; printf '' | sha1sum; sha1sum f1
printf 'abc\n\001\377' | od -v -b; printf 'ab' | od -An -t x1; printf 'abcdefghijklmnopqrstuvwxyz' | od -c
find d -name e -prune -o -type f -print; find d -maxdepth 1 | sort; find d -name '*.c' -exec echo got {} \; | sort
cp f1 f2; cmp f1 f2 && echo same; echo x >> f2; cmp -s f1 f2 || echo differ; diff f1 f2; echo rc=$?
cp -r d d2; find d2 | sort; rm -rf d2; ls d2 2>/dev/null; echo rc=$?
ln -s f1 l1; readlink l1; ln -sf f2 l1; readlink l1; ln -fsn d dl; readlink dl; ls -d d f1
mv f2 f3; ls f3; [ -e f2 ] || echo moved; rm f3 l1 dl
echo "one two" | grep -q two && echo found; grep -c a f1; grep -v a f1; grep -n '^a' f1; grep -E 'a|c' f1 | wc -l | tr -d ' '; grep -e b -e c f1; grep -qi B f1 && echo ci; grep -h a f1 f1 | wc -l | tr -d ' '; grep a f1 f1 | head -1; grep -l a f1; grep -o '[0-9]*' f1 | head -2; grep -w 1 f1; grep -F 'a 1' f1 | head -1; grep -x 'c 1' f1
echo abc | sed 's/b/X/'; echo aaa | sed 's/a/b/g'; echo aaa | sed 's/a/b/2'; echo 'a/b' | sed 's:^.*/::g'; printf 'a\nb\nc\n' | sed -n '2p'; printf 'a\nb\nc\n' | sed '2d'; printf 'a\nb\nc\n' | sed '$d'; printf 'a\nb\nc\n' | sed -n '$p'; printf 'a\nb\nc\n' | sed '1,2s/$/!/'
printf 'x\nINSERT\ny\n' | sed -n '/^INSERT$/ q; p'; echo "hello world" | sed -e 's/\(hello\) \(world\)/\2 \1/' -e 's/^/> /'; echo 'a b' | sed 's/ /\n/' ; echo abc | sed 's/b/[&]/'; echo 'path/to' | sed 's/\//\\\//g'; echo "x" | sed 'a\
appended'; echo x | sed 'i\
inserted'; printf 'a\nb\n' | sed -n '/a/{n;p;}'; printf 'a\nb\n' | sed 'N;s/\n/+/'; printf '1\n2\n3\n4\n' | sed -n '/2/,/3/p'; printf '1\n2\n3\n' | sed '/2/!d'; echo ABC | sed 'y/ABC/xyz/'; printf 'a\nb\n' | sed -n '$='; printf 'a b c\n' | sed 's/[[:space:]]*$//;s/a/A/'; printf 'a\nb\nc\n' | sed ':a;N;$!ba;s/\n/,/g'; printf 'k=v\n' | sed -n 's/^\([a-z]*\)=\(.*\)$/\2=\1/p'; echo aXb | sed 's/x/-/I'; echo "a.b" | sed 's/\./_/g'; echo '//kbuild:lib-y += a.o' | sed -n 's@^//kbuild:@@p'
printf 'one\ntwo\n' | sed -e '1h;2{x;G;}' ; echo abc | sed -E 's/(a)(b)+/\2\1/'; printf 'a\n\nb\n' | sed '/^$/d'; echo 00012 | sed 's/^00*/0/'; printf ' x y\n' | sed -e 's: \([^- ][^ ]*\): -l\1:g' -e 's/^ *//'
echo a > g1; sed -i 's/a/b/' g1; cat g1; printf 'x\n' | sed 's/x/a\
b/'; printf 'a\tb\n' | sed 's/\t/ /'
dd if=f1 bs=2 skip=1 count=2 2>/dev/null; echo; printf 'abcdef' | dd bs=2 skip=1 2>/dev/null; echo
echo 'obase=16; 255; 2^10; x=5; x*3' | bc; echo '10/3; 10%3; (1+2)*3' | bc
touch -c nonexist; ls nonexist 2>/dev/null; chmod 600 f1; ls -l f1 | cut -c1-10; chmod +x f1; ls -l f1 | cut -c1-10; chmod go-rwx,u=rw f1; ls -ln f1 | cut -c1-10
diff -u f1 t1; printf 'a\nb\nc\nd\ne\nf\ng\nh\n' > u1; sed 's/d/D/;7d' u1 > u2; diff u1 u2; diff -u u1 u2 | tail -n +3
