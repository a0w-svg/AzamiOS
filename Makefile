#Sources files compile (userspace/libc is intentionally excluded: it belongs only in user program binaries)
C_SOURCES = $(wildcard kernel/arch/*.c kernel/klibc/*.c kernel/drivers/*.c kernel/klibc/stdio/*.c kernel/mem/*.c kernel/syscall/*.c kernel/filesystem/*.c kernel/proc/*.c)
#Headers files list
HEADERS = $(wildcard kernel/arch/include/*.h kernel/klibc/include/*.h kernel/drivers/include/*.h kernel/mem/include/*.h kernel/syscall/include/*.h kernel/filesystem/include/*.h kernel/proc/include/*.h kernel/userspace/libc/include/*.h)
#Headers files list
#compiled files .o
OBJ = ${C_SOURCES:.c=.o} kernel/arch/cpu.o kernel/arch/interrupts.o kernel/mem/paging_ext.o kernel/arch/smp_boot.o

#add macro to the cross compiler;
CC = i686-elf-gcc
#add macro to the debugger;
GDB = i686-elf-gdb
#GCC flags
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m32 -fno-stack-protector

#create bootable file;
kernel.bin: kernel/boot/boot.o ${OBJ}
	i686-elf-ld $^ -T Link.ld -o kernel.bin

#create file;
kernel.elf: kernel/boot/boot.o ${OBJ}
	i686-elf-ld -T Link.ld $^ -o kernel.elf

fat32.img:
	dd if=/dev/zero of=fat32.img bs=1M count=32 status=none
	mkfs.fat -F 32 -n "AZAMIOS" fat32.img
	echo "Hello from FAT32 inside AzamiOS!" > readme.txt
	mcopy -i fat32.img readme.txt ::README.TXT
	rm -f readme.txt

#run compiled image;
run: kernel.bin initrd.tar fat32.img
	qemu-system-i386 -kernel kernel.bin -initrd initrd.tar -hda fat32.img -smp 4 -serial stdio

#build the userspace initrd (wm and shell ELFs inside a ustar archive)
initrd.tar: kernel/userspace/wm/wm kernel/userspace/shell/shell
	mkdir -p kernel/userspace/bin
	cp kernel/userspace/wm/wm kernel/userspace/bin/wm
	cp kernel/userspace/shell/shell kernel/userspace/bin/shell
	cd kernel/userspace/bin && tar --format=ustar -cf ../../../initrd.tar wm shell

.PHONY: kernel/userspace/wm/wm kernel/userspace/shell/shell
kernel/userspace/wm/wm:
	$(MAKE) -C kernel/userspace/wm
kernel/userspace/shell/shell:
	$(MAKE) -C kernel/userspace/shell
	
#debug compiled image;
run-debug: kernel.bin
	qemu-system-i386 -kernel kernel.bin -smp 4 -serial stdio #-S

build-iso: kernel.bin initrd.tar menu.lst
	mkdir -p iso/boot/grub
	cp stage2_eltorito iso/boot/grub/
	cp kernel.bin iso/boot/
	cp initrd.tar iso/boot/
	cp menu.lst iso/boot/grub
	mkisofs -R \
	  -b boot/grub/stage2_eltorito \
	  -no-emul-boot \
	  -boot-load-size 4 \
	  -A os \
	  -input-charset utf8 \
	  -quiet \
	  -boot-info-table \
	  -o AzamiOS.iso \
	  iso
	  
#run Azami's iso file;
run-iso: build-iso fat32.img
	qemu-system-i386 -cdrom AzamiOS.iso -hda fat32.img -boot d -smp 4 -serial stdio
	
#compile c files;
%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@
	
#compiles nasm files to elf;
%.o: %.asm
	nasm $< -f elf -o $@
	
#compiles nasm files to bin;
%.bin: %.asm
	nasm $< -f bin -o $@
	
#clean binary files;
clean: 
	rm -rf $(OBJ) kernel/boot/boot.o kernel.bin kernel.elf AzamiOS.iso *.bin *.dis initrd.tar fat32.img iso/ kernel/userspace/bin/
	$(MAKE) -C kernel/userspace/wm clean
	$(MAKE) -C kernel/userspace/shell clean
	rm -rf kernel/userspace/hello