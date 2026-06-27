/**
 * ctype.c — AzamiOS libc: character classification & conversion
 *
 * Uses a 256-entry flags table for O(1) lookups.
 * Handles ASCII 0–127; all others return 0 / pass-through.
 */
#include "include/ctype.h"

/* ── Flags table ─────────────────────────────────────────────────── */
#define _U  0x01   /* uppercase letter    */
#define _L  0x02   /* lowercase letter    */
#define _D  0x04   /* decimal digit       */
#define _C  0x08   /* control character   */
#define _P  0x10   /* punctuation         */
#define _S  0x20   /* whitespace          */
#define _X  0x40   /* hex digit           */
#define _B  0x80   /* blank (space / tab) */

static const unsigned char _ctype[256] = {
/*   0 NUL */ _C,
/*   1 SOH */ _C, /*   2 STX */ _C, /*   3 ETX */ _C,
/*   4 EOT */ _C, /*   5 ENQ */ _C, /*   6 ACK */ _C, /*   7 BEL */ _C,
/*   8 BS  */ _C, /*   9 HT  */ _C|_S|_B,
/*  10 LF  */ _C|_S, /*  11 VT  */ _C|_S, /*  12 FF  */ _C|_S,
/*  13 CR  */ _C|_S,
/*  14 SO  */ _C, /*  15 SI  */ _C, /*  16 DLE */ _C, /*  17 DC1 */ _C,
/*  18 DC2 */ _C, /*  19 DC3 */ _C, /*  20 DC4 */ _C, /*  21 NAK */ _C,
/*  22 SYN */ _C, /*  23 ETB */ _C, /*  24 CAN */ _C, /*  25 EM  */ _C,
/*  26 SUB */ _C, /*  27 ESC */ _C, /*  28 FS  */ _C, /*  29 GS  */ _C,
/*  30 RS  */ _C, /*  31 US  */ _C,
/*  32 SPC */ _S|_B,
/*  33 !   */ _P, /*  34 "   */ _P, /*  35 #   */ _P, /*  36 $   */ _P,
/*  37 %   */ _P, /*  38 &   */ _P, /*  39 '   */ _P, /*  40 (   */ _P,
/*  41 )   */ _P, /*  42 *   */ _P, /*  43 +   */ _P, /*  44 ,   */ _P,
/*  45 -   */ _P, /*  46 .   */ _P, /*  47 /   */ _P,
/*  48 0   */ _D|_X, /*  49 1   */ _D|_X, /*  50 2   */ _D|_X,
/*  51 3   */ _D|_X, /*  52 4   */ _D|_X, /*  53 5   */ _D|_X,
/*  54 6   */ _D|_X, /*  55 7   */ _D|_X, /*  56 8   */ _D|_X,
/*  57 9   */ _D|_X,
/*  58 :   */ _P, /*  59 ;   */ _P, /*  60 <   */ _P, /*  61 =   */ _P,
/*  62 >   */ _P, /*  63 ?   */ _P, /*  64 @   */ _P,
/*  65 A   */ _U|_X, /*  66 B   */ _U|_X, /*  67 C   */ _U|_X,
/*  68 D   */ _U|_X, /*  69 E   */ _U|_X, /*  70 F   */ _U|_X,
/*  71 G   */ _U, /*  72 H   */ _U, /*  73 I   */ _U, /*  74 J   */ _U,
/*  75 K   */ _U, /*  76 L   */ _U, /*  77 M   */ _U, /*  78 N   */ _U,
/*  79 O   */ _U, /*  80 P   */ _U, /*  81 Q   */ _U, /*  82 R   */ _U,
/*  83 S   */ _U, /*  84 T   */ _U, /*  85 U   */ _U, /*  86 V   */ _U,
/*  87 W   */ _U, /*  88 X   */ _U, /*  89 Y   */ _U, /*  90 Z   */ _U,
/*  91 [   */ _P, /*  92 \   */ _P, /*  93 ]   */ _P, /*  94 ^   */ _P,
/*  95 _   */ _P, /*  96 `   */ _P,
/*  97 a   */ _L|_X, /*  98 b   */ _L|_X, /*  99 c   */ _L|_X,
/* 100 d   */ _L|_X, /* 101 e   */ _L|_X, /* 102 f   */ _L|_X,
/* 103 g   */ _L, /* 104 h   */ _L, /* 105 i   */ _L, /* 106 j   */ _L,
/* 107 k   */ _L, /* 108 l   */ _L, /* 109 m   */ _L, /* 110 n   */ _L,
/* 111 o   */ _L, /* 112 p   */ _L, /* 113 q   */ _L, /* 114 r   */ _L,
/* 115 s   */ _L, /* 116 t   */ _L, /* 117 u   */ _L, /* 118 v   */ _L,
/* 119 w   */ _L, /* 120 x   */ _L, /* 121 y   */ _L, /* 122 z   */ _L,
/* 123 {   */ _P, /* 124 |   */ _P, /* 125 }   */ _P, /* 126 ~   */ _P,
/* 127 DEL */ _C,
/* 128–255: all 0 */ [128 ... 255] = 0
};

int isalpha (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & (_U|_L)) : 0; }
int isdigit (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _D)      : 0; }
int isalnum (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & (_U|_L|_D)) : 0; }
int isspace (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _S)      : 0; }
int isprint (int c) { return (c >= 32 && c < 127)                         ? 1 : 0; }
int isgraph (int c) { return (c > 32  && c < 127)                         ? 1 : 0; }
int ispunct (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _P)      : 0; }
int isupper (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _U)      : 0; }
int islower (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _L)      : 0; }
int iscntrl (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _C)      : 0; }
int isxdigit(int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _X)      : 0; }
int isblank (int c) { return (c >= 0 && c < 256) ? (_ctype[c] & _B)      : 0; }

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
