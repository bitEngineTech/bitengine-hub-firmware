#include "ota_cloud.h"   /* ota_cloud_set_running_www_version */
#include "webui.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

static const char *TAG = "webui_static";

/* 前端（www 分区）的身份标识，由 tools/stage_www.py 写进镜像里的
   /www/version.json。只在挂载后读一次并缓存。
   线程上下文：webui_init() 里 webui_mount_spiffs() 早于 httpd_start()，
   所以这个写入发生在任何 HTTP handler 能读到它之前，不需要加锁。 */
static char s_www_version[40] = "unknown";

const char *webui_get_www_version(void)
{
    return s_www_version;
}

/* 读 /www/version.json 取 version 字段。任何一步失败都保持 "unknown" ——
   DevKit 没烧 www 分区、或刷了老版本 www.bin（不含 version.json）时就是这个值。 */
static void load_www_version(void)
{
    FILE *f = fopen("/www/version.json", "r");
    if (!f) {
        ESP_LOGW(TAG, "No /www/version.json — frontend version unknown");
        return;
    }

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        ESP_LOGW(TAG, "/www/version.json is empty");
        return;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "/www/version.json parse failed");
        return;
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(s_www_version, v->valuestring, sizeof(s_www_version));
        ESP_LOGI(TAG, "Frontend version: %s", s_www_version);
        /* 云端更新拿它判断"前端要不要换"。只在真读到时才喂：读不到保持未知，
           未知在那边等于"任何 manifest 里的前端都算不同"，会把缺 version.json 的老前端换掉。 */
        ota_cloud_set_running_www_version(s_www_version);
    } else {
        ESP_LOGW(TAG, "/www/version.json has no usable \"version\" field");
    }
    cJSON_Delete(root);
}

/* Returns true if `coding` appears in an Accept-Encoding value with a non-zero
   qvalue. Only the forms that show up in practice are parsed: a bare token
   ("gzip"), and an explicit rejection ("gzip;q=0", "gzip;q=0.000"). Anything
   we don't understand is treated as acceptable — guessing "no" would hand a
   406 to a client that would have been perfectly happy. */
static bool coding_acceptable(const char *hdr, const char *coding)
{
    size_t clen = strlen(coding);

    for (const char *p = hdr; (p = strstr(p, coding)) != NULL; p += clen) {
        /* Must be a whole token, not a substring of e.g. "x-gzip". */
        if (p != hdr) {
            char prev = p[-1];
            if (prev != ',' && prev != ' ' && prev != '\t') continue;
        }
        const char *q = p + clen;
        if (*q != '\0' && *q != ',' && *q != ';' && *q != ' ' && *q != '\t') continue;

        while (*q == ' ' || *q == '\t') q++;
        if (*q != ';') return true;                 /* no parameters -> accepted */
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != 'q' && *q != 'Q') return true;    /* some other parameter */
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') return true;
        q++;
        while (*q == ' ' || *q == '\t') q++;

        if (*q != '0') return true;                 /* q=1, q=0.5, ... */
        q++;
        if (*q != '.') return false;                /* exactly "q=0" */
        q++;
        while (*q >= '0' && *q <= '9') {            /* q=0.000 -> rejected */
            if (*q != '0') return true;             /* q=0.001 -> accepted */
            q++;
        }
        return false;
    }
    return false;
}

/* RFC 9110 §12.5.3: a request with no Accept-Encoding accepts *any* coding, so
   only a header that explicitly rules gzip out forces our hand. The www image
   stores nothing but pre-gzipped files — there is no identity copy to fall back
   on — so such a client gets 406 rather than a gzip body it just told us it
   cannot read. Browsers always send "gzip", so this path is for curl/scripts. */
static bool client_accepts_gzip(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
    if (len == 0) return true;

    char buf[128];
    if (len >= sizeof(buf)) return true;   /* too long to judge -> assume ok */
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof(buf)) != ESP_OK) {
        return true;
    }
    return coding_acceptable(buf, "gzip") || coding_acceptable(buf, "*");
}

static const char *get_content_type(const char *uri)
{
    if (strstr(uri, ".html")) return "text/html";
    if (strstr(uri, ".json")) return "application/json";  // before .js!
    if (strstr(uri, ".js"))   return "application/javascript";
    if (strstr(uri, ".css"))  return "text/css";
    if (strstr(uri, ".svg"))  return "image/svg+xml";
    if (strstr(uri, ".png"))  return "image/png";
    if (strstr(uri, ".ico"))  return "image/x-icon";
    return "text/plain";
}

static esp_err_t handle_static(httpd_req_t *req)
{
    // Build SPIFFS path
    char path[128];
    const char *uri = req->uri;

    // Reject path traversal attempts
    if (strstr(uri, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_OK;
    }

    // Strip query string
    const char *query = strchr(uri, '?');
    size_t uri_len = query ? (size_t)(query - uri) : strlen(uri);

    if (uri_len == 1 && uri[0] == '/') {
        strlcpy(path, "/www/index.html.gz", sizeof(path));
    } else {
        snprintf(path, sizeof(path), "/www%.*s.gz", (int)uri_len, uri);
    }

    // Try to open file
    FILE *f = fopen(path, "r");

    // SPA fallback: try index.html.gz for non-existent paths — but never for
    // API routes: an unknown /api/* must be a 404, not a 200 with HTML.
    if (!f) {
        if (strncmp(uri, "/api", 4) == 0) {
            return send_error(req, 404, "not_found");
        }
        strlcpy(path, "/www/index.html.gz", sizeof(path));
        f = fopen(path, "r");
    }

    if (!f) {
        // DevKit: no frontend at all — return plain text status page
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req,
            "bitEngine API ready. No frontend installed.\n"
            "\n=== Auth ===\n"
            "  GET  /api/v1/auth/status    - Check if password required\n"
            "  POST /api/v1/auth/login     - Login (JSON: {\"password\":\"...\"})\n"
            "  GET  /api/v1/auth/logout    - Logout\n"
            "  POST /api/v1/auth/password  - Change password\n"
            "\n=== Dashboard ===\n"
            "  GET  /api/v1/status         - Full system status (JSON)\n"
            "\n=== WiFi ===\n"
            "  GET  /api/v1/wifi/scan      - Scan 5GHz WiFi networks\n"
            "  POST /api/v1/wifi/config    - Set WiFi (JSON: {\"ssid\":\"...\",\"password\":\"...\"})\n"
            "\n=== Ethernet ===\n"
            "  GET  /api/v1/ethernet/status - ETH link status\n"
            "  POST /api/v1/ethernet/config - Set static IP / DHCP\n"
            "\n=== Zigbee ===\n"
            "  GET  /api/v1/zigbee/status  - Channel, TX power, MG24 health\n"
            "  POST /api/v1/zigbee/restart - Restart MG24 Zigbee chip\n"
            "\n=== Spectrum ===\n"
            "  GET  /api/v1/spectrum/scan      - 802.15.4 energy scan (16 channels)\n"
            "  GET  /api/v1/spectrum/beacon     - Zigbee neighbor discovery\n"
            "  GET  /api/v1/spectrum/recommend  - Best channel recommendation\n"
            "\n=== Config ===\n"
            "  GET  /api/v1/config/export  - Export config as JSON\n"
            "  POST /api/v1/config/import  - Import config from JSON\n"
            "  POST /api/v1/config/factory - Factory reset\n"
            "\n=== OTA ===\n"
            "  POST /api/v1/ota/esp32      - Upload firmware binary (bitengine-*.bin)\n"
            "  POST /api/v1/ota/www        - Upload frontend image (www.bin)\n"
            "\n=== Bridge Mode ===\n"
            "  POST /api/v1/mode           - Switch LAN/USB/Matter (JSON: {\"mode\":0})\n"
            "\n=== SMLIGHT Compat ===\n"
            "  GET  /api2/info             - Device info (HA integration)\n"
            "  GET  /api2/state            - Device state\n"
            "  POST /api2/reboot           - Reboot device\n"
            "  POST /api2/zb_restart       - Restart Zigbee chip\n"
            "\n=== WebSocket ===\n"
            "  WS   /ws                    - Real-time status push (2s interval)\n"
            "\nFlash www partition with SPIFFS image for full WebUI.\n");
        return ESP_OK;
    }

    // Everything below this point is served straight out of the .gz file, so a
    // client that explicitly refused gzip cannot be satisfied — say so instead
    // of sending it a body it cannot decode.
    if (!client_accepts_gzip(req)) {
        fclose(f);
        httpd_resp_set_status(req, "406 Not Acceptable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req,
            "The WebUI is stored pre-compressed; only Content-Encoding: gzip "
            "is available.\nRetry with 'Accept-Encoding: gzip' (curl: use "
            "--compressed).\n");
        return ESP_OK;
    }

    // Determine Content-Type from SPIFFS path (handles "/" -> index.html.gz)
    httpd_resp_set_type(req, get_content_type(path));
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    // Cache headers
    if (strstr(uri, ".js") || strstr(uri, ".css")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    }

    // Chunked transfer
    char buf[1024];
    size_t read;
    while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // End chunked transfer

    return ESP_OK;
}

// ---- SPIFFS mount ----
esp_err_t webui_mount_spiffs(void)
{
    /* 看门狗 L3 重启 httpd 会再走一遍 webui_init()，分区早已挂着：再注册只会回
       ESP_ERR_INVALID_STATE，下面那行 "no frontend — DevKit mode" 就成了误报
       （2026-10-07 TC-W07 复测时出现在 /api/v1/logs 里，前端其实照常）。 */
    if (esp_spiffs_mounted("www")) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path       = "/www",
        .partition_label = "www",
        .max_files       = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s (no frontend — DevKit mode)",
                 esp_err_to_name(err));
        return err;  // non-fatal: static handler will return plain text fallback
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("www", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %d/%d bytes used", (int)used, (int)total);

    load_www_version();
    return ESP_OK;
}

// ---- Registration (must be LAST — wildcard catch-all) ----
void register_static_file_handler(httpd_handle_t server)
{
    httpd_uri_t static_uri = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handle_static,
    };
    /* ⚠️ 必须查返回值。这个 handler 是最后注册的，一旦 max_uri_handlers 用满，
       httpd 只会打一行 "no slots left" 的 warning 就算了，而这里若无条件报成功，
       现象就是**整个前端 404**（httpd 自己的 "Nothing matches the given URI"），
       后端 API 却一切正常 —— 极难联想到是槽位不够。2026-08-11 真踩过：
       槽位恰好用满 80，新增两个 API 端点就把静态 handler 挤掉了。 */
    esp_err_t err = httpd_register_uri_handler(server, &static_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "静态文件 handler 注册失败: %s —— 整个 WebUI 前端将返回 404，"
                      "多半是 max_uri_handlers 用满了（见 webui_server.c）",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Static file handler registered (wildcard /*)");
}
