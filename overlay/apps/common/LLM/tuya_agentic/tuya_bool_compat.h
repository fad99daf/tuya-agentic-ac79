#ifndef TUYA_BOOL_COMPAT_H
#define TUYA_BOOL_COMPAT_H

/*
 * 强制包含头(-include),解决 agentic-kit 与 AC79 的 bool 冲突。
 *
 * 冲突:系统 stdbool.h 会 #define bool _Bool(老 clang 4.0.1 的 stdbool.h
 * 只认 __STDBOOL_H 外层守卫,没有 __bool_true_false_are_defined 内层守卫);
 * 而 AC79 cpu.h 是 typedef unsigned char bool。两者在同一翻译单元里 = error。
 *
 * 这里:
 *   1) 先定义 __STDBOOL_H,让后续任何 <stdbool.h> 整体跳过(不再 #define bool _Bool);
 *   2) C 模式下提供 AC79 同款 bool/true/false(unsigned char)。
 * 与 cpu.h 的 typedef unsigned char bool 是"同类型重复",clang 作为扩展允许 + 已 -w。
 * C++ 模式下 bool/true/false 是原生关键字,整段跳过。
 */
#ifndef __STDBOOL_H
#define __STDBOOL_H 1
#endif

#ifndef __cplusplus
typedef unsigned char bool;
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#endif

#endif /* TUYA_BOOL_COMPAT_H */
