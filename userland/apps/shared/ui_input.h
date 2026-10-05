#pragma once

/* Update before dispatching: handlers often return before the end of an event.
 * Motion with a held button must not repeat a toolbar action. Drag handlers
 * should still use the current button mask for their continuous motion. */
static inline unsigned int uk_mouse_press(unsigned int *previous,
                                         unsigned int buttons)
{
    unsigned int pressed = buttons & ~*previous;
    *previous = buttons;
    return pressed;
}
