#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The kernel ABI uses next-entry cookies, not byte offsets. This probe uses
 * getdents64 directly so libc's buffering cannot hide broken continuation. */
struct raw_dirent {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};
#define MAX_ENTRIES 4096
#define MAX_CPUS 4096
struct entry { int64_t cookie; unsigned char type; char name[256]; };
struct listing { size_t count, calls; struct entry entries[MAX_ENTRIES]; };
static int passed, failed, skipped;

static void check(const char *name, int good)
{
    printf("NSCASE {\"name\":\"%s\",\"value\":%d,\"expected\":1,\"passed\":%s}\n",
           name, !!good, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static int read_text(const char *path, char *out, size_t capacity)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0) return 0;
    size_t used=0;
    while(used+1<capacity) {
        ssize_t n=read(fd,out+used,capacity-used-1);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { close(fd); return 0; }
        if(!n) break;
        used+=(size_t)n;
    }
    char extra;
    ssize_t more=read(fd,&extra,1);
    close(fd); out[used]=0;
    return used && more==0;
}
static int cpu_list(const char *text, unsigned char *set)
{
    memset(set,0,MAX_CPUS);
    const char *p=text;
    for(;;) {
        char *end;
        if(*p<'0' || *p>'9') return 0;
        errno=0; unsigned long first=strtoul(p,&end,10), last=first;
        if(errno || first>=MAX_CPUS) return 0;
        p=end;
        if(*p=='-') {
            p++;
            if(*p<'0' || *p>'9') return 0;
            last=strtoul(p,&end,10); p=end;
            if(errno || last<first || last>=MAX_CPUS) return 0;
        }
        for(unsigned long cpu=first;cpu<=last;cpu++) {
            if(set[cpu]) return 0;
            set[cpu]=1;
        }
        if(*p==',') { p++; continue; }
        while(*p=='\n' || *p==' ' || *p=='\t') p++;
        return !*p;
    }
}
static int collect(int fd, size_t size, struct listing *list)
{
    union { uint64_t align; unsigned char bytes[4096]; } buffer;
    memset(list,0,sizeof(*list));
    for(size_t guard=0;guard<MAX_ENTRIES+1;guard++) {
        long bytes=syscall(SYS_getdents64,fd,buffer.bytes,size);
        if(bytes<0 && errno==EINTR) { guard--; continue; }
        if(bytes<0 || (size_t)bytes>size) return 0;
        if(!bytes) return 1;
        list->calls++;
        for(size_t pos=0;pos<(size_t)bytes;) {
            size_t remaining=(size_t)bytes-pos;
            if(remaining<offsetof(struct raw_dirent,name)+1) return 0;
            struct raw_dirent *d=(struct raw_dirent *)(buffer.bytes+pos);
            if(d->reclen<offsetof(struct raw_dirent,name)+1 ||
               d->reclen>remaining || d->reclen%8 || !d->ino || d->off<=0)
                return 0;
            size_t capacity=d->reclen - offsetof(struct raw_dirent, name);
            const char *zero=memchr(d->name,0,capacity);
            if(!zero || zero==d->name || (size_t)(zero-d->name)>=256 ||
               list->count>=MAX_ENTRIES) return 0;
            for(size_t i=0;i<list->count;i++)
                if(!strcmp(list->entries[i].name,d->name)) return 0;
            struct entry *entry=&list->entries[list->count++];
            strcpy(entry->name,d->name); entry->cookie=d->off; entry->type=d->type;
            pos+=d->reclen;
        }
    }
    return 0;
}
static int same_suffix(const struct listing *full, size_t start, const struct listing *tail)
{
    if(full->count-start!=tail->count) return 0;
    for(size_t i=0;i<tail->count;i++)
        if(strcmp(full->entries[start+i].name,tail->entries[i].name) ||
           full->entries[start+i].type!=tail->entries[i].type ||
           full->entries[start+i].cookie!=tail->entries[i].cookie) return 0;
    return 1;
}
static int has(const struct listing *list, const char *name)
{
    for(size_t i=0;i<list->count;i++) if(!strcmp(list->entries[i].name,name)) return 1;
    return 0;
}
static struct listing *directory(const char *path, const char *label, size_t size)
{
    struct listing *full=calloc(1,sizeof(*full)), *again=calloc(1,sizeof(*again));
    char name[128]; int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    snprintf(name,sizeof(name),"sysfs.%s.open",label); check(name,fd>=0 && full && again);
    if(fd<0 || !full || !again) { if(fd>=0) close(fd); free(full); free(again); return NULL; }
    char tiny;
    errno=0; long short_result=syscall(SYS_getdents64,fd,&tiny,1);
    snprintf(name,sizeof(name),"sysfs.%s.too.small",label);
    check(name,short_result==-1 && errno==EINVAL);
    int good=collect(fd,size,full);
    snprintf(name,sizeof(name),"sysfs.%s.pagination",label); check(name,good && full->calls>0);
    snprintf(name,sizeof(name),"sysfs.%s.dot.entries",label); check(name,has(full,".") && has(full,".."));
    snprintf(name,sizeof(name),"sysfs.%s.eof.repeat",label);
    check(name,collect(fd,size,again) && !again->count);
    snprintf(name,sizeof(name),"sysfs.%s.rewind",label);
    check(name,lseek(fd,0,SEEK_SET)==0 && collect(fd,size,again) && same_suffix(full,0,again));
    size_t start=full->count/2;
    int64_t cookie=start ? full->entries[start-1].cookie : 0;
    snprintf(name,sizeof(name),"sysfs.%s.cookie.seek",label);
    check(name,lseek(fd,(off_t)cookie,SEEK_SET)==cookie && collect(fd,size,again) && same_suffix(full,start,again));
    int metadata=1;
    for(size_t i=0;i<full->count;i++) {
        struct stat st;
        if(fstatat(fd,full->entries[i].name,&st,AT_SYMLINK_NOFOLLOW)) { metadata=0; break; }
        unsigned type=full->entries[i].type;
        if((type==DT_DIR && !S_ISDIR(st.st_mode)) ||
           (type==DT_REG && !S_ISREG(st.st_mode)) ||
           (type==DT_LNK && !S_ISLNK(st.st_mode))) { metadata=0; break; }
    }
    snprintf(name,sizeof(name),"sysfs.%s.metadata.lookup",label); check(name,metadata);
    printf("SYSFSOBS {\"directory\":\"%s\",\"entries\":%zu,\"calls\":%zu,\"buffer\":%zu}\n",
           path,full->count,full->calls,size);
    close(fd); free(again);
    return full;
}
static void cpu_checks(const struct listing *list)
{
    unsigned char present[MAX_CPUS], online[MAX_CPUS], possible[MAX_CPUS], seen[MAX_CPUS]={0};
    char text[32768], path[256];
    int p=read_text("/sys/devices/system/cpu/present",text,sizeof(text)) && cpu_list(text,present);
    int o=read_text("/sys/devices/system/cpu/online",text,sizeof(text)) && cpu_list(text,online);
    int a=read_text("/sys/devices/system/cpu/possible",text,sizeof(text)) && cpu_list(text,possible);
    check("sysfs.cpu.present.list",p); check("sysfs.cpu.online.list",o); check("sysfs.cpu.possible.list",a);
    if(!p || !o || !a || !list) return;
    int exact=1, metadata=1; size_t count=0, present_count=0;
    for(size_t i=0;i<list->count;i++) {
        const char *name=list->entries[i].name;
        if(strncmp(name,"cpu",3) || name[3]<'0' || name[3]>'9') continue;
        char *end; unsigned long cpu=strtoul(name+3,&end,10);
        if(*end || cpu>=MAX_CPUS || seen[cpu] || !present[cpu]) { exact=0; continue; }
        seen[cpu]=1; count++;
        if(list->entries[i].type!=DT_DIR) metadata=0;
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%lu/topology/core_id",cpu);
        if(!read_text(path,text,sizeof(text))) metadata=0;
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%lu/topology/physical_package_id",cpu);
        if(!read_text(path,text,sizeof(text))) metadata=0;
    }
    for(size_t cpu=0;cpu<MAX_CPUS;cpu++) {
        present_count+=present[cpu];
        if(seen[cpu]!=present[cpu] || (online[cpu] && !present[cpu]) ||
           (present[cpu] && !possible[cpu])) exact=0;
    }
    check("sysfs.cpu.enumeration.exact",exact);
    check("sysfs.cpu.topology.direct.lookup",metadata);
    check("sysfs.cpu.stable.attributes",has(list,"online") && has(list,"present") && has(list,"possible"));
    check("sysfs.cpu.small.buffer.continues",list->calls>1);
    printf("SYSFSOBS {\"present_cpu_count\":%zu,\"enumerated_cpu_count\":%zu}\n",present_count,count);
}
static void pmu_checks(void)
{
    char text[512];
    check("sysfs.pmu.software.type",read_text("/sys/bus/event_source/devices/software/type",text,sizeof(text)) && !strcmp(text,"1\n"));
    struct stat st;
    if(stat("/sys/bus/event_source/devices/cpu",&st)) {
        check("sysfs.pmu.absent.lookup",errno==ENOENT);
        skipped++;
        printf("NSSKIP {\"name\":\"sysfs.pmu.cpu\",\"reason\":\"CPU PMU absent\"}\n");
        return;
    }
    check("sysfs.pmu.cpu.type",read_text("/sys/bus/event_source/devices/cpu/type",text,sizeof(text)) && !strcmp(text,"4\n"));
    struct listing *format=directory("/sys/bus/event_source/devices/cpu/format","pmu.format",64);
    check("sysfs.pmu.format.event",format && has(format,"event") && read_text("/sys/bus/event_source/devices/cpu/format/event",text,sizeof(text)) && strstr(text,"config:"));
    free(format);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); alarm(30);
    struct listing *cpu=directory("/sys/devices/system/cpu","cpu",80);
    cpu_checks(cpu); free(cpu);
    const char *paths[]={"/sys/class","/sys/class/block","/sys/devices","/sys/bus/event_source/devices"};
    const char *labels[]={"class","block","devices","event.source"};
    for(size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        struct listing *list=directory(paths[i],labels[i],512);
        if(list && i==0) check("sysfs.class.stable.entries",has(list,"net") && has(list,"block") && has(list,"sound"));
        if(list && i==2) check("sysfs.devices.system.entry",has(list,"system"));
        free(list);
    }
    pmu_checks();
    printf("== namespaces skipped: %d ==\n",skipped);
    printf("== probe complete: %d passed, %d failed ==\n",passed,failed);
    return failed ? 1 : 0;
}
