#Sources files compile
C_SOURCES = $(wildcard  kernel/arch/*.c kernel/klibc/*.c)
#Headers files list
HEADERS = $(wildcard kernel/arch/include/*h kernel/klibc/include/*.h)
#compiled files .o
OBJ = ${C_SOURCES:.c=.o kernel/arch/cpu.o kernel/arch/interrupts.o} 
#add macro to the cross compiler;
CC =i686-elf-gcc
#add macro to the debugger;
GDB = i686-elf-gdb
#GCC flags
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m32
#create bootable file;
kernel.bin: kernel/boot/boot.o ${OBJ}
	i686-elf-ld  $^ -T Link.ld -o kernel.bin

#create file;
kernel.elf: kernel/boot/boot.o ${OBJ}
	i686-elf-ld -T Link.ld ${OBJ} -o kernel.elf

#run compiled image;
run: kernel.bin
	qemu-system-i386 -kernel kernel.bin -soundhw pcspk -serial stdio -hda diskimage.dd
run-debug:
	qemu-system-i386 -kernel kernel.bin -soundhw pcspk -serial stdio -hda diskimage.dd -d int
#debug compiled image;
run-iso: 
		qemu-system-i386 -cdrom IrisOS.iso -soundhw pcspk -serial stdio -hda diskimage.dd
build-iso: kernel.bin menu.lst
	mkdir -p iso/boot/grub              # create the folder structure
	cp stage2_eltorito iso/boot/grub/   # copy the bootloader
	cp kernel.bin iso/boot/     
	cp menu.lst iso/boot/grub           # copy the grub configuration file
	mkisofs -R                              \
	  -b boot/grub/stage2_eltorito    \
	  -no-emul-boot                   \
	  -boot-load-size 4               \
	  -A os                           \
	  -input-charset utf8             \
	  -quiet                          \
	 -boot-info-table                \
	  -o IrisOS.iso                       \
	  iso
#compile c files;
%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -ffreestanding -c $< -o $@
#compiles nasm files to elf;
%.o: %.asm
	nasm $< -f elf -o $@
#compiles nasm files to bin;
%.bin: %.asm
	nasm $< -f bin -o $@
#clean compiled files;
clean:
	