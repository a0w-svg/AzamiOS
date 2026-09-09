/* ============================================================================
 * AzamiOS — Phase 5a dynamic linking milestone: test shared library
 * File: userland/examples/dltest_lib.c
 *
 * Deliberately trivial: no external dependencies, no TLS, one exported
 * symbol — exactly what the plan's milestone 5a asked for as the first
 * proof-of-mechanism .so.
 * ============================================================================ */

int dltest_add(int a, int b)
{
    return a + b;
}
