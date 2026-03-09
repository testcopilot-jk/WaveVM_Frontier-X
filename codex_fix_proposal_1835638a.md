# Fix Proposal: cpu_exec() Hang in Slave TCG Remote Execution

## Problem

`wavevm_slave_net_thread` receives `MSG_VCPU_RUN` (mode_tcg=1), calls `cpu_exec(cpu)` directly at `wavevm-all.c:566`. This call never returns.

## Root Causes (4 interacting issues)

### 1. cpu_exec() called from wrong thread

`wavevm_slave_net_thread` is a plain network thread, not a vCPU thread. It was never initialized for TCG execution:

- No `rcu_register_thread()` — RCU operations inside cpu_exec are undefined behavior
- No `cpu_exec_start(cpu)` / `cpu_exec_end(cpu)` — TCG exclusive context protocol broken
- `current_cpu` not set for this thread (set at `wavevm-cpu.c:525` only for the vCPU thread)

Meanwhile the real vCPU thread (`wavevm_cpu_thread_fn`) IS properly initialized but is parked in a sleep loop at `wavevm-cpu.c:534-539`.

### 2. Kick signal sent to wrong thread

`qemu_cpu_kick(cpu)` at `wavevm-all.c:64` sends `SIG_IPI` to `cpu->thread`. But `cpu->thread` was set at `wavevm-cpu.c:522` to the **parked vCPU thread**, not the net thread running `cpu_exec()`. The 5ms kick never reaches the right thread.

### 3. tb_flush() async work never processed

`wvm_tcg_set_state()` calls `tb_flush(cpu)` which queues `async_safe_run_on_cpu(do_tb_flush)` because it's not in exclusive context. This work item requires `process_queued_cpu_work()` to execute it, which only happens inside the vCPU thread's execution loop. The parked vCPU thread never processes it. If this triggers `start_exclusive()`, it deadlocks waiting for CPU cooperation that never comes.

### 4. PROT_NONE RAM + blocking signal handler

All guest RAM starts as `PROT_NONE` (`wavevm-user-mem.c:1444`). First instruction fetch inside `cpu_exec()` triggers SIGSEGV → `request_page_sync()` → blocking UDP round-trip. Even if the network call succeeds, every new 4KB page repeats this. Execution appears frozen.

## Fix: Delegate execution to the vCPU thread

The fundamental fix is: **never call `cpu_exec()` from the net thread.** Instead, wake the parked vCPU thread and let it do the work.

### Changes to `wavevm-cpu.c` (Slave vCPU thread)

Replace the park loop (`wavevm-cpu.c:534-539`) with a proper request-wait loop:

```c
// Add these globals (or put them in a shared struct):
static QemuMutex g_slave_req_lock;
static QemuCond  g_slave_req_cond;
static QemuCond  g_slave_done_cond;
static volatile bool g_slave_has_req = false;
static volatile bool g_slave_done = false;
static struct wvm_ipc_cpu_run_req *g_slave_pending_req = NULL;
static struct wvm_ipc_cpu_run_ack *g_slave_pending_ack = NULL;

// In wavevm_cpu_thread_fn, replace lines 534-539:
if (is_slave) {
    qemu_mutex_lock(&g_slave_req_lock);
    while (!cpu->unplug) {
        while (!g_slave_has_req && !cpu->unplug) {
            qemu_cond_wait(&g_slave_req_cond, &g_slave_req_lock);
        }
        if (cpu->unplug) break;

        // 1. Import register state
        struct wvm_ipc_cpu_run_req *req = g_slave_pending_req;
        bool local_is_tcg = !kvm_enabled();

        qemu_mutex_lock_iothread();
        if (req->mode_tcg && local_is_tcg) {
            wvm_tcg_set_state(cpu, &req->ctx.tcg);
        }
        // ... (same context import logic currently in wavevm-all.c:506-551)

        cpu->stop = false;
        cpu->halted = 0;
        cpu->exception_index = -1;

        // 2. Schedule kick (now targets THIS thread correctly)
        WaveVMTcgKickCtx *kick = g_new0(WaveVMTcgKickCtx, 1);
        QemuThread kick_thread;
        kick->cpu = cpu;
        kick->delay_us = 5000;
        qemu_thread_create(&kick_thread, "wvm-tcg-kick",
                           wavevm_tcg_kick_thread, kick,
                           QEMU_THREAD_DETACHED);
        qemu_mutex_unlock_iothread();

        // 3. Execute (on this properly initialized vCPU thread)
        cpu_exec(cpu);

        qemu_mutex_lock_iothread();

        // 4. Export state into ack
        struct wvm_ipc_cpu_run_ack *ack = g_slave_pending_ack;
        memset(ack, 0, sizeof(*ack));
        ack->status = 0;
        ack->mode_tcg = req->mode_tcg;
        if (local_is_tcg) {
            wvm_tcg_get_state(cpu, &ack->ctx.tcg);
            ack->ctx.tcg.exit_reason = cpu->exception_index;
        }
        // ... (same context export logic currently in wavevm-all.c:584-660)

        qemu_mutex_unlock_iothread();

        // 5. Signal completion
        g_slave_has_req = false;
        g_slave_done = true;
        qemu_cond_signal(&g_slave_done_cond);
    }
    qemu_mutex_unlock(&g_slave_req_lock);
    goto out;
}
```

### Changes to `wavevm-all.c` (Net thread — MSG_VCPU_RUN handler)

Replace the current `cpu_exec()` call block (`wavevm-all.c:496-660`) with a handoff:

```c
case MSG_VCPU_RUN: {
    // Parse req as before (lines 455-494 unchanged)
    // ...

    // Instead of calling cpu_exec() here, hand off to vCPU thread:
    qemu_mutex_lock(&g_slave_req_lock);
    g_slave_pending_req = req;     // point to the parsed request
    g_slave_pending_ack = &full_ack;
    g_slave_done = false;
    g_slave_has_req = true;
    qemu_cond_signal(&g_slave_req_cond);  // wake vCPU thread

    // Wait for vCPU thread to finish execution
    while (!g_slave_done) {
        qemu_cond_wait(&g_slave_done_cond, &g_slave_req_lock);
    }
    qemu_mutex_unlock(&g_slave_req_lock);

    // Send ack back (existing reply logic, lines 660+)
    // ...
    break;
}
```

### Init (add to wavevm_start_vcpu_thread or early init)

```c
// Call once during Slave startup:
qemu_mutex_init(&g_slave_req_lock);
qemu_cond_init(&g_slave_req_cond);
qemu_cond_init(&g_slave_done_cond);
```

## Why This Fixes All 4 Issues

| Issue | Before | After |
|-------|--------|-------|
| Wrong thread | Net thread calls cpu_exec | vCPU thread calls cpu_exec (has rcu, current_cpu, etc.) |
| Kick target | cpu->thread = parked vCPU thread (unreachable) | cpu->thread = active vCPU thread (correct target) |
| tb_flush async | Queued but parked thread never processes | vCPU thread processes queued work normally inside cpu_exec |
| SIGSEGV handler | Blocks net thread (which also needs to recv) | Blocks vCPU thread only; net thread free to recv |

## Notes for Codex

- The context import/export logic (register serialization) does NOT change — just move it from `wavevm-all.c` into the vCPU thread loop.
- The `g_slave_req_lock` mutex must NOT be held while `cpu_exec()` runs, otherwise the net thread can't wake it for the next request. The lock is only for the handoff signaling.
- The existing `wavevm-cpu.c:609` local TCG path (`cpu_exec(cpu)`) already works correctly because it runs on the vCPU thread — this fix makes the Slave path consistent with it.
- Keep the compact req parsing in the net thread (wavevm-all.c) — only the execution and state import/export moves to the vCPU thread.
