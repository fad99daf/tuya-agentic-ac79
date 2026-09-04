#!/bin/bash
# patch_libstm.sh — 把涂鸦给的 libstm.a(STM OPEN SDK,UDP/DTLS+TCP/TLS 双通道)
# 改造成能在本 SDK 上安全运行的 libstm_tuya.a。
#
# 为什么必须打补丁(三处,都是实测/反汇编确认的硬伤):
#
# ① 引擎线程栈只有 1KB。stm_engine.c.o 里是
#      pthread_create(&h, NULL /*attr*/, stm_engine_run_thread, arg)
#    attr=NULL → 宿主 FreeRTOS_POSIX_pthread.c 用默认值 0x80000400,
#    低 16 位 usStackSize=1024 字节 → xTaskCreate(..., 1024>>2=256 words, ...)。
#    这个线程要跑 stm_net_loop + 完整 DTLS/TLS 握手(mbedtls ssl ctx 本身
#    就 1916~1964 字节),256 words 必爆栈。参照 pal_ac791n.c 的
#    thread_fork("tuya_pal", 4, 6*1024, ...),这里给 12KB(DTLS 比 TLS 更吃栈)。
#    做法:llvm-bcrename 把 stm_engine.c.o 里【未定义引用】pthread_create
#    改名成 stm_ac79_pthread_create,由 stm_port_ac79_shim.c 提供的同名函数
#    先 pthread_attr_setstacksize 再转调真 pthread_create。
#    (已验证 bcrename 只改未定义引用,不动已定义符号。)
#
# ② O_NONBLOCK 常量错位(2026-08-31 实测 UDP 建连失败定位)。libstm 按 Linux
#    头编译,F_GETFL=3/F_SETFL=4(与 lwip 一致)但 O_NONBLOCK=0x4000;本平台
#    lwip_2_2_0 的 O_NONBLOCK=1(sockets.h:456)。stm_socket.c.o 的
#    set_nonblock 用 F_GETFL 返回值 | 0x4000 再 F_SETFL,lwip 一看有它不认识
#    的标志位就返回 ENOSYS(88),libstm 报 -12000-88=-12088,UDP/TCP 建连全挂。
#    做法:把 stm_socket.c.o 里未定义的 lwip_fcntl 改名 stm_ac79_fcntl,
#    shim 里做常量翻译后转发(库侧只会调 F_GETFL/F_SETFL 两种)。
#    其余 lwip 常量已核对无错位:SOL_SOCKET=4095/SO_BROADCAST=32/SO_ERROR=4103
#    /MSG_NOSIGNAL=0x20 均为 lwip 编号;fd_set 双方都是 7 字节字节数组
#    (本平台 FD_SETSIZE=MEMP_NUM_NETCONN=20+16+16+3=55)。
#
# ③ fflush/fprintf/fputs:本 SDK 由 apps/common/c++/cxx_runtime.cpp 提供,
#    无需处理(曾误以为缺失而在 shim 里补桩,导致与 cxx_runtime.o 链接撞符号)。
#
# 何时重跑:换了涂鸦新版 libstm.a 之后。产物 libstm_tuya.a 已入库,
# 平时编译工程不需要跑本脚本。
#
# ⚠ 本机 JL 工具链(llvm-ar/llvm-bcrename/llvm-nm)不支持含中文的路径,
#   故所有工具调用都先 cd 进工作目录再用相对文件名(工作目录在本目录下,
#   本 SDK 路径全 ASCII)。
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
AR=/c/JL/pi32/bin/llvm-ar.exe
NM=/c/JL/pi32/bin/llvm-nm.exe
BCRENAME=/c/JL/pi32/bin/llvm-bcrename.exe
IN_A="$DIR/libstm.a"
OUT_A="$DIR/libstm_tuya.a"
WORK="$DIR/.patch_tmp"

[ -f "$IN_A" ] || { echo "ERROR: 缺少原始库 $IN_A"; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
cp "$IN_A" "$WORK/orig.a"
cd "$WORK"

# ---- ① stm_engine.c.o:引擎线程栈(pthread_create → shim) ----
"$AR" x orig.a stm_engine.c.o
BEFORE=$("$NM" --undefined-only stm_engine.c.o | grep -c 'pthread_create' || true)
[ "$BEFORE" = "1" ] || { echo "ERROR: stm_engine.c.o 未引用 pthread_create(库结构变了,请复核)"; exit 1; }

"$BCRENAME" -rename-list=pthread_create:stm_ac79_pthread_create \
            -o stm_engine_patched.c.o stm_engine.c.o
AFTER=$("$NM" --undefined-only stm_engine_patched.c.o | grep -c 'stm_ac79_pthread_create' || true)
STILL=$("$NM" --undefined-only stm_engine_patched.c.o | grep -c '^ *U pthread_create$' || true)
[ "$AFTER" = "1" ] && [ "$STILL" = "0" ] \
    || { echo "ERROR: ① 重命名未生效(after=$AFTER still=$STILL)"; exit 1; }

# ---- ② stm_socket.c.o:O_NONBLOCK 常量翻译(lwip_fcntl → shim) ----
"$AR" x orig.a stm_socket.c.o
BEFORE2=$("$NM" --undefined-only stm_socket.c.o | grep -c 'lwip_fcntl' || true)
[ "$BEFORE2" = "1" ] || { echo "ERROR: stm_socket.c.o 未引用 lwip_fcntl(库结构变了,请复核)"; exit 1; }

"$BCRENAME" -rename-list=lwip_fcntl:stm_ac79_fcntl \
            -o stm_socket_patched.c.o stm_socket.c.o
AFTER2=$("$NM" --undefined-only stm_socket_patched.c.o | grep -c 'stm_ac79_fcntl' || true)
STILL2=$("$NM" --undefined-only stm_socket_patched.c.o | grep -c '^ *U lwip_fcntl$' || true)
[ "$AFTER2" = "1" ] && [ "$STILL2" = "0" ] \
    || { echo "ERROR: ② 重命名未生效(after=$AFTER2 still=$STILL2)"; exit 1; }

# ---- ③ 改过的成员替换回归档 ----
cp orig.a out.a
cp stm_engine_patched.c.o stm_engine.c.o     # 同名替换,llvm-ar r 按成员名覆盖
cp stm_socket_patched.c.o  stm_socket.c.o
"$AR" r out.a stm_engine.c.o stm_socket.c.o

# ---- 自检:产物符号状态 ----
LEFT=$("$NM" --undefined-only out.a 2>/dev/null | grep -c '^ *U pthread_create$' || true)
[ "$LEFT" = "0" ] || { echo "ERROR: 归档里仍有 $LEFT 处裸 pthread_create 引用"; exit 1; }
LEFT2=$("$NM" --undefined-only out.a 2>/dev/null | grep -c '^ *U lwip_fcntl$' || true)
[ "$LEFT2" = "0" ] || { echo "ERROR: 归档里仍有 $LEFT2 处裸 lwip_fcntl 引用"; exit 1; }
HAS=$("$NM" --undefined-only out.a 2>/dev/null | grep -c 'stm_ac79_pthread_create' || true)
[ "$HAS" = "1" ] || { echo "ERROR: 归档里没有 stm_ac79_pthread_create 引用"; exit 1; }
HAS2=$("$NM" --undefined-only out.a 2>/dev/null | grep -c 'stm_ac79_fcntl' || true)
[ "$HAS2" = "1" ] || { echo "ERROR: 归档里没有 stm_ac79_fcntl 引用"; exit 1; }

rm -f "$OUT_A"
cp out.a "$OUT_A"
cd "$DIR"
echo "OK: $(basename "$OUT_A") ($(du -h "$OUT_A" | cut -f1))"
echo "    ① 引擎线程栈: pthread_create -> stm_ac79_pthread_create (shim 设 12KB)"
echo "    ② O_NONBLOCK 翻译: lwip_fcntl -> stm_ac79_fcntl (lib 0x4000 / 平台 1)"
rm -rf "$WORK"
