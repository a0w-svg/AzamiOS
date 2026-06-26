#include "include/time.h"

void rtc_get_time(time_t* time) {
    asm volatile(
        "int $128\n"
        :
        : "a"(4), "b"(time)
        : "memory"
    );
}
