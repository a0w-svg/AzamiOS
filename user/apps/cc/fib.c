/* fib.c – Sample recursive C program for AzamiCC */
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int res = fib(10);
    printf("fib(10) = %d\n", res);
    return 0;
}
