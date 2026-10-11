#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Linux UAPI layout, including a zero-filled forward-compatible tail. */
struct attr { uint32_t type, size; uint64_t config, period, sample, format, flags;
              unsigned char tail[96]; };
struct reading { uint64_t count, enabled, running, id; };
#define ENABLE 0x2400U
#define DISABLE 0x2401U
#define RESET 0x2403U
#define ID 0x80082407U
static int passed, failed, skipped, guest;
static void check(const char *name, long value, long expected)
{
    int good = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static void skip(const char *name, const char *reason)
{
    printf("NSSKIP {\"name\":\"%s\",\"reason\":\"%s\"}\n", name, reason);
    skipped++;
}
static long result(long n) { return n < 0 ? -errno : n; }
static struct attr attr(unsigned type, unsigned config)
{
    struct attr a = {.type=type, .size=128, .config=config, .format=7, .flags=1|32|64};
    return a;
}
static int open_event(struct attr *a, int pid, int cpu, int group, unsigned flags)
{ return (int)syscall(SYS_perf_event_open, a, pid, cpu, group, flags); }
static uint64_t ns(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t)) _exit(90);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void work(unsigned ms)
{
    uint64_t end = ns(CLOCK_MONOTONIC) + (uint64_t)ms * 1000000;
    volatile unsigned long value = 1;
    while (ns(CLOCK_MONOTONIC) < end)
        for (unsigned i=0; i<1000; i++) value = value * 1664525 + 1013904223;
}
static void pause_ms(unsigned ms)
{
    struct timespec t = {.tv_sec=ms/1000, .tv_nsec=(long)(ms%1000)*1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static int read_event(int fd, struct reading *r) { return read(fd,r,sizeof(*r)) == sizeof(*r); }

static void basic(void)
{
    struct attr a=attr(1,1);
    int fd=open_event(&a,0,-1,-1,8);
    check("perf.basic",fd>=0,1);
    if (fd<0) return;
    struct reading r={0}, frozen={0}, after={0};
    struct attr independent=attr(1,9);
    int single=open_event(&independent,0,-1,fd,1);
    check("group.no_group.independent",single>=0,1);
    if(single>=0) close(single);
    check("group.invalid.fd",result(open_event(&independent,0,-1,10000,1)),-EBADF);
    check("disabled.read",read_event(fd,&r),1);
    check("disabled.zero",r.count==0 && r.enabled==0 && r.running==0,1);
    work(5);
    check("disabled.work.read",read_event(fd,&r),1);
    check("disabled.work.zero",r.count,0);
    /* Bracket both syscalls before printing: console CPU time after read is
     * outside the measured event interval and may be large under VM load. */
    uint64_t start_lo=ns(CLOCK_THREAD_CPUTIME_ID);
    long enable_rc=result(ioctl(fd,ENABLE,0));
    uint64_t start_hi=ns(CLOCK_THREAD_CPUTIME_ID);
    work(7);
    uint64_t end_lo=ns(CLOCK_THREAD_CPUTIME_ID);
    int live_ok=read_event(fd,&r);
    uint64_t end_hi=ns(CLOCK_THREAD_CPUTIME_ID);
    check("enable",enable_rc,0);
    check("live.read",live_ok,1);
    check("live.count",r.count>0 && r.running>0 && r.enabled>=r.running,1);
    printf("PFTIME task-clock count=%llu reference=[%llu,%llu]\n",
           (unsigned long long)r.count,(unsigned long long)(end_lo-start_hi),
           (unsigned long long)(end_hi-start_lo));
    check("live.clock.accuracy",r.count<=end_hi-start_lo+3000000 &&
          end_lo-start_hi<=r.count+3000000,1);
    check("disable",result(ioctl(fd,DISABLE,0)),0);
    check("frozen.read",read_event(fd,&frozen),1);
    work(5); pause_ms(15);
    check("frozen.again.read",read_event(fd,&after),1);
    check("disabled.frozen",memcmp(&frozen,&after,sizeof(after))==0,1);
    check("reset",result(ioctl(fd,RESET,0)),0);
    check("reset.read",read_event(fd,&r),1);
    check("reset.zero",r.count,0);
    check("reset.preserves.times",r.enabled==frozen.enabled && r.running==frozen.running,1);
    uint64_t id=0;
    check("id.ioctl",result(ioctl(fd,ID,&id)),0);
    check("id.matches",id==r.id && id!=0,1);
    check("read.short",result(read(fd,&r,7)),-ENOSPC);
    check("read.bad.pointer",result(read(fd,(void *)1,sizeof(r))),-EFAULT);
    check("ioctl.bad.magic",result(ioctl(fd,0x2500,0)),-ENOTTY);
    if (guest) {
        check("azami.ioctl.bad.encoding",result(ioctl(fd,0x40082400,0)),-ENOTTY);
        check("azami.ioctl.bad.argument",result(ioctl(fd,ENABLE,2)),-EINVAL);
    }
    struct pollfd p={.fd=fd,.events=POLLIN};
    check("poll.counting.hup",poll(&p,1,0)==1 && (p.revents&POLLHUP),1);
    int alias=dup(fd);
    check("fd.dup",alias>=0,1);
    close(fd);
    check("fd.alias.alive",read_event(alias,&r),1);
    check("fd.alias.id",r.id==id,1);
    check("reenable",result(ioctl(alias,ENABLE,0)),0);
    work(5);
    check("reenable.disable",result(ioctl(alias,DISABLE,0)),0);
    check("reenable.read",read_event(alias,&r),1);
    check("reenable.accumulates",r.count>0 && r.running>frozen.running && r.enabled>frozen.enabled,1);
    close(alias);
}

static _Atomic int ready, release_worker;
static int worker_fd, worker_tid, worker_hardware;
static struct reading worker_read;
static void *worker(void *unused)
{
    (void)unused;
    struct attr a=attr(worker_hardware ? 0 : 1,1);
    worker_tid=(int)syscall(SYS_gettid);
    worker_fd=open_event(&a,0,-1,-1,0);
    if(worker_fd>=0) ioctl(worker_fd,ENABLE,0);
    atomic_store(&ready,1);
    while(!atomic_load(&release_worker)) pause_ms(2);
    if(worker_fd>=0) read_event(worker_fd,&worker_read);
    return NULL;
}
static void thread_scope(void)
{
    pthread_t t;
    check("thread.create",pthread_create(&t,NULL,worker,NULL),0);
    while(!atomic_load(&ready)) pause_ms(1);
    struct attr remote_attr=attr(1,1); remote_attr.flags&=~1ULL;
    int attached=open_event(&remote_attr,worker_tid,-1,-1,0);
    check("other.thread.open",attached>=0,1);
    if(attached>=0) {
        check("other.thread.disable",result(ioctl(attached,DISABLE,0)),0);
        check("other.thread.reset",result(ioctl(attached,RESET,0)),0);
        check("other.thread.enable",result(ioctl(attached,ENABLE,0)),0);
    }
    int unique=worker_tid!=(int)getpid();
    for(int i=0;i<16;i++) {
        pid_t child=fork();
        if(child==0) _exit((int)getpid()==worker_tid ? 1 : 0);
        int status=0;
        if(child<0 || waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) unique=0;
    }
    check("identity.pid.tid.unique",unique,1);
    work(40);
    atomic_store(&release_worker,1);
    check("thread.join",pthread_join(t,NULL),0);
    if(attached>=0) {
        struct reading x,y;
        check("other.thread.read",read_event(attached,&x),1);
        check("other.thread.scope",x.count>0 && x.count<20000000 && x.enabled==x.running,1);
        work(3);
        check("other.thread.exit.read",read_event(attached,&y),1);
        check("other.thread.exit.frozen",memcmp(&x,&y,sizeof(x))==0,1);
        close(attached);
    }
    check("thread.open",worker_fd>=0,1);
    if(worker_fd<0) return;
    check("thread.unrelated.work.excluded",worker_read.count<20000000 && worker_read.enabled<20000000,1);
    struct reading a,b;
    check("exit.final.read",read_event(worker_fd,&a),1);
    work(5); pause_ms(10);
    check("exit.again.read",read_event(worker_fd,&b),1);
    check("exit.frozen",memcmp(&a,&b,sizeof(a))==0 && a.count>0,1);
    check("exit.enable",result(ioctl(worker_fd,ENABLE,0)),0);
    check("exit.enable.read",read_event(worker_fd,&b),1);
    check("exit.enable.frozen",memcmp(&a,&b,sizeof(a))==0,1);
    struct pollfd p={.fd=worker_fd,.events=POLLIN};
    check("exit.poll.hup",poll(&p,1,0)==1 && (p.revents&POLLHUP),1);
    close(worker_fd);
}
static void *stress_worker(void *arg)
{
    (void)arg;
    for(int i=0;i<150;i++) {
        struct attr a=attr(1,9);
        int fd=open_event(&a,0,-1,-1,0), alias;
        struct reading r;
        if(fd<0) return (void *)1;
        alias=dup(fd); close(fd);
        if(alias<0) return (void *)2;
        int ok=!ioctl(alias,ENABLE,0) && !ioctl(alias,RESET,0) && read_event(alias,&r) &&
               r.count==0 && !ioctl(alias,DISABLE,0);
        close(alias);
        if(!ok) return (void *)3;
    }
    return NULL;
}
static void stress(void)
{
    pthread_t threads[4]; int made=0, errors=0;
    for(int i=0;i<4;i++) { if(pthread_create(&threads[i],NULL,stress_worker,NULL)) break; made++; }
    for(int i=0;i<made;i++) { void *rc=NULL; if(pthread_join(threads[i],&rc)||rc) errors++; }
    check("race.fd.lifetime",made==4 && errors==0,1);
}
static void occurrences(void)
{
    struct attr a=attr(1,3); a.flags&=~32ULL;
    int fd=open_event(&a,0,-1,-1,0);
    if(fd<0 && !guest && (errno==EACCES || errno==EPERM)) {
        skip("switches.kernel","host kernel counting permission denied");
        a.flags|=32;
        fd=open_event(&a,0,-1,-1,0);
    }
    check("switches.open",fd>=0,1);
    if(fd>=0) {
        struct reading r,b;
        ioctl(fd,ENABLE,0); pause_ms(10); ioctl(fd,DISABLE,0);
        check("switches.read",read_event(fd,&r),1);
        check(a.flags&32 ? "switches.filtered.zero" : "switches.positive",a.flags&32 ? r.count==0 : r.count>0,1);
        pause_ms(10); check("switches.disabled.read",read_event(fd,&b),1);
        check("switches.disabled.frozen",memcmp(&r,&b,sizeof(r))==0,1);
        close(fd);
    }
    a=attr(1,0); fd=open_event(&a,0,-1,-1,0);
    check("cpu.clock.open",fd>=0,1);
    if(fd>=0) {
        struct reading r;
        uint64_t before=ns(CLOCK_THREAD_CPUTIME_ID);
        ioctl(fd,ENABLE,0); work(5); pause_ms(30); ioctl(fd,DISABLE,0);
        uint64_t elapsed=ns(CLOCK_THREAD_CPUTIME_ID)-before;
        check("cpu.clock.read",read_event(fd,&r),1);
        printf("PFTIME cpu-clock count=%llu enabled=%llu running=%llu thread=%llu\n",
               (unsigned long long)r.count,(unsigned long long)r.enabled,
               (unsigned long long)r.running,(unsigned long long)elapsed);
        check("cpu.clock.task.time",r.count>0 && r.count<=elapsed+3000000 &&
              elapsed<=r.count+3000000 && r.running==r.enabled,1);
        close(fd);
    }
    /* Fork turns already-written anonymous pages into real COW faults on both
     * systems, avoiding assumptions about eager versus demand mmap allocation. */
    volatile unsigned char *memory=mmap(NULL,16*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    check("faults.mapping",memory!=MAP_FAILED,1);
    if(memory==MAP_FAILED) return;
    for(int i=0;i<16;i++) memory[i*4096]=1;
    pid_t pid=fork();
    if(pid==0) {
        struct attr faults=attr(1,5); struct reading r,b;
        int event=open_event(&faults,0,-1,-1,0);
        if(event<0 || ioctl(event,ENABLE,0)) _exit(1);
        for(int i=0;i<8;i++) memory[i*4096]=2;
        if(ioctl(event,DISABLE,0) || !read_event(event,&r) || r.count<8) _exit(2);
        for(int i=8;i<16;i++) memory[i*4096]=3;
        if(!read_event(event,&b) || memcmp(&r,&b,sizeof(r))) _exit(3);
        _exit(0);
    }
    int status=0; int waited=pid>0 ? waitpid(pid,&status,0) : -1;
    check("faults.cow.enabled.disabled",waited==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,1);
    int unchanged=1;
    for(int i=0;i<16;i++) if(memory[i*4096]!=1) unchanged=0;
    check("faults.parent.contents",unchanged,1);
    munmap((void *)memory,16*4096);
}
static void migration(void)
{
    cpu_set_t original, one;
    if(sched_getaffinity(0,sizeof(original),&original)) {
        check("migration.affinity.read",0,1); return;
    }
    int cpus[2]={-1,-1}, found=0;
    for(int i=0;i<CPU_SETSIZE && found<2;i++) if(CPU_ISSET(i,&original)) cpus[found++]=i;
    if(found<2) { skip("migration.two.cpus","fewer than two schedulable CPUs"); return; }
    struct attr a=attr(1,4); a.flags&=~32ULL; int fd=open_event(&a,0,-1,-1,0);
    if(fd<0 && !guest && (errno==EACCES || errno==EPERM)) {
        skip("migration.kernel","host kernel counting permission denied");
        a.flags|=32;
        fd=open_event(&a,0,-1,-1,0);
    }
    check("migration.open",fd>=0,1);
    if(fd<0) return;
    CPU_ZERO(&one); CPU_SET(cpus[0],&one);
    check("migration.first.cpu",sched_setaffinity(0,sizeof(one),&one),0);
    pause_ms(5);
    check("migration.enable",result(ioctl(fd,ENABLE,0)),0);
    CPU_ZERO(&one); CPU_SET(cpus[1],&one);
    check("migration.second.cpu",sched_setaffinity(0,sizeof(one),&one),0);
    pause_ms(5);
    struct reading r;
    check("migration.read",read_event(fd,&r),1);
    check(a.flags&32 ? "migration.filtered.zero" : "migration.positive",a.flags&32 ? r.count==0 : r.count>0,1);
    ioctl(fd,DISABLE,0); close(fd);
    check("migration.restore",sched_setaffinity(0,sizeof(original),&original),0);
}
static int two_cpus(cpu_set_t *original,int cpus[2])
{
    if(sched_getaffinity(0,sizeof(*original),original)) return 0;
    int n=0;
    for(int i=0;i<CPU_SETSIZE && n<2;i++) if(CPU_ISSET(i,original)) cpus[n++]=i;
    return n==2;
}
static int pin(int cpu)
{
    cpu_set_t one; CPU_ZERO(&one); CPU_SET(cpu,&one);
    return sched_setaffinity(0,sizeof(one),&one);
}
static void cpu_scope(void)
{
    cpu_set_t original; int cpus[2];
    if(!two_cpus(&original,cpus)) { check("scope.two.cpus",0,1); return; }
    check("scope.first.cpu",pin(cpus[0]),0);
    check("scope.first.cpu.effective",sched_getcpu()==cpus[0],1);
    struct attr a=attr(1,1);
    int fd=open_event(&a,0,cpus[1],-1,0);
    check("scope.task.open",fd>=0,1);
    if(fd>=0) {
        struct reading r;
        check("scope.task.enable",result(ioctl(fd,ENABLE,0)),0);
        work(5);
        check("scope.task.wrong.cpu.read",read_event(fd,&r),1);
        check("scope.task.wrong.cpu.zero",r.count==0 && r.enabled>0 && r.running==0,1);
        check("scope.task.second.cpu",pin(cpus[1]),0);
        check("scope.second.cpu.effective",sched_getcpu()==cpus[1],1);
        work(5); ioctl(fd,DISABLE,0);
        check("scope.task.filtered.read",read_event(fd,&r),1);
        check("scope.task.filtered.time",r.count>0 && r.running>0 && r.enabled>r.running,1);
        close(fd);
    }
    check("scope.remote.caller.cpu",pin(cpus[0]),0);
    a=attr(1,0);
    fd=open_event(&a,-1,cpus[1],-1,0);
    if(fd<0 && !guest && (errno==EACCES || errno==EPERM))
        skip("scope.system.permission","host CPU-wide counting permission denied");
    else {
        check("scope.system.open",fd>=0,1);
        if(fd>=0) {
            struct reading r,frozen,after;
            check("scope.system.enable",result(ioctl(fd,ENABLE,0)),0);
            pause_ms(15);
            check("scope.system.read",read_event(fd,&r),1);
            check("scope.system.idle.time",r.count>=10000000 && r.enabled==r.running,1);
            check("scope.system.reset",result(ioctl(fd,RESET,0)),0);
            check("scope.system.disable",result(ioctl(fd,DISABLE,0)),0);
            check("scope.system.frozen.read",read_event(fd,&frozen),1);
            work(3); pause_ms(5);
            check("scope.system.again.read",read_event(fd,&after),1);
            check("scope.system.frozen",memcmp(&frozen,&after,sizeof(after))==0,1);
            check("scope.system.reset.time",frozen.enabled>=r.enabled && frozen.running>=r.running,1);
            close(fd);
            int ok=1;
            for(int i=0;i<40;i++) {
                a=attr(1,9); a.flags&=~1ULL;
                int x=open_event(&a,-1,cpus[1],-1,0);
                struct reading zero;
                if(x<0) { ok=0; break; }
                if(ioctl(x,RESET,0) || !read_event(x,&zero) || zero.count || ioctl(x,DISABLE,0)) ok=0;
                close(x);
            }
            check("race.remote.fd.lifetime",ok,1);
        }
    }
    check("scope.affinity.restore",sched_setaffinity(0,sizeof(original),&original),0);
}

static _Atomic int remote_ready,remote_release,remote_migrate;
static int remote_tid,remote_cpu,remote_second_cpu;
static void *remote_busy(void *unused)
{
    (void)unused;
    int rc=pin(remote_cpu);
    remote_tid=(int)syscall(SYS_gettid);
    atomic_store(&remote_ready,rc ? -1 : 1);
    int turn=0;
    while(!atomic_load(&remote_release)) {
        work(1);
        if(atomic_load(&remote_migrate)) pin((turn++ & 1) ? remote_cpu : remote_second_cpu);
    }
    return NULL;
}
static void remote_hardware(void)
{
    cpu_set_t original; int cpus[2];
    if(!two_cpus(&original,cpus)) { check("hw.remote.two.cpus",0,1); return; }
    check("hw.remote.caller.cpu",pin(cpus[0]),0);
    remote_cpu=cpus[1]; remote_second_cpu=cpus[0];
    atomic_store(&remote_ready,0); atomic_store(&remote_release,0); atomic_store(&remote_migrate,0);
    pthread_t thread;
    int created=pthread_create(&thread,NULL,remote_busy,NULL);
    check("hw.remote.thread.create",created,0);
    if(created) { sched_setaffinity(0,sizeof(original),&original); return; }
    while(!atomic_load(&remote_ready)) pause_ms(1);
    check("hw.remote.thread.pin",atomic_load(&remote_ready),1);
    check("hw.remote.caller.affinity.independent",sched_getcpu()==cpus[0],1);
    cpu_set_t mask;
    check("hw.remote.target.tid.affinity",!sched_getaffinity(remote_tid,sizeof(mask),&mask) &&
          CPU_ISSET(cpus[1],&mask) && CPU_COUNT(&mask)==1,1);
    struct attr filtered=attr(0,1);
    int filter=open_event(&filtered,0,cpus[1],-1,0);
    check("hw.scope.filter.open",filter>=0,1);
    if(filter>=0) {
        struct reading r;
        ioctl(filter,ENABLE,0); work(3);
        check("hw.scope.wrong.cpu.read",read_event(filter,&r),1);
        check("hw.scope.wrong.cpu.zero",r.count==0 && r.enabled>0 && r.running==0,1);
        pin(cpus[1]); work(3); ioctl(filter,DISABLE,0);
        check("hw.scope.right.cpu.read",read_event(filter,&r),1);
        check("hw.scope.right.cpu.count",r.count>0 && r.running>0 && r.enabled>r.running,1);
        close(filter); pin(cpus[0]);
    }
    struct attr cpuattr=attr(0,1); cpuattr.flags&=~1ULL;
    int wide=open_event(&cpuattr,-1,cpus[1],-1,0);
    if(wide<0 && !guest && (errno==EACCES || errno==EPERM))
        skip("hw.scope.system.permission","host CPU-wide hardware permission denied");
    else {
        check("hw.scope.system.open",wide>=0,1);
        if(wide>=0) {
            struct reading r,frozen,after;
            pause_ms(5);
            check("hw.scope.system.live",read_event(wide,&r) && r.count>0 && r.count<(1ULL<<50),1);
            check("hw.scope.system.disable",result(ioctl(wide,DISABLE,0)),0);
            check("hw.scope.system.frozen.read",read_event(wide,&frozen),1);
            pause_ms(3);
            check("hw.scope.system.frozen",read_event(wide,&after) && !memcmp(&frozen,&after,sizeof(after)),1);
            check("hw.scope.system.reset",result(ioctl(wide,RESET,0)),0);
            check("hw.scope.system.reset.read",read_event(wide,&after) && after.count==0 &&
                  after.enabled==frozen.enabled && after.running==frozen.running,1);
            close(wide);
        }
    }
    struct attr a=attr(0,1); a.flags&=~1ULL;
    int fd=open_event(&a,remote_tid,-1,-1,0);
    check("hw.remote.open.enabled",fd>=0,1);
    if(fd>=0) {
        struct reading first,second,frozen,after;
        pause_ms(8);
        check("hw.remote.live.read",read_event(fd,&first),1);
        check("hw.remote.live.positive",first.count>0 && first.count<(1ULL<<50),1);
        check("hw.remote.reset",result(ioctl(fd,RESET,0)),0);
        pause_ms(3);
        check("hw.remote.disable",result(ioctl(fd,DISABLE,0)),0);
        check("hw.remote.frozen.read",read_event(fd,&frozen),1);
        pause_ms(5);
        check("hw.remote.frozen.again",read_event(fd,&after),1);
        check("hw.remote.frozen",memcmp(&frozen,&after,sizeof(after))==0,1);
        check("hw.remote.enable",result(ioctl(fd,ENABLE,0)),0);
        pause_ms(3);
        check("hw.remote.resumed.read",read_event(fd,&second),1);
        check("hw.remote.resumed",second.count>frozen.count && second.enabled>frozen.enabled,1);
        atomic_store(&remote_migrate,1);
        int ok=1;
        for(int i=0;i<100;i++) {
            if(!read_event(fd,&after) || after.count>=(1ULL<<50) || ioctl(fd,RESET,0) ||
               ioctl(fd,DISABLE,0) || ioctl(fd,ENABLE,0)) { ok=0; break; }
            if(!(i%10)) pause_ms(1);
        }
        check("race.hw.remote.migration.control",ok,1);
    }
    atomic_store(&remote_release,1);
    if(fd>=0) {
        int ok=1; struct reading r;
        for(int i=0;i<30;i++)
            if(!read_event(fd,&r) || ioctl(fd,RESET,0) || ioctl(fd,DISABLE,0) || ioctl(fd,ENABLE,0)) ok=0;
        check("race.hw.remote.exit.control",ok,1);
    }
    check("hw.remote.thread.join",pthread_join(thread,NULL),0);
    if(fd>=0) {
        struct reading x,y;
        check("hw.remote.exit.read",read_event(fd,&x),1);
        work(3); pause_ms(3);
        check("hw.remote.exit.again",read_event(fd,&y),1);
        check("hw.remote.exit.frozen",memcmp(&x,&y,sizeof(x))==0,1);
        close(fd);
    }
    check("hw.remote.affinity.restore",sched_setaffinity(0,sizeof(original),&original),0);
}

/* The explicit kernel fault-injection boot runs only this bounded-time suite. */
static int injected_timeout(void)
{
    char cmdline[2048]={0}; int f=open("/proc/cmdline",O_RDONLY);
    if(f>=0) { (void)read(f,cmdline,sizeof(cmdline)-1); close(f); }
    if(!guest || !strstr(cmdline,"smp_rpc_drop_cpu=1")) return 0;
    check("timeout.caller.cpu",pin(0),0);
    struct attr a=attr(1,9); int fd=open_event(&a,-1,1,-1,0);
    check("timeout.disabled.open",fd>=0,1);
    if(fd>=0) {
        for(int i=0;i<2;i++) {
            uint64_t start=ns(CLOCK_MONOTONIC); struct reading r; char name[64];
#define TIMEOUT_CHECK(label,call) do { snprintf(name,sizeof(name),"timeout.%s.%d",label,i); check(name,result(call),-ETIMEDOUT); } while(0)
            TIMEOUT_CHECK("enable",ioctl(fd,ENABLE,0));
            TIMEOUT_CHECK("read",read(fd,&r,sizeof(r)));
            TIMEOUT_CHECK("reset",ioctl(fd,RESET,0));
            TIMEOUT_CHECK("disable",ioctl(fd,DISABLE,0));
#undef TIMEOUT_CHECK
            snprintf(name,sizeof(name),"timeout.bounded.%d",i);
            check(name,ns(CLOCK_MONOTONIC)-start<2000000000ULL,1);
        }
        close(fd);
    }
    a.flags&=~1ULL;
    for(int i=0;i<70;i++) {
        char name[64]; snprintf(name,sizeof(name),"timeout.enabled.open.rollback.%d",i);
        check(name,result(open_event(&a,-1,1,-1,0)),-ETIMEDOUT);
        struct attr local=attr(1,9); struct reading r;
        int x=open_event(&local,0,-1,-1,0);
        snprintf(name,sizeof(name),"timeout.local.alive.%d",i);
        check(name,x>=0 && !ioctl(x,ENABLE,0) && read_event(x,&r) && r.count==0,1);
        if(x>=0) close(x);
    }
    return 1;
}

static void validation(void)
{
    if(!guest) return;
    struct attr a=attr(1,9);
#define ERROR(name,call,err) check(name,result(call),-(err))
    ERROR("azami.pid.missing",open_event(&a,100000000,-1,-1,0),ESRCH);
    ERROR("azami.cpu.invalid",open_event(&a,0,-2,-1,0),EINVAL);
    ERROR("azami.cpu.offline",open_event(&a,0,1024,-1,0),ENODEV);

    ERROR("azami.system.no.cpu",open_event(&a,-1,-1,-1,0),EINVAL);
    ERROR("azami.pid.invalid",open_event(&a,-2,-1,-1,0),EINVAL);
    a.flags|=2; ERROR("azami.inherit",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.flags&=~2ULL;
    a.flags|=4096; ERROR("azami.enable.exec",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.flags&=~4096ULL;
    a.flags|=1ULL<<63; ERROR("azami.flags.reserved",open_event(&a,0,-1,-1,0),EINVAL); a.flags&=~(1ULL<<63);
    a.sample=1ULL<<16; a.flags|=1ULL<<20;
    int metadata=open_event(&a,0,-1,-1,0);
    check("azami.counting.identifier.guest",metadata>=0,1);
    if(metadata>=0) close(metadata);
    a.period=1; ERROR("azami.identifier.sampling",open_event(&a,0,-1,-1,0),EOPNOTSUPP);
    a.period=0; a.sample|=1; ERROR("azami.identifier.other.sample",open_event(&a,0,-1,-1,0),EOPNOTSUPP);
    a.sample=0; a.flags&=~(1ULL<<20);
    a.flags|=1ULL<<19; ERROR("azami.exclude.host",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.flags&=~(1ULL<<19);
    a.period=1; ERROR("azami.sampling",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.period=0;
    a.format=8; ERROR("azami.groups",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.format=7;
    a.tail[16]=1; ERROR("azami.config1",open_event(&a,0,-1,-1,0),EOPNOTSUPP); a.tail[16]=0;
    a.size=63; ERROR("azami.attr.short",open_event(&a,0,-1,-1,0),E2BIG);
    check("azami.attr.size.writeback",a.size,136);
    a.size=144; a.tail[88]=1; ERROR("azami.attr.extension",open_event(&a,0,-1,-1,0),E2BIG);
    check("azami.attr.extension.writeback",a.size,136);
    a=attr(4,0x100000); ERROR("azami.raw.interrupt",open_event(&a,0,-1,-1,0),EINVAL);
    a=attr(4,0x10000); ERROR("azami.raw.privilege",open_event(&a,0,-1,-1,0),EINVAL);
    a=attr(1,7); ERROR("azami.unsupported.alignment",open_event(&a,0,-1,-1,0),ENOENT);
#undef ERROR
}
/* Fixed instruction loops isolate USR/OS filtering from clock/console calls. */
static void hardware_filters(void)
{
    if(!guest) return;
    struct attr user=attr(0,1), kernel=attr(0,1), raw=attr(4,0xc0);
    kernel.flags=1|16|64;
    int u=open_event(&user,0,-1,-1,0), k=open_event(&kernel,0,-1,-1,0),
        alias=open_event(&raw,0,-1,-1,0);
    check("azami.hw.filters.open",u>=0 && k>=0 && alias>=0,1);
    if(u<0 || k<0 || alias<0) goto out;
    struct reading ur={0},kr={0},rr={0},us={0},ks={0};
    int ok=!ioctl(u,ENABLE,0) && !ioctl(k,ENABLE,0) && !ioctl(alias,ENABLE,0);
    uint64_t value=1; unsigned loops=200000;
    __asm__ volatile("1: addq $1, %0; rolq $1, %0; decl %1; jnz 1b"
                     : "+r"(value), "+r"(loops) :: "cc", "memory");
    ok=ok && !ioctl(alias,DISABLE,0) && !ioctl(k,DISABLE,0) && !ioctl(u,DISABLE,0);
    ok=ok && read_event(u,&ur) && read_event(k,&kr) && read_event(alias,&rr);
    check("azami.hw.filters.user.phase",ok,1);
    check("azami.hw.filters.user.only",ur.count>500000 && kr.count<ur.count/20,1);
    uint64_t difference=ur.count>rr.count ? ur.count-rr.count : rr.count-ur.count;
    check("azami.hw.filters.raw.alias",rr.count>0 && difference<ur.count/20,1);
    ok=!ioctl(u,RESET,0) && !ioctl(k,RESET,0) && !ioctl(u,ENABLE,0) && !ioctl(k,ENABLE,0);
    for(unsigned i=0;i<20000;i++) {
        long result;
        __asm__ volatile("syscall" : "=a"(result) : "a"((long)SYS_getpid)
                         : "rcx", "r11", "memory");
    }
    ok=ok && !ioctl(k,DISABLE,0) && !ioctl(u,DISABLE,0) && read_event(u,&us) && read_event(k,&ks);
    check("azami.hw.filters.kernel.phase",ok,1);
    check("azami.hw.filters.kernel.only",ks.count>100000 && ks.count>kr.count*10,1);
    struct reading frozen={0};
    work(3);
    check("azami.hw.filters.disabled.freeze",read_event(k,&frozen) && !memcmp(&frozen,&ks,sizeof(ks)),1);
    printf("PFFILTER user-loop user=%llu kernel=%llu raw=%llu syscall-loop user=%llu kernel=%llu\n",
           (unsigned long long)ur.count,(unsigned long long)kr.count,(unsigned long long)rr.count,
           (unsigned long long)us.count,(unsigned long long)ks.count);
out:
    if(u>=0) close(u);
    if(k>=0) close(k);
    if(alias>=0) close(alias);
}
static void hardware(void)
{
    struct attr a=attr(0,1);
    int fd=open_event(&a,0,-1,-1,0);
    if(fd<0) {
        if(errno!=ENOENT && errno!=EACCES && errno!=EPERM && errno!=ENOSYS) {
            check("hw.unexpected.open.error",-errno,0); return;
        }
        skip("hardware.instructions",errno==ENOENT ? "PMU instructions unavailable" : "host perf permission denied");
        return;
    }
    struct reading r,frozen,after;
    check("hw.enable",result(ioctl(fd,ENABLE,0)),0);
    work(10);
    check("hw.live.read",read_event(fd,&r),1);
    check("hw.live.positive",r.count>0 && r.count<(1ULL<<50),1);
    printf("PFCOUNT instructions=%llu enabled=%llu running=%llu\n",
           (unsigned long long)r.count,(unsigned long long)r.enabled,(unsigned long long)r.running);
    /* A second thread shares the same free-running PMU slot. Its sleeping
     * intervals must not include this thread's busy-loop instructions. */
    atomic_store(&ready,0); atomic_store(&release_worker,0); worker_hardware=1;
    pthread_t t;
    int created=pthread_create(&t,NULL,worker,NULL);
    check("hw.thread.create",created,0);
    if(!created) {
        while(!atomic_load(&ready)) pause_ms(1);
        work(40); atomic_store(&release_worker,1);
        check("hw.thread.join",pthread_join(t,NULL),0);
        check("hw.thread.open",worker_fd>=0,1);
        if(worker_fd>=0) {
            check("hw.unrelated.work.excluded",worker_read.count>0 && worker_read.count<r.count,1);
            check("hw.exit.read",read_event(worker_fd,&frozen),1);
            pause_ms(10);
            check("hw.exit.again",read_event(worker_fd,&after),1);
            check("hw.exit.frozen",memcmp(&frozen,&after,sizeof(after))==0,1);
            close(worker_fd);
        }
    }
    worker_hardware=0;
    /* Adding a distinct selector must not reset the first raw counter. */
    struct attr cycles=attr(0,0);
    int other=open_event(&cycles,0,-1,-1,0);
    check("hw.second.open",other>=0,1);
    work(5);
    check("hw.slot.stable.read",read_event(fd,&after),1);
    check("hw.slot.stable",after.count>=r.count && after.count<(1ULL<<50),1);
    if(other>=0) close(other);
    check("hw.disable",result(ioctl(fd,DISABLE,0)),0);
    check("hw.frozen.read",read_event(fd,&frozen),1);
    work(5);
    check("hw.frozen.again",read_event(fd,&after),1);
    check("hw.frozen",memcmp(&frozen,&after,sizeof(after))==0,1);
    close(fd);
    remote_hardware();
    hardware_filters();
}
static int write_setting(int fd,const char *value) { return write(fd,value,strlen(value))==(ssize_t)strlen(value); }
static void security(void)
{
    if(!guest) return;
    const char *path="/proc/sys/kernel/perf_event_paranoid";
    int fd=open(path,O_RDWR); check("azami.paranoid.open",fd>=0,1);
    if(fd<0) return;
    check("azami.paranoid.negative",write_setting(fd,"-1\n"),1);
    char b[16]={0}; lseek(fd,0,SEEK_SET);
    check("azami.paranoid.read",read(fd,b,sizeof(b))>0 && !strcmp(b,"-1\n"),1);
    check("azami.paranoid.restore",write_setting(fd,"2\n"),1);
    check("azami.paranoid.invalid",result(write(fd,"2garbage",8)),-EINVAL);
    pid_t pid=fork();
    if(pid==0) {
        struct {uint32_t version,pid;} h={0x20080522,0};
        struct {uint32_t effective,permitted,inheritable;} d[2]={{0}};
        if(syscall(SYS_capset,&h,d)) _exit(1);
        struct attr a=attr(1,9); a.flags&=~32ULL;
        if(open_event(&a,0,-1,-1,0)!=-1 || errno!=EACCES) _exit(2);
        a.flags|=32;
        int event=open_event(&a,0,-1,-1,0);
        if(event<0) _exit(3);
        close(event);
        if(write(fd,"-1\n",3)!=-1 || errno!=EPERM) _exit(4);
        if(open_event(&a,-1,0,-1,0)!=-1 || errno!=EACCES) _exit(5);
        _exit(0);
    }
    int status=0; int waited=pid>0 ? waitpid(pid,&status,0) : -1;
    check("azami.permissions.dropped.root.caps",waited==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,1);
    close(fd);
}
static int set_caps(uint64_t caps)
{
    struct {uint32_t version,pid;} h={0x20080522,0};
    struct {uint32_t effective,permitted,inheritable;} d[2]={
        {(uint32_t)caps,(uint32_t)caps,0},
        {(uint32_t)(caps>>32),(uint32_t)(caps>>32),0}};
    return (int)syscall(SYS_capset,&h,d);
}
static void other_credentials(void)
{
    if(!guest) return;
    pid_t subject=fork();
    if(subject==0) {
        if(syscall(SYS_prctl,4,2,0,0,0)!=-1 || errno!=EINVAL) _exit(1);
        if(syscall(SYS_prctl,4,0,0,0,0)) _exit(2);
        pid_t child=fork();
        if(child==0) {
            if(syscall(SYS_prctl,3,0,0,0,0)!=0) _exit(3);
            execl("/sbin/init.elf","/sbin/init.elf","--dumpable-exec-check",(char *)NULL);
            _exit(4);
        }
        int status=0;
        if(child<0 || waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) _exit(5);
        execl("/definitely-missing-perf-image","missing",(char *)NULL);
        if(errno!=ENOENT || syscall(SYS_prctl,3,0,0,0,0)!=0) _exit(6);
        if(syscall(SYS_prctl,4,1,0,0,0) || syscall(SYS_setresgid,1001,1001,1001) ||
           syscall(SYS_prctl,3,0,0,0,0)!=0) _exit(7);
        if(syscall(SYS_prctl,4,1,0,0,0) || syscall(SYS_setresuid,1001,1001,1001) ||
           syscall(SYS_prctl,3,0,0,0,0)!=0) _exit(8);
        _exit(0);
    }
    int subject_status=0; int subject_wait=subject>0 ? waitpid(subject,&subject_status,0) : -1;
    check("azami.dumpable.fork.exec.credentials",subject_wait==subject && WIFEXITED(subject_status) &&
          WEXITSTATUS(subject_status)==0,1);
    int release[4][2]; pid_t target[4]; int made=0;
    for(int i=0;i<4;i++) {
        int notice[2];
        if(pipe(notice) || pipe(release[i])) break;
        target[i]=fork();
        if(target[i]==0) {
            close(notice[0]); close(release[i][1]);
            int ok=1;
            if(i==1 && (syscall(SYS_setresgid,1001,1001,1001) ||
                        syscall(SYS_setresuid,1001,1001,1001))) ok=0;
            if(i!=3 && set_caps(0)) ok=0;
            if(syscall(SYS_prctl,4,i==2 ? 0 : 1,0,0,0)) ok=0;
            if(syscall(SYS_prctl,3,0,0,0,0)!=(i==2 ? 0 : 1)) ok=0;
            unsigned char value=ok;
            if(write(notice[1],&value,1)!=1) _exit(10);
            close(notice[1]);
            if(read(release[i][0],&value,1)!=1) _exit(11);
            work(3); _exit(ok ? 0 : 12);
        }
        close(notice[1]); close(release[i][0]);
        unsigned char value=0;
        int ready_ok=target[i]>0 && read(notice[0],&value,1)==1 && value;
        close(notice[0]);
        if(!ready_ok) break;
        made++;
    }
    check("azami.credentials.targets",made,4);
    if(made==4) {
        for(int mode=0;mode<3;mode++) {
            pid_t observer=fork();
            if(observer==0) {
                uint64_t caps=mode==0 ? 0 : mode==1 ? (1ULL<<19) : (1ULL<<38);
                if(set_caps(caps)) _exit(20);
                for(int i=0;i<4;i++) {
                    struct attr a=attr(1,9);
                    int fd=open_event(&a,target[i],-1,-1,0);
                    int allowed=mode!=0 || i==0;
                    if(allowed) { if(fd<0) _exit(21+i); close(fd); }
                    else if(fd!=-1 || errno!=EACCES) _exit(25+i);
                }
                struct attr a=attr(1,9); a.flags&=~32ULL;
                int kernel=open_event(&a,0,-1,-1,0);
                if(mode==2) { if(kernel<0) _exit(30); close(kernel); }
                else if(kernel!=-1 || errno!=EACCES) _exit(31);
                a.flags|=32;
                int cpu=open_event(&a,-1,0,-1,0);
                if(mode==2) { if(cpu<0) _exit(32); close(cpu); }
                else if(cpu!=-1 || errno!=EACCES) _exit(33);
                int sysctl=open("/proc/sys/kernel/perf_event_paranoid",O_WRONLY);
                if(sysctl<0 || write(sysctl,"-1\n",3)!=-1 || errno!=EPERM) _exit(34);
                _exit(0);
            }
            int status=0; int waited=observer>0 ? waitpid(observer,&status,0) : -1;
            check(mode==0 ? "azami.credentials.no.caps" : mode==1 ? "azami.credentials.ptrace.only" :
                  "azami.credentials.perfmon.only",waited==observer && WIFEXITED(status) && WEXITSTATUS(status)==0,1);
        }
    }
    for(int i=0;i<made;i++) {
        struct attr a=attr(1,1); a.flags&=~1ULL;
        int fd=open_event(&a,target[i],-1,-1,0);
        unsigned char go=1; (void)write(release[i][1],&go,1); close(release[i][1]);
        int status=0; int waited=waitpid(target[i],&status,0); struct reading x,y;
        int good=waited==target[i] && WIFEXITED(status) && WEXITSTATUS(status)==0 &&
                 fd>=0 && read_event(fd,&x) && x.count>0;
        work(2);
        good=good && read_event(fd,&y) && memcmp(&x,&y,sizeof(x))==0;
        char name[64]; snprintf(name,sizeof(name),"azami.credentials.privileged.exit.%d",i);
        check(name,good,1); if(fd>=0) close(fd);
    }
}

static void exec_lifetime(const char *self)
{
    struct attr a=attr(1,9);
    int keep=open_event(&a,0,-1,-1,0), clo=open_event(&a,0,-1,-1,8);
    check("exec.fds.open",keep>=0 && clo>=0,1);
    if(keep<0 || clo<0) { if(keep>=0) close(keep); if(clo>=0) close(clo); return; }
    pid_t child=fork();
    if(child==0) {
        char k[24],c[24]; snprintf(k,sizeof(k),"%d",keep); snprintf(c,sizeof(c),"%d",clo);
        execl(self,self,"--exec-check",k,c,(char *)NULL); _exit(80);
    }
    int status=0; int waited=child>0 ? waitpid(child,&status,0) : -1;
    check("fork.exec.fd.lifetime",waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0,1);
    struct reading r; check("fork.parent.fd.alive",read_event(keep,&r),1);
    close(keep); close(clo);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--dumpable-exec-check"))
        return syscall(SYS_prctl,3,0,0,0,0)==1 ? 0 : 82;
    if(argc==4 && !strcmp(argv[1],"--exec-check")) {
        int keep=atoi(argv[2]),clo=atoi(argv[3]); struct reading r;
        if(fcntl(clo,F_GETFD)!=-1 || errno!=EBADF || !read_event(keep,&r)) return 81;
        return 0;
    }
    setvbuf(stdout,NULL,_IONBF,0); alarm(45);
    struct utsname u; guest=uname(&u)==0 && strstr(u.sysname,"Azami")!=NULL;
    if(injected_timeout()) {
        printf("== namespaces skipped: %d ==\n",skipped);
        printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
        return failed?1:0;
    }
    struct attr a=attr(1,9); int probe=open_event(&a,0,-1,-1,0);
    if(probe<0 && !guest && (errno==EPERM || errno==EACCES || errno==ENOSYS)) {
        /* This host cannot provide counting observations; preserve the reason. */
        skip("software.counting","host perf permission denied");
        check("host.perf.permission.denied",1,1);
    } else {
        if(probe>=0) close(probe);
        basic(); thread_scope(); stress(); occurrences(); migration(); cpu_scope(); exec_lifetime(guest?"/sbin/init.elf":"/proc/self/exe");
    }
    validation(); security(); other_credentials(); hardware();
    printf("== namespaces skipped: %d ==\n",skipped);
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
    return failed?1:0;
}
