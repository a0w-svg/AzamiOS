/* ============================================================================
 * AzamiOS — Dynamic Linking Dependency Chain A Fixture
 * File: userland/examples/dltest_chain_a.c
 * ============================================================================ */

extern int chain_b_val(void);

int chain_a_val(void)
{
    return chain_b_val() + 5;
}
