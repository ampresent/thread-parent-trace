# utop-kpatch — 内核热补丁专项

> 本 skill 是 `utop` 的子 skill，专注内核级修改（kpatch / livepatch）。
> 通用工作流见父 skill：`utop/SKILL.md`

## 触发条件

当用户请求涉及以下内容时，进入本 skill：

| 信号 | 示例 | 判断依据 |
|------|------|---------|
| 修改内核函数行为 | "追踪线程创建时的父线程"、"hook sys_call_table" | 需要替换内核 C 函数 |
| 内核调度器行为 | "给线程绑核"、"修改 CFS 调度策略"、"追踪 CPU 迁移" | 涉及 `kernel/sched/` |
| 系统调用拦截 | "拦截 openat"、"审计 execve" | 需要修改 `arch/x86/entry/` |
| procfs/sysfs 扩展 | "暴露内核数据到 /proc"、"新增 sysctl 参数" | 需要新增内核文件 |
| 内核模块但需要改已有函数 | "在 kfree 时做审计" | LKM 只能 hook 导出符号 |
| 不能重启的内核修改 | "生产环境不能重启，但要改内核" | 唯一选择是热补丁 |

## 前置检测

进入工作流前，Agent 必须按顺序检测：

```bash
# 1. 内核版本
uname -r

# 2. 内核头文件（编译内核模块需要）
dpkg -l linux-headers-$(uname -r)  # Debian/Ubuntu
rpm -qa kernel-devel               # RPM

# 3. kpatch-build 工具链（kpatch 方式需要）
which kpatch-build || echo "需要安装 kpatch"

# 4. CONFIG_LIVEPATCH 支持（运行时需要）
cat /boot/config-$(uname -r) | grep CONFIG_LIVEPATCH
# CONFIG_LIVEPATCH=y 表示内核已编译热补丁支持

# 5. utop detect 确认包管理后端
utop detect
```

## 技术选型决策树

```
需要修改内核行为？
    │
    ├── 只观测（不改逻辑）
    │     │
    │     ├── 能接受 probe 开销 → kprobes
    │     └── 需要高性能 → eBPF (kprobe/tracepoint)
    │
    └── 要修改（改函数逻辑）
          │
          ├── 能重启？
          │     ├── 是 → 源码补丁 + 重编译 + reboot
          │     └── 否 → kpatch ✅
          │
          └── 只 hook 导出符号？
                ├── 是 → 内核模块 (LKM) 可能够用
                └── 否 → kpatch（非导出函数也能替换）
```

## kpatch 工作流

```
┌─ 0. 环境检测 ──────────────────────────────────────────────┐
│  uname -r                     → 内核版本                    │
│  which kpatch-build           → kpatch 工具链               │
│  grep CONFIG_LIVEPATCH /boot  → 内核支持                    │
│  utop detect                  → 包管理后端                   │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 1. 源码分析 ──────────────────────────────────────────────┐
│  utop source fetch linux                                   │
│  进入 kernel/ 目标子系统                                    │
│  找到要替换的函数：                                         │
│    - grep -rn "函数名" kernel/sched/                       │
│    - 理解函数签名、调用者、被调用者                         │
│    - 确认函数未被 inline（或有 __weak 版本）                │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 2. 编写 kpatch 模块 ──────────────────────────────────────┐
│  核心结构：                                                 │
│                                                            │
│  struct klp_func kpatch_funcs[] = {                        │
│    {                                                       │
│      .old_name = "target_function",                        │
│      .new_func = patched_target_function,                  │
│    },                                                      │
│    { }                                                     │
│  };                                                        │
│                                                            │
│  struct klp_object kpatch_obj = {                          │
│    .name = "vmlinux",                                      │
│    .funcs = kpatch_funcs,                                  │
│  };                                                        │
│                                                            │
│  struct klp_patch kpatch_patch = {                         │
│    .mod = THIS_MODULE,                                     │
│    .objs = &kpatch_obj,                                    │
│  };                                                        │
│                                                            │
│  编写要点：                                                 │
│    - 新函数先执行自定义逻辑，再调用原始函数                  │
│    - 使用 RCU/spinlock 保护共享数据结构                     │
│    - 设置内存上限防止 OOM                                   │
│    - 提供 /proc 或 sysfs 接口暴露数据                       │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 3. 构建 ──────────────────────────────────────────────────┐
│                                                            │
│  方式 A：kpatch-build（推荐）                               │
│    kpatch-build -s /usr/src/linux-headers-$(uname -r) \    │
│      -v /boot/vmlinuz-$(uname -r) \                       │
│      --name my-patch module.c                              │
│    → 输出 kpatch-my-patch.ko                               │
│                                                            │
│  方式 B：标准内核模块（开发/测试）                          │
│    make -C /lib/modules/$(uname -r)/build M=$PWD modules   │
│    → 输出 my_module.ko                                     │
│                                                            │
│  方式 C：utopOS 集成                                       │
│    utop patch create linux --desc "kpatch: ..."            │
│    utop build run linux --patch my-patch.patch             │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 4. 验证 ──────────────────────────────────────────────────┐
│                                                            │
│  危险等级：DANGEROUS（内核级修改，永远需要人工确认）         │
│                                                            │
│  验证项：                                                  │
│    - 模块编译无 warning                                     │
│    - insmod 后 dmesg 无 error                              │
│    - /proc 或 sysfs 接口可读                               │
│    - 功能测试通过（见下方）                                 │
│    - 卸载后原始功能正常（rmmod 测试）                       │
│                                                            │
│  ⚠️  内核热补丁的 risk 永远是 dangerous                     │
│  ⚠️  必须向用户展示完整风险摘要并等待确认                   │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼ 用户确认（必须）
┌─ 5. 安装 ──────────────────────────────────────────────────┐
│                                                            │
│  kpatch 方式：                                             │
│    kpatch load kpatch-my-patch.ko                          │
│    kpatch list                    # 确认已加载              │
│    dmesg | tail                   # 检查内核日志            │
│                                                            │
│  模块方式：                                                 │
│    insmod my_module.ko                                     │
│    lsmod | grep my_module                                  │
│                                                            │
│  utopOS 方式：                                             │
│    utop install linux                                      │
│                                                            │
│  安装后验证：                                               │
│    - 功能测试                                              │
│    - dmesg 无 crash/panic                                  │
│    - 记录回滚命令到 ~/.utop/history/                       │
└────────────────────────────────────────────────────────────┘
                         │
                         ▼ (出问题时)
┌─ 6. 回滚 ──────────────────────────────────────────────────┐
│                                                            │
│  kpatch 方式：                                             │
│    kpatch unload my-patch                                  │
│                                                            │
│  模块方式：                                                 │
│    rmmod my_module                                         │
│                                                            │
│  utopOS 方式：                                             │
│    utop rollback linux                                     │
│                                                            │
│  ⚠️  如果热补丁导致内核 panic：                              │
│    → kpatch 会在下次 reboot 时自动消失（不持久化）          │
│    → 这是 kpatch 比源码补丁更安全的核心原因                │
└────────────────────────────────────────────────────────────┘
```

## 风险摘要卡片模板（内核级）

```
┌─────────────────────────────────────────────────┐
│ 🔴 内核热补丁 — DANGEROUS                        │
│                                                 │
│ 📦 目标: linux (kernel 6.8.0-100-generic)       │
│ 🔧 替换函数: wake_up_new_task()                  │
│ 📝 描述: 线程创建时记录 parent→child 关系        │
│ ⚠️  风险: dangerous（内核函数级修改）             │
│ 🔄 回滚: rmmod thread_parent_trace               │
│    或: kpatch unload thread_parent_trace         │
│ 💀 最坏情况: kernel panic（重启即恢复）          │
│ 📁 影响范围: kernel/sched/core.c                 │
│                                                 │
│ ⚠️  内核热补丁导致的 panic 仅影响本次运行，      │
│    重启后 kpatch 自动消失，不会持久化损坏。       │
│                                                 │
│ 要继续吗？[y/n]                                 │
└─────────────────────────────────────────────────┘
```

## kpatch 与其他方案对比速查

| 能力 | kprobes | eBPF | ftrace | LKM | **kpatch** |
|------|---------|------|--------|-----|-----------|
| 观测函数调用 | ✅ | ✅ | ✅ | ⚠️ 仅导出 | ✅ |
| 修改函数逻辑 | ❌ | ❌ | ⚠️ | ❌ | ✅ |
| 热加载 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 替换非导出函数 | ❌ | ❌ | ⚠️ | ❌ | ✅ |
| 卸载即恢复 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 无需重启 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 性能开销 | 高(trap) | 低(JIT) | 低 | 低 | **无** |
| 安全保证 | 中 | 高(verifier) | 中 | 低 | 中 |

## 常见内核子系统与目标函数

| 需求 | 目标函数 | 所在文件 |
|------|---------|---------|
| 追踪线程创建 | `wake_up_new_task()` | `kernel/sched/core.c` |
| 追踪进程 fork | `sched_fork()` | `kernel/sched/core.c` |
| 追踪 exec | `do_execve()` | `fs/exec.c` |
| 追踪文件打开 | `do_sys_openat2()` | `fs/open.c` |
| 追踪信号发送 | `do_send_sig_info()` | `kernel/signal.c` |
| 追踪内存分配 | `kmalloc()` / `__kmalloc()` | `mm/slub.c` |
| 修改调度策略 | `enqueue_task_fair()` | `kernel/sched/fair.c` |
| CPU 亲和性追踪 | `sched_setaffinity()` | `kernel/sched/core.c` |

## 注意事项

1. **CONFIG_LIVEPATCH 必须开启**：大部分现代发行版默认开启，但需确认
2. **kpatch-build 需要完整的内核源码**：`apt install linux-source-$(uname -r)` 或等价命令
3. **不能 patch inline 函数**：kpatch 只能替换有独立编译单元的函数
4. **不能 patch static 函数**（除非在同一编译单元内）
5. **符号重定位**：kpatch-build 自动处理，但复杂的宏展开可能失败
6. **跨版本不兼容**：为 kernel X 编译的 kpatch 不能在 kernel Y 上使用
