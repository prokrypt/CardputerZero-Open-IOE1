#!/bin/bash
# Flash an M5IOE1 app image over I2C and restore what the PY32 reset loses (READY, OFF_TIME, GPIO
# levels/pulls/mode/drive, LED modes and RTC RAM extras, backlight). Run on the Cardputer Zero as root,
# power switch ON:   ./flash.sh firmware/cz-io.bin
# Needs i2c-tools and M5's upgrade_io1.py (see README); set UPGRADE_IO1=/path/to/upgrade_io1.py if it is
# not next to this script. BUS (default 1) = I2C bus number.
set -e
img=$(realpath "${1:?usage: flash.sh image.bin}")
bus=${BUS:-1}
up=${UPGRADE_IO1:-$(dirname "$(realpath "$0")")/upgrade_io1.py}
[ -f "$up" ] || { echo "upgrade_io1.py not found ($up): set UPGRADE_IO1" >&2; exit 1; }
get() { i2cget -f -y "$bus" 0x4f "$1"; }
set_() { i2cset -f -y "$bus" 0x4f "$1" "$2"; }
# levels and pulls before mode, so a pin that becomes an output starts at the host's level
regs="0x05 0x06 0x09 0x0a 0x0b 0x0c 0x03 0x04 0x13 0x14 0x8d 0x8e 0x8f 0xb2 0xb3 0xbd 0xbe 0xbf"
declare -A v
# an app reset with the switch off never raises PWR_EN again: power is cut (README, Power). Skipped when
# 0x4F is silent (stuck in the bootloader: recovery must still work)
if sw=$(get 0xb9 2>/dev/null) && [ $((sw & 1)) = 0 ]; then echo "power switch reads off (0xB9=$sw): refusing" >&2; exit 1; fi
if [ "$(get 0xb0 2>/dev/null)" = 0x01 ]; then
    for r in $regs; do v[$r]=$(get "$r"); done
    for r in $regs; do echo "$r ${v[$r]}"; done
else
    echo "READY is 0 (PY32 already reset): nothing to save, only READY and OFF_TIME are restored" >&2
fi
python3 "$up" -b "/dev/i2c-$bus" -f "$img"
for _ in $(seq 20); do get 0xb0 >/dev/null 2>&1 && break; sleep 0.25; done   # app answering again
for r in $regs; do [ -n "${v[$r]}" ] && set_ "$r" "${v[$r]}"; done
[ -n "${v[0xb2]}" ] || { set_ 0xb2 0x60; set_ 0xb3 0xea; }   # OFF_TIME 60 s
set_ 0xb0 1        # READY: ends the boot LED walk (the m5ioe1 driver sets it at probe)
bl=/sys/class/backlight/backlight/brightness
if [ -w "$bl" ]; then   # the driver skips an unchanged value: nudge it so PWM4 is rewritten
    b=$(cat "$bl"); echo $((b == 11 ? 12 : 11)) > "$bl"; echo "$b" > "$bl"
fi
echo "flashed $img"
