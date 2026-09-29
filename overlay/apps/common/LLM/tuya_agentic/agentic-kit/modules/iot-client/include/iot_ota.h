#ifndef _IOT_OTA_H_
#define _IOT_OTA_H_

#include "iot_client.h"

/**
 * @file iot_ota.h
 * @brief Public OTA (firmware upgrade) API for the agentic-kit IoT client.
 *
 * This module provides cloud-protocol primitives for OTA:
 * - Reporting the device's current firmware version to the cloud.
 * - Checking for an available firmware upgrade.
 * - Reporting the upgrade lifecycle status (upgrading / success / failure).
 *
 * The SDK does **not** download or flash firmware. The application owns the
 * download and flash logic (e.g. ESP-IDF `esp_ota_*` or a vendor-specific
 * bootloader API).
 *
 * Typical flow:
 * ```c
 * // 1. Report current version (also done automatically in iot_client_init)
 * tuya_iot_ota_report_version(client, "1.2.3");
 *
 * // 2. Check for an upgrade (设备主动自检用 _silent 只拉静默任务;
 * //    APP 确认后拉包才用 tuya_iot_ota_check_upgrade)
 * tuya_iot_ota_upgrade_info_t info;
 * if (tuya_iot_ota_check_upgrade_silent(client, &info) == OPRT_OK && info.has_upgrade) {
 *     // 3. Signal start of upgrade
 *     tuya_iot_ota_report_status(client, 0, OTA_STATUS_UPGRADING);
 *     // 4. Download info.url and flash (application code)
 *     my_firmware_download_and_flash(info.url);
 *     // 5. On success report COMPLETE before reboot; report ERROR on failure
 * }
 * tuya_iot_ota_upgrade_info_free(&info);
 * ```
 */

#if defined(__GNUC__) && (__GNUC__ >= 4)
#define OTA_API __attribute__((visibility("default")))
#else
#define OTA_API
#endif

/* ---- Re-export common types from atop.h for public consumers ---- */

/**
 * @brief OTA upgrade status codes.
 *
 * These map to the Tuya cloud's server-side `FirmwareUpgradeStatus` definition
 * (aligned with upstream agentic-kit 02b4c2a, 2026-08-19;旧值曾把
 * UPGRADING=1/FINI=2/EXEC=3 发给云端,被云端解释为 待升级/升级中/成功——
 * 失败上报会变"成功"导致任务错误闭合,勿改回):
 * - 0: default, no upgrade needed
 * - 1: ready (upgrade task issued, device prepared)
 * - 2: upgrade in progress
 * - 3: upgrade complete
 * - 4: upgrade error / abnormal
 */
typedef enum {
    OTA_STATUS_IDLE      = 0,
    OTA_STATUS_READY     = 1,
    OTA_STATUS_UPGRADING = 2,
    OTA_STATUS_COMPLETE  = 3,
    OTA_STATUS_ERROR     = 4,
} tuya_iot_ota_status_t;

/**
 * @brief Firmware upgrade information returned by tuya_iot_ota_check_upgrade().
 *
 * String fields are heap-allocated; free with tuya_iot_ota_upgrade_info_free().
 */
typedef struct {
    bool has_upgrade;   /**< true if the cloud returned firmware info */
    char *version;      /**< new firmware version string (NULL if none) */
    char *url;          /**< download URL (httpsUrl) */
    long  file_size;    /**< firmware file size in bytes */
    int   channel;      /**< firmware type returned by cloud */
    char *md5;          /**< MD5 hash of firmware (may be NULL) */
    char *hmac;         /**< HMAC of firmware (may be NULL) */
} tuya_iot_ota_upgrade_info_t;

/**
 * @brief Report the device's current firmware version to the cloud.
 *
 * Sends the firmware version via the `tuya.device.versions.update` ATOP API.
 * This is automatically called during iot_client_init() with the SDK default
 * version; call this explicitly to report an application-specific version.
 *
 * @param client  IoT client instance (must be initialized)
 * @param sw_ver  Firmware version string (e.g. "1.2.3"); must not be NULL
 * @return OPRT_OK on success, error code on failure
 */
OTA_API int tuya_iot_ota_report_version(iot_client_t *client, const char *sw_ver);

/**
 * @brief Check for an available firmware upgrade.
 *
 * Sends the `tuya.device.upgrade.get` ATOP API. If the cloud has a pending
 * upgrade, @p info is populated with the firmware URL, version, size, and
 * hashes. 云端对本接口不做"用户已确认"过滤(2026-09-29 实测),设备主动自检
 * 请用 tuya_iot_ota_check_upgrade_silent()(只返回静默任务);本接口留给
 * APP 确认升级(MQTT protocol 15)之后的拉包路径。
 *
 * @param[in]  client   IoT client instance (must be initialized)
 * @param[in]  channel  Firmware channel (0 = main MCU firmware)
 * @param[out] info     Output: upgrade info (caller must free with
 *                      tuya_iot_ota_upgrade_info_free())
 * @return OPRT_OK on success (including no-upgrade case), error code on failure
 */
OTA_API int tuya_iot_ota_check_upgrade(iot_client_t *client, int channel,
                                  tuya_iot_ota_upgrade_info_t *info);

/**
 * @brief 设备主动自检是否有静默升级任务(开机/定时用)。
 *
 * Sends the `tuya.device.upgrade.silent.get` ATOP API(对齐 TuyaOpen
 * matop_service.c 的 auto_upgrade_info_get)。云端只从这里下发"静默升级"
 * 任务;平台配"APP提醒升级"的任务【不会】返回——须等用户在 App 点确认、
 * 云端推 MQTT protocol 15 后再调 tuya_iot_ota_check_upgrade(upgrade.get)
 * 拉包。响应结构与 check_upgrade 完全一致(通道取响应里的 type)。
 *
 * @param[in]  client   IoT client instance (must be initialized)
 * @param[out] info     Output: upgrade info (caller must free with
 *                      tuya_iot_ota_upgrade_info_free())
 * @return OPRT_OK on success (including no-upgrade case), error code on failure
 */
OTA_API int tuya_iot_ota_check_upgrade_silent(iot_client_t *client,
                                  tuya_iot_ota_upgrade_info_t *info);

/**
 * @brief Report the OTA upgrade status to the cloud.
 *
 * Sends the `tuya.device.upgrade.status.update` ATOP API. Call this to notify
 * the cloud of the upgrade lifecycle:
 * - OTA_STATUS_UPGRADING before starting the download/flash.
 * - OTA_STATUS_COMPLETE after a successful flash, before reboot.
 * - OTA_STATUS_ERROR on failure.
 *
 * @param client   IoT client instance (must be initialized)
 * @param channel  Firmware channel (0 = main MCU firmware)
 * @param status   Upgrade status to report
 * @return OPRT_OK on success, error code on failure
 */
OTA_API int tuya_iot_ota_report_status(iot_client_t *client, int channel,
                                  tuya_iot_ota_status_t status);

/**
 * @brief Free memory in an tuya_iot_ota_upgrade_info_t.
 *
 * Safe to call on a zeroed struct or after a no-upgrade result.
 *
 * @param client  IoT client instance (provides the PAL allocator)
 * @param info    Upgrade info to free (fields zeroed after free)
 */
OTA_API void tuya_iot_ota_upgrade_info_free(iot_client_t *client, tuya_iot_ota_upgrade_info_t *info);

/* ---- 升级进度上报(MQTT 协议 16 上行)----
 * 上游 agentic-kit 截止 0.5.0 无此 API,按 TuyaOpen 官方协议在 kit 层补齐
 * (TuyaOpen mqtt_service.c: PRO_UPGE_PUSH = 16, device -> cloud upgrade percent):
 *   topic   smart/device/out/{devid}(经 iot_client_publish,P2.3 加密)
 *   payload {"protocol":16,"t":<unix秒>,"data":{"progress":"%d","firmwareType":%d}}
 * 注意 progress 是【字符串】而非数字,与 TuyaOpen 逐字对齐。
 * 节流由调用方负责(官方节奏:较上次超过 5% 才报,完成强制 100)。*/

/**
 * @brief 上报 OTA 升级进度(0~100%)。
 *
 * 进度是体验性数据,不保证送达:MQTT 掉线时发布失败仅返回错误码,
 * 调用方记日志即可,不应因此中断下载。
 *
 * @param client  IoT client(必须已连 MQTT)
 * @param channel 固件通道(与升级任务一致,主固件 0)
 * @param percent 0~100
 * @return OPRT_OK 发布成功;OPRT_INVALID_PARAMETER 参数非法;其它为发布错误
 */
OTA_API int tuya_iot_ota_report_progress(iot_client_t *client, int channel,
                                         int percent);

/* ---- 流式固件摘要校验(移植自上游 agentic-kit e09d502)----
 * 下载循环逐块喂给校验器,烧启动区【之前】调 finish 比对云端摘要:
 *   hmac 优先: HMAC-SHA256(key=设备 secret_key, msg=大写hex(SHA256(固件)));
 *   无 hmac 时退化 MD5(固件);两者都没下发返回 OPRT_NOT_SUPPORTED(ctx 保持
 *   NULL,由应用决定是否照烧)。比较大小写不敏感。*/

/** @brief 摘要校验上下文(不透明;init 时内部拷贝期望摘要与 HMAC 密钥)。 */
typedef struct tuya_iot_ota_verify_ctx tuya_iot_ota_verify_ctx_t;

/**
 * @brief 创建摘要校验器。必须在 tuya_iot_ota_upgrade_info_free 之前调用。
 *
 * @return OPRT_OK 建好;OPRT_NOT_SUPPORTED 云端未下发任何摘要(ctx 保持 NULL,
 *         可跳过校验);其它错误(摘要格式非法/内存不足)必须中止升级。
 */
OTA_API int tuya_iot_ota_verify_init(iot_client_t *client,
                                     const tuya_iot_ota_upgrade_info_t *info,
                                     tuya_iot_ota_verify_ctx_t **ctx_out);

/**
 * @brief 喂入一块固件数据(必须与写入 flash 的字节严格一致)。
 * @return OPRT_OK;错误时应 abort 并中止本次升级。
 */
OTA_API int tuya_iot_ota_verify_update(tuya_iot_ota_verify_ctx_t *ctx,
                                       const uint8_t *data, size_t len);

/**
 * @brief 全部数据喂完后比对云端摘要。无论成败内部都释放 ctx,之后不得再用。
 * @return OPRT_OK 匹配;OPRT_OTA_VERIFY_FAILED 固件被篡改/损坏(必须中止,
 *         不得切换启动分区);其它错误同样中止。
 */
OTA_API int tuya_iot_ota_verify_finish(tuya_iot_ota_verify_ctx_t *ctx);

/** @brief 下载中途失败时直接释放 ctx(不比对)。NULL 安全。 */
OTA_API void tuya_iot_ota_verify_abort(tuya_iot_ota_verify_ctx_t *ctx);

#endif /* _IOT_OTA_H_ */
