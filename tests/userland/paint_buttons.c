/* Exercise real Paint button handlers and undo storage on the host. */
#define main paint_app_main
#include "../../userland/apps/paint/main.c"
#undef main
#define assert(condition) do { if (!(condition)) { \
    printf("Paint assertion failed at line %d\n", __LINE__); abort(); \
} } while (0)

int main(void)
{
    init_canvas();
    g_canvas[0] = 0xff123456;
    save_undo();
    g_canvas[0] = 0xffabcdef;
    handle_click(390, 16); /* Undo */
    assert(g_canvas[0] == 0xff123456);
    assert(!g_has_undo);
    handle_click(442, 16); /* Clear */
    assert(g_canvas[0] == 0xffffffff && g_has_undo);
    handle_click(390, 16); /* Undo Clear */
    assert(g_canvas[0] == 0xff123456);
    handle_click(20, 16); /* Selecting a tool preserves undo data. */
    assert(g_canvas[0] == 0xff123456);
    puts("Paint Undo and Clear button handlers: PASS");
    return 0;
}
