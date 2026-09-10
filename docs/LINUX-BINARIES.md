# Running Stock Linux Binaries on AzamiOS

AzamiOS implements the Linux `x86_64` system call ABI: the same call numbers,
the same argument registers, the same `struct` layouts, and the same initial
process image. A program compiled by an ordinary Linux toolchain therefore runs
here **unmodified** — not recompiled against AzamiOS's libc, not relinked, not
patched. The binaries this document produces are byte-for-byte what the same
commands would produce for any Linux machine.

```
$ make            # kernel
$ make linux      # musl + BusyBox, built by a real Linux toolchain
$ make iso        # the binaries land in the initrd automatically
$ make run
```

Then, from the AzamiOS shell:

```
/ # busybox sh
~ $ ls -l /etc | head
~ $ seq 1 10 | awk '{s+=$1} END{print s}'
55
```

---

## What actually makes this work

A Linux binary does far more before `main()` than most people expect, and each
step is a place the host kernel has to agree with it exactly.

| Stage | What the binary needs from the kernel |
| :--- | :--- |
| Load | `PT_LOAD` segments mapped at their `p_vaddr`, `ET_DYN` images slid to a load bias |
| Auxv | `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP` on the initial stack |
| TLS | `arch_prctl(ARCH_SET_FS)`, plus a `PT_TLS`-shaped image the libc builds itself |
| Threads | `clone(CLONE_VM\|CLONE_THREAD\|CLONE_SETTLS\|CLONE_CHILD_CLEARTID)` and `futex` |
| Exit | the kernel zeroing and waking the `CLONE_CHILD_CLEARTID` word |
| SIMD | `CR4.OSFXSR` / `OSXSAVE` set and per-thread XSAVE state on context switch |

The first three happen before a single instruction of `main` runs, which is why
a missing piece usually shows up as a page fault at an address near zero rather
than as an error message.

---

## The toolchain

`make linux` builds everything under `tools/linux/`:

```
tools/linux/
├── Makefile          the pipeline
├── dl/               downloaded tarballs
├── src/              unpacked sources
├── musl/             musl 1.2.5, installed — musl/bin/musl-gcc is the compiler
├── out/bin/          busybox, azami-abi-probe
├── log/              build logs
└── tests/abi-probe.c the ABI conformance probe
```

`musl-gcc` is a wrapper around the host's `gcc` that substitutes musl's headers
and startup files. It is an ordinary Linux compiler — nothing in it knows about
AzamiOS.

> `musl-gcc` records its own prefix at configure time. Moving `tools/linux/`
> after building breaks the wrapper; run `make linux-clean && make linux`
> instead of relocating it.

### Building your own program

```sh
tools/linux/musl/bin/musl-gcc -static -O2 -o myprog myprog.c
cp myprog userland/build/usr/bin/
make iso && make run
```

**Link statically.** A dynamic binary needs `ld-linux-x86-64.so.2` and the
shared libc present on the image; the ELF loader does honour `PT_INTERP` and
will load an interpreter, but the static case is the one that is exercised and
tested here.

### Why musl rather than glibc

musl's startup path is small and its syscall use is close to the minimum a
program can get away with, which makes it the right first target for a kernel
growing into the ABI. glibc reaches for more (`rseq`, several `prctl` options,
richer `vDSO` use) and is a reasonable next step, not a starting point.

---

## Testing

```sh
make linux-test                            # run the ABI probe as PID 1
scripts/linux-test.sh path/to/static-binary # run any static Linux binary as PID 1
```

`scripts/linux-test.sh` builds a throwaway initrd containing just the binary
under test (plus BusyBox if it has been built), boots it headless under QEMU
with the binary as PID 1, and prints what it wrote to the serial console.

`tools/linux/tests/abi-probe.c` is a plain Linux C program — it compiles and
passes on a real Linux box too, which is the point: any check it fails on
AzamiOS is a genuine divergence from Linux, not a quirk of the test.

It covers process identity, `uname`, `malloc`/`mmap`/`mprotect`, file I/O,
directories, stdio buffering, clocks, signal delivery, pthreads, `fork`/`exec`,
and hard/symbolic links.

### When something does not work

The kernel logs the first use of every syscall it has no handler for:

```
[SYSCALL] unimplemented syscall 334 from 'myprog' (PID 4) at RIP=0x000000000040a1b2
```

`scripts/linux-test.sh` collects those lines and prints them after the guest
output. That message is nearly always the whole diagnosis — implement the call
in `kernel/syscall/syscall.c` and register it in `syscall_init()`.

---

## Layout on the image

BusyBox installs as `/bin/busybox` with its applet symlinks under `/usr/bin`
and `/usr/sbin`. AzamiOS's own utilities live in `/bin` and `/sbin` under `.elf`
names, and the shell's `PATH` (`/bin:/sbin:/usr/bin:/usr/sbin`) searches those
first, so the two userlands coexist without shadowing each other: native tools
keep priority, BusyBox supplies everything AzamiOS does not ship, and
`busybox sh` gives a complete Linux shell regardless.

To put the applets in `/bin` and `/sbin` instead, edit the `install` target in
`tools/linux/Makefile`.

---

## Known gaps

- **`st_rdev` is always 0.** devfs does not assign major/minor numbers, so
  `ls -l /dev` shows `0, 0` for every node and anything that inspects device
  numbers sees nothing useful.
- **Dynamic linking is untested.** `PT_INTERP` is honoured by the loader but no
  dynamic binary has been run end to end.
- **No vDSO.** `AT_SYSINFO_EHDR` is not supplied, so `clock_gettime` and friends
  take the syscall path. Correct, just slower than Linux.
- **glibc is unexercised.** Only musl-linked binaries have been run.
