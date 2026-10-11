#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* Stock musl startup exercises relocation, TLS and the real dynamic linker.
 * Descriptor slots are installed by the static exec supervisor. */
static _Thread_local volatile int tls = 123;
int main(int argc, char **argv)
{
    struct sigaction action;
    if (argc != 2 || strcmp(argv[1], "argument") ||
        !getenv("EXEC_TEST") || strcmp(getenv("EXEC_TEST"), "yes") || tls != 123)
        return 101;
    if (!getauxval(AT_BASE) || !getauxval(AT_ENTRY) || !getauxval(AT_PHDR) ||
        getauxval(AT_PHENT) != sizeof(Elf64_Phdr)) return 102;
    errno = 0;
    if (fcntl(32, F_GETFD) != -1 || errno != EBADF || fcntl(33, F_GETFD) != 0)
        return 103;
    if (sigaction(SIGUSR1, NULL, &action) || action.sa_handler != SIG_DFL)
        return 104;
    return 43;
}
