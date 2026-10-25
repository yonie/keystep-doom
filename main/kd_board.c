/* KeyStep Pro main MCU as a DOOM box: startup and platform layer (MIT).
 *
 * This replaces the stock application at 0x08008000 (the 32 KB boot region below it is untouched).
 * Bring-up follows what the stock firmware does (docs/stock_init.txt): FPU on, flash wait states,
 * HSE + PLL to 180 MHz with over-drive, APB1 /4, APB2 /2, the stock GPIO state, then USART6 on PG14/PG9
 * at the stock 461538 baud with DMA2 stream 6 for transmit.  Frames go to the UI MCU as group-0x0D
 * packets (ui/ui_patch.S), only the 4-byte cells that changed.
 *
 * The USART6 receive handler below (hold Stop -> update mode) is dormant: on the real unit the UI MCU
 * sends this image no button frames.  The way back to stock is Arturia's own: power on with a cable
 * from Clock In to Reset Out.
 *
 * Interface for the engine: doom/kd_platform.h.
 */
#include <stdint.h>
#include "kd_pins.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define RCC_CR       REG(0x40023800)
#define RCC_PLLCFGR  REG(0x40023804)
#define RCC_CFGR     REG(0x40023808)
#define RCC_AHB1ENR  REG(0x40023830)
#define RCC_APB1ENR  REG(0x40023840)
#define RCC_APB2ENR  REG(0x40023844)
#define FLASH_ACR    REG(0x40023C00)
#define PWR_CR       REG(0x40007000)
#define PWR_CSR      REG(0x40007004)
#define SCB_CPACR    REG(0xE000ED88)
#define SCB_VTOR     REG(0xE000ED08)
#define SYST_CSR     REG(0xE000E010)
#define SYST_RVR     REG(0xE000E014)
#define SYST_CVR     REG(0xE000E018)
#define USART6_SR    REG(0x40011400)
#define USART6_DR    REG(0x40011404)
#define USART6_DR_A  0x40011404u
#define USART6_BRR   REG(0x40011408)
#define USART6_CR1   REG(0x4001140C)
#define USART6_CR2   REG(0x40011410)
#define USART6_CR3   REG(0x40011414)
#define DMA2_HISR    REG(0x40026404)
#define DMA2_HIFCR   REG(0x4002640C)
#define S6_CR        REG(0x400264A0)
#define S6_NDTR      REG(0x400264A4)
#define S6_PAR       REG(0x400264A8)
#define S6_M0AR      REG(0x400264AC)
#define S6_FCR       REG(0x400264B4)
#define S6_FLAGS     (0x3Du << 16)

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void kd_doom_main(void);
extern void kd_intro(void);

static volatile uint32_t ms_ticks;
static volatile uint32_t click_ms;                 /* metronome gate open for this many more ms */
static volatile uint32_t stop_since;               /* ms when Stop went down, 0 = up */

#define BOOT_MAGIC_ADDR 0x20000000u
#define BOOT_MAGIC      0xDEADBEEFu
#define SCB_AIRCR    REG(0xE000ED0C)
#define NVIC_ISER2   REG(0xE000E108)

/* ---- clocks ------------------------------------------------------------------------------------ */

static void wait_set(volatile uint32_t *r, uint32_t mask)
{
    for (uint32_t n = 0; n < 1000000u && !(*r & mask); n++)
        ;
}

/* The bootloader jumps here with its own state still live: SYSCLK already on the PLL at full speed
 * (with 5 flash wait states), USB running with its interrupts enabled.  So: interrupts off and
 * cleared, USB reset (the KeyStep drops off the bus), 5 wait states BEFORE anything else (always safe,
 * at any clock), back to HSI with the PLL off, and only then the stock 180 MHz configuration. */
static void handover_reset(void)
{
    for (int i = 0; i < 8; i++) {
        REG(0xE000E180 + 4 * i) = 0xFFFFFFFFu;     /* NVIC ICER: every line off */
        REG(0xE000E280 + 4 * i) = 0xFFFFFFFFu;     /* NVIC ICPR: nothing pending */
    }
    SYST_CSR = 0;
    REG(0x40023814) |= 1u << 7;                    /* RCC AHB2RSTR: OTG_FS reset */
    REG(0x40023814) &= ~(1u << 7);
    REG(0x40023834) &= ~(1u << 7);                 /* RCC AHB2ENR: OTG_FS clock off */
    REG(0x40023810) |= 1u << 29;                   /* RCC AHB1RSTR: OTG_HS reset too */
    REG(0x40023810) &= ~(1u << 29);
}

static void clocks_init(void)
{
    SCB_CPACR |= 0xFu << 20;                       /* FPU on, as stock (we build soft-float anyway) */
    FLASH_ACR = 0x00000705;                        /* 5 wait states first: safe at any SYSCLK */
    RCC_CR |= 1u;                                  /* HSI on */
    wait_set(&RCC_CR, 1u << 1);
    RCC_CFGR = 0;                                  /* SYSCLK = HSI, no prescalers (stock SystemInit) */
    for (uint32_t n = 0; n < 1000000u && (RCC_CFGR & 0xC); n++)
        ;
    RCC_CR &= ~(1u << 24);                         /* PLL off so it can be reprogrammed */
    for (uint32_t n = 0; n < 1000000u && (RCC_CR & (1u << 25)); n++)
        ;
    RCC_APB1ENR |= 1u << 28;                       /* PWR */
    PWR_CR = 0x0000C000;                           /* VOS scale 1 */
    RCC_CR |= 1u << 16;                            /* HSE on */
    wait_set(&RCC_CR, 1u << 17);
    RCC_PLLCFGR = 0x27402D04;                      /* stock: HSE /4 *180 /2 = 180 MHz, Q /7, R /2 */
    RCC_CR |= 1u << 24;                            /* PLL on */
    wait_set(&RCC_CR, 1u << 25);
    PWR_CR |= 1u << 16;                            /* over-drive */
    wait_set(&PWR_CSR, 1u << 16);
    PWR_CR |= 1u << 17;
    wait_set(&PWR_CSR, 1u << 17);
    RCC_CFGR = 0x0000940A & ~3u;                   /* AHB /1, APB1 /4, APB2 /2 */
    RCC_CFGR = 0x0000940A;                         /* SW = PLL */
    wait_set(&RCC_CFGR, 2u << 2);
}

static void pins_init(void)
{
    RCC_AHB1ENR |= 0x0000007Fu | (1u << 22);       /* GPIOA..G, DMA2 */
    RCC_APB2ENR |= 1u << 5;                        /* USART6 */
    for (int i = 0; i < KD_PINS_N; i++)
        REG(kd_pins[i][0]) = kd_pins[i][1];
}

static void link_init(void)
{
    USART6_CR1 = 0;
    USART6_CR2 = 0;
    USART6_CR3 = 0;
    USART6_BRR = 0x000000C3;                       /* 90 MHz / 0xC3 = 461538 baud, as stock */
    USART6_CR1 = 0x0000202C;                       /* UE, RXNEIE, TE, RE */
    USART6_CR3 = 0x00000080;                       /* DMAT */
    S6_CR = 0;
    S6_PAR = USART6_DR_A;
    S6_FCR = 0;
    S6_CR = 0x0A000440;                            /* channel 5, memory -> peripheral, MINC (stock value) */
}

/* ---- metronome click (the stock metronome's own circuit, found in the stock firmware) ---------- */
/* Tone: TIM12 CH1 PWM on PB14 (AF9), ARR 53567 / CCR1 26784 = 50 % square; the stock firmware sets PSC 2
 * (1120 Hz, accent) or 4 (672 Hz) and leaves it running.  Gate: PA7 (line out) and PB1 (speaker) as
 * outputs, high = silent, low = click (stock drives them from TIM1 CH1N/CH3N for ~20 ms per beat). */
#define TIM12_CR1   REG(0x40001800)
#define TIM12_EGR   REG(0x40001814)
#define TIM12_CCMR1 REG(0x40001818)
#define TIM12_CCER  REG(0x40001820)
#define TIM12_PSC   REG(0x40001828)
#define TIM12_ARR   REG(0x4000182C)
#define TIM12_CCR1  REG(0x40001834)
#define GPIOA_MODER REG(0x40020000)
#define GPIOA_BSRR  REG(0x40020018)
#define GPIOB_MODER REG(0x40020400)
#define GPIOB_BSRR  REG(0x40020418)

static void click_init(void)
{
    GPIOA_BSRR = 1u << 7;                          /* gates high (silent) before they become outputs */
    GPIOB_BSRR = 1u << 1;
    GPIOA_MODER = (GPIOA_MODER & ~(3u << 14)) | (1u << 14);
    GPIOB_MODER = (GPIOB_MODER & ~(3u << 2)) | (1u << 2);
    RCC_APB1ENR |= 1u << 6;                        /* TIM12 */
    TIM12_CR1 = 0;
    TIM12_PSC = 4;
    TIM12_ARR = 53567;
    TIM12_CCR1 = 26784;
    TIM12_CCMR1 = 0x60;                            /* PWM mode 1 */
    TIM12_CCER = 1;
    TIM12_EGR = 1;
    TIM12_CR1 = 1;
}

void kd_plat_click(int pitch, int ms)
{
    if (pitch < 1) pitch = 1;
    if (pitch > 30) pitch = 30;
    TIM12_PSC = (uint32_t)pitch;
    TIM12_EGR = 1;                                 /* new pitch now, not after the current period */
    GPIOA_BSRR = 1u << (7 + 16);                   /* gates low: the click sounds */
    GPIOB_BSRR = 1u << (1 + 16);
    if ((uint32_t)ms > click_ms)
        click_ms = (uint32_t)ms;
}

/* ---- platform interface ----------------------------------------------------------------------- */

void kd_plat_init(void) { }                         /* everything is up before kd_doom_main */

uint32_t kd_plat_ms(void) { return ms_ticks; }

void kd_plat_log(const char *s) { (void)s; }

static void link_wait(void);
static uint8_t sent[1024];
static uint8_t pk[256 * 9 + 8 * 9 + 96 * 9];
static uint8_t xq[96 * 9];                         /* extra LED packets for the next frame (intro) */
static uint32_t xn;
static uint32_t frames;

/* ---- status text through the stock UI firmware's text channel (group 8), for bring-up --------- */
/* Same packet sequence the stock firmware sends for "Project 1" / "120.00 BPM" (captured
 * from a stock boot): 01 {00 00 00 05}, 02 <4 chars>..., 01 {rest, NUL, n}, 00 {00 11 90 row}. */

static uint32_t put_pkt(uint8_t *p, uint8_t idx, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    p[0] = 0xAA; p[1] = 0x12; p[2] = 0x08; p[3] = idx;
    p[4] = a; p[5] = b; p[6] = c; p[7] = d;
    p[8] = (uint8_t)(0xAA + 0x12 + 0x08 + idx + a + b + c + d);
    return 9;
}

static uint8_t txt[16 * 9];

void kd_status(int row, const char *s)
{
    uint32_t n = 0, len = 0;
    while (s[len] && len < 30) len++;
    if ((len & 3) == 3) len--;                     /* keep the final chunk short, as the stock chunker does */
    n += put_pkt(&txt[n], 1, 0, 0, 0, 5);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4)
        n += put_pkt(&txt[n], 2, (uint8_t)s[i], (uint8_t)s[i + 1], (uint8_t)s[i + 2], (uint8_t)s[i + 3]);
    uint8_t r[3] = {0, 0, 0};
    uint32_t k = 0;
    for (; i < len; i++) r[k++] = (uint8_t)s[i];
    n += put_pkt(&txt[n], 1, r[0], r[1], r[2], (uint8_t)(k + 1));
    n += put_pkt(&txt[n], 0, 0x00, 0x11, 0x90, (uint8_t)row);
    link_wait();
    DMA2_HIFCR = S6_FLAGS;
    S6_M0AR = (uint32_t)txt;
    S6_NDTR = n;
    S6_CR |= 1u;
    link_wait();
}


static void link_wait(void)
{
    for (uint32_t n = 0; n < 50000000u && (S6_CR & 1u); n++)
        ;
}

/* ---- panel LEDs as a HUD ---------------------------------------------------------------------- */
/* Through the stock UI firmware's own LED handlers:
 *   mod strip   (group 7 element 0, {lo, hi}, 0..200)       ammo of the weapon in hand, a bar from the bottom
 *   pitch strip (group 7 element 1, {100, value}, 0..200)   armour, a bar up from the centre dot
 *   16 steps    (group 3 batches 0x50 / 0x51: R, G, B, slot mask)  health as lit steps,
 *               green above 66, yellow above 33, red below */
static int hud_health = -1, hud_armor = -1, hud_steps = -1, hud_colour = -1;
static int new_health, new_armor, new_steps, new_colour, new_ammo, hud_ammo = -1;
static int hud_dirty;

void kd_plat_hud(int health, int armor, int ammo, int maxammo)
{
    new_health = health < 0 ? 0 : health > 100 ? 100 : health;
    new_armor = armor < 0 ? 0 : armor > 100 ? 100 : armor;
    new_ammo = (ammo < 0 || maxammo <= 0) ? 0 : (ammo * 200 + maxammo - 1) / maxammo;
    if (new_ammo > 200) new_ammo = 200;
    new_steps = (new_health * 16 + 99) / 100;
    new_colour = health > 66 ? 0 : health > 33 ? 1 : 2;
    hud_dirty = 1;
}

static uint32_t led_pkt(uint8_t *p, uint8_t group, uint8_t idx, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    p[0] = 0xAA; p[1] = 0x12; p[2] = group; p[3] = idx;
    p[4] = a; p[5] = b; p[6] = c; p[7] = d;
    p[8] = (uint8_t)(0xAA + 0x12 + group + idx + a + b + c + d);
    return 9;
}

/* queue one UI LED packet to go out with the next frame (used by main/kd_intro.c) */
void kd_led(uint8_t group, uint8_t idx, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (xn + 9 <= sizeof xq)
        xn += led_pkt(&xq[xn], group, idx, a, b, c, d);
}

void kd_hud_resend(void) { hud_ammo = hud_health = hud_armor = hud_steps = hud_colour = -1; hud_dirty = 1; }

static uint32_t hud_packets(uint8_t *buf, uint32_t n)
{
    static const uint8_t rgb[3][3] = { {0x00, 0x7F, 0x00}, {0x7F, 0x5F, 0x00}, {0x7F, 0x00, 0x00} };
    if (!hud_dirty)
        return n;
    hud_dirty = 0;
    if (frames % 20 == 1)                          /* resend all once a second: the UI MCU drops what */
        hud_ammo = hud_health = hud_armor = hud_steps = hud_colour = -1;   /* arrives while it still boots */
    if (new_ammo != hud_ammo)
        n += led_pkt(&buf[n], 7, 0, 0, (uint8_t)new_ammo, 0, 0);
    if (new_armor != hud_armor)
        n += led_pkt(&buf[n], 7, 1, 100, (uint8_t)(100 + new_armor), 0, 0);
    if (new_steps != hud_steps || new_colour != hud_colour) {
        uint32_t lit = new_steps >= 16 ? 0xFFFFu : (1u << new_steps) - 1u;
        const uint8_t *c = rgb[new_colour];
        for (int bank = 0; bank < 2; bank++) {
            uint8_t on = (uint8_t)(lit >> (8 * bank));
            if ((uint8_t)~on)
                n += led_pkt(&buf[n], 3, (uint8_t)(0x50 + bank), 0, 0, 0, (uint8_t)~on);
            if (on)
                n += led_pkt(&buf[n], 3, (uint8_t)(0x50 + bank), c[0], c[1], c[2], on);
        }
    }
    hud_ammo = new_ammo; hud_health = new_health; hud_armor = new_armor; hud_steps = new_steps; hud_colour = new_colour;
    return n;
}

void kd_plat_present(const uint8_t fb[1024])
{
    link_wait();                                   /* previous frame still on the wire */
    uint32_t n = 0;
    if (frames % 20 == 0)                          /* a whole frame once a second: the UI MCU drops what */
        for (int i = 0; i < 1024; i++) sent[i] ^= 0xFF;   /* arrives while it still boots */
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = &fb[i * 4];
        uint8_t *s = &sent[i * 4];
        if (c[0] == s[0] && c[1] == s[1] && c[2] == s[2] && c[3] == s[3])
            continue;
        uint8_t *p = &pk[n];
        p[0] = 0xAA; p[1] = 0x1D; p[2] = 0x0D; p[3] = (uint8_t)i;
        p[4] = c[0]; p[5] = c[1]; p[6] = c[2]; p[7] = c[3];
        p[8] = (uint8_t)(0xAA + 0x1D + 0x0D + i + c[0] + c[1] + c[2] + c[3]);
        s[0] = c[0]; s[1] = c[1]; s[2] = c[2]; s[3] = c[3];
        n += 9;
    }
    frames++;
    n = hud_packets(pk, n);
    for (uint32_t i = 0; i < xn; i++) pk[n + i] = xq[i];
    n += xn;
    xn = 0;
    if (!n)
        return;
    DMA2_HIFCR = S6_FLAGS;
    S6_M0AR = (uint32_t)pk;
    S6_NDTR = n;
    S6_CR |= 1u;
}

/* ---- panel input: the update-mode exit -------------------------------------------------------- */

static void enter_update_mode(void)
{
    *(volatile uint32_t *)BOOT_MAGIC_ADDR = BOOT_MAGIC;
    __asm volatile("dsb");
    SCB_AIRCR = 0x05FA0004 | (SCB_AIRCR & 0x700);  /* SYSRESETREQ, priority grouping kept (as stock) */
    __asm volatile("dsb");
    for (;;) ;
}

static uint8_t rx[9];
static uint32_t rxn;

void USART6_IRQHandler(void)
{
    uint32_t sr = USART6_SR;
    if (!(sr & (1u << 5)) && !(sr & 0x0F))
        return;
    uint8_t b = (uint8_t)USART6_DR;                /* reading DR clears RXNE and the error flags */
    if (rxn == 0 && b != 0xAA)
        return;
    rx[rxn++] = b;
    if (rxn < 9)
        return;
    rxn = 0;
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++) sum += rx[i];
    if (sum != rx[8] || (rx[1] & 0xF0) != 0x10)
        return;
    if (rx[3] == 0x03 && rx[2] == 0x06)
        stop_since = ms_ticks ? ms_ticks : 1;
    else if (rx[3] == 0x03 && rx[2] == 0x05)
        stop_since = 0;
}

/* ---- vectors and reset ------------------------------------------------------------------------ */

void SysTick_Handler(void)
{
    ms_ticks++;
    if (click_ms && --click_ms == 0) {
        GPIOA_BSRR = 1u << 7;                      /* gates high again: silence */
        GPIOB_BSRR = 1u << 1;
    }
    if (stop_since && ms_ticks - stop_since >= 2000)
        enter_update_mode();
}

static void Default_Handler(void) { for (;;) ; }

void __attribute__((noreturn)) Reset_Handler(void)
{
    __asm volatile("cpsid i");
    SCB_VTOR = 0x08008000;
    handover_reset();
    uint32_t *d = &_sdata, *s = &_sidata;
    while (d < &_edata) *d++ = *s++;
    for (d = &_sbss; d < &_ebss; d++) *d = 0;
    for (int i = 0; i < 1024; i++) sent[i] = 0xA5; /* first frame goes out whole */
    clocks_init();
    pins_init();
    link_init();
    click_init();
    SYST_RVR = 180000 - 1;                         /* 1 kHz at 180 MHz */
    SYST_CVR = 0;
    SYST_CSR = 7;
    NVIC_ISER2 = 1u << (71 - 64);                  /* USART6 */
    __asm volatile("cpsie i");
    kd_intro();
    kd_doom_main();
    for (;;) ;
}

__attribute__((section(".vectors"), used))
const void *const kd_vectors[16 + 97] = {
    &_estack, Reset_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, 0, 0, 0, 0, Default_Handler, Default_Handler, 0, Default_Handler, SysTick_Handler,
    [16 ... 16 + 96] = Default_Handler,
    [16 + 71] = USART6_IRQHandler,
};
