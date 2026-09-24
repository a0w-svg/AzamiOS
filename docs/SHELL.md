# The system shell

`/bin/sh` on AzamiOS is **GNU Bash 5.2.21**, built from upstream source by
[tools/linux/ports.mk](../tools/linux/ports.mk) and running under the
kernel's Linux-ABI layer like every other port.

```
/ # /bin/sh --version
GNU bash, version 5.2.21(1)-release (x86_64-pc-linux-musl)

/ # ls -l /bin/sh /bin/sh.elf /bin/bash /bin/azami-sh.elf
/bin/sh          -> bash
/bin/sh.elf      -> bash
/bin/bash        (1.1 MB, the real binary)
/bin/azami-sh.elf (46 KB, the original native shell)
```

`/bin/sh` and `/bin/sh.elf` both point at it, so everything that already
spawned a shell by name gets bash without knowing: libc's `system()` and
`popen()`, the terminal's fallback path, a `#!/bin/sh` script, the kernel's
emergency init.

---

## What this replaced

The original shell — [userland/apps/sh/main.c](../userland/apps/sh/main.c),
~1150 lines, pipes and redirection and a builtin set — is still built and
still installed, as **`/bin/azami-sh.elf`**. It is what the image falls back
to when `tools/linux/ports.mk` has not been built: an image with no shell at
all would be a brick, and that is not a failure worth risking to save 200 KB.
The build says which one it installed:

```
✓  System shell: GNU bash (1180848 bytes) -> /bin/{sh,sh.elf,bash}
·  System shell: native azami-sh (bash not built — make -C tools/linux ports)
```

What you get from the swap is the rest of the language: functions,
`case`/`while`/`until`, arrays, parameter expansion, command substitution,
`trap`, here-documents, job control where a terminal supports it, and the
hundreds of scripts written against it that now simply run.

## Startup files

Two shells read these, so they are split by what each can parse.

| file | read by | holds |
|---|---|---|
| `/etc/profile`, `~/.profile` | both shells | plain `export K=V` lines: PATH, USER, HOME, TERM, SHELL, TMPDIR, PAGER, EDITOR |
| `~/.bashrc` | bash, interactive | `PS1`, history settings |
| `~/.bash_profile` | bash, login | sources `/etc/profile` then `~/.bashrc` |

The plain files stay plain on purpose. The native shell's parser splits on
spaces, so a line like

```sh
export PS1="root@azamios:\w\$ "
```

came back at it as `PS1="root@azamios:\w\$: command not found` — three times
per startup, once per file it sourced. Anything with quoting, conditionals
or `.`-sourcing therefore lives in the two bash-only files.

One consequence worth knowing: invoked as `sh` (a login `sh -l`), bash
mimics sh and reads `/etc/profile` and `~/.profile` only — not
`~/.bash_profile` — so that shell has the environment but no prompt. Logins
through `/etc/passwd` run `/bin/bash`, which reads `~/.bash_profile` and gets
both.

## Where the shell is named

- `/etc/passwd` — both accounts' login shell is `/bin/bash`
- `/etc/shells` — lists bash first, `azami-sh` last
- libc: `_PATH_BSHELL`, `system()`, `popen()`, `getenv("SHELL")` → `/bin/sh`
- kernel: the default `SHELL=` in the initial environment → `/bin/sh`;
  the no-init fallback tries `/bin/sh`, then `/bin/azami-sh.elf`
- [terminal.elf](../userland/apps/terminal/main.c) — parses a plain command
  itself and hands anything else to `/bin/sh -c`, which is how pipes,
  redirection and quoting work there

## Limits to know

- **Job control needs a terminal.** On a pipe or a non-tty, bash prints
  `cannot set terminal process group` / `no job control in this shell` and
  runs on. That is ordinary bash behaviour, not an AzamiOS limitation.
- **No readline.** The port is built `--disable-readline --disable-history`:
  no line editing, no arrow-key history, no tab completion at the prompt.
  Scripting is unaffected. (Building readline in would mean linking a
  terminfo-driven library statically for a console that does not need it.)
- **One argument cannot exceed 1023 bytes.** The kernel copies each `argv`
  string into a 1 KB buffer
  ([kernel/syscall/syscall.c](../kernel/syscall/syscall.c), `execve_core`)
  and silently truncates past it, so a `sh -c '<very long script>'` loses
  its tail. The whole `argv`+`envp` block also has to fit the single page
  the initial stack is built in
  ([kernel/sched/elf.c](../kernel/sched/elf.c)), and an exec that overflows
  it fails rather than truncating. Linux allows 128 KB per string; raising
  this means giving the initial stack more than one page.
