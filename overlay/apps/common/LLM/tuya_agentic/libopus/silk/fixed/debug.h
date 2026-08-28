/* 本地适配 shim(见同目录 typedef.h 注释):silk/fixed/main_FIX.h 以带引号方式
 * #include "debug.h",本目录没有同名文件时会落到 -I 链,被 lwip 的 debug.h
 * 抢先命中(lwip 头再拖进 lwipopts/generic typedefs,与 lwip/arch.h 的 ssize_t
 * typedef 相互冲突 → 编译爆炸)。转发回 silk/debug.h,对上游源码零改动。 */
#include "../debug.h"
