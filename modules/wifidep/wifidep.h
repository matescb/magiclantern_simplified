#ifndef _WIFIDEP_H_
#define _WIFIDEP_H_

/*
 * wifidep — WiFi build-deploy for the EOS M50 Mark II (dev tool).
 * Loaded from ML/SETTINGS/WIFI.CFG (same format as modules/yolo).
 */

/* Network config, loaded from card. Fields are overwritten by WIFI.CFG:
 *   line 1: SSID
 *   line 2: passphrase
 *   line 3: client_IP   (decimal uint32, host byte order for sin_addr)
 *   line 4: server_IP    (decimal uint32)
 *   line 5: gateway_IP   (decimal uint32)
 *   line 6: server_port  (decimal)
 * Each line is NUL-terminated.
 */
struct network_config
{
    char SSID[32];
    char passphrase[64];
    uint32_t client_IP;
    uint32_t gateway_IP;
    uint32_t server_IP;
    uint16_t server_port;
};

/* Per-file ack status codes returned to the host (LE u32). */
#define WFD_OK          0
#define WFD_ERR_CRC     1
#define WFD_ERR_ALLOC   2
#define WFD_ERR_CREATE  3
#define WFD_ERR_WRITE   4
#define WFD_ERR_SIZE    5

#endif
