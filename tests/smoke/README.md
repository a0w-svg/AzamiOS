# BusyBox and dynamic ELF smoke tests (W3.7)

Run from the repository root:

```sh
./tests/smoke/busybox_smoke.sh
```

The runner builds the kernel, stock static BusyBox, upstream shared musl, and
native libc. It boots an ephemeral ext2 initrd in headless QEMU, choosing KVM
when available and TCG otherwise. It never attaches or rebuilds `hdd.img`.
The shared musl build stays under `tools/linux`; it does not install a loader
into the host's `/lib` or change the static BusyBox build.
After building shared musl, `make -C tools/linux install` also stages
`/lib/ld-musl-x86_64.so.1` for regular images, alongside native `libc.so`.

A static musl PID 1 supervises three tests and checks each child's actual
`waitpid` exit status:

- BusyBox `sh`, `ls`, `ps`, `cat`, `mkdir`, `rm`, `free`, `top`, `uname`, and
  `env`, including `sh -c 'ls -la / && free && ps'`, file contents, removal,
  and environment inheritance. `top -b -n 1` produces one finite snapshot.
- A native PIE using `/lib/ld-azami.so` and native `libc.so`.
- A stock musl PIE using `/lib/ld-musl-x86_64.so.1`.

Both dynamic probes verify arguments and environment, mapped program headers
and interpreter, the executable entry, `AT_RANDOM`, `AT_SECURE`,
`AT_SYSINFO_EHDR`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, and `AT_PAGESZ`.
They exercise `DT_NEEDED`, startup TLS, pointer relocations, constructors,
`dlopen`, `dlsym`, `dlclose`, and errors for missing libraries and symbols.
Native fixtures include both SysV and GNU hash tables; the loader and its
libc bridge are also built with the host linker's default hash style.

The runner exits nonzero if any child fails or the VM produces no final
verdict. Logs and disposable boot images remain in `build/smoke/vm`.
Set `SMOKE_TIMEOUT=120` for a slower host. To run the bounded GNU hash parser's
host tests alone, use `make -C tests/smoke check`.

The separate regression gates are:

```sh
make -C tools/linux probe
./scripts/linux-test.sh
./tests/boot/matrix.sh
```

The ABI harness now builds a hybrid BIOS/UEFI ISO. The matrix runs that same
ABI payload under both firmware modes, so UEFI requires a completed probe
instead of accepting an early kernel initialization message. Six TCG cases
are available without KVM; four additional cases require accessible `/dev/kvm`.
