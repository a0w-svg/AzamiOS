#Sources files compile
C_SOURCES = $(wildcard kernel/arch/*.c kernel/klibc/*.c kernel/drivers/*.c kernel/klibc/stdio/*.c kernel/mem/*.c kernel/syscall/*.c kernel/filesystem/*.c kernel/userspace/libc/*.c)
#Headers files list
HEADERS = $(wildcard kernel/arch/include/*.h kernel/klibc/include/*.h kernel/drivers/include/*.h kernel/mem/include/*.h kernel/syscall/include/*.h kernel/filesystem/include/*.h kernel/userspace/libc/include/*.h)
#Headers files list
#compiled files .o
OBJ = ${C_SOURCES:.c=.o} kernel/arch/cpu.o kernel/arch/interrupts.o kernel/mem/paging_ext.o 

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

#run compiled image;
run: kernel.bin
	qemu-system-i386 -kernel kernel.bin  -serial stdio
	
#debug compiled image;
run-debug: kernel.bin
	qemu-system-i386 -kernel kernel.bin  -serial stdio -S

build-iso: kernel.bin menu.lst
	mkdir -p iso/boot/grub
	cp stage2_eltorito iso/boot/grub/
	cp kernel.bin iso/boot/     
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
run-iso: AzamiOS.iso
	qemu-system-i386 -cdrom AzamiOS.iso  -serial stdio
	
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
	rm -rf $(OBJ) kernel/boot/boot.o kernel.bin kernel.elf AzamiOS.iso *.bin *.dis