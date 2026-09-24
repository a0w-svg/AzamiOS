/* ============================================================================
 * AzamiOS — VirtualBox Guest Additions Driver (VMMDev)
 * File: drivers/misc/vboxguest.h
 * ============================================================================ */

#pragma once
#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

#define VMMDEV_VERSION 0x00010003

enum vmmdev_request_type {
    VMMDEVREQ_GET_MOUSE_STATUS = 1,
    VMMDEVREQ_SET_MOUSE_STATUS = 2,
    VMMDEVREQ_ACKNOWLEDGE_EVENTS = 41,
    VMMDEVREQ_REPORT_GUEST_INFO = 50,
    VMMDEVREQ_SET_GUEST_CAPABILITIES = 55,
};

#define VMMDEV_GUEST_SUPPORTS_SEAMLESS           (1 << 0)
#define VMMDEV_GUEST_SUPPORTS_GRAPHICS           (1 << 1)
#define VMMDEV_GUEST_SUPPORTS_ABSOLUTE_POINTER   (1 << 2)

#define VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE          (1 << 0)
#define VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE         (1 << 1)

#define VMMDEV_EVENT_DISPLAY_CHANGE              (1 << 2)
#define VMMDEV_EVENT_MOUSE_POSITION_CHANGED      (1 << 9)

typedef struct {
    u32 size;
    u32 version;
    u32 request_type;
    s32 rc;
    u32 reserved1;
    u32 reserved2;
} __packed vmmdev_req_header_t;

typedef struct {
    vmmdev_req_header_t header;
    u32 capabilities;
} __packed vmmdev_req_guest_caps_t;

typedef struct {
    vmmdev_req_header_t header;
    u32 features;
    s32 x;
    s32 y;
} __packed vmmdev_req_mouse_status_t;

typedef struct {
    vmmdev_req_header_t header;
    u32 events;
} __packed vmmdev_req_ack_events_t;

typedef struct {
    vmmdev_req_header_t header;
    u32 os_type;     /* e.g., 0 for "Other" or 0x100 for Linux */
    u32 os_version;
} __packed vmmdev_req_report_guest_info_t;

