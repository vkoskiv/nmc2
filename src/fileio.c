// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

#include "fileio.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>

size_t get_file_size(const char *file_path) {
	FILE *file = fopen(file_path, "r");
	if (!file) return 0;
	fseek(file, 0L, SEEK_END);
	size_t size = ftell(file);
	fclose(file);
	return size;
}

char *load_file(const char *file_path, size_t *bytes) {
	FILE *file = fopen(file_path, "r");
	if (!file) {
		logr("File not found at %s\n", file_path);
		return NULL;
	}
	size_t file_bytes = get_file_size(file_path);
	if (!file_bytes) {
		fclose(file);
		return NULL;
	}
	char *buf = malloc(file_bytes + 1 * sizeof(char));
	size_t read_bytes = fread(buf, sizeof(char), file_bytes, file);
	if (read_bytes != file_bytes) {
		logr("Failed to load file at %s\n", file_path);
		fclose(file);
		return NULL;
	}
	if (ferror(file) != 0) {
		logr("Error reading file %s\n", file_path);
	} else {
		buf[file_bytes] = '\0';
	}
	fclose(file);
	if (bytes) *bytes = read_bytes;
	return buf;
}

void human_file_size(unsigned long bytes, char buf[64]) {
	float kilobytes, megabytes, gigabytes, terabytes, petabytes, exabytes, zettabytes, yottabytes; // <- Futureproofing?!
	kilobytes  = bytes      / 1000.0f;
	megabytes  = kilobytes  / 1000.0f;
	gigabytes  = megabytes  / 1000.0f;
	terabytes  = gigabytes  / 1000.0f;
	petabytes  = terabytes  / 1000.0f;
	exabytes   = petabytes  / 1000.0f;
	zettabytes = exabytes   / 1000.0f;
	yottabytes = zettabytes / 1000.0f;
	
	// Okay, okay. In reality, this never gets even close to a zettabyte,
	// it'll overflow at around 18 exabytes.
	// I *did* get it to go to yottabytes using __uint128_t, but that's
	// not in C99. Maybe in the future.
	
	if (zettabytes >= 1000) {
		sprintf(buf, "%.02fYB", yottabytes);
	} else if (exabytes >= 1000) {
		sprintf(buf, "%.02fZB", zettabytes);
	} else if (petabytes >= 1000) {
		sprintf(buf, "%.02fEB", exabytes);
	} else if (terabytes >= 1000) {
		sprintf(buf, "%.02fPB", petabytes);
	} else if (gigabytes >= 1000) {
		sprintf(buf, "%.02fTB", terabytes);
	} else if (megabytes >= 1000) {
		sprintf(buf, "%.02fGB", gigabytes);
	} else if (kilobytes >= 1000) {
		sprintf(buf, "%.02fMB", megabytes);
	} else if (bytes >= 1000) {
		sprintf(buf, "%.02fkB", kilobytes);
	} else {
		sprintf(buf, "%ldB", bytes);
	}
}

