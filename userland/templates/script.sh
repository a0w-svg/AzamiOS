#!/bin/sh
# AzamiOS Shell Automation Script
set -e

echo "=== AzamiOS System Script ==="
echo "Date: $(date)"
echo "Kernel: $(uname -a)"
echo "Uptime: $(uptime)"
echo "Disk Space:"
df -h
echo "Done."
