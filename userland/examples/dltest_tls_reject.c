/* ============================================================================
 * AzamiOS — Dynamic Linking PT_TLS Reject Fixture
 * File: userland/examples/dltest_tls_reject.c
 * ============================================================================ */

__thread int g_tls_var = 1234;

int dltest_tls_func(void)
{
    return g_tls_var;
}
