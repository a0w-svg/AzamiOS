/* ============================================================================
 * AzamiOS — Dynamic Linking Dependency Chain B Fixture
 * File: userland/examples/dltest_chain_b.c
 * ============================================================================ */

extern int chain_c_val(void);

int chain_b_val(void)
{
    return chain_c_val() + 50;
}
