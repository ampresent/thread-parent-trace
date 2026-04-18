# UtopOS Skill — 决策层

> AI Agent 的源码修复工作流。支持 Nix / RPM / Conda / Btrfs / kpatch 多种后端。
> **脚本做机械事，Skill 做决策，Agent 做连接，用户做确认。**

## 主动检测（Proactive Detection）

每次收到用户请求或命令返回错误时，先过这层再继续。

### 信号表

| 信号 | 示例 | 动作 | 后端 |
|------|------|------|------|
| 缺头文件/库 | `fatal error: openssl/ssl.h: No such file` | 进入工作流 | 用户态 |
| 缺 binary | `cmake: command not found` | 进入工作流 | 用户态 |
| GLIBC/库版本不匹配 | `GLIBCXX_3.4.29 not found` | 进入工作流 | 用户态 |
| 链接失败 | `cannot find -lssl` | 进入工作流 | 用户态 |
| 用户意图：改编译器/工具链 | "upgrade gcc", "switch c++ standard" | 进入工作流 | 用户态 |
| 用户意图：补丁系统库/守护进程 | "fix libevent bug", "patch openssl" | 进入工作流 | 用户态 |
| 用户意图：替换系统 binary | "build custom nginx" | 进入工作流 | 用户态 |
| 用户意图：内核行为（轻量） | "change sysctl", "rebuild module" | 进入工作流 | 用户态 |
| **用户意图：内核函数级修改** | "追踪线程创建的父线程"、"hook wake_up_new_task" | **进入 kpatch 工作流** | **kpatch** |
| **用户意图：内核调度器** | "给线程绑核"、"修改 CFS 策略"、"追踪 CPU 迁移" | **进入 kpatch 工作流** | **kpatch** |
| **用户意图：内核热补丁** | "kpatch"、"livepatch"、"不重启改内核" | **进入 kpatch 工作流** | **kpatch** |
| **用户意图：系统调用拦截** | "拦截 openat"、"审计 execve" | **进入 kpatch 工作流** | **kpatch** |
| **用户意图：procfs/sysfs 扩展** | "新增 /proc 接口暴露内核数据" | **进入 kpatch 工作流** | **kpatch** |
| 级联到 binary 的配置变更 | `ld.so.conf`, `LD_LIBRARY_PATH`, `update-alternatives` | 进入工作流 | 用户态 |

### 内核意图识别规则

当用户请求中出现以下关键词/模式时，Agent 应主动判断为内核级需求：

```
关键词/短语                        → 判断
──────────────────────────────────────────────
"线程" + "追踪"/"跟踪"            → 内核级（调度器 hook）
"进程" + "创建"/"fork" + "监控"   → 内核级
"CPU 亲和性"/"绑核"/"affinity"   → 内核级（如果要改调度逻辑）
"系统调用"/"syscall" + "拦截"     → 内核级
"/proc" + "新建"/"暴露"           → 内核级（需要新模块）
"kpatch"/"livepatch"/"热补丁"     → 明确内核级
"调度器"/"scheduler" + "修改"     → 内核级
"hook" + 内核函数名               → 内核级
"不重启" + "改内核"               → kpatch
```

**注意**：用户空间的 CPU 绑核（`sched_setaffinity`/`taskset`）**不是**内核级需求。
区分标准：是否需要修改/替换内核函数逻辑。

### `$UTOPOS_LEVEL` — 干预激进程度（默认 3）

| Level | 行为 |
|-------|------|
| **1** | 只拦截直接编辑系统文件。不推断意图和报错。纯被动。 |
| **2** | 扫描命令报错（`command not found`、缺头文件）。建议 `utop detect` + 安装。 |
| **3** | **默认。** 扫报错 + 用户意图。主动建议 `utop source fetch → patch → build`，防用户走弯路。 |
| **4** | 激进：编译/链接错误直接自动 fetch 源码诊断，不等确认。补丁前仍需确认。 |
| **5** | 全自动：诊断→补丁→构建→安装。仅 `dangerous` 级别需确认。 |

### 判断流程

```
用户请求 / 命令输出
        │
        ▼
  命中信号表？─── NO ──→ 正常处理
        │
       YES
        │
        ▼
  是内核级需求？
    ├── YES → 进入 kpatch 工作流（utop-kpatch/SKILL.md）
    └── NO  → 继续 ↓
        │
        ▼
  $UTOPOS_LEVEL 满足阈值？
   1: 仅硬拦截直接编辑
   2+: 建议 UtopOS
   4+: 自动 fetch 诊断
   5+: 全自动
        │
        ▼
  进入下方「工作流决策树」
```

---

## 前置条件

`utop` 二进制安装在 `/usr/bin/` 下，所有操作通过 `utop <子命令>` 调用，无需设置环境变量。
如果 `~/.utop/` 不存在，运行 `utop init` 会自动创建（幂等，已有则跳过）。

```bash
# 直接调用，无需 SCRIPTS 变量
utop detect
utop source fetch nginx
utop build run nginx --patch fix.patch
```

---

## 核心铁律（不可违反）

1. **❌ 不要**用 workaround 绕过问题（重启服务、改运行时配置糊弄）
2. **❌ 不要**直接替换二进制、编辑运行时文件
3. **⚠️ 内核热补丁（kpatch）是唯一例外**：`insmod` / `kpatch load` 是内核级修改的标准安装方式，走完整验证流程后允许执行
4. **❌ 不要**跳过验证直接安装（必须先 dry-build / test install）
5. **❌ 不要**混用包管理器（conda 环境里用 pip、RPM 系统上用 apt）
6. **✅ 要**走完整流程：检测 → 源码 → 分析 → 补丁 → 验证 → 安装
7. **✅ 内核级修改永远是 dangerous**：无论改动多小，kpatch 操作必须向用户展示风险并等待确认

---

## 工作流决策树

收到用户问题后，按以下流程走：

```
┌─ 0. 初始化 ──────────────────────────────────────────┐
│  utop init                                           │
│  (幂等，已有 ~/.utop/ 就跳过)                          │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 0.5 判断层级 ───────────────────────────────────────┐
│  检查信号表 + 内核意图识别规则                         │
│                                                      │
│  内核级？── YES ──→ 跳转 utop-kpatch/SKILL.md         │
│      │                                               │
│      NO                                              │
│      │                                               │
│      ▼                                               │
│  继续用户态工作流 ↓                                   │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 1. 诊断 + 获取源码 ─────────────────────────────────┐
│  utop detect           → { backend, version, tools } │
│  utop source fetch pkg → { src_dir, spec?, recipe? } │
│  utop source info pkg     → { version, desc, deps }     │
│  (并行) system diagnosis: journalctl / systemctl     │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 2. 分析源码 ────────────────────────────────────────┐
│  进入 src_dir，找到根因                                │
│  关注：配置模板默认值、编译参数、源码 bug              │
│  ⚠️ 不看运行时文件，只看源码                          │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─ 3. 生成补丁 ────────────────────────────────────────┐
│  修改源码 → utop patch create pkg --desc "..."        │
│  → { patch_file, risk, files_changed }               │
│  ▶ 向用户展示风险摘要（见下方模板）                   │
│  ▶ 等待用户确认                                      │
└─────────────────────────────────────────────────────┘
                         │
                         ▼ 用户确认
┌─ 4. 构建 ────────────────────────────────────────────┐
│  utop build run pkg --patch <patch_file>                  │
│  → { result, log }                                   │
│  utop verify pkg       → { risk, changes }            │
│  ▶ 再次向用户确认风险等级                             │
└─────────────────────────────────────────────────────┘
                         │
                         ▼ 用户确认
┌─ 5. 安装 ────────────────────────────────────────────┐
│  utop install pkg      → { txn_id, rollback_cmd }    │
│  验证服务是否正常                                    │
└─────────────────────────────────────────────────────┘
                         │
                         ▼ (可选)
┌─ 6. 提交上游 ────────────────────────────────────────┐
│  如果是上游 bug，生成 PR                              │
└─────────────────────────────────────────────────────┘
```

---

## 脚本速查

### 基础设施

| 脚本 | 什么时候调 | 输出关键字段 |
|------|-----------|-------------|
| `utop init` | 首次使用、`~/.utop/` 不存在时 | `dirs` |
| `utop workspace create <pkg>` | 开始处理一个新包 | `work_dir`, `utop_work` |
| `utop workspace list` | 查看有哪些进行中的修复 | `workspaces[]` |
| `utop workspace status <pkg>` | 查看某包的工作目录状态 | `patches`, `size`, `last_build` |
| `utop workspace archive <pkg>` | 修复完成，归档工作目录 | `archive` |
| `utop cleanup` | 定期清理、磁盘紧张时 | `cleaned`, `freed_bytes`, `disk_warning` |

### 检测 + 获取

| 脚本 | 什么时候调 | 输出关键字段 |
|------|-----------|-------------|
| `utop detect` | 第一步，确定后端 | `backend`, `version`, `tools` |
| `utop source fetch <pkg>` | 检测后立即调 | `src_dir`, `spec`, `recipe` |
| `utop source info <pkg>` | 需要包详细信息时 | `version`, `description`, `homepage` |

### 构建 + 安装

| 脚本 | 什么时候调 | 输出关键字段 |
|------|-----------|-------------|
| `utop build run <pkg> --patch <f>` | 分析完成、用户确认后 | `result`, `log` |
| `utop verify <pkg>` | 安装前必须调 | `risk`, `changes`, `missing_deps` |
| `utop install <pkg>` | verify 通过、用户确认后 | `txn_id`, `rollback_cmd` |
| `utop rollback <pkg>` | 出问题时 | `rolled_to`, `log` |

### Patch 管理

| 脚本 | 什么时候调 | 输出关键字段 |
|------|-----------|-------------|
| `utop patch create <pkg> --desc "..."` | 源码修改完成后 | `patch`, `risk`, `files_changed` |
| `utop patch list <pkg>` | 查看已有补丁 | `patches[]` |
| `utop patch check <pkg>` | 构建前检查兼容性 | `results[].status` (compatible/conflict) |
| `utop patch series show <pkg>` | 多个补丁需要排序时 | `series[]` |

---

## 用户交互协议

### 必须确认的节点

以下 3 个节点**必须**向用户展示信息并等待确认，不能自动跳过：

#### ① 补丁生成后

```
📋 风险摘要
─────────
包名: nginx
修改: src/http/ngx_http_core_module.c
影响: upstream timeout 默认值从 60s → 120s
风险: moderate
回滚: utop rollback nginx

要继续构建吗？[y/n]
```

#### ② 验证通过后（安装前）

```
🔍 验证结果
─────────
风险等级: safe
缺失依赖: 0
测试安装: 通过

要安装吗？[y/n]
```

#### ③ 内核热补丁（必须确认）

```
🔴 内核热补丁 — DANGEROUS
─────────
目标: linux (kernel X.Y.Z)
替换函数: func_name()
回滚: rmmod module_name
最坏情况: kernel panic（重启即恢复）

要继续吗？[y/n]
```

### 风险摘要卡片模板

```
┌─────────────────────────────────────┐
│ 📦 包名: {pkg}                       │
│ 🔧 修改: {文件列表}                  │
│ 📝 描述: {补丁描述}                  │
│ ⚠️  风险: {safe|moderate|dangerous}  │
│ 💾 回滚: {rollback_cmd}             │
│ 🎫 关联: {ticket 或 none}           │
└─────────────────────────────────────┘
```

风险等级判定规则：
- **safe**: 配置值变更、不影响二进制、可无损回滚
- **moderate**: 源码逻辑修改、影响单一功能、有回滚方案
- **dangerous**: 核心模块修改、影响面广、回滚代价高
  - **特殊规则**：所有内核级修改（kpatch / 内核模块）自动升为 **dangerous**，不可降级

---

## 信任白名单

文件位置：`~/.utop/trust.toml`

格式：
```toml
[trust.nginx]
safe_auto = true          # safe 级补丁自动 apply，不再询问
risk_levels = ["safe"]

[trust.php]
safe_auto = true
risk_levels = ["safe", "moderate"]   # moderate 也自动
```

**决策逻辑**：

```
1. 读 ~/.utop/trust.toml
2. 找到 [trust.<pkg>] 配置
3. 如果 patch.risk 在 risk_levels 内 → 跳过确认，直接构建
4. 否则 → 走正常确认流程
```

**安全限制**：
- `dangerous` 级补丁永远不自动 apply，即使在白名单中
- **内核级修改（kpatch）永远不进入白名单**，必须每次人工确认
- 白名单变更需要用户手动编辑 trust.toml，agent 不自动修改

---

## 后端专项 Skill

通用工作流本 skill 已覆盖。后端的深度知识（语言速查、构建细节、常见问题排障）拆分为独立 skill：

| 后端 | Skill | 内容 |
|------|-------|------|
| **Nix / NixOS** | `utop-nix/SKILL.md` | Nix 语言速查、overlay / overrideAttrs 深度用法、NixOS module 开发、generation 回滚、**用户态安装（nix-env / nix shell / home-manager）** |
| **RPM** | `utop-rpm/SKILL.md` | Spec 文件结构、rpmbuild 用法、SRPM 工作流、yum history 回滚、发行版差异、**用户态安装（rpm2cpio + 环境变量注入）** |
| **Conda** | `utop-conda/SKILL.md` | Feedstock 结构、meta.yaml Jinja2 模板、conda build / skeleton、revision 回滚、私有 channel、**用户态安装（conda activate 栈叠加）** |
| **kpatch** | `utop-kpatch/SKILL.md` | 内核热补丁技术选型、kpatch-build 工作流、klp_func 结构、内核子系统目标函数速查、**危险等级永远为 dangerous** |

**何时读子 skill**：
- Agent 遇到该后端特有的问题（如 "spec 文件怎么写"、"overlay 语法"）
- 需要排查该后端特有的构建错误
- 要做后端深度操作（写 NixOS module、调 rpmbuild 参数、编写 kpatch 模块）
- **需要非 root 安装包**（查看子 skill 的「非 root 安装方式」章节）

### 安装方式速查

不同后端对「普通用户能否安装包」的支持差异很大，选择策略：

```
用户说 "装一个包"
    │
    ▼
是内核级修改？
    ├── 是 → 查 utop-kpatch/SKILL.md（永远需要 root）
    └── 否 → 继续 ↓
              │
              ▼
        有 root 权限？
            ├── 是 → 用标准安装流程（dnf / nixos-rebuild / conda install）
            └── 否 → 查子 skill 的「非 root 安装方式」
```

**各后端安装方式：**

| 后端 | 用户态方式 | 核心机制 |
|------|-----------|---------|
| Nix | nix-env / nix shell / home-manager | /nix/store 隔离 + profile 指针 |
| RPM | rpm2cpio + 环境变量 | 解压到用户目录 + PATH/LD_LIBRARY_PATH |
| Conda | conda activate（原生） | 环境栈叠加 |
| **kpatch** | **不支持用户态** | **必须 root，insmod/kpatch load** |

---

## 常见场景快速入口

### "服务挂了 / 502 / 启动失败"

```
1. utop detect
2. utop source fetch <pkg>
3. 诊断（journalctl / systemctl）
4. 分析源码 → utop patch create
5. utop build run → utop verify → utop install
```

### "默认配置不合理，想改默认值"

```
1. utop detect
2. utop source fetch <pkg>
3. 找到配置模板/默认值 → utop patch create --risk safe
4. 检查 trust.toml → 如果 safe_auto → 直接构建
5. utop build run → utop verify → utop install
```

### "补丁冲突了（上游更新后）"

```
1. utop source fetch <pkg> --force   # 重新拉取新版本
2. utop patch check <pkg>            # 检查哪些 patch 冲突
3. 逐个解决冲突 → utop patch create
4. utop build run → utop verify → utop install
```

### "想回滚"

```
1. utop rollback <pkg>                    # 回滚到上一个
2. utop rollback <pkg> --to <id>          # 回滚到指定版本
```

### "要改内核 / 不重启改内核"

```
1. 判断：是观测还是修改？
   - 只观测 → kprobes / eBPF
   - 要修改 → 进入 kpatch 工作流
2. uname -r && 检查 CONFIG_LIVEPATCH
3. utop source fetch linux
4. 找到目标函数 → 编写 kpatch 模块
5. kpatch-build 或 make
6. ⚠️ 向用户展示 DANGEROUS 风险摘要
7. 用户确认 → kpatch load / insmod
8. 功能测试 → dmesg 检查
```

---

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `UTOP_HOME` | `~/.utop` | 工作目录根路径，`utop init` 自动创建 |

---

## 反模式速查

| ❌ 反模式 | ✅ 正确做法 |
|----------|-----------|
| `vim /etc/nginx/nginx.conf` | 分析源码 → patch → build → install |
| `systemctl restart nginx` | 找根因，修源码 |
| 跳过 `utop verify` | 永远先 verify 再 install |
| `pip install` in conda | 只用对应包管理器 |
| 不记录就修改 | `utop patch create` + git commit |
| 直接 `insmod` 未经验证的 .ko | 走 kpatch 工作流，先验证再加载 |
| 用户态问题用 kpatch | 用户态修改走标准工作流，kpatch 只用于内核级 |
| kpatch 操作跳过用户确认 | 内核级永远 dangerous，必须人工确认 |
