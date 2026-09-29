#!/bin/bash
# run-vbox.sh - Runs AzamiOS in VirtualBox
set -e

VM_NAME="AzamiOS_Dev"
VDI_FILE="hdd.vdi"
RAW_IMAGE="hdd.img"

if ! command -v VBoxManage &> /dev/null; then
    echo "Error: VBoxManage is not installed. Please install VirtualBox."
    exit 1
fi

if [ ! -f "$RAW_IMAGE" ]; then
    echo "Error: $RAW_IMAGE not found. Please run 'make' first."
    exit 1
fi

echo "[1/4] Converting raw image to VDI..."
# VirtualBox requires a VDI or VMDK. Converting raw to VDI is the cleanest approach.
rm -f "$VDI_FILE"
VBoxManage convertfromraw "$RAW_IMAGE" "$VDI_FILE" --format VDI

# Check if VM already exists, and if so, unregister and delete it
if VBoxManage showvminfo "$VM_NAME" &> /dev/null; then
    echo "[2/4] Removing existing VM..."
    VBoxManage controlvm "$VM_NAME" poweroff &> /dev/null || true
    sleep 1
    VBoxManage unregistervm "$VM_NAME" --delete-all
fi

echo "[3/4] Creating and configuring the VirtualBox VM..."
VBoxManage createvm --name "$VM_NAME" --ostype "Other_64" --register

# Configure System: 1024MB RAM, VMSVGA (VMware SVGA II) Graphics, NAT Network
VBoxManage modifyvm "$VM_NAME" \
    --memory 1024 \
    --vram 64 \
    --graphicscontroller vmsvga \
    --nic1 nat \
    --nictype1 82540EM \
    --uart1 0x3F8 4 --uartmode1 file "$(pwd)/azami_vbox_serial.log"

# Attach Storage: SATA AHCI Controller
VBoxManage storagectl "$VM_NAME" --name "SATA Controller" --add sata --controller IntelAhci --portcount 1
VBoxManage storageattach "$VM_NAME" --storagectl "SATA Controller" --port 0 --device 0 --type hdd --medium "$VDI_FILE"

echo "[4/4] Starting VirtualBox..."
VBoxManage startvm "$VM_NAME"

echo "Done! Serial logs are being written to $(pwd)/azami_vbox_serial.log"
