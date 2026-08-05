#ifndef _TUYA_OTA_H_
#define _TUYA_OTA_H_

/*
 * tuya_ota.h -- 涂鸦云 OTA(固件升级)编排层入口。
 *
 * 涂鸦 OTA 是 pull 模型:设备主动向涂鸦云(ATOP over HTTPS)查询是否有升级,
 * 拿到固件 URL 后用杰理的下载+烧写链路(get_update_data)完成升级。
 * 本文件只做"协议 → 下载 → 烧写"的串联编排,不实现协议本身(协议在涂鸦
 * agentic-kit 的 iot_ota.c)和下载烧写本身(在杰理 http_update.c / net_update.c)。
 *
 * 详见 tuya_ota.c 的流程注释。
 */

#include "iot_client.h"

/**
 * @brief 检查涂鸦云是否有新固件,有则下载烧写并触发重启。
 *
 * 必须在 iot_client 已初始化(已建好身份/MQTT)、尚未 iot_client_deinit 之前调用。
 * 典型调用点:开机连 AI 之前(此时 iot_client 活着,且 AI 会话还没起,无并发冲突)。
 *
 * @param client 已初始化的 iot_client_t(复用调用方创建的实例,本函数不负责 deinit)
 * @return 0 = 无升级 / 查询失败 / 下载烧写失败(调用方应继续走原流程连 AI);
 *         1 = 下载烧写成功(杰理 net_fclose 内部会自动重启,调用方应 return 不再连 AI)。
 *
 * 副作用:
 *   - 有升级时:播报"正在升级"提示音,上报 UPGRADING,下载烧写,上报 FINI。
 *     get_update_data 内部烧完 boot info 后会 system_reset(2s 后),函数实际不返回。
 *   - 失败时:上报 EXEC,播报"升级失败"提示音,返回 0 让调用方继续。
 */
int tuya_ota_check_and_upgrade(iot_client_t *client);

/**
 * @brief 取上报涂鸦云的"生效版本号"。
 *
 * 版本号来源优先级:
 *   1. VM 里存的"平台上次 OTA 下发过的版本号"(OTA 成功烧写前写入,VM_OPT=1 保证不丢)
 *   2. 都没有 → 源码基线 TUYA_FIRMWARE_VERSION
 *
 * 这样源码 TUYA_FIRMWARE_VERSION 永远不用改:平台每次配新升级包(如 1.0.3),
 * 设备升级完就把 1.0.3 存进 VM,重启后上报 1.0.3 == 平台 1.0.3,不再死循环。
 *
 * @return 指向静态字符串缓冲区的指针(调用方无需 free,进程级生命周期)
 */
const char *tuya_get_effective_sw_ver(void);

/**
 * @brief 把平台下发的版本号持久化到 VM(供 tuya_get_effective_sw_ver 下次开机读到)。
 *
 * 在 OTA 下载烧写【之前】调用(必须在 get_update_data 之前,否则会和 system_reset
 * 倒计时撞车,导致 VM 数据损坏)。
 *
 * @param ver 平台下发的版本号字符串(如 "1.0.3"),NULL 或空串则忽略
 */
void tuya_save_upgraded_sw_ver(const char *ver);

/**
 * @brief 清除 VM 里的升级版本号(回滚到源码基线)。
 *
 * 在 OTA 下载/烧写【失败】时调用,防止"固件没换成但 VM 版本号已更新",导致设备
 * 上报新版本号→平台不再推升级→设备永久卡在旧版本。
 */
void tuya_clear_upgraded_sw_ver(void);

#endif /* _TUYA_OTA_H_ */
