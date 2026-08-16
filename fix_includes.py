import os
import sys

moved_files = {
    "drivers/acpi.c": "drivers/acpi/acpi.c",
    "drivers/acpi.h": "drivers/acpi/acpi.h",
    "drivers/ioapic.c": "drivers/acpi/ioapic.c",
    "drivers/ioapic.h": "drivers/acpi/ioapic.h",
    "drivers/block.c": "drivers/block/block.c",
    "drivers/block.h": "drivers/block/block.h",
    "drivers/ahci.h": "drivers/block/ahci.h",
    "drivers/console.c": "drivers/char/console.c",
    "drivers/console.h": "drivers/char/console.h",
    "drivers/uart.c": "drivers/char/uart.c",
    "drivers/uart.h": "drivers/char/uart.h",
    "drivers/lpt.c": "drivers/char/lpt.c",
    "drivers/lpt.h": "drivers/char/lpt.h",
    "drivers/input.c": "drivers/input/input.c",
    "drivers/input.h": "drivers/input/input.h",
    "drivers/rtc.c": "drivers/misc/rtc.c",
    "drivers/rtc.h": "drivers/misc/rtc.h",
    "drivers/bga.c": "drivers/misc/bga.c",
    "drivers/bga.h": "drivers/misc/bga.h",
}

moved_dirs = {
    "kernel/fs/": "fs/",
    "kernel/hal/": "hal/",
    "kernel/drivers/video/": "drivers/video/"
}

def resolve_include(current_file, include_path):
    current_dir = os.path.dirname(current_file)
    target_abs = os.path.normpath(os.path.join(current_dir, include_path))
    return target_abs

def compute_new_include(current_file, target_abs):
    new_target_abs = target_abs
    if target_abs in moved_files:
        new_target_abs = moved_files[target_abs]
    else:
        for old_dir, new_dir in moved_dirs.items():
            if target_abs.startswith(old_dir):
                new_target_abs = new_dir + target_abs[len(old_dir):]
                break

    current_dir = os.path.dirname(current_file)
    rel_path = os.path.relpath(new_target_abs, current_dir)
    return rel_path

def process_file(file_path):
    with open(file_path, "r") as f:
        lines = f.readlines()

    changed = False
    for i, line in enumerate(lines):
        if line.strip().startswith("#include \""):
            start_idx = line.find('"') + 1
            end_idx = line.rfind('"')
            if start_idx > 0 and end_idx > start_idx:
                inc_path = line[start_idx:end_idx]
                target_abs = resolve_include(file_path, inc_path)
                
                is_old = False
                if target_abs in moved_files:
                    is_old = True
                else:
                    for old_dir in moved_dirs.keys():
                        if target_abs.startswith(old_dir):
                            is_old = True
                            break
                            
                if is_old:
                    new_rel = compute_new_include(file_path, target_abs)
                    lines[i] = line[:start_idx] + new_rel + line[end_idx:]
                    changed = True

    if changed:
        print("Updated " + file_path)
        with open(file_path, "w") as f:
            f.writelines(lines)

for root, _, files in os.walk("."):
    if ".git" in root or "build" in root or "tools" in root or "userland" in root:
        continue
    for file in files:
        if file.endswith(".c") or file.endswith(".h") or file.endswith(".asm"):
            process_file(os.path.normpath(os.path.join(root, file)))

for root, _, files in os.walk("userland"):
    if "build" in root: continue
    for file in files:
        if file.endswith(".c") or file.endswith(".h"):
            process_file(os.path.normpath(os.path.join(root, file)))
