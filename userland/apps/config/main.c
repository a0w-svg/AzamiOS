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
#include "../../libc/include/sys/wait.h"
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
    "/etc/power.conf",
    "/etc/security.conf",
    "/etc/sysctl.conf",
    "/etc/session.conf",
    "/etc/font.conf",
    "/etc/mime.conf"
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
    printf("  \033[1;36msession\033[0m [list|add|del]      Manage autostart and background services\n");
    printf("  \033[1;36mfont\033[0m [show|set]             Manage system typography and font sizing\n");
    printf("  \033[1;36mmime\033[0m [list|set]             Manage default GUI file associations\n");
    printf("  \033[1;36mautoaccept\033[0m [on|off|status]  Manage unattended auto-accept execution policies\n");
    printf("  \033[1;36medit\033[0m [file]                 Open config file in graphical Text Editor\n");
    printf("  \033[1;36mreload\033[0m                      Broadcast reload to Desktop Environment & Compositor\n\n");
    printf("Config domains: desktop, terminal, audio, network, power, security, sysctl, session, font, mime\n\n");
    printf("Examples:\n");
    printf("  config list\n");
    printf("  config get theme_id\n");
    printf("  config session list\n");
    printf("  config font set /usr/share/fonts/vga_regular.azf 16\n");
    printf("  config mime set ppm /bin/imageviewer.elf\n");
    printf("  config autoaccept on\n");
    printf("  config edit security\n");
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
    if (strcmp(name, "power") == 0 || strcmp(name, "power.conf") == 0) return "/etc/power.conf";
    if (strcmp(name, "security") == 0 || strcmp(name, "security.conf") == 0 || strcmp(name, "sec") == 0) return "/etc/security.conf";
    if (strcmp(name, "sysctl") == 0 || strcmp(name, "sysctl.conf") == 0) return "/etc/sysctl.conf";
    if (strcmp(name, "session") == 0 || strcmp(name, "session.conf") == 0) return "/etc/session.conf";
    if (strcmp(name, "font") == 0 || strcmp(name, "font.conf") == 0) return "/etc/font.conf";
    if (strcmp(name, "mime") == 0 || strcmp(name, "mime.conf") == 0) return "/etc/mime.conf";
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

    if (strcmp(cmd, "autoaccept") == 0) {
        char *aa_argv[5];
        aa_argv[0] = "/bin/autoaccept.elf";
        if (argc >= 3) {
            aa_argv[1] = argv[2];
            aa_argv[2] = (argc >= 4) ? argv[3] : NULL;
            aa_argv[3] = NULL;
        } else {
            aa_argv[1] = "status";
            aa_argv[2] = NULL;
        }
        int pid = fork();
        if (pid == 0) {
            execve("/bin/autoaccept.elf", aa_argv, environ);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        return 1;
    }

    if (strcmp(cmd, "session") == 0) {
        if (argc >= 3 && strcmp(argv[2], "add") == 0 && argc >= 6) {
            const char *sec = argv[3];  /* services or autostart */
            const char *name = argv[4];
            const char *path = argv[5];
            int fd = open("/etc/session.conf", O_RDWR, 0644);
            if (fd >= 0) {
                char buf[MAX_FILE_SIZE];
                ssize_t n = read(fd, buf, sizeof(buf) - 256);
                if (n > 0) {
                    buf[n] = '\0';
                    char target_sec[64];
                    snprintf(target_sec, sizeof(target_sec), "[%s]", sec);
                    char *p = strstr(buf, target_sec);
                    if (p) {
                        char *nl = strchr(p, '\n');
                        if (nl) {
                            char new_buf[MAX_FILE_SIZE];
                            size_t pre_len = (size_t)(nl - buf + 1);
                            memcpy(new_buf, buf, pre_len);
                            int added = snprintf(new_buf + pre_len, sizeof(new_buf) - pre_len, "%s=%s\n", name, path);
                            strcpy(new_buf + pre_len + added, nl + 1);
                            lseek(fd, 0, SEEK_SET);
                            write(fd, new_buf, strlen(new_buf));
                            ftruncate(fd, strlen(new_buf));
                            printf("\033[1;32m✓\033[0m Added [%s] \033[1;36m%s\033[0m = \033[1;33m%s\033[0m to /etc/session.conf\n", sec, name, path);
                            close(fd);
                            return 0;
                        }
                    }
                }
                close(fd);
            }
            printf("\033[1;31mError:\033[0m Failed to add session entry.\n");
            return 1;
        } else if (argc >= 4 && (strcmp(argv[2], "del") == 0 || strcmp(argv[2], "remove") == 0)) {
            const char *name = argv[3];
            int fd = open("/etc/session.conf", O_RDWR, 0644);
            if (fd >= 0) {
                char buf[MAX_FILE_SIZE];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    char new_buf[MAX_FILE_SIZE];
                    new_buf[0] = '\0';
                    char *line = buf;
                    while (line && *line) {
                        char *eol = strchr(line, '\n');
                        if (eol) *eol = '\0';
                        char *eq = strchr(line, '=');
                        bool skip = false;
                        if (eq) {
                            *eq = '\0';
                            char *k = line;
                            while (*k == ' ' || *k == '\t') k++;
                            if (strcmp(k, name) == 0) skip = true;
                            *eq = '=';
                        }
                        if (!skip) {
                            strncat(new_buf, line, sizeof(new_buf) - strlen(new_buf) - 1);
                            strncat(new_buf, "\n", sizeof(new_buf) - strlen(new_buf) - 1);
                        }
                        line = eol ? eol + 1 : NULL;
                    }
                    lseek(fd, 0, SEEK_SET);
                    write(fd, new_buf, strlen(new_buf));
                    ftruncate(fd, strlen(new_buf));
                    printf("\033[1;32m✓\033[0m Removed entry '%s' from /etc/session.conf\n", name);
                    close(fd);
                    return 0;
                }
                close(fd);
            }
            return 1;
        } else {
            cmd_list_file("/etc/session.conf");
            return 0;
        }
    }

    if (strcmp(cmd, "font") == 0) {
        if (argc >= 4 && strcmp(argv[2], "set") == 0) {
            set_key_in_file("/etc/font.conf", "system_font", argv[3]);
            if (argc >= 5) {
                set_key_in_file("/etc/font.conf", "size", argv[4]);
            }
            printf("\033[1;32m✓\033[0m Updated /etc/font.conf font settings.\n");
            broadcast_reload();
            return 0;
        }
        cmd_list_file("/etc/font.conf");
        return 0;
    }

    if (strcmp(cmd, "mime") == 0) {
        if (argc >= 5 && strcmp(argv[2], "set") == 0) {
            set_key_in_file("/etc/mime.conf", argv[3], argv[4]);
            printf("\033[1;32m✓\033[0m Associated extension '%s' -> '%s' in /etc/mime.conf\n", argv[3], argv[4]);
            return 0;
        }
        cmd_list_file("/etc/mime.conf");
        return 0;
    }

    if (strcmp(cmd, "reload") == 0) {
        broadcast_reload();
        return 0;
    }

    printf("\033[1;31mUnknown command:\033[0m '%s'. Type 'config' for help.\n", cmd);
    return 1;
}
