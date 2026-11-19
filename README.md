# CardputerZero-Open-IOE1

Open-source firmware for the **M5IOE1 I/O expander** (Puya PY32F002B, Cortex-M0+) on the M5Stack
**Cardputer Zero**. It replaces M5's closed app image behind M5's I2C bootloader and keeps the register
contract the Linux `m5ioe1` driver relies on: power switch and power hold, backlight PWM, keyboard
modifier LEDs, GPIO, ADC.

Status: running on a real Cardputer Zero (registers, backlight fade, modifier LEDs, power switch off/on, and
0xFD IAP back to the stock image all verified on the device). Passes the contract test in the emulator.

**Flashing the PY32 can cut your device's power path. Read [Recovery](#recovery-ladder) first. Use at your own risk.**

## What it adds over stock

- Backlight fades on a perceptual curve instead of jumping; EN off then on fades up from black, and the
  boot-time disable/re-enable by the driver no longer blinks the screen.
- Aa/Fn/Sym LEDs on TIM1 PWM with level and breathe settings in RTC RAM 0x8D-0x8F.
- WFI idle between 1 ms ticks (stock spins at 48 MHz).
- SWD pins stay SWD after reset until the host first writes a GPIO register.
- Small, readable C (~510 lines, ~6 KB code), no HAL.

## Layout

| Path | What |
|------|------|
| `firmware/main.c` | the whole app: startup + vectors, 24 MHz clock, IWDG, I2C slave + register file, GPIO ownership, TIM1 PWM4 + fade, LEDs, power state machine, ADC, 0xFD IAP |
| `firmware/py32.h` | the PY32F002B register structs used (from Puya's header, Apache-2.0, see `NOTICE`) |
| `firmware/link.ld`, `mkimg.py`, `Makefile` | link at 0x08001000, pad to 0x5000, CRC-32/MPEG-2 trailer |
| `firmware/cz-io.bin` | prebuilt flashable image (20480 bytes) |
| `tools/stockemu.py` | unicorn whole-chip model: boots an app image, drives pins/ADC, runs I2C transactions through the real ISR |
| `tools/flash.sh` | flash over I2C from the Cardputer Zero and restore the registers the reset loses |
| `tests/test_contract.py` | the register/power/LED contract any replacement image must pass |
| `tests/test_open.py` | image format, the contract, and this firmware's own behaviour; checks `cz-io.bin` matches the source when gcc is installed |

## Build and test

```sh
pip install unicorn
python3 tests/test_open.py                 # ~5 s; also runs tests/test_contract.py
python3 tests/test_contract.py other.bin   # check any app image against the contract

sudo apt install gcc-arm-none-eabi         # optional, to rebuild
make -C firmware                           # -> firmware/cz-io.bin
```

## Flash

On the Cardputer Zero, as root, **with the power switch on**:

```sh
# M5's flasher (MIT), from CardputerZero/FactoryTest; not vendored here
export UPGRADE_IO1=/path/to/upgrade_io1.py
./tools/flash.sh firmware/cz-io.bin
```

`flash.sh` refuses when the switch reads off, saves GPIO/LED/OFF_TIME registers, runs `upgrade_io1.py`
(0xFD on 0x4F -> bootloader at 0x5F -> write -> jump), then restores them and sets READY. To go back to stock,
flash M5's app image (`assets/py32_firmware` in CardputerZero/FactoryTest) the same way.

## Chip and pins

PY32F002B, 24 KB flash (4 KB M5 bootloader at 0x08000000, write-protected; 20 KB app at 0x08001000), 3 KB SRAM.
This firmware runs at HSI 24 MHz (stock: 48 MHz), the bootloader's own clock, so the 0xFD handover needs no clock
change. I2C1 slave on PB3/PB4 (AF6). PC0 = NRST (host GPIO16 on Cardputer Zero). Hardware IWDG (option byte:
always on, ~0.5 s until configured).

**Power path (schematic V0.6.1 p4):** the boost that powers the Pi is enabled by a diode-OR of the switch
(`SW_EN`) and `PYG14_PWR_EN` (PB7), with a 10k pull-down. So with the switch on the Pi is powered whatever the PY32
does; PB7 only holds power after the switch goes off (the OFF_TIME countdown). IO5 sense = `SW_EN` / 2 (~1.8 V).
The PY32 itself is unpowered while the device is off, so every switch-on is a PY32 cold boot through the
bootloader, and RTC RAM / OFF_TIME / threshold never survive power-off.

Option bytes (keep them; this firmware never writes them): IWDG_SW = 0 (hardware watchdog), NRST_MODE = 0 and
SWD_MODE = 0 (PC0 = NRST, SWD on PB6/PA2), BOR off, boot from main flash, WRPR = 0x3E (sector 0 = bootloader
write-protected).

| Reg bit / IO | Pin | Cardputer Zero use | Owner |
|--------------|-----|--------------------|-------|
| 0 / IO1 | PB5 | ext USB | host |
| 1 / IO2 | PB1 | ADC ch0; **HW id strap at boot** | host |
| 2 / IO3 | PA1 | keyboard TCA8418 reset | host |
| 3 / IO4 | PA3 | grove 5V / mic, ADC ch1 | host |
| 4 / IO5 | PA4 | **power switch sense, ADC ch2** | firmware (analog, always) |
| 5 / IO6 | PA5 | - | host |
| 6 / IO7 | PA6 | ADC ch3; **I2C address strap at boot** | host |
| 7 / IO8 | PB0 | Aa LED (active low), TIM1 CH2 | firmware while 0xBD != 0 |
| 8 / IO9 | PA0 | Fn LED, TIM1 CH1 | firmware while 0xBE != 0 |
| 9 / IO10 | PA7 | backlight, TIM1 CH4 AF2 | firmware while PWM4 EN |
| 10 / IO11 | PB2 | Sym LED, TIM1 CH3 | firmware while 0xBF != 0 |
| 11 / IO12 | PB6 | LCD reset net; SWDIO at reset | host |
| 12 / IO13 | PA2 | ext 5V out; SWCLK at reset | host |
| 13 / IO14 | PB7 | **PWR_EN: holds system power** | firmware while 0xB1 = 1 |

Boot straps: I2C address 0x4F (0x50 if IO7 > ~1.65 V). 0xB8 HW id from IO2 mV: 2300-2700 -> 3, 1800-2200 -> 6
(Cardputer Zero), 1300-1700 -> 7, 800-1200 -> 8, 300-700 -> 9, <= 200 -> 10, else 0.

## Register map

SMBus byte access, auto-increment. A one-byte write sets the pointer; the next read returns a snapshot taken at
that moment. Writes are applied at STOP. Read-only writes are ignored.

| Reg | Name | Behaviour |
|-----|------|-----------|
| 0x00/01 | UID | low 16 bits of CRC-32/MPEG-2 over the 3 UID words at 0x1FFF0000, each byte-swapped |
| 0x02 | REV | 0x5A. The `m5ioe1` driver only sets READY if it reads 0x5A |
| 0x03/04 | GPIO_M | 1 = output. Bits the firmware owns read back as the firmware's state |
| 0x05/06 | GPIO_O | output level |
| 0x07/08 | GPIO_I | live input levels, bit n = IOn+1 |
| 0x09-0x0C | PU / PD | pull-up / pull-down masks |
| 0x13/14 | GPIO_DRV | **1 = open drain**, 0 = push-pull |
| 0x15 | ADC_CTRL | write 0x40 + ch (1-4 = IO2/IO4/IO5/IO7). ch 3 (IO5) refused while 0xB1 = 1. bit7 busy |
| 0x16/17 | ADC_D | raw 12-bit counts |
| 0x18 | TEMP_CTRL | 0x40 starts (reads 0 when done). 0x19/1A = degC (int16): (raw - CAL30) * 55 / (CAL85 - CAL30) + 30, CAL30/CAL85 = u16 at 0x1FFF0114/0x1FFF0118 (stock formula; averages 16 samples) |
| 0x21/22 | PWM4 | duty 12 bit; H bit7 EN, bit6 POL. CCR4 = (4096 - duty) * (ARR+1) / 4096. EN clear: holds 0.1 s, fades out, TIM1 stops, PA7 back to the host's GPIO setting |
| 0x23 | IIC_CFG | stored, ignored (stock: sleep that ignores the power switch) |
| 0x25/26 | PWM_FREQ | Hz. 500 at reset, 20000 at power-on. A change keeps the brightness |
| 0x27/28 | REF_VOLT | VDDA mV from VREFINT (read-only) |
| 0x29 | RESET | 0x3A: PY32 system reset |
| 0x70-0x8F | RTC_RAM | plain RAM; 0x8D-0x8F = Aa/Fn/Sym LED level (bits 0-4, 0 = full) + breathe (bit7). Cleared by a PY32 reset |
| 0xA0 | addr offset | 0 = 0x4F, 1 = 0x50 (read-only) |
| 0xB0 | READY | bit0. 0: boot LED walk, host LED modes not shown. The switch-off countdown clears it |
| 0xB1 | PWR_EN lock | 1: firmware runs the power state machine and owns PB7. 0: machine stops, switch ignored, host drives PB7 via GPIO regs. Any change forces state 0: writing 0 mid-countdown cancels it; writing 1 with the switch off cuts power within one poll |
| 0xB2/B3 | OFF_TIME | ms, min 200 (clamped per write: write both bytes in one transaction), default 2000 |
| 0xB4/B5 | switch threshold | mV on IO5 above which the switch counts as on. Default 1000, max 3300 |
| 0xB6 | power state | 0 off, 1 on, 2 off-debounce, 3 countdown, 4 cutting (read-only) |
| 0xB8 | HW id | boot strap above |
| 0xB9 | status | bit0 switch on, bit1 PWR_EN high, bits2-3 LED walk active, bit4 power-on config done |
| 0xBA | minor version | 0x10+ (stock 0x01) |
| 0xBB/BC | switch mV | last IO5 reading |
| 0xBD-0xBF | LED modes | Aa/Fn/Sym: 0 release pin, 1 blink 500/500 ms, 2 fast 150/150 ms, 3 on. Values > 3 ignored |
| 0xFD | IAP | write 0x1A: next main-loop pass, only if the word at 0x08000000 looks like a RAM SP: reset TIM1/I2C, ADC off, EXTICR cleared, HSI 24 MHz, NVIC masked, SysTick off, set MSP, jump to the bootloader. **GPIO is left alone, so PWR_EN stays high while flashing** |

## Power state machine

Every 50 ms while 0xB1 = 1:

- 0 off: switch sense above 0xB4 -> PWR_EN high; first time also PWM4 on and PWM_FREQ = 20000 -> 1. With READY = 0
  the boot walk runs: Aa -> Fn -> Sym, 300 ms each.
- 1 on: below threshold -> 2.
- 2: back above within 100 ms -> 1. Else -> 3.
- 3: READY := 0, shutdown walk Sym -> Fn -> Aa. After OFF_TIME (re-read live, so lowering it ends the wait) -> 4.
  Flipping the switch back on does not cancel.
- 4: PWR_EN low; after 100 ms -> 0. If the switch is on by then, power comes straight back (flip-back = power cycle).

After any PY32 reset (cold start, watchdog, 0x29, reflash) PB7 floats until the first power pass. Harmless with the
switch on (diode-OR). **If the switch is off when the app starts, PB7 is never raised**: a reset or reflash during
the OFF_TIME countdown cuts power at once. Never reflash or reset the PY32 with the switch off.

Other: I2C bus error or a transaction stuck > 2 s -> I2C slave re-init. GPIO writes apply in the main loop within ~1 ms.

## Design

SysTick only counts ms; the I2C ISR moves bytes (32-byte snapshot at a read's ADDR, writes applied at STOP, like
stock 0x8002e84) and sets flags; everything else runs once per ms in the main loop, which sleeps in WFI between.
The 0xFD handover copies stock 0x8001a28 (SP test, I2C/TIM1 RCC reset, ADC off, SysTick off, NVIC masked, EXTICR
cleared, MSP, jump; GPIO untouched), with the clock sequence of stock 0x8001e1c.

## Deviations from stock (intended)

- PWM4 = 0x8000 (on, duty 0) at power-on and the backlight fades to the host's level (stock: 0x8800, 50%, at once).
- Aa/Fn/Sym run as TIM1 CH2/CH1/CH3 PWM with level/breathe in RTC RAM 0x8D-0x8F; an LED in mode 0 hands its pin
  back to the host once faded out.
- PWM4 EN cleared: the light holds 0.1 s, then fades out (stock cuts at once). The `m5ioe1` probe disables every PWM
  and `pwm_bl` re-enables right after; the old cut showed as a blink during boot.
- 24 MHz core (stock 48 MHz): ARR = 1199 at 20 kHz.
- SWD pins PB6/PA2 stay SWD until the host writes any GPIO register.
- 0xBA = 0x10. HW id strap: mean of 8 back-to-back samples (stock: median of 10, 3 ms apart).
- Dropped (unused on Cardputer Zero): IE/IP/IS + PC1 IRQ, IIC_CFG sleep, 0x50 strap, 0xF0, option-byte check
  blinker, the ~113 ms busy loop at reset. Stock lacks PWM1-3, LED_RAM, PULSE too.

## How the stock behaviour was established

Ghidra headless decompile of M5's app image 0x01 (addresses above refer to it), the unicorn model in
`tools/stockemu.py`, and the conformance test pinned on the stock image. M5's generic M5IOE1 manual
(`m5stack/M5IOE1`, `docs/IO_Expander_Datasheet_EN.pdf`) agrees with 0x00-0x90; the Cardputer Zero image is a custom
build (address 0x4F/0x50, REV 0x5A, power registers 0xB0-0xBF). Puya PY32F002B datasheet and reference manual for
pins, option bytes, watchdog and reset states.

Emulator limits: WFI is a no-op, ISRs run as plain calls, busy-wait delays take ~0 ms, I2C flags are set one per ISR
call, peripherals start at 0, no RCC resets, no real STOP.

## Bootloader (inferred, not read)

M5's bootloader at 0x08000000-0x08000FFF (I2C 0x5F) is not published. It speaks the protocol of M5's open STM32
bootloaders (`m5stack/*-Internal-FW`): 0x06 WREN, 0x77 jump, packet `[06, addr32 BE, len16 BE, skipped byte, data]`.
Most likely behaviour:

- Answers at 0x5F for ~0.5 s after any entry (measured after a 0x29 reset: ~0.1 s to ~0.65 s; 0x4F back at
  ~0.75 s). A bare `0x06` extends the stay to 60 s.
- Jumps to the app only if CRC-32/MPEG-2 (init 0xFFFFFFFF, no xor, byte-wise) over the first 0x4FFC bytes equals the
  last 4 bytes and the SP is valid; else stays at 0x5F. So an image must be exactly 0x5000 bytes with that trailer
  (`mkimg.py`).
- PY32 flash pages are 128 B: chunks must be 128 B, 128-aligned.

## Recovery ladder

1. App alive: `flash.sh <stock image>` (0xFD -> 0x5F).
2. App ignores 0xFD, or stuck in the bootloader (switch on keeps the Pi up): pulse NRST (host GPIO16), send `0x06` to
   0x5F within ~0.5 s, then flash as usual; or `upgrade_io1.py` recovery mode when already at 0x5F.
3. Bootloader dead (should not happen: write-protected): SWD under reset. Pads TP7 = SWCLK (PA2), TP8 = SWDIO (PB6),
   TP9 = NRST (schematic p6). pyOCD + Puya pack (`PY32F002Bx5`). The IWDG has no debug freeze: feed it in a halted
   session (Puya's flash algorithm sets PR 256 / RLR 0xFFF).

No ROM UART ISP on this part.

## License

GPL-2.0-or-later (`LICENSE`). `firmware/py32.h` is derived from Puya's `py32f002bx5.h` (Apache-2.0, see `NOTICE`); Apache-2.0 is compatible with GPLv3, hence "or later".
Not affiliated with M5Stack or Puya.
