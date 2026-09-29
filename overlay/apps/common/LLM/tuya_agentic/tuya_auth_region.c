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
 *      第4参是 mode(全SDK调用点一律传0),区内偏移必须折进地址 sclust+off 传
 *      (debug.c exception_fix 同款: addr + EXCEPTION_FIX_*_OFFSET, mode=0)。
 *      9/29 实测教训:把 0x800 当偏移喂给 mode → ver 记录被编程到 +0 与授权码
 *      撞车,uuid 域回读坏两次(OTA 上下文必现,9/17 验证全过是因为只验过 +0 路径);
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

/* 全 0xFF = 擦除态(未写)。回写擦除态字节无意义,直接跳过写。 */
static int buf_all_ff(const u8 *p, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        if (p[i] != 0xFF) {
            return 0;
        }
    }
    return 1;
}

/* 回读不一致时的脱敏定位(2026-09-29 实测 sw_ver persist 失败只有一句 mismatch,
 * 无现场可查根因):① 整块是否仍为擦除态(=擦落盘了但写没落);② 字段级哪个域
 * 不一致。auth_key 只报一致与否;uuid/密钥原始字节永不落日志(方案§5.4)。*/
static void auth_readback_diag(const tuya_auth_region_t *back, const tuya_auth_region_t *exp)
{
    printf("[TUYA_AUTH] readback diag: actual is %s\r\n",
           buf_all_ff((const u8 *)back, sizeof(*back))
               ? "ALL-FF (erase landed, program lost)"
               : "programmed data (partial/corrupt)");
    printf("[TUYA_AUTH] diff fields:");
    if (memcmp(back->magic, exp->magic, sizeof(exp->magic)) != 0) {
        printf(" magic");
    }
    if (back->version != exp->version || back->head_size != exp->head_size) {
        printf(" head(ver=%u size=%u)", back->version, back->head_size);
    }
    if (memcmp(back->product_key, exp->product_key, sizeof(exp->product_key)) != 0) {
        printf(" product_key");
    }
    if (memcmp(back->uuid, exp->uuid, sizeof(exp->uuid)) != 0) {
        printf(" uuid");
    }
    if (memcmp(back->auth_key, exp->auth_key, sizeof(exp->auth_key)) != 0) {
        printf(" auth_key");
    }
    if (memcmp(back->reserved, exp->reserved, sizeof(exp->reserved)) != 0) {
        printf(" reserved");
    }
    if (back->crc16 != exp->crc16) {
        printf(" crc16");
    }
    printf("\r\n");
}

/* 单轮 擦→写→回读比对,任何一步失败即 -1(日志带 attempt 序号)。
 * auth/ver 是擦除前快照(内存里的原文),fp/sclust 由调用方持有。*/
static int auth_zone_rewrite_once(FILE *fp, u32 sclust,
                                  const tuya_auth_region_t *auth,
                                  const tuya_ver_region_t *ver,
                                  int keep_ver, int attempt)
{
    /* 先擦后写(9/17 教训:对已编程扇区不擦直接叠写,位会写坏) */
    int e = sdfile_reserve_zone_erase(sclust, TUYA_AUTH_ZONE_LEN, 0);
    if (e != 0 && e != TUYA_AUTH_ZONE_LEN) {
        printf("[TUYA_AUTH] rewrite: erase fail ret=%d (attempt %d)\r\n", e, attempt);
        return -1;
    }
    if (!buf_all_ff((const u8 *)auth, sizeof(*auth))) {
        int w = sdfile_reserve_zone_write(auth, sclust, sizeof(*auth), 0);
        if (w != 0 && w != (int)sizeof(*auth)) {
            printf("[TUYA_AUTH] rewrite: auth program fail ret=%d (attempt %d)\r\n", w, attempt);
            return -1;
        }
    }
    if (keep_ver && !buf_all_ff((const u8 *)ver, sizeof(*ver))) {
        int w = sdfile_reserve_zone_write(ver, sclust + TUYA_VER_OFF, sizeof(*ver), 0);
        if (w != 0 && w != (int)sizeof(*ver)) {
            printf("[TUYA_VER] rewrite: ver program fail ret=%d (attempt %d)\r\n", w, attempt);
            return -1;
        }
    }
    /* 回读逐字节比对(该写的必须一致,该空的必须是擦除态) */
    tuya_auth_region_t auth_back;
    tuya_ver_region_t  ver_back;
    sdfile_reserve_zone_read(&auth_back, sclust, sizeof(auth_back), 0);
    sdfile_reserve_zone_read(&ver_back,  sclust + TUYA_VER_OFF, sizeof(ver_back),  0);
    if (memcmp(&auth_back, auth, sizeof(auth_back)) != 0) {
        printf("[TUYA_AUTH] rewrite: AUTH READBACK MISMATCH (96B @0x5FE000, attempt %d)\r\n", attempt);
        auth_readback_diag(&auth_back, auth);
        return -1;
    }
    if (keep_ver) {
        if (memcmp(&ver_back, ver, sizeof(ver_back)) != 0) {
            printf("[TUYA_VER] rewrite: VER READBACK MISMATCH (@+0x800, attempt %d)\r\n", attempt);
            printf("[TUYA_VER] readback diag: back=%s back_len=%u exp_len=%u back_crc=0x%04x exp_crc=0x%04x\r\n",
                   buf_all_ff((const u8 *)&ver_back, sizeof(ver_back)) ? "ALL-FF" : "has-data",
                   ver_back.ver_len, ver->ver_len, ver_back.crc16, ver->crc16);
            return -1;
        }
    } else if (!buf_all_ff((const u8 *)&ver_back, sizeof(ver_back))) {
        printf("[TUYA_VER] rewrite: VER NOT CLEARED (attempt %d)\r\n", attempt);
        return -1;
    }
    return 0;
}

/* 整区重写(2026-09-29):授权码(96B@+0)与版本记录(30B@+0x800)共用 4K 扇区,
 * 擦除不可避免,任何一方写入都必须先把另一方原文读出来,擦后按序回写两条。
 *   auth_in/ver_in 传 NULL = 该记录保留盘上现状(原文回写,字节不变);
 *   ver_drop=1 = 丢弃版本记录(=清版本,授权码仍保留)。
 * 失败整轮重试一次(2026-09-29 实测一次回读不一致但无现场;快照在内存,重试
 * 只是再擦再写,成本低),再失败返回 -1(每步带日志)。*/
static int auth_zone_rewrite(const tuya_auth_region_t *auth_in,
                             const tuya_ver_region_t *ver_in, int ver_drop)
{
    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return -1;
    }
    /* 擦除前留存两条记录现状(快照只取一次:首轮擦除后盘上原文即不可再读) */
    tuya_auth_region_t auth_cur;
    tuya_ver_region_t  ver_cur;
    sdfile_reserve_zone_read(&auth_cur, sclust, sizeof(auth_cur), 0);
    sdfile_reserve_zone_read(&ver_cur,  sclust + TUYA_VER_OFF, sizeof(ver_cur),  0);

    const tuya_auth_region_t *auth = auth_in ? auth_in : &auth_cur;
    /* 版本记录去留:显式 drop 不写;调用方给了新记录写新的;
     * 没给则保留原记录(原记录本就为空 = 不写,等价无记录)。*/
    int keep_ver = !ver_drop &&
                   !(ver_in == NULL && buf_all_ff((const u8 *)&ver_cur, sizeof(ver_cur)));
    const tuya_ver_region_t *ver = ver_in ? ver_in : &ver_cur;

    if (auth_zone_rewrite_once(fp, sclust, auth, ver, keep_ver, 1) != 0) {
        printf("[TUYA_AUTH] rewrite: attempt 1 fail, retry once\r\n");
        if (auth_zone_rewrite_once(fp, sclust, auth, ver, keep_ver, 2) != 0) {
            fclose(fp);
            return -1;
        }
        printf("[TUYA_AUTH] rewrite: pass on retry (attempt 2)\r\n");
    }
    fclose(fp);
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

    /* 整区重写:授权码写新值,区内版本记录(@+0x800)原文保留(2026-09-29 起共存) */
    if (auth_zone_rewrite(&t, NULL, 0) != 0) {
        return -1;   /* 失败日志由 auth_zone_rewrite 打 */
    }
    char uprefix[9];
    memcpy(uprefix, t.uuid, 8);
    uprefix[8] = 0;
    printf("[TUYA_AUTH] write OK: uuid=%s... crc=0x%04x (erase+program+readback pass)\r\n",
           uprefix, t.crc16);
    return 0;
}

/* ---- 跨 OTA 版本号记录(区内偏移 +0x800,布局见 tuya_auth_region.h) -------- */

int tuya_ver_region_load(char *sw_ver, unsigned int cap)
{
    if (sw_ver == NULL || cap < sizeof(((tuya_ver_region_t *)0)->sw_ver)) {
        return -1;
    }
    FILE *fp;
    u32 sclust;
    if (auth_zone_open(&fp, &sclust) != 0) {
        return -1;
    }
    tuya_ver_region_t v;
    sdfile_reserve_zone_read(&v, sclust + TUYA_VER_OFF, sizeof(v), 0);
    fclose(fp);

    if (memcmp(v.magic, "TUYAVER", 8) != 0) {
        return -1;   /* 无记录(空区/未写过):正常状态,静默 */
    }
    if (v.version != TUYA_VER_STRUCT_VER ||
        v.ver_len == 0 || v.ver_len >= sizeof(v.sw_ver) ||
        v.sw_ver[v.ver_len] != 0) {
        printf("[TUYA_VER] load: malformed record (ver=%u len=%u)\r\n",
               v.version, v.ver_len);
        return -1;
    }
    u16 crc = tuya_auth_crc16((const u8 *)&v, TUYA_VER_STRUCT_SIZE - 2);
    if (crc != v.crc16) {
        printf("[TUYA_VER] load: CRC mismatch calc=0x%04x stored=0x%04x\r\n",
               crc, v.crc16);
        return -1;
    }
    strncpy(sw_ver, v.sw_ver, cap - 1);
    sw_ver[cap - 1] = 0;
    return 0;
}

int tuya_ver_region_save(const char *sw_ver)
{
    if (sw_ver == NULL || sw_ver[0] == 0) {
        printf("[TUYA_VER] save: reject - empty ver\r\n");
        return -1;
    }
    u32 n = strlen(sw_ver);
    if (n >= 16) {
        printf("[TUYA_VER] save: reject - ver too long (%u >= 16)\r\n", n);
        return -1;
    }
    tuya_ver_region_t v;
    memset(&v, 0, sizeof(v));
    memcpy(v.magic, "TUYAVER", 8);   /* 7字符+隐式NUL 恰好 8 字节 */
    v.version = TUYA_VER_STRUCT_VER;
    v.ver_len = (u16)n;
    strncpy(v.sw_ver, sw_ver, sizeof(v.sw_ver) - 1);
    v.crc16 = tuya_auth_crc16((const u8 *)&v, TUYA_VER_STRUCT_SIZE - 2);

    if (auth_zone_rewrite(NULL, &v, 0) != 0) {
        return -1;   /* 失败日志由 auth_zone_rewrite 打 */
    }
    printf("[TUYA_VER] save OK: ver=%s crc=0x%04x (auth record preserved)\r\n",
           v.sw_ver, v.crc16);
    return 0;
}

int tuya_ver_region_clear(void)
{
    if (auth_zone_rewrite(NULL, NULL, 1) != 0) {
        return -1;
    }
    printf("[TUYA_VER] clear OK (auth record preserved)\r\n");
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
    printf("[TUYA_AUTH] erase OK (4K @0x5FE000, auth+ver both wiped)\r\n");
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
        printf("[TUYA_AUTH] usage: auth show | auth write <pk> <uuid> <authkey> | auth erase | auth verset <ver> | auth verclr\r\n");
        return -1;
    }
    if (strcmp(cmd, "show") == 0) {
        char ver[16];
        if (tuya_ver_region_load(ver, sizeof(ver)) == 0) {
            printf("[TUYA_VER] record: %s\r\n", ver);
        } else {
            printf("[TUYA_VER] record: (none)\r\n");
        }
        tuya_auth_region_t t;
        if (tuya_auth_region_load(&t) == 0) {
            return 0;
        }
        tuya_auth_region_probe();   /* 失败时补一行区诊断 */
        return -1;
    }
    if (strcmp(cmd, "verset") == 0) {
        char ver[20];
        if (!auth_tok_next(&p, ver, sizeof(ver))) {
            printf("[TUYA_VER] usage: auth verset <ver> (e.g. auth verset 1.0.13)\r\n");
            return -1;
        }
        return (tuya_ver_region_save(ver) == 0) ? 0 : -1;
    }
    if (strcmp(cmd, "verclr") == 0) {
        return (tuya_ver_region_clear() == 0) ? 0 : -1;
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
