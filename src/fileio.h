// Copyright (c) 2022 Valtteri Koskivuori (vkoskiv). All rights reserved.

#include <stddef.h>

size_t get_file_size(const char *file_path);
char *load_file(const char *file_path, size_t *bytes);
void human_file_size(unsigned long bytes, char buf[64]);
