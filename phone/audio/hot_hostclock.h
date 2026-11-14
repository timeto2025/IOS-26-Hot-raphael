/*
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * CoreAudio's host clock frequency on the 19.2 MHz SM8150 timer.
 *
 * CoreAudio takes the host clock to be 24 MHz (Apple's timer): the audio
 * server's IO clock and AudioGetHostClockFrequency read one double constant
 * in the dyld shared cache. Here mach_absolute_time runs at 19.2 MHz, so the
 * server expects 500 host ticks per frame at 48 kHz where the device gives
 * 400 (re-anchoring every ~0.4 s, rate scalar 0.85, crackle). The constant
 * is on a read-only shared-cache page; this finds its address from
 * AudioGetHostClockFrequency's adrp + ldr d0 and asks the kernel
 * (sysctl kern.sweet.hostclock) to set it, once for every process.
 *
 * After UniXNU-sheng (UniXNU contributors, BSD-2-Clause-Patent):
 * fix(audio): give CoreAudio this board's host clock frequency.
 */
#ifndef _HOT_HOSTCLOCK_H_
#define _HOT_HOSTCLOCK_H_

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define HOT_HOSTCLOCK_SYSCTL "kern.sweet.hostclock"
#define HOT_HOSTCLOCK_COREAUDIO "/System/Library/Frameworks/CoreAudio.framework/CoreAudio"

/* Kernel status: low 32 bits the last result (1 set, 2 already, < 0 refused), high 32 the pages set. -1000 no sysctl. */
static inline int64_t
hot_hostclock_status(void)
{
	int64_t v = 0;
	size_t len = sizeof(v);
	if (sysctlbyname(HOT_HOSTCLOCK_SYSCTL, &v, &len, NULL, 0) != 0) {
		return -1000;
	}
	return v;
}

/*
 * Find the constant and have the kernel set it. load: dlopen CoreAudio if it is not in the process yet.
 * Returns the sysctl's result (0 done) or a negative step: -1 no symbol, -2 not adrp + ldr d, -3 sysctl failed.
 * diag (optional) gets the instruction words, the address and the value read.
 */
static inline int
hot_hostclock_fix(int load, char *diag, size_t diag_len)
{
	void *sym = dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency");
	if (sym == NULL && load) {
		(void)dlopen(HOT_HOSTCLOCK_COREAUDIO, RTLD_LAZY | RTLD_GLOBAL);
		sym = dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency");
	}
	if (diag != NULL && diag_len != 0) {
		diag[0] = 0;
	}
	if (sym == NULL) {
		return -1;
	}
	/* arm64e signs the pointer: keep the 47-bit user address, read the code as data. */
	const uint32_t *code = (const uint32_t *)((uintptr_t)sym & (((uintptr_t)1 << 47) - 1));
	uint64_t address = 0;
	/* The first instructions: adrp xN then ldr dM, [xN, #imm] (scaled by 8). */
	for (int i = 0; i < 6 && address == 0; i++) {
		uint32_t adrp = code[i], ldr = code[i + 1];
		if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u ||
		    ((ldr >> 5) & 31) != (adrp & 31)) {
			continue;
		}
		int64_t page = (int64_t)((((adrp >> 5) & 0x7ffffu) << 2) | ((adrp >> 29) & 3u));
		if (page & (1 << 20)) {
			page -= 1 << 21;
		}
		address = (((uintptr_t)&code[i]) & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
	}
	if (diag != NULL && diag_len != 0) {
		uint64_t bits = 0;
		if (address != 0) {
			memcpy(&bits, (const void *)(uintptr_t)address, sizeof(bits));
		}
		snprintf(diag, diag_len, "code %08x %08x %08x %08x const 0x%llx = %016llx", code[0], code[1], code[2], code[3],
		    (unsigned long long)address, (unsigned long long)bits);
	}
	if (address == 0) {
		return -2;
	}
	int64_t old = 0;
	size_t len = sizeof(old);
	if (sysctlbyname(HOT_HOSTCLOCK_SYSCTL, &old, &len, &address, sizeof(address)) != 0) {
		return -3;
	}
	return 0;
}

#endif /* _HOT_HOSTCLOCK_H_ */
