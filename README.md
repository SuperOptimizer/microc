# mcc

A small, fast, self-hosting C89 toolchain written from scratch in C89:
compiler, assembler, linker, archiver and C library. Target: arm64 macOS.

    make              build mcc with the host compiler, then libc with mcc
    make test         run the test suite            (STAGE=2 tests the self-built compiler)
    make bootstrap    build mcc with itself three times; stages 2 and 3 must be identical

    build/stage0/mcc hello.c -o hello     compile and link
    build/stage0/mcc -c file.c            object file (Mach-O)
    build/stage0/mcc -E file.c            preprocess
    build/stage0/mcc --ir file.c          print the optimised IR
    build/stage0/mcc -fno-strict-aliasing file.c   see "Aliasing" below
    build/stage0/mcc -flto a.c b.c -o prog         see "Link-time optimisation" below
    build/stage0/mcc -finline-limit=N file.c       inline functions of up to N instructions (default 12; 0 turns it off)
    build/stage0/mcc -Os file.c                    prefer small code: less inlining, loops tested once at the top
    build/stage0/mcc -fno-jump-tables file.c       a switch stays a tree of comparisons
    build/stage0/mcc -ffp-contract=fast file.c     a * b + c is rounded once (fmadd); off by default
    build/stage0/mcc -jN a.c b.c c.c               compile up to N files at once (default: one per processor; -j1 for none)
    build/stage0/mcc as file.s -o file.o  assemble
    build/stage0/mcc ld a.o b.o lib.a     link
    build/stage0/mcc ar lib.a a.o b.o     archive

Options: `-o -c -E -D -I -e entry -nostdlib -nostdinc`. mcc finds `include/`
and `lib/libc.a` next to its own binary, or under `$MCC_HOME`.

## How it is put together

    source -> lexer + preprocessor -> parser -> SSA IR -> optimiser
           -> arm64 instruction selection -> register allocation -> machine code
           -> in-memory object -> linker -> signed Mach-O executable

| File | Role |
| --- | --- |
| `src/lex.c`, `src/pp.c` | tokens, source locations, full C89 preprocessor |
| `src/type.c`, `src/parse.c` | types and a one-pass parser that builds IR directly (no syntax tree) |
| `src/ir.c`, `src/ir.h` | target-independent SSA IR, constant folding, printer |
| `src/opt_ssa.c` | stack slots to SSA values, dead code elimination |
| `src/opt.c` | value numbering, memory forwarding (what is known about memory is carried across branches and joined where they meet), control-flow simplification (jump threading, if-conversion to selects), loop-invariant code motion |
| `src/opt_inline.c` | inlining of small functions |
| `src/lto.c` | link-time optimisation: IR in object files, whole-program inlining, removal of unused functions |
| `src/mir.h`, `src/ra.c` | machine IR and a target-independent linear-scan allocator with lifetime holes, loop-weighted spill costs and coalescing of copies |
| `src/arm64.c` | instruction selection (addressing modes, shifted and inverted operands, rotates, bit-field extracts, conditional-compare chains, bit-test branches, jump tables), frames, tail calls, early returns ahead of the frame, tail merging, encoding |
| `src/arm64_asm.c` | assembler |
| `src/obj.c`, `src/macho.c`, `src/link.c`, `src/macho_exe.c`, `src/sha256.c` | objects, archives, linker, executable writer with ad-hoc code signature |
| `libc/` | C89 library; `libc/sys/darwin_arm64.s` is the only OS-specific file |

The IR and everything above it know nothing about arm64 or Mach-O. A new
target supplies instruction selection and encoding (`arm64.c`), a register
description for `ra.c`, and an object/executable format.

## Speed of the generated code

`sh bench/cmp.sh [runs]` builds the kernels of `bench/bench.c` with mcc, with `mcc -O3` and with
`clang -O2`, checks that all three print the same checksums, and prints the least time of each kernel
and the ratios to clang; then it builds mcc's own source into a compiler both ways and compares the
two compiling that source (cycles, least of the runs). Decisions about the optimiser are made from
this table, not from instruction counts.

A switch whose case values lie close together (at least five tests, at least a third of the range in
use) jumps through a table of offsets that sits in the code right behind the jump. A branch on a value
that was just loaded and is used for nothing else stays a branch where it could have been a select:
the processor guesses it and carries on, where a select would make everything after it wait for the
load. `lo <= x && x <= hi` is one unsigned comparison of `x - lo`. Variables that change in a loop
keep one register all the way round: copies whose two sides are never alive together are joined
before registers are handed out, and an increment written ahead of the last use of the old value
(`*p++`) is computed after it.

## Debug information

`-g` (any `-g...` but `-g0`; nvcc's `-lineinfo` / `-G` too) on x86-64 Linux adds DWARF 5 to each object:
`.debug_line` (a row wherever the source location changes, with columns), `.debug_info` (the compile unit
and each function with its address range), `.debug_rnglists`, and `.eh_frame` frame descriptions of the
prologues and epilogues, so gdb, perf, addr2line and backtraces see `file:line` and walk the stack. There
are no variables or types yet. Each IR instruction carries the location of the token the parser was at
when it was made; what the optimiser makes carries none, and the line table goes on with the one before.
In CUDA device code `-g` writes `.file` / `.loc` into the PTX, and mcudahw then asks the driver's compiler
for line information (compute-sanitizer, cuda-gdb). Without `-g` the output is byte-identical. mcc's own
ELF linker keeps the `.debug_*` sections after the image.

## Aliasing

The optimiser assumes the C rule that an object is accessed through its own
type or as characters: a store through an `int *` is taken not to change
what a `long *`, a pointer-to-pointer or a `double *` reads, unless both
addresses are visibly the same. Code that reads memory through a cast to an
unrelated pointer type needs `-fno-strict-aliasing`.

`while` and `for` loops are compiled as a guard followed by a bottom-tested
loop, so the condition is parsed twice.

## Parallel compilation

When several `.c` files are given, each is compiled in a child process and
its object comes back through a pipe (with `-c` the child writes the file).
Results are taken in command-line order, so the output does not depend on
timing, and the code is the same as with `-j1`; only the order of the
symbol table differs. Building mcc itself takes about a third of the time.
Linking and `-flto` code generation are still serial.

## Code size

Small `static` functions get code of their own only if some call was not
inlined or their address is used. A function has one copy of its epilogue,
which the other returns jump to. Inlining trades size and compile time for
speed: `-finline-limit=0` gives the smallest output.

Blocks that go to the same place and end with the same instructions share
one copy of them. Neighbouring loads or stores of neighbouring fields
become one `ldp` or `stp`. A constant or address already in a register is
reused in the blocks that follow without a join. Identical string literals in a source file are stored
once. A call in return position whose arguments fit in registers becomes a
jump. `-Os` goes further at some cost in speed.

## Frames

A function with a frame of up to 504 bytes and no outgoing stack arguments
does not set up `x29`; it still saves the caller's `x29` and `x30`, but a
frame-pointer backtrace skips it. Leaf functions with nothing to save have
no frame. When a function starts with a test that leads straight to a
return (`if (!p) return 0;`), the frame is made after that test.

## Link-time optimisation

With `-flto` the compiler stops after parsing: each function goes into the
object as IR (section `__MCC,__ir`; `.mcc.ir` in ELF), and its symbol is
defined at its record there (Mach-O: a one-instruction placeholder). Data is emitted as usual. When such objects reach the linker
(`mcc`, `mcc ld`, straight from source or from `.o` and `.a` files, mixed
freely with ordinary objects), they are merged into one object and compiled
there:

- functions are compiled callees first, so small functions are inlined
  across source files, `static` ones included;
- a function called from exactly one place in the whole program, and whose
  address is not taken, is inlined there up to a much larger size and gets
  no code of its own;
- functions that nothing reaches from the entry point, from ordinary
  objects or from initialised data are left out.

`-flto` objects can only be linked by mcc. Under `-mglibc` (x86-64 Linux,
where the system's `cc` does the final link) mcc reads its objects back,
compiles the IR ones as one and hands `ld` the result. Given them directly,
`ld` stops at "defined in discarded section `.mcc.ir`" (the section is
SHF_EXCLUDE); `ld -r` keeps the IR for a later mcc link. Functions with inline
assembly and weak definitions are compiled at once, not kept as IR
(tests/ltoglibc.sh, `make test-lto`). An archive member compiled with `-flto` is always loaded
(its unused functions are still dropped). The C library can be built this
way too (`mklibc.sh` with `-flto -c`); `make` does not do so by default.

## Profile-guided optimisation

    mcc -fprofile-generate -o prog-gen *.c    # every block counts its executions
    ./prog-gen typical-input                  # writes (or adds to) mcc.prof at exit
    mcc -fprofile-use -o prog *.c             # rebuild from the counts

`-fprofile-use=FILE` and the `MCC_PROFILE` environment variable of the
instrumented program name a file other than `mcc.prof`. Counts from several
runs add up. A function is matched by source path (as given on the command
line) and name, and its counts are used only if it still has the same number
of blocks; anything else compiles as if there were no profile. Programs that
leave through `_exit` or a signal write nothing, and instrumented processes
that exit at the same moment can lose each other's counts.

The counts drive three things (src/prof.c attaches them to blocks right after
parsing; `Block.freq` carries them through the passes and through `-flto`):

- layout: blocks are chained along the most-taken edges so the common path
  falls through, and blocks that never ran go to the end of the function;
- register allocation: spill weights come from real counts, not loop depth;
- inlining: call sites that never ran inline only trivial callees, hot ones
  (within 1/1024 of the busiest block; `-fhot-shift=N`) take callees of up to
  40 instructions (`-finline-hot=N`) where the usual limit is 12.

On mcc compiling itself, trained on that same job: 3.3% fewer cycles, 5.9%
with `-flto` as well (code grows 7% and 25%: the price of the hot inlining).

On ufsm under `-mglibc` (2026-11-15: its 78 single-thread CPU test lines,
cycles:u, best of two; trained on one pass of the same suite) neither pays:
plain 1016.9 G cycles, `-flto` 1062.6 (+4.5%: test_zcache +22%,
test_phase_interp +17%, one spatial-augment part -28%), `-fprofile-use`
1014.3 (-0.25%, per test -6% .. +8%), both 1034.4 (+1.7%). The instrumented
build ran ~32x slower. ufsm keeps both off under mcc.

## -O3

The default pipeline is tuned for compile speed. `-O3` spends more:

- the value-numbering and control-flow passes repeat until nothing changes
  (3.5% more compile work for 0.4% less code on mcc itself);
- without a profile, block frequencies are guessed (loops run eight times;
  branch directions from the kind of test and what each side does, weights
  fitted to profiles of mcc itself, right for 63% of executed branches where
  the profile is right for 81%) and used like a profile's for layout and
  spill weights: 0.8% fewer cycles on mcc for 1.1% more code;
- loops are vectorised (src/opt_vec.c). The shape handled is a one-block
  loop whose memory accesses all move on by one element each time round,
  through an index (`a[i]`) or through pointers that are stepped (`*d++ =
  *s++`). The body may do element by element arithmetic (`+ - * & | ^`,
  negation and shifts by constants on 8 to 64-bit integers, `+ - * /` and
  negation on float and double, conversions between integers and floating
  point of the same width), compare elements and choose between values on
  the result (`a[i] > 0 ? a[i] : 0`), use values from outside the loop
  (also ones the loop re-reads from memory each time, such as the bound in
  `i < v->n`: the guard checks they are not among the elements written) and
  the loop counter itself (`a[i] = i * k`), and gather the elements into an
  integer sum or a running maximum or minimum (four vectors side by side),
  or bytes and shorts into an `int` sum. The loop may count with `i < n` or
  `i != n` (signed or unsigned), `while (n--)`, `while (n-- > 0)`, `do ...
  while (--n)`, or `p < end` / `p != end`. A copy of the loop working in
  16-byte vectors (two per iteration, four for sums) with NEON runs first,
  guarded by run-time checks that there are enough iterations and that the
  arrays written do not overlap the others; the original loop finishes the
  rest. Floating-point sums are vectorised only with `-ffast-math`, since
  adding in another order changes results.

`-fvec-report` says, for every candidate loop, whether it was vectorised and
if not, what stood in the way.

On such kernels (4096 elements) the gain is 4x to 18x, level with clang -O2:

                 mcc    mcc -O3   clang -O2
    saxpy        218       46        45  ms
    int sum      198       23        23
    int mul-add  259       60        57
    byte add     213       12        10
    double       203       92        91

The 128-bit values exist in the IR only after the vectoriser (type `v128`,
lane kind in `Ins.mem`); functions that have them are not offered to the
inliner. The register allocator never spills them (the pass limits how many
a loop may need).

## Building Linux

mcc is also a complete toolchain for building the Linux kernel (arm64) and a
user space for it, with no tool from the system involved:

    make tools                    the Linux and host C libraries, mbox and mvm, all compiled by mcc
    make test-tools               tests of the shell and the tools (tests/box) and of the virtual machine (tests/vm)
    build/stage0/bin/             every tool by name; put only this on PATH

| needed by the build            | provided by                                                     |
|--------------------------------|-----------------------------------------------------------------|
| cross compiler, assembler      | `mcc` as `aarch64-linux-gnu-gcc` (GNU C dialect, ELF objects)   |
| linker with linker scripts     | `mcc` as `aarch64-linux-gnu-ld`                                 |
| ar, nm, objcopy, strip, objdump| `mcc` under those names                                         |
| make                           | `mcc make` (src/make.c): GNU make 4.3 work-alike with a job server |
| lex, yacc                      | `mcc lex`, `mcc yacc` (src/mlex.c, src/myacc.c): flex and bison work-alikes |
| host compiler (fixdep, kconfig, modpost, dtc, ...) | `mcc --hosted`: a POSIX C library over raw Darwin system calls |
| sh                             | `mbox sh` (box/sh.c): a POSIX shell                             |
| a machine to boot the result   | `mvm` (vm/): an arm64 virtual machine, see below                |
| awk, sed, grep, find, bc, expr, xargs, sort, tr, cut, od, dd, cmp, diff, sha1sum, gzip, bzip2, rm, mkdir, mv, cp, ln, cat, ... | `mbox` (box/): about sixty tools in one 280 KB binary |

`mbox` is built by `mcc --hosted` from the strict C89 sources in `box/`. Its
shell runs the tools in the same process image after `fork` (no `exec`), runs
`#!/bin/sh` and `#!/usr/bin/awk -f` scripts itself, and takes `/bin/NAME` and
`/usr/bin/NAME` to mean its own tool when it has one. `mcc make` uses the `sh`
installed beside it for recipes. The host C library's `system` and `popen` use
the `sh` on PATH.

With linux-5.15 and busybox-1.36 unpacked on a case-sensitive volume, the
scripts in `linux/tools/` (not part of this tree) do the following; `jail.sh`
runs a build inside a sandbox that refuses to execute anything under `/bin`,
`/usr`, `/sbin`, `/opt`, `/Library`, `/System` or `/Applications`:

    sh linux/tools/jail.sh linux/tools/mb7.sh -j10       kernel: 17 s on ten cores
    sh linux/tools/jail.sh linux/tools/bbmake7.sh -j10   busybox: 5 s
    build/stage0/bin/sh linux/tools/boot7.sh             root file system (the kernel's gen_init_cpio), then mvm

Both builds give the same generated files as builds that use the system's
make, shell and tools (GNU make, flex, bison, Apple's sh, sed, awk, ...), bar
the time stamps. The kernel boots in mvm into the busybox built the same
way and runs a script of about sixty commands; the output is what qemu shows. The whole is repeatable with
`MCCDIR=build/stage2`, the toolchain compiled by itself; `make bootstrap` and
`make test-tools` also pass when run by `mcc make` and `mbox` inside the sandbox.

### mvm

`mvm` (vm/, 5200 lines of C89, built by `mcc --hosted`) is the machine the
kernel runs on: RAM, up to eight CPUs, a GICv2, a PL011 console on the
terminal, the generic timers, PSCI and virtio devices, described to the guest
by a device tree that mvm writes itself. It takes qemu's spelling of the
options it shares with it:

    mvm [-m megabytes] [-smp cpus] [-accel hvf|emu] -kernel Image [-initrd file] [-append "command line"]
        [-disk image]... [-net] [-fwd hostport:guestport]... [-share dir | -share-ro dir] [-rng] [-vcon]
        [-gpu] [-res WxH] [-headless] [-screenshot file] [-play script] [-d]

There are two CPUs behind it. `-accel emu` interprets A64: EL0 and EL1, the
stage-1 MMU with a software TLB, exceptions, system registers, exclusives,
scalar floating point and the Advanced SIMD subset that compilers emit for
plain C. It counts time in executed instructions and skips ahead when the
guest idles with nobody at the keyboard, so a run is repeatable to the
instruction; it does about 70 million instructions a second and boots the
kernel to the end of the busybox script in under two seconds. `-accel hvf`
runs the guest on the real CPU through Hypervisor.framework, the macOS
counterpart of KVM, and comes back to mvm for devices, PSCI calls and
idling. The default is hvf when the host permits it, else emu. Ctrl-A x
leaves an interactive session.

With `-smp N` the guest has N processors, started through PSCI. Under hvf
each is a thread of the host (the pthread functions come from libSystem like
the hypervisor's), and one lock guards the devices. The interpreter runs them
in turn on one thread, 20000 instructions at a time, so a run with several
CPUs is still repeatable.

The devices are virtio ones behind the MMIO transport (vm/virtio.c):

- `-disk FILE`: a disk (`/dev/vda`, ...) kept in a file of the host.
- `-net`: a network card, with the network behind it in vm/net.c. The guest
  is alone on 10.0.2.0/24 as 10.0.2.15 (DHCP says so); mvm answers ARP and
  pings to the gateway 10.0.2.2, and carries the guest's TCP connections and
  UDP packets over ordinary sockets of the host, so nothing needs privilege.
  10.0.2.2 is the host's loopback, 10.0.2.3 the name server of the host's
  /etc/resolv.conf, port 7 of the gateway echoes. `-fwd 8080:80` makes port
  8080 of the host's loopback reach port 80 of the guest.
- `-share DIR`: a directory of the host, by the 9P2000.L protocol (vm/p9.c):
  `mount -t 9p -o trans=virtio,version=9p2000.L host /mnt`. Names cannot
  leave the directory and the host follows no symbolic links for the guest.
- `-rng`: entropy from the host's /dev/urandom (`/dev/hwrng`).
- `-vcon`: a virtio console (`console=hvc0`) in place of the PL011 one.

- `-gpu` (or `-res WxH`): a screen, a keyboard and a pointer (vm/gpu.c:
  virtio-gpu in two dimensions, virtio-input), shown in a window (vm/gui.c).
  The window is Cocoa's, opened from C89 through the Objective-C runtime
  with `-framework Cocoa`; closing it or Command-Q turns the machine off.
  `-headless` leaves the window out, `-screenshot FILE` writes the screen
  as a BMP at the end, and `-play FILE` feeds a script of key presses,
  pointer movements and screenshots, which is how the screen is tested.

On that screen Linux shows its console (`console=tty0`), or `mdesk` (desk/,
700 lines of C89 built by mcc for Linux): a desktop that draws on /dev/fb0
with the kernel's console font, with windows to drag, raise and close, a
terminal running the shell on a pseudo-terminal, and a system monitor.
`linux/tools/desk.sh` boots into it; `linux/tools/desktest.sh` drives it
without a window on both kinds of CPU, typing a command into its terminal.

`linux/tools/devtest.sh` boots a kernel that has the drivers with two CPUs
and all five devices on both kinds of CPU and compares what the guest
reports: a file system made, filled and remounted on the disk; files created,
renamed, linked and removed in the shared directory; a DHCP lease, pings,
three megabytes through a TCP connection and back, a refused connection.

Using the hypervisor took two additions to mcc's Mach-O linker, since an mcc
program otherwise imports nothing: `-framework NAME` (or `--dylib=PATH`) loads
a library and binds the symbols left undefined to it through stubs, and
`--entitlement=KEY` puts an entitlement into the ad-hoc code signature.

`tests/vm` is a bare-metal guest, built with mcc's cross tools: batches of
about 300 instruction forms run on pseudo-random inputs and folded into one
number per batch, then exceptions, the timer and interrupt controller, and a
visit to EL0 behind page tables with permission faults. `make test-tools` runs
it on both CPUs, and the interpreter must print what the real CPU prints.

What is still the system's: the kernel that all of this runs on (and its
Hypervisor.framework when `-accel hvf` is used).
Not in mvm: floating-point exception flags and rounding modes other
than to-nearest, the SIMD instructions mcc does not generate; IPv6, IP
fragments and listening on other than the loopback address; owners, device
nodes and extended attributes in the shared directory (files are root's);
3-D acceleration, the hardware cursor and resizing of the screen.
mcc passes and returns structures of floating-point members its own way, not
the platform's, so such calls into system libraries need a detour (vm/gui_arm64.s).
Limits: the hosted library has no signal handlers and its local
time is UTC; `bzip2` only compresses; `bc` is integer-only; the shell has no
job control, aliases or interactive mode; busybox's documentation targets want
perl's pod2text, which is absent (the makefile ignores that).

## Threads, thread-local storage and atomics

For Linux programs (`--target=aarch64-linux-gnu`) the compiler and its C
library support threads. `make test-threads` runs tests/thr in mvm, on the
real CPU with four processors and on the interpreter with two.

Atomics. `_Atomic` (qualifier and `_Atomic(T)`), `<stdatomic.h>`, and the GNU
`__atomic_*` and `__sync_*` built-ins work for objects of 1, 2, 4 and 8 bytes
on every target. Reading or assigning an `_Atomic` object, `++`, `--` and the
compound assignments are atomic and sequentially consistent, as C11 requires.
In the IR an atomic operation is an `OP_CALL` flagged `IF_ATOMIC` whose
"callee" is a constant naming the operation, the size and whether the order
is relaxed; the back end expands a load to `ldar`, a store to `stlr` and
everything else to an `ldaxr`/`stlxr` loop (ARMv8.0: no LSE instructions, no
library calls). Relaxed loads and stores are plain ones. Operators that the
loop cannot do in one instruction (`*=`, shifts, floating point, `_Bool`) are
a compare-and-swap loop. Not atomic: `_Atomic` structures and anything not of
those four sizes (accepted, and accessed plainly). The weak compare and
exchange is the strong one; acquire and release are sequentially consistent.

Thread-local storage. `_Thread_local`, `thread_local` and `__thread`, static
or extern, at file or block scope, go to `.tdata` and `.tbss`. Only the
local-exec model exists (everything is linked statically): the address is
`OP_TLS`, which becomes `mrs x, tpidr_el0` and two `add`s with the
`TLSLE_ADD_TPREL_HI12` and `LO12_NC` relocations. The linker lays the two
sections out as one `PT_TLS` segment and resolves the offsets for variant 1
(the thread pointer, a 16-byte block, then the image at its alignment).

Threads. libc/linux has `<pthread.h>`, `<semaphore.h>` and `<sched.h>`:
threads (made with `clone` on stacks from `mmap` with a guard page; one
mapping holds guard, stack, the thread's descriptor and its thread-local
data), mutexes (normal, recursive, error checking; timed), condition
variables, read-write locks, barriers, once, keys with destructors, spin
locks, semaphores, all on futexes. `errno` is per thread. The library takes
its own locks only once a second thread has been made: `malloc` keeps a small
cache of free blocks per thread and locks the heap behind it; every stream
has a recursive lock (`flockfile`); `rand`, `atexit`, `getenv`/`setenv` and
the stream table have one each. Missing: cancellation (`pthread_cancel`
fails with ENOSYS), `<threads.h>`, robust and priority-inheriting mutexes,
process-shared objects; `strtok`, `asctime` and their kind keep static state.

## Third-party libraries for Linux programs

`mkulibs.sh MCC DIR` (or `make ulibs`) builds, with mcc and the tools in `DIR/bin` only, the libraries a
larger Linux program such as ufsm links, and installs them in `DIR/linux/ext/include` and `DIR/linux/ext/lib`:

| library | from | notes |
| --- | --- | --- |
| `libz.a` | zlib 1.4 | |
| `libzstd.a` | zstd 1.5.7 | no assembly; `ZSTD_MULTITHREAD` (with `THREADS=0`: one thread, and a request for workers is accepted and ignored) |
| `libblosc.a` | c-blosc 1.21.6 with lz4 1.9.4 inside | codecs blosclz, lz4, lz4hc, zlib, zstd; generic (scalar) shuffle |
| `libjpeg.a` | libjpeg-turbo 2.1.5.1, plain C | the libjpeg v8 API, as Debian and Ubuntu build it |
| `libcurl.a` | curl 8.11.0 | HTTP/1.1 and HTTPS; no HTTP/2 (asking for it is accepted and gives HTTP/1.1); IPv4; BearSSL is inside, so `-lcurl` is enough |
| `libbearssl.a` | BearSSL 0.6 | curl's TLS (1.0 to 1.2); certificates are checked against `/etc/ssl/certs/ca-certificates.crt` in the guest |
| `libcrypto.a` | `libc/ext/crypto` (ours) | only `openssl/evp.h`'s digest calls, over our own SHA-256 |
| `xxhash.h` | xxHash 0.8.3 | header only; compiles as it is, in its scalar path |

    sh libc/ext/fetch.sh                     # download, check sha256 (libc/ext/SOURCES.txt), unpack under build/ext
    make ulibs && make test-ulibs            # THREADS=0 make ulibs: zstd, blosc and curl without pthreads
    mcc --target=aarch64-linux-gnu prog.c -lcurl -lblosc -lzstd -ljpeg -lcrypto -lz    # the driver looks in DIR/linux/ext by itself

Nothing of the libraries' own build systems is run: the config headers are written by hand (`libc/ext/jpeg`,
`libc/ext/curl`), the file lists are in the script, and the few source changes are `sed` lines there (each
checks that it applied). `tests/ulibs.sh DIR` builds `tests/ulibs/t_*.c` and runs them in one boot of mvm:
round trips, streams and images made by the Mac's tools (`tests/ulibs/gen.py`), SHA-256 and XXH3 vectors,
and libcurl's easy, multi and share interfaces with range requests over HTTP and HTTPS against a small
server of our own (`tests/ulibs/tls_server.c`). The guest has no clock: a program that checks
certificates needs the date set first (`date -s @SECONDS`).

## CUDA: a PTX backend and a CPU run time

mcc compiles CUDA sources (`.cu`) by itself: the host part to arm64 as usual, the device part to NVIDIA's
PTX, which is kept as text inside the object file. No NVIDIA software is involved, and the PTX has never
been given to NVIDIA's assembler or a real GPU: what runs it is our own, `cuda/`, **mcuda**, a replacement
for `libcudart` and `libcublasLt` whose one device is the CPU, interpreting the PTX.

    make cuda && make test-cuda              # mkcuda.sh: headers to DIR/cuda/include, libraries to DIR/cuda/lib64
    mcc elf nvcc -O3 -c kernels.cu -o kernels.o      # the driver under nvcc's name: nvcc's own options are accepted
    mcc elf gcc main.o kernels.o -L DIR/cuda/lib64 -lcudart -lcublasLt -lstdc++ -lm -lpthread
    mcc --hosted -isystem DIR/cuda/include -include DIR/cuda/include/mcuda_device.h k.cu DIR/host/cuda/lib/libmcuda.a   # on the Mac

**The language.** A `.cu` file is read as a subset of C++ (`opt_cxx` in `src/parse.c`), chosen by what
real kernels use: function and class templates (kept as tokens and instantiated on use, with deduction,
defaults, non-type parameters, overloads picked by conversion cost), references, lambdas (inlined),
`constexpr` functions and variables evaluated at compile time, constructors with member initialisers,
static member functions and nested types, default arguments, `auto`, namespaces (transparent),
`extern "C"`, `std::lock_guard`, brace initialisation in assignments and returns. There are no
exceptions, no virtual functions, no destructors beyond lock guards, no standard library beyond the few
headers in `cuda/devinc`. A function that is called but not declared is an error.

**Device code.** `__global__` and `__device__` functions are compiled to SSA like any other and then
written by `src/ptx.c` instead of the arm64 backend: one PTX register per value, structured memory as
`ld`/`st`, calls as `call.uni`, `__shared__` variables as `.shared`, the mathematical library as PTX
instructions where there is one and as calls of run-time functions (`__mcuda_expf`) where not. Inline
assembly in device code is PTX (`asm("popc.b32 %0, %1;" : "=r"(r) : "r"(x))`), and that is how
`cuda/devinc/mcuda_device.h` (included before every `.cu` file) defines what nvcc knows without a header:
barriers, warp shuffles and votes, atomics (`atom.*`), bit operations, rounding-mode arithmetic,
`cuda_fp16.h`'s half type over `_Float16`. A launch `k<<<grid, block, shared, stream>>>(args)` becomes a
call of `__mcuda_launch(&module, "name", grid, block, shared, stream, argv)`. A variable at file scope
that device code reads is the host's object: the module carries a table of such names and addresses.
On Linux every object also puts its module's address into the section `mcuda_modules`, which the run
time walks, so that a kernel may be launched from another translation unit than the one that defines it.
Device code is never vectorised.
`__CUDA_ARCH__` is defined for the whole unit (860, or from nvcc's `-arch=sm_NN` / `-gencode`), because a
unit is read once, not once for each side as nvcc does; so a function for both sides takes its device
form, and host code can come to call what nvcc reserves for the device. The atomic functions therefore
work on the host too (as the plain operation): they ask `__builtin_mcuda_device()`, which the PTX
backend answers with 1, dropping the other arm, and the arm64 backend with 0. PTX assembly that ends up in
the host's copy of a function for both sides traps if it is reached.
The PTX is written with the interpreter in mind: a comparison is computed where its result is used,
blocks fall through, `threadIdx` and its like are read from their registers, and a value carried round a loop
shares a register with its next value where that needs no liveness analysis.
A product and the sum it feeds become one `fma` (nvcc's `-fmad`, on unless `-fmad=false`), by the rules a
real GPU was found to follow (NVIDIA's PTX compiler fuses too, so its PTX does not tell): a product is
fused when every use of it is an addition or subtraction in its own block, straight or negated; of two
products in one sum, the one LLVM's reassociation ranks lower; and a sum of floats made in double and
narrowed at once is a float sum. `tests/cuda/fmad.cu` has 36 shapes of this, bit for bit as the GPU has
them. A product and its sum in different blocks (nvcc first turns small branches into selects) are not fused.

**The run time** (`cuda/`, strict C89 like the rest): `ptxparse.c` reads PTX into a compact code,
`ptxexec.c` and `ptxops.c` interpret it: the threads of a block are run in turn, each until it ends or
must wait for the others (a barrier, a warp-synchronous instruction), and blocks are spread over host
threads. `rt.c` is the `cuda*` API (device memory is host memory, streams and events are bookkeeping),
`driver.c` the `cu*` driver API, `cublaslt.c` the matrix products of cublasLt, `algo.c` sorting. The
device presents itself as an RTX 3090 (sm_86). A thread that goes wrong stops the launch with a message
naming the kernel, the block and thread, and the line of the PTX.

The sums of a floating-point `mma` are made in double precision and rounded to the nearest once, which is
closer to the true sum than a GPU's: the hardware cuts every term and the result toward zero, and how it
cuts differs between its generations, so portable programs cannot depend on those bits. With
`MCUDA_MMA_HW=1` in the environment (or `mc_mma_hw(1)` of `cuda/mc.h`) the engine makes them bit for bit as
an sm_120 device does, in every floating-point shape: f16, bf16 and tf32 operands, f32 and f16 sums, e4m3,
e5m2, and the block-scaled kinds (mxf8f6f4 with e4m3, e5m2, e2m3, e3m2 and e2m1 elements, mxf4, mxf4nvf4).
`tests/cuda/mmahw.py` states the model, `tests/cuda/hw/mma.txt` holds the 18688 words of the device it
was found from, and `h_mma.ptx` runs them all. It costs time: a loop that is all m16n8k16 of f16 runs 2.2
times as long (the other shapes, rare in practice, go through C and are slower still). Without the switch
only what costs nothing is the hardware's: a NaN is 0x7fffffff and a zero sum is +0. Sums with an infinity
or a NaN among the terms are the same in both modes. (`MCUDA_MMA=0` is something else: it turns off the
assembly routines for these sums, for testing.)

**Tests.** `tests/cuda.sh DIR`: PTX files run by the engine against expected output (`g_*.ptx` were
generated from reference semantics by `tests/cuda/gen.py`), the run time's C interface, a whole CUDA
program, and `diff.cu`, in which the same functions (integer and floating arithmetic of every width,
conversions, NaNs, halves, control flow, structures, arrays, bit operations) run as device code and as host
code and must agree, with warp operations and atomics checked against their definitions; on the Mac and in
a Linux guest.
The programs that are plain CUDA have their expected output from hardware: `tests/cuda/gpu.sh` builds them
with NVIDIA's nvcc on a machine with a GPU (`CUDA_SSH="ssh ..."`) and compares, or with `UPDATE=1` writes
the `.exp` files; mcc's build of the same source on mcuda must then print the same. Each is small and aimed
at one thing programs need: `fmad` (fusing), `tc` (tensor cores as inline PTX: `mma` of fp16, bf16, tf32 and
int8, `ldmatrix`, `cp.async`), `mx` (e4m3 / e2m1 conversions and the block-scaled `mma` of sm_120a), `intr`
(some 60 functions with one right answer summed over special and random operands, the approximate ones held
to a few units in the last place, shuffles, reductions and votes of a warp, atomics), `lt` (cublasLt
products of every type and layout used), `api` (streams, events, asynchronous and two-dimensional copies,
page-locked and managed memory, large shared memory, the errors of impossible launches), `cxx` (templates,
`if constexpr`, lambdas, records by value, vector types), `shared`, `symbol`, `diff`. An RTX PRO 4000
Blackwell (driver 595, CUDA 13.2) gave the present expectations.

**ufsm.** The reason for all this was building [ufsm](https://github.com/SuperOptimizer/ufsm) (160 C23
files, 165 CUDA files, about 2500 kernels) unmodified, with `mcc make` driving its own Makefile:

    sh libc/ext/fetch.sh && make tools ulibs cuda
    mcc make -C ufsm ARCH=86 CCACHE= HOSTCC="mcc elf gcc" NVCC="mcc elf nvcc" CUDA=DIR/cuda
         # with `ld` -> `mcc elf ld` and a pkg-config that prints the -I and -l of DIR/linux/ext on the PATH

The binaries run in mvm on our kernel and libc. For that the C side of the compiler learned what
ufsm's C23 uses (`[[gnu::...]]` attributes, `constexpr`, `nullptr`, `typeof`, multi-dimensional variable length arrays that are given back at the end of their scope,
`thread_local`, atomics, `#pragma once`, `__builtin_*g` bit functions), and the Linux libc `rand()` gives
the GNU library's sequence, which tests with fixed seeds depend on.

### On a real GPU

`cuda/hw` is a second run time with the same interface to compiled code (`__mcuda_register`,
`__mcuda_launch`) that hands mcc's PTX to NVIDIA's driver: it opens `libcuda.so.1`, links the modules of the
units that share device functions or objects together with a library of device functions (`cuda/hw/dev`,
compiled by mcc to PTX), keeps the compiled result in `~/.cache/mcudahw` (`MCUDA_CACHE`), launches with
`cuLaunchKernel`, and forwards cublasLt to the real library. `MCUDA_VERBOSE=1` reports what is compiled and
every error, with `MCUDA_TRACE=1` every launch. The driver is a shared library of the GNU C library, so
programs for it are built in the compiler's glibc mode on x86-64 (below). What a GPU insists on and a CPU
does not, and the code generator therefore observes:

- a load or store must be aligned to its width: block copies and clears carry the alignment of their type
  and use nothing wider (a long one is a loop in place);
- memory of the host is not the device's: a structure too large for registers is passed to a kernel by
  value (`.param .align 8 .b8 p[n]`, copied into the kernel's frame), not as the address of the caller's
  copy as between host functions;
- `-use_fast_math` is honoured as nvcc does it: single precision operations flush to zero (`.ftz`), divide
  and take square roots with the approximate instructions.

ufsm (ARCH=120, sm_120a block-scaled tensor cores included) built this way on an RTX PRO 4000 Blackwell
trains, resumes and predicts: its `tests/test_pipeline_cli.py` passes, a prediction from a given checkpoint
is byte for byte that of the nvcc build, and with the input built on the host (`--gpu-input 0`) so are the
losses of the first training steps. Where it is not identical: the choice of which of two products a sum is
fused with follows nvcc in the cases the tests cover, not in all (one voxel in 4096 of an augmentation
kernel differs by one unit of a half float), and the device's mathematical functions are mcc's own, not
NVIDIA's. `tests/cuda/gpumcc.sh` runs the tests of `tests/cuda` this way.

**Speed of device code.** What a kernel costs on the GPU is decided by how many registers each thread needs
and how much of its state stays in memory, so device code is treated differently from host code:

- `#pragma unroll` (optionally with a count) is done in the parser: the tokens of the loop are kept and read
  once for each value of the counter, which is a constant while they are read; a loop whose condition is not
  constant then stays a loop. Short `for` loops that count to a constant are unrolled the same way unasked
  (at most 32 times, and 600 instructions as measured by reading the body once into blocks that are never
  run), as nvcc does;
- local arrays and structures that are only read and written at constant places are split into one local
  per place (`opt_split` in `src/opt_ssa.c`), which the ordinary promotion then turns into values; a place
  read as a float and written as an integer is converted with a move between register classes (`OP_BITCAST`);
- loop-invariant code is not moved out of loops (the GPU's assembler moves what is worth it; values hoisted
  out of a kernel's outer loop are alive through all of it and made the assembler spill kilobytes per thread);
- the address of a local or a global object is computed where it is read, and a constant added to an address
  is folded into the load or store;
- a kernel's parameter that is a structure arrives as doublewords that are values, put together in a local
  that is split like any other, so its members are registers and not loads from a copy on the stack; a load
  through a reference bound to one of several locals (`c ? a : b`) becomes the choice between what each
  holds (`select_loads`), since the chosen address would keep them all in memory;
- an address in shared memory (a `__shared__` object's plus or minus whole numbers) is worked out in 32 bits
  from the low halves of its indices and read with `ld.shared`/`st.shared`, as nvcc does: a 64-bit register
  costs a thread two, and the kernels that keep their tiles in shared memory needed four times as many;
- a 64-bit division whose operands both fit 32 bits is done as a 32-bit one (tested where it runs), and a
  branch around a few cheap instructions becomes a select more readily than in host code;
- a kernel's pointer parameters are addresses of global memory, and so is what is reached from them by adding
  whole numbers or choosing: loads, stores and atomic operations through them say `global` (an atomic add on
  memory of unknown kind is a test of the kind and a loop);
- a structure by value handed to a function that is put in line is copied to a local that is split, and a place
  wider than the alignment of the memory it is copied from is cut into whole numbers that can be read there;
- the 64-bit product of two 32-bit numbers (an index times a size) is one `mul.wide`, a choice of the smaller
  or larger of two numbers is `min`/`max`, a 32-bit division by a constant multiplies with `mul.hi`, and
  `a && b` on two cheap tests is one branch;
- sums are put in the order that lets them share: `c ? x + j : x + k` is `x + (c ? j - k : 0) + k`, a constant
  in a sum goes to the outside of it, and out of an `int` index of a pointer (`p[i + k]`: an `int` sum does not
  wrap), where it becomes part of the address;
- the pair of halves (`__half2`) is multiplied and added as the pair it is (`fma.rn.f16x2` and the like);
- pointers inside a kernel's structure parameters are of global memory too, and the inference starts at the
  pointer parameters, so that a number counted up from 0 is never taken for one; `__ldg` is the read of memory
  that nothing writes (`ld.global.nc`, of two and four numbers at once for the vector types), and a local
  object's memory is read and written as local (`ld.local`, `st.local`);
- a branch whose arms hold only arithmetic becomes selects also when the arithmetic is a call that is one PTX
  instruction (`fmaf` and the like) or is itself a select, so `c ? (g ? fma(...) : x) : 0` is two selects and
  no branch; `and`/`or` of comparisons stay in predicate registers (`and.pred`) and are not made numbers first;
- what is known about how large a number can be (`umax_of`: masks, shifts, small products) lets
  `(long)(x + k)` be `(long)x + k`, with `(x + k) * c` multiplied out, so addresses that differ by a constant
  share one sum and the constant is part of the address; sums are kept in one operand order, and the value
  numbering also numbers what its own folding creates;
- a whole number that is cheap to work out and is made in one block and read in another is worked out again
  where it is read, so that it holds no register in between: always when it is a value plus, masked by or shifted
  by a constant (`xrem_mark` in `src/ptx.c`), and when it is a small tree of sums, products, shifts and selects
  whose leaves are alive at every place it is read in any case (`xrem_live`: liveness of those leaves only).
  Once per stretch of code without a label. A kernel's per-thread constants (row offsets, addresses of its
  rows) otherwise each hold a register through the main loop, and the assembler spills: the producer loop of
  the weight-gradient kernel had 75 registers' worth of them against nvcc's 36 and was 23% slower for it.
  Addresses in shared memory are left alone. `MCC_NOXREM=1` turns all of this off, `=3` the trees, `=2`
  all but the widening of 32-bit numbers;
- a load through `c ? &a : &b` is always the choice between two loads, whatever else is done with `a` and `b`
  (the two are read where the load stood);
- a float rounded to a 16-bit float and then stored as one is rounded once.
- `extern template` is honoured, so a unit does not instantiate again the kernels another unit provides.

`MCC_NOUNROLL`, `MCC_NOAUTO`, `MCC_NOSPLIT`, `MCC_NOSH32`, `MCC_NOIFC`, `MCC_NOCHAIN`, `MCC_NOWIDE`, `MCC_NOMINMAX`,
`MCC_NOSELK`, `MCC_NOGLOBAL`, `MCC_NOLOCAL`, `MCC_NOIFCALL`, `MCC_NOPRED`, `MCC_NODIST`, `MCC_NOASMCO`, `MCC_LICM` (`=2`: all but what costs nothing in the loop) in the environment turn these
off (or the hoisting on) for experiments; `MCC_SLOTCHECK` checks after the passes that no instruction addresses a local that was
given up. `ptxas -v` on a `--ptx-out` file shows the spills per kernel without a GPU. On the pipeline fixture
(150 training steps, RTX PRO 4000 Blackwell) ufsm built by mcc trains at about 498 samples a second, nvcc's
build at about 555; without these changes it was 86. ufsm's own `bench_f4conv` times each convolution kernel
by itself and prints a hash of what it wrote, the same in both builds: the sum over all of it is 1.08 times
nvcc's at 64 voxels a side and 1.07 at 16 (the weight gradient that was 1.2 times slower is level). In the
training run the kernels, each timed between two synchronisations (`MCUDA_PROFILE=1`), add up to the same
time in both builds, and still the run is a tenth slower: with nvcc's PTX put in place of mcc's for one kernel
at a time (`MCUDA_SUBST`) four small kernels give back 1 to 1.5 percent each, the rest less.

On real data (the PHerc1299 scan at 9.6 um read over HTTP, the `wind96` checkpoint) the two builds predict
the same thing: of 157 million voxels of a 384 x 640 x 640 box 1591 differ, by one level of 255, in 5.4 s
against 5.0; and forty training steps on that scan against its published surface labels give the same
losses to four places. That training waits for its loader, and there mcc's host code is behind: reading
labels (a binary range decoder, one bit at a time) takes 1.7 times gcc's time, reading CT 2.7 times, since
the codec's AVX2 kernels are not used (`-DVOLCOMP_NO_AVX2`: mcc's `<immintrin.h>` is exact but element by
element), so the loader delivers about half the samples a second.

## x86-64 Linux

`--target=x86_64-linux-gnu` (a compiler that runs on x86-64 Linux targets it by default): `src/x64.c` selects
from the same IR into the same machine IR and register allocator as arm64, with its own encoder (loads folded
into operands, read-modify-write instructions, short branches); `src/x64_asm.h` is an AT&T-syntax assembler
(integer, SSE2 and system instructions) for `.s` files, top-level and inline assembly. Arguments follow the
System V classification, `va_list` included, so objects interoperate with gcc's. The Linux libc is the same
source with its own start-up, system calls and thread-local layout.

    make x64                               # the libc: DIR/linux-x64/{include,lib}
    export X64_SSH="ssh -p PORT user@host" # an x86-64 Linux machine to run on ("local" on one)
    make test-x64                          # tests/c in five variants, tests/thr, inline assembly, the assembler
                                           # against gas, the calling convention against gcc both ways
    make test-x64-self                     # mcc cross-built, then building itself there: stage 2 = stage 3,
                                           # and = the cross-built one; test-x64-pgo: also with a profile
    make ulibs-x64 test-ulibs-x64          # the third-party libraries and their tests
    make fuzz-x64 FUZZ_FIRST=1 FUZZ_COUNT=200   # random programs against the machine's C compiler

**With the GNU C library.** `--sysroot=DIR` (or `-mglibc`; the default of a compiler that was itself built
this way, `-mno-glibc` for the other) compiles against the headers under DIR/usr/include, with a few of the
compiler's own in front of them (`libc/glibc/include`: `long double` is `double` here, so the library's
long-double functions are redirected; C++ is shown the declarations written for an old GCC, without
overloads). The link is done by the machine's `cc -no-pie` for now, with `glibc/lib/libmcc.a` (what compiled
code calls unasked) added. `make test-x64-glibc`: mcc cross-built against the library builds itself there
to a fixed point and runs tests/c and tests/thr natively.

Not there: vector code and SSE intrinsics (the vectoriser is off), x87 and AVX, position-independent code and
dynamic linking by mcc's own linker (local-exec thread-local storage only), `long double` wider than `double` (so not
gcc's), link-time optimisation in the ELF linker (either Linux target). Its code runs mcc itself about 1.2
times slower than gcc -O2's.

## hostfs: a container root file system

`mkhostfs.sh MCC OUTDIR` (or `make hostfs`, into `build/hostfs`) makes a root file system for x86-64 Linux
containers in which every program is built by mcc from this tree against our own C library, statically:
`OUTDIR/rootfs`, the same as `OUTDIR/hostfs.tar` (the archive is written by mbox's `tar`, with owner root and
one fixed time, so the same tree gives the same bytes), and a `Dockerfile` (`FROM scratch`, `ADD hostfs.tar /`).
About 12 MB. Nothing in it comes from a distribution. Two things in it are not this tree's: the list of
certificate authorities, which is data, and ssh, which is Dropbear built here by mcc from its source (both below).

| path | what |
| --- | --- |
| `/usr/mbox` | the shell and the tools (`box/`); `init` and `tar` were added for this |
| `/usr/mcc` | the compiler, assembler, linker, binutils, make, lex, yacc |
| `/usr/bin` | every tool by name (links to the two programs); `gcc`, `ld`, `ar`, `nm`, `nvcc`, ... and `pkg-config` are two-line scripts; `/bin` and `/sbin` are links to it |
| `/usr/linux-x64` | the C library (`include`, `lib/crt1.o`, `lib/libc.a`) and the third-party libraries (`ext`) |
| `/usr/cuda` | `include`: what a `.cu` file includes; `lib64/libcudart.a`: the CUDA run time, which runs kernels on the GPU when NVIDIA's driver is there and on the CPU otherwise (with a `libcublasLt.a` and a `libstdc++.a` that hold nothing) |
| `/usr/dropbearmulti` | ssh: Dropbear's server, client, `dropbearkey` and `scp` (not ours, built here from source: below), and `/usr/bin/start-ssh` |
| `/etc/passwd`, `group`, `hosts` | root alone; `/tmp`, `/root`, `/proc`, `/sys`, `/dev` are empty |
| `/etc/ssl/certs/ca-certificates.crt` | the certificate authorities HTTPS trusts (libcurl's default file; `/etc/ssl/cert.pem` is a link to it). **The one file that is data from outside this tree**: a copy of `CACERTS=file` (PEM), by default the build machine's own `/etc/ssl/certs/ca-certificates.crt` (Linux) or `/etc/ssl/cert.pem` (macOS: 128 certificates, readable by anyone; the keychain's list, `security find-certificate -a -p /System/Library/Keychains/SystemRootCertificates.keychain > roots.pem`, also needs no rights and can be given as `CACERTS=roots.pem`). `CACERTS=none` leaves it out. Nothing is downloaded; `mkhostfs.sh` prints the file's SHA-256, and the same file gives the same archive |

`init [COMMAND...]` is the image's entry point: it runs the command (default `/bin/sh`), waits for every
orphan a process 1 inherits, passes TERM, INT, HUP and QUIT on to the command and leaves with its status.
`sh` at a terminal prompts and runs each command when it is whole (no history, no job control).

    make hostfs                             # build/hostfs/{rootfs,hostfs.tar,Dockerfile}
    docker import build/hostfs/hostfs.tar mcc-hostfs     # or: docker build -t mcc-hostfs build/hostfs
    X64_SSH="ssh user@host" X64_DIR=dir make test-hostfs [UFSM=path/to/ufsm] [GPU=1 [UFSM_GPU_TESTS=test_nn]]

`tests/hostfs.sh` needs no Docker and no root: on the x86-64 machine it unpacks the archive, checks with that
machine's `readelf` that every ELF file in it is a static executable or a relocatable object (no interpreter,
no dynamic section), and runs `tests/hostfs/smoke.sh` inside it through bubblewrap (`bwrap --bind rootfs /
--unshare-all --as-pid-1`), started by the image's `init`: the tools, a C program, one with threads, make with
`ar` and `ld -r`, a program linking every third-party library through `pkg-config`, a CUDA unit compiled,
linked with `-lcudart` and its kernel run, and mcc building itself to the bytes of the installed `/usr/mcc`. With `UFSM=dir`, ufsm's tests
that need no GPU are built there by ufsm's own Makefile and run (`tests/hostfs/ufsm.sh`).

mcuda on x86-64 (`make x64 cuda` builds it into `DIR/linux-x64/cuda`; `make test-x64-cuda` runs the tests of
`tests/cuda.sh` on the x86-64 machine) is the interpreter alone: what is assembly on arm64 is C over the C
library there (`cuda/fp_c.c`), the sums of `mma` are the plain C ones, and there is no translator to machine
code, so kernels run slowly.

### The GPU from a static program

The image has no GNU C library and no `ld.so`, and NVIDIA's driver library `libcuda.so.1` is a shared object
made for both. Three pieces of ours stand in, and nothing of NVIDIA's is in the image: the container run time
brings the driver's libraries and device nodes, as it does for any image.

- **`dlopen` in the C library** (x86-64 Linux, `libc/linux/src/dl.c`): maps a shared object into the static
  program, relocates it, runs its initialisers, and answers `dlsym`. What the object imports from `libc.so.6`,
  `libm`, `libdl`, `libpthread`, `librt` is our library itself, by name from a table (`glibctab.c`, about 270
  names: what `libcuda.so.1` and `libnvidia-ptxjitcompiler.so.1` of driver 615 import or ask for), with the GNU
  library's spellings and layouts where they differ (`glibcabi.c`: `__xstat`, `__isoc99_sscanf`, `sigaction`,
  `strerror_r`, `posix_spawn`, `nftw`, `shm_open`, ...). It is not a general dynamic linker: an object with
  thread-local storage is refused with a message (so NVIDIA's `libnvidia-nvvm`, `libcublasLt`, `libnvidia-ml`
  do not load, and are not needed), symbol versions are ignored, nothing is unloaded. A missing name shows in
  `dlerror` (and with `MCC_DLDEBUG=1`, which traces every request) and is a line to add to the table.
  Objects are looked for in `LD_LIBRARY_PATH`, then `/usr/lib/x86_64-linux-gnu`, `/usr/lib64`, `/lib64`,
  `/usr/lib`, `/usr/local/nvidia/lib64` ... (there is no `ld.so.cache`).
  `malloc` now returns blocks aligned to 16 bytes, which the driver's code (like any compiled for the ABI)
  takes for granted.
- **mcudahw on our C library** (`cuda/hw`, the run time on the driver's API that was built with glibc before):
  `cuInit`, the PTX that mcc wrote given to `cuModuleLoadData` (the driver's compiler is
  `libnvidia-ptxjitcompiler.so.1`, loaded the same way), launches, memory, streams. Compiled kernels are kept
  in `~/.cache/mcudahw` (`MCUDA_CACHE`).
  `cublasLtMatmul` is a kernel of our own (`cuda/hw/ltk.cu`: 16 x 16 tiles through shared memory), not
  NVIDIA's cuBLAS: it supports what the CPU's does (`cuda/include/cublasLt.h`) and rounds the same way, so
  the two give the same bits (`tests/hostfs/ltcmp.cu`: 20 products of fp32, fp16 and bf16 matrices,
  transposed, batched, with bias and relu). It is a plain kernel, far from cuBLAS in speed on large matrices.
- **One `libcudart.a` with both run times** (`cuda/mux`): every global name of the CPU's run time gets `mcpu_`
  before it and of the GPU's `mgpu_` (`symprefix.c` rewrites the objects' symbol tables), and each public
  function (`cuda*`, `cublasLt*`, `cu*`, `__mcuda_*`) is two instructions that jump through a slot. At the
  first call the GPU's run time is taken if `libcuda.so.1` loads and `cuInit` finds a device, the CPU's
  otherwise; `MCUDA_BACKEND=cpu` or `gpu` decides instead, `MCUDA_VERBOSE=1` says which it is. A program that
  never calls CUDA loads nothing. The driver API by name (`cuMemAlloc`, ...) is the CPU run time's alone;
  on the GPU a program gets the driver's functions from `cudaGetDriverEntryPointByVersion`, as ufsm does.

`GPU=1` makes `tests/hostfs.sh` bind the machine's two driver libraries (read-only) and `/dev/nvidia*` into the
image the way NVIDIA's container run time does and run `tests/hostfs/gpu.sh` there: `cuInit` and the devices'
names through `dlopen`, a kernel on the GPU and the same program on the CPU, the cublasLt comparison, and
with `UFSM_GPU_TESTS` tests of ufsm. On two RTX 3090 (driver 615.71, under bubblewrap) all 63 checks pass
(with ufsm's and ssh's);
ufsm's `test_nn` takes 5 s on the GPU (498 s on the CPU's run time). `GPU=1 make test-x64-cuda` runs the
programs of `tests/cuda` on the GPU: 41 pass; `mx` (for sm_120a) and `ldg` (it reads a `uint2` at an address
that is a multiple of 4 but not of 8, which `ld.global.nc.v2.u32` does not take on a real device) are left out.

### ssh

The image's ssh is **Dropbear, which is not ours** (Matt Johnston's, MIT licence): the second thing in the
image, after the certificate authorities, that this tree did not write. It is built from its source by mcc
against our C library, statically, like everything else (`mkssh.sh MCC DIR`; `make hostfs` runs it when
`build/ext/dropbear-2026.90` is there): `/usr/dropbearmulti`, one program that is `dropbear` (the server),
`dbclient` / `ssh` (the client), `dropbearkey` and `scp`. Its `configure` and makefiles are not run:
`libc/ext/dropbear/config.h` is what configure would find, written by hand, and `localoptions.h` what the
image wants. For it the C library got the users and groups of `/etc/passwd` and `/etc/group` (`getpwnam`,
`getgrnam`, `initgroups`, ...: `libc/linux/src/pwgrp.c`), which the headers declared and nothing defined.

The release is fetched once, by hand, and checked before it is unpacked:

    curl -O https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.90.tar.bz2      # into an empty directory
    curl -O https://matt.ucc.asn.au/dropbear/releases/SHA256SUM.asc
    sha256sum dropbear-2026.90.tar.bz2   # 16be820347723271b0fea6049ffeed6d6680d7429c65406d8af37776393a0250, as SHA256SUM.asc lists it
    gpg --verify SHA256SUM.asc           # good signature of F734 7EF2 EE2E 07A2 6762 8CA9 4493 1494 F29C 6773
                                         # ("Dropbear SSH Release Signing"; the key is releases/dropbear-key-2015.asc)
    tar -xjf dropbear-2026.90.tar.bz2 -C build/ext

Keys only: the server takes no passwords and the client offers none (both are compiled out). No key is in
the image. `start-ssh [PORT]` starts the server for root: it adds the public keys in `$PUBLIC_KEY`,
`$SSH_PUBLIC_KEY` or `$AUTHORIZED_KEYS` to `/root/.ssh/authorized_keys` (which may also be mounted), refuses
to start without one, takes the host's keys from `/etc/dropbear` if any are mounted there and otherwise makes
an ed25519 one at this first start, and returns; so a container that should be reachable runs
`start-ssh && exec its-command`. (`PUBLIC_KEY` is the name RunPod's own images read, from memory: not checked.)

`tests/hostfs/ssh.sh` runs in a second bubblewrap container whose network is its own and empty, so the server
listens on a loopback nobody else can reach: a key pair is made, the server started by `start-ssh`, and the
client logs in by `dbclient` and by `ssh`, runs a command, gets its status, copies a file with `scp`; another
key is refused and no password is asked for. It runs as a user that is not root, because in a user namespace
made without privilege nobody may set groups, which the server does for root before starting root's shell:
root's login is for Docker to show. A session with a terminal was tried by hand (`dbclient -t`: `/dev/pts/0`).

Not in it: time zones (the library knows UTC), bash and python (ufsm's suite runner and some of its tests),
sftp. Not tried: Docker itself and NVIDIA's container run time (the checks above are under bubblewrap, with
the libraries and devices bound by hand), RunPod.

## Known gaps

- Structures are always passed and returned by reference. This differs from
  the platform ABI for structures of 16 bytes or less, so such calls cannot
  be mixed with clang-compiled code. Narrow stack arguments use 4-byte slots.
- Programs load libSystem (the kernel requires it) but import nothing: libc
  makes raw system calls, which Apple does not promise to keep stable.
- No debug information, no dynamic linking. Thread-local storage and threads
  only for Linux programs.
- Diagnostics stop at the first error.
- Optimiser: no division by constants other than powers of two, no scalar
  replacement of aggregates, no induction-variable strength reduction, no
  interval splitting in the register allocator (a spilled value lives in
  memory for its whole life; about 1% of values, nearly all in a handful of
  very large functions). Memory forwarding stops at loops and at joins
  other than a plain if/else whose sides store nothing. A branch only
  teaches the optimiser that a value is zero or non-zero (so a repeated
  test folds); it learns no ranges.
- `signal` handlers and `system` are stubs; time functions are UTC only;
  math functions are accurate to a few ulp, not correctly rounded.
- One target so far (arm64 macOS).
