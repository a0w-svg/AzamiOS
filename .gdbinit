# AzamiOS GDB Debugging Helper
set architecture auto

define load-wm
  add-symbol-file build/user_bin/usr/bin/wm 0x10000000
end
document load-wm
  Load Window Manager (wm) symbols at userspace entry point 0x10000000
end

define load-shell
  add-symbol-file build/user_bin/bin/shell 0x10000000
end
document load-shell
  Load shell symbols at userspace entry point 0x10000000
end
