#ifndef __STDBOOL_H
#define __STDBOOL_H
/*
 * AC79 专用 stdbool.h(放在最高优先级 -I 目录,覆盖系统 stdbool.h)。
 *
 * 为什么不用系统的:JL clang 4.0.1 自带的 stdbool.h 会 #define bool _Bool
 * (且无 __bool_true_false_are_defined 内层守卫);而 AC79 cpu.h 是
 * `typedef unsigned char bool`。两者同处一个翻译单元 = "cannot combine
 * with previous 'char'" 错误。
 *
 * 这里改成 typedef(与 AC79 cpu.h 完全一致),从根本上消除 _Bool 宏冲突。
 * 同类型 typedef 重复定义,clang 作为扩展允许,且 Makefile 已 -w。
 *
 * C++ 下 bool/true/false 是原生关键字,整段跳过。
 */
#ifdef __cplusplus
#define __bool_true_false_are_defined 1
#else
#ifndef _AC79_BOOL_TYPEDEFED
#define _AC79_BOOL_TYPEDEFED 1
typedef unsigned char bool;
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#define __bool_true_false_are_defined 1
#endif

#endif /* __STDBOOL_H */
