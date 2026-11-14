import re,sys
gen={}
txt=open('/usr/include/asm-generic/unistd.h').read()
d={}
for m in re.finditer(r'#define\s+(__NR3264_|__NR_)(\w+)\s+(\d+)', txt):
    d[m.group(2)]=int(m.group(3))
# 3264 aliases on 64-bit
alias={'fcntl':'fcntl','statfs':'statfs','fstatfs':'fstatfs','truncate':'truncate','ftruncate':'ftruncate','lseek':'lseek','sendfile':'sendfile','fstatat':'newfstatat','fstat':'fstat','mmap':'mmap','fadvise64':'fadvise64'}
x={}
for m in re.finditer(r'#define\s+__NR_(\w+)\s+(\d+)', open('/usr/include/x86_64-linux-gnu/asm/unistd_64.h').read()):
    x[m.group(1)]=int(m.group(2))
out=[]
seen={}
for name,n in sorted(d.items(), key=lambda kv: kv[1]):
    xn=alias.get(name,name)
    if xn in ('syscalls','arch_specific_syscall'): continue
    if n in seen: continue
    if xn in x:
        seen[n]=1
        out.append("#define __X64_SYS_%d %d /* %s */" % (n, x[xn], xn))
    else:
        sys.stderr.write("no x86-64 call for %s (%d)\n" % (name,n))
print("\n".join(out))
