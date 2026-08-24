#pragma once

#include <stdbool.h>
#include <stdint.h>

bool sd_card_mount(const char *mount_point);

/* Sequential-read throughput of `path` (first 4 MB), in KB/s. */
uint32_t sd_card_bench_kbps(const char *path);
