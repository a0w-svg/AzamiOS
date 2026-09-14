/* ============================================================================
 * AzamiOS C Application Template
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("AzamiOS Application Started.\n");
    if (argc > 1) {
        printf("Arguments (%d):\n", argc - 1);
        for (int i = 1; i < argc; i++) {
            printf("  [%d]: %s\n", i, argv[i]);
        }
    }
    return 0;
}
