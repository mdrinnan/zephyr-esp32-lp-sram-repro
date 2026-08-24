# Zephyr + MCUboot: LP-SRAM sections are never loaded on Espressif targets

A reproducer for [zephyrproject-rtos/zephyr#117359](https://github.com/zephyrproject-rtos/zephyr/pull/117359).
Related: [discussion #106900](https://github.com/zephyrproject-rtos/zephyr/discussions/106900), which
reports the same symptom on ESP32-S3.

**The defect.** For MCUboot builds, `soc/espressif/<soc>/default.ld` emits an
`esp_image_load_header_t` (`.metadata`) that the MCUboot Espressif port uses to copy the
application into RAM. It has one descriptor for LP_IRAM and one for LP_DRAM, but there are
**four** loadable LP-SRAM sections. `.rtc.force_fast` and `.rtc.force_slow` are described by
neither, so MCUboot never copies them and they come up as whatever LP-SRAM last contained.
The bytes are in the flashed image; nothing ever reads them.

A directly-booted image is unaffected — the ROM loader uses the esptool segment list, which
is correct. Only the MCUboot path is affected, which is why this presents as "MCUboot costs
current".

**Why it matters.** `.rtc.force_slow` holds `s_sleep_sub_mode_ref_cnt[]` from
`esp_hw_support/sleep_modes.c`. Garbage counts make `esp_deep_sleep_start()` believe several
sub-modes are active, so it keeps the LP peripheral domain powered. On our ESP32-C6 that was
worth about 80 µA of deep-sleep current.

---

## Two ways to reproduce

| | needs | proves |
|---|---|---|
| **A. ELF check** | a toolchain, ~5 min | the load table does not describe the sections |
| **B. On-device** | a board and a console | the data really does not arrive in RAM |

Path A is the one to run first. It needs no hardware, it is deterministic, and it fails on
every affected target.

Reproducing the **current** figures additionally needs a power analyser — see
[Measuring the current](#measuring-the-current). You do not need to, in order to confirm the
defect.

## Prerequisites

A Zephyr workspace with the Espressif HAL and an installed Zephyr SDK. Any recent `main`
works; the defect has been present for a long time.

```sh
west init -m https://github.com/zephyrproject-rtos/zephyr zephyrproject
cd zephyrproject && west update && west blobs fetch hal_espressif
```

Then, from this repository's root, with `ZEPHYR_BASE` pointing at that workspace's `zephyr`.

---

## A. The ELF check (no hardware)

`lp_sram_repro` puts a known pattern into each of the three loadable LP data sections and
also populates the two `NOLOAD` ones. `check_lp_descriptors.py` reads the built ELF's
`.metadata` header and asserts, for every loadable LP section, that

1. it is covered by exactly one descriptor, **and**
2. its offset within that descriptor is the same in VMA as in LMA.

The second assertion is the one that matters if you are reviewing a fix rather than
confirming the bug: a descriptor that merely *spans* the sections is still wrong while the
`NOLOAD` `.rtc.bss` / `.rtc_noinit` sit between the loadable ones, because then the VMA and
LMA spans diverge and the loader writes to the wrong address. A coverage-only check passes
that broken fix.

### Before the fix

```sh
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore -p always \
    -s lp_sram_repro -d build-stock
python3 check_lp_descriptors.py build-stock/lp_sram_repro/zephyr/zephyr.elf
```

```
  LP_IRAM  dst=0x50000000 lma=0x000080 size=0x0000
  LP_DRAM  dst=0x50000010 lma=0x000090 size=0x0010
  .rtc.force_fast    vma=0x50000000 size=0x0010
  .rtc.data          vma=0x50000010 size=0x0010
  .rtc.force_slow    vma=0x50000034 size=0x0010
  FAIL
    - .rtc.force_fast @0x50000000+0x10 covered by 0 descriptors
    - .rtc.force_slow @0x50000034+0x10 covered by 0 descriptors
```

`.rtc.data` **is** covered, exactly and correctly. That is the point: the load table works,
it just describes the wrong sections. Note also that `.rtc.force_slow` sits at `+0x34`, not
`+0x20` — the `NOLOAD` `.rtc.bss` and `.rtc_noinit` are in between.

### After the fix

```sh
curl -L https://github.com/zephyrproject-rtos/zephyr/pull/117359.patch \
  | git -C "$ZEPHYR_BASE" am
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore -p always \
    -s lp_sram_repro -d build-patched
python3 check_lp_descriptors.py build-patched/lp_sram_repro/zephyr/zephyr.elf
```

```
  LP_IRAM  dst=0x50000000 lma=0x000080 size=0x0000
  LP_DRAM  dst=0x50000000 lma=0x000080 size=0x0030
  .rtc.force_fast    vma=0x50000000 size=0x0010
  .rtc.data          vma=0x50000010 size=0x0010
  .rtc.force_slow    vma=0x50000020 size=0x0010
  PASS  (3 LP sections fully covered)
```

`.rtc.force_slow` has moved to `+0x20`, contiguous with `.rtc.data`, and the descriptor now
spans all three.

### Other targets

`lp_sram_repro` uses only section attributes — no SoC-specific linker symbols, no
`esp_sleep` calls — so it builds unchanged everywhere. Swap the board:

```
esp32c3_devkitm    esp32c5_devkitc/esp32c5/hpcore    esp32c6_devkitc/esp32c6/hpcore
esp32h2_devkitm    esp32p4_function_ev_board/esp32p4/hpcore
```

All five fail before the patch and pass after it.

---

## B. On-device

Same application. Flash it and read the console.

```sh
west flash -d build-stock
```

It prints every word of the three loadable LP sections and a verdict:

```
=== LP-SRAM load check ===
boot flag at entry: ........
mode: reporting whatever LP-SRAM contains at boot
.rtc.force_fast  at 0x50000000, expect f0f0f0f0
  [0] .......  <-- NOT LOADED
  ...
.rtc.data        at 0x50000010, expect dadadada
  [0] dadadada ok
  ...
VERDICT: BUG -- 8 words of .rtc.force_fast/.rtc.force_slow were never loaded, while the
.rtc.data control loaded correctly.
```

`.rtc.data` is the built-in control. If it fails too, something else is wrong — nothing is
loading LP-SRAM at all — and the verdict says `INCONCLUSIVE` rather than blaming this
defect.

Expected results:

| build | verdict |
|---|---|
| `--sysbuild` (MCUboot), unpatched | `BUG` |
| `--sysbuild` (MCUboot), patched | `OK` |
| plain `west build` (no MCUboot) | `OK` |

### Making the on-device result deterministic

By default the application reports whatever LP-SRAM holds at boot, which is normally
uninitialised garbage — but not guaranteed to be. **If the previous image left those words
at zero, an unpatched build can report `OK`.** The realistic way to trip over this is to run
the good build first and the bad build second without removing power in between.

Build with the `poison.conf` fragment to remove the doubt. The application then writes
`a5a5a5a5` over the three sections, reboots, and reports on the next boot — so the values it
finds can only have come from the load path:

```sh
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore -p always \
    -s lp_sram_repro -d build-stock-poison \
    -- -Dlp_sram_repro_EXTRA_CONF_FILE=poison.conf
```

Under sysbuild, application Kconfig fragments must carry the image-name prefix
(`lp_sram_repro_`). Without it the fragment is silently ignored and you get the
non-deterministic build back. For a plain build it is just `-DEXTRA_CONF_FILE=poison.conf`.

In poison mode an unpatched build reads back `a5a5a5a5` for the two uncovered sections —
the exact bytes it wrote before rebooting — rather than arbitrary garbage.

Two lines to check in the output:

- `boot flag at entry: 5eeded01` — the handshake worked. That flag lives in `.rtc_noinit`
  and survived the reboot, which is the same retention that poison mode depends on. Any
  other value means the poisoning pass and the reporting pass did not connect.
- If the board **reboot-loops and never prints**, `.rtc_noinit` is not surviving
  `sys_reboot()` on your target, so the two passes can never hand over. Drop `poison.conf`
  and use the default mode, which needs no reboot.

### Console notes

The application waits `CONFIG_LP_REPRO_STARTUP_DELAY_MS` (default 8000) before printing,
because a native USB-CDC console has to re-enumerate after every reset before it can carry
output. On a UART console set it to 0.

### One thing that is *not* a bug

MCUboot loads the LP_DRAM descriptor only when the reset reason is **not** deep-sleep wake,
so that retained data survives a wake as intended. If you modify this application to deep
sleep, expect the sections to keep their pre-sleep values on wake — that is correct
behaviour, not the defect. This is also why the fix puts the retained sections in the
LP_DRAM descriptor and not the unconditionally-loaded LP_IRAM one.

---

## Confirming it from the image alone

No console, no run. The ELF's own program headers say what a direct boot loads; the
`.metadata` table says what MCUboot loads. For the unpatched c6 build above:

```sh
readelf -lW build-stock/lp_sram_repro/zephyr/zephyr.elf | awk '/LOAD/ && /0x5000/'
```

```
  LOAD  0x0002c4 0x50000000 0x00000080 0x00020 0x00030 RW  0x4   <-- .rtc.force_fast + .rtc.data
  LOAD  0x000000 0x50000030 0x000000a0 0x00000 0x00004 RW  0x4   <-- NOLOAD, nothing to copy
  LOAD  0x0002e4 0x50000034 0x000000a0 0x00010 0x00010 RW  0x4   <-- .rtc.force_slow
```

`0x20 + 0x10 = 0x30` bytes of initialised LP data in the image, and the ROM loader copies
all of it — a direct boot prints one `load:0x50000000,len:...` line per segment. The
`.metadata` table MCUboot reads describes `0x10`, the middle one. The bytes are there; the
table does not mention them.

To see which symbols land in the affected region:

```sh
nm -S build-stock/lp_sram_repro/zephyr/zephyr.elf | awk '$1 ~ /^500000/'
```

---

## Measuring the current

Only needed to reproduce the power figures, not the defect. Absolute values depend on the
board, its regulator, the supply voltage and what the application leaves enabled, so treat
the **difference** between arms as the result and the absolutes as ours, not yours.

Our rig: Seeed XIAO ESP32-C6, Nordic PPK2 in source-meter mode into the **BAT** pad at
3700 mV, board USB **unplugged** (USB power swamps a 12 µA floor).

| build | deep-sleep floor |
|---|---|
| release + MCUboot | 92.94 µA |
| release + MCUboot, sub-mode counts re-zeroed by the application | 17.15 µA |
| release, no MCUboot | 12.56 µA |
| **release + MCUboot + the patch** | **11.84 µA** |

Two things that will otherwise cost you time:

- **Average over the sleep gaps; do not take a median, and do not filter for the longest
  contiguous run below some threshold.** A buck regulator in PFM/burst mode at light load
  makes sleep current legitimately spiky — roughly 1 kHz pulses with decaying tails. A
  contiguous-run filter finds nothing and a median under-reads badly.
- **Build with logging, shell and console off.** A console left enabled dominates the floor
  and hides the effect entirely.

An application-side workaround, if you need one before a fix lands — re-zero the counts at
init, before any sleep:

```c
#include <esp_sleep.h>
#include <esp_private/esp_sleep_internal.h>

for (int mode = 0; mode < (int)ESP_SLEEP_MODE_MAX; mode++) {
	(void)esp_sleep_sub_mode_force_disable((esp_sleep_sub_mode_t)mode);
}
```

That recovered most of it (92.94 → 17.15 µA) but not all: it only re-zeroes the
`.rtc.force_slow` half. It also discards any deliberate sub-mode configuration, which is the
thing that section exists to retain.

---

## Things already ruled out

Recorded so nobody repeats them. Each was a single-variable A/B on hardware.

| suspected | result |
|---|---|
| MCUboot's own Kconfig | a minimal bootloader with watchdog, serial, console, log and USB-Serial-JTAG all disabled still measured 94.27 µA |
| MCUmgr / `IMG_MANAGER` / `STREAM_FLASH` | 93.01 µA without it |
| USB-Serial-JTAG | 92.63 µA |
| Application GPIO pull-ups | 92.29 µA |
| Power-analyser wiring | logic-port VCC/GND pair measured 0.27 µA |
| Calling `esp_deep_sleep_start()` instead of `sys_poweroff()`, per #106900 | **does not help on C6**: 91.66 µA against 93.64 µA. The `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)` it avoids is present in the 12 µA arm too |
| Register state before sleep | a 517-register dump immediately before `sys_poweroff()` — PMU, LP_CLKRST, LP_WDT, LP_ANA, LP_AON, PCR, APB_SARADC, PWDET_CONF, LP_I2C_ANA, HP_SYSTEM, PAU, MODEM_SYSCON, MODEM_LPCON — found **zero** systematic differences between arms. The state is consumed inside the sleep call, so a dump taken before it cannot see the divergence |

## Scope

Fixed by the PR: **esp32c3, esp32c5, esp32c6, esp32h2, esp32p4** — the targets whose LP
sections all live in one memory region.

Not fixed there: `esp32s3` (two regions, so `.rtc.force_fast` would land in the
unconditionally-loaded LP_IRAM descriptor and lose deep-sleep retention) and `esp32` /
`esp32s2` (three regions against two descriptors — needs a third descriptor, which fits in
the `_reserved[4]` words of `esp_image_load_header_t` but is a coordinated MCUboot change).
`esp32c2` emits no LP descriptors and is unaffected.

## Verified against

| | |
|---|---|
| Zephyr | `b6a5e6e8aa9` (`main`), with the patch applied on top |
| `hal_espressif` | `3d4d922` |
| MCUboot | `7ad6710` |
| Hardware measurements | Seeed XIAO ESP32-C6 |

Ten sysbuilds — five targets, before and after — with no build errors in either phase:
5/5 `FAIL` before the patch, 5/5 `PASS` after.

## Licence

Apache-2.0, matching Zephyr.
