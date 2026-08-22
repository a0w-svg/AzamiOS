#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT 500

int main(int argc, char **argv) {
    int max_num = LIMIT;
    if (argc > 1) {
        max_num = atoi(argv[1]);
        if (max_num < 10 || max_num > 10000) max_num = LIMIT;
    }

    printf("Calculating primes up to %d using Sieve of Eratosthenes...\n", max_num);

    bool *is_prime = (bool *)malloc((max_num + 1) * sizeof(bool));
    if (!is_prime) {
        printf("Memory allocation failed!\n");
        return 1;
    }

    for (int i = 0; i <= max_num; i++) is_prime[i] = true;
    is_prime[0] = is_prime[1] = false;

    for (int p = 2; p * p <= max_num; p++) {
        if (is_prime[p]) {
            for (int i = p * p; i <= max_num; i += p) {
                is_prime[i] = false;
            }
        }
    }

    int count = 0;
    for (int i = 2; i <= max_num; i++) {
        if (is_prime[i]) {
            printf("%5d", i);
            count++;
            if (count % 10 == 0) printf("\n");
        }
    }
    if (count % 10 != 0) printf("\n");

    printf("\nTotal primes found: %d\n", count);
    free(is_prime);
    return 0;
}
