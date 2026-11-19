// Open-source app image for the M5IOE1 I/O expander (PY32F002B) on Cardputer Zero. Replaces M5's closed
// stock app image at 0x08001000 behind M5's I2C bootloader. Contract: ../README.md (register map, power
// state machine, 0xFD IAP); conformance: ../tests/test_open.py.
// 24 MHz HSI (the bootloader's clock), so the 0xFD handover needs no clock change. All work runs in the main
// loop once per ms; SysTick only counts and the I2C ISR only moves bytes and sets flags.
#include "py32.h"

#define HCLK 24000000u
#define FW_MINOR 0x10                   // 0xBA: stock 0x01; 0x10+ = this firmware

// IO index (register bit) -> pin. Linux m5ioe1 gpio n = index n.
static GPIO_T *const PORT[14] = {GPIOB, GPIOB, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA,
                                 GPIOB, GPIOA, GPIOA, GPIOB, GPIOB, GPIOA, GPIOB};
static const uint8_t PIN[14] = {5, 1, 1, 3, 4, 5, 6, 0, 0, 7, 2, 6, 2, 7};
enum { IO_SW = 4, IO_BL = 9, IO_SWDIO = 11, IO_SWCLK = 12, IO_PWR = 13 };
static const uint8_t LED_IO[3] = {7, 8, 10};    // Aa PB0, Fn PA0, Sym PB2 (active low)
static const uint8_t LED_AF[3] = {2, 2, 3};
static const uint8_t LED_CH[3] = {1, 0, 2};     // TIM1 CH2, CH1, CH3 (CCR index)
static const uint8_t ADC_IO[4] = {1, 3, 4, 6};  // ADC_CTRL ch 1-4 = IO2/IO4/IO5/IO7 = ADC ch 0-3

static volatile uint32_t tick;                  // ms

// --- host-visible registers (written by the I2C ISR, applied by the main loop)
static volatile uint16_t g_mode, g_out, g_pu, g_pd, g_drv = 0x3fff;
static volatile uint8_t adc_ctrl, temp_ctrl, iic_cfg, ready, lock = 1, state, led_mode[3], rtc[32];
static volatile int16_t temp_c;
static volatile uint16_t adc_d, pwm4, pwm_freq = 500, ref_mv = 3300, off_time = 2000, thresh = 1000, sw_mv;
static volatile uint8_t f_gpio, f_pwm, f_lock, f_iap, f_i2c_reset;
static uint8_t hwid, poweron_done, gpio_touched, walk;
static uint16_t uid;

// --- pins
static void pin_mode(int io, unsigned m) {      // 0 in, 1 out, 2 AF, 3 analog
    GPIO_T *g = PORT[io]; unsigned s = PIN[io] * 2;
    g->MODER = (g->MODER & ~(3u << s)) | m << s;
}
static void pin_af(int io, unsigned af) {       // all AF pins used here are < 8: AFRL
    GPIO_T *g = PORT[io]; unsigned s = PIN[io] * 4;
    uint32_t v = (g->AFR[0] & ~(15u << s)) | af << s;
    if (g->AFR[0] != v) g->AFR[0] = v;
    pin_mode(io, 2);
}
static void pin_set(int io, int hi) { if (hi) PORT[io]->BSRR = 1u << PIN[io]; else PORT[io]->BRR = 1u << PIN[io]; }
static void host_pin(int io) {                  // the host's GPIO_M/O/PU/PD/DRV setting for one pin
    GPIO_T *g = PORT[io]; unsigned n = PIN[io], b = 1u << io;
    unsigned pu = g_pu & b, pd = g_pd & b;      // both set: floats
    g->OTYPER = (g->OTYPER & ~(1u << n)) | (g_drv & b ? 1u << n : 0);
    g->PUPDR = (g->PUPDR & ~(3u << 2 * n)) | (pu && !pd ? 1u : pd && !pu ? 2u : 0u) << 2 * n;
    pin_set(io, g_out & b);
    pin_mode(io, g_mode & b ? 1 : 0);
}

// --- LEDs and backlight on TIM1 (CH1-3 LEDs, CH4 backlight on PA7)
struct led { uint32_t cur; uint16_t blink; uint8_t dark, phase, prev, owned; };
static struct led L[3];
static uint8_t tim_on, bl_valid, bl_off;
static uint32_t bl_off_t;                       // tick when the host cleared PWM4 EN
static uint16_t tim_freq;
static uint32_t tim_max, bl_cur, bl_tgt;        // max = ARR+1; cur/tgt in 8.8 fixed point

static int owned(int io) {
    if (io == IO_SW) return 1;
    if (io == IO_PWR) return lock;
    if (io == IO_BL) return tim_on;
    for (int i = 0; i < 3; i++) if (io == LED_IO[i]) return L[i].owned;
    return 0;
}
static void apply_gpio(void) {
    for (int io = 0; io < 14; io++)             // SWD pins stay SWD until the host configures any GPIO
        if (!owned(io) && (gpio_touched || (io != IO_SWDIO && io != IO_SWCLK))) host_pin(io);
}

static void timebase(void) {
    uint16_t req = pwm_freq;                    // read once: the ISR may write it meanwhile
    uint32_t f = req ? req : 500, period = HCLK / f, psc = (period - 1) >> 16;
    tim_max = period / (psc + 1);
    TIM1->PSC = psc;
    TIM1->ARR = tim_max - 1;
    tim_freq = req;
}
static void tim_stop(void) {
    TIM1->CR1 = 0;
    RCC->APBRSTR2 |= RCC_TIM1; RCC->APBRSTR2 &= ~RCC_TIM1;
    tim_on = 0;
    bl_cur = bl_tgt = tim_max << 8;             // dark: the next enable fades up from black
    host_pin(IO_BL);
}
static void apply_pwm(void) {
    uint16_t p = pwm4;
    // EN off: hold 0.1 s, then fade out and stop (bl_tick). The m5ioe1 probe disables every PWM and pwm_bl
    // re-enables ~8 ms later (cz dmesg): without the hold that blinked the backlight off and faded it up at boot.
    if (!(p & 0x8000)) { if (tim_on && !bl_off) { bl_off = 1; bl_off_t = tick; } return; }
    bl_off = 0;
    uint32_t old = tim_max;
    int newf = !tim_on || tim_freq != pwm_freq;
    if (newf) timebase();
    if (!tim_on && bl_valid) bl_cur = tim_max << 8;   // re-enable after a stop: from black, in the new ARR units
    else if (newf && bl_valid) {                // new frequency while on: same brightness in the new ARR units
        bl_cur = (uint64_t)bl_cur * tim_max / old;
        for (int i = 0; i < 3; i++) L[i].cur = ~0u;   // out of range: LEDs snap
    }
    bl_tgt = ((4096u - (p & 0xfff)) * tim_max >> 12) << 8;   // CCR4 = (4096 - duty) * (ARR+1) / 4096
    if (!bl_valid) { bl_cur = bl_tgt; bl_valid = 1; }   // cold start: jump, no fade
    uint32_t ccer = 0x1333 | (p & 0x4000 ? 0x2000 : 0);   // CH1-3 on + inverted (low = lit), CH4 on, POL
    if (!tim_on) {
        TIM1->CCMR1 = 0x6868;                   // CH1, CH2 PWM mode 1 + preload
        TIM1->CCMR2 = 0x7868;                   // CH3 PWM mode 1, CH4 PWM mode 2 (+ preload)
        TIM1->CCR[3] = bl_cur >> 8;
        TIM1->CCER = ccer;
        TIM1->BDTR = 1u << 15;                  // MOE
        TIM1->EGR = 1;
        TIM1->CR1 = 0x81;                       // ARPE + CEN
        pin_af(IO_BL, 2);
        tim_on = 1;
    } else if (TIM1->CCER != ccer) TIM1->CCER = ccer;
}

static uint32_t ease(uint32_t cur, uint32_t tgt, uint32_t step) {
    if (cur < tgt) return tgt - cur > step ? cur + step : tgt;
    return cur - tgt > step ? cur - step : tgt;
}

// One LED per ms (fade.S hook_led + leds): mode reg 0xBD+i = 0 off, 1 blink 500/500, 2 fast 150/150, 3 on;
// RTC RAM 0x8D+i = bits 0-4 level (0 = full), bit 7 breathe (mode 3 slow ~1 s, mode 1 fast; mode 2 ignores it).
static void led_tick(int i, int walk_led) {
    struct led *l = &L[i];
    uint8_t m = led_mode[i], x = rtc[0x1d + i], lvl = x & 31 ? x & 31 : 31, br = x >> 7, want;
    enum { NORMAL, QUICK, JUMP, SLOW } kind = NORMAL;
    if (walk) { want = i == walk_led ? 31 : 0; kind = QUICK; br = 0; m = 0xff; }
    else if (m == 0) { want = 0; br = 0; }
    else if (m == 2) { want = (tick / 150) & 1 ? 0 : lvl; kind = JUMP; br = 0; }
    else if (m == 1 && !br) {                   // own phase, lit for a full half first (stock's is shared)
        if (l->prev != 1) { l->blink = 0; l->dark = 0; }
        else if (++l->blink >= 500) { l->blink = 0; l->dark ^= 1; }
        want = l->dark ? 0 : lvl;
    } else { want = lvl; if (m == 3 && br) kind = SLOW; }
    l->prev = m;
    if (!br) l->phase = 0;
    uint32_t tgt = want ? (uint32_t)(want + 1) * (want + 1) * tim_max >> 10 : 0;   // perceptual
    if (br && !l->phase) tgt = 0;               // breathe: phase 0 down to 0, phase 1 up to level
    tgt <<= 8;

    int io = LED_IO[i];
    if (!(walk || m || l->cur)) {               // off and faded out: the pin goes back to the host
        if (l->owned) { l->owned = 0; host_pin(io); }
        return;
    }
    l->owned = 1;
    if (!tim_on) {                              // timer stopped (PWM4 off): plain output, on or off
        l->cur = tgt;
        pin_set(io, !tgt);
        pin_mode(io, 1);
        return;
    }
    if (l->cur > tim_max << 8) l->cur = tgt;
    uint32_t c = l->cur;
    l->cur = kind == JUMP ? tgt
           : kind == QUICK ? ease(c, tgt, c >> 5 > 1024 ? c >> 5 : 1024)   // ~0.15 s: walk steps overlap
           : kind == SLOW ? ease(c, tgt, c >> 8 > 64 ? c >> 8 : 64)        // ~1 s each way
           : ease(c, tgt, c >> 6 > 256 ? c >> 6 : 256);                    // ~0.3 s full swing
    if (br && l->cur == tgt) l->phase ^= 1;
    TIM1->CCR[LED_CH[i]] = l->cur >> 8;
    pin_af(io, LED_AF[i]);
}

static void bl_tick(void) {                     // fade.S bl_fade: each ms the duty moves 1/128 of itself
    if (!tim_on) return;
    if (bl_off && tick - bl_off_t >= 100) {
        bl_tgt = tim_max << 8;                  // fade to dark, then the timer stops
        if (bl_cur == bl_tgt) { bl_off = 0; tim_stop(); return; }
    }
    if (bl_cur > tim_max << 8) bl_cur = bl_tgt;
    uint32_t step = ((tim_max << 8) - bl_cur) >> 7;    // duty = ARR+1 - pulse (inverted output)
    bl_cur = ease(bl_cur, bl_tgt, step > 32 ? step : 32);
    TIM1->CCR[3] = bl_cur >> 8;
}

// --- ADC
static void delay_us(uint32_t us) { for (volatile uint32_t n = us * 3; n; n--) ; }   // >= 1 us per count at 24 MHz
static int adc_raw(unsigned ch) {               // stock register sequence (0x80012c8), -1 on timeout
    ADC1->CHSELR = 1u << ch;
    ADC1->CR = ADC_CR_ADEN;
    delay_us(10);                               // stock waits 10 us between ADEN and ADSTART
    ADC1->CR = ADC_CR_ADSTART;
    int n = 20000;
    while (n && !(ADC1->ISR & ADC_ISR_EOC)) n--;
    ADC1->ISR = ADC_ISR_EOC;
    int v = n ? (int)(ADC1->DR & 0xfff) : -1;
    ADC1->CR = ADC_CR_ADDIS;
    ADC1->CHSELR = 0;
    return v;
}
static int adc_pin(int io, unsigned ch) {       // a host pin is analog only while it is sampled
    if (io == IO_SW) return adc_raw(ch);
    uint32_t moder = PORT[io]->MODER;
    pin_mode(io, 3);
    int v = adc_raw(ch);
    PORT[io]->MODER = moder;
    return v;
}
static uint16_t to_mv(uint32_t raw) { return (raw * ref_mv + 2047) / 4095; }
// Internal sensor (ADC ch 8), stock formula (0x8004816): raw scaled to 3.3 V, then the line through the factory
// calibration at 30 C (0x1FFF0114) and 85 C (0x1FFF0118).
static void temp(void) {
    ADC_CCR |= 1u << 23;                        // TSEN
    delay_us(60);                               // sensor start-up (DS tSTART up to ~120 us)
    int sum = 0, n = 0;                         // mean of 16 (one sample read +-3 C on cz; ~1 ms)
    for (int i = 0; i < 16; i++) { int r = adc_raw(ADC_CH_TEMP); if (r >= 0) { sum += r; n++; } }
    ADC_CCR &= ~(1u << 23);
    int c1 = TS_CAL30, c2 = TS_CAL85;
    if (n && c2 != c1) temp_c = ((int)((sum + n / 2) / n * ref_mv / 3300) - c1) * 55 / (c2 - c1) + 30;
}
static void vref(void) {                        // one bad sample must not skew the switch reading for 1 s
    int r = adc_raw(ADC_CH_VREFINT);
    uint32_t mv = r > 0 ? 1200u * 4095 / r : 0;
    if (mv >= 2000 && mv <= 3600) ref_mv = mv;
}

// --- power state machine (every 50 ms): 0 off, 1 on, 2 off-debounce, 3 countdown, 4 cutting
static uint32_t t_state;
static void pwr(int on) { pin_set(IO_PWR, on); pin_mode(IO_PWR, 1); PORT[IO_PWR]->OTYPER &= ~(1u << PIN[IO_PWR]); }
static int pwr_mode_out(void) { return (PORT[IO_PWR]->MODER >> 14 & 3) == 1; }
static int pwr_high(void) { return pwr_mode_out() && PORT[IO_PWR]->ODR >> 7 & 1; }

static void power_poll(void) {
    int r = adc_raw(2);
    if (r >= 0) sw_mv = to_mv(r);               // timeout: keep the last reading
    int on = sw_mv > thresh;
    if (!lock) return;
    uint32_t now = tick;
    switch (state) {
    case 0:
        if (!on) { pwr(0); break; }
        pwr(1);
        if (!poweron_done) {                    // fade.S: PWM4 on at duty 0 (stock 0x8800); the splash fades it up
            poweron_done = 1;
            pwm4 = 0x8000; pwm_freq = 20000; g_drv = 0x1dff;
            f_pwm = 1; f_gpio = 1;
        }
        state = 1;
        break;
    case 1: if (!on) { state = 2; t_state = now; } break;
    case 2:
        if (on) state = 1;
        else if (now - t_state >= 100) { state = 3; t_state = now; ready = 0; }
        break;
    case 3:                                     // flipping back on does not cancel (stock); OFF_TIME read live
        if (now - t_state >= off_time) { pwr(0); state = 4; t_state = now; }
        break;
    case 4: if (now - t_state >= 100) state = 0; break;
    }
}

static uint32_t walk_t0;
static void per_ms(uint32_t ms) {
    if (f_lock) { f_lock = 0; state = 0; }
    if (ms % 50 == 0) power_poll();
    if (ms % 1000 == 0) vref();
    uint8_t w = ready ? 0 : (state == 1 || state == 2) ? 1 : state == 3 ? 2 : 0;   // boot / shutdown walk
    if (w != walk) { walk = w; walk_t0 = ms; }
    unsigned step = (ms - walk_t0) / 300 % 3;   // Aa -> Fn -> Sym, 300 ms each; shutdown the other way
    for (int i = 0; i < 3; i++) led_tick(i, walk == 1 ? step : 2 - step);
    bl_tick();
}

// --- I2C register file. A write sets the pointer, then data bytes applied at STOP; reads start at the pointer.
static uint16_t hw_bits(int out) {              // firmware-owned pins read back as their real state
    uint16_t r = 0;
    for (int io = 0; io < 14; io++) {
        GPIO_T *g = PORT[io]; unsigned n = PIN[io];
        if (owned(io) && (out ? (g->MODER >> 2 * n & 3) == 1 : g->ODR >> n & 1)) r |= 1u << io;
    }
    return r;
}
static uint16_t own_mask(void) { uint16_t r = 0; for (int io = 0; io < 14; io++) if (owned(io)) r |= 1u << io; return r; }
static uint16_t gpio_in(void) {
    uint16_t r = 0;
    for (int io = 0; io < 14; io++) if (PORT[io]->IDR >> PIN[io] & 1) r |= 1u << io;
    return r;
}
static uint8_t status(void) {
    return (sw_mv > thresh) | pwr_high() << 1 | (walk == 1) << 2 | (walk == 2) << 3 | poweron_done << 4;
}

#define R16(lo, x) case lo: return (x); case lo + 1: return (x) >> 8;
static uint8_t rd(uint8_t a) {
    switch (a) {
    R16(0x00, uid)
    case 0x02: return 0x5a;                     // REV: m5ioe1 sets READY only if it reads 0x5A
    R16(0x03, (g_mode & ~own_mask()) | hw_bits(1))
    R16(0x05, (g_out & ~own_mask()) | hw_bits(0))
    R16(0x07, gpio_in())
    R16(0x09, g_pu)
    R16(0x0b, g_pd)
    R16(0x13, g_drv)
    case 0x15: return adc_ctrl;
    R16(0x16, adc_d)
    case 0x18: return temp_ctrl;
    R16(0x19, (uint16_t)temp_c)
    R16(0x21, pwm4)
    case 0x23: return iic_cfg;
    R16(0x25, pwm_freq)
    R16(0x27, ref_mv)
    case 0xb0: return ready;
    case 0xb1: return lock;
    R16(0xb2, off_time)
    R16(0xb4, thresh)
    case 0xb6: return state;
    case 0xb8: return hwid;
    case 0xb9: return status();
    case 0xba: return FW_MINOR;
    R16(0xbb, sw_mv)
    case 0xbd: case 0xbe: case 0xbf: return led_mode[a - 0xbd];
    case 0xfe: return 0x03;
    case 0xff: return 0x32;
    }
    return a >= 0x70 && a < 0x90 ? rtc[a - 0x70] : 0;
}

static uint16_t set8(uint16_t old, int hi, uint8_t v) { return hi ? (old & 0xff) | v << 8 : (old & 0xff00) | v; }
#define W16(lo, var, then) case lo: case lo + 1: var = set8(var, a - lo, v); then; break;
static void wr(uint8_t a, uint8_t v) {
    uint16_t t;
    switch (a) {
    W16(0x03, g_mode, f_gpio = gpio_touched = 1)
    W16(0x05, g_out, f_gpio = gpio_touched = 1)
    W16(0x09, g_pu, f_gpio = gpio_touched = 1)
    W16(0x0b, g_pd, f_gpio = gpio_touched = 1)
    W16(0x13, g_drv, f_gpio = gpio_touched = 1)
    case 0x18: if (v & 0x40) temp_ctrl = 0xc0; break;   // start; reads 0 when done
    case 0x15: {                                // 0x40 + ch 1-4; IO5 (power sense) refused while 0xB1 = 1
        unsigned ch = v & 7;
        if (v & 0x40 && ch >= 1 && ch <= 4 && !(ch == 3 && lock)) adc_ctrl = v | 0x80;
        break;
    }
    W16(0x21, pwm4, f_pwm = 1)
    case 0x23: iic_cfg = v; break;              // stored only: the stock idle sleep ignores the switch
    W16(0x25, pwm_freq, f_pwm = 1)
    case 0x29:
        if (v == 0x3a) { SCB_AIRCR = 0x05fa0004; __asm volatile("dsb"); for (;;) ; }
        break;
    case 0xb0: ready = v & 1; break;
    case 0xb1:
        if ((v & 1) == lock) break;
        if (lock) {                             // hand PB7 to the host as it is now (stock): output high stays high
            uint16_t b = 1u << IO_PWR;
            g_mode = pwr_mode_out() ? g_mode | b : g_mode & ~b;
            g_out = GPIOB->ODR >> 7 & 1 ? g_out | b : g_out & ~b;
            g_drv &= ~b;
        }
        lock = v & 1; f_lock = 1;
        break;
    case 0xb2: case 0xb3: t = set8(off_time, a - 0xb2, v); off_time = t < 200 ? 200 : t; break;   // clamp per byte
    case 0xb4: case 0xb5: t = set8(thresh, a - 0xb4, v); thresh = t > 3300 ? 3300 : t; break;
    case 0xbd: case 0xbe: case 0xbf: if (v <= 3) led_mode[a - 0xbd] = v; break;
    case 0xfd: if (v == 0x1a) f_iap = 1; break;
    default: if (a >= 0x70 && a < 0x90) rtc[a - 0x70] = v;
    }
}

#define SNAP 32
static uint8_t ptr, wbuf[40], wn, rx_first, txi, tx[SNAP];
static volatile uint32_t txn_since;             // tick at ADDR
static volatile uint8_t txn_open;

static void commit(void) {
    for (int k = 0; k < wn; k++) wr(ptr + k, wbuf[k]);
    wn = 0;
}
void I2C1_IRQHandler(void) {
    uint32_t sr1 = I2C1->SR1;
    if (sr1 & I2C_SR1_ADDR) {
        uint32_t sr2 = I2C1->SR2;               // SR1 then SR2 read clears ADDR
        commit();                               // repeated start ends the write phase
        if (sr2 & I2C_SR2_TRA) { for (int k = 0; k < SNAP; k++) tx[k] = rd(ptr + k); txi = 0; }
        rx_first = !(sr2 & I2C_SR2_TRA);
        txn_since = tick; txn_open = 1;
    }
    if (sr1 & I2C_SR1_RXNE) {
        uint8_t b = I2C1->DR;
        if (rx_first) { ptr = b; rx_first = 0; }
        else if (wn < sizeof wbuf) wbuf[wn++] = b;
    }
    if (sr1 & I2C_SR1_TXE) { I2C1->DR = txi < SNAP ? tx[txi] : rd(ptr + txi); txi++; }
    if (sr1 & I2C_SR1_AF) { I2C1->SR1 = ~I2C_SR1_AF & 0xffff; txn_open = 0; }
    if (sr1 & I2C_SR1_STOPF) { I2C1->CR1 = I2C1->CR1; commit(); txn_open = 0; }   // SR1 read + CR1 write clears
    if (sr1 & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)) {
        I2C1->SR1 = ~(I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR) & 0xffff;
        wn = 0; txn_open = 0; f_i2c_reset = 1;
    }
}

static void i2c_init(void) {
    RCC->APBRSTR1 |= RCC_I2C; RCC->APBRSTR1 &= ~RCC_I2C;
    I2C1->CR2 = 0x700 | HCLK / 1000000;         // ITERREN, ITEVTEN, ITBUFEN, FREQ
    I2C1->CCR = HCLK / 200000;                  // 100 kHz (master timing; unused as a slave)
    I2C1->TRISE = HCLK / 1000000 + 1;
    I2C1->OAR1 = 0x4f << 1;                     // 0x50 strap (IO7 high) dropped: cz is 0x4F
    I2C1->CR1 = I2C_CR1_PE;
    I2C1->CR1 = I2C_CR1_PE | I2C_CR1_ACK;
    wn = 0; txn_open = 0;
}

// --- clock: HSI 24 MHz exactly as stock leaves it before jumping to the bootloader (0x8001e1c)
static void clock_24m(void) {
    RCC->ICSCR = (RCC->ICSCR & 0xffff0000) | HSI_TRIM_24M;
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) ;
    RCC->CFGR = 0;
    while (RCC->CFGR & 0x38) ;
    RCC->CR = RCC_CR_HSION;
    RCC->CIER = 0;
    RCC->CICR = ~0u;
}

static void feed(void) { IWDG->KR = 0xaaaa; }

// 0xFD = 0x1A: hand over to M5's bootloader (I2C 0x5F) like stock 0x8001a28. GPIO is left alone, so PWR_EN
// stays high while flashing; the IWDG keeps running (the bootloader feeds it).
static void iap(void) {
    uint32_t sp = *(volatile uint32_t *)0x08000000, pc = *(volatile uint32_t *)0x08000004;
    if ((sp & 0x2ffe0000) != 0x20000000) return;    // no bootloader: ignore, like stock
    __asm volatile("cpsid i");
    RCC->APBRSTR1 |= RCC_I2C; RCC->APBRSTR1 &= ~RCC_I2C;
    I2C1->CR1 = 0;
    RCC->APBRSTR2 |= RCC_TIM1; RCC->APBRSTR2 &= ~RCC_TIM1;
    TIM1->CR1 = 0;
    ADC1->CR = (ADC1->CR & 0x7fffffe8) | ADC_CR_ADDIS;
    SYST->CTRL = 0;
    NVIC_ICER = ~0u;
    NVIC_ICPR = ~0u;
    EXTI->EXTICR[0] = 0; EXTI->EXTICR[1] = 0;
    clock_24m();
    SYST->CTRL = 0; SYST->LOAD = 0; SYST->VAL = 0;
    feed();
    __asm volatile("cpsie i\n msr msp, %0\n bx %1" :: "r"(sp), "r"(pc));
}

void SysTick_Handler(void) { tick++; }

int main(void) {
    __asm volatile("cpsid i");                  // whatever IRQs the bootloader left enabled
    NVIC_ICER = ~0u; NVIC_ICPR = ~0u;
    SCB_VTOR = 0x08001000;
    SCB_SCR = 0;                                // WFI = sleep, never STOP (SysTick must keep running)
    // Hardware IWDG (option byte) runs from reset at ~0.5 s: LSI/32, reload 4000 = ~3.9 s, like stock
    IWDG->KR = 0xcccc; IWDG->KR = 0x5555; IWDG->PR = 3; IWDG->RLR = 4000;
    for (int n = 100000; n && IWDG->SR; n--) ;
    feed();
    clock_24m();
    FLASH_ACR = 0;                              // 0 wait states at 24 MHz
    RCC->IOPENR |= 3;                           // GPIOA, GPIOB
    RCC->APBENR1 |= RCC_I2C;
    RCC->APBENR2 |= RCC_TIM1 | RCC_ADC;
    RCC->APBRSTR2 |= RCC_TIM1 | RCC_ADC; RCC->APBRSTR2 &= ~(RCC_TIM1 | RCC_ADC);   // whatever the bootloader left

    // UID: low 16 bits of CRC-32/MPEG-2 over the 12 UID bytes in memory order (= stock's byte-swapped words)
    uint32_t c = 0xffffffff;
    for (int i = 0; i < 12; i++) {
        c ^= (uint32_t)UID_BYTES[i] << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000 ? c << 1 ^ 0x04c11db7 : c << 1;
    }
    uid = c;

    pin_mode(IO_SW, 3);                         // power switch sense: analog, always
    ADC1->CFGR1 = 0; ADC1->CFGR2 = 0x20000000; ADC1->SMPR = 7; ADC1->CFGR1 = 0x1000;   // stock config
    ADC_CCR = 1u << 22;                         // VREFINT on
    delay_us(100);                              // VREFINT settle (stock has a settle loop too)
    ADC1->CR = ADC_CR_ADCAL;
    for (int n = 20000; n && ADC1->CR & ADC_CR_ADCAL; n--) ;
    delay_us(1000);                             // stock waits 1 ms after calibration
    vref();
    // HW id strap: IO2 (PB1) mV, mean of 8 samples 1 ms apart (stock: median of 10, 3 ms apart)
    uint32_t mv = 0, k = 0;
    for (int i = 0; i < 8; i++) {
        int r = adc_pin(1, 0);
        if (r >= 0) { mv += to_mv(r); k++; }
        delay_us(1000);
    }
    mv = k ? mv / k : 0xffff;
    hwid = mv >= 2300 && mv <= 2700 ? 3 : mv >= 1800 && mv <= 2200 ? 6 : mv >= 1300 && mv <= 1700 ? 7
         : mv >= 800 && mv <= 1200 ? 8 : mv >= 300 && mv <= 700 ? 9 : mv <= 200 ? 10 : 0;

    apply_gpio();
    GPIOB->OTYPER |= 3u << 3;                   // SCL PB3, SDA PB4: AF6 open drain
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xffu << 12)) | 0x66u << 12;
    GPIOB->MODER = (GPIOB->MODER & ~(0xfu << 6)) | 0xau << 6;
    i2c_init();
    NVIC_ISER = 1u << I2C_IRQ;
    SYST->LOAD = HCLK / 1000 - 1; SYST->VAL = 0; SYST->CTRL = 7;
    __asm volatile("cpsie i");

    uint32_t ms = tick, fed = tick;
    for (;;) {
        if (f_iap) { f_iap = 0; iap(); }
        if (f_gpio) { f_gpio = 0; apply_gpio(); }
        if (f_pwm) { f_pwm = 0; apply_pwm(); }
        uint8_t req = adc_ctrl;
        if (req & 0x80) {
            unsigned ch = (req & 7) - 1;
            int r = adc_pin(ADC_IO[ch], ch);
            if (r >= 0) adc_d = r;
            __asm volatile("cpsid i");
            if (adc_ctrl == req) adc_ctrl = req & 0x7f;   // a new request meanwhile stays pending
            __asm volatile("cpsie i");
        }
        if (temp_ctrl & 0x80) {
            temp();
            __asm volatile("cpsid i");
            if (temp_ctrl == 0xc0) temp_ctrl = 0;
            __asm volatile("cpsie i");
        }
        while (ms != tick) per_ms(++ms);
        // a transaction stuck > 2 s, or a bus error: re-init the slave (stock does the same)
        if (f_i2c_reset || (txn_open && (int32_t)(tick - txn_since) > 2000)) {
            __asm volatile("cpsid i"); f_i2c_reset = 0; i2c_init(); __asm volatile("cpsie i");
        }
        if (tick - fed >= 10) { fed = tick; feed(); }
        if (!(f_iap | f_gpio | f_pwm | ((adc_ctrl | temp_ctrl) & 0x80)) && ms == tick) __asm volatile("wfi");
    }
}

// --- startup
extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
void start(void) {
    uint32_t *s = &_sidata, *d = &_sdata;
    while (d < &_edata) *d++ = *s++;
    for (d = &_sbss; d < &_ebss; ) *d++ = 0;
    main();
}
__attribute__((naked)) void Reset_Handler(void) {
    __asm volatile("ldr r0, =_estack\n mov sp, r0\n bl start\n .ltorg");
}
static void Hang(void) { for (;;) ; }           // the IWDG resets the chip

__attribute__((section(".vectors"), used)) static void (*const vectors[16 + 32])(void) = {
    [0] = (void (*)(void))&_estack, [1] = Reset_Handler,
    [2 ... 14] = Hang, [15] = SysTick_Handler,
    [16 ... 16 + I2C_IRQ - 1] = Hang, [16 + I2C_IRQ] = I2C1_IRQHandler, [16 + I2C_IRQ + 1 ... 47] = Hang,
};
