#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define PAGES 4096
#define PAGE_BYTES 4096
#define MAP_BYTES ((size_t)PAGES * PAGE_BYTES)
#define ROUND_NS UINT64_C(30000000000)
#define TOTAL_NS UINT64_C(300000000000)
#define MAGIC UINT32_C(0x504d4d31)

/* Fork supplies independent mm/FD tables. Each worker blocks in its private
 * command pipe while snapshots are taken; maps survive the held barrier.
 * These are mapping/first-touch measurements, never inferred fault counts or
 * mixed-order buddy measurements. Worker timers are not aggregate throughput. */
struct channel { int command[2], response[2]; pid_t pid; int cpu; };
struct result {
    uint32_t magic, phase, worker;
    int32_t repetition, cpu, unmap_cpu;
    uint64_t mmap_ns, touch_ns, unmap_ns, checksum;
};
static volatile sig_atomic_t stopped;
static int passed, failed;
static uint64_t total_deadline;

static void stop(int signo) { (void)signo; stopped=1; }
static int now(uint64_t *value)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts) || ts.tv_sec<0 || ts.tv_nsec<0 ||
       ts.tv_nsec>=1000000000L || (uint64_t)ts.tv_sec>UINT64_MAX/1000000000)
        return 0;
    *value=(uint64_t)ts.tv_sec*1000000000+(uint64_t)ts.tv_nsec;
    return 1;
}
static int duration(uint64_t start, uint64_t *elapsed)
{
    uint64_t end;
    if(!now(&end) || end<=start) return 0;
    *elapsed=end-start; return 1;
}
static int transfer(int fd, void *data, size_t size, int writing, uint64_t deadline)
{
    unsigned char *bytes=data; size_t done=0;
    while(done<size) {
        uint64_t current;
        if(stopped || !now(&current) || current>=deadline) return 0;
        uint64_t left=(deadline-current+999999)/1000000;
        int timeout=left>INT_MAX ? INT_MAX : (int)left;
        struct pollfd pfd={.fd=fd,.events=writing ? POLLOUT : POLLIN};
        int ready=poll(&pfd,1,timeout);
        if(ready<0 && errno==EINTR) continue;
        if(ready<=0 || !(pfd.revents & pfd.events)) return 0;
        ssize_t count=writing ? write(fd,bytes+done,size-done) : read(fd,bytes+done,size-done);
        if(count<0 && errno==EINTR) continue;
        if(count<=0) return 0;
        done+=(size_t)count;
    }
    return 1;
}
static uint64_t checksum(unsigned worker)
{
    uint64_t sum=0;
    for(unsigned page=0;page<PAGES;page++) sum+=(page+worker*17)%251+1;
    return sum;
}
static int worker_loop(struct channel *channel, unsigned worker)
{
    cpu_set_t affinity; CPU_ZERO(&affinity); CPU_SET(channel->cpu,&affinity);
    if(sched_setaffinity(0,sizeof(affinity),&affinity) || sched_getcpu()!=channel->cpu)
        return 10;
    struct result result={.magic=MAGIC,.phase=0,.worker=worker,.cpu=sched_getcpu()};
    uint64_t clock;
    if(!now(&clock) || !transfer(channel->response[1],&result,sizeof(result),1,clock+ROUND_NS))
        return 11;
    volatile unsigned char *memory=MAP_FAILED;
    int error=12;
    for(int repetition=-1;repetition<10;repetition++) {
        unsigned char command;
        if(!now(&clock) || !transfer(channel->command[0],&command,1,0,clock+ROUND_NS) || command!='M')
            goto out;
        memset(&result,0,sizeof(result));
        result.magic=MAGIC; result.phase=1; result.worker=worker;
        result.repetition=repetition; result.cpu=sched_getcpu();
        if(result.cpu!=channel->cpu || !now(&clock)) goto out;
        memory=mmap(NULL,MAP_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(memory==MAP_FAILED || !duration(clock,&result.mmap_ns) || !now(&clock)) goto out;
        for(unsigned page=0;page<PAGES;page++) {
            size_t index=(size_t)page*PAGE_BYTES;
            if(memory[index] || memory[index+1] || memory[index+PAGE_BYTES-1]) goto out;
            memory[index]=(unsigned char)((page+worker*17)%251+1);
        }
        if(!duration(clock,&result.touch_ns)) goto out;
        for(unsigned page=0;page<PAGES;page++) {
            size_t index=(size_t)page*PAGE_BYTES;
            unsigned char value=memory[index];
            if(value!=(unsigned char)((page+worker*17)%251+1) ||
               memory[index+1] || memory[index+PAGE_BYTES-1]) goto out;
            result.checksum+=value;
        }
        if(result.checksum!=checksum(worker) || sched_getcpu()!=channel->cpu ||
           !now(&clock) || !transfer(channel->response[1],&result,sizeof(result),1,clock+ROUND_NS))
            goto out;
        if(!now(&clock) || !transfer(channel->command[0],&command,1,0,clock+ROUND_NS) || command!='U')
            goto out;
        /* Recheck while the held barrier is released: the mapping and its
         * contents must remain owned by this worker until unmap. */
        for(unsigned page=0;page<PAGES;page++) {
            size_t index=(size_t)page*PAGE_BYTES;
            if(memory[index]!=(unsigned char)((page+worker*17)%251+1) ||
               memory[index+1] || memory[index+PAGE_BYTES-1]) goto out;
        }
        result.unmap_cpu=sched_getcpu();
        if(result.unmap_cpu!=channel->cpu || !now(&clock) || munmap((void *)memory,MAP_BYTES)) goto out;
        memory=MAP_FAILED;
        if(!duration(clock,&result.unmap_ns)) goto out;
        result.phase=2;
        if(!now(&clock) || !transfer(channel->response[1],&result,sizeof(result),1,clock+ROUND_NS)) goto out;
    }
    unsigned char command;
    if(!now(&clock) || !transfer(channel->command[0],&command,1,0,clock+ROUND_NS) || command!='X') goto out;
    error=0;
out:
    if(memory!=MAP_FAILED && munmap((void *)memory,MAP_BYTES)) error=13;
    return error;
}

enum metadata_status { META_OK, META_ABSENT, META_UNPARSEABLE, META_READ_ERROR };
static const char *status_name(enum metadata_status status)
{
    static const char *const names[]={"ok","absent","unparseable","read_error"};
    return names[status];
}
static enum metadata_status open_status(void)
{
    return errno==ENOENT || errno==ENOTDIR ? META_ABSENT : META_READ_ERROR;
}
static enum metadata_status memfree(uint64_t *bytes)
{
    FILE *file=fopen("/proc/meminfo","r");
    if(!file) return open_status();
    enum metadata_status status=META_UNPARSEABLE;
    char line[512]; int found=0;
    while(fgets(line,sizeof(line),file)) {
        if(strncmp(line,"MemFree:",8)) continue;
        if(found++) { status=META_UNPARSEABLE; break; }
        const char *p=line+8;
        while(*p==' ' || *p=='\t') p++;
        char *end; errno=0;
        if(*p<'0' || *p>'9') break;
        unsigned long long kb=strtoull(p,&end,10);
        if(errno || kb>INT64_MAX/1024) break;
        p=end; while(*p==' ' || *p=='\t') p++;
        if(strncmp(p,"kB",2)) break;
        p+=2; while(*p==' ' || *p=='\t' || *p=='\n') p++;
        if(*p) break;
        *bytes=(uint64_t)kb*1024; status=META_OK;
    }
    if(ferror(file)) status=META_READ_ERROR;
    if(fclose(file)) status=META_READ_ERROR;
    return status;
}
static enum metadata_status buddy(uint64_t *bytes)
{
    FILE *file=fopen("/proc/buddyinfo","r");
    if(!file) return open_status();
    enum metadata_status status=META_UNPARSEABLE;
    char line[4096]; uint64_t pages=0; unsigned rows=0, expected_orders=0;
    while(fgets(line,sizeof(line),file)) {
        unsigned node; char zone[64]; int prefix=0;
        if(!strchr(line,'\n') || sscanf(line,"Node %u, zone %63s %n",&node,zone,&prefix)!=2 || !prefix)
            goto out;
        const char *p=line+prefix; unsigned order=0;
        for(;;) {
            while(*p==' ' || *p=='\t' || *p=='\n') p++;
            if(!*p) break;
            if(*p<'0' || *p>'9' || order>=32) goto out;
            char *end; errno=0; unsigned long long blocks=strtoull(p,&end,10);
            if(errno || blocks>((uint64_t)INT64_MAX/PAGE_BYTES-pages)/(UINT64_C(1)<<order)) goto out;
            pages+=(uint64_t)blocks<<order; order++; p=end;
            if(*p && *p!=' ' && *p!='\t' && *p!='\n') goto out;
        }
        if(!order || (rows && order!=expected_orders)) goto out;
        expected_orders=order; rows++;
    }
    if(rows) { *bytes=pages*PAGE_BYTES; status=META_OK; }
out:
    if(ferror(file)) status=META_READ_ERROR;
    if(fclose(file)) status=META_READ_ERROR;
    return status;
}
static int snapshot(unsigned workers, int repetition, const char *phase)
{
    uint64_t free_bytes=0, buddy_bytes=0, clock;
    if(stopped || !now(&clock) || clock>=total_deadline) return 0;
    enum metadata_status ms=memfree(&free_bytes), bs=buddy(&buddy_bytes);
    const char *available=ms==META_OK && bs==META_OK ? "ok" :
        ms==META_OK || bs==META_OK ? "partial" : "unavailable";
    printf("PMMSNAPSHOT {\"workers\":%u,\"repetition\":%d,\"phase\":\"%s\","
           "\"available\":\"%s\",\"memfree_status\":\"%s\",\"buddy_status\":\"%s\",\"memfree_bytes\":",
           workers,repetition,phase,available,status_name(ms),status_name(bs));
    if(ms==META_OK) printf("%" PRIu64,free_bytes); else printf("null");
    printf(",\"buddy_bytes\":");
    if(bs==META_OK) printf("%" PRIu64,buddy_bytes); else printf("null");
    printf(",\"estimate\":");
    if(ms==META_OK && bs==META_OK) printf("%" PRId64,(int64_t)free_bytes-(int64_t)buddy_bytes);
    else printf("null");
    puts("}");
    return !stopped && now(&clock) && clock<total_deadline;
}
static void close_channels(struct channel *channels, unsigned count)
{
    for(unsigned i=0;i<count;i++) {
        for(unsigned end=0;end<2;end++) {
            if(channels[i].command[end]>=0) { close(channels[i].command[end]); channels[i].command[end]=-1; }
            if(channels[i].response[end]>=0) { close(channels[i].response[end]); channels[i].response[end]=-1; }
        }
    }
}
static int reap(struct channel *channels, unsigned count, uint64_t deadline)
{
    int good=1;
    for(unsigned i=0;i<count;i++) {
        if(channels[i].pid<=0) continue;
        for(;;) {
            int status=0; pid_t child=waitpid(channels[i].pid,&status,WNOHANG);
            if(child==channels[i].pid) {
                if(!WIFEXITED(status) || WEXITSTATUS(status)) good=0;
                channels[i].pid=0; break;
            }
            if(child<0 && errno!=EINTR) { good=0; break; }
            uint64_t clock;
            if(stopped || !now(&clock) || clock>=deadline) { good=0; break; }
            struct timespec delay={.tv_nsec=1000000};
            if(nanosleep(&delay,NULL) && errno!=EINTR) { good=0; break; }
        }
    }
    return good;
}
static int valid(const struct result *r, unsigned worker, int cpu, int repetition, uint32_t phase)
{
    return r->magic==MAGIC && r->phase==phase && r->worker==worker &&
        r->repetition==repetition && r->cpu==cpu && r->mmap_ns && r->touch_ns &&
        r->checksum==checksum(worker) && (phase!=2 || (r->unmap_cpu==cpu && r->unmap_ns));
}
static int scenario(unsigned count, const int *cpus)
{
    struct channel channels[WORKERS]; struct result results[WORKERS];
    for(unsigned i=0;i<WORKERS;i++) {
        channels[i]=(struct channel){.command={-1,-1},.response={-1,-1},.cpu=cpus[i]};
    }
    const char *error="pipe"; int good=0;
    for(unsigned i=0;i<count;i++) if(pipe(channels[i].command) || pipe(channels[i].response)) goto cleanup;
    error="fork";
    for(unsigned i=0;i<count;i++) {
        pid_t child=fork();
        if(child<0) goto cleanup;
        if(!child) {
            signal(SIGINT,SIG_DFL); signal(SIGTERM,SIG_DFL); signal(SIGALRM,SIG_DFL);
            for(unsigned j=0;j<count;j++) {
                if(j!=i) { close(channels[j].command[0]); close(channels[j].response[1]); }
                close(channels[j].command[1]); close(channels[j].response[0]);
            }
            _exit(worker_loop(&channels[i],i));
        }
        channels[i].pid=child;
    }
    for(unsigned i=0;i<count;i++) {
        close(channels[i].command[0]); channels[i].command[0]=-1;
        close(channels[i].response[1]); channels[i].response[1]=-1;
    }
    error="worker_ready";
    for(unsigned i=0;i<count;i++) {
        if(!transfer(channels[i].response[0],&results[i],sizeof(results[i]),0,total_deadline) ||
           results[i].magic!=MAGIC || results[i].phase!=0 || results[i].worker!=i || results[i].cpu!=cpus[i])
            goto cleanup;
    }
    for(int repetition=-1;repetition<10;repetition++) {
        uint64_t clock;
        if(!now(&clock)) { error="clock"; goto cleanup; }
        uint64_t deadline=clock+ROUND_NS;
        if(deadline>total_deadline) deadline=total_deadline;
        error="snapshot_before";
        if(!snapshot(count,repetition,"before")) goto cleanup;
        unsigned char command='M'; error="map_start";
        for(unsigned i=0;i<count;i++) if(!transfer(channels[i].command[1],&command,1,1,deadline)) goto cleanup;
        error="allocation_verification";
        for(unsigned i=0;i<count;i++)
            if(!transfer(channels[i].response[0],&results[i],sizeof(results[i]),0,deadline) ||
               !valid(&results[i],i,cpus[i],repetition,1)) goto cleanup;
        error="snapshot_held";
        if(!snapshot(count,repetition,"held")) goto cleanup;
        command='U'; error="unmap_start";
        for(unsigned i=0;i<count;i++) if(!transfer(channels[i].command[1],&command,1,1,deadline)) goto cleanup;
        error="unmap_verification";
        for(unsigned i=0;i<count;i++)
            if(!transfer(channels[i].response[0],&results[i],sizeof(results[i]),0,deadline) ||
               !valid(&results[i],i,cpus[i],repetition,2)) goto cleanup;
        error="snapshot_after";
        if(!snapshot(count,repetition,"after")) goto cleanup;
        for(unsigned i=0;i<count;i++) {
            struct result *r=&results[i];
            printf("PMMBENCH {\"workers\":%u,\"repetition\":%d,\"worker\":%u,\"requested_cpu\":%d,"
                   "\"cpu\":%d,\"unmap_cpu\":%d,\"pages\":%d,\"bytes\":%zu,\"checksum\":%" PRIu64 ","
                   "\"mmap_ns\":%" PRIu64 ",\"touch_ns\":%" PRIu64 ",\"unmap_ns\":%" PRIu64 ",\"passed\":true}\n",
                   count,repetition,i,cpus[i],r->cpu,r->unmap_cpu,PAGES,MAP_BYTES,r->checksum,
                   r->mmap_ns,r->touch_ns,r->unmap_ns);
            passed++;
        }
    }
    error="worker_exit";
    unsigned char command='X';
    for(unsigned i=0;i<count;i++) if(!transfer(channels[i].command[1],&command,1,1,total_deadline)) goto cleanup;
    if(!reap(channels,count,total_deadline)) goto cleanup;
    good=1;
cleanup:
    if(!good) {
        printf("PMMFAIL {\"workers\":%u,\"operation\":\"%s\",\"passed\":false}\n",count,error);
        failed++;
        for(unsigned i=0;i<count;i++) if(channels[i].pid>0) kill(channels[i].pid,SIGKILL);
    }
    close_channels(channels,count);
    /* Kill removes each child's mappings even if it failed before sending a
     * response. Reap every child before allowing another scenario to start. */
    for(unsigned i=0;i<count;i++) if(channels[i].pid>0) {
        pid_t waited;
        do { waited=waitpid(channels[i].pid,NULL,0); } while(waited<0 && errno==EINTR);
        if(waited!=channels[i].pid) { if(good) failed++; good=0; }
        channels[i].pid=0;
    }
    return good;
}
static int selected_cpus(int argc, char **argv, int *cpus)
{
    cpu_set_t allowed;
    if(sched_getaffinity(0,sizeof(allowed),&allowed)) return 0;
    int count=0;
    if(argc==2) {
        const char *p=argv[1];
        for(;;) {
            if(*p<'0' || *p>'9' || count==WORKERS) return 0;
            char *end; errno=0; unsigned long cpu=strtoul(p,&end,10);
            if(errno || cpu>=CPU_SETSIZE || !CPU_ISSET(cpu,&allowed)) return 0;
            for(int i=0;i<count;i++) if(cpus[i]==(int)cpu) return 0;
            cpus[count++]=(int)cpu; p=end;
            if(!*p) break;
            if(*p++!=',') return 0;
        }
    } else if(argc==1) {
        for(int cpu=0;cpu<CPU_SETSIZE && count<WORKERS;cpu++)
            if(CPU_ISSET(cpu,&allowed)) cpus[count++]=cpu;
    } else return 0;
    return count;
}
int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    struct sigaction action={.sa_handler=stop}; sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || signal(SIGPIPE,SIG_IGN)==SIG_ERR) {
        puts("PMMFAIL {\"operation\":\"signals\",\"passed\":false}"); failed++; goto verdict;
    }
    int cpus[WORKERS]={0}; int count=selected_cpus(argc,argv,cpus);
    uint64_t clock; struct timespec resolution;
    if(!count || sysconf(_SC_PAGESIZE)!=PAGE_BYTES || !now(&clock) ||
       clock>UINT64_MAX-TOTAL_NS || clock_getres(CLOCK_MONOTONIC,&resolution) ||
       resolution.tv_sec<0 || resolution.tv_nsec<0 || resolution.tv_nsec>=1000000000L) {
        puts("PMMFAIL {\"operation\":\"environment\",\"passed\":false}"); failed++; goto verdict;
    }
    total_deadline=clock+TOTAL_NS;
    printf("PMMENV {\"mapping_bytes_per_worker\":%zu,\"pages_per_worker\":%d,\"page_bytes\":%d,"
           "\"warmup_rounds\":1,\"measured_rounds\":10,\"round_timeout_ns\":%" PRIu64 ","
           "\"total_timeout_ns\":%" PRIu64 ",\"timer_resolution_ns\":%" PRIu64 ",\"cpus\":[",
           MAP_BYTES,PAGES,PAGE_BYTES,ROUND_NS,TOTAL_NS,
           (uint64_t)resolution.tv_sec*1000000000+(uint64_t)resolution.tv_nsec);
    for(int i=0;i<count;i++) printf("%s%d",i ? "," : "",cpus[i]);
    puts("]}");
    if(!scenario(1,cpus)) goto verdict;
    if(count==WORKERS) scenario(WORKERS,cpus);
    else printf("PMMSKIP {\"workers\":4,\"reason\":\"fewer than four selected allowed CPUs\"}\n");
verdict:
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
    return failed ? 1 : 0;
}
