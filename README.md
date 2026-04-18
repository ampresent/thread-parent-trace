# Thread Parent Trace

通过 kpatch 实现的内核线程追踪特性 — 实时记录每个线程的创建者。

## 快速开始

```bash
# 方式 1：内核模块（开发测试）
cd src/ && make && sudo make install-mod
cat /proc/thread_parent_trace

# 方式 2：kpatch（生产环境，需安装 kpatch-build）
cd src/ && sudo make kpatch-build && sudo make kpatch-install

# 方式 3：utopOS 集成
utop source fetch linux
utop patch create linux --desc "thread parent tracing"
utop build run linux --patch thread-parent-trace.patch
utop install linux
```

## 输出示例

```json
{
  "entries": [
    {"child_pid": 1234, "child_comm": "worker",   "parent_pid": 1200, "parent_tgid": 1200, "parent_comm": "main",   "ts_ns": 1712345678901234567},
    {"child_pid": 1235, "child_comm": "pool-1",   "parent_pid": 1200, "parent_tgid": 1200, "parent_comm": "main",   "ts_ns": 1712345678905678901}
  ],
  "total": 2
}
```

## 实现原理

基于 **kpatch**（Kernel Live Patching）技术：
- 替换 `wake_up_new_task()` 函数，在线程创建时记录 parent → child 关系
- 数据存储在内核哈希表中，通过 `/proc/thread_parent_trace` 暴露
- 支持热加载/卸载，无需重启内核

为什么选择 kpatch？详见 [docs/DESIGN.md](docs/DESIGN.md)

## 使用 utopOS 监控

```bash
# 结合 utopOS 的文件系统监控
utop monitor /tmp -- ./my_app
# → 自动检测文件变更 + 线程创建关系
```
