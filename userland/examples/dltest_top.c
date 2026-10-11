/* ============================================================================
 * AzamiOS — Dynamic Linking Top-Level Library Fixture
 * File: userland/examples/dltest_top.c
 * ============================================================================ */

extern void dltest_event_record(int id);
extern int  dltest_dep_func(void);

#define EVENT_TOP_CTOR 102
#define EVENT_TOP_DTOR 202

__attribute__((constructor))
void dltest_top_ctor(void)
{
    dltest_event_record(EVENT_TOP_CTOR);
}

__attribute__((destructor))
void dltest_top_dtor(void)
{
    dltest_event_record(EVENT_TOP_DTOR);
}

int dltest_top_calc(int x)
{
    return dltest_dep_func() + x;
}
