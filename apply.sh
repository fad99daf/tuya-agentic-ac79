#!/usr/bin/env bash
#==============================================================================
# apply.sh — 把 tuya-agentic 对接代码合并到杰理 AC79 AIoT SDK
#
# 用法:
#   bash apply.sh <AC79_SDK根目录>
# 例:
#   bash apply.sh ../fw-AC79_AIoT_SDK
#   bash apply.sh .                 # 在 SDK 根目录里执行
#
# 作用:把 overlay/ 下的全部文件(保持 SDK 相对路径)覆盖到 SDK 根。
# 仅适用于官方 SDK tag: AC79NN_SDK_V1.2.0
#==============================================================================
set -euo pipefail

SDK="${1:-}"
if [ -z "$SDK" ]; then
  echo "用法: bash apply.sh <AC79_SDK根目录>"
  echo "例:   bash apply.sh ../fw-AC79_AIoT_SDK"
  exit 1
fi

# 解析为绝对路径
SDK="$(cd "$SDK" 2>/dev/null && pwd)" || {
  echo "错误: 目录不存在: $SDK"
  exit 1
}

# 校验是不是 AC79 SDK 根(看 apps/ 和 cpu/wl82/)
for d in apps cpu/wl82; do
  if [ ! -d "$SDK/$d" ]; then
    echo "错误: $SDK 下没有 $d/, 请确认传入的是 AC79 AIoT SDK 根目录"
    exit 1
  fi
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVER="$SCRIPT_DIR/overlay"
if [ ! -d "$OVER" ]; then
  echo "错误: 找不到 overlay/ 目录(应在 $OVER)"
  exit 1
fi

echo "==> SDK 根 : $SDK"
echo "==> overlay: $OVER"
echo "==> 开始合并(覆盖同名文件)..."

cd "$OVER"
n=0
while IFS= read -r -d '' f; do
  rel="${f#./}"
  mkdir -p "$SDK/$(dirname "$rel")"
  cp -f "$f" "$SDK/$rel"
  n=$((n+1))
done < <(find . -type f -print0)

echo "==> 完成: 合并 $n 个文件"
echo ""
echo "下一步编译:"
echo "    cd \"$SDK\""
echo "    make ac791n_wifi_story_machine"
echo ""
echo "若只想看改动(不覆盖), 可改用 patch:"
echo "    cd \"$SDK\" && git apply \"$SCRIPT_DIR/patches/tuya-agentic-v1.2.0.patch\""
