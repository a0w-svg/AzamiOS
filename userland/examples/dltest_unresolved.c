/* ============================================================================
 * AzamiOS — Dynamic Linking Unresolved Symbol Fixture
 * File: userland/examples/dltest_unresolved.c
 * ============================================================================ */

extern int non_existent_symbol_that_cannot_resolve(void);

int dltest_unresolved_call(void)
{
    return non_existent_symbol_that_cannot_resolve();
}
