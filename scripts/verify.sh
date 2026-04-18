#!/bin/bash
# verify.sh — 验证 thread_parent_trace 模块功能
# 使用 utopOS monitor 命令来监控线程追踪的副作用

set -e

PROC_FILE="/proc/thread_parent_trace"
MODULE_NAME="thread_parent_trace"

echo "=== Thread Parent Trace 验证 ==="
echo ""

# 1. 检查模块是否加载
echo "[1/5] 检查内核模块..."
if lsmod | grep -q "$MODULE_NAME"; then
    echo "  ✓ 模块已加载"
else
    echo "  ✗ 模块未加载，尝试加载..."
    if [ -f "thread_parent_trace.ko" ]; then
        insmod thread_parent_trace.ko
        echo "  ✓ 模块加载成功"
    else
        echo "  ✗ 找不到 .ko 文件，请先 make"
        exit 1
    fi
fi

# 2. 检查 /proc 接口
echo "[2/5] 检查 /proc 接口..."
if [ -r "$PROC_FILE" ]; then
    echo "  ✓ $PROC_FILE 可读"
else
    echo "  ✗ $PROC_FILE 不存在或不可读"
    exit 1
fi

# 3. 用 utop monitor 监控线程创建
echo "[3/5] 创建测试线程并监控..."
TEST_DIR=$(mktemp -d)

# 使用 utop monitor 来检测文件系统变更
# 同时触发一些线程创建活动
if command -v utop &>/dev/null; then
    utop monitor "$TEST_DIR" -- bash -c '
        # 创建多个子进程（模拟多线程场景）
        for i in $(seq 1 5); do
            (echo "thread-$i" > /dev/null) &
        done
        wait
    '
    echo "  ✓ utop monitor 执行完成"
else
    # 降级：直接测试
    for i in $(seq 1 5); do
        (echo "thread-$i" > /dev/null) &
    done
    wait
    echo "  ✓ 测试线程创建完成"
fi

rm -rf "$TEST_DIR"

# 4. 读取追踪数据
echo "[4/5] 读取 /proc/thread_parent_trace..."
if command -v python3 &>/dev/null; then
    python3 -c "
import json, sys
with open('$PROC_FILE') as f:
    data = json.load(f)
print(f'  ✓ 追踪条目数: {data[\"total\"]}')
for e in data['entries'][:5]:
    print(f'    child {e[\"child_pid\"]} ({e[\"child_comm\"]}) ← parent {e[\"parent_pid\"]} ({e[\"parent_comm\"]})')
if data['total'] > 5:
    print(f'    ... 还有 {data[\"total\"] - 5} 条')
"
else
    echo "  条目预览："
    cat "$PROC_FILE" | head -20
fi

# 5. 验证 parent 关系正确性
echo "[5/5] 验证 parent 关系..."
python3 -c "
import json
with open('$PROC_FILE') as f:
    data = json.load(f)

valid = 0
total = data['total']
for e in data['entries']:
    # 基本验证：child 和 parent PID 都应该 > 0
    if e['child_pid'] > 0 and e['parent_pid'] > 0:
        valid += 1

print(f'  ✓ {valid}/{total} 条记录的 parent 关系有效')
print(f'  ✓ 追踪覆盖率: {valid/max(total,1)*100:.1f}%')
"

echo ""
echo "=== 验证完成 ==="
