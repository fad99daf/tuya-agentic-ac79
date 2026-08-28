#!/bin/bash
# build_tuya_libopus.sh — 把 libopus 1.4(定点编码器子集)预编译成符号隔离的私有静态库。
#
# 为什么:杰理闭源 lib_opus_enc.a / lib_opus_stenc.a / lib_opus_dec.a 也是 libopus
# 改的,与源码直编的 libopus 有 141/74/137 个同名全局符号(连 ec_enc_*/silk_* 表
# 都同名),混链必报 multiple definition。解法:本脚本两遍编译——第一遍收集全部
# 全局符号,生成 rename.h(#define sym topus_sym),第二遍以 -include rename.h
# 重编(定义与引用同步改名,libc 名不动),打包成 libopus_tuya.a。
# tuya_opus_enc.c 里用 #define 把用到的 API 映射到 topus_ 前缀名。
#
# 何时重跑:修改 libopus/ 下任何 .c/.h 或本脚本后,重跑本脚本再重编工程。
# (产物是 LTO bitcode 归档,与工程其余对象同样走 pi32v2-lto-wrapper 链接:
# 普通 ELF 对象会被长跳转搬迁 R_PI32V2_LONG_JUMP_23M2 卡死,2026-08-28 实测)
# ⚠ 本机 JL 工具链(llvm-nm 等)不支持含中文的绝对路径:所有工具调用均先 cd 进
#   工作目录再用相对文件名。工作目录放在本目录 .build_tmp(可手动删,重建自动清理)。
#
# 许可:libopus 按 BSD-3-Clause 授权(见同目录 COPYING),随二进制分发需保留声明。
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"           # .../tuya_agentic/libopus
SDK_ROOT="$(cd "$DIR/../../../../.." && pwd)"  # libopus→tuya_agentic→LLM→common→apps→SDK 根
CC=/c/JL/pi32/bin/clang.exe
NM=/c/JL/pi32/bin/llvm-nm.exe
AR=/c/JL/pi32/bin/llvm-ar.exe
OUT_A="$DIR/libopus_tuya.a"
WORK="$DIR/.build_tmp"
rm -rf "$WORK"; mkdir -p "$WORK"
W=$(cd "$WORK" && pwd -W)                       # Windows 风格,给 clang -I 用

COMMON="--target=pi32v2 -mcpu=r3 -mfprev1 -integrated-as -Oz -flto \
-ffunction-sections -fdata-sections -femulated-tls -fms-extensions \
-w -fno-unwind-tables -DHAVE_CONFIG_H \
-I$(cygpath -m "$DIR") -I$(cygpath -m "$DIR/include") -I$(cygpath -m "$DIR/celt") \
-I$(cygpath -m "$DIR/silk") -I$(cygpath -m "$DIR/silk/fixed") \
-I$(cygpath -m "$SDK_ROOT/include_lib/newlib/include")"

cd "$DIR"
FILES=$(find . -name '*.c' | sed 's|^\./||' | LC_ALL=C sort)

# ---- 第一遍:无重命名编译,收集全局符号 ----
i=0
for f in $FILES; do
    i=$((i+1))
    "$CC" $COMMON -c "$f" -o "$WORK/p1_$i.o"
done
echo "pass1: compiled $i objects"

cd "$WORK"
: > all_syms.txt
for o in p1_*.o; do
    "$NM" --defined-only "$o" | awk '$1!=""&&$2~/^[A-TV-Z]$/&&NF>=3{print $3}' >> all_syms.txt
done
sort -u all_syms.txt > syms.txt
NS=$(wc -l < syms.txt)

# rename.h:全部全局符号加前缀(排除 C 关键字不可能撞——全是 opus/silk/celt 名)
awk '{print "#define "$1" topus_"$1}' syms.txt > rename.h
echo "renaming $NS global symbols (topus_ prefix)"

# ---- 第二遍:带 rename.h 重编 ----
i=0
for f in $FILES; do
    i=$((i+1))
    (cd "$DIR" && "$CC" $COMMON -include "$W/rename.h" -c "$f" -o "$WORK/p2_$i.o")
done
echo "pass2: compiled $i objects"

cd "$WORK"
rm -f "$OUT_A"
"$AR" rcs "$(cygpath -m "$OUT_A")" p2_*.o

# ---- 自检(回到 .a 所在目录再 nm,相对路径避开中文路径工具坑) ----
cd "$DIR"
"$NM" --defined-only "$(basename "$OUT_A")" | awk '$1!=""&&$2~/^[A-TV-Z]$/&&NF>=3{print $3}' | sort -u > "$WORK/out_syms.txt"
BAD=$(grep -vc '^topus_' "$WORK/out_syms.txt" || true)
echo "archive globals: $(wc -l < "$WORK/out_syms.txt"), non-prefixed: $BAD"
[ "$BAD" = "0" ] || { echo "ERROR: 有符号未加前缀"; exit 1; }

SDK="$SDK_ROOT/cpu/wl82/liba"
for j in lib_opus_enc lib_opus_stenc lib_opus_dec; do
    [ -f "$SDK/$j.a" ] || continue
    (cd "$SDK" && "$NM" --defined-only "$j.a" 2>/dev/null) | awk '$1!=""&&$2~/^[A-TV-Z]$/&&NF>=3{print $3}' | sort -u > "$WORK/jl.txt"
    N=$(comm -12 "$WORK/out_syms.txt" "$WORK/jl.txt" | wc -l)
    echo "intersect with $j.a: $N"
    [ "$N" = "0" ] || { echo "ERROR: 与 $j.a 仍有同名符号"; exit 1; }
done

echo "OK: $OUT_A ($(du -h "$OUT_A" | cut -f1))"
