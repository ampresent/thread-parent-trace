# Thread Parent Trace — 基于 kpatch 的内核线程追踪

## 一、功能概述

Thread Parent Trace 是一个内核特性，通过 **kpatch (Kernel Live Patching)** 技术，实时追踪系统中每个线程/进程的创建者（父线程）。

```
用户空间视角：

  $ cat /proc/thread_parent_trace
  {
    "entries": [
      {"child_pid": 1234, "child_comm": "worker",   "parent_pid": 1200, "parent_comm": "main",   "ts_ns": ...},
      {"child_pid": 1235, "child_comm": "pool-1",   "parent_pid": 1200, "parent_comm": "main",   "ts_ns": ...},
      {"child_pid": 1236, "child_comm": "sh",       "parent_pid": 1234, "parent_comm": "worker", "ts_ns": ...}
    ],
    "total": 3
  }
```

每个条目记录：
- `child_pid` / `child_comm` — 新创建的线程/进程
- `parent_pid` / `parent_tgid` / `parent_comm` — 创建者信息
- `ts_ns` — 创建时间戳（纳秒级）

---

## 二、实现方式：为什么是 kpatch？

### 2.1 内核线程追踪的技术选型

| 方案 | 原理 | 能否替换内核函数 | 热加载 | 侵入性 | 适用场景 |
|------|------|:---:|:---:|:---:|------|
| **kprobes** | 在函数入口插入 int3 断点 | ❌ 只能观测 | ✅ | 低 | 性能分析、采样 |
| **ftrace** | 在函数头插入 nop→call 指令 | ⚠️ 可 hook 但不能替换 | ✅ | 中 | 函数跟踪、延迟分析 |
| **eBPF** | 在内核中运行验证过的字节码 | ❌ 受 verifier 限制 | ✅ | 低 | 安全的观测和过滤 |
| **LKM (模块)** | 注册回调、拦截导出符号 | ❌ 只能 hook 导出函数 | ✅ | 中 | 设备驱动、扩展 |
| **源码补丁 + 重编译** | 直接改内核源码 | ✅ | ❌ 需重启 | 高 | 大型功能改动 |
| **kpatch ✅** | 函数级替换，差分 .ko | ✅ 替换任意函数 | ✅ | **低** | **生产环境热修复** |

### 2.2 选择 kpatch 的核心理由

#### ① 需要"修改"而非仅仅"观测" `wake_up_new_task()`

kprobes 和 eBPF 只能在函数执行时做观测（读取参数、计数等），**不能改变函数行为**。

我们的需求是：在 `wake_up_new_task()` 执行前，先记录父线程信息，**然后继续执行原始逻辑**。这需要"替换"函数——kpatch 恰好做这件事。

```
kprobe/eBPF 视角：
  wake_up_new_task()  ──→  原始函数（不变）
       │
       └──→ 你只能在旁边看（probe handler）

kpatch 视角：
  调用者 ──→ patched_wake_up_new_task()  ──→ 原始函数
                  │
                  └──→ 记录 parent 关系（修改了调用路径）
```

#### ② 无需重启内核

源码补丁方案需要重新编译内核 + 重启，这对生产环境不可接受。

kpatch 通过 `insmod` 一个很小的 .ko 模块即可生效，**秒级热加载**。

#### ③ 安全性：差分替换

kpatch 不会替换整个函数体，而是：
1. 编译原始内核源码和补丁后的源码
2. 对比两者的 ELF 目标文件
3. 只提取**被修改的指令**，生成差分 .ko
4. 加载时，用 jump label 替换原始函数入口

这意味着：
- 如果你的补丁只改了 2 行代码，.ko 只包含那 2 行的差异
- 加载失败不会影响系统（验证阶段就能发现）
- 卸载时恢复原始函数，无副作用

#### ④ 对比 ftrace

ftrace 可以 hook 函数，但它的设计目的是**跟踪**而非**替换**：

```
ftrace 方式：
  wake_up_new_task()
    ├── 原始 prologue（被改写为 nop）
    ├── ──→ ftrace_caller ──→ 你的 handler
    └── （handler 必须手动调用原始函数）

问题：
  1. 需要理解函数 prologue 的具体指令布局
  2. handler 中引用原始函数需要 fentry/fexit 技巧
  3. 不能直接"替换"函数逻辑，只能在前后插入
  4. 对内联函数无能为力
```

kpatch 不需要关心这些底层细节——它自动处理 prologue patching 和符号重定位。

#### ⑤ 对比 eBPF

eBPF 非常强大，但有明确限制：

```
eBPF 限制：
  1. verifier 禁止修改内核内存（不能写入全局数据结构）
  2. 不能动态分配 hash 表（bpf_map 只能用预定义的 BPF_MAP_TYPE）
  3. 不能替换函数逻辑，只能 kprobe/kretprobe
  4. 每次调用都有 verifier 开销
```

对于"追踪并暴露数据"这个需求，eBPF 能做一部分（读参数、计数），但 **kpatch 能做到更干净的集成**（直接在调度路径中插入逻辑）。

### 2.3 总结

```
选型决策树：

  需要观测还是修改？
    ├── 只观测 → kprobes / eBPF（轻量、安全）
    └── 要修改 → 需要重启吗？
                  ├── 可以重启 → 源码补丁 + 重编译
                  └── 不能重启 → kpatch ✅
                                  ├── 函数级替换
                                  ├── 热加载/卸载
                                  ├── 差分安全
                                  └── 生产环境验证
```

**Thread Parent Trace 选择 kpatch，因为：**
1. 需要**修改** `wake_up_new_task()` 的调用路径（不只是观测）
2. 生产环境不能重启内核
3. kpatch 的差分机制保证了补丁的**最小侵入性**
4. 可以随时卸载，恢复原始行为

---

## 三、架构设计

### 3.1 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                    内核空间                                │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │  thread_parent_trace.ko (kpatch 模块)              │  │
│  │                                                    │  │
│  │  ┌──────────────────────────┐  ┌───────────────┐  │  │
│  │  │ patched_wake_up_new_task │  │ trace_hash[]  │  │  │
│  │  │                          │  │ (哈希表)       │  │  │
│  │  │  1. 读取 p->real_parent  │  │ child→parent  │  │  │
│  │  │  2. 插入哈希表           │──│ 映射存储      │  │  │
│  │  │  3. 调用原始函数         │  │               │  │  │
│  │  └──────────┬───────────────┘  └───────┬───────┘  │  │
│  │             │                           │          │  │
│  │             ▼                           ▼          │  │
│  │  ┌──────────────────────────────────────────────┐  │  │
│  │  │        /proc/thread_parent_trace             │  │  │
│  │  │        (seq_file 接口)                        │  │  │
│  │  └────────────────────┬─────────────────────────┘  │  │
│  └───────────────────────┼────────────────────────────┘  │
└──────────────────────────┼───────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│                    用户空间                                │
│                                                          │
│   cat /proc/thread_parent_trace  →  JSON 输出            │
│                                                          │
│   utop monitor /tmp -- ./thread_test  →  侧效果报告      │
└──────────────────────────────────────────────────────────┘
```

### 3.2 关键函数

| 函数 | 职责 |
|------|------|
| `patched_wake_up_new_task()` | kpatch 替换函数，在原始逻辑前记录 parent 关系 |
| `trace_insert()` | 向哈希表插入 child→parent 映射 |
| `trace_find()` | 按 child_pid 查找 parent 信息 |
| `trace_cleanup_exited()` | 清理已退出线程的条目（惰性清理） |
| `trace_proc_show()` | /proc 文件的读取处理（JSON 格式） |

### 3.3 数据流

```
调度器调用 wake_up_new_task(rq, p)
    │
    ▼
patched_wake_up_new_task()
    │
    ├──→ RCU 读锁保护
    │      获取 p->real_parent (父 task_struct)
    │
    ├──→ trace_insert()
    │      kmalloc 分配 entry
    │      填充 child_pid, parent_pid, parent_comm, timestamp
    │      spinlock 保护 → hash_add 插入
    │
    └──→ 调用原始 wake_up_new_task()
           线程被正常唤醒，行为完全不变
```

---

## 四、使用方式

### 4.1 编译（内核模块方式）

```bash
cd src/
make                    # 编译内核模块
make install-mod        # 加载（insmod）
```

### 4.2 编译（kpatch 方式，需要 kpatch-build）

```bash
cd src/
make kpatch-build       # 生成 kpatch .ko
make kpatch-install     # 加载 kpatch 补丁
```

### 4.3 使用 utopOS 工作流

```bash
# utopOS 管理整个流程：源码 → 补丁 → 构建 → 安装 → 回滚
utop source fetch linux                    # 拉取内核源码
utop patch create linux --desc "thread parent tracing"  # 创建补丁
utop build run linux --patch thread-parent-trace.patch  # 构建
utop verify linux                          # 验证
utop install linux                         # 安装

# 出问题时回滚
utop rollback linux
```

### 4.4 读取追踪数据

```bash
# JSON 格式
cat /proc/thread_parent_trace

# 解析为 Python 对象
python3 -c "import json; d=json.load(open('/proc/thread_parent_trace')); print(d)"

# 持续监控
watch -n1 'cat /proc/thread_parent_trace | python3 -m json.tool'
```

### 4.5 与 utopOS monitor 集成

```bash
# 用 utopOS monitor 追踪某个命令创建的所有子线程
utop monitor /tmp -- ./my_multithread_app

# 报告会包含：
# - 文件系统变更（快照对比）
# - /proc/thread_parent_trace 中新增的条目
```

---

## 五、性能影响

| 操作 | 开销 | 说明 |
|------|------|------|
| 每次线程创建 | ~200ns | RCU 读 + spinlock + hash_add |
| /proc 读取 | O(n) | n = 当前活跃追踪条目数 |
| 内存占用 | ~100 bytes/条目 | 上限 65536 条目，约 6MB |
| 卸载开销 | 瞬间 | kpatch jump label 恢复原始函数 |

由于 `wake_up_new_task()` 本身已经是热路径（~2μs），增加的 200ns 开销约 10%，在可接受范围内。

---

## 六、对比其他方案的详细分析

### 6.1 vs kprobes

kprobes 通过在函数入口处替换第一条指令为 `int3`（软件断点），触发 trap 后执行 probe handler：

```
优点：
  - 无需修改内核源码
  - 热加载/卸载
  - 可以获取函数参数

缺点：
  - 只能"观测"，不能修改函数行为
  - 每次触发都走 trap 路径（~1-2μs 开销）
  - 不能处理被内联的函数
  - probe handler 中不能 sleep（原子上下文）
```

**我们的场景需要修改函数行为**（在执行前插入记录逻辑），kprobes 不满足需求。

### 6.2 vs eBPF/kfunc

eBPF 的 kprobe 程序可以在 `wake_up_new_task()` 被调用时执行：

```
优点：
  - 验证器保证安全性（不会 crash 内核）
  - 可以与 BPF map 配合存储数据
  - JIT 编译，性能好

缺点：
  - verifier 限制严格：不能访问任意内核结构
  - 不能修改被 hook 函数的行为
  - BPF map 有大小和类型限制
  - 对 task_struct 的深层访问可能被 verifier 拒绝
```

eBPF 适合做"安全的观测器"，但不能做"修改调用路径"的事。此外，eBPF 对 `real_parent` 这类 RCU 保护的指针访问存在 verifier 兼容性问题。

### 6.3 vs 源码补丁重编译

```
优点：
  - 最彻底的修改方式
  - 性能最优（编译器优化所有路径）

缺点：
  - 需要完整的内核编译环境（~30min+）
  - 必须重启才能生效
  - 每次内核更新都需要重新 patch
  - 对生产环境影响大
```

kpatch 本质上就是"安全化的源码补丁"，保留了修改能力，去掉了重启需求。

---

## 七、安全考虑

1. **RCU 保护**：读取 `p->real_parent` 时使用 `rcu_read_lock()`，保证指针安全
2. **Spinlock**：哈希表操作在 spinlock 保护下，防止并发损坏
3. **内存上限**：`MAX_ENTRIES = 65536` 防止内存耗尽
4. **惰性清理**：读取 /proc 时清理已退出的线程条目
5. **可卸载**：`rmmod` 时清理所有数据结构，恢复原始函数

---

## 八、文件说明

```
thread-parent-trace/
├── src/
│   ├── thread_parent_trace.c    # 核心实现
│   └── Makefile                 # 构建配置（模块 + kpatch）
├── scripts/
│   └── verify.sh               # 功能验证脚本
├── docs/
│   └── DESIGN.md               # 本文档
└── README.md                   # 快速开始
```
