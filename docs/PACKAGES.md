# Package Management

`pkg.elf` installs, removes, and lists software from a package repository —
one tool for both AzamiOS's own native apps and stock Linux binaries, since
both run under this kernel's Linux-ABI syscall layer (see
[LINUX-BINARIES.md](LINUX-BINARIES.md)).

```
/ # pkg list --available
cowsay           1.0.0      native   Prints a cow saying whatever you tell it
busybox          1.36.1     linux    Swiss-army-knife of Linux utilities (ash, wget, grep, ...)

/ # pkg install cowsay
pkg: fetching cowsay-1.0.0.tar (cowsay 1.0.0)...
pkg: installing cowsay 1.0.0...
pkg: installed cowsay 1.0.0 — Prints a cow saying whatever you tell it

/ # cowsay.elf AzamiOS package manager works!
 ________________________________
< AzamiOS package manager works! >
 --------------------------------
        \   ^__^
         \  (oo)\_______
            (__)\       )\/\
                ||----w |
                ||     ||

/ # pkg remove cowsay
pkg: removed bin/cowsay.elf
pkg: removed cowsay 1.0.0
```

---

## Packages

A package is a plain [ustar](https://en.wikipedia.org/wiki/Tar_(computing))
archive — the same format `tar.elf` reads and writes — whose first entry is
named `PKGINFO`, a small `key=value` manifest:

```
name=cowsay
version=1.0.0
type=native
description=Prints a cow saying whatever you tell it
```

Every other entry is a file to install, with its path relative to the root
filesystem (`bin/cowsay.elf`, not `/bin/cowsay.elf`). `type` is `native` for
a binary built by AzamiOS's own cross-toolchain or `linux` for a stock Linux
(musl-linked) binary — `pkg.elf` treats them identically; it only shows the
distinction back to you in `pkg list`.

## Repositories

A repository is a directory — local (`file:///path`) or served over plain
HTTP (`http://host[:port]/path`) — holding an `index.txt`:

```
# name version type file description
cowsay 1.0.0 native cowsay-1.0.0.tar Prints a cow saying whatever you tell it
```

and the package archives it names. `/etc/pkg/repos.conf` lists one
repository URL per line; `pkg install`/`pkg list --available` check each in
order. The disk image ships with one local repository already configured:

```
file:///repo
```

built by `scripts/generate_pkg_repo.py` from whatever `cowsay.elf` (this
OS's own toolchain) and `tools/linux/out/bin/busybox` (`make linux`) happen
to be built at image-build time — see the `pkgrepo` target in
`userland/Makefile`. Add an `http://` line to point at a repository served
over the network instead; nothing about the package format or `pkg.elf`'s
commands changes.

## Installed-package state

`/var/pkg/db/<name>` records what `pkg install` put where — the manifest,
then a `FILES:` line, then one installed path per line. `pkg remove` deletes
exactly those files and nothing else; `pkg list` (no `--available`) reads
this directory to show what's actually installed, independent of whatever a
repository currently offers.

Fetched archives are cached under `/var/pkg/cache/`.

## Commands

```
pkg install <name>          # fetch + install; no-op if already installed
pkg remove <name>            # delete every file it installed
pkg list                     # what's installed
pkg list --available         # what every configured repository offers
```

There is no dependency resolution or upgrade command yet — a package is a
flat list of files, and installing one that's already present just
overwrites those files in place.

## `http://` repositories

A `pkg install` needs two HTTP fetches — the repository's index, then the
package archive — and for a while, only the *first* TCP connection a boot
session made reliably completed; a second one could hang the whole machine.
The cause was a kernel bug in `tcp_connect()`/`tcp_input()` (see
[kernel/net/tcp.c](../kernel/net/tcp.c)'s locking comment), not anything
specific to `pkg.elf`, and is now fixed: `pkg list --available`, `pkg
install`, and further fetches all complete normally over `http://` within
one boot session (verified end-to-end against a real HTTP server: index
fetch, package fetch, and a follow-up `pkg list` in sequence). `file://`
repositories were never affected — they involve no sockets — and remain
what the bundled sample repository uses by default.
