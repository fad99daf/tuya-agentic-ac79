/* libopus 1.4 嵌入式配置 —— 上游 ./configure 的手工等价物(杰理 pi32v2 / clang 4.0.1)。
 * 编译期经 -DHAVE_CONFIG_H + -I libopus 根目录 生效(上游惯例,仅 libopus 源码读取)。
 *
 * ▶ FIXED_POINT:全整数运算。教训:上一版本地 libopus【浮点解码】在 silk_Decode 内
 *   触发确定性 axi_wr_inv 崩溃(疑 clang 4.0.1 + LTO 误编译浮点路径),编码器走定点规避。
 * ▶ VAR_ARRAYS:用 C99 变长数组做临时缓冲(免 alloca/非线程安全伪栈)。
 * ▶ 无 RTCD/无 SIMD/无线程:纯标量 C,单线程使用(demo 任务独占,无需锁)。*/
#ifndef TUYA_LIBOPUS_CONFIG_H
#define TUYA_LIBOPUS_CONFIG_H

#define OPUS_BUILD 1           /* 必须定义为 1:opus_types.h/opus_defines.h 据此启用实现 */
#define FIXED_POINT 1          /* 定点数(silk/celt 全 int 路径) */
#define VAR_ARRAYS 1           /* VLA 临时缓冲 */
#define OPUS_VERSION "1.4-tuya-fixed"

/* 注意:不要在这里定义 FLOAT_OPUS/CUSTOM_MODES/OPUS_HAVE_RTCD 等任何 SIMD/RTCD 宏,
 * pi32v2 无对应 intrinsics,上游 x86/arm 目录已整体删除。*/

#endif /* TUYA_LIBOPUS_CONFIG_H */
