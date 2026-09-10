/* ============================================================================
 * ipcrm — remove System V IPC objects
 * File: userland/apps/ipcrm/main.c
 *
 * The counterpart to ipcs: identifiers listed there can be handed back here
 * to be destroyed.  Removal is by identifier (-m/-s/-q) or by key
 * (-M/-S/-Q), matching the POSIX-documented options.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>

static int rm_shm(int id) { return shmctl(id, IPC_RMID, NULL); }
static int rm_sem(int id) { return semctl(id, 0, IPC_RMID); }
static int rm_msg(int id) { return msgctl(id, IPC_RMID, NULL); }

/* Resolve a key to an identifier without creating anything. */
static int id_from_key(char kind, key_t key)
{
    switch (kind) {
    case 'm': return shmget(key, 0, 0);
    case 's': return semget(key, 0, 0);
    case 'q': return msgget(key, 0);
    default:  return -1;
    }
}

static void usage(void)
{
    printf("Usage: ipcrm [-m shmid] [-s semid] [-q msqid]\n"
           "             [-M shmkey] [-S semkey] [-Q msgkey]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    int failures = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0' || argv[i][2] != '\0') {
            usage();
            return 1;
        }
        char opt = argv[i][1];
        if (i + 1 >= argc) { usage(); return 1; }

        const char *arg = argv[++i];
        long v = strtol(arg, NULL, 0);
        int id, ret;
        char kind;

        switch (opt) {
        case 'm': case 's': case 'q':
            id = (int)v;
            kind = opt;
            break;
        case 'M': case 'S': case 'Q':
            kind = (char)(opt - 'A' + 'a');
            id = id_from_key(kind, (key_t)v);
            if (id < 0) {
                fprintf(stderr, "ipcrm: key 0x%lx: %s\n", v, strerror(errno));
                failures++;
                continue;
            }
            break;
        default:
            usage();
            return 1;
        }

        ret = (kind == 'm') ? rm_shm(id) : (kind == 's') ? rm_sem(id) : rm_msg(id);
        if (ret != 0) {
            fprintf(stderr, "ipcrm: id %d: %s\n", id, strerror(errno));
            failures++;
        }
    }
    return failures ? 1 : 0;
}
