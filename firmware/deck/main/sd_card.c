/* ESP-DJ3000 deck — microSD mount (SDMMC 1-bit) + P0 throughput bench. */
#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "pins.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;

bool sd_card_mount(const char *mount_point)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "mounted %s (%s, %llu MB)", mount_point, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return true;
}

/* P0 bench (design doc §6.2): sustained sequential read on the actual
 * wiring. One stereo 44.1k/16 stream needs ~176 KB/s; the 8-channel
 * stems interleave needs ~706 KB/s. Logs the verdict for both. */
uint32_t sd_card_bench_kbps(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "bench: cannot open %s", path);
        return 0;
    }
    static char buf[16 * 1024];
    setvbuf(f, NULL, _IONBF, 0);

    int64_t t0 = esp_timer_get_time();
    size_t total = 0;
    while (total < 4 * 1024 * 1024) {
        size_t got = fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
        total += got;
    }
    int64_t dt = esp_timer_get_time() - t0;
    fclose(f);
    if (dt <= 0 || total == 0) return 0;

    uint32_t kbps = (uint32_t)((uint64_t)total * 1000000ull / (uint64_t)dt / 1024ull);
    ESP_LOGI(TAG, "bench: %u KB in %lld ms -> %lu KB/s | 1 stream: %s | stems: %s",
             (unsigned)(total / 1024), dt / 1000, (unsigned long)kbps,
             kbps > 176 * 2 ? "OK" : "MARGINAL",
             kbps > 706 * 2 ? "OK" : "NO-GO");
    return kbps;
}
