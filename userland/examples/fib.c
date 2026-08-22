#include <stdio.h>
#include <stdlib.h>

static unsigned long long fib(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    unsigned long long a = 0, b = 1, c = 0;
    for (int i = 2; i <= n; i++) {
        c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(int argc, char **argv) {
    int count = 20;
    if (argc > 1) {
        count = atoi(argv[1]);
        if (count < 1 || count > 50) count = 20;
    }

    printf("Computing first %d Fibonacci numbers:\n", count);
    for (int i = 0; i <= count; i++) {
        printf("  F(%02d) = %llu\n", i, fib(i));
    }
    return 0;
}
