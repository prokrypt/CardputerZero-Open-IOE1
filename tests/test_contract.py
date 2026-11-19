#!/usr/bin/env python3
"""The M5IOE1 (PY32) contract a Cardputer Zero app image must keep, checked in the unicorn model
(tools/stockemu.py): register map, power state machine and timings, LED modes and boot walk, GPIO ownership,
PWM4, ADC, straps, 0xFD IAP handover, watchdog feeds. Pinned on M5's stock image and relaxed only where this
firmware deviates on purpose (README "Deviations"). Timings are checked in bands: stock polls the switch every 50 ms.
    python3 tests/test_contract.py [app.bin]      (default firmware/cz-io.bin; or PY32_IMG=app.bin)"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import stockemu as E  # noqa: E402

IMG = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('PY32_IMG', os.path.join(ROOT, 'firmware/cz-io.bin'))
TIM1 = 0x40012C00
LEDS = (('B', 0), ('A', 0), ('B', 2))   # Aa, Fn, Sym (active low)
LED_CCR = (0x38, 0x34, 0x3c)            # TIM1 CH2, CH1, CH3 when an LED runs as PWM (fade)
chips = []


def new():
    c = E.Chip(IMG)
    chips.append(c)
    return c


def chip(on=True, ready=True):
    c = new()
    if on:
        c.switch(True)                   # warm boot: switch already on
    c.boot()
    c.tick(200)
    if ready:
        c.i2c_write(0xb0, 1)
        c.tick(20)
    return c


def b(c, reg, n=1):
    return c.i2c_read(reg, n)


def w16(c, reg):
    v = b(c, reg, 2)
    return v[0] | v[1] << 8


def edges(c, ms, probe, step=10):
    """[(t, value)] whenever probe() changes, sampled every step ms."""
    out, prev, t0 = [], None, c.ms()
    for _ in range(ms // step):
        c.tick(step)
        v = probe()
        if v != prev:
            out.append((c.ms() - t0, v))
            prev = v
    return out


def lit_led(c, i):
    """LED i gives light: GPIO output low (stock), or on TIM1 (AF) with CCR > 0 while the timer runs (fade)."""
    m, o = c.pin(*LEDS[i])
    if m == 'O':
        return o == 0
    return m == 'A' and bool(c.r32(TIM1) & 1) and c.r32(TIM1 + LED_CCR[i]) > 0


def ons(c, ms, step=10):
    """[(t, led)] whenever an LED starts giving light (a fading LED turns off later than stock's, so only
    switch-on edges are compared)."""
    out, prev, t0 = [], set(), c.ms()
    for _ in range(ms // step):
        c.tick(step)
        now = {i for i in range(3) if lit_led(c, i)}
        out += [(c.ms() - t0, i) for i in sorted(now - prev)]
        prev = now
    return out


def near(v, want, lo=0, hi=60):
    return want - lo <= v <= want + hi


# --- registers before the switch is on (cold start, switch off)
c = new()
c.boot()
c.tick(100)
assert b(c, 0x02)[0] == 0x5A, 'REV'
assert w16(c, 0xb2) == 2000 and w16(c, 0xb4) == 1000 and b(c, 0xb1)[0] == 1 and b(c, 0xb6)[0] == 0
assert c.pin('B', 7)[1] == 0, 'PWR_EN low while off'
# --- switch on: PWR_EN within one poll, backlight defaults
c.switch(True)
e = edges(c, 200, lambda: c.pin('B', 7), step=1)
assert e[-1][1] == ('O', 1) and e[-1][0] <= 60, ('PWR_EN after switch-on', e)
assert b(c, 0xb6)[0] == 1
duty = w16(c, 0x21)
assert duty in (0x8800, 0x8000) and w16(c, 0x25) == 20000, 'PWM4 50% (fade: 0) at 20 kHz'
arr = c.r32(TIM1 + 0x2c)
assert abs(c.r32(TIM1 + 0x40) - (4096 - (duty & 0xfff)) * (arr + 1) // 4096) <= 1, 'CCR4 = (4096-duty)*(ARR+1)/4096'
assert c.pin('A', 7)[0] == 'A', 'PA7 on TIM1 CH4'
# threshold: 0xB4/B5 mV on IO5
c.i2c_write(0xb4, 0xe8, 0x03)
c.switch(True, 1001); c.tick(300)
assert b(c, 0xb6)[0] == 1, '1001 mV > 1000 = on'

# --- READY=0: boot walk Aa -> Fn -> Sym, 300 ms each
e = ons(c, 1300)
order = [i for _, i in e]
assert all((order[i + 1] - order[i]) % 3 == 1 for i in range(len(order) - 1)), ('walk order', e)
assert {e[i + 1][0] - e[i][0] for i in range(1, len(e) - 1)} == {300}, ('walk 300 ms', e)
c.i2c_write(0xb0, 1)
c.tick(300)
assert not any(lit_led(c, i) for i in range(3)) and b(c, 0xb9)[0] & 0x0c == 0

# --- LED modes 0xBD-0xBF, single-byte writes like the kbd driver
aa = lambda: lit_led(c, 0)
c.i2c_write(0xbd, 3); c.tick(20)
assert aa(), 'mode 3 = on'
c.i2c_write(0xbd, 1)
e = [t for t, i in ons(c, 3100) if i == 0]
assert {e[i + 1] - e[i] for i in range(1, len(e) - 1)} == {1000} and len(e) >= 3, ('blink 500/500', e)
c.i2c_write(0xbd, 2)
e = edges(c, 1300, aa)
assert {e[i + 1][0] - e[i][0] for i in range(1, len(e) - 1)} <= {150, 140, 160}, ('fast 150/150', e)
c.i2c_write(0xbd, 0); c.tick(400)
assert c.pin('B', 0)[0] == 'I', 'mode 0 releases the pin'
c.i2c_write(0xbd, 4); c.tick(20)
assert b(c, 0xbd)[0] == 0, 'mode > 3 rejected'

# --- switch off: 2 (debounce 100 ms) -> 3 (READY cleared, walk, OFF_TIME) -> 4 (PWR_EN low, 100 ms) -> 0
for off_time, want in ((500, 500), (100, 200)):
    c = chip()
    c.i2c_write(0xb2, off_time & 0xff)
    c.i2c_write(0xb3, off_time >> 8)
    c.switch(False)
    e = [x for x in edges(c, 1500, lambda: (b(c, 0xb6)[0], c.pin('B', 7)[1])) if x[1] != (1, 1)]
    t = {v[0]: tt for tt, v in e}
    assert [v for _, v in e] == [(2, 1), (3, 1), (4, 0), (0, 0)], e
    assert t[2] <= 60 and near(t[3] - t[2], 100) and near(t[4] - t[3], want) and near(t[0] - t[4], 100), (off_time, e)
    assert b(c, 0xb0)[0] == 0, 'READY cleared by the countdown'

c = chip()                                       # lowering OFF_TIME mid-countdown ends it (shutdown hook relies on it)
c.switch(False); c.tick(400)
assert b(c, 0xb6)[0] == 3
c.i2c_write(0xb2, 0xc8, 0x00)
e = edges(c, 300, lambda: c.pin('B', 7)[1])
assert e[-1][1] == 0 and e[-1][0] <= 60, e

c = chip()                                       # flip back during the debounce: stays on
c.switch(False); c.tick(30); c.switch(True); c.tick(300)
assert b(c, 0xb6)[0] == 1 and c.pin('B', 7) == ('O', 1)

c = chip()                                       # flip back during the countdown: cut anyway, then power on again
c.i2c_write(0xb2, 0xe8, 0x03)
c.switch(False); c.tick(300); c.switch(True)
e = edges(c, 1500, lambda: c.pin('B', 7)[1])
assert [v for _, v in e] == [1, 0, 1], e

# --- 0xB1: 0 hands PWR_EN to the host and stops the machine; 1 again re-locks with no debounce/OFF_TIME
c = chip()
c.i2c_write(0xb1, 0); c.tick(60)
assert b(c, 0xb6)[0] == 0
c.switch(False); c.tick(500)
assert c.pin('B', 7) == ('O', 1), 'switch ignored with 0xB1=0'
c.i2c_write(0xb1, 1)
e = edges(c, 200, lambda: c.pin('B', 7)[1], step=5)
assert e[-1][1] == 0 and e[-1][0] <= 60, ('re-lock with switch off cuts at once', e)
c = chip()                                       # 0xB1=0 during the countdown cancels it
c.switch(False); c.tick(300)
c.i2c_write(0xb1, 0); c.tick(3000)
assert c.pin('B', 7) == ('O', 1) and b(c, 0xb6)[0] == 0

# --- GPIO registers: bit n = IO(n+1); IO5 (power sense) and IO14 (PWR_EN) are not the host's
c = chip()
PINS = [('B', 5), ('B', 1), ('A', 1), ('A', 3), ('A', 4), ('A', 5), ('A', 6), ('B', 0), ('A', 0), ('A', 7), ('B', 2), ('B', 6), ('A', 2), ('B', 7)]
for i, pn in enumerate(PINS):
    c.pins = {k: 0 for k in c.pins}; c.pins[pn] = 1
    assert w16(c, 0x07) == 1 << i, (i, pn)
c.i2c_write(0x03, 0xff); c.i2c_write(0x04, 0x3f); c.i2c_write(0x05, 0); c.i2c_write(0x06, 0); c.tick(20)
assert w16(c, 0x03) & 0x2010 == 0x2000 and c.pin('A', 4)[0] == 'N' and c.pin('B', 7) == ('O', 1)
assert c.pin('B', 5) == ('O', 0)
c.i2c_write(0x13, 0xff, 0x3f); c.tick(20)
assert (c.r32(E.GPIOB + 4) >> 5) & 1, 'DRV bit 1 = open drain'

# --- PWM4 off returns PA7 to the host's GPIO setting (ours holds 0.1 s and fades out first)
c.i2c_write(0x22, 0x00); c.tick(2000)
assert c.r32(TIM1) & 1 == 0 and c.pin('A', 7)[0] != 'A'
# --- RTC RAM, ADC, read-only regs, threshold clamp
c.i2c_write(0x70, *range(1, 33))
assert b(c, 0x70, 32) == bytes(range(1, 33))
c.adc_mv[0] = 1000                               # IO2 = PB1 = ADC ch0, ADC_CTRL channel 1
c.i2c_write(0x15, 0x41); c.tick(5)
assert abs(w16(c, 0x16) - 1000 * 4095 // 3300) <= 2
c.i2c_write(0x02, 0x00); c.i2c_write(0xb6, 9)
assert b(c, 0x02)[0] == 0x5A and b(c, 0xb6)[0] == 1
c.i2c_write(0xb4, 0xff, 0xff)
assert w16(c, 0xb4) == 3300

# --- straps: address 0x4F with IO7 low; HW id from IO2 (PB1 1.8-2.2 V -> 6, cz)
c = new(); c.adc_mv[0] = 2000; c.adc_mv[3] = 0; c.boot()
assert (c.r32(0x40005408) >> 1) & 0x7f == 0x4f and b(c, 0xb8)[0] == 6
# --- 0xFD=0x1A jumps to the bootloader with GPIO untouched and the clock back on HSI 24 MHz; 0x29=0x3A resets
c = chip()
hit = []
c.mu.hook_add(E.UC_HOOK_CODE, lambda mu, a, s, u: (hit.append(a), mu.emu_stop()), begin=0x08000100, end=0x08000102)
c.i2c_write(0xfd, 0x1a)
c.tick(2)
assert hit and c.pin('B', 7) == ('O', 1), 'bootloader entered, PWR_EN still high'
assert c.r32(0x40005400) & 1 == 0 and c.r32(E.SYST) & 3 == 0 and c.r32(0x40021008) & 7 == 0, 'I2C off, SysTick off, SW = HSI'
c = chip()
c.i2c_write(0x29, 0x3a)
assert c.r32(0xE000ED0C) == 0x05FA0004

# --- hardware watchdog (option bytes: always on, ~0.5 s from reset): every run must keep feeding it
for c in chips:
    assert c.feeds, 'image never feeds the watchdog'
    assert c.wdg_max_gap() < c.wdg_timeout_ms(), ('watchdog gap', c.wdg_max_gap(), c.wdg_timeout_ms())

print('test_contract: ok')
