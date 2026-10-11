/* ============================================================================
 * AzamiOS — Dynamic Linking Dependency Library Fixture
 * File: userland/examples/dltest_dep.c
 * ============================================================================ */

extern void dltest_event_record(int id);

#define EVENT_DEP_CTOR 101
#define EVENT_DEP_DTOR 201

__attribute__((constructor))
void dltest_dep_ctor(void)
{
    dltest_event_record(EVENT_DEP_CTOR);
}

__attribute__((destructor))
void dltest_dep_dtor(void)
{
    dltest_event_record(EVENT_DEP_DTOR);
}

int dltest_dep_func(void)
{
    return 1000;
}
