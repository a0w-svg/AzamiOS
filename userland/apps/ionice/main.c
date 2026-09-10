/* ============================================================================
 * AzamiOS Userspace — ionice Utility (main.c)
 * File: userland/apps/ionice/main.c
 *
 * Read or set the I/O scheduling class and priority of a process, a process
 * group, or a user — the counterpart to nice(1) for disk bandwidth rather than
 * CPU time. With a command instead of a pid, it sets the priority and then
 * execs, so the command runs under it from its first request.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioprio.h>

static const char *class_name(int cls)
{
    switch (cls) {
    case IOPRIO_CLASS_NONE: return "none";
    case IOPRIO_CLASS_RT:   return "realtime";
    case IOPRIO_CLASS_BE:   return "best-effort";
    case IOPRIO_CLASS_IDLE: return "idle";
    default:                return "unknown";
    }
}

/* Accept both the numeric class and the name, as util-linux does. */
static int parse_class(const char *s)
{
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0') return s[0] - '0';
    if (strcmp(s, "none") == 0)        return IOPRIO_CLASS_NONE;
    if (strcmp(s, "realtime") == 0)    return IOPRIO_CLASS_RT;
    if (strcmp(s, "rt") == 0)          return IOPRIO_CLASS_RT;
    if (strcmp(s, "best-effort") == 0) return IOPRIO_CLASS_BE;
    if (strcmp(s, "be") == 0)          return IOPRIO_CLASS_BE;
    if (strcmp(s, "idle") == 0)        return IOPRIO_CLASS_IDLE;
    return -1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: ionice [OPTION]... [-p PID]...\n"
        "       ionice [OPTION]... COMMAND [ARG]...\n"
        "Show or change the I/O scheduling class and priority of a process.\n\n"
        "  -c, --class CLASS   name or number of the scheduling class:\n"
        "                        0/none  1/realtime  2/best-effort  3/idle\n"
        "  -n, --classdata N   priority level within the class (0-7, 0 is highest)\n"
        "  -p, --pid PID       act on this process (repeatable)\n"
        "  -P, --pgid PGID     act on this process group\n"
        "  -u, --uid UID       act on the processes of this user\n"
        "  -t, --ignore        ignore failures to set the priority\n"
        "  -h, --help          display this help and exit\n\n"
        "With no class or level, the current priority is printed instead.\n"
        "Only the real-time class requires privilege.\n");
}

static int show(int which, int who)
{
    int v = ioprio_get(which, who);
    if (v < 0) {
        fprintf(stderr, "ionice: ioprio_get failed: %s\n", strerror(errno));
        return 1;
    }
    int cls = IOPRIO_PRIO_CLASS(v);
    if (cls == IOPRIO_CLASS_IDLE || cls == IOPRIO_CLASS_NONE)
        printf("%s\n", class_name(cls));
    else
        printf("%s: prio %d\n", class_name(cls), IOPRIO_PRIO_DATA(v));
    return 0;
}

int main(int argc, char *argv[])
{
    int cls = -1, level = -1;
    int ignore = 0;
    int targets[64];
    int target_which[64];
    int ntargets = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;

        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        if (strcmp(a, "-t") == 0 || strcmp(a, "--ignore") == 0) { ignore = 1; continue; }

        int which = -1;
        const char *val = NULL;
        if (strcmp(a, "-c") == 0 || strcmp(a, "--class") == 0)       val = argv[++i];
        else if (strcmp(a, "-n") == 0 || strcmp(a, "--classdata") == 0) val = argv[++i];
        else if (strcmp(a, "-p") == 0 || strcmp(a, "--pid") == 0)  { which = IOPRIO_WHO_PROCESS; val = argv[++i]; }
        else if (strcmp(a, "-P") == 0 || strcmp(a, "--pgid") == 0) { which = IOPRIO_WHO_PGRP;    val = argv[++i]; }
        else if (strcmp(a, "-u") == 0 || strcmp(a, "--uid") == 0)  { which = IOPRIO_WHO_USER;    val = argv[++i]; }
        else {
            fprintf(stderr, "ionice: unknown option '%s'\n", a);
            usage(stderr);
            return 1;
        }

        if (i >= argc || !val) {
            fprintf(stderr, "ionice: option '%s' requires an argument\n", a);
            return 1;
        }

        if (which >= 0) {
            if (ntargets >= (int)(sizeof(targets) / sizeof(targets[0]))) {
                fprintf(stderr, "ionice: too many targets\n");
                return 1;
            }
            targets[ntargets] = atoi(val);
            target_which[ntargets] = which;
            ntargets++;
        } else if (a[1] == 'c' || strcmp(a, "--class") == 0) {
            cls = parse_class(val);
            if (cls < 0) {
                fprintf(stderr, "ionice: unknown scheduling class '%s'\n", val);
                return 1;
            }
        } else {
            level = atoi(val);
            if (level < 0 || level >= IOPRIO_NR_LEVELS) {
                fprintf(stderr, "ionice: priority level must be 0-%d\n",
                        IOPRIO_NR_LEVELS - 1);
                return 1;
            }
        }
    }

    /* No class and no level: this is a query, not a change. */
    if (cls < 0 && level < 0) {
        if (ntargets == 0) return show(IOPRIO_WHO_PROCESS, 0);
        int rc = 0;
        for (int t = 0; t < ntargets; t++)
            if (show(target_which[t], targets[t]) != 0) rc = 1;
        return rc;
    }

    /* A level without a class means best-effort, which is the only class where
     * a level is the natural thing to be adjusting. */
    if (cls < 0) cls = IOPRIO_CLASS_BE;
    if (level < 0) level = (cls == IOPRIO_CLASS_BE || cls == IOPRIO_CLASS_RT) ? 4 : 0;
    if (cls == IOPRIO_CLASS_NONE || cls == IOPRIO_CLASS_IDLE) level = 0;

    int prio = IOPRIO_PRIO_VALUE(cls, level);

    if (ntargets > 0) {
        int rc = 0;
        for (int t = 0; t < ntargets; t++) {
            if (ioprio_set(target_which[t], targets[t], prio) != 0) {
                fprintf(stderr, "ionice: ioprio_set failed for %d: %s\n",
                        targets[t], strerror(errno));
                if (!ignore) rc = 1;
            }
        }
        return rc;
    }

    if (i >= argc) {
        fprintf(stderr, "ionice: need a command or at least one -p/-P/-u target\n");
        usage(stderr);
        return 1;
    }

    /* Set our own priority, then exec: the child inherits it, so the command
     * runs under the new class from its very first I/O request. */
    if (ioprio_set(IOPRIO_WHO_PROCESS, 0, prio) != 0 && !ignore) {
        fprintf(stderr, "ionice: ioprio_set failed: %s\n", strerror(errno));
        return 1;
    }
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "ionice: %s: %s\n", argv[i], strerror(errno));
    return 127;
}
