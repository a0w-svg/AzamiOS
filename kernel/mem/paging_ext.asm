global switch_page_dir
switch_page_dir:
    mov eax, [esp+4]
    mov cr3, eax
    ; Enable paging (PG) without CR0.WP — WP breaks kernel MMIO writes to user-mapped pages.
    mov ebx, cr0
    or ebx, 0x80000001
    mov cr0, ebx
    ret