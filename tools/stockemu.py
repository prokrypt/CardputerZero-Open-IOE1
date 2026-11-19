# Peripheral-level unicorn model of the M5IOE1 (PY32F002B) app image, for checking behaviour (stock or our own):
# boots the image, drives pin voltages (ADC) and levels, runs I2C master transactions through the slave ISR
# (vector 39) and 1 ms SysTick ticks with the main loop in between. Peripherals are RAM plus the few read/write
# hooks below (RCC ready flags, SysTick, ADC, I2C, GPIO BSRR/BRR, CRC unit, IWDG feeds).
# Limits (review 2026-11-18): WFI is a no-op, ISRs run as plain calls (PRIMASK ignored), busy-wait delays on
# COUNTFLAG take ~0 ms, I2C flags are set one per ISR call and cleared by the harness, peripherals start at 0
# instead of reset values, RCC resets and EXTI/LPTIM/STOP are not modelled. Time = s.t (ms), owned by the harness.
# Spec and findings: README.md. Conformance test: tests/test_contract.py.
import os
import struct
from unicorn import *
from unicorn.arm_const import *

GPIOA, GPIOB, I2C, ADC, SYST = 0x50000000, 0x50000400, 0x40005400, 0x40012400, 0xE000E010
TRAP = 0x7000

class Chip:
    def __init__(s, img):
        d = open(img, 'rb').read()
        s.vec = struct.unpack_from('<48I', d)
        mu = s.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        mu.mem_map(0x08000000, 0x8000); mu.mem_write(0x08001000, d)
        mu.mem_write(0x08000000, struct.pack('<II', 0x20000c00, 0x08000101))  # fake bootloader vectors
        mu.mem_map(0x1FFF0000, 0x1000); mu.mem_map(0x20000000, 0x1000)
        mu.mem_map(0x40000000, 0x30000); mu.mem_map(0x50000000, 0x1000); mu.mem_map(0xE0000000, 0x100000)
        mu.mem_map(TRAP, 0x1000); mu.mem_write(TRAP, b'\xfe\xe7' * 8)
        # VREFINT/temp calibration words (0x1FFF0114 region): plausible values
        mu.mem_write(0x1FFF0114, struct.pack('<HHHH', 0x5f0, 0, 0x6f0, 0))
        s.pins = {('A', i): 0 for i in range(8)}; s.pins.update({('B', i): 0 for i in range(8)})
        s.adc_mv = {}          # channel -> mV
        s.vdda = 3300
        s.i2c_sr1 = 0; s.i2c_sr2 = 0; s.i2c_dr_in = 0; s.i2c_tx = []
        s.syst_cvr = 0; s.writes = []
        mu.hook_add(UC_HOOK_MEM_READ, s.on_read, begin=0x40000000, end=0x5000ffff)
        mu.hook_add(UC_HOOK_MEM_READ, s.on_read, begin=0xE000E000, end=0xE000EFFF)
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_write, begin=0x40005400, end=0x40005420)
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_gpio, begin=0x50000000, end=0x50000fff)
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_write, begin=0x40022000, end=0x400220ff)
        s.adc_ch = 0
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_write, begin=ADC, end=ADC + 0x400)
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_write, begin=0x40023000, end=0x4002300f)
        s.crc = 0xffffffff
        mu.hook_add(UC_HOOK_MEM_WRITE, s.on_write, begin=0x40003000, end=0x4000300f)
        s.t = 0; s.feeds = []; s.iwdg = [0, 0xfff]   # IWDG PR, RLR (reset values)
        s.opt_ok()

    def w32(s, a, v): s.mu.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def r32(s, a): return struct.unpack('<I', s.mu.mem_read(a, 4))[0]
    def r16(s, a): return struct.unpack('<H', s.mu.mem_read(a, 2))[0]
    def r8(s, a): return s.mu.mem_read(a, 1)[0]

    def opt_ok(s):
        # option bytes that make 0x80041b4 return 1 (bit12=0, field 0x3e, SDK start>end, WRP 0)
        s.w32(0x40022020, 0x0000); s.w32(0x40022024, 0x00ff); s.w32(0x40022028, 0); s.w32(0x4002202c, 0x3e)

    def on_read(s, mu, acc, a, size, v, ud):
        if a == GPIOA + 0x10: s.w32(a, sum(s.pins[('A', i)] << i for i in range(8)))
        elif a == GPIOB + 0x10: s.w32(a, sum(s.pins[('B', i)] << i for i in range(8)))
        elif a == 0x40021000: s.w32(a, s.r32(a) | 0x00020400)   # HSIRDY, HSERDY-ish
        elif a == 0x40021008: v = s.r32(a); s.w32(a, (v & ~0x38) | ((v & 7) << 3))   # SWS = SW
        elif a == 0x40021058 or a == 0x4002105c or a == 0x40021060: s.w32(a, s.r32(a) | 0x2)   # LSIRDY/LSERDY
        elif a == SYST:   # COUNTFLAG every read (1 ms per read)
            s.w32(a, s.r32(a) | 0x10000)
        elif a == SYST + 8:
            s.syst_cvr = (s.syst_cvr - 997) % ((s.r32(SYST + 4) & 0xffffff) + 1); s.w32(a, s.syst_cvr)
        elif a == ADC:    # EOC always
            s.w32(a, 4)
        elif a == ADC + 0x40:
            ch = s.adc_ch
            mv = 1200 if ch == 9 else s.adc_mv.get(ch, 0)
            s.w32(a, min(4095, mv * 4095 // s.vdda))
        elif a == 0x40023000: s.w32(a, s.crc)          # CRC DR
        elif a == I2C + 0x14: s.w32(a, s.i2c_sr1)
        elif a == I2C + 0x18: s.w32(a, s.i2c_sr2)
        elif a == I2C + 0x10: s.w32(a, s.i2c_dr_in)
        elif a == 0x40003000 + 0xc: s.w32(a, 0)   # IWDG SR

    def on_write(s, mu, acc, a, size, v, ud):
        if a == I2C + 0x10: s.i2c_tx.append(v & 0xff)
        elif a == 0x40023000:   # CRC unit: CRC-32/MPEG-2 over 32-bit words, MSB first
            c = s.crc ^ (v & 0xffffffff)
            for _ in range(32): c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
            s.crc = c
        elif a == 0x40023008 and v & 1: s.crc = 0xffffffff
        elif a == ADC + 0x28:
            m = v & 0x7ff
            s.adc_ch = (m & -m).bit_length() - 1 if m else 0
        elif 0x40022000 <= a < 0x40022100: s.writes.append(('FLASH', hex(a), hex(v)))
        elif a == 0x40003000 and v & 0xffff == 0xAAAA: s.feeds.append(s.t)
        elif a == 0x40003004: s.iwdg[0] = v & 7
        elif a == 0x40003008: s.iwdg[1] = v & 0xfff

    def wdg_timeout_ms(s):
        return (4 << s.iwdg[0]) * (s.iwdg[1] + 1) * 1000 // 32768   # LSI 32.768 kHz

    def wdg_max_gap(s):
        t = [0] + s.feeds + [s.t]
        return max(b - a for a, b in zip(t, t[1:]))

    def on_gpio(s, mu, acc, a, size, v, ud):
        base, off = a & ~0x3ff, a & 0x3ff
        if off == 0x18:
            odr = s.r32(base + 0x14); odr = (odr | (v & 0xffff)) & ~((v >> 16) & 0xffff); s.w32(base + 0x14, odr)
        elif off == 0x28:
            s.w32(base + 0x14, s.r32(base + 0x14) & ~(v & 0xffff))

    def call(s, fn, *args, count=3_000_000):
        mu = s.mu
        saved = [mu.reg_read(r) for r in REGS]
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): mu.reg_write(r, v)
        sp = saved[13] - 0x40 if saved[13] > 0x20000100 else 0x20000a60
        mu.reg_write(UC_ARM_REG_SP, sp & ~7); mu.reg_write(UC_ARM_REG_LR, TRAP | 1)
        mu.emu_start(fn | 1, TRAP, count=count)
        ret = mu.reg_read(UC_ARM_REG_R0)
        for r, v in zip(REGS, saved): mu.reg_write(r, v)
        return ret

    # --- main thread: runs between ticks from wherever it stopped (like a real interrupt)
    def boot(s, steps=8_000_000):
        mu = s.mu
        mu.reg_write(UC_ARM_REG_SP, s.vec[0])
        s.main_pc = s.vec[1]
        s.run_main(count=steps)

    def run_main(s, stop=None, count=200_000):
        mu = s.mu
        if stop is not None:
            mu.emu_start(s.main_pc | 1, stop, count=50_000_000)
            s.main_pc = stop
        else:
            mu.emu_start(s.main_pc | 1, 0, count=count)
            s.main_pc = mu.reg_read(UC_ARM_REG_PC)

    def isr(s, n): s.call(s.vec[n] & ~1)

    def tick(s, ms=1, main_steps=20_000):
        for _ in range(ms):
            s.t += 1
            if s.r32(SYST) & 3 == 3:   # ENABLE + TICKINT
                s.isr(15)
            s.run_main(count=main_steps)

    # --- I2C master operations through the slave ISR (vector 39)
    def _ev(s, sr1, sr2=0, dr=0):
        s.i2c_sr1, s.i2c_sr2, s.i2c_dr_in = sr1, sr2, dr
        s.isr(39)
        s.i2c_sr1 = 0

    def i2c_write(s, reg, *data):
        s._ev(0x2)                  # ADDR, write
        for b in (reg,) + data: s._ev(0x40, 0, b)   # RXNE
        s._ev(0x10)                 # STOPF

    def i2c_read(s, reg, n=1):
        s._ev(0x2); s._ev(0x40, 0, reg)
        s.i2c_tx = []
        s._ev(0x2, 0x4)             # repeated start, ADDR + TRA
        for _ in range(n): s._ev(0x80)   # TXE
        s._ev(0x400)                # AF (master NACK)
        s._ev(0x10)
        return bytes(s.i2c_tx[:n])

    def switch(s, on, mv=1799):
        # power switch sense IO5 = PA4 = ADC ch2 only (cz reads ~1799 mV when on)
        s.adc_mv[2] = mv if on else 0

    def pin(s, port, n):
        b = GPIOA if port == 'A' else GPIOB
        return 'IOAN'[(s.r32(b) >> (2 * n)) & 3], (s.r32(b + 0x14) >> n) & 1   # mode (In/Out/Af/aNalog), ODR

    def ms(s): return s.t

    def gpio(s, port):
        b = GPIOA if port == 'A' else GPIOB
        return dict(MODER=s.r32(b), OTYPER=s.r32(b + 4), PUPDR=s.r32(b + 0xc), ODR=s.r32(b + 0x14), AFRL=s.r32(b + 0x20))

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
        UC_ARM_REG_LR, UC_ARM_REG_XPSR]
