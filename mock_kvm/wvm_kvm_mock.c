/*
 * WaveVM KVM Mock Library (LD_PRELOAD)
 *
 * Intercepts /dev/kvm open + KVM ioctl calls to simulate a KVM environment
 * on hosts without hardware virtualization (e.g. nested VMware VMs).
 *
 * Usage:
 *   LD_PRELOAD=./libwvm_kvm_mock.so ./wavevm_node_slave ...
 *   LD_PRELOAD=./libwvm_kvm_mock.so qemu-system-x86_64 -accel wavevm ...
 *
 * Environment:
 *   WVM_MOCK_LOG=0|1|2   Log verbosity (0=quiet, 1=info, 2=debug)
 *   WVM_MOCK_DIRTY_PCT=N Dirty page percentage per GET_DIRTY_LOG (default 5)
 *   WVM_MOCK_RUN_USEC=N  Simulated KVM_RUN delay in usec (default 100)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/kvm.h>
#include <pthread.h>

#include "wvm_kvm_mock_internal.h"

/* ================================================================== */
/*  Global state                                                       */
/* ================================================================== */

mock_global_t g_mock = { .initialized = false };

/* Real syscall function pointers (resolved via dlsym) */
static int     (*real_open)(const char *, int, ...) = NULL;
static int     (*real_close)(int) = NULL;
static int     (*real_ioctl)(int, unsigned long, ...) = NULL;
static void *  (*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int     (*real_access)(const char *, int) = NULL;

/* Tuning */
static int      g_dirty_pct  = 5;
static int      g_run_usec   = 100;

/* ================================================================== */
/*  Lazy initialization of real function pointers                      */
/* ================================================================== */

__attribute__((constructor))
static void mock_lib_init(void) {
    real_open   = dlsym(RTLD_NEXT, "open");
    real_close  = dlsym(RTLD_NEXT, "close");
    real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    real_mmap   = dlsym(RTLD_NEXT, "mmap");
    real_access = dlsym(RTLD_NEXT, "access");

    mock_init_once();

    const char *env_dirty = getenv("WVM_MOCK_DIRTY_PCT");
    if (env_dirty) g_dirty_pct = atoi(env_dirty);
    const char *env_run = getenv("WVM_MOCK_RUN_USEC");
    if (env_run) g_run_usec = atoi(env_run);

    MOCK_INFO("KVM Mock Library loaded (dirty_pct=%d, run_usec=%d)", g_dirty_pct, g_run_usec);
}

/* Allocate a mock fd using memfd_create (gives us a real fd number) */
static int alloc_mock_fd(const char *name) {
    int fd = memfd_create(name, 0);
    if (fd < 0) {
        /* Fallback: dup /dev/null */
        int nfd = real_open("/dev/null", O_RDWR);
        return nfd;
    }
    return fd;
}

/* ================================================================== */
/*  open() interception                                                */
/* ================================================================== */

int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname && strcmp(pathname, "/dev/kvm") == 0) {
        int fd = alloc_mock_fd("mock-kvm-system");
        if (fd >= 0) {
            mock_fd_entry_t *e = mock_register_fd(fd, MOCK_FD_KVM_SYSTEM);
            if (e) {
                MOCK_INFO("open(\"/dev/kvm\") -> mock fd %d", fd);
                return fd;
            }
            real_close(fd);
        }
        errno = ENODEV;
        return -1;
    }

    if (pathname && strcmp(pathname, "/dev/wavevm") == 0) {
        MOCK_DEBUG("open(\"/dev/wavevm\") -> ENOENT (not mocked)");
        errno = ENOENT;
        return -1;
    }

    /* Pass through to real open */
    if (flags & O_CREAT)
        return real_open(pathname, flags, mode);
    else
        return real_open(pathname, flags);
}

/* Also intercept open64 which some glibc versions use */
int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname && strcmp(pathname, "/dev/kvm") == 0) {
        return open(pathname, flags, mode);
    }
    if (pathname && strcmp(pathname, "/dev/wavevm") == 0) {
        return open(pathname, flags, mode);
    }

    if (flags & O_CREAT)
        return real_open(pathname, flags, mode);
    else
        return real_open(pathname, flags);
}

/* ================================================================== */
/*  access() interception                                              */
/* ================================================================== */

int access(const char *pathname, int mode) {
    if (pathname && strcmp(pathname, "/dev/kvm") == 0) {
        MOCK_DEBUG("access(\"/dev/kvm\") -> 0 (mocked)");
        return 0;
    }
    return real_access(pathname, mode);
}

/* ================================================================== */
/*  close() interception                                               */
/* ================================================================== */

int close(int fd) {
    mock_fd_entry_t *e = mock_lookup_fd(fd);
    if (e) {
        MOCK_DEBUG("close(mock fd %d, type=%d)", fd, e->type);
        mock_unregister_fd(fd);
    }
    return real_close(fd);
}

/* ================================================================== */
/*  ioctl() interception - KVM System FD                               */
/* ================================================================== */

static int mock_ioctl_kvm_system(mock_fd_entry_t *e, unsigned long request, void *argp) {
    (void)e;

    switch (request) {
    case KVM_GET_API_VERSION:
        MOCK_DEBUG("KVM_GET_API_VERSION -> 12");
        return 12;

    case KVM_CREATE_VM: {
        int vm_fd = alloc_mock_fd("mock-kvm-vm");
        if (vm_fd < 0) return -1;
        mock_fd_entry_t *ve = mock_register_fd(vm_fd, MOCK_FD_KVM_VM);
        if (!ve) { real_close(vm_fd); errno = ENOMEM; return -1; }
        MOCK_INFO("KVM_CREATE_VM -> mock fd %d", vm_fd);
        return vm_fd;
    }

    case KVM_GET_VCPU_MMAP_SIZE: {
        /* Return page-aligned size for kvm_run */
        int sz = (sizeof(struct kvm_run) + 4095) & ~4095;
        if (sz < 4096) sz = 4096;
        MOCK_DEBUG("KVM_GET_VCPU_MMAP_SIZE -> %d", sz);
        return sz;
    }

    case KVM_CHECK_EXTENSION: {
        int ext = (int)(intptr_t)argp;
        int ret = 0;
        switch (ext) {
        case KVM_CAP_IRQCHIP:           ret = 1; break;
        case KVM_CAP_USER_MEMORY:        ret = 1; break;
        case KVM_CAP_SET_TSS_ADDR:       ret = 1; break;
        case KVM_CAP_EXT_CPUID:          ret = 1; break;
        case KVM_CAP_NR_VCPUS:           ret = 64; break;
        case KVM_CAP_MAX_VCPUS:          ret = 64; break;
        case KVM_CAP_NR_MEMSLOTS:        ret = 32; break;
        case KVM_CAP_PIT2:               ret = 1; break;
        case KVM_CAP_IRQ_ROUTING:        ret = 1; break;
        case KVM_CAP_IOEVENTFD:          ret = 1; break;
        case KVM_CAP_IRQFD:             ret = 1; break;
        case KVM_CAP_MCE:               ret = 1; break;
        case KVM_CAP_ADJUST_CLOCK:       ret = 1; break;
        case KVM_CAP_SIGNAL_MSI:         ret = 1; break;
        case KVM_CAP_READONLY_MEM:       ret = 1; break;
        case KVM_CAP_MP_STATE:           ret = 1; break;
        case KVM_CAP_IMMEDIATE_EXIT:     ret = 1; break;
        default:
            MOCK_DEBUG("KVM_CHECK_EXTENSION(%d) -> 0 (unknown)", ext);
            ret = 0;
            break;
        }
        return ret;
    }

    case KVM_GET_SUPPORTED_CPUID: {
        /* Provide minimal CPUID: just the basic entry */
        struct kvm_cpuid2 *cpuid = (struct kvm_cpuid2 *)argp;
        if (cpuid->nent < 1) {
            errno = E2BIG;
            cpuid->nent = 1;
            return -1;
        }
        cpuid->nent = 1;
        memset(&cpuid->entries[0], 0, sizeof(struct kvm_cpuid_entry2));
        cpuid->entries[0].function = 0;
        cpuid->entries[0].eax = 0x0D;  /* max basic CPUID leaf */
        cpuid->entries[0].ebx = 0x756E6547; /* "Genu" */
        cpuid->entries[0].ecx = 0x6C65746E; /* "ntel" */
        cpuid->entries[0].edx = 0x49656E69; /* "ineI" */
        MOCK_DEBUG("KVM_GET_SUPPORTED_CPUID -> 1 entry");
        return 0;
    }

    default:
        MOCK_DEBUG("KVM_SYSTEM ioctl 0x%lx -> 0 (pass-through no-op)", request);
        return 0;
    }
}

/* ================================================================== */
/*  ioctl() interception - VM FD                                       */
/* ================================================================== */

static int mock_ioctl_vm(mock_fd_entry_t *e, unsigned long request, void *argp) {
    mock_vm_state_t *vm = &e->state.vm;

    switch (request) {
    case KVM_CREATE_VCPU: {
        int vcpu_id = (int)(intptr_t)argp;
        int vcpu_fd = alloc_mock_fd("mock-kvm-vcpu");
        if (vcpu_fd < 0) return -1;
        mock_fd_entry_t *ve = mock_register_fd(vcpu_fd, MOCK_FD_KVM_VCPU);
        if (!ve) { real_close(vcpu_fd); errno = ENOMEM; return -1; }

        /* Pre-allocate kvm_run region */
        size_t run_sz = (sizeof(struct kvm_run) + 4095) & ~4095;
        if (run_sz < 4096) run_sz = 4096;
        void *run_mem = real_mmap(NULL, run_sz, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (run_mem == MAP_FAILED) {
            mock_unregister_fd(vcpu_fd);
            real_close(vcpu_fd);
            errno = ENOMEM;
            return -1;
        }
        memset(run_mem, 0, run_sz);

        ve->state.vcpu.run = (struct kvm_run *)run_mem;
        ve->state.vcpu.run_size = run_sz;
        ve->state.vcpu.vcpu_id = vcpu_id;
        ve->state.vcpu.run_count = 0;

        /* Initialize sregs to a sane x86 real-mode state */
        struct kvm_sregs *sr = &ve->state.vcpu.sregs;
        sr->cs.base = 0; sr->cs.limit = 0xFFFF; sr->cs.selector = 0;
        sr->ds.base = 0; sr->ds.limit = 0xFFFF; sr->ds.selector = 0;
        sr->es.base = 0; sr->es.limit = 0xFFFF; sr->es.selector = 0;
        sr->ss.base = 0; sr->ss.limit = 0xFFFF; sr->ss.selector = 0;
        sr->fs.base = 0; sr->fs.limit = 0xFFFF; sr->fs.selector = 0;
        sr->gs.base = 0; sr->gs.limit = 0xFFFF; sr->gs.selector = 0;
        sr->cr0 = 0x10;  /* ET bit */

        MOCK_INFO("KVM_CREATE_VCPU(id=%d) -> mock fd %d", vcpu_id, vcpu_fd);
        return vcpu_fd;
    }

    case KVM_SET_USER_MEMORY_REGION: {
        struct kvm_userspace_memory_region *region =
            (struct kvm_userspace_memory_region *)argp;
        if (region && region->slot < MOCK_MAX_MEMSLOTS) {
            mock_memslot_t *s = &vm->slots[region->slot];
            s->slot = region->slot;
            s->flags = region->flags;
            s->guest_phys_addr = region->guest_phys_addr;
            s->memory_size = region->memory_size;
            s->userspace_addr = region->userspace_addr;
            s->active = true;
            MOCK_DEBUG("KVM_SET_USER_MEMORY_REGION: slot=%u gpa=0x%llx size=0x%llx",
                       region->slot, region->guest_phys_addr, region->memory_size);
        }
        return 0;
    }

    case KVM_SET_TSS_ADDR:
        MOCK_DEBUG("KVM_SET_TSS_ADDR -> 0");
        return 0;

    case KVM_SET_IDENTITY_MAP_ADDR:
        MOCK_DEBUG("KVM_SET_IDENTITY_MAP_ADDR -> 0");
        return 0;

    case KVM_CREATE_IRQCHIP:
        vm->irqchip_created = true;
        MOCK_DEBUG("KVM_CREATE_IRQCHIP -> 0");
        return 0;

    case KVM_IRQ_LINE:
        MOCK_DEBUG("KVM_IRQ_LINE -> 0");
        return 0;

    case KVM_CREATE_PIT2:
        vm->pit_created = true;
        MOCK_DEBUG("KVM_CREATE_PIT2 -> 0");
        return 0;

    case KVM_SET_PIT2:
        MOCK_DEBUG("KVM_SET_PIT2 -> 0");
        return 0;

    case KVM_ENABLE_CAP:
        MOCK_DEBUG("KVM_ENABLE_CAP -> 0");
        return 0;

    case KVM_GET_DIRTY_LOG: {
        struct kvm_dirty_log *log = (struct kvm_dirty_log *)argp;
        if (!log || !log->dirty_bitmap) return 0;

        /* Find the slot to determine bitmap size */
        uint64_t mem_size = 0;
        if (log->slot < MOCK_MAX_MEMSLOTS && vm->slots[log->slot].active) {
            mem_size = vm->slots[log->slot].memory_size;
        }
        if (mem_size == 0) return 0;

        uint64_t num_pages = mem_size / 4096;
        uint64_t bitmap_bytes = (num_pages + 7) / 8;

        /* Clear bitmap first, then randomly set ~g_dirty_pct% of bits */
        memset(log->dirty_bitmap, 0, bitmap_bytes);

        static __thread unsigned int seed = 0;
        if (seed == 0) seed = (unsigned int)time(NULL) ^ (unsigned int)pthread_self();

        for (uint64_t i = 0; i < num_pages; i++) {
            if ((rand_r(&seed) % 100) < g_dirty_pct) {
                uint8_t *bm = (uint8_t *)log->dirty_bitmap;
                bm[i / 8] |= (1 << (i % 8));
            }
        }
        MOCK_DEBUG("KVM_GET_DIRTY_LOG: slot=%u pages=%lu dirty_pct=%d%%",
                   log->slot, (unsigned long)num_pages, g_dirty_pct);
        return 0;
    }

    case KVM_SET_GSI_ROUTING:
        MOCK_DEBUG("KVM_SET_GSI_ROUTING -> 0");
        return 0;

    case KVM_IOEVENTFD:
        MOCK_DEBUG("KVM_IOEVENTFD -> 0");
        return 0;

    case KVM_IRQFD:
        MOCK_DEBUG("KVM_IRQFD -> 0");
        return 0;

    default:
        MOCK_DEBUG("KVM_VM ioctl 0x%lx -> 0 (no-op)", request);
        return 0;
    }
}

/* ================================================================== */
/*  ioctl() interception - vCPU FD                                     */
/* ================================================================== */

static int mock_ioctl_vcpu(mock_fd_entry_t *e, unsigned long request, void *argp) {
    mock_vcpu_state_t *vcpu = &e->state.vcpu;

    switch (request) {
    case KVM_RUN: {
        /* Simulate brief execution, then HLT exit */
        if (g_run_usec > 0) usleep(g_run_usec);
        vcpu->run_count++;
        if (vcpu->run) {
            vcpu->run->exit_reason = KVM_EXIT_HLT;
        }
        MOCK_DEBUG("KVM_RUN(vcpu=%d) -> HLT exit (count=%u)",
                   vcpu->vcpu_id, vcpu->run_count);
        return 0;
    }

    case KVM_GET_REGS: {
        struct kvm_regs *regs = (struct kvm_regs *)argp;
        if (regs) memcpy(regs, &vcpu->regs, sizeof(*regs));
        return 0;
    }

    case KVM_SET_REGS: {
        struct kvm_regs *regs = (struct kvm_regs *)argp;
        if (regs) memcpy(&vcpu->regs, regs, sizeof(*regs));
        MOCK_DEBUG("KVM_SET_REGS(vcpu=%d): rip=0x%llx rsp=0x%llx",
                   vcpu->vcpu_id, regs->rip, regs->rsp);
        return 0;
    }

    case KVM_GET_SREGS: {
        struct kvm_sregs *sregs = (struct kvm_sregs *)argp;
        if (sregs) memcpy(sregs, &vcpu->sregs, sizeof(*sregs));
        return 0;
    }

    case KVM_SET_SREGS: {
        struct kvm_sregs *sregs = (struct kvm_sregs *)argp;
        if (sregs) memcpy(&vcpu->sregs, sregs, sizeof(*sregs));
        MOCK_DEBUG("KVM_SET_SREGS(vcpu=%d): cr0=0x%llx cr3=0x%llx cr4=0x%llx",
                   vcpu->vcpu_id, sregs->cr0, sregs->cr3, sregs->cr4);
        return 0;
    }

    case KVM_SET_CPUID2:
        MOCK_DEBUG("KVM_SET_CPUID2 -> 0");
        return 0;

    case KVM_SET_MSRS:
        MOCK_DEBUG("KVM_SET_MSRS -> 0");
        return 0;

    case KVM_GET_MSRS: {
        /* Return 0 MSRs processed */
        struct kvm_msrs *msrs = (struct kvm_msrs *)argp;
        if (msrs) {
            for (uint32_t i = 0; i < msrs->nmsrs; i++) {
                msrs->entries[i].data = 0;
            }
        }
        return msrs ? msrs->nmsrs : 0;
    }

    case KVM_SET_MP_STATE:
        MOCK_DEBUG("KVM_SET_MP_STATE -> 0");
        return 0;

    case KVM_GET_MP_STATE: {
        struct kvm_mp_state *mp = (struct kvm_mp_state *)argp;
        if (mp) mp->mp_state = KVM_MP_STATE_RUNNABLE;
        return 0;
    }

    case KVM_SET_SIGNAL_MASK:
        MOCK_DEBUG("KVM_SET_SIGNAL_MASK -> 0");
        return 0;

    case KVM_GET_VCPU_EVENTS:
        if (argp) memset(argp, 0, sizeof(struct kvm_vcpu_events));
        return 0;

    case KVM_SET_VCPU_EVENTS:
        return 0;

    case KVM_GET_FPU:
        if (argp) memset(argp, 0, sizeof(struct kvm_fpu));
        return 0;

    case KVM_SET_FPU:
        return 0;

    case KVM_GET_LAPIC:
        if (argp) memset(argp, 0, sizeof(struct kvm_lapic_state));
        return 0;

    case KVM_SET_LAPIC:
        return 0;

    case KVM_GET_XSAVE:
        if (argp) memset(argp, 0, sizeof(struct kvm_xsave));
        return 0;

    case KVM_SET_XSAVE:
        return 0;

    case KVM_GET_XCRS:
        if (argp) {
            struct kvm_xcrs *xcrs = (struct kvm_xcrs *)argp;
            memset(xcrs, 0, sizeof(*xcrs));
            xcrs->nr_xcrs = 1;
            xcrs->xcrs[0].xcr = 0;
            xcrs->xcrs[0].value = 1; /* x87 enabled */
        }
        return 0;

    case KVM_SET_XCRS:
        return 0;

    default:
        MOCK_DEBUG("KVM_VCPU ioctl 0x%lx -> 0 (no-op)", request);
        return 0;
    }
}

/* ================================================================== */
/*  ioctl() main dispatch                                              */
/* ================================================================== */

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void *);
    va_end(ap);

    mock_fd_entry_t *e = mock_lookup_fd(fd);
    if (e) {
        switch (e->type) {
        case MOCK_FD_KVM_SYSTEM:
            return mock_ioctl_kvm_system(e, request, argp);
        case MOCK_FD_KVM_VM:
            return mock_ioctl_vm(e, request, argp);
        case MOCK_FD_KVM_VCPU:
            return mock_ioctl_vcpu(e, request, argp);
        }
    }

    return real_ioctl(fd, request, argp);
}

/* ================================================================== */
/*  mmap() interception                                                */
/* ================================================================== */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    mock_fd_entry_t *e = mock_lookup_fd(fd);

    if (e && e->type == MOCK_FD_KVM_VCPU) {
        /* Return the pre-allocated kvm_run region */
        mock_vcpu_state_t *vcpu = &e->state.vcpu;
        if (vcpu->run) {
            MOCK_DEBUG("mmap(vcpu fd %d) -> returning pre-allocated kvm_run at %p",
                       fd, vcpu->run);
            return vcpu->run;
        }
        /* Shouldn't happen, but allocate on demand */
        size_t sz = (length + 4095) & ~4095;
        void *p = real_mmap(NULL, sz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            memset(p, 0, sz);
            vcpu->run = (struct kvm_run *)p;
            vcpu->run_size = sz;
        }
        return p;
    }

    return real_mmap(addr, length, prot, flags, fd, offset);
}

/* Also intercept mmap64 */
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return mmap(addr, length, prot, flags, fd, offset);
}
