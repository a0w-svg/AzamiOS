
global switch_page_dir
switch_page_dir:
    mov eax, [esp+4]
    mov cr3, eax
    mov ebx, cr0
    or ebx, 0x80010000
    mov cr0, ebx
    ret