/* ============================================================================
 * AzamiOS — Dynamic Linking Destructor Array Order Fixture
 * File: userland/examples/dltest_dtor_order.c
 * ============================================================================ */

extern void dltest_event_record(int id);

#define EVENT_DTOR_A 301
#define EVENT_DTOR_B 302
#define EVENT_DTOR_C 303

__attribute__((destructor))
void dltest_dtor_first(void)
{
    dltest_event_record(EVENT_DTOR_A);
}

__attribute__((destructor))
void dltest_dtor_second(void)
{
    dltest_event_record(EVENT_DTOR_B);
}

__attribute__((destructor))
void dltest_dtor_third(void)
{
    dltest_event_record(EVENT_DTOR_C);
}

int dltest_dtor_dummy(void)
{
    return 42;
}
