/*
 * Copyright (c) 2026 the IOS-26-Hot raphael port authors. MIT License.
 *
 * put: stdin into a file, for copying over ssh to a phone that has neither cat nor an sftp server:
 *   ssh root@PHONE /var/local/bin/put /var/local/bin/btup 755 < btup
 * Writes PATH.new, sets the mode (octal, default 644), renames it over PATH.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: put PATH [OCTAL_MODE] < data\n");
		return 2;
	}
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.new", argv[1]);
	mode_t mode = argc > 2 ? (mode_t)strtoul(argv[2], NULL, 8) : 0644;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) {
		perror(tmp);
		return 1;
	}
	char buf[65536];
	ssize_t n;
	size_t total = 0;
	while ((n = read(0, buf, sizeof(buf))) > 0) {
		if (write(fd, buf, (size_t)n) != n) {
			perror("write");
			return 1;
		}
		total += (size_t)n;
	}
	fchmod(fd, mode);
	close(fd);
	if (rename(tmp, argv[1]) != 0) {
		perror("rename");
		return 1;
	}
	printf("put: %s: %zu bytes, mode %o\n", argv[1], total, (unsigned)mode);
	return 0;
}
