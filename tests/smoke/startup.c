/* Exercises constructor ordering, pointer relocation, and initial-exec TLS. */
static int constructed;
static int *constructed_ptr = &constructed;
static __thread int tls_value = 39;

__attribute__((constructor)) static void startup_init(void)
{
    constructed = 3;
}

int smoke_start_value(void)
{
    return tls_value + *constructed_ptr;
}
