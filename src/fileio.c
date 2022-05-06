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
