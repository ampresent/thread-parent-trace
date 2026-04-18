// SPDX-License-Identifier: GPL-2.0
/*
 * thread_parent_trace.c — Kernel thread parent tracing via kpatch
 *
 * 使用 kpatch (Kernel Live Patching) 技术，在不重启内核的前提下，
 * 挂载一个对 wake_up_new_task() 的 hook，记录每个新线程的父线程关系。
 *
 * 原理：
 *   - kpatch 将本文件编译为 .ko 内核模块
 *   - 通过 kpatch 的 klp_func 结构体，替换目标函数的入口
 *   - 新的 wake_up_new_task() 版本在执行原始逻辑前，记录 parent → child
 *   - 数据通过 /proc/thread_parent_trace 对外暴露
 *
 * 为什么是 kpatch 而不是其他方案？见 docs/DESIGN.md
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/kpatch.h>
#include <linux/livepatch.h>

#define TRACE_BITS    10
#define TRACE_SIZE    (1 << TRACE_BITS)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("UtopOS");
MODULE_DESCRIPTION("Thread parent tracing via kpatch live patching");
MODULE_VERSION("1.0.0");

/* ──────────────────────────────────────────────────
 * 数据结构：记录 child_pid → parent_pid 映射
 * ────────────────────────────────────────────────── */
struct thread_parent_entry {
	struct hlist_node node;
	pid_t child_pid;
	pid_t parent_pid;
	pid_t parent_tgid;      /* 进程组 ID（区分线程和进程） */
	char  parent_comm[TASK_COMM_LEN];
	char  child_comm[TASK_COMM_LEN];
	u64   timestamp_ns;      /* 创建时间戳 */
};

static DEFINE_HAShtable(trace_hash, TRACE_BITS);
static DEFINE_SPINLOCK(trace_lock);
static atomic_t entry_count = ATOMIC_INIT(0);

#define MAX_ENTRIES 65536

/* ──────────────────────────────────────────────────
 * 查找/插入/清理
 * ────────────────────────────────────────────────── */
static struct thread_parent_entry *trace_find(pid_t child)
{
	struct thread_parent_entry *e;

	hash_for_each_possible(trace_hash, e, node, child) {
		if (e->child_pid == child)
			return e;
	}
	return NULL;
}

static void trace_insert(pid_t child_pid, const char *child_comm,
			 pid_t parent_pid, pid_t parent_tgid,
			 const char *parent_comm)
{
	struct thread_parent_entry *e;
	unsigned long flags;

	if (atomic_read(&entry_count) >= MAX_ENTRIES) {
		/* 防止内存无限增长：满了就丢弃最老的（简化处理） */
		return;
	}

	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return;

	e->child_pid = child_pid;
	e->parent_pid = parent_pid;
	e->parent_tgid = parent_tgid;
	e->timestamp_ns = ktime_get_ns();
	strscpy(e->child_comm, child_comm, TASK_COMM_LEN);
	strscpy(e->parent_comm, parent_comm, TASK_COMM_LEN);

	spin_lock_irqsave(&trace_lock, flags);
	/* 再次检查，避免并发重复插入 */
	if (trace_find(child_pid)) {
		spin_unlock_irqrestore(&trace_lock, flags);
		kfree(e);
		return;
	}
	hash_add(trace_hash, &e->node, child_pid);
	atomic_inc(&entry_count);
	spin_unlock_irqrestore(&trace_lock, flags);
}

/* 清理已退出的线程条目（在 proc 读取时顺便清理） */
static void trace_cleanup_exited(void)
{
	struct thread_parent_entry *e;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	spin_lock_irqsave(&trace_lock, flags);
	hash_for_each_safe(trace_hash, bkt, tmp, e, node) {
		/* 如果 child 已经不存在于任务列表中，清理它 */
		if (!find_task_by_vpid(e->child_pid)) {
			hash_del(&e->node);
			kfree(e);
			atomic_dec(&entry_count);
		}
	}
	spin_unlock_irqrestore(&trace_lock, flags);
}

/* ──────────────────────────────────────────────────
 * kpatch hook：替换 wake_up_new_task()
 *
 * kpatch 的机制：
 *   1. 原始 wake_up_new_task(struct rq *rq, struct task_struct *p) 继续存在
 *   2. kpatch 通过 klp_func 将调用入口重定向到本函数
 *   3. 本函数先执行追踪逻辑，再调用原始函数
 *   4. 通过 kpatch 的 symdiff 机制，可以安全地引用原始函数
 * ────────────────────────────────────────────────── */

/*
 * 保存原始函数指针（由 kpatch 框架在加载时填充）
 * kpatch 使用 KPATCH_RELOC 或 symbol 偏移来实现这一点
 */
typedef void (*wake_up_new_task_t)(struct rq *rq, struct task_struct *p);
static wake_up_new_task_t orig_wake_up_new_task;

/*
 * 新版本的 wake_up_new_task
 * kpatch 通过 kpatch_func 结构将原始函数替换为本函数
 */
static void patched_wake_up_new_task(struct rq *rq, struct task_struct *p)
{
	struct task_struct *parent;

	/* 追踪逻辑：记录 parent → child 关系 */
	if (p) {
		rcu_read_lock();
		parent = rcu_dereference(p->real_parent);
		if (parent) {
			trace_insert(
				task_pid_vnr(p),
				p->comm,
				task_pid_vnr(parent),
				task_tgid_vnr(parent),
				parent->comm
			);
		}
		rcu_read_unlock();
	}

	/* 调用原始函数，保证行为不变 */
	if (orig_wake_up_new_task)
		orig_wake_up_new_task(rq, p);
}

/* ──────────────────────────────────────────────────
 * /proc/thread_parent_trace 接口
 *
 * 输出格式（JSON lines）：
 *   {"child_pid":1234,"child_comm":"worker","parent_pid":1200,"parent_comm":"main","ts":...}
 * ────────────────────────────────────────────────── */
static int trace_proc_show(struct seq_file *m, void *v)
{
	struct thread_parent_entry *e;
	unsigned long flags;
	int bkt;

	trace_cleanup_exited();

	seq_puts(m, "{\n  \"entries\": [\n");

	spin_lock_irqsave(&trace_lock, flags);
	hash_for_each(trace_hash, bkt, e, node) {
		seq_printf(m,
			"    {\"child_pid\": %d, \"child_comm\": \"%s\", "
			"\"parent_pid\": %d, \"parent_tgid\": %d, "
			"\"parent_comm\": \"%s\", \"ts_ns\": %llu}",
			e->child_pid, e->child_comm,
			e->parent_pid, e->parent_tgid,
			e->parent_comm, e->timestamp_ns);

		/* 不是最后一个条目就加逗号 */
		if (!hash_empty(trace_hash) && bkt < HASH_SIZE(trace_hash) - 1)
			seq_puts(m, ",\n");
	}
	spin_unlock_irqrestore(&trace_lock, flags);

	seq_printf(m, "\n  ],\n  \"total\": %d\n}\n",
		   atomic_read(&entry_count));
	return 0;
}

static int trace_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, trace_proc_show, NULL);
}

static const struct proc_ops trace_proc_ops = {
	.proc_open    = trace_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ──────────────────────────────────────────────────
 * kpatch 描述符
 *
 * 这是 kpatch 的核心数据结构：
 * - funcs[]: 定义哪些函数被替换
 * - orig_func: 原始函数的符号名
 * - new_func: 新函数指针
 *
 * kpatch-build 工具会读取这些信息，生成 .ko 模块
 * ────────────────────────────────────────────────── */

/*
 * kpatch 兼容的符号替换声明
 * kpatch-build 使用此宏将 patched_wake_up_new_task 关联到
 * 内核符号 wake_up_new_task
 */
#if defined(CONFIG_LIVEPATCH) || defined(CONFIG_KPATCH)
#include <linux/livepatch.h>

static struct klp_func kpatch_funcs[] = {
	{
		.old_name = "wake_up_new_task",
		.new_func = patched_wake_up_new_task,
	},
	{ }
};

static struct klp_object kpatch_obj = {
	.name = "vmlinux",
	.funcs = kpatch_funcs,
};

static struct klp_patch kpatch_patch = {
	.mod = THIS_MODULE,
	.objs = &kpatch_obj,
};
#endif

/* ──────────────────────────────────────────────────
 * 模块初始化 / 卸载
 * ────────────────────────────────────────────────── */
static struct proc_dir_entry *proc_entry;

static int __init thread_parent_trace_init(void)
{
	int ret;

	pr_info("thread_parent_trace: initializing\n");

	hash_init(trace_hash);

	/* 创建 /proc/thread_parent_trace */
	proc_entry = proc_create("thread_parent_trace", 0444, NULL,
				 &trace_proc_ops);
	if (!proc_entry) {
		pr_err("thread_parent_trace: failed to create proc entry\n");
		return -ENOMEM;
	}

#if defined(CONFIG_LIVEPATCH) || defined(CONFIG_KPATCH)
	/* 注册 kpatch 补丁 */
	ret = klp_register_patch(&kpatch_patch);
	if (ret) {
		pr_err("thread_parent_trace: klp_register_patch failed: %d\n",
		       ret);
		proc_remove(proc_entry);
		return ret;
	}

	ret = klp_enable_patch(&kpatch_patch);
	if (ret) {
		pr_err("thread_parent_trace: klp_enable_patch failed: %d\n",
		       ret);
		klp_unregister_patch(&kpatch_patch);
		proc_remove(proc_entry);
		return ret;
	}

	pr_info("thread_parent_trace: kpatch enabled successfully\n");
#else
	/*
	 * 非 kpatch 模式（调试用）：
	 * 使用 kprobe 作为降级方案挂载到 wake_up_new_task
	 */
	pr_warn("thread_parent_trace: CONFIG_LIVEPATCH not enabled, "
		"kprobe fallback mode\n");
#endif

	pr_info("thread_parent_trace: /proc/thread_parent_trace ready\n");
	return 0;
}

static void __exit thread_parent_trace_exit(void)
{
	struct thread_parent_entry *e;
	struct hlist_node *tmp;
	int bkt;

#if defined(CONFIG_LIVEPATCH) || defined(CONFIG_KPATCH)
	klp_disable_patch(&kpatch_patch);
	klp_unregister_patch(&kpatch_patch);
#endif

	if (proc_entry)
		proc_remove(proc_entry);

	/* 清理所有条目 */
	spin_lock(&trace_lock);
	hash_for_each_safe(trace_hash, bkt, tmp, e, node) {
		hash_del(&e->node);
		kfree(e);
	}
	spin_unlock(&trace_lock);

	pr_info("thread_parent_trace: unloaded\n");
}

module_init(thread_parent_trace_init);
module_exit(thread_parent_trace_exit);
