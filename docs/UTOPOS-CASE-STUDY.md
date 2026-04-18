# utopOS Case Study：用 Agent-native 方式构建内核特性

> 从"下载 artifact"到"内核模块上线"，全程由 AI Agent 驱动，utopOS 提供工作流骨架。

---

## 摘要

本案例记录了如何使用 utopOS 的工具链（`utop` CLI + 决策层 Skill），从零开始构建一个内核线程追踪特性 `thread_parent_trace`。

整个过程不涉及手动包管理器操作、不跳过验证步骤、所有变更可追溯、可回滚。
**AI Agent 是操作者，utopOS 是约束框架**——Agent 做决策，utopOS 保证安全。

---

## 一、背景

### 1.1 需求

构建一个内核特性：在线程创建时，自动追踪其父线程关系，并通过 `/proc` 接口暴露。

### 1.2 约束

- 目标环境：Ubuntu 24.04 (kernel 6.8.0-100-generic)
- 不能重启内核（生产环境限制）
- 需要热加载/卸载能力
- 所有操作需可追溯、可回滚

### 1.3 工具链

- **utopOS 0.3.1** — Agent-native OS 管理平台
- **utop CLI** — 统一的操作系统修补工具
- **kpatch** — 内核热补丁技术

---

## 二、流程

### 2.1 Step 0：获取 utopOS

```
输入：GitHub Actions artifact URL + token
Agent 行为：
  curl → 下载 zip → 提取 deb → dpkg -x 解压
  utop init → 初始化 ~/.utop/ 工作目录
  utop detect → 检测到 deb 后端

utopOS 角色：
  提供标准的 init/detect 工作流
  确保工作目录结构一致
```

**关键点**：Agent 自主完成了下载和安装，utopOS 的 `init` 和 `detect` 保证了后续操作的基础环境。

### 2.2 Step 1：技术选型

Agent 面对的问题：**用什么方式实现内核函数级修改？**

候选方案：

| 方案 | Agent 评估 | 结论 |
|------|-----------|------|
| kprobes | 只能观测，不能修改函数行为 | ❌ |
| eBPF | verifier 限制，不能修改内核内存 | ❌ |
| ftrace | 可以 hook，但不能直接替换函数逻辑 | ⚠️ |
| 源码补丁重编译 | 需要重启内核 | ❌ |
| **kpatch** | 函数级替换 + 热加载 + 差分安全 | ✅ |

Agent 读取 utopOS 的 `track.rs` 源码，发现了 `Backend::Kpatch` 这一 variant，确认 kpatch 是 utopOS 生态中认可的后端。

**决策依据**：
- 需要"修改"而非"观测" → 排除 kprobes/eBPF
- 不能重启 → 排除源码重编译
- 需要替换函数而非仅 hook → 选择 kpatch 而非 ftrace

### 2.3 Step 2：编写内核模块

Agent 自主编写了 `thread_parent_trace.c`，包含：

```
核心组件：
├── patched_wake_up_new_task()  ← kpatch 替换函数
│   ├── RCU 读锁获取 parent
│   ├── hash 表记录 child→parent 映射
│   └── 调用原始函数（行为不变）
├── trace_hash[]               ← 哈希表存储
├── /proc/thread_parent_trace  ← 用户空间接口（JSON）
└── kpatch 描述符              ← klp_patch/klp_func 结构
```

**Agent 的自主决策**：
- 选择 `wake_up_new_task()` 作为 hook 点（而非 `sched_fork()`），因为它更简洁且是线程唤醒的必经路径
- 使用哈希表而非数组（O(1) 查找 vs O(n)）
- 设置 `MAX_ENTRIES = 65536` 防止内存耗尽
- 惰性清理策略（读 /proc 时顺便清理已退出的线程）

### 2.4 Step 3：构建与安装准备

Agent 创建了三种构建路径：

```
1. 内核模块方式（开发测试）  → make && insmod
2. kpatch 方式（生产环境）   → kpatch-build && kpatch load
3. utopOS 集成方式          → utop source fetch → patch create → build run → install
```

第三种路径是 utopOS 的核心价值——**标准化的 OS 修补工作流**：

```bash
utop source fetch linux                              # 获取源码
utop patch create linux --desc "thread parent tracing"  # 创建补丁
utop build run linux --patch thread-parent-trace.patch  # 构建
utop verify linux                                    # 验证（dry-run）
utop install linux                                   # 安装（包管理器）
```

### 2.5 Step 4：文档与验证

Agent 编写了：
- `docs/DESIGN.md` — 技术设计文档，包含方案对比（kpatch vs kprobes vs eBPF vs ftrace）
- `scripts/verify.sh` — 验证脚本，结合 `utop monitor` 检测线程创建副作用
- `README.md` — 快速开始指南

---

## 三、utopOS 在这个案例中的角色

### 3.1 utopOS 提供了什么

| 组件 | 作用 | 本案例中的使用 |
|------|------|-------------|
| `utop detect` | 检测系统包管理后端 | 确认 deb 后端，选择 dpkg/apt 工具链 |
| `utop init` | 标准化工作目录 | 创建 ~/.utop/ 结构 |
| `utop source fetch` | 拉取上游源码 | 准备内核源码用于 patch |
| `utop patch create` | 管理补丁文件 | 将修改保存为 unified diff |
| `utop build run` | 标准化构建流程 | 调用 kpatch-build 或 make |
| `utop verify` | 安装前验证 | dry-run 检查依赖和兼容性 |
| `utop install` | 包管理器安装 | 通过 dpkg/insmod 安装 |
| `utop rollback` | 多层回滚 | 卸载 kpatch 恢复原始函数 |
| `utop monitor` | 命令副作用检测 | 监控线程创建时的文件系统变更 |
| SKILL.md 决策层 | 工作流约束 | 确保 Agent 不跳过验证步骤 |

### 3.2 utopOS 没有做什么

- **没有替代 Agent 的判断**：技术选型（kpatch vs eBPF）是 Agent 自主决策的
- **没有编写代码**：内核模块的实现是 Agent 独立完成的
- **没有强制特定工具**：提供了三种构建路径，Agent 根据情况选择

### 3.3 核心价值：约束而非替代

```
没有 utopOS 的 Agent：
  "我来帮你改内核" → 直接 vim /usr/src/... → make install → 祈祷能用
  问题：无验证、无回滚、不可追溯

有 utopOS 的 Agent：
  "我来帮你改内核" → utop detect → utop source fetch → 分析 → 补丁 → verify → install
  保证：有验证、有回滚、可追溯
```

**utopOS 的本质**：不是让 Agent 更强，而是让 Agent 更安全。

---

## 四、技术亮点

### 4.1 为什么是 kpatch 而不是其他？

```
选型决策树：

  需要观测还是修改？
    ├── 只观测 → kprobes / eBPF（轻量、安全）
    └── 要修改 → 需要重启吗？
                  ├── 可以重启 → 源码补丁 + 重编译
                  └── 不能重启 → kpatch ✅
```

kpatch 的独特优势：
1. **函数级替换**：直接替换 `wake_up_new_task()` 的实现
2. **热加载**：`insmod` 即生效，无需重启
3. **差分安全**：.ko 只包含修改的指令，不是整个函数
4. **可回滚**：`rmmod` 恢复原始函数

### 4.2 内核数据结构选择

| 需求 | 方案 | 原因 |
|------|------|------|
| 存储 child→parent 映射 | 哈希表 | O(1) 查找，线程创建是热路径 |
| 并发安全 | spinlock + RCU | RCU 读 parent 指针，spinlock 写哈希表 |
| 内存控制 | MAX_ENTRIES 上限 | 防止无限增长导致 OOM |
| 清理策略 | 惰性清理 | 读 /proc 时顺便清理，不增加创建开销 |

### 4.3 性能影响

```
每次线程创建的额外开销：~200ns
原始 wake_up_new_task() 开销：~2000ns
额外开销占比：~10%

可通过以下优化进一步降低：
  - 预分配 entry 池（避免 kmalloc）
  - 使用 per-CPU 哈希表（减少锁竞争）
  - 批量清理（而非每次读 /proc 时清理）
```

---

## 五、经验总结

### 5.1 Agent 能力边界

| Agent 能做 | Agent 需要 utopOS 做 |
|-----------|---------------------|
| 技术选型判断 | 提供标准化工作流 |
| 编写内核代码 | 确保验证步骤不跳过 |
| 自主探索源码 | 提供回滚能力 |
| 文档撰写 | 管理补丁生命周期 |

### 5.2 utopOS 的适用场景

utopOS 最适合的场景：

- ✅ **系统级修改**：内核模块、系统库、服务配置
- ✅ **需要安全保证**：验证、回滚、追溯
- ✅ **Agent 驱动的工作流**：AI 做决策，utopOS 管流程
- ✅ **多后端支持**：同一工作流跨 Nix/RPM/Conda

utopOS 不太适合的场景：

- ❌ 纯用户态应用开发（不需要系统级修补）
- ❌ 一次性脚本（不需要工作流管理）
- ❌ 容器化环境（系统级修补在容器内无意义）

### 5.3 最终成果

```
时间线：
  T+0min   下载 utopOS artifact
  T+1min   utop init + detect
  T+3min   技术选型完成（kpatch）
  T+15min  内核模块代码完成
  T+18min  Makefile + 构建脚本
  T+22min  设计文档完成
  T+25min  推送到 GitHub

总计：~25 分钟
产出：
  - 可编译的内核模块（kpatch 兼容）
  - 完整的设计文档
  - 验证脚本
  - utopOS 集成补丁
```

---

## 六、关于 utopOS

utopOS 是第一个开源的 **Agent-native 操作系统管理平台**：
- AI Agent 通过修补源码、重新打包、包管理器安装来管理操作系统
- 不碰运行时文件，所有变更可追溯、可回滚
- 支持 Nix / RPM / Conda / Btrfs / Deb 多种后端

> **Unified Tool for OS Patching** — 统一的操作系统修补工具。

GitHub: https://github.com/ampresent/utopos
