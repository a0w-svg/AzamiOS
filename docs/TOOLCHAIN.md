# Building on AzamiOS

AzamiOS carries a C compiler and can build and run C programs with nothing
mounted from outside. The compiler is [TinyCC](https://repo.or.cz/tinycc.git),
built from upstream source by [tools/linux/ports.mk](../tools/linux/ports.mk)
and configured for this filesystem's layout.

```
/ # cc --version
tcc version 0.9.28rc 0fb54300b56512754221d80adda85ddb9815bceb (x86_64 Linux)

/ # tcc -static /examples/hello.c -o /tmp/hello

/ # /tmp/hello
========================================
 Hello from a C program compiled on AzamiOS!
========================================
Program arguments count: 1
  argv[0] = /tmp/hello
```

`touch /etc/run-toolchain-selftest` before packing the image and init runs
exactly that at boot — compile, link, execute, then the separate
compile-then-link path — and prints pass/fail on the serial console.

---

## What replaced what

The image used to ship the whole `x86_64-elf` cross-GCC: `gcc`, `cc1`,
`collect2`, `as`, `ld`, the rest of binutils, GCC's internal headers, the
linker scripts, and a farm of symlinks recreating
`/home/<developer>/opt/cross-x86_64` *inside the rootfs* so that GCC could
find its own guts at the absolute paths it had been configured with.

|                  | packed cross-GCC | ported TinyCC |
|------------------|------------------|---------------|
| `/usr/bin`       | 30 MB            | 0.4 MB        |
| `/usr/libexec`   | 47 MB            | —             |
| `/usr/lib/gcc`   | 7.2 MB           | —             |
| `/usr/lib/tcc`   | —                | 0.25 MB       |
| `/usr/include`   | 3.5 MB (125 GCC headers) | libc's own headers only |
| built for        | a Linux host     | source built here, runs under the Linux-ABI layer |

Everything a compiler needs to *target* this system stayed: the libc and
libgame headers under `/usr/include`, `libc.a`/`libm.a`/`libgame.a` and the
startup object under `/usr/lib` and `/lib`.

## How the compiler is set up

TinyCC compiles, assembles and links in one pass — there is no separate `as`
or `ld` to install or to get wrong. It is configured (see the `tcc` rule in
`ports.mk`) to look where AzamiOS actually keeps things:

```
install:    /usr/lib/tcc
include:    /usr/lib/tcc/include   ← the compiler's own <stdarg.h>, <stddef.h>, <float.h>
            /usr/include           ← this libc's headers
            /usr/local/include
libraries:  /usr/lib/tcc           ← libtcc1.a, the compiler runtime
            /usr/lib  /lib  /usr/local/lib
crt:        /usr/lib  /lib
```

Two details make a plain `tcc hello.c` work:

- **`crt1.o`, `crti.o`, `crtn.o`.** Every compiler driver links these unless
  told `-nostdlib`. AzamiOS's startup object is `crt0.o`, so `crt1.o` is that
  same object under the name the driver expects; this libc has no
  `.init`/`.fini` fragments at all (`crt0.asm` calls `__init_tls` and
  `__libc_init` directly and never `_init`), so `crti.o` and `crtn.o` are
  empty objects that satisfy the link and contribute nothing. They are
  created by the `toolchain` target in [userland/Makefile](../userland/Makefile).

- **The compiler's own headers come first.** This libc deliberately ships no
  `<stdarg.h>` or `<stddef.h>` — those belong to the compiler, and GCC's
  copies (which the old image staged into `/usr/include`) are written against
  `__builtin_va_list`, which TinyCC does not have.

## `-static` is not optional

Every native binary on the image is statically linked against
`/usr/lib/libc.a`. TinyCC's default output is a *dynamic* executable with a
`PT_INTERP`, so link with `-static`:

```
tcc -static prog.c -o prog          # a working native binary
tcc -c prog.c -o prog.o             # object files need no -static
tcc -static a.o b.o -o prog         # link step does
```

## Multi-file builds

`pkg install make` adds GNU Make, and an ordinary makefile drives TinyCC the
way it would any other compiler:

```make
CC      = tcc
CFLAGS  = -O2 -Wall
LDFLAGS = -static

prog: main.o util.o
	$(CC) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
```

`pkg install azami-examples` puts a few small C programs under
`/usr/share/examples` to try this on.

## If the image has no compiler

`/usr/bin/tcc` is staged only when `make -C tools/linux ports` has built it
(see [PACKAGES.md](PACKAGES.md)); otherwise the image is built without a
compiler and says so:

```
·  No on-device compiler staged (build one with: make -C tools/linux ports)
```

Either way `pkg install tcc` installs the identical files at runtime — the
package and the image staging come from the same build.

## Why a development snapshot, not 0.9.27

TinyCC's last release is 0.9.27 (2017), and it cannot link against this libc
at all: `errno` is `__thread`, and 0.9.27 stops with

```
tcc: error: Unknown relocation type: 23
```

on `R_X86_64_TPOFF32`, the thread-local relocation every object that touches
`errno` carries. Upstream has handled the TLS relocations for years, so the
port pins a commit of the development branch (`TCC_COMMIT` in `ports.mk`)
rather than tracking a branch head, so the build stays reproducible.

## Limits

- C only. TinyCC has no C++ front end; nothing on the image compiles C++.
- No LTO, no `-march=` tuning, far less optimisation than GCC. TinyCC is
  fast and small, not an optimiser.
- Some GCC extensions are unsupported — most visibly the `<*intrin.h>`
  x86 intrinsics headers, which are GCC's own and are no longer on the image.
- The kernel and the native userland are still cross-compiled on a build
  host by `make`; this is a compiler *on* the OS, not a self-hosting build of
  the OS itself.
