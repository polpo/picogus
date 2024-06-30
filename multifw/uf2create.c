// By Jeroen Taverne

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "uf2.h"
#include "flash_firmware.h"

#define FLASH_SIZE 0x200000
#define PAYLOAD_SIZE 0x100
#define INPUT_FILES (NR_OF_FIRMWARES + 1)

int main(int argc, char *argv[])
{
	int total_size = 0;
	static uint8_t flashbuffer[FLASH_SIZE];
	uint32_t offset[INPUT_FILES] = {0, FLASH_FIRMWARE1, FLASH_FIRMWARE2, FLASH_FIRMWARE3, FLASH_FIRMWARE4, FLASH_FIRMWARE5, FLASH_FIRMWARE6};
	struct uf2_block uf2;

	FILE *f_input, *f_output;

	if (argc != INPUT_FILES+2) {
		fprintf(stderr, "Usage: %s <%d binary input files> <UF2 output file>\n", argv[0], INPUT_FILES);
		return -1;
	}

	memset(flashbuffer, 0xff, FLASH_SIZE);

	for (int i=0; i<INPUT_FILES; i++) {
		int arg = i + 1;
		printf("Opening %s for read\n", argv[arg]);
		f_input = fopen(argv[arg], "rb");
		if (f_input == NULL) {
			fprintf(stderr, "%s: can't open %s for reading\n", argv[0], argv[arg]);
			return -1;
		}
		fseek(f_input, 0L, SEEK_END);
		uint32_t size = ftell(f_input);
		fseek(f_input, 0L, SEEK_SET);
		if ((size + offset[i]) > FLASH_SIZE) {
			fprintf(stderr, "Too big to fit in FLASH!\n");
			return -1;
		}
		for (int j=0; j<size; j++) {
			if (flashbuffer[offset[i] + j] != 0xff) {
				fprintf(stderr, "Files will overlap in FLASH!\n");
				return -1;
			}
		}
		fread(&flashbuffer[offset[i]], size, 1, f_input);
		fclose(f_input);
		if (size > 0) {
			size += offset[i];
			if (size > total_size) {
				total_size = size;
			}
		}
	}

	int arg = INPUT_FILES + 1;
	printf("Opening %s for write\n", argv[arg]);
	f_output = fopen(argv[arg], "wb");

	uf2.magic_start0 = UF2_MAGIC_START0;
	uf2.magic_start1 = UF2_MAGIC_START1;
	uf2.flags = UF2_FLAG_FAMILY_ID_PRESENT;
	uf2.file_size = RP2040_FAMILY_ID;
	uf2.payload_size = PAYLOAD_SIZE;
	uf2.block_no = 0;
	uf2.num_blocks = (total_size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
	memset(uf2.data, 0, sizeof(uf2.data));
	uf2.magic_end = UF2_MAGIC_END;

	int index = 0;
	while (index < total_size) {
		uf2.target_addr = 0x10000000 + index;
		memcpy(uf2.data, &flashbuffer[index], PAYLOAD_SIZE);
		fwrite(&uf2, sizeof(uf2), 1, f_output);
		uf2.block_no++;
		index += PAYLOAD_SIZE;
	}

	fclose(f_output);

        printf("Creating all.bin");
        f_output = fopen("all.bin", "wb");
        fwrite(flashbuffer, total_size, 1, f_output);
        fclose(f_output);

	printf("Done!\n");

	return 0;
}
