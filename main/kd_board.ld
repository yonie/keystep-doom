/* KeyStep Pro DOOM image: the whole application region, 128 KB SRAM (STM32F446-class). */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 0x0807FFF0 - 0x08008000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_stack_size = 6K;
_estack = ORIGIN(RAM) + LENGTH(RAM);
ENTRY(Reset_Handler)

SECTIONS
{
  .vectors : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .wad : { . = ALIGN(4); KEEP(*(.wad)) . = ALIGN(4); _sidata = .; } > FLASH
  .data : AT(_sidata) { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  ._stack (NOLOAD) : { . = ALIGN(8); . = . + _stack_size; } > RAM
  /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.ARM.exidx*) }
  _flash_end = _sidata + SIZEOF(.data);
}
