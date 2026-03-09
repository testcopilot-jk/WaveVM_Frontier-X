#ifndef WVM_KVM_MOCK_INTERNAL_H
#define WVM_KVM_MOCK_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <linux/kvm.h>

/* ------------------------------------------------------------------ */
/*  Mock FD Registry                                                   */
/* ------------------------------------------------------------------ */

enum mock_fd_type {
    MOCK_FD_KVM_SYSTEM,   /* /dev/kvm */
    MOCK_FD_KVM_VM,       /* KVM_CREATE_VM result */
    MOCK_FD_KVM_VCPU,     /* KVM_CREATE_VCPU result */
};

#define MOCK_MAX_FDS      256
#define MOCK_MAX_MEMSLOTS  32
#define MOCK_MAX_VCPUS     64

/* Per-memslot record (for KVM_SET_USER_MEMORY_REGION tracking) */
typedef struct {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
    bool     active;
} mock_memslot_t;

/* Per-vCPU state */
typedef struct {
    struct kvm_run  *run;           /* mmap-able kvm_run region */
    size_t           run_size;      /* mmap size (page-aligned) */
    struct kvm_regs  regs;          /* saved general registers */
    struct kvm_sregs sregs;         /* saved segment registers */
    int              vcpu_id;
    uint32_t         run_count;     /* how many KVM_RUN calls */
} mock_vcpu_state_t;

/* Per-VM state */
typedef struct {
    mock_memslot_t   slots[MOCK_MAX_MEMSLOTS];
    int              slot_count;
    int              next_vcpu_id;
    bool             irqchip_created;
    bool             pit_created;
} mock_vm_state_t;

/* Single FD entry in the registry */
typedef struct {
    int               fd;
    enum mock_fd_type type;
    bool              active;
    union {
        mock_vm_state_t   vm;
        mock_vcpu_state_t vcpu;
    } state;
} mock_fd_entry_t;

/* ------------------------------------------------------------------ */
/*  Global State                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    mock_fd_entry_t  fds[MOCK_MAX_FDS];
    pthread_mutex_t  lock;
    bool             initialized;
    int              log_level;     /* 0=quiet, 1=info, 2=debug */
} mock_global_t;

/* Defined in wvm_kvm_mock.c */
extern mock_global_t g_mock;

/* ------------------------------------------------------------------ */
/*  Registry helpers (all thread-safe via g_mock.lock)                 */
/* ------------------------------------------------------------------ */

static inline void mock_init_once(void) {
    if (!g_mock.initialized) {
        pthread_mutex_init(&g_mock.lock, NULL);
        for (int i = 0; i < MOCK_MAX_FDS; i++) {
            g_mock.fds[i].active = false;
        }
        const char *env = getenv("WVM_MOCK_LOG");
        g_mock.log_level = env ? atoi(env) : 1;
        g_mock.initialized = true;
    }
}

/* Find a free slot and register fd */
static inline mock_fd_entry_t *mock_register_fd(int fd, enum mock_fd_type type) {
    pthread_mutex_lock(&g_mock.lock);
    for (int i = 0; i < MOCK_MAX_FDS; i++) {
        if (!g_mock.fds[i].active) {
            g_mock.fds[i].fd = fd;
            g_mock.fds[i].type = type;
            g_mock.fds[i].active = true;
            memset(&g_mock.fds[i].state, 0, sizeof(g_mock.fds[i].state));
            pthread_mutex_unlock(&g_mock.lock);
            return &g_mock.fds[i];
        }
    }
    pthread_mutex_unlock(&g_mock.lock);
    return NULL;
}

/* Lookup by fd */
static inline mock_fd_entry_t *mock_lookup_fd(int fd) {
    /* No lock needed for read if we accept eventual consistency on active flag */
    for (int i = 0; i < MOCK_MAX_FDS; i++) {
        if (g_mock.fds[i].active && g_mock.fds[i].fd == fd) {
            return &g_mock.fds[i];
        }
    }
    return NULL;
}

/* Unregister fd */
static inline void mock_unregister_fd(int fd) {
    pthread_mutex_lock(&g_mock.lock);
    for (int i = 0; i < MOCK_MAX_FDS; i++) {
        if (g_mock.fds[i].active && g_mock.fds[i].fd == fd) {
            /* Free vCPU run region if applicable */
            if (g_mock.fds[i].type == MOCK_FD_KVM_VCPU &&
                g_mock.fds[i].state.vcpu.run) {
                munmap(g_mock.fds[i].state.vcpu.run,
                       g_mock.fds[i].state.vcpu.run_size);
            }
            g_mock.fds[i].active = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_mock.lock);
}

/* ------------------------------------------------------------------ */
/*  Logging                                                            */
/* ------------------------------------------------------------------ */

#define MOCK_LOG(level, fmt, ...) do { \
    if (g_mock.log_level >= (level)) { \
        fprintf(stderr, "[KVM-MOCK] " fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

#define MOCK_INFO(fmt, ...)  MOCK_LOG(1, fmt, ##__VA_ARGS__)
#define MOCK_DEBUG(fmt, ...) MOCK_LOG(2, fmt, ##__VA_ARGS__)

#endif /* WVM_KVM_MOCK_INTERNAL_H */
