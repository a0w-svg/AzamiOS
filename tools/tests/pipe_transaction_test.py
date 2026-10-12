#!/usr/bin/env python3
"""Host production pipe transactions with scheduler/uaccess stubs."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
def production(path):
    return re.sub(r'^\s*#(?:include|pragma).*$', '', (root/path).read_text(), flags=re.M)
stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;
#define TASK_SIZE_MAX ((uintptr_t)1 << 47)
#define S_IFIFO 0010000
#define S_IFSOCK 0140000
typedef struct process { u64 sig_pending, sig_blocked; u32 pid; } process_t;
typedef struct thread { struct thread *next; process_t *proc; } thread_t;
typedef struct spinlock { bool locked; } spinlock_t;
#define SPINLOCK_INIT {false}
static void spinlock_lock(spinlock_t *l) { assert(!l->locked); l->locked=true; }
static void spinlock_unlock(spinlock_t *l) { assert(l->locked); l->locked=false; }
typedef struct vfs_iovec { void *iov_base; size_t iov_len; } vfs_iovec_t;
struct inode; struct file;
typedef struct file_operations {
 s64 (*read)(struct file*,void*,size_t,u64*);
 s64 (*write)(struct file*,const void*,size_t,u64*);
 s64 (*read_user)(struct file*,const vfs_iovec_t*,u32,size_t);
 s64 (*write_user)(struct file*,const vfs_iovec_t*,u32,size_t);
 s64 (*ioctl)(struct file*,u32,u64);
 s64 (*release)(struct inode*,struct file*);
 int (*poll)(struct file*);
} file_operations_t;
typedef struct inode { u32 i_mode; u64 i_size,i_blocks,i_ino; } inode_t;
typedef struct file { inode_t *f_inode; file_operations_t *f_op; u32 f_flags,f_count; void *private_data; } file_t;
static void *kzalloc(size_t n) { return calloc(1,n); }
static void kfree(void *p) { free(p); }
static process_t proc={.pid=1}; static thread_t current={.proc=&proc};
static thread_t *sched_current_thread(void) { return &current; }
static process_t *sched_current_process(void) { return &proc; }
#define THREAD_BLOCKED_PENDING 1
static void (*block_hook)(void);
static void sched_block(int state) { (void)state; assert(block_hook); void (*fn)(void)=block_hook; block_hook=NULL; fn(); }
static int wake_count;
static void sched_unblock(thread_t *t) { assert(t); wake_count++; }
static int broken_signals;
static s64 sched_kill_process(u32 pid,int sig) { assert(pid==1&&sig==13); broken_signals++; return 0; }
static void vfs_close(file_t *f) { f->f_op->release(f->f_inode,f); free(f); }
static size_t copy_from_user(void *d,const void *s,size_t n) { memcpy(d,s,n);return 0; }
static size_t copy_to_user(void *d,const void *s,size_t n) { memcpy(d,s,n);return 0; }
static size_t io_iter_copy_from_user(const vfs_iovec_t*,u32,size_t,void*,size_t);
static size_t io_iter_copy_to_user(const vfs_iovec_t*,u32,size_t,const void*,size_t);
'''
harness=r'''
static pipe_t *observed; static long copy_budget=-1; static void (*copy_hook)(void);
static size_t iter_copy(const vfs_iovec_t *iov,u32 count,size_t off,void *buf,size_t n,bool to_user) {
 assert(!observed->lock.locked);
 size_t done=0;
 for(u32 i=0;i<count&&done<n;i++) {
  if(off>=iov[i].iov_len) { off-=iov[i].iov_len; continue; }
  size_t take=iov[i].iov_len-off; if(take>n-done)take=n-done;
  if(copy_budget>=0&&(size_t)copy_budget<take)take=copy_budget;
  if(to_user)memcpy((u8*)iov[i].iov_base+off,(u8*)buf+done,take);
  else memcpy((u8*)buf+done,(u8*)iov[i].iov_base+off,take);
  if(copy_budget>=0)copy_budget-=take;
  done+=take; off=0;
  if(copy_hook) { void(*fn)(void)=copy_hook;copy_hook=NULL;fn(); }
  if(copy_budget==0)break;
 }
 return done;
}
static size_t io_iter_copy_from_user(const vfs_iovec_t *i,u32 c,size_t o,void*d,size_t n) {return iter_copy(i,c,o,d,n,false);}
static size_t io_iter_copy_to_user(const vfs_iovec_t *i,u32 c,size_t o,const void*s,size_t n) {return iter_copy(i,c,o,(void*)s,n,true);}
static u8 bytes[9000], output[9000];
static void reset(file_t **r,file_t **w) {proc.sig_pending=0;copy_budget=-1;copy_hook=NULL; assert(pipe_create(r,w)==0);observed=(*r)->private_data;}
static void cleanup(file_t *r,file_t *w) {vfs_close(r);vfs_close(w);observed=NULL;}
static void consume_eight(void) {u8 tmp[8];assert(pipe_read_pipe(observed,tmp,8,true)==8);}
static void consume_page(void) {assert(pipe_read_pipe(observed,output,4096,true)==4096);}
static void fill_competing(void) {assert(pipe_write_pipe(observed,bytes,4096,true)==4096);}
static void almost_fill_competing(void) {assert(pipe_write_pipe(observed,bytes,4088,true)==4088);}
static void consume_signal(void) {consume_eight();proc.sig_pending=1;}
int main(void) {
 for(size_t i=0;i<sizeof bytes;i++)bytes[i]=(u8)(i*73+11);
 file_t *r,*w; vfs_iovec_t iov[3]; u8 check[64]; int tests=0;
 /* Short scalar fault cannot consume ring data despite a user prefix copy. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,32,true)==32);
 iov[0]=(vfs_iovec_t){output,32};copy_budget=16;
 assert(pipe_read_user(r,iov,1,32)==-EFAULT);assert(observed->count==32&&observed->read_seq==0);
 assert(memcmp(output,bytes,16)==0);copy_budget=-1;
 assert(pipe_read_pipe(observed,check,32,true)==32&&memcmp(check,bytes,32)==0);cleanup(r,w);tests++;
 /* Wrapped read and write faults leave exact ring positions/data intact. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,4090,true)==4090);assert(pipe_read_pipe(observed,output,4080,true)==4080);
 u32 oldw=observed->write_pos,oldr=observed->read_pos;u64 olds=observed->read_seq;
 iov[0]=(vfs_iovec_t){bytes+4090,32};copy_budget=20;
 assert(pipe_write_user(w,iov,1,32)==-EFAULT);assert(observed->write_pos==oldw&&observed->count==10);
 copy_budget=-1;assert(pipe_write_user(w,iov,1,32)==32);
 iov[0]=(vfs_iovec_t){output,15};iov[1]=(vfs_iovec_t){output+15,27};copy_budget=22;
 assert(pipe_read_user(r,iov,2,42)==-EFAULT);assert(observed->read_pos==oldr&&observed->read_seq==olds&&observed->count==42);
 copy_budget=-1;assert(pipe_read_user(r,iov,2,42)==42&&memcmp(output,bytes+4080,42)==0);cleanup(r,w);tests++;
 /* Fault in the second iovec commits no prefix of an atomic write. */
 reset(&r,&w);iov[0]=(vfs_iovec_t){bytes,8};iov[1]=(vfs_iovec_t){bytes+8,8};copy_budget=8;
 assert(pipe_write_user(w,iov,2,16)==-EFAULT&&observed->count==0);cleanup(r,w);tests++;
 /* Empty iovecs do not change transaction boundaries. */
 reset(&r,&w);iov[0]=(vfs_iovec_t){NULL,0};iov[1]=(vfs_iovec_t){bytes,16};iov[2]=(vfs_iovec_t){NULL,0};
 assert(pipe_write_user(w,iov,3,16)==16);iov[1].iov_base=output;
 assert(pipe_read_user(r,iov,3,16)==16&&memcmp(output,bytes,16)==0);cleanup(r,w);tests++;
 /* Competing kernel reader invalidates snapshot; retry commits the next prefix. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,32,true)==32);iov[0]=(vfs_iovec_t){output,16};copy_hook=consume_eight;
 assert(pipe_read_user(r,iov,1,16)==16&&memcmp(output,bytes+8,16)==0&&observed->count==8&&observed->read_seq==2);cleanup(r,w);tests++;
 /* PIPE_BUF atomic write must reject insufficient room before user access. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,4090,true)==4090);w->f_flags|=O_NONBLOCK;iov[0]=(vfs_iovec_t){bytes,16};copy_budget=0;
 assert(pipe_write_user(w,iov,1,16)==-EAGAIN&&observed->count==4090);assert(pipe_write_pipe(observed,bytes,16,true)==-EAGAIN);cleanup(r,w);tests++;
 /* Competing writer fills the ring while user staging is unlocked: no overcommit. */
 reset(&r,&w);w->f_flags|=O_NONBLOCK;iov[0]=(vfs_iovec_t){bytes,16};copy_hook=fill_competing;
 assert(pipe_write_user(w,iov,1,16)==-EAGAIN&&observed->count==4096);cleanup(r,w);tests++;
 /* A later large-write fault returns exactly the previously committed chunk. */
 reset(&r,&w);iov[0]=(vfs_iovec_t){bytes,8192};copy_budget=4096;block_hook=consume_page;
 assert(pipe_write_user(w,iov,1,8192)==4096&&observed->count==0&&memcmp(output,bytes,4096)==0);cleanup(r,w);tests++;
 /* Faulting pointers on empty/full nonblocking pipes do not mask readiness. */
 reset(&r,&w);r->f_flags|=O_NONBLOCK;iov[0]=(vfs_iovec_t){output,16};copy_budget=0;
 assert(pipe_read_user(r,iov,1,16)==-EAGAIN);copy_budget=-1;assert(pipe_write_pipe(observed,bytes,4096,true)==4096);copy_budget=0;w->f_flags|=O_NONBLOCK;
 assert(pipe_write_user(w,iov,1,16)==-EAGAIN);cleanup(r,w);tests++;
 /* Closure and zero-length operations preserve EOF/EPIPE precedence. */
 reset(&r,&w);vfs_close(w);iov[0]=(vfs_iovec_t){output,16};copy_budget=0;
 assert(pipe_read_user(r,iov,1,16)==0);assert(pipe_read_user(r,NULL,0,0)==0);vfs_close(r);tests++;
 reset(&r,&w);vfs_close(r);iov[0]=(vfs_iovec_t){bytes,16};copy_budget=0;
 assert(pipe_write_user(w,iov,1,16)==-EPIPE&&broken_signals==1);assert(pipe_write_user(w,NULL,0,0)==0);vfs_close(w);tests++;
 /* Pending signals escape a readiness wait without registering a stale waiter. */
 reset(&r,&w);proc.sig_pending=1;iov[0]=(vfs_iovec_t){output,16};
 assert(pipe_read_user(r,iov,1,16)==-EINTR&&observed->read_wait==NULL);cleanup(r,w);tests++;
 /* Fault rollback passes readiness to another queued reader/writer. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,32,true)==32);thread_t other={.proc=&proc};observed->read_wait=&other;int wakes=wake_count;iov[0]=(vfs_iovec_t){output,32};copy_budget=0;
 assert(pipe_read_user(r,iov,1,32)==-EFAULT&&observed->count==32&&observed->read_wait==NULL&&wake_count==wakes+1);
 observed->write_wait=&other;wakes=wake_count;iov[0]=(vfs_iovec_t){bytes,32};
 assert(pipe_write_user(w,iov,1,32)==-EFAULT&&observed->write_wait==NULL&&wake_count==wakes+1);cleanup(r,w);tests++;
 /* Large nonblocking writes make progress when a racing writer leaves some room. */
 reset(&r,&w);w->f_flags|=O_NONBLOCK;iov[0]=(vfs_iovec_t){bytes,8192};copy_hook=almost_fill_competing;
 assert(pipe_write_user(w,iov,1,8192)==8&&observed->count==4096);assert(pipe_read_pipe(observed,output,4096,true)==4096&&memcmp(output+4088,bytes,8)==0);cleanup(r,w);tests++;
 /* Read generation conflict plus signal leaves only the competing read committed. */
 reset(&r,&w);assert(pipe_write_pipe(observed,bytes,32,true)==32);iov[0]=(vfs_iovec_t){output,16};copy_hook=consume_signal;
 assert(pipe_read_user(r,iov,1,16)==-EINTR&&observed->count==24&&observed->read_seq==1);cleanup(r,w);tests++;
 /* Duplicate wait insertion must not truncate subsequent waiters. */
 thread_t first={0}, second={0};thread_t *queue=&first;first.next=&second;
 pipe_wait_push(&queue,&first);assert(queue==&first&&first.next==&second);tests++;
 printf("production pipe transaction tests: %d passed\n",tests);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='azami-pipe-host-') as d:
 p=Path(d);source=p/'test.c';exe=p/'test';source.write_text(stub+production('fs/pipe.h')+production('fs/pipe.c')+harness)
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fsanitize-undefined-trap-on-error','-fno-omit-frame-pointer',str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
