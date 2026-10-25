/* KeyStep Pro UI MCU (STM32F091-class, Cortex-M0) patch: a raw-pixel packet group for the OLED.
 *
 * The stock UI firmware only renders TEXT the main MCU sends (group 8).  This patch adds group 0x0D:
 *
 *   AA 1D 0D idx v0 v1 v2 v3 chk     write framebuffer bytes [idx*4 .. idx*4+3] = v0..v3
 *                                    (idx 0..255 covers the whole 1024-byte SSD1306 page buffer)
 *                                    and enter frame mode (the stock redraw is suspended)
 *   AA 1E 0D xx xx xx xx xx chk      leave frame mode (the stock screens come back)
 *
 * The framebuffer at 0x200008FC is pushed to the SSD1306 by the firmware's own SPI2 DMA, so nothing
 * else is needed to get pixels on the glass.  Two call sites in the stock image are redirected here
 * (see tools/build.py): the frame parser call in the USART consumer, and the redraw dispatcher call
 * in the UI main loop.
 */
    .syntax unified
    .cpu cortex-m0
    .thumb

    .equ FB,     0x200008FC
    .equ FLAG,   0x20007000          /* in the 23 KB the stock firmware never writes (docs/measure.json) */
    .equ MAGIC,  0x4B53444D          /* "MDSK": frame mode on.  Any other value (incl. power-on noise) = off */
    .equ GROUP,  0x0D
    /* FLAG+4 = DIRTY (nonzero = flush wanted) */

    .section .text.ui_patch, "ax"

/* replaces `bl 0x0800c704` at 0x0800c7e0.  r0 = parser state, r1 = byte, r2 = 7-byte message out.
 * Returns what the parser returned (0 = complete frame -> dispatch to listeners), except that a
 * complete group-0x0D frame is consumed here and reported as 1 ("nothing to dispatch"). */
    .global ui_rx_hook
    .thumb_func
ui_rx_hook:
    push    {r4, lr}
    mov     r4, r2
    bl      stock_parse
    cmp     r0, #0
    bne     1f
    ldrb    r3, [r4, #1]            /* group */
    cmp     r3, #GROUP
    bne     1f
    ldr     r1, =FLAG
    ldrb    r3, [r4, #0]            /* b1: 0x1E = leave frame mode */
    cmp     r3, #0x1E
    beq     2f
    ldr     r2, =MAGIC
    str     r2, [r1]
    str     r2, [r1, #4]            /* DIRTY: the frame changed since the last flush */
    ldrb    r3, [r4, #2]            /* idx */
    lsls    r3, r3, #2
    ldr     r2, =FB
    adds    r2, r2, r3
    ldrb    r3, [r4, #3]
    strb    r3, [r2, #0]
    ldrb    r3, [r4, #4]
    strb    r3, [r2, #1]
    ldrb    r3, [r4, #5]
    strb    r3, [r2, #2]
    ldrb    r3, [r4, #6]
    strb    r3, [r2, #3]
    movs    r0, #1
    pop     {r4, pc}
2:  movs    r2, #0
    str     r2, [r1]
    movs    r0, #1
1:  pop     {r4, pc}

/* replaces `bl 0x0801a844` at 0x0800f162.  In frame mode the stock redraw would paint its own
 * screen over ours, so it is skipped; instead, if the frame changed, the firmware's own OLED flush
 * request (DMA slot 5 = the 1024-byte framebuffer) is raised.  Otherwise the redraw runs unchanged.
 * The caller ignores r0. */
    .global ui_redraw_hook
    .thumb_func
ui_redraw_hook:
    ldr     r1, =FLAG
    ldr     r3, [r1]
    ldr     r2, =MAGIC
    cmp     r3, r2
    beq     3f
    push    {r4, lr}
    bl      stock_redraw
    pop     {r4, pc}
3:  ldr     r2, [r1, #4]
    cmp     r2, #0
    beq     4f
    movs    r2, #0
    str     r2, [r1, #4]
    push    {r4, lr}
    bl      stock_flush
    pop     {r4, pc}
4:  bx      lr

    .ltorg
