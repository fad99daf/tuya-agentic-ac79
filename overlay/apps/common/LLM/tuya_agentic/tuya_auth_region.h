/* tuya_auth_region.h — 涂鸦三元组量产授权区(USER 保留区 @flash 0x5FE000, len 0x1000)
 *
 * 96B 定长结构体,区域起始裸放(2026-09-28 首烧实测:内容在区偏移 +0,无 0x20 文件头;
 * flen(fp) 报 0x1000 区长,内容长度以 head_size 为准)。
 * CRC16 = CRC16-CCITT-FALSE(poly 0x1021, init 0xFFFF, 无反射无异或,
 * 校验向量 crc16("123456789")=0x29B1),覆盖字节 [0..93],
 * 与测试bin(cpu/wl82/tools/tuya_auth/auth_test.bin)及产线脚本三方约定(方案§4/§6.2)。
 *
 * STRICT 策略(方案§5.3,消费点=首次配网;已激活设备走 VM 直连不受影响):
 *   0 = 开发:授权区无效回退 tuya_agentic_demo.c 顶部默认宏,现有开发板行为不变;
 *   1 = 量产:授权区无效禁止进配网/激活,杜绝漏烧码设备出厂。
 */
#ifndef __TUYA_AUTH_REGION_H__
#define __TUYA_AUTH_REGION_H__

#include "typedef.h"

#define TUYA_AUTH_STRUCT_VER   1
#define TUYA_AUTH_STRUCT_SIZE  96

/* 量产固件置 1(方案§8.3);也可在 app_config.h 里 define 覆盖 */
#ifndef TUYA_AUTH_REGION_STRICT
#define TUYA_AUTH_REGION_STRICT  0
#endif

typedef struct {
    u8   magic[8];        /* "TUYAAUTH" */
    u16  version;         /* TUYA_AUTH_STRUCT_VER */
    u16  head_size;       /* TUYA_AUTH_STRUCT_SIZE */
    char product_key[17]; /* 16字符+'\0' */
    char uuid[21];        /* 20字符+'\0' */
    char auth_key[33];    /* 32字符+'\0' */
    u8   reserved[11];    /* 预留(SN/区域等扩展),写号时清零 */
    u16  crc16;           /* 覆盖 [0..93] */
} tuya_auth_region_t;

/* 编译期锁死 96B 布局(加域必须动 reserved,不许破坏三方契约) */
typedef char tuya_auth_region_size_chk[(sizeof(tuya_auth_region_t) == TUYA_AUTH_STRUCT_SIZE) ? 1 : -1];

/* 读+整包校验(magic/version/head_size/域内NUL/CRC16)。成功 0,失败 -1(日志含原因)。 */
int tuya_auth_region_load(tuya_auth_region_t *out);

/* 写号:只取 in 的三个字符串域;magic/version/head_size/reserved/crc16 由本函数
 * 规范化重填(防调用方拼错)。流程=擦4K→写96B→回读逐字节比对。成功 0。 */
int tuya_auth_region_write(const tuya_auth_region_t *in);

/* 擦整个授权区(重写号/返修前)。成功 0。 */
int tuya_auth_region_erase(void);

/* 串口命令行入口(transport 未接,产线/调试通道一行喂进来即可,方案§6.2 通道b):
 *   auth show | auth write <pk> <uuid> <authkey> | auth erase
 * 返回 0=本模块命令且成功, -1=本模块命令但失败, 1=不是本模块命令(调用方继续)。 */
int tuya_auth_region_serial_line(const char *line);

/* 诊断:区地址/头16B(阶段1探测,保留)。 */
void tuya_auth_region_probe(void);

#endif /* __TUYA_AUTH_REGION_H__ */
