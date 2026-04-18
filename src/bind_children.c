/*
 * bind_children.c — 将指定线程的所有子线程绑定到指定 CPU 核心
 *
 * 用法：
 *   ./bind_children <parent_pid> <cpu_list>
 *
 * 示例：
 *   ./bind_children 1234 0,1,2       # 绑定到 CPU 0,1,2
 *   ./bind_children 1234 4-7         # 绑定到 CPU 4,5,6,7
 *   ./bind_children 1234 all         # 绑定到所有 CPU
 *
 * 原理：
 *   1. 读取 /proc/<pid>/task/ 获取所有线程 TID
 *   2. 解析 CPU 列表构造 cpu_set_t
 *   3. 对每个子线程调用 sched_setaffinity()
 *
 * 这是一个用户空间程序，展示如何在不修改内核的情况下
 * 实现线程 CPU 亲和性管理。
 *
 * 如果需要在内核层面自动追踪并绑定（比如每当创建新线程就自动绑核），
 * 就需要 kpatch —— 这正是 thread_parent_trace 的用武之地。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <ctype.h>

/* gettid() 已在 glibc 2.30+ 中提供 */

#define MAX_CPUS CPU_SETSIZE
#define PROC_BUF_SIZE 4096

/* ──────────────────────────────────────────────────
 * 解析 CPU 列表
 * 支持格式：0,1,2 / 0-3 / 0,2-5,7 / all
 * ────────────────────────────────────────────────── */
static int parse_cpu_list(const char *list, cpu_set_t *set, int ncpus)
{
	CPU_ZERO(set);

	if (strcmp(list, "all") == 0) {
		for (int i = 0; i < ncpus; i++)
			CPU_SET(i, set);
		return ncpus;
	}

	const char *p = list;
	int count = 0;

	while (*p) {
		/* 跳过空白和逗号 */
		while (*p && (*p == ',' || isspace(*p)))
			p++;
		if (!*p)
			break;

		/* 解析起始值 */
		char *end;
		long start = strtol(p, &end, 10);
		if (end == p || start < 0 || start >= ncpus) {
			fprintf(stderr, "invalid CPU number at: %.10s\n", p);
			return -1;
		}

		p = end;

		if (*p == '-') {
			/* 范围：start-end */
			p++;
			long finish = strtol(p, &end, 10);
			if (end == p || finish < start || finish >= ncpus) {
				fprintf(stderr, "invalid CPU range: %ld-%.*s\n",
					start, 10, p);
				return -1;
			}
			p = end;

			for (long i = start; i <= finish; i++) {
				CPU_SET(i, set);
				count++;
			}
		} else {
			/* 单个值 */
			CPU_SET(start, set);
			count++;
		}

		if (*p == ',')
			p++;
	}

	return count;
}

/* ──────────────────────────────────────────────────
 * 获取指定 PID 的所有线程 TID
 * 通过读取 /proc/<pid>/task/ 目录
 * ────────────────────────────────────────────────── */
static int get_thread_ids(pid_t pid, pid_t **tids)
{
	char task_dir[64];
	snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", (int)pid);

	DIR *d = opendir(task_dir);
	if (!d) {
		fprintf(stderr, "cannot open %s: %s\n", task_dir, strerror(errno));
		return -1;
	}

	/* 第一遍：计数 */
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		count++;
	}

	if (count == 0) {
		closedir(d);
		return 0;
	}

	*tids = malloc(count * sizeof(pid_t));
	if (!*tids) {
		closedir(d);
		return -1;
	}

	/* 第二遍：收集 */
	rewinddir(d);
	int i = 0;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		(*tids)[i++] = (pid_t)atoi(ent->d_name);
	}

	closedir(d);
	return count;
}

/* ──────────────────────────────────────────────────
 * 打印线程当前的 CPU 亲和性
 * ────────────────────────────────────────────────── */
static void print_affinity(pid_t tid)
{
	cpu_set_t current;
	CPU_ZERO(&current);

	if (sched_getaffinity(tid, sizeof(current), &current) < 0) {
		printf("    tid %d: cannot read affinity (%s)\n",
		       (int)tid, strerror(errno));
		return;
	}

	printf("    tid %d: CPUs [", (int)tid);
	int first = 1;
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &current)) {
			printf("%s%d", first ? "" : ",", i);
			first = 0;
		}
	}
	printf("]\n");
}

/* ──────────────────────────────────────────────────
 * 设置线程的 CPU 亲和性
 * ────────────────────────────────────────────────── */
static int bind_thread(pid_t tid, const cpu_set_t *set)
{
	if (sched_setaffinity(tid, sizeof(*set), set) < 0) {
		fprintf(stderr, "sched_setaffinity(tid=%d) failed: %s\n",
			(int)tid, strerror(errno));
		return -1;
	}
	return 0;
}

/* ──────────────────────────────────────────────────
 * 打印 CPU_set 内容
 * ────────────────────────────────────────────────── */
static void print_cpuset(const char *label, const cpu_set_t *set)
{
	printf("%s: [", label);
	int first = 1;
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, set)) {
			printf("%s%d", first ? "" : ",", i);
			first = 0;
		}
	}
	printf("]\n");
}

/* ──────────────────────────────────────────────────
 * 演示模式：创建子线程然后绑核
 * ────────────────────────────────────────────────── */
#include <pthread.h>

#define DEMO_THREADS 4

struct demo_arg {
	int id;
	pid_t tid;  /* 线程自己填入 */
};

static void *demo_worker(void *arg)
{
	struct demo_arg *a = arg;
	a->tid = gettid();  /* 线程启动后记录自己的 TID */
	printf("  [demo] thread %d (tid=%d) started on CPU %d\n",
	       a->id, (int)a->tid, sched_getcpu());

	/* 模拟工作 */
	volatile long sum = 0;
	for (long i = 0; i < 100000000L; i++)
		sum += i;

	printf("  [demo] thread %d done (sum=%ld)\n", a->id, sum);
	return NULL;
}

static void run_demo(const cpu_set_t *target_set)
{
	pthread_t threads[DEMO_THREADS];
	struct demo_arg args[DEMO_THREADS];

	printf("\n=== Demo: 创建 %d 个子线程并绑核 ===\n\n", DEMO_THREADS);
	printf("Parent PID: %d\n\n", (int)getpid());

	/* 创建线程 */
	for (int i = 0; i < DEMO_THREADS; i++) {
		args[i].id = i;
		args[i].tid = 0;
		pthread_create(&threads[i], NULL, demo_worker, &args[i]);
	}

	/* 等线程启动并写入 TID */
	usleep(300000);

	/* 显示绑核前状态 */
	printf("\n--- 绑核前 ---\n");
	for (int i = 0; i < DEMO_THREADS; i++)
		print_affinity(args[i].tid);

	/* 绑核 */
	printf("\n--- 执行绑核 ---\n");
	int bound = 0;
	for (int i = 0; i < DEMO_THREADS; i++) {
		if (bind_thread(args[i].tid, target_set) == 0) {
			printf("  tid %d → bound\n", (int)args[i].tid);
			bound++;
		}
	}
	printf("  %d/%d threads bound\n", bound, DEMO_THREADS);

	/* 显示绑核后状态 */
	printf("\n--- 绑核后 ---\n");
	for (int i = 0; i < DEMO_THREADS; i++)
		print_affinity(args[i].tid);

	/* 等待完成 */
	for (int i = 0; i < DEMO_THREADS; i++)
		pthread_join(threads[i], NULL);

	printf("\n=== Demo 完成 ===\n");
}

/* ──────────────────────────────────────────────────
 * 主函数
 * ────────────────────────────────────────────────── */
static void usage(const char *prog)
{
	fprintf(stderr,
		"用法:\n"
		"  %s <pid> <cpu_list>     绑定指定进程的所有线程\n"
		"  %s demo <cpu_list>      演示模式（自动创建线程并绑核）\n"
		"\n"
		"cpu_list 格式:\n"
		"  0,1,2        指定 CPU\n"
		"  0-3          范围\n"
		"  0,2-5,7      混合\n"
		"  all          所有 CPU\n"
		"\n"
		"示例:\n"
		"  %s 1234 0,1\n"
		"  %s demo 2-5\n",
		prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus <= 0) {
		ncpus = 4;
		fprintf(stderr, "warning: cannot determine CPU count, assuming %d\n",
			ncpus);
	}
	printf("系统可用 CPU 数: %d\n\n", ncpus);

	/* 解析 CPU 列表 */
	cpu_set_t target_set;
	int cpu_count = parse_cpu_list(argv[2], &target_set, ncpus);
	if (cpu_count <= 0) {
		fprintf(stderr, "error: invalid CPU list: %s\n", argv[2]);
		return 1;
	}
	print_cpuset("目标 CPU", &target_set);

	/* 演示模式 */
	if (strcmp(argv[1], "demo") == 0) {
		run_demo(&target_set);
		return 0;
	}

	/* 正常模式：指定 PID */
	pid_t pid = (pid_t)atoi(argv[1]);
	if (pid <= 0) {
		fprintf(stderr, "error: invalid PID: %s\n", argv[1]);
		return 1;
	}

	/* 检查进程是否存在 */
	if (kill(pid, 0) < 0) {
		fprintf(stderr, "error: process %d does not exist: %s\n",
			(int)pid, strerror(errno));
		return 1;
	}

	/* 获取所有线程 */
	printf("\n扫描进程 %d 的线程...\n", (int)pid);
	pid_t *tids = NULL;
	int count = get_thread_ids(pid, &tids);
	if (count < 0)
		return 1;

	printf("找到 %d 个线程\n\n", count);

	/* 显示绑核前 */
	printf("--- 绑核前 ---\n");
	for (int i = 0; i < count; i++)
		print_affinity(tids[i]);

	/* 执行绑核 */
	printf("\n--- 执行绑核 ---\n");
	int bound = 0;
	int failed = 0;
	for (int i = 0; i < count; i++) {
		if (bind_thread(tids[i], &target_set) == 0) {
			printf("  tid %d → bound\n", (int)tids[i]);
			bound++;
		} else {
			failed++;
		}
	}

	/* 显示绑核后 */
	printf("\n--- 绑核后 ---\n");
	for (int i = 0; i < count; i++)
		print_affinity(tids[i]);

	printf("\n结果: %d bound, %d failed, %d total\n", bound, failed, count);

	free(tids);
	return failed > 0 ? 1 : 0;
}
