#include "wav_reader.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "wav";

static uint32_t rd_u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool wav_open(wav_reader_t *w, const char *path)
{
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (!w->f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        goto fail;
    }

    /* Chunk walk: find fmt and data. */
    bool have_fmt = false;
    while (!w->data_offset || !have_fmt) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, w->f) != 8) goto fail;
        uint32_t sz = rd_u32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, w->f) != 16) goto fail;
            uint16_t audio_format = rd_u16(fmt);
            w->channels = rd_u16(fmt + 2);
            w->sample_rate = rd_u32(fmt + 4);
            w->bits = rd_u16(fmt + 14);
            if (audio_format != 1 || w->bits != 16 ||
                w->channels < 1 || w->channels > 8) {
                ESP_LOGE(TAG, "%s: need 16-bit PCM (fmt=%u bits=%u ch=%u)",
                         path, audio_format, w->bits, w->channels);
                goto fail;
            }
            if (sz > 16 && fseek(w->f, (long)(sz - 16), SEEK_CUR) != 0) goto fail;
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_offset = (uint32_t)ftell(w->f);
            w->data_bytes = sz;
            if (fseek(w->f, (long)sz + (sz & 1), SEEK_CUR) != 0) goto fail;
        } else {
            if (fseek(w->f, (long)sz + (sz & 1), SEEK_CUR) != 0) goto fail;
        }
    }

    w->total_frames = w->data_bytes / (2u * w->channels);
    if (w->sample_rate != 44100) {
        ESP_LOGW(TAG, "%s: %lu Hz (v1 targets 44.1k; playing anyway)",
                 path, (unsigned long)w->sample_rate);
    }
    fseek(w->f, (long)w->data_offset, SEEK_SET);
    ESP_LOGI(TAG, "%s: %u ch, %lu frames", path, w->channels,
             (unsigned long)w->total_frames);
    return true;

fail:
    ESP_LOGE(TAG, "bad WAV: %s", path);
    fclose(w->f);
    w->f = NULL;
    return false;
}

void wav_close(wav_reader_t *w)
{
    if (w->f) {
        fclose(w->f);
        w->f = NULL;
    }
}

bool wav_seek_frame(wav_reader_t *w, uint32_t frame)
{
    if (!w->f) return false;
    if (frame > w->total_frames) frame = w->total_frames;
    return fseek(w->f, (long)(w->data_offset + (uint64_t)frame * 2u * w->channels),
                 SEEK_SET) == 0;
}

uint32_t wav_read_stereo(wav_reader_t *w, int16_t *dst, uint32_t n,
                         uint16_t ch0, int16_t *scratch, uint32_t scratch_frames)
{
    if (!w->f) return 0;

    if (w->channels == 2 && ch0 == 0) {
        return (uint32_t)fread(dst, 2u * sizeof(int16_t), n, w->f);
    }
    if (w->channels == 1) {
        /* Read mono into the tail half of dst, then fan out in place.
         * Forward iteration only ever overwrites mono[j] with j <= i,
         * i.e. samples already consumed, so no scratch buffer is needed. */
        int16_t *mono = dst + n; /* caller provides 2n int16s */
        uint32_t got = (uint32_t)fread(mono, sizeof(int16_t), n, w->f);
        for (uint32_t i = 0; i < got; i++) {
            dst[i * 2] = mono[i];
            dst[i * 2 + 1] = mono[i];
        }
        return got;
    }

    /* Multichannel (stems): deinterleave the selected pair via scratch. */
    if (ch0 + 1 >= w->channels || !scratch) return 0;
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done;
        if (chunk > scratch_frames) chunk = scratch_frames;
        uint32_t got = (uint32_t)fread(scratch, 2u * w->channels, chunk, w->f);
        if (got == 0) break;
        for (uint32_t i = 0; i < got; i++) {
            dst[(done + i) * 2]     = scratch[i * w->channels + ch0];
            dst[(done + i) * 2 + 1] = scratch[i * w->channels + ch0 + 1];
        }
        done += got;
        if (got < chunk) break;
    }
    return done;
}
