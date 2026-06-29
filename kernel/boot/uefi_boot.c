/**
 * kernel/boot/uefi_boot.c — 64-bit UEFI Entry Point for AzamiOS
 */
#include "../include/uefi.h"

extern void x86_arch_init(unsigned long magic, unsigned long addr);

static uefi_boot_info_t g_boot_info;
static EFI_GUID g_gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL*)0;
    
    /* Locate GOP Framebuffer */
    EFI_STATUS status = SystemTable->BootServices->LocateProtocol(&g_gop_guid, (void*)0, (void **)&gop);
    if (status == EFI_SUCCESS && gop && gop->Mode && gop->Mode->Info) {
        g_boot_info.framebuffer_base = gop->Mode->FrameBufferBase;
        g_boot_info.width  = gop->Mode->Info->HorizontalResolution;
        g_boot_info.height = gop->Mode->Info->VerticalResolution;
        g_boot_info.pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
        g_boot_info.bpp    = 32;
    }

    /* Retrieve memory map and exit boot services */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    SystemTable->BootServices->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR*)0, &map_key, &desc_size, &desc_ver);
    map_size += 4096; /* allocate extra room for descriptor overhead */
    
    char memory_map[4096];
    if (map_size <= sizeof(memory_map)) {
        SystemTable->BootServices->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR*)memory_map, &map_key, &desc_size, &desc_ver);
        SystemTable->BootServices->ExitBootServices(ImageHandle, map_key);
    }

    /* Transfer control to 64-bit kernel main */
    x86_arch_init(0xEF1B0072, (unsigned long)&g_boot_info);

    while (1) __asm__ volatile ("cli; hlt");
    return EFI_SUCCESS;
}
