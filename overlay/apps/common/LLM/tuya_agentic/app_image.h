/*
 * app_image.h -- App 图片下发(面板"打印图片")接收链,AC791N 移植。
 *
 * 参考实现:agentic-kit-solution solutions/printer/main/app_image.c(ESP32-S3,
 * 板测收图成功)。规格照搬:thing.tc.init 注册 tc 通道 → 云端推协议64
 * fileStorageDl 事件 → 下载 COS/S3/Azure 密文分片 → tckey AES-128-CBC 解密
 * → 协议19 回执。本模块只负责"收到图",打印流水线不在此(测试阶段未实现)。
 *
 * 线程模型(与参考实现的差异):
 *   参考实现自持 MQTT 线程并用全局互斥锁包死所有 iot_client 调用;本侧
 *   MQTT 循环归 tuya_mqtt_ka 线程(coreMQTT 非线程安全),故协议19回执不走
 *   直接 publish,而是入队、由 tuya_agentic_demo 的保活循环调
 *   app_image_poll_publish() 在同一线程串行发布。
 */
#ifndef __TUYA_APP_IMAGE_H__
#define __TUYA_APP_IMAGE_H__

#include <stdint.h>
#include <stddef.h>

#include "iot_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 图片收完回调:跑在 app_image 下载线程;jpeg 指针仅在回调期间有效,
 * 需要保留请拷贝(返回后由本模块释放)。 */
typedef void (*app_image_ready_cb_t)(const uint8_t *jpeg, size_t len, void *ud);

/* 启动图片下发链(幂等):fork 引导线程做 thing.tc.init,然后起事件线程。
 * 在激活完成(iot_client 实例可用)之后调用;与 MQTT 连接状态无关。 */
int app_image_start(iot_client_t *iot, app_image_ready_cb_t cb, void *ud);

/* MQTT 下行入口:挂在 iot_client 的 message_callback 链上(收到的是解密后
 * 明文)。只筛 protocol==64 && reqType==fileStorageDl,其余静默忽略;
 * 命中即拷贝入队,回调内不做重活(iot_client.h 契约)。 */
void app_image_mqtt_message(const char *topic, size_t topic_len,
                            const uint8_t *data, size_t data_len);

/* 协议19回执发布泵:非阻塞,须在持有 MQTT 客户端的线程
 * (tuya_mqtt_ka 的 iot_client_process 循环)里周期调用。 */
void app_image_poll_publish(iot_client_t *iot);

#ifdef __cplusplus
}
#endif

#endif /* __TUYA_APP_IMAGE_H__ */
