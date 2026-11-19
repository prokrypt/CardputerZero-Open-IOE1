#!/usr/bin/env python3
"""firmware/cz-io.bin: image format, the contract (tests/test_contract.py) and what it adds (backlight fade, LED PWM, LED pins handed back, SWD pins).
If arm-none-eabi-gcc is installed, also checks that the committed image is what the source builds."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, 'firmware')
IMG = os.path.join(FW, 'cz-io.bin')
sys.path.insert(0, FW)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mkimg  # noqa: E402
import stockemu as E  # noqa: E402

TIM1 = 0x40012C00

# --- image: 20 KB slot, MPEG-2 trailer, vectors the bootloader accepts
d = open(IMG, 'rb').read()
assert len(d) == mkimg.SIZE and mkimg.crc_mpeg(d[:-4]) == struct.unpack('<I', d[-4:])[0], 'size/CRC'
sp, pc = struct.unpack_from('<II', d)
assert sp & 0x2FFE0000 == 0x20000000 and sp <= 0x20000C00 and 0x08001000 < pc < 0x08006000 and pc & 1

if shutil.which('arm-none-eabi-gcc'):
    with tempfile.TemporaryDirectory() as t:
        for f in ('main.c', 'py32.h', 'link.ld', 'mkimg.py', 'Makefile'):
            shutil.copy(os.path.join(FW, f), t)
        subprocess.run(['make', '-s', '-C', t], check=True, stdout=subprocess.DEVNULL)
        assert open(os.path.join(t, 'cz-io.bin'), 'rb').read() == d, 'cz-io.bin is stale: run make in firmware/'

# --- the contract
subprocess.run([sys.executable, os.path.join(ROOT, 'tests/test_contract.py'), IMG], check=True)


def chip():
    c = E.Chip(IMG)
    c.switch(True)
    c.boot()
    c.tick(200)
    c.i2c_write(0xb0, 1)
    c.tick(300)
    return c


def run(c, ms, reg):
    out = []
    for _ in range(ms):
        c.tick(1)
        out.append(c.r32(TIM1 + reg))
    return out


# --- backlight: power-on dark (duty 0), a duty write eases there monotonically; EN off then on fades from black
c = chip()
assert c.r32(TIM1 + 0x40) == c.r32(TIM1 + 0x2c) + 1, 'power-on: PWM4 duty 0'
c.i2c_write(0x21, 0x00, 0x88)
v = run(c, 1000, 0x40)
half = (c.r32(TIM1 + 0x2c) + 1) // 2
assert v[50] > half + 100 and v[-1] == half and all(a >= b for a, b in zip(v, v[1:])), 'fade 0 -> 50%'
c.i2c_write(0x21, 0x00, 0x08); c.tick(2000)
assert c.r32(TIM1) & 1 == 0
c.i2c_write(0x21, 0x00, 0x88)
v = run(c, 20, 0x40)
assert v[0] > half + 400, ('re-enable fades up from black', v[:3])
c.tick(1500)
c.i2c_write(0x25, 0x10, 0x27); c.tick(5)                # 10 kHz: ARR doubles, brightness kept
arr = c.r32(TIM1 + 0x2c)
assert arr == 2399 and c.r32(TIM1 + 0x40) == (arr + 1) // 2

# --- boot: the m5ioe1 probe writes PWM_FREQ 500 and disables every PWM, pwm_bl re-enables at its own level
# right after. The backlight must not go dark in between (it blinked off and faded up).
c = chip()
c.i2c_write(0x21, 0x00, 0x88); c.tick(1000)
mx = c.r32(TIM1 + 0x2c) + 1
c.i2c_write(0x25, 0xf4, 0x01); c.i2c_write(0x22, 0x00); c.i2c_write(0x21, 0x00)
v = run(c, 50, 0x40)
c.i2c_write(0x25, 0x20, 0x4e); c.i2c_write(0x21, 0x00, 0x84)   # 20 kHz, duty 1024 (25%)
v2 = run(c, 1500, 0x40)
duty = lambda ccr, m: 1 - ccr / m
assert min(duty(x, 48000) for x in v) > 0.45, 'held during the probe gap'
assert min(duty(x, mx) for x in v2) > 0.2 and abs(duty(v2[-1], mx) - 0.25) < 0.01, 'fades 50% -> 25%, never dark'
c.i2c_write(0x22, 0x04)
v = run(c, 2000, 0x40)
assert duty(v[50], mx) > 0.2 and c.r32(TIM1) & 1 == 0 and c.pin('A', 7)[0] != 'A', 'EN off for good: hold, fade, stop'

# --- LEDs: level and breathe from RTC RAM 0x8D-0x8F, fast blink ignores breathe, timer off = plain GPIO
c = chip()
mx = c.r32(TIM1 + 0x2c) + 1
c.i2c_write(0x8d, 10); c.i2c_write(0xbd, 3)
v = run(c, 600, 0x38)
assert v[-1] == 11 * 11 * mx >> 10 and c.pin('B', 0)[0] == 'A', 'level 10'
c.i2c_write(0x8d, 0x80 | 10)
v = run(c, 3000, 0x38)
lo = [i for i in range(1, len(v) - 1) if v[i] == 0 and v[i - 1] > 0]
assert len(lo) >= 2 and max(v) == 11 * 11 * mx >> 10, 'breathe: down to 0 and back, ~1 s each way'
c.i2c_write(0xbe, 2); c.i2c_write(0x8e, 0x80)
v = run(c, 400, 0x34)
assert set(v) == {0, mx}, 'fast blink jumps between dark and full'
c.i2c_write(0x8d, 10)                                   # breathe off (it may sit dark in phase 0)
c.i2c_write(0x22, 0x08); c.tick(2000)                   # PWM4 off: TIM1 stops, LEDs become outputs
assert c.pin('B', 0) == ('O', 0), 'mode 3 + timer off: output low = lit'
c.i2c_write(0xbd, 0); c.tick(5)
assert c.pin('B', 0)[0] == 'I', 'mode 0: pin back to the host (input)'

# --- SWD pins (PB6 SWDIO, PA2 SWCLK) stay as reset left them until the host writes any GPIO register
c = E.Chip(IMG)
c.w32(E.GPIOB, 0x2 << 12); c.w32(E.GPIOA, 0x2 << 4)   # AF at reset
c.switch(True); c.boot(); c.tick(300)
assert c.pin('B', 6)[0] == 'A' and c.pin('A', 2)[0] == 'A', 'SWD kept'
c.i2c_write(0x03, 0, 0); c.tick(5)
assert c.pin('B', 6)[0] == 'I' and c.pin('A', 2)[0] == 'I', 'host owns them after its first GPIO write'

# --- 0xFD without a bootloader (blank word at 0x08000000) is ignored, like stock
c = chip()
c.mu.mem_write(0x08000000, b'\xff' * 8)
c.i2c_write(0xfd, 0x1a); c.tick(20)
assert c.i2c_read(0x02)[0] == 0x5a and c.r32(E.SYST) & 3 == 3

# --- the main loop runs between I2C events (stockemu does not): no slave re-init mid-transaction, odd or even tick
c = chip()
resets = []
c.mu.hook_add(E.UC_HOOK_MEM_WRITE, lambda mu, acc, a, size, v, u: resets.append(v), begin=0x4002102c, end=0x4002102f)


def ev(sr1, sr2=0, dr=0):
    c._ev(sr1, sr2, dr)
    c.run_main(count=20_000)


for _ in range(2):
    c.tick(1)
    ev(0x2); ev(0x40, 0, 0xb2); ev(0x2, 0x4); c.i2c_tx = []; ev(0x80); ev(0x80); ev(0x400); ev(0x10)
    assert bytes(c.i2c_tx[:2]) == bytes([0xd0, 0x07]) and not resets, ('I2C re-init mid-transaction', resets)
ev(0x2); c.tick(2100)                                   # a transaction that never ends: re-init after 2 s
assert resets, 'stuck transaction re-inits the slave'

# --- 0xB1 = 0 hands PB7 to the host as it is: a later unrelated GPIO write keeps power on (stock)
c = chip()
c.i2c_write(0xb1, 0); c.tick(5)
c.i2c_write(0x05, 0x01); c.tick(5)
assert c.pin('B', 7) == ('O', 1) and (c.i2c_read(0x03, 2)[1] >> 5) & 1 and (c.i2c_read(0x05, 2)[1] >> 5) & 1

# --- temperature (0x18 = 0x40 starts, 0x19/0x1A = degC): the numbers M5's stock image gives for these sensor voltages
def temp(mv):
    c = E.Chip(IMG)
    c.switch(True); c.boot(); c.tick(200)
    c.adc_mv[8] = mv                                    # ADC ch 8 = internal sensor; cal 30 C 0x5f0, 85 C 0x6f0
    c.i2c_write(0x18, 0x40); c.tick(20)
    v = c.i2c_read(0x19, 2)
    return c.i2c_read(0x18)[0], int.from_bytes(v, 'little', signed=True)


for mv, want in ((1100, -3), (1328, 57), (1500, 103)):
    assert temp(mv) == (0, want), (mv, temp(mv))

print('test_open: ok')
