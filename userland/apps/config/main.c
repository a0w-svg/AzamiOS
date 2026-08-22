/* ============================================================================
 * AzamiOS Userspace — System Configuration Manager (config.elf)
 * File: userland/apps/config/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/stat.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../../libc/include/az/ipc.h"

#define MAX_LINE 256
#define MAX_FILE_SIZE 8192

static const char *const s_config_files[] = {
    "/etc/desktop.conf",
    "/etc/terminal.conf",
    "/etc/audio.conf",
    "/etc/network.conf",
    "/etc/sysctl.conf"
};
#define NUM_CONFIG_FILES (sizeof(s_config_files) / sizeof(s_config_files[0]))

static void print_banner(void)
{
    printf("\033[1;35mAzamiOS v7.0 System Configuration Manager\033[0m\n");
    printf("Storage: /etc/*.conf (POSIX INI Format)\n\n");
}

static void print_usage(void)
{
    print_banner();
    printf("Usage: config <command> [arguments]\n\n");
    printf("Commands:\n");
    printf("  \033[1;36mlist\033[0m [file]                 List all config files or dump a specific file\n");
    printf("  \033[1;36mget\033[0m  <key>                  Get value of <key> (searches all .conf files)\n");
    printf("  \033[1;36mget\033[0m  <file> <key>           Get value of <key> in specific <file>\n");
    printf("  \033[1;36mset\033[0m  <key> <value>          Set <key>=<value> in matching config file\n");
    printf("  \033[1;36mset\033[0m  <file> <key> <value>   Set <key>=<value> in specific <file>\n");
    printf("  \033[1;36medit\033[0m [file]                 Open config file in graphical Text Editor\n");
    printf("  \033[1;36mreload\033[0m                      Broadcast reload to Desktop Environment & Compositor\n\n");
    printf("Examples:\n");
    printf("  config list\n");
    printf("  config get theme_id\n");
    printf("  config set theme_id 2\n");
    printf("  config set terminal.theme nord\n");
    printf("  config edit desktop\n");
}

/* Helper to resolve alias/partial file names */
static const char *resolve_file(const char *name)
{
    if (!name) return NULL;
    if (name[0] == '/') return name;
    if (strcmp(name, "desktop") == 0 || strcmp(name, "desktop.conf") == 0) return "/etc/desktop.conf";
    if (strcmp(name, "terminal") == 0 || strcmp(name, "terminal.conf") == 0 || strcmp(name, "term") == 0) return "/etc/terminal.conf";
    if (strcmp(name, "audio") == 0 || strcmp(name, "audio.conf") == 0 || strcmp(name, "sound") == 0) return "/etc/audio.conf";
    if (strcmp(name, "network") == 0 || strcmp(name, "network.conf") == 0 || strcmp(name, "net") == 0) return "/etc/network.conf";
    if (strcmp(name, "sysctl") == 0 || strcmp(name, "sysctl.conf") == 0) return "/etc/sysctl.conf";
    return name;
}

static void cmd_list_file(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("\033[1;31mError:\033[0m Cannot open %s\n", path);
        return;
    }
    printf("\033[1;34m=== %s ===\033[0m\n", path);
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '[') printf("\033[1;33m[");
            else if (buf[i] == ']') printf("]\033[0m");
            else if (buf[i] == '#') printf("\033[0;34m#");
            else if (buf[i] == '\n') printf("\033[0m\n");
            else putchar(buf[i]);
        }
    }
    printf("\033[0m\n");
    close(fd);
}

static void cmd_list_all(void)
{
    print_banner();
    for (size_t i = 0; i < NUM_CONFIG_FILES; i++) {
        cmd_list_file(s_config_files[i]);
    }
}

/* Parse key and retrieve value from a file */
static bool get_key_from_file(const char *path, const char *key, char *out_val, size_t max_val)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return false;

    char buf[MAX_FILE_SIZE];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) return false;
    buf[len] = '\0';

    const char *kname = key;
    const char *dot = strchr(key, '.');
    if (dot) kname = dot + 1;

    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        while (*line == ' ' || *line == '\t') line++;
        if (*line != '#' && *line != '[' && *line != '\0') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *k = line;
                char *v = eq + 1;
                while (*k && k[strlen(k) - 1] == ' ') k[strlen(k) - 1] = '\0';
                while (*v == ' ' || *v == '\t') v++;

                if (strcmp(k, kname) == 0 || strcmp(k, key) == 0) {
                    strncpy(out_val, v, max_val - 1);
                    out_val[max_val - 1] = '\0';
                    return true;
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

/* Set key in a specific file */
static bool set_key_in_file(const char *path, const char *key, const char *val)
{
    int fd = open(path, O_RDONLY, 0);
    char buf[MAX_FILE_SIZE];
    ssize_t len = 0;
    if (fd >= 0) {
        len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    if (len < 0) len = 0;
    buf[len] = '\0';

    const char *kname = key;
    const char *dot = strchr(key, '.');
    if (dot) kname = dot + 1;

    char new_content[MAX_FILE_SIZE];
    new_content[0] = '\0';
    bool found = false;

    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        char lcopy[MAX_LINE];
        strncpy(lcopy, line, sizeof(lcopy) - 1);
        lcopy[sizeof(lcopy) - 1] = '\0';

        char *trim = line;
        while (*trim == ' ' || *trim == '\t') trim++;

        if (*trim != '#' && *trim != '[' && *trim != '\0') {
            char *eq = strchr(trim, '=');
            if (eq) {
                *eq = '\0';
                char *k = trim;
                while (*k && k[strlen(k) - 1] == ' ') k[strlen(k) - 1] = '\0';

                if (strcmp(k, kname) == 0 || strcmp(k, key) == 0) {
                    char entry[MAX_LINE];
                    snprintf(entry, sizeof(entry), "%s=%s\n", kname, val);
                    strncat(new_content, entry, sizeof(new_content) - strlen(new_content) - 1);
                    found = true;
                    line = eol ? eol + 1 : NULL;
                    continue;
                }
            }
        }

        strncat(new_content, lcopy, sizeof(new_content) - strlen(new_content) - 1);
        strncat(new_content, "\n", sizeof(new_content) - strlen(new_content) - 1);
        line = eol ? eol + 1 : NULL;
    }

    if (!found) {
        char entry[MAX_LINE];
        snprintf(entry, sizeof(entry), "%s=%s\n", kname, val);
        strncat(new_content, entry, sizeof(new_content) - strlen(new_content) - 1);
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    write(fd, new_content, strlen(new_content));
    close(fd);
    return true;
}

static void broadcast_reload(void)
{
    /* Notify Compositor / Desktop */
    char val[32];
    if (get_key_from_file("/etc/desktop.conf", "theme_id", val, sizeof(val))) {
        int tid = atoi(val);
        az_wm_msg_t tmsg;
        memset(&tmsg, 0, sizeof(tmsg));
        tmsg.type = AZ_WM_SET_THEME;
        AZ_WM_MSG_THEME(&tmsg)->theme_id = (unsigned int)tid;
        az_channel_send(1, (az_ipc_msg_t *)&tmsg);
    }
    printf("\033[1;32m✓\033[0m Configuration reloaded and synchronized with desktop session.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "show") == 0) {
        if (argc >= 3) {
            const char *f = resolve_file(argv[2]);
            cmd_list_file(f);
        } else {
            cmd_list_all();
        }
        return 0;
    }

    if (strcmp(cmd, "get") == 0) {
        if (argc < 3) {
            printf("Usage: config get <key>  OR  config get <file> <key>\n");
            return 1;
        }
        char val[128];
        val[0] = '\0';
        if (argc >= 4) {
            const char *f = resolve_file(argv[2]);
            if (get_key_from_file(f, argv[3], val, sizeof(val))) {
                printf("%s\n", val);
                return 0;
            }
        } else {
            for (size_t i = 0; i < NUM_CONFIG_FILES; i++) {
                if (get_key_from_file(s_config_files[i], argv[2], val, sizeof(val))) {
                    printf("%s\n", val);
                    return 0;
                }
            }
        }
        printf("\033[1;31mError:\033[0m Key '%s' not found.\n", argc >= 4 ? argv[3] : argv[2]);
        return 1;
    }

    if (strcmp(cmd, "set") == 0) {
        if (argc < 4) {
            printf("Usage: config set <key> <value>  OR  config set <file> <key> <value>\n");
            return 1;
        }
        const char *file = NULL;
        const char *key = NULL;
        const char *val = NULL;

        if (argc >= 5) {
            file = resolve_file(argv[2]);
            key = argv[3];
            val = argv[4];
        } else {
            key = argv[2];
            val = argv[3];
            for (size_t i = 0; i < NUM_CONFIG_FILES; i++) {
                char tmp[128];
                if (get_key_from_file(s_config_files[i], key, tmp, sizeof(tmp))) {
                    file = s_config_files[i];
                    break;
                }
            }
            if (!file) file = "/etc/desktop.conf";
        }

        if (set_key_in_file(file, key, val)) {
            printf("\033[1;32m✓\033[0m Updated [%s] \033[1;36m%s\033[0m = \033[1;33m%s\033[0m\n", file, key, val);
            broadcast_reload();
            return 0;
        } else {
            printf("\033[1;31mError:\033[0m Failed to write to %s\n", file);
            return 1;
        }
    }

    if (strcmp(cmd, "edit") == 0) {
        const char *f = (argc >= 3) ? resolve_file(argv[2]) : "/etc/desktop.conf";
        char *ed_argv[3];
        ed_argv[0] = "/bin/texteditor.elf";
        ed_argv[1] = (char *)f;
        ed_argv[2] = NULL;
        printf("Opening '%s' in Text Editor...\n", f);
        int pid = fork();
        if (pid == 0) {
            execve("/bin/texteditor.elf", ed_argv, environ);
            _exit(127);
        }
        return 0;
    }

    if (strcmp(cmd, "reload") == 0) {
        broadcast_reload();
        return 0;
    }

    printf("\033[1;31mUnknown command:\033[0m '%s'. Type 'config' for help.\n", cmd);
    return 1;
}
