
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "sysemu/cpus.h"
#include "sysemu/hw_accel.h"
#include "sysemu/kvm.h" 
#include "sysemu/runstate.h"
#include "qapi/error.h"
#include "linux/kvm.h"
#include "hw/boards.h"
#include "qemu/main-loop.h"
#include "exec/address-spaces.h"
#include "exec/cpu-all.h"
#include "../kvm/kvm-cpus.h"
#include "../../../common_include/wavevm_protocol.h" 
#include "../../../common_include/wavevm_config.h"
#include "../../../common_include/wavevm_ioctl.h"
#include "wavevm-accel.h"
#include "qemu/thread.h"
#include "qemu/guest-random.h"
#include "tcg/tcg.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <limits.h>

extern void wavevm_apply_split_hint(int split);

// Per-vCPU Socket Pool to eliminate lock contention
static int *g_vcpu_socks = NULL;
static int g_configured_vcpus = 0;
static int g_wvm_debug_once = 0;

static void wavevm_try_import_split_from_peer(int sock)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    char path[64];
    FILE *fp;
    char envbuf[8192];
    size_t n;
    size_t i = 0;

    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0) {
        return;
    }

    snprintf(path, sizeof(path), "/proc/%ld/environ", (long)cred.pid);
    fp = fopen(path, "rb");
    if (!fp) {
        return;
    }

    n = fread(envbuf, 1, sizeof(envbuf) - 1, fp);
    fclose(fp);
    if (n == 0) {
        return;
    }
    envbuf[n] = '\0';

    while (i < n) {
        char *kv = &envbuf[i];
        size_t kv_len = strnlen(kv, n - i);

        if (kv_len == 0) {
            i++;
            continue;
        }

        if (strncmp(kv, "WVM_LOCAL_SPLIT=", 16) == 0) {
            char *endptr = NULL;
            long parsed = strtol(kv + 16, &endptr, 10);
            if (endptr && *endptr == '\0' && parsed >= 0 && parsed <= INT_MAX) {
                wavevm_apply_split_hint((int)parsed);
            }
            break;
        }

        i += kv_len + 1;
    }
#else
    (void)sock;
#endif
}

static int connect_to_master_helper(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *env_path;
    char fallback[128];

    if (sock < 0) {
        return -1;
    }

    env_path = getenv("WVM_ENV_SOCK_PATH");
    if (!env_path) {
        const char *inst_id = getenv("WVM_INSTANCE_ID");
        snprintf(fallback, sizeof(fallback), "/tmp/wvm_user_%s.sock",
                 inst_id ? inst_id : "0");
        env_path = fallback;
    }

    strncpy(addr.sun_path, env_path, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    wavevm_try_import_split_from_peer(sock);
    return sock;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

// TCG Helper Declarations (Defined in wavevm-tcg.c)
extern void wvm_tcg_get_state(CPUState *cpu, wvm_tcg_context_t *ctx);
extern void wvm_tcg_set_state(CPUState *cpu, wvm_tcg_context_t *ctx);
extern void wavevm_slave_vcpu_loop(CPUState *cpu);
static pthread_once_t g_wavevm_tcg_region_once = PTHREAD_ONCE_INIT;

static void wavevm_init_tcg_region_once(void)
{
    tcg_region_init();
}

struct wavevm_policy_ops {
    int (*schedule_policy)(int cpu_index);
};

// [Policy] Tiered Scheduling: Local vs Remote
static int remote_rpc_policy(int cpu_index) {
    //不再使用 WVM_LOCAL_CPU_COUNT 宏，而是使用动态变量
    if (cpu_index >= g_wvm_local_split) return 1; // 远程执行
    return 0; // 本地执行
}

static struct wavevm_policy_ops ops = { .schedule_policy = remote_rpc_policy };

static bool wavevm_valid_io_exit(const struct kvm_run *run)
{
    if (run->io.size != 1 && run->io.size != 2 &&
        run->io.size != 4 && run->io.size != 8) {
        return false;
    }
    if (run->io.count == 0 || run->io.count > 8) {
        return false;
    }
    return true;
}

static bool wavevm_valid_mmio_exit(const struct kvm_run *run)
{
    return run->mmio.len == 1 || run->mmio.len == 2 ||
           run->mmio.len == 4 || run->mmio.len == 8;
}

/* Keep local APIC/interrupt state and only import architectural sregs fields. */
static void wavevm_apply_arch_sregs(struct kvm_sregs *dst,
                                    const struct kvm_sregs *src)
{
    dst->cs = src->cs;
    dst->ds = src->ds;
    dst->es = src->es;
    dst->fs = src->fs;
    dst->gs = src->gs;
    dst->ss = src->ss;
    dst->tr = src->tr;
    dst->ldt = src->ldt;
    dst->gdt = src->gdt;
    dst->idt = src->idt;
    dst->cr0 = src->cr0;
    dst->cr2 = src->cr2;
    dst->cr3 = src->cr3;
    dst->cr4 = src->cr4;
    dst->cr8 = src->cr8;
    dst->efer = src->efer;
}

/* 
 * [物理意图] 充当远程 Slave 的“本地代理执行人”。
 * [关键逻辑] 当远程计算节点触发 PIO/MMIO 退出时，Master 在本地 QEMU 中代为执行该 I/O 操作并返回结果。
 * [后果] 解决了 I/O 设备物理位置的透明性。它让 Guest 以为显卡就在本地，即便真实的读写指令是在千里之外执行的。
 */
static void wavevm_handle_io(CPUState *cpu) {
    struct kvm_run *run = cpu->kvm_run;
    uint16_t port = run->io.port;
    void *data = (uint8_t *)run + run->io.data_offset;
    int is_write = (run->io.direction == KVM_EXIT_IO_OUT);

    /* [FIX] 处理 string I/O (REP INS/OUTS)：count 可能 > 1，需逐个迭代 */
    for (uint32_t i = 0; i < run->io.count; i++) {
        address_space_rw(&address_space_io, port, MEMTXATTRS_UNSPECIFIED,
                         data, run->io.size, is_write);
        data = (uint8_t *)data + run->io.size;
    }
}

static void wavevm_handle_mmio(CPUState *cpu) {
    struct kvm_run *run = cpu->kvm_run;
    hwaddr addr = run->mmio.phys_addr;
    void *data = run->mmio.data;

    address_space_rw(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                     data, run->mmio.len,
                     run->mmio.is_write);
}

/* 
 * [物理意图] 实现 CPU 核心的“跨物理节点迁移（Context Serialization）”。
 * [关键逻辑] 将 CPU 的全量寄存器状态、MSR 及时钟计数器进行二进制打包，并发射至远程 Slave。
 * [后果] 实现了算力的透明流动。若序列化逻辑不严谨（如漏掉 rflags），远程执行会立即触发 Guest Kernel Panic。
 */
static void wavevm_remote_exec(CPUState *cpu) {
    // 动态边界检查
    if (cpu->cpu_index >= g_configured_vcpus) return;
    
    int vcpu_sock = g_vcpu_socks[cpu->cpu_index];
    if (!g_wvm_debug_once) {
        g_wvm_debug_once = 1;
        fprintf(stderr, "[WVM-DBG] cpu=%d kvm_enabled=%d vcpu_sock=%d split=%d\n",
                cpu->cpu_index, kvm_enabled(), vcpu_sock, g_wvm_local_split);
    }
    
    // 如果没有 socket (说明是 Kernel Mode), 走 IOCTL 路径
    if (vcpu_sock < 0) { 
        WaveVMAccelState *s = WAVEVM_ACCEL(current_machine->accelerator);
        if (s->mode != WVM_MODE_KERNEL) {
            g_usleep(1000); // 异常状态，非 Kernel 且无 Socket
            return;
        }

        // 1. 准备请求结构体
        struct wvm_ipc_cpu_run_req req;
        struct wvm_ipc_cpu_run_ack ack;
        memset(&req, 0, sizeof(req));
        
        req.slave_id = WVM_NODE_AUTO_ROUTE;
        req.vcpu_index = cpu->cpu_index;

        // 2. 序列化 CPU 状态
        if (kvm_enabled()) {
            struct kvm_regs kregs;
            struct kvm_sregs ksregs;
            cpu_synchronize_state(cpu);
            ioctl(cpu->kvm_fd, KVM_GET_REGS, &kregs);
            ioctl(cpu->kvm_fd, KVM_GET_SREGS, &ksregs);

            req.mode_tcg = 0;
            req.ctx.kvm.rax = kregs.rax; req.ctx.kvm.rbx = kregs.rbx; req.ctx.kvm.rcx = kregs.rcx;
            req.ctx.kvm.rdx = kregs.rdx; req.ctx.kvm.rsi = kregs.rsi; req.ctx.kvm.rdi = kregs.rdi;
            req.ctx.kvm.rsp = kregs.rsp; req.ctx.kvm.rbp = kregs.rbp;
            req.ctx.kvm.r8  = kregs.r8;  req.ctx.kvm.r9  = kregs.r9;  req.ctx.kvm.r10 = kregs.r10;
            req.ctx.kvm.r11 = kregs.r11; req.ctx.kvm.r12 = kregs.r12; req.ctx.kvm.r13 = kregs.r13;
            req.ctx.kvm.r14 = kregs.r14; req.ctx.kvm.r15 = kregs.r15;
            req.ctx.kvm.rip = kregs.rip; req.ctx.kvm.rflags = kregs.rflags;
            memcpy(req.ctx.kvm.sregs_data, &ksregs, sizeof(ksregs));
        } else {
            req.mode_tcg = 1;
            wvm_tcg_get_state(cpu, &req.ctx.tcg);
        }

        // 3. 陷入内核 (Trap into Kernel)
        // 这一步会阻塞，直到远程执行完毕并返回结果
        fprintf(stderr, "[WVM-DBG] kernel path cpu=%d target=%u mode_tcg=%u\n",
                cpu->cpu_index, req.slave_id, req.mode_tcg);
        int ret = ioctl(s->dev_fd, IOCTL_WVM_REMOTE_RUN, &req);
        
        if (ret < 0) {
            //fprintf(stderr, "WaveVM: Remote Run IOCTL failed: %s\n", strerror(errno));
            return;
        }

        // 4. 反序列化结果 (直接复用 req 的内存空间读取 ack，或者使用 memcpy)
        // 注意：内核将 Ack 数据回写到了 req 指针所在的内存
        memcpy(&ack, &req, sizeof(ack)); 

        if (ack.mode_tcg) {
            wvm_tcg_set_state(cpu, &ack.ctx.tcg);
        } else {
            struct kvm_regs kregs;
            struct kvm_sregs ksregs;
            wvm_kvm_context_t *kctx = &ack.ctx.kvm;

            kregs.rax = kctx->rax; kregs.rbx = kctx->rbx; kregs.rcx = kctx->rcx; 
            kregs.rdx = kctx->rdx; kregs.rsi = kctx->rsi; kregs.rdi = kctx->rdi;
            kregs.rsp = kctx->rsp; kregs.rbp = kctx->rbp;
            kregs.r8 = kctx->r8;   kregs.r9 = kctx->r9;   kregs.r10 = kctx->r10; 
            kregs.r11 = kctx->r11; kregs.r12 = kctx->r12; kregs.r13 = kctx->r13;
            kregs.r14 = kctx->r14; kregs.r15 = kctx->r15;
            kregs.rip = kctx->rip; kregs.rflags = kctx->rflags;
            
            ioctl(cpu->kvm_fd, KVM_GET_SREGS, &ksregs);
            wavevm_apply_arch_sregs(&ksregs, (const struct kvm_sregs *)kctx->sregs_data);
            ioctl(cpu->kvm_fd, KVM_SET_SREGS, &ksregs);
            ioctl(cpu->kvm_fd, KVM_SET_REGS, &kregs);
            
            struct kvm_run *run = cpu->kvm_run;
            run->exit_reason = kctx->exit_reason;

            if (kctx->exit_reason == KVM_EXIT_IO) {
                run->io.direction = kctx->io.direction;
                run->io.size      = kctx->io.size;
                run->io.port      = kctx->io.port;
                run->io.count     = kctx->io.count;
                if (run->io.direction == KVM_EXIT_IO_OUT) {
                    size_t mmap_size = ioctl(cpu->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
                    if (run->io.data_offset + run->io.size * run->io.count <= mmap_size) {
                        uint8_t *io_ptr = (uint8_t *)run + run->io.data_offset;
                        size_t io_bytes = run->io.size * run->io.count;
                        if (io_bytes > sizeof(kctx->io.data)) {
                            io_bytes = sizeof(kctx->io.data);
                        }
                        memcpy(io_ptr, kctx->io.data, io_bytes);
                    }
                }
                if (!wavevm_valid_io_exit(run)) {
                    return;
                }
                qemu_mutex_lock_iothread();
                wavevm_handle_io(cpu);
                qemu_mutex_unlock_iothread();
            } 
            else if (kctx->exit_reason == KVM_EXIT_MMIO) {
                run->mmio.phys_addr = kctx->mmio.phys_addr;
                run->mmio.len       = kctx->mmio.len;
                run->mmio.is_write  = kctx->mmio.is_write;
                memcpy(run->mmio.data, kctx->mmio.data, 8);
                if (!wavevm_valid_mmio_exit(run)) {
                    return;
                }
                qemu_mutex_lock_iothread();
                wavevm_handle_mmio(cpu);
                qemu_mutex_unlock_iothread();
            }
        }
        return; 
    }

    // 准备发送缓冲区
    struct wvm_ipc_header_t ipc_hdr = {
        .type = WVM_IPC_TYPE_CPU_RUN,
        .len = sizeof(struct wvm_ipc_cpu_run_req),
    };
    struct wvm_ipc_cpu_run_req req;
    struct wvm_ipc_cpu_run_ack ack;
    memset(&req, 0, sizeof(req));

    req.slave_id = WVM_NODE_AUTO_ROUTE;
    req.vcpu_index = cpu->cpu_index;

    // 1. 序列化 CPU 状态 (Serialization)
    if (kvm_enabled()) {
        struct kvm_regs kregs;
        struct kvm_sregs ksregs;
        cpu_synchronize_state(cpu);
        ioctl(cpu->kvm_fd, KVM_GET_REGS, &kregs);
        ioctl(cpu->kvm_fd, KVM_GET_SREGS, &ksregs);

        req.mode_tcg = 0;

        wvm_kvm_context_t *kctx = &req.ctx.kvm;
        kctx->rax = kregs.rax; kctx->rbx = kregs.rbx; kctx->rcx = kregs.rcx;
        kctx->rdx = kregs.rdx; kctx->rsi = kregs.rsi; kctx->rdi = kregs.rdi;
        kctx->rsp = kregs.rsp; kctx->rbp = kregs.rbp;
        kctx->r8  = kregs.r8;  kctx->r9  = kregs.r9;  kctx->r10 = kregs.r10;
        kctx->r11 = kregs.r11; kctx->r12 = kregs.r12; kctx->r13 = kregs.r13;
        kctx->r14 = kregs.r14; kctx->r15 = kregs.r15;
        kctx->rip = kregs.rip; kctx->rflags = kregs.rflags;
        memcpy(kctx->sregs_data, &ksregs, sizeof(ksregs));

        /* [FIX] 携带上一轮 IO/MMIO 处理结果：
         * 若上次远程执行触发了 IO IN 或 MMIO READ，Master 已在本地
         * wavevm_handle_io/mmio 中将设备读结果填入 run->io.data / mmio.data。
         * Slave 需要这些数据在下次 KVM_RUN 时完成未决的 IO 指令。 */
        struct kvm_run *run = cpu->kvm_run;
        kctx->exit_reason = run->exit_reason;
        if (run->exit_reason == KVM_EXIT_IO) {
            kctx->io.direction = run->io.direction;
            kctx->io.size      = run->io.size;
            kctx->io.port      = run->io.port;
            kctx->io.count     = run->io.count;
            if (run->io.direction == KVM_EXIT_IO_IN) {
                size_t io_bytes = (size_t)run->io.size * run->io.count;
                if (io_bytes > sizeof(kctx->io.data)) io_bytes = sizeof(kctx->io.data);
                memcpy(kctx->io.data, (uint8_t *)run + run->io.data_offset, io_bytes);
            }
        } else if (run->exit_reason == KVM_EXIT_MMIO) {
            kctx->mmio.phys_addr = run->mmio.phys_addr;
            kctx->mmio.len       = run->mmio.len;
            kctx->mmio.is_write  = run->mmio.is_write;
            if (!run->mmio.is_write) {
                memcpy(kctx->mmio.data, run->mmio.data, 8);
            }
        }
    } else {
        req.mode_tcg = 1;
        wvm_tcg_get_state(cpu, &req.ctx.tcg);
    }

    // 2. IPC 发送 (wvm_ipc_header + wvm_ipc_cpu_run_req)
    if (write_full(vcpu_sock, &ipc_hdr, sizeof(ipc_hdr)) < 0) {
        return;
    }
    if (write_full(vcpu_sock, &req, sizeof(req)) < 0) {
        return;
    }

    // 3. IPC 接收 ACK (阻塞本线程，不影响其他 vCPU)
    if (read_full(vcpu_sock, &ack, sizeof(ack)) < 0) {
        return;
    }
    if (ack.status < 0) {
        return;
    }

    // 4. 反序列化 CPU 状态
    if (ack.mode_tcg) {
        wvm_tcg_set_state(cpu, &ack.ctx.tcg);
    } else {
        struct kvm_regs kregs;
        struct kvm_sregs ksregs;
        wvm_kvm_context_t *kctx = &ack.ctx.kvm;

        kregs.rax = kctx->rax; kregs.rbx = kctx->rbx; kregs.rcx = kctx->rcx; 
        kregs.rdx = kctx->rdx; kregs.rsi = kctx->rsi; kregs.rdi = kctx->rdi;
        kregs.rsp = kctx->rsp; kregs.rbp = kctx->rbp;
        kregs.r8 = kctx->r8;   kregs.r9 = kctx->r9;   kregs.r10 = kctx->r10; 
        kregs.r11 = kctx->r11; kregs.r12 = kctx->r12; kregs.r13 = kctx->r13;
        kregs.r14 = kctx->r14; kregs.r15 = kctx->r15;
        kregs.rip = kctx->rip; kregs.rflags = kctx->rflags;
        
        ioctl(cpu->kvm_fd, KVM_GET_SREGS, &ksregs);
        wavevm_apply_arch_sregs(&ksregs, (const struct kvm_sregs *)kctx->sregs_data);
        ioctl(cpu->kvm_fd, KVM_SET_SREGS, &ksregs);
        ioctl(cpu->kvm_fd, KVM_SET_REGS, &kregs);
        
        // 5. Replay IO/MMIO
        struct kvm_run *run = cpu->kvm_run;
        run->exit_reason = kctx->exit_reason;

        if (kctx->exit_reason == KVM_EXIT_IO) {
            run->io.direction = kctx->io.direction;
            run->io.size      = kctx->io.size;
            run->io.port      = kctx->io.port;
            run->io.count     = kctx->io.count;
            
            if (run->io.direction == KVM_EXIT_IO_OUT) {
                size_t mmap_size = ioctl(cpu->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
                if (run->io.data_offset + run->io.size * run->io.count <= mmap_size) {
                    uint8_t *io_ptr = (uint8_t *)run + run->io.data_offset;
                    size_t io_bytes = run->io.size * run->io.count;
                    if (io_bytes > sizeof(kctx->io.data)) {
                        io_bytes = sizeof(kctx->io.data);
                    }
                    memcpy(io_ptr, kctx->io.data, io_bytes);
                }
            }
            if (!wavevm_valid_io_exit(run)) {
                return;
            }
            wavevm_handle_io(cpu);
        } 
        else if (kctx->exit_reason == KVM_EXIT_MMIO) {
            run->mmio.phys_addr = kctx->mmio.phys_addr;
            run->mmio.len       = kctx->mmio.len;
            run->mmio.is_write  = kctx->mmio.is_write;
            memcpy(run->mmio.data, kctx->mmio.data, 8);
            if (!wavevm_valid_mmio_exit(run)) {
                return;
            }
            wavevm_handle_mmio(cpu);
        }
    }
}

/* 
 * [物理意图] 重新定义 vCPU 的“心脏跳动规律”。
 * [关键逻辑] 拦截标准的 KVM_RUN 循环，根据调度策略决定本轮指令是交给本地 KVM 还是进行远程上下文序列化。
 * [后果] 这是超级虚拟机的总节拍器。它保证了在异构算力环境下，vCPU 能够平滑地在本地与远程之间切换执行流。
 */
/*
 * WaveVM vCPU thread function -- aligned with standard QEMU MTTCG skeleton.
 *
 * Key difference from vanilla MTTCG: the scheduling branch that routes
 * vCPUs to either local cpu_exec() or remote wavevm_remote_exec() based
 * on g_wvm_local_split.  Everything else (iothread lock discipline,
 * cpu_exec_start/end, exit_request, qemu_wait_io_event every iteration)
 * follows the standard QEMU pattern so that vm_start()/resume_all_vcpus()
 * and the full VM lifecycle management work correctly.
 */
static void *wavevm_cpu_thread_fn(void *arg) {
    CPUState *cpu = arg;
    char *role = getenv("WVM_ROLE");
    bool is_slave = (role && strcmp(role, "SLAVE") == 0);

    rcu_register_thread();
    /*
     * Only register TCG context for vCPUs that will execute locally.
     * Remote-only vCPUs (cpu_index >= g_wvm_local_split on Master) only
     * do RPC forwarding and never call cpu_exec(), so they do not need
     * a TCG region.  Without this guard, tcg_region_initial_alloc fails
     * because non-MTTCG mode only provisions a single TCG region.
     */
    if (!kvm_enabled() && (is_slave || cpu->cpu_index < g_wvm_local_split)) {
        tcg_register_thread();
    }

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* Slave: hand off to dedicated slave loop (holds its own lock discipline) */
    if (is_slave) {
        qemu_mutex_unlock_iothread();
        wavevm_slave_vcpu_loop(cpu);
        goto out;
    }

    /* KVM bootstrap for this vCPU (if KVM is the underlying engine) */
    if (kvm_enabled()) {
        kvm_init_vcpu(cpu, &error_fatal);
        kvm_init_cpu_signals(cpu);
        cpu_synchronize_state(cpu);
    }

    /*
     * Trigger an exit from the first cpu_exec() call so we cycle through
     * qemu_wait_io_event() and properly synchronize with vm_start().
     */
    cpu->exit_request = 1;

    /*
     * Main vCPU loop -- mirrors tcg_cpu_thread_fn (MTTCG) structure.
     * The iothread lock is held at the top of every iteration.
     */
    do {
        if (cpu_can_run(cpu)) {
            /* Remote vCPU: forward to Slave via RPC */
            if (ops.schedule_policy(cpu->cpu_index) == 1) {
                qemu_mutex_unlock_iothread();
                wavevm_remote_exec(cpu);
                qemu_mutex_lock_iothread();
            }
            /* Local vCPU: 复用 QEMU 原生 kvm_cpu_exec()
             * 包含完整的 kvm_arch_pre_run (中断注入/APIC 同步)、
             * kvm_arch_post_run (状态回收)、kvm_arch_put_registers
             * (脏寄存器推送) 以及全部 exit reason 处理。
             */
            else if (kvm_enabled()) {
                int r = kvm_cpu_exec(cpu);
                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                }
            } else {
                /* TCG local execution */
                int r;
                qemu_mutex_unlock_iothread();
                cpu_exec_start(cpu);
                r = cpu_exec(cpu);
                cpu_exec_end(cpu);
                qemu_mutex_lock_iothread();

                switch (r) {
                case EXCP_DEBUG:
                    cpu_handle_guest_debug(cpu);
                    break;
                case EXCP_HALTED:
                    break;
                case EXCP_ATOMIC:
                    qemu_mutex_unlock_iothread();
                    cpu_exec_step_atomic(cpu);
                    qemu_mutex_lock_iothread();
                    break;
                default:
                    break;
                }
            }
        }

        qatomic_mb_set(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    if (kvm_enabled() && cpu->kvm_fd >= 0) {
        kvm_destroy_vcpu(cpu);
    }
    cpu_thread_signal_destroyed(cpu);
    qemu_mutex_unlock_iothread();
out:
    rcu_unregister_thread();
    return NULL;
}

/* 
 * [物理意图] 为每个逻辑核心开辟专属的“高速公路（Per-vCPU Socket Pool）”。
 * [关键逻辑] 在 vCPU 创建瞬间建立独立的 IPC 通道，彻底消除多核并发时的 Socket 锁竞争。
 * [后果] 实现了算力的线性扩展能力。这是 V30 支撑一亿核心的关键，保证了 vCPU 数量增加时通信延迟不上升。
 */
void wavevm_start_vcpu_thread(CPUState *cpu) {
    char thread_name[VCPU_THREAD_NAME_SIZE];
    WaveVMAccelState *s = WAVEVM_ACCEL(current_machine->accelerator);
    char *role = getenv("WVM_ROLE");

    static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

    if (!kvm_enabled()) {
        pthread_once(&g_wavevm_tcg_region_once, wavevm_init_tcg_region_once);
    }
    
    // 双重检查锁定 (Double-Checked Locking) 优化性能，或者直接加锁也行（毕竟只执行一次）
    if (!g_vcpu_socks) {
        pthread_mutex_lock(&g_init_lock);
        // 再次检查，防止在等待锁的过程中已被其他线程初始化
        if (!g_vcpu_socks) {
            g_configured_vcpus = (current_machine && current_machine->smp.cpus > 0) ?
                                 current_machine->smp.cpus : 1;
            
            g_vcpu_socks = g_malloc0(sizeof(int) * g_configured_vcpus);
            for (int i = 0; i < g_configured_vcpus; i++) {
                g_vcpu_socks[i] = -1;
            }
        }
        pthread_mutex_unlock(&g_init_lock);
    }

    if (s->mode == WVM_MODE_USER && !(role && strcmp(role, "SLAVE") == 0)) {
        // 动态边界检查
        if (cpu->cpu_index < g_configured_vcpus) {
            g_vcpu_socks[cpu->cpu_index] = connect_to_master_helper();
            if (g_vcpu_socks[cpu->cpu_index] < 0) {
                fprintf(stderr, "vCPU %d failed to connect master!\n", cpu->cpu_index);
                exit(1);
            }
        }
    } else {
        // Kernel 模式和 Slave 模式不使用此机制
        g_vcpu_socks[cpu->cpu_index] = -1; 
    }
    
    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/WVM", cpu->cpu_index);
    
    qemu_thread_create(cpu->thread, thread_name, wavevm_cpu_thread_fn, cpu, QEMU_THREAD_JOINABLE);
}
