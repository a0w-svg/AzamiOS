/* ============================================================================
 * AzamiOS — Dynamic Linking Test Event Sink
 * File: userland/examples/dltest_event.c
 *
 * Keeps a persistent log of constructor and destructor events across
 * load and unload cycles of test shared objects.
 * ============================================================================ */

#define MAX_EVENTS 64

static int s_events[MAX_EVENTS];
static int s_event_count = 0;

void dltest_event_clear(void)
{
    s_event_count = 0;
    for (int i = 0; i < MAX_EVENTS; i++) s_events[i] = 0;
}

void dltest_event_record(int id)
{
    if (s_event_count < MAX_EVENTS) {
        s_events[s_event_count++] = id;
    }
}

int dltest_event_count(void)
{
    return s_event_count;
}

int dltest_event_get(int index)
{
    if (index >= 0 && index < s_event_count) return s_events[index];
    return -1;
}
