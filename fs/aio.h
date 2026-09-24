/* ============================================================================
 * AzamiOS — Linux asynchronous I/O, kernel-side API
 * File: fs/aio.h
 *
 * The five io_*(2) syscalls in kernel/syscall/syscall.c are thin wrappers
 * over these. See fs/aio.c for what "asynchronous" means here.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/uapi/aio_abi.h"

struct process;

/** aio_setup(nr_events, out_id) — io_setup(2). */
s64 aio_setup(u32 nr_events, u64 *out_id);

/** aio_destroy(id) — io_destroy(2). */
s64 aio_destroy(u64 id);

/** aio_submit(id, nr, user_iocb_ptrs) — io_submit(2). @user_iocb_ptrs is the
 *  user-space array of `struct iocb *`. Returns how many were accepted. */
s64 aio_submit(u64 id, long nr, u64 *user_iocb_ptrs);

/** aio_getevents(...) — io_getevents(2). @user_events is a user pointer.
 *  @have_timeout distinguishes "wait forever" from a zero timeout. */
s64 aio_getevents(u64 id, long min_nr, long nr, struct io_event *user_events,
                  u64 timeout_ns, bool have_timeout);

/** aio_cancel(id, user_iocb, user_result) — io_cancel(2). */
s64 aio_cancel(u64 id, u64 user_iocb, struct io_event *user_result);

/** aio_process_exit(proc) — release every AIO context @proc still owns.
 *  Called from the process teardown path; without it a process that exits
 *  without io_destroy(2) leaks its rings for the life of the system. */
void aio_process_exit(struct process *proc);
