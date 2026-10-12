#!/bin/sh
set -eu

check() {
    name=$1
    shift
    "$@"
    printf 'PASS: %s\n' "$name"
}

# 1. SQLite3
check "sqlite3 :memory: count(*)" sh -c 'test "$(sqlite3 :memory: "CREATE TABLE t(x); INSERT INTO t VALUES (1),(2),(3); SELECT count(*) FROM t;")" = "3"'

# 2. Lua 5.4
check "lua 5.4 print(1+1)" sh -c 'test "$(lua -e "print(1+1)")" = "2"'

# 3. MicroPython
check "micropython print(40+2)" sh -c 'test "$(micropython -c "print(40+2)")" = "42"'

# 4. Dash loop
check "dash loop" sh -c 'test "$(dash -c "for i in 1 2 3; do echo \$i; done")" = "1
2
3"'

# 5. Toybox echo and uname
check "toybox echo" sh -c 'test "$(toybox echo hello)" = "hello"'
check "toybox uname" toybox uname -a

# 6. Zstandard stream compression and decompression pipeline
check "zstd roundtrip" sh -c 'test "$(printf "AzamiOS Fast Systems" | zstd -c | zstd -d)" = "AzamiOS Fast Systems"'

# 7. Procfs ENOSYS telemetry check
printf "\n-- /proc/azami/enosys telemetry --\n"
cat /proc/azami/enosys
printf "PASS: /proc/azami/enosys readable\n"

printf 'Ports smoke complete: 0 failed\n'

