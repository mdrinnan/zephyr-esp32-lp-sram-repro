# MCUboot never loads `.rtc.force_fast` / `.rtc.force_slow` on Espressif targets

Reproducer for **[zephyrproject-rtos/zephyr#117359](https://github.com/zephyrproject-rtos/zephyr/pull/117359)**
· same symptom on ESP32-S3 in [#106900](https://github.com/zephyrproject-rtos/zephyr/discussions/106900)

`soc/espressif/<soc>/default.ld` emits a `.metadata` load table with **two** LP descriptors
for **four** loadable LP-SRAM sections. `.rtc.force_fast` and `.rtc.force_slow` are described
by neither, so MCUboot never copies them and they come up as whatever LP-SRAM last held.
Direct boot is unaffected — the ROM loader uses the esptool segment list, which is correct.

| | |
|---|---|
| **Affected** | 8 of 9 Espressif SoCs on `main` (all but `esp32c2`). #117359 fixes five — see [Scope](#scope) |
| **Cost** | `s_sleep_sub_mode_ref_cnt[]` lands in `.rtc.force_slow`; garbage counts keep the LP peripheral domain powered through deep sleep — ~80 µA on an ESP32-C6 |
| **To confirm** | one build, one script, **no hardware** |

## Confirm it

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

`.rtc.data` *is* covered, exactly and correctly — the load table works, it just describes the
wrong sections. `.rtc.force_slow` sits at `+0x34` rather than `+0x20` because the `NOLOAD`
`.rtc.bss` and `.rtc_noinit` are in between, which is why a descriptor fix alone is not
enough.

Apply the fix and repeat:

```sh
curl -L https://github.com/zephyrproject-rtos/zephyr/pull/117359.patch \
  | git -C "$ZEPHYR_BASE" am
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore -p always \
    -s lp_sram_repro -d build-patched
python3 check_lp_descriptors.py build-patched/lp_sram_repro/zephyr/zephyr.elf
```

```
  LP_DRAM  dst=0x50000000 lma=0x000080 size=0x0030
  .rtc.force_fast    vma=0x50000000 size=0x0010
  .rtc.data          vma=0x50000010 size=0x0010
  .rtc.force_slow    vma=0x50000020 size=0x0010
  PASS  (3 LP sections fully covered)
```

<details>
<summary>Workspace setup, if you do not already have one</summary>

Needs a Zephyr workspace with the Espressif HAL, an installed SDK, and `readelf` on `PATH`.
Any recent `main` works — the defect is long-standing.

```sh
west init -m https://github.com/zephyrproject-rtos/zephyr zephyrproject
cd zephyrproject && west update && west blobs fetch hal_espressif
export ZEPHYR_BASE=$PWD/zephyr
```

Then run the commands above from a clone of this repository.

</details>

Everything below is detail: [what the check asserts](#what-the-check-asserts-and-why-the-second-assertion-matters),
[other targets](#other-targets), [on-device](#on-device-check),
[current measurements](#measuring-the-current), [scope](#scope).

---

## What the check asserts, and why the second assertion matters

`lp_sram_repro` puts a known pattern into each of the three loadable LP data sections and also
populates the two `NOLOAD` ones. `check_lp_descriptors.py` reads the built ELF's `.metadata`
header and asserts, for every loadable LP section, that

1. it is covered by exactly one descriptor, **and**
2. its offset within that descriptor is the same in VMA as in LMA.

If you are reviewing a fix rather than confirming the bug, the second assertion is the one to
care about. A descriptor that merely *spans* the sections is still wrong while the `NOLOAD`
`.rtc.bss` / `.rtc_noinit` sit between the loadable ones: the VMA and LMA spans then diverge
and the loader writes to the wrong address, clobbering the `NOLOAD` sections. A coverage-only
check passes that broken fix.

`.rtc.data` doubles as a positive control. The unpatched table describes it correctly, so if it
ever fails the problem is something else — nothing is loading LP-SRAM at all — and the
on-device verdict says `INCONCLUSIVE` rather than blaming this defect.

## Other targets

`lp_sram_repro` uses only section attributes — no SoC-specific linker symbols, no `esp_sleep`
calls — so it builds unchanged everywhere. Swap the board:

```
esp32c3_devkitm    esp32c5_devkitc/esp32c5/hpcore    esp32c6_devkitc/esp32c6/hpcore
esp32h2_devkitm    esp32p4_function_ev_board/esp32p4/hpcore
```

All five fail before the patch and pass after it.

Reproducing the **current** figures additionally needs a power analyser — see
[Measuring the current](#measuring-the-current). You do not need to, to confirm the defect.

---

## On-device check

Same application. Flash it and read the console.

```sh
west flash -d build-stock
```

It prints every word of the three loadable LP sections and a verdict. Captured from a XIAO
ESP32-C6, unpatched MCUboot build, built with `poison.conf`:

```
=== LP-SRAM load check ===
boot flag at entry: 5eeded01
mode: poisoned with a5a5a5a5 and rebooted, so this is a cold-start load
.rtc.force_fast  at 0x50000000, expect f0f0f0f0
  [0] a5a5a5a5 <-- NOT LOADED
  [1] a5a5a5a5 <-- NOT LOADED
  [2] a5a5a5a5 <-- NOT LOADED
  [3] a5a5a5a5 <-- NOT LOADED
.rtc.data        at 0x50000010, expect dadadada
  [0] dadadada ok
  [1] dadadada ok
  [2] dadadada ok
  [3] dadadada ok
.rtc.force_slow  at 0x50000034, expect 51005100
  [0] a5a5a5a5 <-- NOT LOADED
  [1] a5a5a5a5 <-- NOT LOADED
  [2] a5a5a5a5 <-- NOT LOADED
  [3] a5a5a5a5 <-- NOT LOADED

VERDICT: BUG -- 8 words of .rtc.force_fast/.rtc.force_slow were never loaded, while the
.rtc.data control loaded correctly.
```

The same build with the patch applied reports every word `ok`, and `.rtc.force_slow` moves to
`0x50000020` — the reorder, visible on the device.

Expected results:

| build | verdict |
|---|---|
| `--sysbuild` (MCUboot), unpatched | `BUG` |
| `--sysbuild` (MCUboot), patched | `OK` |
| plain `west build` (no MCUboot) | `OK` |

### Making the on-device result deterministic

By default the application reports whatever LP-SRAM holds at boot, which is normally
uninitialised garbage — but not guaranteed to be. **If a previous image left the correct
values there, an unpatched build reports `OK`.**

This is not hypothetical. Flashing the no-MCUboot build (which loads all three sections
correctly) and then the unpatched MCUboot build, without removing power in between, produced
a clean `VERDICT: OK` from the *broken* build: MCUboot never copied those sections, but the
right values were already sitting in LP-SRAM from the previous run.

Build with the `poison.conf` fragment to remove the doubt. The application then writes
`a5a5a5a5` over the three sections, reboots, and reports on the next boot — so the values it
finds can only have come from the load path:

```sh
west build --sysbuild -b esp32c6_devkitc/esp32c6/hpcore -p always \
    -s lp_sram_repro -d build-stock-poison \
    -- -Dlp_sram_repro_EXTRA_CONF_FILE=poison.conf
```

The `lp_sram_repro_` prefix names the sysbuild image the fragment applies to. Plain
`-DEXTRA_CONF_FILE=poison.conf` also reaches the application image, but the prefixed form is
unambiguous and is the only way to aim a fragment at a different image
(`mcuboot_EXTRA_CONF_FILE=...`). For a non-sysbuild build it is just
`-DEXTRA_CONF_FILE=poison.conf`.

Either way, confirm it took before trusting the result:
`grep CONFIG_LP_REPRO_POISON build-stock-poison/lp_sram_repro/zephyr/.config`.

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

**If the console stays silent, check the boot mode before suspecting the firmware.** On a
target whose console is the built-in USB-Serial-JTAG, DTR and RTS drive `EN` and the `GPIO9`
boot strap — so merely opening the port can reset the chip into ROM download mode, where it
enumerates normally, opens without error, and says nothing. That is indistinguishable from
"the application printed nothing" unless you look:

```
rst:0x15 (USB_UART_HPSYS),boot:0x77 (DOWNLOAD(USB/UART0/SDIO_REI_REO))
waiting for download
```

A normal boot shows `boot:0x7f (SPI_FAST_FLASH_BOOT)` instead. `esptool --before no-reset
--after no-reset flash-id` is a quick confirmation: it performs no reset, so it succeeds only
if the chip is already parked in ROM. Clear DTR and RTS immediately after opening the port,
and check nothing is holding the BOOT button down.

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
                offset   vaddr      paddr      filesz  memsz
  LOAD  0x0002c4 0x50000000 0x00000080 0x00020 0x00030 RW  0x4   <-- .rtc.force_fast + .rtc.data,
                                                                    memsz also spans .rtc.bss
  LOAD  0x000000 0x50000030 0x000000a0 0x00000 0x00004 RW  0x4   <-- .rtc_noinit, NOLOAD
  LOAD  0x0002e4 0x50000034 0x000000a0 0x00010 0x00010 RW  0x4   <-- .rtc.force_slow
```

Two of the three segments are file-backed, holding `0x20 + 0x10 = 0x30` bytes of initialised
LP data between them, and the ROM loader copies all of it. From a direct boot on hardware:

```
rst:0x15 (USB_UART_HPSYS),boot:0x7f (SPI_FAST_FLASH_BOOT)
...
load:0x50000000,len:0x20
load:0x50000034,len:0x10
...
I (boot): RTC_IRAM : lma=0000a814h vma=50000000h size=00020h (    32)
I (boot): RTC_IRAM : lma=0000a83ch vma=50000034h size=00010h (    16)
```

Those are the same two spans the program headers list, at the same addresses.

The `.metadata` table MCUboot reads describes `0x10` of that: `.rtc.data`, which sits inside
the first segment. `.rtc.force_fast` — the other half of that same segment — and
`.rtc.force_slow` are never mentioned. The bytes are in the image; the table does not point
at them.

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

On hardware, a XIAO ESP32-C6 with `poison.conf`, same board and same application throughout,
differing only by the linker patch:

| build | verdict |
|---|---|
| unpatched + MCUboot | `BUG` — both uncovered sections read back as the poison pattern |
| patched + MCUboot | `OK` — all three sections loaded, `.rtc.force_slow` at `0x50000020` |
| no MCUboot | `OK` |

## Licence

Apache-2.0, matching Zephyr.
