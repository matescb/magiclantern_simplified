/*
 * wifidep — Magic Lantern WiFi build-deploy for the Canon EOS M50 Mark II (DIGIC 8).
 *
 * Dev tool.  On a serial-injected REC/LV key the camera joins a 2.4 GHz WPA2 LAN,
 * dials the host as a TCP CLIENT, pulls a fresh AUTOEXEC.BIN (framed + CRC32),
 * writes it to the card with an atomic rename (keeping a .BAK for reader-side
 * rollback), acks the host, tears the WLAN back down, and optionally reboots into
 * the new build.  No card shuffle.
 *
 * Reuses the HW-proven DIGIC-8 WLAN bring-up + socket recipe from
 * modules/yolo/yolo.c and the socket/WLAN API in src/ml_socket.h.
 * Canon .FIR firmware is NEVER touched.
 *
 * Wire protocol (camera = client; all app ints little-endian):
 *   cam ->  "MLWF" (4 bytes hello)
 *   srv ->  "MLWF" (4) + u32 file_count
 *   srv ->  per file: u32 name_len, name[name_len], u32 file_len, u32 crc32, bytes
 *   cam ->  per file ack: u32 status, u32 recv_crc
 */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include "string.h"
#include "fio-ml.h"
#include "config.h"
#include "ml_socket.h"
#include "propvalues.h"
#include "wifidep.h"

/* core primitives provided by the M50 II platform */
extern void m50ii_uart_str(const char *s);   /* src/boot-d678.c */

/* Reboot into the new build automatically after a successful deploy. */
static int g_wfd_reboot = 1;
/* Guard so a repeated trigger doesn't spawn overlapping deploy tasks. */
static int g_wfd_busy = 0;

/* --------------------------------------------------------------------- log */
static void wfd_log(const char *s)
{
    m50ii_uart_str(s);
}

static void wfd_logx(const char *label, uint32_t v)
{
    char b[96];
    snprintf(b, sizeof(b), "[WFD] %s=0x%x\n", label, v);
    m50ii_uart_str(b);
}

/* ------------------------------------------------------------------ helpers */
static uint16_t wfd_htons(uint16_t p)
{
    return (uint16_t)((p >> 8) | ((p & 0xff) << 8));
}

/* zlib-compatible CRC32 (poly 0xEDB88320), matches host zlib.crc32. */
static uint32_t wfd_crc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static int wfd_recv_all(int s, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n)
    {
        int r = socket_recv(s, p + got, (int)(n - got), 0);
        if (r <= 0)
            return -1;
        got += (uint32_t)r;
    }
    return 0;
}

static int wfd_send_all(int s, const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t sent = 0;
    while (sent < n)
    {
        int r = socket_send(s, (void *)(p + sent), (int)(n - sent), 0);
        if (r <= 0)
            return -1;
        sent += (uint32_t)r;
    }
    return 0;
}

static int wfd_recv_u32(int s, uint32_t *v)
{
    uint8_t b[4];
    if (wfd_recv_all(s, b, 4) < 0)
        return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int wfd_send_u32(int s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return wfd_send_all(s, b, 4);
}

/* ------------------------------------------------------------- config (card)
 * Parses ML/SETTINGS/WIFI.CFG. Same format/parser as modules/yolo/yolo.c. */
static int wfd_get_config(struct network_config *config)
{
    char filename[FIO_MAX_PATH_LENGTH];
    memset(config, '\0', sizeof(struct network_config));

    uint32_t config_buf_size = 1024;
    char *config_buf = malloc(config_buf_size);
    if (config_buf == NULL)
        return -1;
    memset(config_buf, '\0', config_buf_size);

    snprintf(filename, FIO_MAX_PATH_LENGTH, "%sWIFI.CFG", get_config_dir());
    FILE *f = FIO_OpenFile(filename, O_RDONLY | O_SYNC);
    if (!f)
    {
        wfd_log("[WFD] couldn't open ML/SETTINGS/WIFI.CFG\n");
        free(config_buf);
        return -1;
    }
    if (!FIO_ReadFile(f, config_buf, config_buf_size))
    {
        wfd_log("[WFD] couldn't read WIFI.CFG\n");
        FIO_CloseFile(f);
        free(config_buf);
        return -1;
    }
    FIO_CloseFile(f);

    /* line 1: SSID */
    strncpy(config->SSID, config_buf, sizeof(config->SSID));
    config->SSID[sizeof(config->SSID) - 1] = '\0';
    int i = 0;
    int null_max = sizeof(config->SSID);
    while (i < null_max && config_buf[i] != '\0')
        i++;
    if (config_buf[i] != '\0')
        goto ret_err;
    i++;

    /* line 2: passphrase */
    strncpy(config->passphrase, config_buf + i, sizeof(config->passphrase));
    config->passphrase[sizeof(config->passphrase) - 1] = '\0';
    null_max += sizeof(config->passphrase);
    while (i < null_max && config_buf[i] != '\0')
        i++;
    if (config_buf[i] != '\0')
        goto ret_err;
    i++;

    /* line 3: client IP (decimal) */
    config->client_IP = atoi(config_buf + i);
    null_max += 10;
    while (i < null_max && config_buf[i] != '\0')
        i++;
    if (config_buf[i] != '\0')
        goto ret_err;
    i++;

    /* line 4: server IP (decimal) */
    config->server_IP = atoi(config_buf + i);
    null_max += 10;
    while (i < null_max && config_buf[i] != '\0')
        i++;
    if (config_buf[i] != '\0')
        goto ret_err;
    i++;

    /* line 5: gateway IP (decimal) */
    config->gateway_IP = atoi(config_buf + i);
    null_max += 10;
    while (i < null_max && config_buf[i] != '\0')
        i++;
    if (config_buf[i] != '\0')
        goto ret_err;
    i++;

    /* line 6: server port */
    int port = atoi(config_buf + i);
    if (port <= 0 || port > 65535)
        goto ret_err;
    config->server_port = (uint16_t)port;

    free(config_buf);
    return 0;
ret_err:
    wfd_log("[WFD] WIFI.CFG parse error\n");
    free(config_buf);
    return -1;
}

/* ------------------------------------------------------------------ WLAN up
 * Station mode, WPA2-PSK/AES. Mirrors the yolo bring-up, minus the 200D-only
 * Lime-ready pointer poll (replaced by a settle delay for MVP robustness). */
static int wfd_wlan_up(struct network_config *config)
{
    struct wlan_settings *ws = malloc(sizeof(*ws));
    if (ws == NULL)
        return -1;
    memset(ws, 0, sizeof(*ws));

    ws->mode = WLAN_MODE_INFRA;
    ws->mode_something = 2;
    strcpy(ws->SSID, config->SSID);
    ws->channel = 0;                     /* auto */
    ws->auth_mode = WLAN_AUTH_MODE_WPA2PSK;
    ws->cipher_mode = WLAN_CIPHER_MODE_AES;
    ws->unk_03 = 6;                      /* set to 6 for INFRA/BOTH */
    strcpy(ws->key, config->passphrase);

    /* wake the Lime core (WiFi) and bring the interface up */
    call("NwLimeInit");
    call("NwLimeOn");
    call("wlanpoweron");
    call("wlanup");
    call("wlanchk");
    msleep(2000);                        /* let Lime/WLAN settle */

    int res = wlan_connect(ws);
    free(ws);                            /* wlan_connect copies to a global */
    if (res != 0)
    {
        wfd_logx("wlan_connect err", (uint32_t)res);
        return -1;
    }

    nif_setup(0);
    res = set_IP_address(0, config->client_IP, 0x00ffffff, config->gateway_IP);
    if (res != 0)
        wfd_logx("set_IP err (non-fatal)", (uint32_t)res);

    msleep(1500);                        /* allow association to complete */
    return 0;
}

static void wfd_wlan_down(void)
{
    call("wlandown");
    call("wlanpoweroff");
}

/* -------------------------------------------------------------- write + commit
 * ubuf must be an UNCACHEABLE() alias whose bytes were sync_caches()'d. */
static uint32_t wfd_write_file(const char *name, void *ubuf, uint32_t len)
{
    char full[64], tmp[80], bak[80];
    snprintf(full, sizeof(full), "B:/%s", name);
    snprintf(tmp,  sizeof(tmp),  "%s.TMP", full);
    snprintf(bak,  sizeof(bak),  "%s.BAK", full);

    FIO_RemoveFile(tmp);                  /* clear any stale temp */
    FILE *f = FIO_CreateFile(tmp);
    if (f == NULL)
    {
        wfd_log("[WFD] create TMP failed\n");
        return WFD_ERR_CREATE;
    }
    int w = FIO_WriteFile(f, ubuf, len);  /* single uncacheable write (docs/35 gate) */
    FIO_CloseFile(f);
    if (w != (int)len)
    {
        wfd_logx("write ret", (uint32_t)w);
        return WFD_ERR_WRITE;
    }

    uint32_t on_card = FIO_GetFileSize_direct(tmp);
    if (on_card != len)
    {
        wfd_logx("card size", on_card);
        return WFD_ERR_SIZE;
    }

    /* atomic-ish commit; keep <name>.BAK for card-reader rollback */
    FIO_RemoveFile(bak);                  /* best effort */
    FIO_RenameFile(full, bak);            /* best effort (absent on first deploy) */
    if (FIO_RenameFile(tmp, full) != 0)
    {
        wfd_log("[WFD] rename TMP->live failed\n");
        return WFD_ERR_WRITE;
    }
    return WFD_OK;
}

/* --------------------------------------------------------------- deploy task */
static void wfd_deploy_task(void *unused)
{
    struct network_config config;
    int s = -1;

    wfd_log("[WFD] deploy start\n");

    if (wfd_get_config(&config) < 0)
        goto done;

    if (wfd_wlan_up(&config) < 0)
    {
        wfd_log("[WFD] WLAN bring-up failed\n");
        goto teardown;
    }

    s = socket_create(1, 1, 0);
    if (s < 0)
    {
        wfd_logx("socket_create", (uint32_t)s);
        s = -1;
        goto teardown;
    }

    struct sockaddr_in sa;
    sa.sin_family = SOCK_FAMILY_IPv4;
    sa.sin_port = wfd_htons(config.server_port);
    sa.sin_addr = config.server_IP;
    memset(sa.sin_zero, '\0', sizeof(sa.sin_zero));

    int connected = 0;
    for (int r = 0; r < 6; r++)
    {
        if (socket_connect(s, &sa, sizeof(sa)) >= 0)
        {
            connected = 1;
            break;
        }
        wfd_log("[WFD] connect retry\n");
        msleep(700);
    }
    if (!connected)
    {
        wfd_log("[WFD] connect failed\n");
        goto close;
    }
    wfd_log("[WFD] connected\n");

    /* handshake: hello, then read server magic + file count */
    if (wfd_send_all(s, "MLWF", 4) < 0)
    {
        wfd_log("[WFD] hello send failed\n");
        goto close;
    }
    char magic[4];
    if (wfd_recv_all(s, magic, 4) < 0 ||
        magic[0] != 'M' || magic[1] != 'L' || magic[2] != 'W' || magic[3] != 'F')
    {
        wfd_log("[WFD] bad server magic\n");
        goto close;
    }

    uint32_t file_count = 0;
    if (wfd_recv_u32(s, &file_count) < 0)
        goto close;
    wfd_logx("file_count", file_count);
    if (file_count == 0 || file_count > 8)
    {
        wfd_log("[WFD] bad file_count\n");
        goto close;
    }

    for (uint32_t fi = 0; fi < file_count; fi++)
    {
        uint32_t name_len = 0, file_len = 0, crc = 0, got_crc = 0, status;
        char name[48];

        if (wfd_recv_u32(s, &name_len) < 0)
            goto close;
        if (name_len == 0 || name_len >= sizeof(name))
        {
            wfd_log("[WFD] bad name_len\n");
            goto close;
        }
        if (wfd_recv_all(s, name, name_len) < 0)
            goto close;
        name[name_len] = '\0';
        /* The name is untrusted network input that flows into a card path
         * (B:/<name>). Allow card subdirs (e.g. ML/MODULES/foo.mo) so modules can
         * be deployed, but reject traversal / absolute / drive-letter escapes so a
         * rogue LAN server can't write outside the card tree. */
        {
            int bad = (name[0] == '/' || name[0] == '.');  /* no absolute, no leading dot */
            for (uint32_t j = 0; j < name_len && !bad; j++)
            {
                char c = name[j];
                if (c == '\\' || c == ':' || (unsigned char)c < 0x20)
                    bad = 1;                                /* no backslash/drive/control */
                if (c == '.' && j + 1 < name_len && name[j + 1] == '.')
                    bad = 1;                                /* no ".." anywhere (traversal) */
            }
            if (bad)
            {
                wfd_log("[WFD] rejecting unsafe filename\n");
                goto close;
            }
        }
        if (wfd_recv_u32(s, &file_len) < 0)
            goto close;
        if (wfd_recv_u32(s, &crc) < 0)
            goto close;

        wfd_log("[WFD] file: ");
        wfd_log(name);
        wfd_log("\n");
        wfd_logx("file_len", file_len);

        void *buf = (file_len > 0) ? fio_malloc(file_len) : NULL;
        if (buf == NULL)
        {
            wfd_log("[WFD] fio_malloc failed\n");
            wfd_send_u32(s, WFD_ERR_ALLOC);
            wfd_send_u32(s, 0);
            goto close;
        }

        /* recv straight into the uncacheable alias: CPU stores land in RAM, so the
         * later DMA FIO_WriteFile is coherent with no cache-clean needed. */
        void *ubuf = UNCACHEABLE(buf);
        if (wfd_recv_all(s, ubuf, file_len) < 0)
        {
            fio_free(buf);
            wfd_log("[WFD] recv body failed\n");
            goto close;
        }

        got_crc = wfd_crc32((const uint8_t *)ubuf, file_len);
        if (got_crc != crc)
        {
            wfd_logx("crc got", got_crc);
            wfd_logx("crc want", crc);
            status = WFD_ERR_CRC;
        }
        else
        {
            status = wfd_write_file(name, ubuf, file_len);
        }
        fio_free(buf);

        wfd_logx("file status", status);
        wfd_send_u32(s, status);
        wfd_send_u32(s, got_crc);
        if (status != WFD_OK)
            goto close;
    }
    wfd_log("[WFD] all files written OK\n");

close:
    if (s >= 0)
        socket_close_caller(socket_convertfd(s));
teardown:
    wfd_wlan_down();
done:
    wfd_log("[WFD] deploy end\n");
    if (g_wfd_reboot)
    {
        wfd_log("[WFD] rebooting into new build\n");
        msleep(800);
        int reboot = 0;
        prop_request_change(PROP_REBOOT, &reboot, 4);
    }
    g_wfd_busy = 0;
}

/* ------------------------------------------------------------- key trigger
 * REC/LV key (serial-injectable via handle_module_keys) kicks a deploy.
 * NOTE: this dev build hijacks the REC/LV key for deploy. */
static unsigned int wfd_keypress_cbr(unsigned int key)
{
    /* Trigger on INFO (inject BGMT_INFO=0x08 via handle_module_keys). INFO is a
     * key raw_vidx ignores (it only handles LV/REC/ZOOMIN), so this fires in
     * EVERY mode — including movie mode — with no conflict over the record key,
     * hence no is_movie_mode() guard needed. */
    if (key == MODULE_KEY_INFO)
    {
        if (!g_wfd_busy)
        {
            g_wfd_busy = 1;
            wfd_log("[WFD] trigger (INFO) -> deploy\n");
            task_create("wfd_deploy", 0x1d, 0x2000, wfd_deploy_task, NULL);
        }
        return 0;   /* consume */
    }
    return 1;       /* pass through */
}

static unsigned int wfd_init()
{
    wfd_log("[WFD] module loaded (INFO key = wifi deploy, all modes)\n");
    return 0;
}

static unsigned int wfd_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(wfd_init)
    MODULE_DEINIT(wfd_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, wfd_keypress_cbr, 0)
MODULE_CBRS_END()
