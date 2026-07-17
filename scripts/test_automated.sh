#!/usr/bin/env bash
#
# scripts/test_automated.sh — AzamiOS Automated Test Suite
#
# Tests kernel-independence of lib/ and verifies clean compilation, linking,
# and binary generation for x86_64 (64-bit) architecture.
#

set -e

echo "======================================================================"
echo "          AzamiOS Automated Test Suite (x86_64)"
echo "======================================================================"
echo ""

# Step 1: Check kernel-independence of lib/
echo "[STEP 1] Running host-based lib/ kernel-independence tests..."
make test-lib
echo "--> Step 1 PASSED!"
echo ""

# Step 2: Build and verify x86_64 architecture
echo "[STEP 2] Building system for x86_64 (64-bit)..."
make clean
make all

# Verify 64-bit artifacts
echo "[CHECK] Verifying build artifacts..."
if [ ! -f "build/kernel.elf" ]; then
    echo "ERROR: build/kernel.elf missing for x86_64!"
    exit 1
fi
if [ ! -f "build/initrd.tar" ]; then
    echo "ERROR: build/initrd.tar missing for x86_64!"
    exit 1
fi
echo "--> Step 2 PASSED: 64-bit (x86_64) kernel and initrd generated successfully!"
echo ""

echo "======================================================================"
echo "  SUMMARY: ALL AUTOMATED TESTS PASSED SUCCESSFULLY!"
echo "======================================================================"
