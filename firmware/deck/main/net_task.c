#include "net_task.h"

#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "bus_link.h"
#include "deck_state.h"
#include "espdj_mix.h"

static const char *TAG = "net";

#define AP_SSID     "ESP-DJ"
#define AP_MAX_CONN 4
#define WS_MAX_CLIENTS 4

static httpd_handle_t s_httpd;
static int s_ws_fd[WS_MAX_CLIENTS];
/* Phone-side controller state (used when no hardware controller docked). */
static float s_phone_xf = 0.5f;
static float s_phone_tempo[2] = {0.0f, 0.0f};

/* ---- captive portal DNS: answer every A query with our AP address ---- */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &flen);
        if (len < 12) continue;
        /* Minimal DNS response: copy the query, set QR|AA, answer count 1,
         * append an A record pointing at 192.168.4.1. */
        buf[2] = 0x84; buf[3] = 0x00; /* response, authoritative */
        buf[6] = 0x00; buf[7] = 0x01; /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int pos = len;
        if (pos + 16 > (int)sizeof(buf)) continue;
        const uint8_t answer[] = {
            0xC0, 0x0C,             /* name: pointer to query */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60 */
            0x00, 0x04,             /* rdlength */
            192, 168, 4, 1,
        };
        memcpy(&buf[pos], answer, sizeof(answer));
        sendto(sock, buf, pos + sizeof(answer), 0,
               (struct sockaddr *)&from, flen);
    }
}

/* ---- WebSocket ---- */

static void ws_register(int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fd[i] == fd) return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fd[i] < 0) {
            s_ws_fd[i] = fd;
            return;
        }
    }
}

static void apply_phone_control(void)
{
    /* Phone is the controller of last resort: if a hardware controller is
     * docked, its 250 Hz CONTROL frames win and we don't fight them. */
    espdj_control_t hw;
    if (link_get_control(&hw)) return;

    float ga, gb;
    espdj_xfader_gains(s_phone_xf, 0.0f, &ga, &gb);
    uint8_t side = atomic_load(&g_deck.side);
    bool right = (side == ESPDJ_SIDE_RIGHT);
    atomic_store(&g_deck.xfader_gain,
                 atomic_load(&g_deck.wireless) ? 1.0f : (right ? gb : ga));
    atomic_store(&g_deck.pitch, s_phone_tempo[right ? 1 : 0]);
}

static void handle_ws_json(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) return;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(t)) goto out;

    if (strcmp(t->valuestring, "xf") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        if (cJSON_IsNumber(v)) {
            s_phone_xf = (float)v->valuedouble;
            apply_phone_control();
        }
    } else if (strcmp(t->valuestring, "tempo") == 0) {
        const cJSON *side = cJSON_GetObjectItemCaseSensitive(root, "side");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        if (cJSON_IsNumber(side) && cJSON_IsNumber(v)) {
            int i = side->valueint == ESPDJ_SIDE_RIGHT ? 1 : 0;
            s_phone_tempo[i] = (float)v->valuedouble;
            apply_phone_control();
        }
    } else if (strcmp(t->valuestring, "eq") == 0 ||
               strcmp(t->valuestring, "filter") == 0) {
        const cJSON *side = cJSON_GetObjectItemCaseSensitive(root, "side");
        uint8_t my_side = atomic_load(&g_deck.side);
        if (cJSON_IsNumber(side) && (uint8_t)side->valueint == my_side) {
            const cJSON *lo = cJSON_GetObjectItemCaseSensitive(root, "low");
            const cJSON *mi = cJSON_GetObjectItemCaseSensitive(root, "mid");
            const cJSON *hi = cJSON_GetObjectItemCaseSensitive(root, "high");
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
            if (cJSON_IsNumber(lo)) atomic_store(&g_deck.eq_low_db, (float)lo->valuedouble);
            if (cJSON_IsNumber(mi)) atomic_store(&g_deck.eq_mid_db, (float)mi->valuedouble);
            if (cJSON_IsNumber(hi)) atomic_store(&g_deck.eq_high_db, (float)hi->valuedouble);
            if (cJSON_IsNumber(v)) atomic_store(&g_deck.filter_pos, (float)v->valuedouble);
        }
        /* EQ/filter for the partner deck rides the transport relay in a
         * v2 message; v1 phone EQ addresses the serving deck only. */
    } else if (strcmp(t->valuestring, "op") == 0) {
        const cJSON *side = cJSON_GetObjectItemCaseSensitive(root, "side");
        const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
        const cJSON *arg = cJSON_GetObjectItemCaseSensitive(root, "arg");
        if (cJSON_IsNumber(side) && cJSON_IsString(op)) {
            static const struct { const char *name; espdj_transport_op_t op; } ops[] = {
                {"play", ESPDJ_TR_PLAY_TOGGLE}, {"cue", ESPDJ_TR_CUE_PRESS},
                {"cue_up", ESPDJ_TR_CUE_RELEASE}, {"hotcue", ESPDJ_TR_HOT_CUE},
                {"loop", ESPDJ_TR_LOOP_TOGGLE}, {"jump", ESPDJ_TR_BEAT_JUMP},
                {"slip", ESPDJ_TR_SLIP_TOGGLE}, {"sync", ESPDJ_TR_SYNC_TOGGLE},
                {"load", ESPDJ_TR_LOAD_TRACK},
            };
            for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
                if (strcmp(op->valuestring, ops[i].name) == 0) {
                    int a = cJSON_IsNumber(arg) ? arg->valueint : 0;
                    espdj_transport_t tr = {
                        .side = (uint8_t)side->valueint,
                        .op = ops[i].op,
                        .arg = (uint8_t)(int8_t)a,
                        .arg2 = (uint8_t)((a >> 8) & 0xFF),
                    };
                    link_route_transport(&tr);
                    break;
                }
            }
        }
    }
out:
    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ws_register(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.len == 0 || frame.len > 512) return ESP_OK;

    uint8_t buf[513];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;
    buf[frame.len] = '\0';
    ws_register(httpd_req_to_sockfd(req));
    handle_ws_json((const char *)buf);
    return ESP_OK;
}

/* 10 Hz state push to connected phones. */
static void state_push_task(void *arg)
{
    (void)arg;
    char json[512];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_httpd) continue;
        uint32_t pos_ms = (uint32_t)((uint64_t)atomic_load(&g_deck.position_frames) * 1000 / 44100);
        snprintf(json, sizeof(json),
                 "{\"t\":\"state\",\"side\":%u,\"playing\":%d,\"bpm\":%.1f,"
                 "\"pos_ms\":%lu,\"pitch\":%.4f,\"loop\":%d,\"slip\":%d,"
                 "\"sync\":%d,\"phase\":%.3f,\"loaded\":%d,\"title\":\"%s\","
                 "\"wireless\":%d,\"xf\":%.3f}",
                 atomic_load(&g_deck.side), atomic_load(&g_deck.playing),
                 (double)atomic_load(&g_deck.effective_bpm),
                 (unsigned long)pos_ms, (double)atomic_load(&g_deck.pitch),
                 atomic_load(&g_deck.loop_active), atomic_load(&g_deck.slip),
                 atomic_load(&g_deck.sync_enabled),
                 (double)atomic_load(&g_deck.beat_phase),
                 atomic_load(&g_deck.track_loaded), g_deck.meta.title,
                 atomic_load(&g_deck.wireless), (double)s_phone_xf);
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_ws_fd[i] >= 0 &&
                httpd_ws_send_frame_async(s_httpd, s_ws_fd[i], &frame) != ESP_OK) {
                s_ws_fd[i] = -1;
            }
        }
    }
}

/* ---- static SPA from LittleFS (with captive-portal redirect) ---- */

static esp_err_t index_handler(httpd_req_t *req)
{
    FILE *f = fopen("/lfs/index.html", "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "web app missing from LittleFS");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, (ssize_t)n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    /* OS captive-portal probes (generate_204, hotspot-detect, ...) land
     * here; a 302 to our root pops the sign-in sheet with the app. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void net_task_start(void)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++) s_ws_fd[i] = -1;

    /* LittleFS partition holds the SPA (flashed from webapp/index.html). */
    esp_vfs_littlefs_conf_t lfs = {
        .base_path = "/lfs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&lfs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "littlefs: %s", esp_err_to_name(err));
    }

    /* SoftAP */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN, /* captive portal, zero typing */
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* mDNS: http://dj.local for repeat visits */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("dj");
    mdns_instance_name_set("ESP-DJ3000");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    /* HTTP + WS */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.uri_match_fn = httpd_uri_match_wildcard;
    hcfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hcfg));

    static const httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true,
    };
    static const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler,
    };
    static const httpd_uri_t catchall_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = redirect_handler,
    };
    httpd_register_uri_handler(s_httpd, &ws_uri);
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &catchall_uri);

    xTaskCreatePinnedToCore(dns_task, "captive_dns", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(state_push_task, "ws_push", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "SoftAP '%s' up — captive portal + http://dj.local", AP_SSID);
}
