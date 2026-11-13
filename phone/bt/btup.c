/*
 * Copyright (c) 2026 the IOS-26-Hot raphael port authors. MIT License.
 *
 * btup: bring the K20 Pro's WCN3990 Bluetooth controller up over /dev/raphael-bt (bsd/dev/raphael_bt.c).
 *
 *   btup [-d DEV] [-f FWDIR] [-b BAUD] [-n NN]   (defaults /dev/raphael-bt, /var/local/fw/bt, 3000000;
 *                                               NN: firmware number in hex, default from the ROM version)
 *
 * The steps of Qualcomm's WCN399x bring-up (facts from Linux btqca.h / hci_qca.c, no code taken):
 *   1. power pulses at 2400 baud: 0xC0 (off), then 0xFC (on); 100 ms later the controller listens at 115200
 *   2. HCI 0xFC00 EDL_PATCH_VER_REQ (0x19): product, patch, ROM and SoC versions
 *   3. HCI 0xFC48: the operating rate (enum index, 3000000 = 14), host follows 100 ms later, CTS/RTS on
 *   4. rampatch crbtfwNN.tlv, then NVM crnvNN.bin (NN from the ROM version), in 243-byte EDL_PATCH_TLV_REQ
 *      (0x1E) segments, each answered by a vendor event
 *   5. HCI_Reset, Read_Local_Version, Read_BD_ADDR
 * The firmware files are the phone's own (its bluetooth partition / vendor image), never in git.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define RBT_SET_BAUD    _IOW('B', 1, uint32_t)
#define RBT_SET_FLOW    _IOW('B', 2, uint32_t)
#define RBT_FLUSH       _IO('B', 3)

static int fd = -1;

static uint64_t
now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static int
set_baud(uint32_t baud)
{
	if (ioctl(fd, RBT_SET_BAUD, &baud) < 0) {
		printf("btup: host rate %u: %s\n", baud, strerror(errno));
		return -1;
	}
	return 0;
}

static int
read_exact(uint8_t *p, size_t n, int timeout_ms)
{
	uint64_t end = now_ms() + (uint64_t)timeout_ms;
	size_t got = 0;
	while (got < n) {
		int64_t left = (int64_t)(end - now_ms());
		if (left <= 0) {
			return -1;
		}
		fd_set r;
		FD_ZERO(&r);
		FD_SET(fd, &r);
		struct timeval tv = { (time_t)(left / 1000), (suseconds_t)((left % 1000) * 1000) };
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) {
			continue;
		}
		ssize_t k = read(fd, p + got, n - got);
		if (k > 0) {
			got += (size_t)k;
		}
	}
	return 0;
}

/* One HCI event (04 code len params); other packet types are skipped. Returns the parameter length. */
static int
read_event(uint8_t *code, uint8_t *params, int timeout_ms)
{
	uint8_t t;
	for (;;) {
		if (read_exact(&t, 1, timeout_ms)) {
			return -1;
		}
		if (t == 0x04) {
			uint8_t h[2];
			if (read_exact(h, 2, timeout_ms) || read_exact(params, h[1], timeout_ms)) {
				return -1;
			}
			*code = h[0];
			return h[1];
		}
		if (t == 0x02) {        /* ACL: skip */
			uint8_t h[4], skip[1024];
			if (read_exact(h, 4, timeout_ms)) {
				return -1;
			}
			uint16_t len = (uint16_t)(h[2] | (h[3] << 8));
			if (len > sizeof(skip) || read_exact(skip, len, timeout_ms)) {
				return -1;
			}
			continue;
		}
		/* in-band sleep bytes (0xfd/0xfe) or noise between packets */
	}
}

static int
send_cmd(uint16_t opcode, const uint8_t *p, uint8_t len)
{
	uint8_t b[260];
	b[0] = 0x01;
	b[1] = (uint8_t)opcode;
	b[2] = (uint8_t)(opcode >> 8);
	b[3] = len;
	if (len) {
		memcpy(b + 4, p, len);
	}
	return write(fd, b, (size_t)len + 4) == (ssize_t)len + 4 ? 0 : -1;
}

/* A command and its Command Complete (0x0e) or Status (0x0f) or vendor (0xff) event. */
static int
cmd(uint16_t opcode, const uint8_t *p, uint8_t len, uint8_t *out, int *out_len, int timeout_ms)
{
	if (send_cmd(opcode, p, len)) {
		return -1;
	}
	uint8_t code, ev[256];
	for (;;) {
		int n = read_event(&code, ev, timeout_ms);
		if (n < 0) {
			printf("btup: no answer to %#06x\n", opcode);
			return -1;
		}
		if (code == 0x0e && n >= 3 && (ev[1] | (ev[2] << 8)) == opcode) {
			if (out) {
				memcpy(out, ev, (size_t)n);
				*out_len = n;
			}
			return ev[3 <= n - 1 ? 3 : 0];
		}
		if (code == 0xff || code == 0x0f) {
			if (out) {
				memcpy(out, ev, (size_t)n);
				*out_len = n;
			}
			return 0;
		}
	}
}

static uint8_t *
load(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	*len = d ? (size_t)n : 0;
	return d;
}

static int
download(const char *path)
{
	size_t len;
	uint8_t *d = load(path, &len);
	if (!d) {
		printf("btup: %s: %s\n", path, strerror(errno));
		return -1;
	}
	printf("btup: %s: %zu bytes\n", path, len);
	size_t off = 0;
	unsigned seg = 0;
	while (off < len) {
		uint8_t p[2 + 243];
		size_t n = len - off > 243 ? 243 : len - off;
		p[0] = 0x1e;
		p[1] = (uint8_t)n;
		memcpy(p + 2, d + off, n);
		uint8_t ev[256];
		int evlen = 0;
		if (cmd(0xfc00, p, (uint8_t)(n + 2), ev, &evlen, 3000) < 0) {
			printf("btup: segment %u at %zu: no answer\n", seg, off);
			free(d);
			return -1;
		}
		off += n;
		seg++;
	}
	printf("btup: %s: %u segments sent\n", path, seg);
	free(d);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *dev = "/dev/raphael-bt", *fwdir = "/var/local/fw/bt";
	uint32_t baud = 3000000;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-d")) {
			dev = argv[i + 1];
		} else if (!strcmp(argv[i], "-f")) {
			fwdir = argv[i + 1];
		} else if (!strcmp(argv[i], "-b")) {
			baud = (uint32_t)strtoul(argv[i + 1], NULL, 0);
		}
	}
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		printf("btup: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	uint32_t off = 0;
	ioctl(fd, RBT_SET_FLOW, &off);

	/* 1. power pulses */
	if (set_baud(2400)) {
		return 1;
	}
	uint8_t pulse = 0xc0;
	write(fd, &pulse, 1);
	usleep(100000);
	pulse = 0xfc;
	write(fd, &pulse, 1);
	usleep(100000);
	if (set_baud(115200)) {
		return 1;
	}
	ioctl(fd, RBT_FLUSH);

	/* 2. versions */
	/* the answer: Command Complete (0x0e) or a vendor event (0xff) with EDL 0x00 0x19 and the 12-byte record;
	 * other events (the controller's own hellos) are printed and skipped */
	uint8_t p = 0x19, ev[256], code = 0;
	int evlen = 0, at = -1;
	if (send_cmd(0xfc00, &p, 1)) {
		return 2;
	}
	for (int tries = 0; tries < 6 && at < 0; tries++) {
		evlen = read_event(&code, ev, 2000);
		if (evlen < 0) {
			break;
		}
		printf("btup: event %#04x, %d bytes:", code, evlen);
		for (int i = 0; i < evlen; i++) {
			printf(" %02x", ev[i]);
		}
		printf("\n");
		/* raphael's WCN3998 answers with vendor event 0xff: 00 02 <product 4> <patch 2> <rom 2> <soc 4> */
		if (code == 0xff && evlen >= 14 && ev[0] == 0x00) {
			at = 2;
			break;
		}
		for (int i = 0; i + 1 + 12 <= evlen; i++) {
			if (ev[i] == 0x19 && (i == 0 || ev[i - 1] == 0x00)) {
				at = i + 1;
				break;
			}
		}
	}
	if (at < 0) {
		printf("btup: no version record in the answers%s\n", evlen < 0 ? " (nothing more within 2 s)" : "");
		return 2;
	}
	uint32_t product = ev[at] | ev[at + 1] << 8 | ev[at + 2] << 16 | (uint32_t)ev[at + 3] << 24;
	uint16_t patch = (uint16_t)(ev[at + 4] | ev[at + 5] << 8), rom = (uint16_t)(ev[at + 6] | ev[at + 7] << 8);
	uint32_t soc = ev[at + 8] | ev[at + 9] << 8 | ev[at + 10] << 16 | (uint32_t)ev[at + 11] << 24;
	/* the files are named by ROM version (Linux: crbtfwNN.tlv, NN from the ROM version's nibbles); if this
	 * phone's partition has no such pair, the newest pair it has (raphael: 21, for the WCN3990/3998 v2.1) */
	unsigned nn = ((rom & 0x0f00u) >> 4) | (rom & 0x000fu);
	static const unsigned fallback[] = { 0x21, 0x20, 0x11 };
	char probe[512];
	snprintf(probe, sizeof(probe), "%s/crbtfw%02x.tlv", fwdir, nn);
	for (unsigned i = 0; access(probe, R_OK) != 0 && i < sizeof(fallback) / sizeof(fallback[0]); i++) {
		nn = fallback[i];
		snprintf(probe, sizeof(probe), "%s/crbtfw%02x.tlv", fwdir, nn);
	}
	if (argc > 2) {
		for (int i = 1; i + 1 < argc; i += 2) {
			if (!strcmp(argv[i], "-n")) {
				nn = (unsigned)strtoul(argv[i + 1], NULL, 16);
			}
		}
	}
	printf("btup: product %#x patch %#x rom %#x soc %#x: firmware crbtfw%02x.tlv / crnv%02x.bin\n", product, patch,
	    rom, soc, nn, nn);

	/* 3. operating rate */
	static const struct { uint32_t baud; uint8_t idx; } rates[] = {
		{ 115200, 0 }, { 921600, 10 }, { 1000000, 11 }, { 2000000, 13 }, { 3000000, 14 }, { 3200000, 17 },
	};
	int ri = -1;
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i].baud == baud) {
			ri = (int)i;
		}
	}
	if (ri > 0) {
		send_cmd(0xfc48, &rates[ri].idx, 1);
		usleep(100000);
		if (set_baud(baud)) {
			printf("btup: staying at 115200\n");
			uint8_t z = 0;
			send_cmd(0xfc48, &z, 1);
			usleep(100000);
		}
		ioctl(fd, RBT_FLUSH);
	}
	uint32_t on = 1;
	ioctl(fd, RBT_SET_FLOW, &on);

	/* 4. firmware */
	char path[512];
	snprintf(path, sizeof(path), "%s/crbtfw%02x.tlv", fwdir, nn);
	if (download(path)) {
		return 3;
	}
	snprintf(path, sizeof(path), "%s/crnv%02x.bin", fwdir, nn);
	if (download(path)) {
		return 3;
	}

	/* 5. the controller as Bluetooth sees it */
	int r = cmd(0x0c03, NULL, 0, ev, &evlen, 3000);
	printf("btup: HCI_Reset: %d\n", r);
	if (cmd(0x1001, NULL, 0, ev, &evlen, 2000) >= 0 && evlen >= 12) {
		printf("btup: HCI version %u rev %#x, LMP %u subversion %#x, manufacturer %u\n", ev[4],
		    ev[5] | ev[6] << 8, ev[7], ev[10] | ev[11] << 8, ev[8] | ev[9] << 8);
	}
	if (cmd(0x1009, NULL, 0, ev, &evlen, 2000) >= 0 && evlen >= 10) {
		printf("btup: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n", ev[9], ev[8], ev[7], ev[6], ev[5], ev[4]);
	}
	printf("btup: done; the controller runs at %u baud with CTS/RTS on %s\n", ri > 0 ? baud : 115200, dev);
	return 0;
}
