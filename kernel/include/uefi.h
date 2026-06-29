/**
 * kernel/include/uefi.h — Standalone UEFI Type Definitions for 64-bit AzamiOS
 */
#ifndef UEFI_H
#define UEFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t UINTN;
typedef int64_t  INTN;
typedef uint64_t EFI_STATUS;
typedef void *   EFI_HANDLE;
typedef void *   EFI_EVENT;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS 0ULL

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN    SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN    FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber, UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    uint32_t Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    char _buf[24];
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (*RaiseTPL)(void);
    void (*RestoreTPL)(void);
    EFI_STATUS (*AllocatePages)(void);
    EFI_STATUS (*FreePages)(void);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, uint32_t *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(void);
    EFI_STATUS (*FreePool)(void);
    EFI_STATUS (*CreateEvent)(void);
    EFI_STATUS (*SetTimer)(void);
    EFI_STATUS (*WaitForEvent)(void);
    EFI_STATUS (*SignalEvent)(void);
    EFI_STATUS (*CloseEvent)(void);
    EFI_STATUS (*CheckEvent)(void);
    EFI_STATUS (*InstallProtocolInterface)(void);
    EFI_STATUS (*ReinstallProtocolInterface)(void);
    EFI_STATUS (*UninstallProtocolInterface)(void);
    EFI_STATUS (*HandleProtocol)(void);
    void *Reserved;
    EFI_STATUS (*RegisterProtocolNotify)(void);
    EFI_STATUS (*LocateHandle)(void);
    EFI_STATUS (*LocateDevicePath)(void);
    EFI_STATUS (*InstallConfigurationTable)(void);
    EFI_STATUS (*LoadImage)(void);
    EFI_STATUS (*StartImage)(void);
    EFI_STATUS (*Exit)(void);
    EFI_STATUS (*UnloadImage)(void);
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    EFI_STATUS (*GetNextMonotonicCount)(void);
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(void);
    EFI_STATUS (*ConnectController)(void);
    EFI_STATUS (*DisconnectController)(void);
    EFI_STATUS (*OpenProtocol)(void);
    EFI_STATUS (*CloseProtocol)(void);
    EFI_STATUS (*OpenProtocolInformation)(void);
    EFI_STATUS (*ProtocolsPerHandle)(void);
    EFI_STATUS (*LocateHandleBuffer)(void);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    uint16_t *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* AzamiOS Framebuffer Boot Information passed to kernel */
typedef struct {
    uint64_t framebuffer_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* bytes per line */
    uint32_t bpp;
} uefi_boot_info_t;

#endif /* UEFI_H */
