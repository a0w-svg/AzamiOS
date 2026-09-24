# The desktop

AzamiOS's desktop is five cooperating programs talking the `azwm` protocol
([protocol.h](../userland/apps/azwm/protocol.h),
[de_protocol.h](../userland/apps/azwm/de_protocol.h)):

| process | role |
|---|---|
| `azwm.elf` | compositor and window manager — input, stacking, damage, cursor |
| `sessiond.elf` | starts the session and the services below |
| `wallpaper.elf` | root window: background, desktop icons, icon context menu |
| `taskbar.elf` | panel: Start button, quick-launch dock, window list, system tray |
| `launcher.elf` | the application grid, opened from the Start button |

Everything else — terminal, files, settings, editor, calculator, games — is
an ordinary client of the same protocol.

---

## What the controls do

Every control described here performs the action it names. Where the
hardware or kernel cannot do the thing a control used to claim, the control
says what it actually does instead (see **Display blanking** below).

### Taskbar

| control | behaviour |
|---|---|
| **Apps** | opens the launcher; clicking again closes it |
| dock icons | launch that application (`/etc/taskbar_dock.conf`) |
| window buttons | focus the window, or minimise it if it is already focused |
| network icon | opens Settings; the tooltip shows the live `net0` address |
| speaker | cycles 25 % → 100 % → mute, applying it to `/dev/dsp` and saving it to `/etc/audio.conf` |
| lock icon | runs `/sbin/lockscreen.elf` |
| CPU/RAM widget | opens the system monitor |
| clock | opens the clock and calendar |

Window buttons share the strip between the dock and the tray, shrinking
from 140px down to 64px as windows are added, so a fourth and fifth window
get a button instead of an overflow badge.

Tooltips and toasts float in a transparent strip **above** the panel rather
than inside it, so they never cover the icon they describe, and they clear
when the pointer leaves the panel.

### Launcher

Shows the applications registered in `/etc/launcher.conf` — an allowlist,
so the ~200 command-line tools in `/bin` do not appear as entries that open
no window. Typing filters; Enter launches the highlighted entry; Escape
dismisses. It closes as soon as it launches something. The footer reports
real disk usage from `statvfs()`.

### Desktop (right-click)

New Text Note, New Folder, Refresh, Auto-Arrange, and shortcuts to the
terminal, file manager and editor. Each acts on the real filesystem under
`/home/azami/Desktop`.

### Settings

Display, Audio, Theme, Time, Network, Power, Disks, Security and System.
Each control writes through to the thing it names: sysctls via `/proc/sys`,
volume via the `/dev/dsp` ioctl, addresses via `SIOCSIFADDR`, themes and
timezones to their config files, `Restart`/`Power Off` via `reboot(2)`.

### Display blanking

`Settings > Power > Blank Display` blanks the screen immediately, and the
**Screen timeout** row (5 / 15 / 30 / Never) blanks it after that long
without input. Any key or mouse event wakes it; input is ignored for 600 ms
after blanking so the release of the click that asked for it does not wake
it again.

This is display blanking, not suspend. The kernel implements ACPI S5 (soft
off) and no sleep states, so there is no S3 to enter — the button used to
be labelled "Sleep / Standby" and print "Entering ACPI S3 Standby state..."
while doing nothing at all.

## Windows whose client is gone

A client should send `AZ_WM_DESTROY_WINDOW` before exiting. When one does
not — it crashed, it was killed from the terminal, it just called `exit()`
— the compositor notices within a second and removes the window, telling
the taskbar so the button disappears too.

Liveness is decided by the client's IPC channel, not by its PID. The kernel
tears down a process's channels when it exits, so a send to a dead client's
channel fails with `EPIPE` (closed) or `EINVAL` (already unregistered) —
immediately, and without depending on anything else having run first.

A PID is a worse signal: `kill(pid, 0)` succeeds for a zombie, which is
correct (POSIX keeps a zombie a process until it is waited for), so a
window gated on it would linger until somebody reaped its owner. That used
to be never — init's idle loop only slept despite being named the zombie
reaper, and procfs kept `/proc/<pid>` alive as a directory of empty files
after the process was gone. Both are fixed now: init reaps orphans, `azwm`
waits for the children it spawns, and `procfs_pid_exited()` drops the dead
PID's directory from the dentry cache.

## Terminal

The terminal runs a plain command directly and hands anything containing
shell syntax — `|`, `>`, `<`, `;`, `&`, `$`, quotes, globs — to `/bin/sh`,
which is GNU Bash ([SHELL.md](SHELL.md)). Before that, the operators were
passed to the program as literal arguments, so `echo a; echo b` printed
"a; echo b" and a redirect wrote nothing.

Built-ins it keeps for itself: `cd`, `clear`, `pwd`, `history`, `help`,
`exit`, `config`, `launch`, and the names of GUI apps (which are launched
as windows rather than run inside the terminal).

## Code Studio

`Run (F5)` saves the buffer, compiles it with TinyCC
(`tcc -O2 -std=c11 -static`, see [TOOLCHAIN.md](TOOLCHAIN.md)) and runs the
result in the Runner tab. With no compiler installed it says so and points
at `pkg install tcc`; it does not report a build that did not happen as a
success.

## Adding an application to the desktop

1. Build it into `/bin` (one line in `userland/Makefile`'s app list).
2. Register it in `/etc/launcher.conf`: `name=category:subtitle`, where
   category is `system`, `productivity`, `media` or `games`.
3. Optionally add it to `/etc/taskbar_dock.conf` for a dock icon and
   `/etc/desktop_icons.conf` for a desktop icon.

Both files have compiled-in defaults in the apps that read them, so a
missing or corrupt config degrades to a sane desktop rather than an empty
one.
