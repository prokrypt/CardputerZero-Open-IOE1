# Needs arm-none-eabi-gcc (Debian/Ubuntu: gcc-arm-none-eabi). Output cz-io.bin = flashable 20 KB app image.
CC = arm-none-eabi-gcc
CFLAGS = -mcpu=cortex-m0plus -mthumb -Os -std=gnu11 -Wall -Wextra -Werror -ffreestanding -nostdlib \
         -fno-tree-loop-distribute-patterns -ffunction-sections -Wl,--gc-sections

cz-io.bin: main.c py32.h link.ld mkimg.py
	$(CC) $(CFLAGS) -T link.ld main.c -lgcc -o cz-io.elf
	arm-none-eabi-objcopy -O binary cz-io.elf cz-io.raw
	python3 mkimg.py cz-io.raw $@
	rm -f cz-io.raw

clean:
	rm -f cz-io.elf cz-io.raw cz-io.bin
