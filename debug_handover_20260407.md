# WaveVM KVM Mode B Debug Handover

## Scope

This note summarizes the current debugging state for the dual-node KVM smoke path, so another model can continue without replaying the full history.

Environment assumptions:
- Repository: `/workspaces/WaveVM_Frontier-X`
- Main smoke script: `scripts/ci_guest_ssh_smoke.sh`
- Mode under test: `Mode B`
- Topology under test: dual-node fractal path

Current objective:
- Make the guest boot reliably to the point where SSH on forwarded port `2226` works under the WaveVM path, not only under native QEMU.

## High-Level Status

The project is no longer blocked on the broad control-plane path.

These layers are already confirmed working:
- `gateway / sidecar / l1 / l2 / master / slave` cross-node chain
- `VCPU_RUN -> VCPU_EXIT -> rid` matching
- AP `SIPI / trampoline / parked` large boot blockers
- major KVM context gaps such as `LAPIC`, `vcpu_events`, `FPU`, `XCRS`
- guest progression from BIOS to kernel to early userspace

Native stock QEMU on the same host can boot the same guest image and reach working SSH, so:
- the guest image is not fundamentally broken
- host forwarding on `2226` is not fundamentally broken
- the remaining blocker is inside the WaveVM execution semantics

## What Has Been Proven

### 1. Network and RPC path are not the main blocker anymore

The following are already proven by prior runs:
- local path `target=0` works
- cross-node path `target=1` works
- packets reach Node B
- Node B executes and returns `VCPU_EXIT`
- Node A matches the response by `rid`

This means the current root problem is not:
- route learning
- sidecar forwarding
- l1/l2 forwarding
- `rid` matching
- basic master/slave RPC transport

### 2. Several earlier missing-state bugs were real and were fixed

Real fixes already added and considered valid:
- `LAPIC` state sync
- `vcpu_events` state sync
- `FPU` state sync
- `XCRS` state sync
- replacing fake timeout-as-`HLT` with explicit `WVM_EXIT_PREEMPT`

These were not test-only hacks. They moved the system forward in reproducible ways.

### 3. The guest now reaches early userspace under WaveVM

Recent healthy runs progressed to logs such as:
- `Run /init as init process`
- `Saving random seed: OK`
- `Starting acpid: OK`
- `Starting network: dhcpcd...`

So the current state is no longer "guest cannot boot".
The guest boots, but later progress is still semantically wrong.

## Current Narrow Root Problem

The active remaining issue is now very narrow:

- `HLT + LAPIC timer interrupt` restore/consume semantics are still wrong on the master/QEMU side

Observed pattern:
- ACK returns from the slave with a pending timer interrupt
- representative state:
  - `int_inj=1`
  - `int_nr=236`
  - `cur=0`
- after restore on the master side, the vCPU does not truly consume that pending timer interrupt
- instead, the same `HLT RIP` is redispatched again and again

This is the main reason the system feels "almost there but not there":
- large subsystems are already working
- one deep execution-semantics issue remains

## Important Findings by Topic

### A. Timeout semantics

An important bug existed in the slave timeout path:
- on 50ms `KVM_RUN` timeout, the slave used to synthesize `KVM_EXIT_HLT`

That was wrong.
It conflated "slice preemption" with "guest executed HLT".

This has been changed to:
- `WVM_EXIT_PREEMPT`

This fix should be kept.

Relevant files:
- `common_include/wavevm_protocol.h`
- `slave_daemon/slave_hybrid.c`

### B. FPU and XCRS

Missing `FPU` and `XCRS` propagation was a real issue.
After adding both directions of sync:
- guest progressed farther
- early crashes became much less severe

These fixes should be kept.

Relevant files:
- `common_include/wavevm_protocol.h`
- `wavevm-qemu/accel/wavevm/wavevm-cpu.c`
- `slave_daemon/slave_hybrid.c`

### C. Slave-side HLT handling

There was a real bug around `KVM_EXIT_HLT` on the slave side:
- `KVM_GET_LAPIC` often failed during the early probe path with `errno=22`
- this caused "should wait locally for timer" cases to ACK too early

This led to a fallback based on `lapic_shadow`.

What is known:
- fallback is useful to detect timer presence
- but `lapic_shadow.cur` is a static snapshot, not a live countdown
- therefore it cannot be used for unbounded local waiting

Current status:
- bounded `shadow-assisted wait` is better than immediate ACK
- but this alone does not solve the final issue

### D. A rejected experiment: local timer consumption on master

An experimental branch was added in `wavevm-cpu.c` to locally consume the pending timer interrupt after HLT restore.

It was intentionally narrow:
- only for `KVM_EXIT_HLT`
- only when `int_inj=1`
- only when `int_nr=236`
- only when LAPIC current count was `0`

Result:
- the branch did fire
- but the run later crashed into a bad kernel path

Conclusion:
- that local-consume approach was wrong
- it has already been reverted

This rejected path should not be reintroduced casually.

### E. Native-QEMU comparison

A native stock-QEMU smoke run on the same host successfully reached SSH.

Meaning:
- guest image is fine
- hostfwd is fine
- the WaveVM remaining issue is not about basic guest image viability

This comparison strongly suggests that the current remaining problem is execution semantics, not environment basics.

## What Is Probably Not Worth Chasing Right Now

These areas are no longer good primary suspects:
- route learning bugs
- packet size / MTU as the main blocker
- `gateway` multi-hop correctness as the main blocker
- `rid` matching bugs
- "guest image is broken"
- "host port 2226 forwarding is broken"
- broad environment-only explanations

Environment instability did exist earlier in Codespaces, but the current failing pattern is now too specific and too repeatable to treat as pure environment noise.

## Current Best Hypothesis

The best current hypothesis is:

- `WVM_EXIT_PREEMPT` is correct and should stay
- `FPU/XCRS` sync is correct and should stay
- the remaining defect is in how the master/QEMU side restores and resumes a vCPU after `HLT` ACK when a timer interrupt is already pending

This likely means one of these is still semantically incomplete:
- how pending interrupt state is restored
- how `cpu->halted` and `mp_state` are restored after HLT ACK
- whether the restored vCPU must be resumed in a different way from a normal HLT path
- whether pending timer state is restored but immediately buried by the halt bookkeeping

## Recommended Next Checks

These are the next useful checks, in order:

1. Audit `wavevm-qemu/accel/wavevm/wavevm-cpu.c` for explicit handling of `WVM_EXIT_PREEMPT`
Reason:
- the slave already produces it
- the master/QEMU side may still be treating it too implicitly

2. Audit `KVM_EXIT_HLT` restore logic in `wavevm-cpu.c`
Reason:
- current evidence points to the timer interrupt being returned but not truly consumed

3. Keep logs extremely narrow
Only inspect:
- `PREEMPT`
- `WVM-HLT-LAPIC`
- `dispatch #`
- `ack#`
- repeated identical `RIP`
- `failed N/20`
- `cirros login`
- `dropbear`

4. Do not widen the search back to `gateway` or route learning unless new evidence forces it

## Files Most Worth Reading

Core files:
- `wavevm-qemu/accel/wavevm/wavevm-cpu.c`
- `slave_daemon/slave_hybrid.c`
- `common_include/wavevm_protocol.h`

Secondary files:
- `master_core/user_backend.c`
- `master_core/main_wrapper.c`

## Short Change Inventory

Changes that should remain:
- `WVM_EXIT_PREEMPT`
- `FPU` sync
- `XCRS` sync
- `LAPIC` sync
- `vcpu_events` sync

Changes that were exploratory and should not be treated as validated fixes:
- local master-side timer-consume branch after HLT restore

## Bottom-Line Assessment

The system is not "far away" in the old broad sense.
It is also not "one tiny patch from guaranteed success".

The honest assessment is:
- most broad subsystems are already working
- the remaining blocker is narrow
- but the remaining blocker is a deep execution-semantics issue, which is why progress feels slow even though the search space is now much smaller

If another model continues from here, it should focus on:
- `HLT`
- pending timer interrupt restore
- `PREEMPT`
- master-side resume semantics

It should not restart the investigation from networking or guest-image basics.
