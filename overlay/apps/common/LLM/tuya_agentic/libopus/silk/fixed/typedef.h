/* 本地适配 shim(2026-08-27):silk/fixed/ 下的头(如 structs_FIX.h)以带引号方式
 * #include "typedef.h",本目录没有同名文件时会落到 -I 链,被杰理 SDK 的
 * include_lib/system/generic/typedef.h 抢先命中(两边内容完全不同 → 编译爆炸)。
 * 上游通过 configure 的 include 顺序保证解析到 silk/typedef.h;这里用"引号含入
 * 先搜本目录"的规则放一个转发 shim,对上游源码零改动达成同样效果。 */
#include "../typedef.h"
