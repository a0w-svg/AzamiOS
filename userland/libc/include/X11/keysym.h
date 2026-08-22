/* ============================================================================
 * AzamiOS — Standard POSIX X11 KeySym Definitions
 * File: userland/libc/include/X11/keysym.h
 * ============================================================================ */
#ifndef _X11_KEYSYM_H_
#define _X11_KEYSYM_H_

#define XK_VoidSymbol 0xFFFFFF

/* TTY Function & Control Keys */
#define XK_BackSpace 0xFF08
#define XK_Tab       0xFF09
#define XK_Linefeed  0xFF0A
#define XK_Clear     0xFF0B
#define XK_Return    0xFF0D
#define XK_Pause     0xFF13
#define XK_Scroll_Lock 0xFF14
#define XK_Sys_Req   0xFF15
#define XK_Escape    0xFF1B
#define XK_Delete    0xFFFF

/* Cursor & Motion */
#define XK_Home      0xFF50
#define XK_Left      0xFF51
#define XK_Up        0xFF52
#define XK_Right     0xFF53
#define XK_Down      0xFF54
#define XK_Page_Up   0xFF55
#define XK_Page_Down 0xFF56
#define XK_End       0xFF57
#define XK_Begin     0xFF58
#define XK_Insert    0xFF63

/* Modifiers */
#define XK_Shift_L   0xFFE1
#define XK_Shift_R   0xFFE2
#define XK_Control_L 0xFFE3
#define XK_Control_R 0xFFE4
#define XK_Caps_Lock 0xFFE5
#define XK_Shift_Lock 0xFFE6
#define XK_Meta_L    0xFFE7
#define XK_Meta_R    0xFFE8
#define XK_Alt_L     0xFFE9
#define XK_Alt_R     0xFFEA
#define XK_Super_L   0xFFEB
#define XK_Super_R   0xFFEC

/* Latin 1 (ASCII) */
#define XK_space     0x0020
#define XK_exclam    0x0021
#define XK_quotedbl  0x0022
#define XK_numbersign 0x0023
#define XK_dollar    0x0024
#define XK_percent   0x0025
#define XK_ampersand 0x0026
#define XK_apostrophe 0x0027
#define XK_parenleft 0x0028
#define XK_parenright 0x0029
#define XK_asterisk  0x002a
#define XK_plus      0x002b
#define XK_comma     0x002c
#define XK_minus     0x002d
#define XK_period    0x002e
#define XK_slash     0x002f

#define XK_0 0x0030
#define XK_1 0x0031
#define XK_2 0x0032
#define XK_3 0x0033
#define XK_4 0x0034
#define XK_5 0x0035
#define XK_6 0x0036
#define XK_7 0x0037
#define XK_8 0x0038
#define XK_9 0x0039

#define XK_colon     0x003a
#define XK_semicolon 0x003b
#define XK_less      0x003c
#define XK_equal     0x003d
#define XK_greater   0x003e
#define XK_question  0x003f
#define XK_at        0x0040

#define XK_A 0x0041
#define XK_B 0x0042
#define XK_C 0x0043
#define XK_D 0x0044
#define XK_E 0x0045
#define XK_F 0x0046
#define XK_G 0x0047
#define XK_H 0x0048
#define XK_I 0x0049
#define XK_J 0x004a
#define XK_K 0x004b
#define XK_L 0x004c
#define XK_M 0x004d
#define XK_N 0x004e
#define XK_O 0x004f
#define XK_P 0x0050
#define XK_Q 0x0051
#define XK_R 0x0052
#define XK_S 0x0053
#define XK_T 0x0054
#define XK_U 0x0055
#define XK_V 0x0056
#define XK_W 0x0057
#define XK_X 0x0058
#define XK_Y 0x0059
#define XK_Z 0x005a

#define XK_bracketleft 0x005b
#define XK_backslash 0x005c
#define XK_bracketright 0x005d
#define XK_asciicircum 0x005e
#define XK_underscore 0x005f
#define XK_grave     0x0060

#define XK_a 0x0061
#define XK_b 0x0062
#define XK_c 0x0063
#define XK_d 0x0064
#define XK_e 0x0065
#define XK_f 0x0066
#define XK_g 0x0067
#define XK_h 0x0068
#define XK_i 0x0069
#define XK_j 0x006a
#define XK_k 0x006b
#define XK_l 0x006c
#define XK_m 0x006d
#define XK_n 0x006e
#define XK_o 0x006f
#define XK_p 0x0070
#define XK_q 0x0071
#define XK_r 0x0072
#define XK_s 0x0073
#define XK_t 0x0074
#define XK_u 0x0075
#define XK_v 0x0076
#define XK_w 0x0077
#define XK_x 0x0078
#define XK_y 0x0079
#define XK_z 0x007a

#define XK_braceleft 0x007b
#define XK_bar       0x007c
#define XK_braceright 0x007d
#define XK_asciitilde 0x007e

#endif /* _X11_KEYSYM_H_ */
