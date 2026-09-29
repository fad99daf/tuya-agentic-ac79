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
 * @brief 开机自检静默升级任务,有则下载烧写并触发重启(主固件通道 0)。
 *
 * 必须在 iot_client 已初始化(已建好身份/MQTT)、尚未 iot_client_deinit 之前调用。
 * 典型调用点:开机连 AI 之前(此时 iot_client 活着,且 AI 会话还没起,无并发冲突)。
 * 走 silent.get 自检(2026-09-29 起):云端只返回"静默升级"任务,平台配
 * "APP提醒升级"的任务开机【不】拉(等用户在 App 点确认走 _channel 入口);
 * 与 tuya_ota_check_and_upgrade_channel 的区别就在查询接口不同(upgrade.get)。
 *
 * @param client 已初始化的 iot_client_t(复用调用方创建的实例,本函数不负责 deinit)
 * @return 0 = 无升级 / 查询失败 / 下载烧写失败(调用方应继续走原流程连 AI);
 *         1 = 下载烧写成功(net_fclose 内部已烧 boot info 并安排 2s 后重启,
 *             调用方应 return 不再连 AI)。
 *
 * 副作用:
 *   - 有升级时:播报"正在升级"提示音,上报 UPGRADING,下载烧写(逐块喂涂鸦
 *     摘要校验器,烧 boot info 前比对,不匹配不烧不重启),成功上报 COMPLETE。
 *   - 失败时:上报 ERROR,播报"升级失败"提示音,返回 0 让调用方继续。
 */
int tuya_ota_check_and_upgrade(iot_client_t *client);

/**
 * @brief APP 确认升级后的拉包入口(upgrade.get,按指定固件通道查询)。
 *
 * APP 确认升级流程(上游 agentic-kit eb19466):云端在用户于 App 点"确认升级"
 * 后经 MQTT protocol 15 推送通知,data.firmwareType 即固件通道。iot_client 的
 * ota_confirm_callback 跑在 MQTT 处理线程里,只置标志(见 tuya_agentic_demo.c
 * 的 on_ota_confirm);由应用 worker 线程调本函数完成 查询→下载→校验→烧写。
 * channel 0 = 主固件;与开机路径的区别:本入口走 upgrade.get(拉当前待升级
 * 任务,含提醒任务),开机路径走 silent.get(只拉静默任务)。
 *
 * @param client  已初始化的 iot_client_t(尚未 deinit)
 * @param channel 固件通道(protocol 15 的 data.firmwareType;0 = 主固件)
 * @return 同 tuya_ota_check_and_upgrade(1 = 成功待重启)。
 */
int tuya_ota_check_and_upgrade_channel(iot_client_t *client, int channel);

/**
 * @brief 取上报涂鸦云的"生效版本号"。
 *
 * 双轨方案(2026-09-29 起):优先返回 USER 保留区版本记录(tuya_ver_region_load,
 * 即上次 OTA 的云端任务版本,跨重启/跨烧录保留);无记录/损坏则回退源码宏
 * TUYA_FIRMWARE_VERSION(app_config.h)。
 * 版本比较在涂鸦云端:每次开机 iot_client_init 先用本函数的返回值调
 * tuya.device.versions.update 上报,云端拿它和平台配置的固件版本比,
 * 设备上报版本更低,tuya.device.upgrade.get 才会返回升级包 URL。
 * 发版纪律不变:出包前宏仍要改成平台版本号;但即使忘了(2026-09-29 旧宏包
 * 事故:包内宏 1.0.11 配平台 1.0.13 → 升级死循环),记录兜底——升级后上报
 * 记录值,任务照常闭合;开机日志 base=宏/effective=记录 不一致即暴露旧宏包。
 *
 * @return 指向静态字符串缓冲区的指针(调用方无需 free,进程级生命周期)
 */
const char *tuya_get_effective_sw_ver(void);

/**
 * @brief 持久化平台下发的版本号(写入 USER 保留区 @0x5FE000+0x800,断电验证链路)。
 *
 * 只应在 tuya_ota_download_and_flash 的烧录成功点调用(镜像全部落盘+摘要校验
 * 通过之后、net_fclose 之前——2s system_reset 定时器在 net_fclose 内部才拉起,
 * 此前写盘无复位撞车)。失败路径不回滚:记录保持上一次成功升级的旧值(设备
 * 真实状态)。写失败不阻断升级(日志告警,行为退回纯宏方案)。
 *
 * @param ver 平台下发的版本号字符串(如 "1.0.3"),NULL 或空串则忽略
 */
void tuya_save_upgraded_sw_ver(const char *ver);

#endif /* _TUYA_OTA_H_ */
