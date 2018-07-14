
#define _POSIX_C_SOURCE 1

#include "machine.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdint.h>
#include <getopt.h>

static void usage(char *argv[]) {
	fprintf(stderr, "Usage: %s image.bin\n", argv[0]);
}

int main(int argc, char *argv[]) {
	int loglevel = 0;
	int opt;
	while ((opt = getopt(argc, argv, "v")) != -1) {
		switch (opt) {
			case 'v':
				loglevel++;
				break;
			default:
				usage(argv);
				return 1;
		}
	}

	if (optind >= argc) {
		usage(argv);
		return 1;
	}
	const char *imagepath = argv[optind];

	FILE *fp = fopen(imagepath, "r");
	if (!fp) {
		perror("could not open file");
		return 1;
	}

	struct stat st;
	if (fstat(fileno(fp), &st)) {
		perror("could not stat file");
		return 1;
	}

	size_t image_size = st.st_size & (~0b11UL);
	uint32_t *image = malloc(image_size);
	if (fread(image, 1, image_size, fp) != image_size) {
		perror("could not read file");
		return 1;
	}
	fclose(fp);

	size_t ram_size = 16 * 1024; // 16kB of RAM
	uint32_t *ram = calloc(ram_size, 1);

	run_emulator(image, image_size, ram, ram_size, loglevel);
	free(image);
	free(ram);
}
