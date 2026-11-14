set -e
f() { false; echo notreached; }
if f; then echo t; else echo e; fi
false || echo ok1
! true
echo before
(false) || echo ok2
x=$(false) || echo ok3
g() { return 2; }
g && echo no
echo st=$?
trap 'echo bye $?' EXIT
h() { false; }
h
echo notreached
