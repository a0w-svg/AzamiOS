#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "perf-stat-csv.h"

static int passed, failed, skipped;
static void check(const char *name, int good)
{
    printf("NSCASE {\"name\":\"%s\",\"value\":%d,\"expected\":1,\"passed\":%s}\n",
           name, !!good, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static void counting(const char *event, const char *name, pid_t worker)
{
    int output[2];
    if (pipe(output)) { check("stock.pipe", 0); return; }
    pid_t child = fork();
    if (!child) {
        close(output[0]);
        if (dup2(output[1], 1)<0 || dup2(output[1], 2)<0) _exit(91);
        close(output[1]);
        char tid[32]; snprintf(tid,sizeof(tid),"%d",worker);
        execl("/usr/bin/perf", "perf", "stat", "-vv", "--no-inherit", "--no-big-num",
              "-x", ";", "-e", event, "-t", tid, "--timeout", "300", (char *)0);
        _exit(92);
    }
    close(output[1]);
    char log[32768]; size_t used=0; int truncated=0;
    for (;;) {
        char part[1024]; ssize_t n=read(output[0],part,sizeof(part));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        size_t copy=(size_t)n;
        if (copy>sizeof(log)-1-used) { copy=sizeof(log)-1-used; truncated=1; }
        memcpy(log+used,part,copy); used+=copy;
    }
    close(output[0]); log[used]=0;
    int status=0; int waited=child>0 ? waitpid(child,&status,0) : -1;
    printf("STOCKPERF %s BEGIN\n%s\nSTOCKPERF %s END\n",name,log,name);
    char label[96];
    snprintf(label,sizeof(label),"stock.%s.exit",name);
    check(label,waited==child && WIFEXITED(status) && !WEXITSTATUS(status));
    snprintf(label,sizeof(label),"stock.%s.log.complete",name); check(label,!truncated);
    /* CSV must include a positive count and positive time-running percentage.
     * Exit zero alone is insufficient: upstream can print <not counted>. */
    int positive=0;
    for (char *line=log; line && *line;) {
        char *next=strchr(line,'\n'); if (next) *next=0;
        if (perf_stat_csv_counted(line,event)) positive=1;
        line=next ? next+1 : NULL;
    }
    snprintf(label,sizeof(label),"stock.%s.positive",name); check(label,positive);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); alarm(15);
    pid_t worker=fork();
    if(!worker) {
        uint64_t value=1;
        for(;;) __asm__ volatile("addq $1, %0; rolq $1, %0" : "+r"(value) :: "cc");
    }
    check("stock.worker",worker>0);
    if(worker>0) {
        counting("task-clock:u","task-clock",worker);
        struct {uint32_t type,size; uint64_t config,period,sample,format,flags; char tail[80];}
            a={.type=0,.size=128,.config=1,.flags=1|32|64};
        int fd=(int)syscall(SYS_perf_event_open,&a,0,-1,-1,0);
        if(fd>=0) {
            close(fd);
            counting("instructions:u","instructions",worker);
            counting("rC0:u","raw",worker);
        }
        else if(errno==ENOENT || errno==EACCES || errno==EPERM) {
            skipped++;
            printf("NSSKIP {\"name\":\"stock.instructions\",\"reason\":\"PMU unavailable or permission denied\"}\n");
        } else check("stock.hardware.open",0);
        kill(worker,SIGKILL); waitpid(worker,NULL,0);
    }
    printf("== namespaces skipped: %d ==\n",skipped);
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
    return failed ? 1 : 0;
}
