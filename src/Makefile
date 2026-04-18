# kpatch 构建配置
# 使用 utopOS 管理内核补丁的源码 → 构建 → 安装流程

obj-m += thread_parent_trace.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# kpatch-build 集成
KPATCH_BUILD ?= kpatch-build

# 内核版本
KVER ?= $(shell uname -r)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# ──────────────────────────────────────────────────
# 方式 1：标准内核模块（开发/测试用）
# ──────────────────────────────────────────────────
install-mod: all
	insmod thread_parent_trace.ko

uninstall-mod:
	rmmod thread_parent_trace

# ──────────────────────────────────────────────────
# 方式 2：kpatch 构建（生产用，需要 kpatch-build）
#
# kpatch-build 的工作流程：
#   1. 读取本源文件，找到要替换的函数（patched_wake_up_new_task）
#   2. 编译原始内核源码和补丁后的源码
#   3. 对比两者的目标文件，生成差异 .ko
#   4. 输出的 .ko 只包含差异指令，体积小，加载快
#
# 对比其他方案：
#   - ftrace: 需要手动写 ftrace handler，侵入性强
#   - eBPF: 受 verifier 限制，不能修改内核函数逻辑
#   - 纯模块: 只能 hook 导出符号，不能替换非导出函数
#   - kpatch: 可以替换任意内核函数，热加载，无需重启
# ──────────────────────────────────────────────────
kpatch-build:
	$(KPATCH_BUILD) \
		-s $(KDIR) \
		-v /boot/vmlinuz-$(KVER) \
		-t vmlinux \
		--name thread_parent_trace \
		thread_parent_trace.c

kpatch-install:
	kpatch load kpatch-thread_parent_trace.ko

kpatch-uninstall:
	kpatch unload thread_parent_trace

# ──────────────────────────────────────────────────
# 使用 utopOS 工作流
# ──────────────────────────────────────────────────
utop-build:
	utop build run linux --patch thread-parent-trace.patch

utop-verify:
	utop verify linux

utop-install:
	utop install linux

.PHONY: all clean install-mod uninstall-mod kpatch-build kpatch-install kpatch-uninstall utop-build utop-verify utop-install
