/* UI MCU patch: free flash after the stock image (ends 0x0802F004; the .kspf header limit is 0x0803FFFF). */
MEMORY { FLASH (rx) : ORIGIN = 0x08030000, LENGTH = 0x10000 }

stock_parse  = 0x0800c704;   /* USART frame parser: (state, byte, msg) -> 0 complete, 1 busy, -1 bad */
stock_redraw = 0x0801a844;   /* redraw dispatcher, called every 10 SysTicks from the UI main loop */
stock_flush  = 0x08025040;   /* raise the OLED DMA request (SPI2 queue slot 5 = framebuffer) */

SECTIONS
{
  .text : { KEEP(*(.text.ui_patch)) *(.text*) *(.rodata*) } > FLASH
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
