#include "track_meta.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "track_meta";

static void meta_defaults(track_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->bpm = 120.0f;
    m->first_beat_s = 0.0;
    m->beats_per_bar = 4;
    for (int i = 0; i < TRACK_MAX_CUES; i++) {
        m->cues[i] = -1.0;
    }
    strncpy(m->title, "Unknown", sizeof(m->title) - 1);
}

static void sidecar_path(const char *wav_path, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", wav_path);
    char *dot = strrchr(out, '.');
    size_t used = dot ? (size_t)(dot - out) : strlen(out);
    snprintf(out + used, out_len - used, ".json");
}

static char *read_whole_file(const char *path, long max_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > max_len) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

static void copy_str(const cJSON *obj, const char *key, char *dst, size_t n)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(dst, it->valuestring, n - 1);
        dst[n - 1] = '\0';
    }
}

bool track_meta_load(const char *wav_path, track_meta_t *out)
{
    meta_defaults(out);

    char jpath[224];
    sidecar_path(wav_path, jpath, sizeof(jpath));
    char *text = read_whole_file(jpath, 64 * 1024);
    if (!text) {
        ESP_LOGW(TAG, "no sidecar %s, playing with defaults", jpath);
        return false;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON in %s", jpath);
        return false;
    }

    copy_str(root, "title", out->title, sizeof(out->title));
    copy_str(root, "artist", out->artist, sizeof(out->artist));
    copy_str(root, "key", out->key, sizeof(out->key));

    const cJSON *it;
    if (cJSON_IsNumber((it = cJSON_GetObjectItemCaseSensitive(root, "bpm"))) &&
        it->valuedouble > 40.0 && it->valuedouble < 250.0) {
        out->bpm = (float)it->valuedouble;
    }
    if (cJSON_IsNumber((it = cJSON_GetObjectItemCaseSensitive(root, "first_beat_s")))) {
        out->first_beat_s = it->valuedouble;
    }
    if (cJSON_IsNumber((it = cJSON_GetObjectItemCaseSensitive(root, "beats_per_bar"))) &&
        it->valueint >= 1 && it->valueint <= 16) {
        out->beats_per_bar = it->valueint;
    }
    if (cJSON_IsBool((it = cJSON_GetObjectItemCaseSensitive(root, "has_stems")))) {
        out->has_stems = cJSON_IsTrue(it);
    }

    const cJSON *cues = cJSON_GetObjectItemCaseSensitive(root, "cues");
    if (cJSON_IsArray(cues)) {
        int i = 0;
        cJSON_ArrayForEach(it, cues) {
            if (i >= TRACK_MAX_CUES) break;
            if (cJSON_IsNumber(it)) out->cues[i] = it->valuedouble;
            i++;
        }
    }

    const cJSON *ov = cJSON_GetObjectItemCaseSensitive(root, "overview");
    if (cJSON_IsArray(ov)) {
        int i = 0;
        cJSON_ArrayForEach(it, ov) {
            if (i >= TRACK_OVERVIEW_BINS) break;
            if (cJSON_IsNumber(it)) {
                int v = it->valueint;
                out->overview[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
            i++;
        }
        out->overview_len = i;
    }

    cJSON_Delete(root);
    return true;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

void library_scan(library_t *lib, const char *root)
{
    lib->count = 0;
    DIR *d = opendir(root);
    if (!d) {
        ESP_LOGE(TAG, "cannot open %s", root);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && lib->count < LIBRARY_MAX_TRACKS) {
        if (de->d_name[0] == '.') continue;
        if (!ends_with(de->d_name, ".wav")) continue;
        if (ends_with(de->d_name, ".8ch.wav")) continue; /* stems variant */

        library_entry_t *e = &lib->entry[lib->count];
        snprintf(e->path, sizeof(e->path), "%s/%s", root, de->d_name);

        track_meta_t m;
        track_meta_load(e->path, &m);
        strncpy(e->title, m.title[0] ? m.title : de->d_name, sizeof(e->title) - 1);
        e->title[sizeof(e->title) - 1] = '\0';
        strncpy(e->artist, m.artist, sizeof(e->artist) - 1);
        e->artist[sizeof(e->artist) - 1] = '\0';
        e->bpm = m.bpm;
        lib->count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "library: %d tracks under %s", lib->count, root);
}
