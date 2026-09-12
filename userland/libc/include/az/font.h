#pragma once
#if __has_include(<azami/font.h>)
#include <azami/font.h>
#elif __has_include("../azami/font.h")
#include "../azami/font.h"
#elif __has_include("userland/libc/include/azami/font.h")
#include "userland/libc/include/azami/font.h"
#endif
