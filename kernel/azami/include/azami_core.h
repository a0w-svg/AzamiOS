/*
 * azamiOS Core System Header
 * Path: /kernel/azami/include/azami_core.h
 * Description: Unified header containing az_status_t codes, atomic primitives,
 *              az_object_t structures, az_irp_t definitions, handle tables,
 *              static limits for zero post-boot dynamic allocation, and all
 *              kernel subsystem function prototypes.
 */

#ifndef AZAMI_CORE_H
#define AZAMI_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* =========================================================================
 * 1. STATIC MEMORY HARDENING BOUNDS & RESOURCE LIMITS
 * ========================================================================= */

#define AZ_MAX_HANDLES_PER_TABLE      1024U
#define AZ_MAX_PROCESSES              64U
#define AZ_MAX_THREADS                256U
#define AZ_MAX_OBJECTS                2048U
#define AZ_MAX_IRPS                   512U
#define AZ_MAX_SURFACES               16U
#define AZ_MAX_PATH_LEN               256U
#define AZ_IRP_DATA_BUFFER_SIZE       4096U
#define AZ_WIN32K_MAX_WINDOWS         32U
#define AZ_FB_MAX_WIDTH               1920U
#define AZ_FB_MAX_HEIGHT              1080U
#define AZ_FB_BPP                     32U
#define AZ_FB_PITCH                   (AZ_FB_MAX_WIDTH * (AZ_FB_BPP / 8U))
#define AZ_FB_PAGE_SIZE               (AZ_FB_PITCH * AZ_FB_MAX_HEIGHT)
#define AZ_INVALID_HANDLE             ((az_handle_t)0xFFFFFFFFU)

/* =========================================================================
 * 2. STATUS CODES & ERROR PROPAGATION
 * ========================================================================= */

typedef int32_t az_status_t;

#define AZ_STATUS_SUCCESS                     ((az_status_t)0x00000000)
#define AZ_STATUS_UNSUCCESSFUL                ((az_status_t)0xC0000001)
#define AZ_STATUS_NOT_IMPLEMENTED             ((az_status_t)0xC0000002)
#define AZ_STATUS_INVALID_PARAMETER           ((az_status_t)0xC000000D)
#define AZ_STATUS_INVALID_HANDLE              ((az_status_t)0xC0000008)
#define AZ_STATUS_OBJECT_TYPE_MISMATCH        ((az_status_t)0xC0000024)
#define AZ_STATUS_INSUFFICIENT_RESOURCES      ((az_status_t)0xC000009A)
#define AZ_STATUS_ACCESS_DENIED               ((az_status_t)0xC0000022)
#define AZ_STATUS_OBJECT_PATH_NOT_FOUND       ((az_status_t)0xC000003A)
#define AZ_STATUS_PENDING                     ((az_status_t)0x00000103)
#define AZ_STATUS_TIMEOUT                     ((az_status_t)0x00000102)
#define AZ_STATUS_BUFFER_OVERFLOW             ((az_status_t)0x80000005)
#define AZ_STATUS_BUFFER_TOO_SMALL            ((az_status_t)0xC0000023)
#define AZ_STATUS_END_OF_FILE                 ((az_status_t)0xC0000011)
#define AZ_STATUS_INVALID_DEVICE_REQUEST      ((az_status_t)0xC0000010)
#define AZ_STATUS_THREAD_IS_TERMINATING       ((az_status_t)0xC000004B)

#define AZ_SUCCESS(Status)                    (((az_status_t)(Status)) >= 0)
#define AZ_ERROR(Status)                      (((az_status_t)(Status)) < 0)

/* =========================================================================
 * 3. ATOMIC SPINLOCK PRIMITIVES
 * ========================================================================= */

typedef struct az_spinlock {
    atomic_flag flag;
} az_spinlock_t;

static inline void az_spinlock_init(az_spinlock_t* lock) {
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    }
}

static inline void az_spinlock_acquire(az_spinlock_t* lock) {
    if (lock != NULL) {
        while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
            __builtin_ia32_pause();
        }
    }
}

static inline void az_spinlock_release(az_spinlock_t* lock) {
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    }
}

/* =========================================================================
 * 4. OBJECT MANAGER & HANDLE TABLE STRUCTURES
 * ========================================================================= */

typedef uint32_t az_handle_t;

typedef enum az_object_type {
    AZ_OBJ_TYPE_UNKNOWN = 0,
    AZ_OBJ_TYPE_PROCESS = 1,
    AZ_OBJ_TYPE_THREAD  = 2,
    AZ_OBJ_TYPE_FILE    = 3,
    AZ_OBJ_TYPE_DEVICE  = 4,
    AZ_OBJ_TYPE_SURFACE = 5,
    AZ_OBJ_TYPE_EVENT   = 6
} az_object_type_t;

struct az_handle_table;
struct az_object;

typedef struct az_process {
    uint32_t pid;
    uint32_t parent_pid;
    char comm[32];
    struct az_handle_table* handle_table;
    uint64_t page_directory_cr3;
    _Atomic uint32_t active_thread_count;
    bool is_terminated;
} az_process_t;

typedef enum az_thread_state {
    AZ_THREAD_STATE_READY      = 0,
    AZ_THREAD_STATE_RUNNING    = 1,
    AZ_THREAD_STATE_BLOCKED    = 2,
    AZ_THREAD_STATE_TERMINATED = 3
} az_thread_state_t;

typedef struct az_thread {
    uint32_t tid;
    uint32_t owner_pid;
    uint8_t base_priority;
    _Atomic uint8_t current_priority;
    uint8_t inherited_priority;
    az_thread_state_t state;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    uint64_t saved_rsp;
    uint64_t saved_rip;
    _Atomic bool is_preempted;
    struct az_object* process_obj;
    struct az_object* blocked_on_mutex;
    struct az_object* next_ready;
} az_thread_t;

typedef struct az_file {
    char path[AZ_MAX_PATH_LEN];
    uint32_t access_flags;
    uint64_t file_offset;
    uint64_t file_size;
    bool is_directory;
    bool is_readable;
    bool is_writable;
} az_file_t;

typedef struct az_surface {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t active_page_index;
    uint8_t* framebuffer_pages[2];
} az_surface_t;

typedef struct az_object {
    az_object_type_t type;
    _Atomic int32_t ref_count;
    az_spinlock_t lock;
    char name[AZ_MAX_PATH_LEN];
    bool is_allocated;
    union {
        az_process_t process;
        az_thread_t thread;
        az_file_t file;
        az_surface_t surface;
    } data;
} az_object_t;

typedef struct az_handle_entry {
    az_object_t* object;
    uint32_t granted_access;
    bool in_use;
} az_handle_entry_t;

typedef struct az_handle_table {
    az_spinlock_t lock;
    az_handle_entry_t entries[AZ_MAX_HANDLES_PER_TABLE];
    uint32_t active_count;
    bool is_allocated;
} az_handle_table_t;

/* =========================================================================
 * 5. I/O REQUEST PACKET (IRP) & ASYNCHRONOUS I/O DEFINITIONS
 * ========================================================================= */

typedef enum az_irp_major {
    AZ_IRP_MJ_CREATE         = 0x01,
    AZ_IRP_MJ_CLOSE          = 0x02,
    AZ_IRP_MJ_READ           = 0x03,
    AZ_IRP_MJ_WRITE          = 0x04,
    AZ_IRP_MJ_DEVICE_CONTROL = 0x05,
    AZ_IRP_MJ_FLUSH          = 0x06
} az_irp_major_t;

struct az_irp;
typedef void (*az_irp_completion_routine_t)(struct az_irp* irp, void* context);

typedef struct az_irp {
    az_irp_major_t major_function;
    az_status_t io_status;
    uint64_t information;
    az_object_t* target_object;
    az_object_t* calling_thread;
    uint64_t offset;
    size_t buffer_length;
    uint8_t buffer[AZ_IRP_DATA_BUFFER_SIZE];
    _Atomic bool is_completed;
    _Atomic bool is_pending;
    az_spinlock_t lock;
    az_irp_completion_routine_t completion_routine;
    void* completion_context;
    struct az_irp* next;
    bool is_allocated;
} az_irp_t;

typedef struct az_irp_queue {
    az_spinlock_t lock;
    az_irp_t* head;
    az_irp_t* tail;
    uint32_t count;
} az_irp_queue_t;

/* =========================================================================
 * 6. WIN32K COMPOSITOR & GRAPHICS SUB-SYSTEM DEFINITIONS
 * ========================================================================= */

typedef struct az_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} az_rect_t;

typedef struct az_window {
    uint32_t window_id;
    az_handle_t surface_handle;
    az_rect_t bounds;
    az_rect_t clip_rect;
    uint32_t z_order;
    bool is_visible;
    uint32_t background_color;
    bool is_allocated;
} az_window_t;

/* =========================================================================
 * 7. LINUX ABI SUBSYSTEM (LXSS) CONSTANTS & CALL STRUCTURES
 * ========================================================================= */

#define AZ_LXSS_SYS_READ          0U
#define AZ_LXSS_SYS_WRITE         1U
#define AZ_LXSS_SYS_OPEN          2U
#define AZ_LXSS_SYS_CLOSE         3U
#define AZ_LXSS_SYS_MMAP          9U
#define AZ_LXSS_SYS_CLONE         56U
#define AZ_LXSS_SYS_EXIT          60U

#define AZ_LXSS_O_RDONLY          0x0000U
#define AZ_LXSS_O_WRONLY          0x0001U
#define AZ_LXSS_O_RDWR            0x0002U
#define AZ_LXSS_O_CREAT           0x0040U

#define AZ_LXSS_PROT_READ         0x01U
#define AZ_LXSS_PROT_WRITE        0x02U

#define AZ_LXSS_MAP_PRIVATE       0x02U
#define AZ_LXSS_MAP_ANONYMOUS     0x20U

#define AZ_LXSS_CLONE_VM          0x00000100U
#define AZ_LXSS_CLONE_FS          0x00000200U
#define AZ_LXSS_CLONE_FILES       0x00000400U
#define AZ_LXSS_CLONE_THREAD      0x00010000U

typedef struct az_lxss_syscall_args {
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
} az_lxss_syscall_args_t;

/* =========================================================================
 * 8. FUNCTION PROTOTYPES - OBJECT MANAGER (nt_kernel.c)
 * ========================================================================= */

az_status_t az_object_manager_init(void);
az_status_t az_object_create(az_object_type_t type, const char* name, az_object_t** out_object);
az_status_t az_object_reference(az_object_t* object);
az_status_t az_object_dereference(az_object_t* object);
az_status_t az_handle_table_create(az_handle_table_t** out_table);
az_status_t az_handle_table_destroy(az_handle_table_t* table);
az_status_t az_handle_create(az_handle_table_t* table, az_object_t* object, uint32_t access, az_handle_t* out_handle);
az_status_t az_handle_lookup(az_handle_table_t* table, az_handle_t handle, az_object_type_t expected_type, az_object_t** out_object);
az_status_t az_handle_close(az_handle_table_t* table, az_handle_t handle);

/* =========================================================================
 * 9. FUNCTION PROTOTYPES - THREAD SCHEDULER & PRIORITY (nt_kernel.c)
 * ========================================================================= */

az_status_t az_scheduler_init(void);
az_status_t az_thread_create_kernel(az_object_t* process_obj, uint8_t priority, uint64_t entry_point, az_object_t** out_thread);
az_status_t az_scheduler_schedule_thread(az_object_t* thread_obj);
az_status_t az_scheduler_yield(void);
az_status_t az_scheduler_get_current_thread(az_object_t** out_thread);
az_status_t az_mutex_acquire_with_priority_inheritance(az_object_t* mutex_obj, az_object_t* calling_thread);
az_status_t az_mutex_release_with_priority_inheritance(az_object_t* mutex_obj, az_object_t* calling_thread);

/* =========================================================================
 * 10. FUNCTION PROTOTYPES - ASYNCHRONOUS I/O MANAGER (nt_kernel.c)
 * ========================================================================= */

az_status_t az_io_manager_init(void);
az_status_t az_irp_allocate(az_irp_major_t major_func, az_object_t* target, az_irp_t** out_irp);
az_status_t az_irp_free(az_irp_t* irp);
az_status_t az_irp_submit_async(az_irp_t* irp);
az_status_t az_irp_complete(az_irp_t* irp, az_status_t io_status, uint64_t info);
az_status_t az_irp_queue_init(az_irp_queue_t* queue);
az_status_t az_irp_queue_push(az_irp_queue_t* queue, az_irp_t* irp);
az_status_t az_irp_queue_pop(az_irp_queue_t* queue, az_irp_t** out_irp);

/* =========================================================================
 * 11. FUNCTION PROTOTYPES - LINUX ABI SUBSYSTEM (syscall_dispatcher.c)
 * ========================================================================= */

az_status_t az_lxss_init(void);
az_status_t az_lxss_dispatch_syscall(uint64_t syscall_num, const az_lxss_syscall_args_t* args, int64_t* out_return_value);
az_status_t az_lxss_sys_read(int32_t fd, uint64_t buf_addr, size_t count, int64_t* out_ret);
az_status_t az_lxss_sys_write(int32_t fd, uint64_t buf_addr, size_t count, int64_t* out_ret);
az_status_t az_lxss_sys_open(uint64_t pathname_addr, int32_t flags, int32_t mode, int64_t* out_ret);
az_status_t az_lxss_sys_close(int32_t fd, int64_t* out_ret);
az_status_t az_lxss_sys_mmap(uint64_t addr, size_t length, int32_t prot, int32_t flags, int32_t fd, uint64_t offset, int64_t* out_ret);
az_status_t az_lxss_sys_clone(uint64_t clone_flags, uint64_t stack_top, int64_t* out_ret);
az_status_t az_lxss_sys_exit(int32_t status, int64_t* out_ret);

/* =========================================================================
 * 12. FUNCTION PROTOTYPES - WIN32K 2D COMPOSITOR & DRIVER (win32k.c)
 * ========================================================================= */

az_status_t az_win32k_init(void);
az_status_t az_win32k_surface_create(uint32_t width, uint32_t height, az_object_t** out_surface);
az_status_t az_win32k_window_create(az_handle_t surface_handle, const az_rect_t* bounds, uint32_t z_order, uint32_t* out_window_id);
az_status_t az_win32k_window_fill_rect_clipped(uint32_t window_id, const az_rect_t* rect, uint32_t color);
az_status_t az_win32k_surface_page_flip(az_object_t* surface_obj);
az_status_t az_win32k_composite_all_windows(az_object_t* target_framebuffer_surface);

/* =========================================================================
 * 13. FUNCTION PROTOTYPES - KERNEL MAIN ENTRY (main.c)
 * ========================================================================= */

az_status_t az_kernel_main(void);

#endif /* AZAMI_CORE_H */
