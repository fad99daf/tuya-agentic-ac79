#include "iot_ota.h"
#include "atop.h"
#include "iot_dp_internal.h"
#include "iot_config_defaults.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/**
 * @file iot_ota.c
 * @brief Public OTA API implementation — wraps the ATOP-layer functions,
 *        resolving ATOP host/port from the iot_client_t.
 *
 * Each public function follows the same pattern as iot_client_get_session_token():
 * resolve host/port via iot_client_resolve_atop_host(), then call the
 * corresponding atop_* function with client->devid / secret_key / cacert.
 */

int tuya_iot_ota_report_version(iot_client_t *client, const char *sw_ver)
{
    if (client == NULL || sw_ver == NULL || sw_ver[0] == '\0') {
        return OPRT_INVALID_PARAMETER;
    }

    char host[64] = {0};
    uint16_t port = IOT_DEFAULT_PORT;
    iot_client_resolve_atop_host(client, host, sizeof(host), &port);

    ota_version_update_request_t req = {
        .devid   = client->devid,
        .key     = client->secret_key,
        .sw_ver  = sw_ver,
        .pv      = IOT_SDK_PV,
        .bv      = IOT_SDK_BV,
        .channel = 0,
        .host    = host[0] ? host : NULL,
        .port    = port,
        .cacert  = client->cacert,
        .cert_bundle_attach = client->cert_bundle_attach,
    };

    return atop_version_update(client->pal, &req);
}

/* upgrade.get(确认后拉包) / silent.get(设备自检) 公共实现:仅分叉 ATOP 接口,
 * 请求构造与响应映射完全一致(响应结构两接口相同,见 atop.c 公共实现注释)。*/
static int iot_ota_check_upgrade_impl(iot_client_t *client, int channel,
                                      bool silent, tuya_iot_ota_upgrade_info_t *info)
{
    if (client == NULL || info == NULL) {
        return OPRT_INVALID_PARAMETER;
    }

    memset(info, 0, sizeof(tuya_iot_ota_upgrade_info_t));

    char host[64] = {0};
    uint16_t port = IOT_DEFAULT_PORT;
    iot_client_resolve_atop_host(client, host, sizeof(host), &port);

    ota_upgrade_request_t req = {
        .devid   = client->devid,
        .key     = client->secret_key,
        .channel = channel,
        .host    = host[0] ? host : NULL,
        .port    = port,
        .cacert  = client->cacert,
        .cert_bundle_attach = client->cert_bundle_attach,
    };

    ota_upgrade_response_t resp = {0};
    int rt = silent ? atop_upgrade_silent_get(client->pal, &req, &resp)
                    : atop_upgrade_get(client->pal, &req, &resp);
    if (rt != OPRT_OK) {
        return rt;
    }

    /* Map internal response to public struct */
    info->has_upgrade = resp.has_upgrade;
    info->version     = resp.version;
    info->url         = resp.url;
    info->file_size   = resp.file_size;
    info->channel     = resp.channel;
    info->md5         = resp.md5;
    info->hmac        = resp.hmac;

    return OPRT_OK;
}

int tuya_iot_ota_check_upgrade(iot_client_t *client, int channel,
                          tuya_iot_ota_upgrade_info_t *info)
{
    return iot_ota_check_upgrade_impl(client, channel, false, info);
}

/* 设备主动自检(开机/定时)专用:silent.get 只返回"静默升级"任务,APP 提醒
 * 升级任务不下发——须等用户在 App 确认、云端推 MQTT protocol 15 后再走
 * tuya_iot_ota_check_upgrade(upgrade.get)拉包。(2026-09-29 修复:开机自检
 * 误用 upgrade.get,平台"APP提醒升级"任务被当静默任务直接刷机。) */
int tuya_iot_ota_check_upgrade_silent(iot_client_t *client,
                                      tuya_iot_ota_upgrade_info_t *info)
{
    return iot_ota_check_upgrade_impl(client, 0, true, info);
}

int tuya_iot_ota_report_status(iot_client_t *client, int channel, tuya_iot_ota_status_t status)
{
    if (client == NULL) {
        return OPRT_INVALID_PARAMETER;
    }

    char host[64] = {0};
    uint16_t port = IOT_DEFAULT_PORT;
    iot_client_resolve_atop_host(client, host, sizeof(host), &port);

    ota_status_update_request_t req = {
        .devid   = client->devid,
        .key     = client->secret_key,
        .channel = channel,
        .status  = status,
        .host    = host[0] ? host : NULL,
        .port    = port,
        .cacert  = client->cacert,
        .cert_bundle_attach = client->cert_bundle_attach,
    };

    return atop_upgrade_status_update(client->pal, &req);
}

void tuya_iot_ota_upgrade_info_free(iot_client_t *client, tuya_iot_ota_upgrade_info_t *info)
{
    if (client == NULL || info == NULL) {
        return;
    }
    const pal_t *pal = client->pal;
    if (info->version) pal->free(info->version);
    if (info->url)     pal->free(info->url);
    if (info->md5)     pal->free(info->md5);
    if (info->hmac)    pal->free(info->hmac);
    memset(info, 0, sizeof(tuya_iot_ota_upgrade_info_t));
}

int tuya_iot_ota_report_progress(iot_client_t *client, int channel,
                                 int percent)
{
    if (client == NULL || percent < 0 || percent > 100) {
        return OPRT_INVALID_PARAMETER;
    }

    /* 外层 protocol/t 与内层 data 都由本端拼(TuyaOpen 是 protocol_data_publish
     * 包装外层,这里 publish 是透传加密,故带上全帧)。progress 为字符串。*/
    char json[96];
    int len = snprintf(json, sizeof(json),
                       "{\"protocol\":16,\"t\":%u,\"data\":"
                       "{\"progress\":\"%d\",\"firmwareType\":%d}}",
                       (unsigned)(uint32_t)time(NULL), percent, channel);
    if (len <= 0 || (size_t)len >= sizeof(json)) {
        return OPRT_INVALID_PARAMETER;
    }

    int rt = iot_client_publish(client, (const uint8_t *)json, (size_t)len);
    if (rt != OPRT_OK) {
        log_warn("ota progress publish fail (rt=%d, percent=%d ch=%d)",
                 rt, percent, channel);
    }
    return rt;
}
