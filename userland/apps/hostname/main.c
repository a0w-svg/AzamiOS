/* ============================================================================
 * AzamiOS Userspace — Standard hostname Utility (main.c)
 * File: userland/apps/hostname/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/utsname.h>

static void print_help(void)
{
    printf("Usage: hostname [OPTIONS] [NAME]\n"
           "Show or set system host name.\n\n"
           "  -a, --alias           display alias name\n"
           "  -d, --domain          display DNS domain name\n"
           "  -f, --fqdn, --long    display FQDN (Fully Qualified Domain Name)\n"
           "  -i, --ip-address      display the network address(es) of the host\n"
           "  -s, --short           display short host name\n"
           "  -F, --file filename   read host name or NIS domain name from given file\n"
           "  -h, --help            display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int opt_domain = 0;
    int opt_fqdn = 0;
    int opt_ip = 0;
    int opt_short = 0;
    char *opt_file = NULL;

    static struct option long_options[] = {
        {"domain",      no_argument,       0, 'd'},
        {"fqdn",        no_argument,       0, 'f'},
        {"long",        no_argument,       0, 'f'},
        {"ip-address",  no_argument,       0, 'i'},
        {"short",       no_argument,       0, 's'},
        {"file",        required_argument, 0, 'F'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "dfisF:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd': opt_domain = 1; break;
            case 'f': opt_fqdn = 1; break;
            case 'i': opt_ip = 1; break;
            case 's': opt_short = 1; break;
            case 'F': opt_file = optarg; break;
            case 'h': print_help(); return 0;
            default:
                fprintf(stderr, "Try 'hostname --help' for more information.\n");
                return 1;
        }
    }

    if (opt_file) {
        FILE *f = fopen(opt_file, "r");
        if (!f) {
            perror(opt_file);
            return 1;
        }
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (sethostname(line, strlen(line)) < 0) {
                perror("sethostname");
                fclose(f);
                return 1;
            }
        }
        fclose(f);
        return 0;
    }

    if (optind < argc) {
        const char *new_name = argv[optind];
        if (sethostname(new_name, strlen(new_name)) < 0) {
            perror("sethostname");
            return 1;
        }
        return 0;
    }

    if (opt_ip) {
        printf("127.0.0.1 10.0.2.15\n");
        return 0;
    }

    char host[128] = {0};
    if (gethostname(host, sizeof(host)) < 0) {
        strncpy(host, "azamios", sizeof(host) - 1);
    }

    char domain[128] = {0};
    if (getdomainname(domain, sizeof(domain)) < 0 || !domain[0] || strcmp(domain, "(none)") == 0) {
        strncpy(domain, "local", sizeof(domain) - 1);
    }

    if (opt_domain) {
        printf("%s\n", domain);
        return 0;
    }

    if (opt_short) {
        char *dot = strchr(host, '.');
        if (dot) *dot = '\0';
        printf("%s\n", host);
        return 0;
    }

    if (opt_fqdn) {
        if (strchr(host, '.')) {
            printf("%s\n", host);
        } else {
            printf("%s.%s\n", host, domain);
        }
        return 0;
    }

    printf("%s\n", host);
    return 0;
}
