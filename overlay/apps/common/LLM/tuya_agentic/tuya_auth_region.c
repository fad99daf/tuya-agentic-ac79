/* tuya_auth_region.c — 涂鸦三元组量产授权区(独立flash保留区 USER @flash 0x5FE000, 4K)
 *
 * 阶段2(2026-09-28): load/write/erase + magic/CRC16-CCITT-FALSE 整包校验 + STRICT
 * 消费策略载体 + 串口命令解析入口(transport 未接,见 tuya_auth_region_serial_line)。
 * 阶段1的按名探测保留为 tuya_auth_region_probe()。
 *
 * 首烧实测结论(2026-09-28,离线镜像+板端双向确认):
 *   - 96B 结构体在区偏移 +0 裸放,无 0x20 文件头(AUPACKRES 那个头是 packres 私有格式);
 *   - flen(fp)=0x1000 报的是区长,内容长度以结构体 head_size 为准;
 *   - isd_download 普通烧录即把 USER_FILE 写入空白区(不叠写非空区)。
 *
 * 地址域契约(9/17 写入bug根因,铁律):
 *   fget_attrs 的 attr.sclust 是 CPU 映射地址(实测 0x025F9FE0,超出8MB物理容量)。
 *   ① 喂 sdfile_reserve_zone_read/write/erase: sclust 原样直传(debug.c/aisp.c 同款);
 *   ② 与烧写器钉死的 flash 原始地址比对/打印: 必须先 sdfile_cpu_addr2flash_addr()
 *      (flash_user 示例/custom_cfg.c 等SDK调用点同款)。
 *
 * 注意:本文件不得 #include <stdio.h> —— fs/fs.h 的 fseek/ftell/fgetc 返回值声明与
 * newlib stdio.h 冲突(demo.c 已含 stdio.h,故本模块与其分开;printf 声明经 fs.h 引入链可得)。
 */
#include "app_config.h"
#include "fs/fs.h"
#include <string.h>
#include "tuya_auth_region.h"

#define TUYA_AUTH_USER_PATH        "mnt/sdfile/EXT_RESERVED/user"
#define TUYA_AUTH_USER_ADR_EXPECT  0x5FE000  /* 与 isd_config_rule.c USER_ADR 三方契约;运行期寻址按名,不落死地址 */
#define TUYA_AUTH_ZONE_LEN         0x1000    /* USER_LEN,擦除按整区 */

/* CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, 无输入/输出反射, 无异或输出。
 * 校验向量: crc16("123456789",9)=0x29B1;auth_test.bin 实测 CRC=0x7081(小端落盘,
 * 字节94/95=81/70,勿按字节序倒读成0x8170);与产线脚本三方约定一致(方案§6.2)。 */
static u16 tuya_auth_crc16(const u8 *p, u32 len)
{
    u16 crc = 0xFFFF;
    for (u32 i = 0; i < len; i++) {
        crc ^= (u16)((u16)p[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
        }
    }
    return crc;
}

/* 域合法 = 域内有 NUL、非空串、非擦除态(0xFF 开头) */
static int auth_field_ok(const char *s, int cap)
{
    for (int i = 0; i < cap; i++) {
        if (s[i] == 0) {
            return (i > 0 && (u8)s[0] != 0xFF);
        }
    }
    return 0;   /* 域内无 NUL:越界串或全FF未擦净 */
}

/* 打开授权区并核对钉死地址(三方契约自检)。
 * 成功返回 0,*sclust_out 即喂 reserve_zone_* 的 CPU 域地址(原样直传,不换算);
 * 调用方负责 fclose。 */
static int auth_zone_open(FILE **fp, u32 *sclust_out)
{
    *fp = fopen(TUYA_AUTH_USER_PATH, "r");
    if (!*fp) {
        printf("[TUYA_AUTH] fopen(%s) fail: partition table has no such zone?\r\n",
               TUYA_AUTH_USER_PATH);
        return -1;
    }
    struct vfs_attr attr;
    fget_attrs(*fp, &attr);
    u32 flash_adr = sdfile_cpu_addr2flash_addr(attr.sclust);
    if (flash_adr != TUYA_AUTH_USER_ADR_EXPECT) {
        printf("[TUYA_AUTH] !! zone flash=0x%08x != expect 0x%08x (image/partition mismatch)\r\n",
               flash_adr, (u32)TUYA_AUTH_USER_ADR_EXPECT);
        fclose(*fp);
        return -1;
    }
    *sclust_out = attr.sclust;
    return 0;
}

void tuya_auth_region_probe(void)
{
    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return;
    }
    u8 head[16] = {0};
    sdfile_reserve_zone_read(head, sclust, sizeof(head), 0);
    printf("[TUYA_AUTH] zone sclust=0x%08x flash=0x%08x len=0x1000 head16:",
           sclust, (u32)TUYA_AUTH_USER_ADR_EXPECT);
    for (u32 i = 0; i < sizeof(head); i++) {
        printf(" %02X", head[i]);
    }
    printf("\r\n");
    fclose(fp);
}

int tuya_auth_region_load(tuya_auth_region_t *out)
{
    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sdfile_reserve_zone_read(out, sclust, sizeof(*out), 0);   /* 内容在区偏移+0(实测) */
    fclose(fp);

    if (memcmp(out->magic, "TUYAAUTH", 8) != 0) {
        if (out->magic[0] == 0xFF) {
            printf("[TUYA_AUTH] load: zone empty (erased)\r\n");
        } else {
            printf("[TUYA_AUTH] load: bad magic (zone neither auth nor erased-clean)\r\n");
        }
        return -1;
    }
    if (out->version != TUYA_AUTH_STRUCT_VER || out->head_size != TUYA_AUTH_STRUCT_SIZE) {
        printf("[TUYA_AUTH] load: ver/size mismatch ver=%u size=%u\r\n",
               out->version, out->head_size);
        return -1;
    }
    if (!auth_field_ok(out->product_key, sizeof(out->product_key)) ||
        !auth_field_ok(out->uuid, sizeof(out->uuid)) ||
        !auth_field_ok(out->auth_key, sizeof(out->auth_key))) {
        printf("[TUYA_AUTH] load: string field malformed\r\n");
        return -1;
    }
    u16 crc = tuya_auth_crc16((const u8 *)out, TUYA_AUTH_STRUCT_SIZE - 2);
    if (crc != out->crc16) {
        printf("[TUYA_AUTH] load: CRC mismatch calc=0x%04x stored=0x%04x\r\n",
               crc, out->crc16);
        return -1;
    }
    /* 脱敏(方案§5.4):pk 产品级全打;uuid 只打前 8;auth_key 永不打印 */
    char uprefix[9];
    memcpy(uprefix, out->uuid, 8);
    uprefix[8] = 0;
    printf("[TUYA_AUTH] load OK: pk=%s uuid=%s... key_len=%d crc=0x%04x\r\n",
           out->product_key, uprefix, (int)strlen(out->auth_key), out->crc16);
    return 0;
}

int tuya_auth_region_write(const tuya_auth_region_t *in)
{
    /* 头域/CRC 一律本函数规范化重填,调用方只负责三个字符串域 */
    tuya_auth_region_t t;
    memset(&t, 0, sizeof(t));
    strncpy(t.product_key, in->product_key, sizeof(t.product_key) - 1);
    strncpy(t.uuid,        in->uuid,        sizeof(t.uuid) - 1);
    strncpy(t.auth_key,    in->auth_key,    sizeof(t.auth_key) - 1);
    if (!auth_field_ok(t.product_key, sizeof(t.product_key)) ||
        !auth_field_ok(t.uuid, sizeof(t.uuid)) ||
        !auth_field_ok(t.auth_key, sizeof(t.auth_key))) {
        printf("[TUYA_AUTH] write: reject - need non-empty NUL-free pk<=16 uuid<=20 key<=32 chars\r\n");
        return -1;
    }
    memcpy(t.magic, "TUYAAUTH", 8);
    t.version   = TUYA_AUTH_STRUCT_VER;
    t.head_size = TUYA_AUTH_STRUCT_SIZE;
    t.crc16     = tuya_auth_crc16((const u8 *)&t, TUYA_AUTH_STRUCT_SIZE - 2);

    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return -1;
    }
    /* 先擦后写(9/17 教训:对已编程扇区不擦直接叠写,位会写坏) */
    int e = sdfile_reserve_zone_erase(sclust, TUYA_AUTH_ZONE_LEN, 0);
    if (e != 0 && e != TUYA_AUTH_ZONE_LEN) {
        fclose(fp);
        printf("[TUYA_AUTH] write: erase fail ret=%d\r\n", e);
        return -1;
    }
    int w = sdfile_reserve_zone_write(&t, sclust, sizeof(t), 0);
    if (w != 0 && w != (int)sizeof(t)) {
        fclose(fp);
        printf("[TUYA_AUTH] write: program fail ret=%d\r\n", w);
        return -1;
    }
    tuya_auth_region_t back;
    memset(&back, 0, sizeof(back));
    sdfile_reserve_zone_read(&back, sclust, sizeof(back), 0);
    fclose(fp);
    if (memcmp(&back, &t, sizeof(t)) != 0) {
        printf("[TUYA_AUTH] write: READBACK MISMATCH (96B @0x5FE000)\r\n");
        return -1;
    }
    char uprefix[9];
    memcpy(uprefix, t.uuid, 8);
    uprefix[8] = 0;
    printf("[TUYA_AUTH] write OK: uuid=%s... crc=0x%04x (erase+program+readback pass)\r\n",
           uprefix, t.crc16);
    return 0;
}

int tuya_auth_region_erase(void)
{
    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return -1;
    }
    int e = sdfile_reserve_zone_erase(sclust, TUYA_AUTH_ZONE_LEN, 0);
    u8 head[16] = {0};
    sdfile_reserve_zone_read(head, sclust, sizeof(head), 0);
    fclose(fp);
    if (e != 0 && e != TUYA_AUTH_ZONE_LEN) {
        printf("[TUYA_AUTH] erase fail ret=%d\r\n", e);
        return -1;
    }
    for (u32 i = 0; i < sizeof(head); i++) {
        if (head[i] != 0xFF) {
            printf("[TUYA_AUTH] erase: readback not erased (head[%u]=0x%02x)\r\n", i, head[i]);
            return -1;
        }
    }
    printf("[TUYA_AUTH] erase OK (4K @0x5FE000)\r\n");
    return 0;
}

/* ---- 串口命令解析(transport 未接) ------------------------------------------
 * 产线/调试通道把一行文本喂进来即可(UART 选型随产线工装定,当前挂起,方案§6.2 通道b)。
 */
static int auth_tok_next(const char **pp, char *out, int cap)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    const char *s = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    int n = (int)(p - s);
    if (n == 0 || n >= cap) {
        *pp = p;
        return 0;
    }
    memcpy(out, s, n);
    out[n] = 0;
    *pp = p;
    return 1;
}

int tuya_auth_region_serial_line(const char *line)
{
    char head[8], cmd[8];
    const char *p = line;
    if (!auth_tok_next(&p, head, sizeof(head)) || strcmp(head, "auth") != 0) {
        return 1;   /* 不是本模块命令 */
    }
    if (!auth_tok_next(&p, cmd, sizeof(cmd))) {
        printf("[TUYA_AUTH] usage: auth show | auth write <pk> <uuid> <authkey> | auth erase\r\n");
        return -1;
    }
    if (strcmp(cmd, "show") == 0) {
        tuya_auth_region_t t;
        if (tuya_auth_region_load(&t) == 0) {
            return 0;
        }
        tuya_auth_region_probe();   /* 失败时补一行区诊断 */
        return -1;
    }
    if (strcmp(cmd, "erase") == 0) {
        return (tuya_auth_region_erase() == 0) ? 0 : -1;
    }
    if (strcmp(cmd, "write") == 0) {
        char pk[20], uuid[24], key[36];
        tuya_auth_region_t t;
        memset(&t, 0, sizeof(t));
        if (!auth_tok_next(&p, pk, sizeof(pk)) ||
            !auth_tok_next(&p, uuid, sizeof(uuid)) ||
            !auth_tok_next(&p, key, sizeof(key))) {
            printf("[TUYA_AUTH] usage: auth write <pk> <uuid> <authkey>\r\n");
            return -1;
        }
        strncpy(t.product_key, pk, sizeof(t.product_key) - 1);
        strncpy(t.uuid,        uuid, sizeof(t.uuid) - 1);
        strncpy(t.auth_key,    key, sizeof(t.auth_key) - 1);
        return (tuya_auth_region_write(&t) == 0) ? 0 : -1;
    }
    printf("[TUYA_AUTH] unknown subcmd '%s'\r\n", cmd);
    return -1;
}
