# tar: create, list, extract; long names; fixed owner and time give the same archive twice
mkdir -p in/sub/deep
echo hello > in/a.txt
seq 1 3000 > in/sub/nums
ln -s ../a.txt in/sub/lnk
chmod 750 in/sub/deep
chmod 600 in/a.txt
d=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
mkdir -p in/$d/$d/$d/$d
echo long > in/$d/$d/$d/$d/a-file-whose-path-is-longer-than-two-hundred-and-fifty-five-characters.txt
tar -cf one.tar --owner=0 --group=0 --mtime=@1700000000 -C in .
tar -cf two.tar --owner=0 --group=0 --mtime=@1700000000 -C in .
cmp one.tar two.tar && echo "same archive twice"
tar -tf one.tar | sed "s/$d/D/g"
mkdir out && tar -xf one.tar -C out
for f in a.txt sub/nums $d/$d/$d/$d/a-file-whose-path-is-longer-than-two-hundred-and-fifty-five-characters.txt; do cmp in/$f out/$f || echo "differs: $f"; done; echo "round trip"
readlink out/sub/lnk
ls -ld out/sub/deep out/a.txt | cut -c1-10
# members by name, the old option form, and standard input / output
mkdir part && tar cf - -C in sub/nums a.txt | tar xvf - -C part sub 2>&1
ls part part/sub
# what must not be unpacked
mkdir evil && cd evil && mkdir -p x && echo no > x/f && tar -cf ../e.tar x/../x/f 2>&1; cd ..
mkdir safe && tar -xf e.tar -C safe; echo "status $?"; ls safe
echo junk-that-is-not-an-archive-at-all > bad.tar; dd if=/dev/zero bs=512 count=2 >> bad.tar 2>/dev/null
tar -tf bad.tar; echo "status $?"
