/*
 * Reproducer for the Espressif MCUboot LP-SRAM load-table defect.
 *
 * The .metadata load table that soc/espressif/<soc>/default.ld emits for MCUboot builds
 * describes only two of the four loadable LP-SRAM sections. .rtc.force_fast and
 * .rtc.force_slow are present in the flashed image but described by no descriptor, so
 * MCUboot never copies them and they come up as whatever LP-SRAM last contained.
 *
 * This application puts a known pattern in each of the three loadable LP data sections and
 * checks, at run time, whether the pattern arrived. .rtc.data acts as a positive control:
 * the unpatched load table does describe it, so it should always read back correctly, which
 * distinguishes "the load table is broken" from "nothing is being loaded at all".
 *
 * Two independent uses, one build:
 *   1. Inspect the ELF with check_lp_descriptors.py -- no hardware needed.
 *   2. Flash it and read the console verdict.
 *
 * Uses only section attributes: no SoC-specific linker symbols, no esp_sleep calls, so it
 * builds unchanged on every Espressif target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#if defined(CONFIG_LP_REPRO_POISON)
#include <zephyr/sys/reboot.h>
#endif

#define FF_PATTERN 0xf0f0f0f0U
#define RD_PATTERN 0xdadadadaU
#define FS_PATTERN 0x51005100U

#define POISON_WORD     0xa5a5a5a5U
#define BOOT_FLAG_MAGIC 0x5eeded01U

/* The three loadable LP data sections. Every word is initialised, so any word that does not
 * read back as its pattern was never copied out of flash. */
static __attribute__((section(".rtc.force_fast"), used))
volatile uint32_t ff[4] = {FF_PATTERN, FF_PATTERN, FF_PATTERN, FF_PATTERN};

static __attribute__((section(".rtc.data"), used))
volatile uint32_t rd[4] = {RD_PATTERN, RD_PATTERN, RD_PATTERN, RD_PATTERN};

static __attribute__((section(".rtc.force_slow"), used))
volatile uint32_t fs[4] = {FS_PATTERN, FS_PATTERN, FS_PATTERN, FS_PATTERN};

/* NOLOAD sections. Present so the ELF exercises the interleaved-NOLOAD case that the
 * unpatched linker script creates -- a descriptor fix alone is wrong unless these move out
 * from between the loadable sections. Their contents are garbage by design and are never
 * checked. boot_flag survives sys_reboot(), which is what lets the poisoning pass hand over
 * to the reporting pass. */
static __attribute__((section(".rtc.bss"), used)) volatile uint32_t rb[4];
static __attribute__((section(".rtc_noinit"), used)) volatile uint32_t boot_flag;

struct lp_section {
	const char *name;
	volatile uint32_t *data;
	uint32_t expect;
	bool described_by_unpatched_table;
};

static const struct lp_section sections[] = {
	{".rtc.force_fast", ff, FF_PATTERN, false},
	{".rtc.data", rd, RD_PATTERN, true},
	{".rtc.force_slow", fs, FS_PATTERN, false},
};

#define WORDS_PER_SECTION 4U

int main(void)
{
	unsigned int bad_uncovered = 0U, bad_control = 0U;
	uint32_t flag_at_entry = boot_flag;

	/* Keep the NOLOAD sections referenced so --gc-sections does not drop them. */
	rb[0]++;

#if defined(CONFIG_LP_REPRO_POISON)
	if (boot_flag != BOOT_FLAG_MAGIC) {
		for (size_t s = 0U; s < ARRAY_SIZE(sections); s++) {
			for (size_t i = 0U; i < WORDS_PER_SECTION; i++) {
				sections[s].data[i] = POISON_WORD;
			}
		}
		boot_flag = BOOT_FLAG_MAGIC;
		sys_reboot(SYS_REBOOT_COLD);
	}
	boot_flag = 0U;
#endif

	k_msleep(CONFIG_LP_REPRO_STARTUP_DELAY_MS);

	printk("\n=== LP-SRAM load check ===\n");
	/* Also keeps boot_flag referenced, so --gc-sections cannot drop .rtc_noinit.
	 * In poison mode this must read as 5eeded01: it is the evidence that .rtc_noinit
	 * survived the reboot, which is the mechanism poison mode depends on. */
	printk("boot flag at entry: %08x\n", flag_at_entry);
#if defined(CONFIG_LP_REPRO_POISON)
	printk("mode: poisoned with %08x and rebooted, so this is a cold-start load\n",
	       POISON_WORD);
#else
	printk("mode: reporting whatever LP-SRAM contains at boot\n");
#endif

	for (size_t s = 0U; s < ARRAY_SIZE(sections); s++) {
		unsigned int bad = 0U;

		printk("%-16s at %p, expect %08x\n", sections[s].name,
		       (void *)sections[s].data, sections[s].expect);
		for (size_t i = 0U; i < WORDS_PER_SECTION; i++) {
			uint32_t got = sections[s].data[i];

			printk("  [%zu] %08x %s\n", i, got,
			       got == sections[s].expect ? "ok" : "<-- NOT LOADED");
			if (got != sections[s].expect) {
				bad++;
			}
		}
		if (sections[s].described_by_unpatched_table) {
			bad_control += bad;
		} else {
			bad_uncovered += bad;
		}
	}

	printk("\n");
	if (bad_control != 0U) {
		printk("VERDICT: INCONCLUSIVE -- the .rtc.data control failed too (%u words). "
		       "Nothing is loading LP-SRAM; this is not the descriptor defect.\n",
		       bad_control);
	} else if (bad_uncovered != 0U) {
		printk("VERDICT: BUG -- %u words of .rtc.force_fast/.rtc.force_slow were never "
		       "loaded, while the .rtc.data control loaded correctly.\n", bad_uncovered);
	} else {
		printk("VERDICT: OK -- all three loadable LP sections were loaded.\n");
	}

	for (;;) {
		k_msleep(30000);
		printk("(idle -- reset to run again)\n");
	}
	return 0;
}
