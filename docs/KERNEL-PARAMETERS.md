# Kernel command-line parameters

The kernel reads its command line from the bootloader (`kernel_cmdline:` in a
Limine boot entry) and parses it the way Linux does: parameters are separated
by spaces, a value follows the first `=`, and double quotes group a value that
contains spaces. When a parameter appears more than once, the last occurrence
wins, so a boot menu can append an override to a default line.

The running system shows the command line it booted with in `/proc/cmdline`.

## Setting parameters

| Where | How |
|---|---|
| `make run` / `make` | `make run CMDLINE="quiet init=/bin/sh"` appends to every entry of the disk image's boot menu. `hdd.img` is rebuilt automatically when `CMDLINE` changes. |
| The ISO | Edit `kernel_cmdline:` in `limine.conf`, then `make iso`. |
| `scripts/linux-test.sh` | `LINUX_TEST_CMDLINE="clocksource=hpet" scripts/linux-test.sh` |
| An installed system | Edit `/boot/limine.conf` on the boot partition. |

## Parameters

### Root filesystem and init

| Parameter | Meaning |
|---|---|
| `root=<device>` | Block device to mount as `/`, with or without `/dev/`: `root=/dev/sata0p2`, `root=nvme0p2`, `root=vda2`. If the device is missing or does not mount, the kernel logs why and falls back to probing `sata0p2`, `nvme0p2`, `vda2`, `sata0`, `hda2`, then the initrd. |
| `rootfstype=<fs>` | Filesystem type for `root=` (default `ext2`; also `fat32`, `squashfs`). |
| `init=<path>` | First user process, instead of `/sbin/init.elf`. `init=/bin/sh` boots to a shell on the console, with the keyboard and serial port as its input; this is the rescue path when the desktop does not start. If the program cannot be started, the normal init search runs. |

Partition devices follow Linux's naming: a `p` separates the partition number
only when the disk's name ends in a digit (`sata0p2`, `nvme0p2`) and not
otherwise (`vda2`, `hda2`).

### CPUs

| Parameter | Meaning |
|---|---|
| `nosmp` | Run on the boot CPU only. |
| `maxcpus=<n>` | Bring up at most `n` CPUs (`maxcpus=0` is the same as `nosmp`). |
| `nr_cpus=<n>` | Same limit as `maxcpus=`. |

### Console and logging

| Parameter | Meaning |
|---|---|
| `quiet` | Keep kernel messages in the log only (`dmesg`, `/proc/kmsg`); they are not echoed to the screen or serial port. On real hardware this saves the time a verbose boot spends writing to a 115200-baud UART and scrolling the framebuffer. A panic always prints. |
| `loglevel=<n>` | `n <= 4` behaves like `quiet`, `n >= 5` shows everything; overrides `quiet`. |
| `console=ttyS0` | Echo kernel messages and `/dev/console` output to COM1. |
| `console=tty0` | Echo them to the framebuffer text console. |

Without `console=`, both are used. Repeat `console=` to select more than one.
Userspace output to `/dev/console` is never affected by `quiet`.

### Time

| Parameter | Meaning |
|---|---|
| `clocksource=<name>` | Force the clocksource: `tsc`, `hpet` or `jiffies`. By default the TSC is used when the CPU reports it invariant or the hypervisor reports it stable (KVM's pvclock flag, VMware's timing leaf); otherwise the HPET. The TSC makes `clock_gettime()` a ~20 ns vDSO call; the HPET costs microseconds per read in a VM. |

### Testing

| Parameter | Meaning |
|---|---|
| `azami.disk_selftest=1` | Run the block-driver self-tests at boot (AHCI TRIM and NCQ, NVMe and virtio-scsi write/read-back). **They write to the disks they test**, so they are off by default; only use this on disposable disk images. |
