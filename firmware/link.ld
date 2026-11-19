/* App image at 0x08001000 behind M5's bootloader; the last 4 bytes of the 20 KB slot hold the CRC (mkimg.py). */
MEMORY { FLASH (rx) : ORIGIN = 0x08001000, LENGTH = 0x5000 - 4
         RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 3K }
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  /DISCARD/ : { *(.ARM.*) *(.comment) }
}
