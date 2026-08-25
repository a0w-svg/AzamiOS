/* ============================================================================
 * AzamiOS Userspace — Linux Inotify Monitoring Tool (inotifywait)
 * File: userland/apps/inotifywait/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/inotify.h>

#define EVENT_BUF_LEN 1024

static void print_event_mask(uint32_t mask)
{
    bool first = true;
    if (mask & IN_ACCESS)        { printf("%sACCESS", first ? "" : ","); first = false; }
    if (mask & IN_MODIFY)        { printf("%sMODIFY", first ? "" : ","); first = false; }
    if (mask & IN_ATTRIB)        { printf("%sATTRIB", first ? "" : ","); first = false; }
    if (mask & IN_CLOSE_WRITE)   { printf("%sCLOSE_WRITE", first ? "" : ","); first = false; }
    if (mask & IN_CLOSE_NOWRITE) { printf("%sCLOSE_NOWRITE", first ? "" : ","); first = false; }
    if (mask & IN_OPEN)          { printf("%sOPEN", first ? "" : ","); first = false; }
    if (mask & IN_MOVED_FROM)    { printf("%sMOVED_FROM", first ? "" : ","); first = false; }
    if (mask & IN_MOVED_TO)      { printf("%sMOVED_TO", first ? "" : ","); first = false; }
    if (mask & IN_CREATE)        { printf("%sCREATE", first ? "" : ","); first = false; }
    if (mask & IN_DELETE)        { printf("%sDELETE", first ? "" : ","); first = false; }
    if (mask & IN_DELETE_SELF)   { printf("%sDELETE_SELF", first ? "" : ","); first = false; }
    if (mask & IN_MOVE_SELF)     { printf("%sMOVE_SELF", first ? "" : ","); first = false; }
    if (mask & IN_IGNORED)       { printf("%sIGNORED", first ? "" : ","); first = false; }
    if (mask & IN_ISDIR)         { printf("%sISDIR", first ? "" : ","); first = false; }
}

int main(int argc, char **argv)
{
    bool monitor = false;
    const char *target_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monitor") == 0) {
            monitor = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: inotifywait [-m] <file or directory>\n");
            printf("Wait for and output filesystem events via Linux inotify.\n");
            printf("  -m, --monitor    Execute indefinitely rather than exiting on first event\n");
            return 0;
        } else if (argv[i][0] != '-') {
            target_path = argv[i];
        }
    }

    if (!target_path) {
        target_path = ".";
    }

    int fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        return 1;
    }

    int wd = inotify_add_watch(fd, target_path, IN_ALL_EVENTS);
    if (wd < 0) {
        perror("inotify_add_watch");
        close(fd);
        return 1;
    }

    printf("Setting up watches.\n");
    printf("Watches established. %s for events on: %s (wd=%d)\n",
           monitor ? "Continuously monitoring" : "Waiting", target_path, wd);

    char buffer[EVENT_BUF_LEN];

    while (1) {
        ssize_t len = read(fd, buffer, sizeof(buffer));
        if (len < 0) {
            perror("read");
            break;
        }

        ssize_t i = 0;
        while (i < len) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            printf("%s ", target_path);
            print_event_mask(event->mask);
            if (event->len > 0) {
                printf(" %s", event->name);
            }
            printf("\n");
            i += sizeof(struct inotify_event) + event->len;
        }

        if (!monitor) {
            break;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
    return 0;
}
