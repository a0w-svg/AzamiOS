#!/bin/sh
# Host regression tests; does not boot or modify a disk image.
set -eu
cd "$(dirname "$0")/.."
ui_test_dir=$(mktemp -d /tmp/azami-ui-regressions.XXXXXX)
trap 'rm -rf "$ui_test_dir"' EXIT HUP INT TERM
ui_test_cc=${CC:-gcc}
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror tests/userland/settings_helpers.c -o "$ui_test_dir/helpers"
"$ui_test_dir/helpers"
"$ui_test_cc" -std=c11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Dmain=settings_app_main \
    -c userland/apps/settings/main.c -o "$ui_test_dir/settings.o"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -no-pie -Wl,--gc-sections \
    tests/userland/settings_actions.c "$ui_test_dir/settings.o" -o "$ui_test_dir/actions"
"$ui_test_dir/actions"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-pie -no-pie \
    -nostdinc -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Wl,--gc-sections \
    tests/userland/settings_viewport.c "$ui_test_dir/settings.o" -o "$ui_test_dir/viewport"
"$ui_test_dir/viewport"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-pie -no-pie \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Wl,--gc-sections \
    tests/userland/paint_buttons.c -o "$ui_test_dir/paint"
"$ui_test_dir/paint"
"$ui_test_cc" -std=c11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -c userland/libc/time.c -o "$ui_test_dir/time.o"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-pie -no-pie \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Wl,--gc-sections \
    tests/userland/taskbar_preferences.c "$ui_test_dir/time.o" -o "$ui_test_dir/taskbar"
"$ui_test_dir/taskbar"
# Rendering uses the actual library; host file I/O exercises binary loaders.
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
    -Dread=font_test_read -ffunction-sections -fdata-sections \
    -c userland/libc/font.c -o "$ui_test_dir/font.o"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-pie -no-pie \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Wl,--gc-sections \
    tests/userland/font_rendering.c "$ui_test_dir/font.o" -o "$ui_test_dir/rendering"
"$ui_test_dir/rendering"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -Wl,--gc-sections \
    tests/userland/font_loading.c "$ui_test_dir/font.o" -o "$ui_test_dir/loading"
"$ui_test_dir/loading"
"$ui_test_cc" -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-pie -no-pie \
    -ffunction-sections -fdata-sections -nostdinc \
    -isystem "$("$ui_test_cc" -print-file-name=include)" \
    -Iuserland/libc/include -Iuserland -Wl,--gc-sections \
    tests/userland/clock_behavior.c "$ui_test_dir/time.o" -o "$ui_test_dir/clock"
"$ui_test_dir/clock"
