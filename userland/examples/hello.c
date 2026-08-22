#include <stdio.h>

int main(int argc, char **argv) {
    printf("========================================\n");
    printf(" Hello from authentic GNU GCC on AzamiOS!\n");
    printf("========================================\n");
    printf("Program arguments count: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
