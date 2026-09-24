/* ELF note identifying the vDSO's kernel, as Linux's own vdso/note.S does:
 * name "Linux", type 0, desc = LINUX_VERSION_CODE. glibc reads it to learn
 * the running kernel version without a uname() call. The version matches
 * the release string uname(2) reports (7.0.0). */
    .section .note.Linux, "a", @note
    .balign 4
    .long 2f - 1f           /* namesz */
    .long 4f - 3f           /* descsz */
    .long 0                 /* type   */
1:  .asciz "Linux"
2:  .balign 4
3:  .long 0x070000          /* KERNEL_VERSION(7, 0, 0) */
4:  .balign 4

    .section .note.GNU-stack, "", @progbits
