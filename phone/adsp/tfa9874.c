/*
 * SPDX-License-Identifier: MIT
 *
 * tfa9874.c: NXP TFA9874 (DSP-less "TFA2" smart amplifier) driver for the
 * Redmi K20 Pro (raphael): mono bottom speaker on Quaternary MI2S SD0.
 *
 * Hardware: I2C 7-bit 0x34 on SOUTH gpio114 (SDA) / gpio115 (SCL), reset on
 * SOUTH gpio59, interrupt on gpio141 (unused). Registers are 8-bit addresses
 * with 16-bit big-endian values.
 *
 * Bring-up follows the order the stock Android stack uses (written from
 * scratch from public facts, no vendor code):
 *   1. pulse the reset pin, write I2CR (reset all I2C registers to POR);
 *   2. unlock key 1 (0x0F=0x5A6B) and key 2 (0xA0 = MTP 0xFB ^ 0x5A);
 *   3. per-revision "optimal" register values (rev 0x0a74 / 0x0b74 / 0x0c74);
 *   4. the device list of the vendor container /var/local/fw/audio/tfa98xx.cnt
 *      (register patches + bitfields up to the first patch/file/profile);
 *   5. the profile list of the chosen profile ("speaker" by default) up to
 *      its default section;
 *   6. MANSCONF=1, PWDN=0, wait for CLKS (needs the I2S clocks running);
 *   7. AMPE=1 (unmute) when audio plays, AMPE=0 + PWDN=1 when idle.
 *
 * Env overrides (for testing without a rebuild):
 *   TFA_PROFILE=name   profile to apply (default "speaker")
 *   TFA_RESET_POL=0|1  level that asserts reset (default 1 = active high)
 *   TFA_REG_OFS=n      byte offset added to register-patch descriptors (default 0)
 *   TFA_NO_CNT=1       skip the container (steps 4-5)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adspd.h"

/* bitfield = reg << 8 | pos << 4 | (len - 1) */
#define BF(reg, pos, len)  ((uint16_t)(((reg) << 8) | ((pos) << 4) | ((len) - 1)))
#define BF_PWDN            BF(0x00, 0, 1)
#define BF_I2CR            BF(0x00, 1, 1)
#define BF_AMPE            BF(0x00, 3, 1)
#define BF_DCA             BF(0x00, 4, 1)
#define BF_MANSCONF        BF(0x01, 2, 1)
#define BF_AUDFS           BF(0x02, 0, 4)
#define BF_CLKS            BF(0x11, 7, 1)
#define BF_PLLS            BF(0x11, 6, 1)
#define BF_AMPS            BF(0x11, 14, 1)
#define BF_MANSTATE        BF(0x14, 3, 4)

#define REG_REV            0x03
#define REG_KEY1           0x0F
#define REG_KEY2           0xA0
#define REG_MTP_FB         0xFB

/* container descriptor types */
#define DSC_DEVICE         0x00
#define DSC_PROFILE        0x01
#define DSC_REGISTER       0x02
#define DSC_STRING         0x03
#define DSC_FILE           0x04
#define DSC_PATCH          0x05
#define DSC_BITFIELD       0x10
#define DSC_DEFAULT        0x11
#define CNT_HDR_SIZE       46u

static bool     tfa_ok;
static bool     tfa_powered;
static bool     tfa_enabled;
static bool     voice_handset;
static uint16_t tfa_rev;
static bool     tfa_logged_unmute;

static uint8_t *cnt;
static size_t   cnt_len;

/* ---- register / bitfield access ---- */

static int
tfa_rd(uint8_t reg, uint16_t *v)
{
	for (int i = 0; i < 3; i++) {
		if (spk_i2c_read8_16(TFA9874_I2C_ADDR, reg, v) == 0) {
			return 0;
		}
		usleep(200);
	}
	adsp_log("tfa9874: i2c read 0x%02x failed", reg);
	return -1;
}

static int
tfa_wr(uint8_t reg, uint16_t v)
{
	for (int i = 0; i < 3; i++) {
		if (spk_i2c_write8_16(TFA9874_I2C_ADDR, reg, v) == 0) {
			return 0;
		}
		usleep(200);
	}
	adsp_log("tfa9874: i2c write 0x%02x=0x%04x failed", reg, v);
	return -1;
}

static uint16_t
bf_mask(uint16_t bf)
{
	unsigned pos = (bf >> 4) & 0xfu, len = (bf & 0xfu) + 1u;

	return (uint16_t)(((1u << len) - 1u) << pos);
}

static int
bf_set(uint16_t bf, uint16_t val)
{
	uint8_t reg = (uint8_t)(bf >> 8);
	unsigned pos = (bf >> 4) & 0xfu;
	uint16_t m = bf_mask(bf), cur;

	if (tfa_rd(reg, &cur) != 0) {
		return -1;
	}
	uint16_t nv = (uint16_t)((cur & ~m) | ((uint16_t)(val << pos) & m));
	if (nv == cur) {
		return 0;
	}
	return tfa_wr(reg, nv);
}

static int
bf_get(uint16_t bf)
{
	uint16_t cur;

	if (tfa_rd((uint8_t)(bf >> 8), &cur) != 0) {
		return -1;
	}
	return (cur & bf_mask(bf)) >> ((bf >> 4) & 0xfu);
}

/* ---- hardware reset ---- */

static int
env_int(const char *name, int def)
{
	const char *e = getenv(name);

	return (e != NULL && *e != '\0') ? (int)strtol(e, NULL, 0) : def;
}

void
spk_amp_hw_reset(void)
{
	int pol = env_int("TFA_RESET_POL", 1) ? 1 : 0;

	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_TFA9874_RESET, pol);
	usleep(2000);
	tlmm_gpio_drive(TLMM_SOUTH_PA, GPIO_TFA9874_RESET, !pol);
	usleep(5000);
}

/* ---- per-revision optimal settings (public register facts) ---- */

struct regval {
	uint8_t  reg;
	uint16_t val;
};

static const struct regval rev_0a74[] = {
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 },
	{ 0x61, 0x0110 }, { 0x66, 0x0701 }, { 0x6f, 0x00a3 }, { 0x70, 0x07f8 },
	{ 0x73, 0x0007 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0594 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
};
static const struct regval rev_0b74[] = {
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 },
	{ 0x61, 0x0110 }, { 0x66, 0x0701 }, { 0x6f, 0x00a3 }, { 0x70, 0x07f8 },
	{ 0x73, 0x0047 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0595 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
};
static const struct regval rev_0c74[] = {
	{ 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 },
	{ 0x61, 0x0110 }, { 0x6f, 0x00a5 }, { 0x70, 0x07f8 }, { 0x73, 0x0047 },
	{ 0x74, 0x5098 }, { 0x75, 0x8d28 }, { 0x80, 0x0000 }, { 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

static void
tfa_write_table(const struct regval *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		(void)tfa_wr(t[i].reg, t[i].val);
	}
}

static void
tfa_unlock_and_optimal(void)
{
	uint16_t fb = 0;

	(void)tfa_wr(REG_KEY1, 0x5a6b);
	(void)tfa_rd(REG_MTP_FB, &fb);
	(void)tfa_wr(REG_KEY2, (uint16_t)(fb ^ 0x005a));
	/* key 2 (MTPKEY2) unlock is part of the stock sequence as well */
	(void)tfa_wr(REG_KEY1, 0x5a6b);
	(void)tfa_wr(0xA1, 0x005a);
	(void)tfa_wr(REG_KEY1, 0x0000);

	switch (tfa_rev) {
	case 0x0a74:
		tfa_write_table(rev_0a74, sizeof(rev_0a74) / sizeof(rev_0a74[0]));
		break;
	case 0x0b74:
		tfa_write_table(rev_0b74, sizeof(rev_0b74) / sizeof(rev_0b74[0]));
		break;
	case 0x0c74:
		tfa_write_table(rev_0c74, sizeof(rev_0c74) / sizeof(rev_0c74[0]));
		break;
	default:
		adsp_log("tfa9874: no optimal table for rev 0x%04x", tfa_rev);
		break;
	}
}

/* ---- container (.cnt) parsing ---- */

static int
cnt_load(void)
{
	FILE *f;
	long n;

	if (cnt != NULL) {
		return 0;
	}
	f = fopen(TFA9874_CNT_PATH, "rb");
	if (f == NULL) {
		adsp_log("tfa9874: %s: %s", TFA9874_CNT_PATH, strerror(errno));
		return -1;
	}
	(void)fseek(f, 0, SEEK_END);
	n = ftell(f);
	(void)fseek(f, 0, SEEK_SET);
	if (n < (long)CNT_HDR_SIZE || n > 1024 * 1024) {
		fclose(f);
		adsp_log("tfa9874: bad container size %ld", n);
		return -1;
	}
	cnt = malloc((size_t)n);
	if (cnt == NULL || fread(cnt, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(cnt);
		cnt = NULL;
		return -1;
	}
	fclose(f);
	cnt_len = (size_t)n;
	adsp_log("tfa9874: container %.2s%.2s%.2s cust=%.8s app=%.8s type=%.8s (%zu B)",
	    (char *)cnt, (char *)cnt + 2, (char *)cnt + 4,
	    (char *)cnt + 16, (char *)cnt + 24, (char *)cnt + 32, cnt_len);
	return 0;
}

static uint16_t
le16(size_t off)
{
	return (off + 2 <= cnt_len) ? (uint16_t)(cnt[off] | (cnt[off + 1] << 8)) : 0;
}

static uint32_t
le32(size_t off)
{
	return (off + 4 <= cnt_len) ?
	    ((uint32_t)cnt[off] | ((uint32_t)cnt[off + 1] << 8) |
	    ((uint32_t)cnt[off + 2] << 16) | ((uint32_t)cnt[off + 3] << 24)) : 0;
}

static const char *
cnt_string(uint32_t desc)
{
	size_t off = desc & 0xffffffu;

	if ((desc >> 24) != DSC_STRING || off >= cnt_len ||
	    memchr(cnt + off, 0, cnt_len - off) == NULL) {
		return "?";
	}
	return (const char *)cnt + off;
}

/*
 * Apply one list of descriptors. Stops at a patch/file/profile (device list)
 * or at the default section (profile list), like the stock driver.
 */
static void
cnt_apply_items(size_t items_off, unsigned count, bool is_profile, const char *what)
{
	int reg_ofs = env_int("TFA_REG_OFS", 0);
	unsigned nreg = 0, nbf = 0;

	for (unsigned i = 0; i < count; i++) {
		uint32_t d = le32(items_off + 4u * i);
		unsigned type = d >> 24;
		size_t off = d & 0xffffffu;

		if (!is_profile && (type == DSC_PATCH || type == DSC_FILE || type == DSC_PROFILE)) {
			break;
		}
		if (is_profile && type == DSC_DEFAULT) {
			break;
		}
		if (type == DSC_BITFIELD) {
			uint16_t val = le16(off), field = le16(off + 2);

			adsp_log("tfa9874: %s bf reg 0x%02x pos %u len %u = 0x%x", what,
			    field >> 8, (field >> 4) & 0xfu, (field & 0xfu) + 1u, val);
			(void)bf_set(field, val);
			nbf++;
		} else if (type == DSC_REGISTER) {
			size_t p = off + (size_t)reg_ofs;
			uint8_t addr = p < cnt_len ? cnt[p] : 0;
			uint16_t val = le16(p + 1), mask = le16(p + 3), cur = 0;

			adsp_log("tfa9874: %s reg 0x%02x = 0x%04x mask 0x%04x", what, addr, val, mask);
			if (mask == 0xffffu) {
				(void)tfa_wr(addr, val);
			} else if (tfa_rd(addr, &cur) == 0) {
				(void)tfa_wr(addr, (uint16_t)((cur & ~mask) | (val & mask)));
			}
			nreg++;
		}
	}
	adsp_log("tfa9874: %s: %u registers, %u bitfields", what, nreg, nbf);
}

static void
cnt_apply(void)
{
	const char *want = getenv("TFA_PROFILE");
	uint32_t dev_desc;
	size_t dev_off, prof_off = 0;
	unsigned ndev, dev_len;
	const char *chosen = NULL;

	if (want == NULL || *want == '\0') {
		want = "speaker";
	}
	if (env_int("TFA_NO_CNT", 0) || cnt_load() != 0) {
		adsp_log("tfa9874: container skipped");
		return;
	}
	ndev = le16(40);
	dev_desc = le32(CNT_HDR_SIZE);
	if (ndev < 1 || (dev_desc >> 24) != DSC_DEVICE) {
		adsp_log("tfa9874: container has no device list (ndev=%u desc=0x%08x)", ndev, dev_desc);
		return;
	}
	dev_off = dev_desc & 0xffffffu;
	dev_len = cnt[dev_off];
	adsp_log("tfa9874: device '%s' bus %u addr 0x%02x, %u items",
	    cnt_string(le32(dev_off + 8)), cnt[dev_off + 1], cnt[dev_off + 2], dev_len);

	cnt_apply_items(dev_off + 12, dev_len, false, "device");

	/* find the profile by name (first profile if none matches) */
	for (unsigned i = 0; i < dev_len; i++) {
		uint32_t d = le32(dev_off + 12 + 4u * i);

		if ((d >> 24) != DSC_PROFILE) {
			continue;
		}
		const char *name = cnt_string(le32((d & 0xffffffu) + 4));
		adsp_log("tfa9874: profile '%s' (id 0x%x)", name, le16((d & 0xffffffu) + 2));
		if (prof_off == 0 ||
		    (strcmp(name, want) == 0 && strcmp(chosen, want) != 0)) {
			prof_off = d & 0xffffffu;
			chosen = name;
		}
	}
	if (prof_off != 0) {
		unsigned plen = cnt[prof_off];

		/* length counts the name too */
		cnt_apply_items(prof_off + 8, plen > 0 ? plen - 1u : 0u, true, chosen);
	}
}

/* ---- power sequence ---- */

static int
tfa_wait_clocks(int tries)
{
	for (int i = 0; i < tries; i++) {
		if (bf_get(BF_CLKS) == 1) {
			return 0;
		}
		usleep(1000);
	}
	return -1;
}

static void
tfa_log_status(const char *tag)
{
	uint16_t r00 = 0, r10 = 0, r11 = 0, r13 = 0, r14 = 0;

	(void)tfa_rd(0x00, &r00);
	(void)tfa_rd(0x10, &r10);
	(void)tfa_rd(0x11, &r11);
	(void)tfa_rd(0x13, &r13);
	(void)tfa_rd(0x14, &r14);
	adsp_log("tfa9874: %s: sys=%04x stat1=%04x stat2=%04x stat3=%04x stat4=%04x "
	    "(CLKS=%u PLLS=%u AMPS=%u MANSTATE=%u)", tag, r00, r10, r11, r13, r14,
	    (r11 >> 7) & 1u, (r11 >> 6) & 1u, (r11 >> 14) & 1u, (r14 >> 3) & 0xfu);
}

static int
tfa_power_up(void)
{
	(void)bf_set(BF_MANSCONF, 1);
	(void)bf_set(BF_PWDN, 0);
	if (tfa_wait_clocks(100) != 0) {
		tfa_log_status("clocks not stable");
		return -1;
	}
	tfa_powered = true;
	return 0;
}

static void
tfa_power_down(void)
{
	(void)bf_set(BF_AMPE, 0);
	usleep(10000);
	(void)bf_set(BF_PWDN, 1);
	tfa_powered = false;
}

int
spk_amp_init(void)
{
	uint16_t rev = 0;

	spk_amp_hw_reset();
	tfa_ok = false;
	tfa_enabled = false;
	tfa_powered = false;
	g_status.amp_left_ok = false;
	g_status.amp_right_ok = false;
	g_status.amp_enabled = false;

	if (tfa_rd(REG_REV, &rev) != 0 || (rev & 0xffu) != 0x74) {
		adsp_log("tfa9874@0x%02x: probe failed (rev=0x%04x)", TFA9874_I2C_ADDR, rev);
		return -1;
	}
	tfa_rev = rev;
	g_status.amp_left_rev = (uint8_t)(rev >> 8);

	/* 1. I2C register reset (I2CR auto-clears) */
	(void)tfa_wr(0x00, (uint16_t)(1u << 1));
	usleep(2000);
	/* 2-3. keys + per-revision optimal values */
	tfa_unlock_and_optimal();
	/* 4-5. vendor container: device list, then the speaker profile */
	cnt_apply();
	tfa_log_status("configured");

	/* 6. leave powerdown (I2S clocks are already running here) */
	if (tfa_power_up() == 0) {
		tfa_log_status("clocks stable");
	}
	tfa_ok = true;
	g_status.amp_left_ok = true;
	adsp_log("tfa9874@0x%02x: initialized (rev 0x%04x, AUDFS=%d, %u-bit I2S)",
	    TFA9874_I2C_ADDR, tfa_rev, bf_get(BF_AUDFS), adsp_i2s_bits());
	return 0;
}

int
spk_amp_set_enable(bool enable)
{
	bool on = enable && !voice_handset;

	if (!tfa_ok) {
		return -1;
	}
	if (on == tfa_enabled) {
		return 0;
	}
	/*
	 * Idle only mutes (AMPE=0) and keeps the device out of powerdown: leaving
	 * PWDN means ~100 ms until CLKS, longer than the DSP queue ahead of the
	 * speaker, so short sounds (keyboard clicks) were lost entirely.
	 */
	if (on) {
		if (!tfa_powered && tfa_power_up() != 0) {
			return -1;
		}
		(void)bf_set(BF_AMPE, 1);
		if (!tfa_logged_unmute) {
			tfa_logged_unmute = true;
			tfa_log_status("unmuted");
		}
	} else {
		(void)bf_set(BF_AMPE, 0);
	}
	tfa_enabled = on;
	g_status.amp_enabled = on;
	adsp_log("tfa9874: power -> %s", on ? "ON" : "OFF");
	adsp_status_write();
	return 0;
}

int
spk_amp_set_voice_mode(bool voice_call, bool speakerphone)
{
	/* The earpiece is on the WCD9340 codec, so a handset call mutes this amp */
	voice_handset = voice_call && !speakerphone;
	if (voice_handset) {
		(void)spk_amp_set_enable(false);
	} else if (voice_call && speakerphone) {
		(void)spk_amp_set_enable(true);
	}
	return 0;
}

/* ---- CLI: adspd tfa dump|r REG|w REG VAL|init|on|off ---- */

int
spk_amp_cli(int argc, char **argv)
{
	const char *op = argc > 0 ? argv[0] : "dump";

	if (strcmp(op, "dump") == 0) {
		for (unsigned r = 0; r <= 0xff; r++) {
			uint16_t v = 0;
			int rc = spk_i2c_read8_16(TFA9874_I2C_ADDR, (uint8_t)r, &v);

			if (r % 8 == 0) {
				printf("%02x:", r);
			}
			if (rc == 0) {
				printf(" %04x", v);
			} else {
				printf(" ----");
			}
			if (r % 8 == 7) {
				printf("\n");
			}
		}
		return 0;
	}
	if (strcmp(op, "r") == 0 && argc > 1) {
		uint16_t v = 0;
		unsigned r = (unsigned)strtoul(argv[1], NULL, 16);

		if (spk_i2c_read8_16(TFA9874_I2C_ADDR, (uint8_t)r, &v) != 0) {
			printf("0x%02x: i2c error\n", r);
			return 1;
		}
		printf("0x%02x = 0x%04x\n", r, v);
		return 0;
	}
	if (strcmp(op, "w") == 0 && argc > 2) {
		unsigned r = (unsigned)strtoul(argv[1], NULL, 16);
		unsigned v = (unsigned)strtoul(argv[2], NULL, 16);
		int rc = spk_i2c_write8_16(TFA9874_I2C_ADDR, (uint8_t)r, (uint16_t)v);

		printf("0x%02x <- 0x%04x: %s\n", r, v, rc == 0 ? "ok" : "i2c error");
		return rc == 0 ? 0 : 1;
	}
	if (strcmp(op, "init") == 0) {
		int rc = spk_amp_init();

		printf("init: %s (see %s)\n", rc == 0 ? "ok" : "failed", ADSP_LOG_PATH);
		return rc == 0 ? 0 : 1;
	}
	if (strcmp(op, "on") == 0 || strcmp(op, "off") == 0) {
		tfa_ok = true;
		tfa_powered = false;
		tfa_enabled = strcmp(op, "on") != 0;
		if (strcmp(op, "off") == 0) {
			tfa_power_down();
			return 0;
		}
		return spk_amp_set_enable(true) == 0 ? 0 : 1;
	}
	if (strcmp(op, "status") == 0) {
		tfa_log_status("cli");
		return 0;
	}
	fprintf(stderr, "usage: adspd tfa [dump | r REG | w REG VAL | init | on | off | status]  (hex)\n");
	return 2;
}
