Codex 的 suggest.md 分析得基本准确，我补充几个从日志里看到的具体问题。

### 当前状态总结

**已经通了的**：
- Slave `cpu_exec()` 卡死问题已修（vCPU service loop online）
- SIGSEGV fault hook 在 Master QEMU 上稳定工作
- 远端缺页 `request_page_sync` 链路闭环——3212 次远端页拉取成功（`IPC Fault Ack`）
- Gossip 心跳双向互通，两节点都到了 `state=2`（WARMING → 正常状态）
- GDB 那个 SIGSEGV 确认是调试干扰，不挂 GDB 裸跑后 QEMU 正常

**没通的**：SSH banner 没出来，serial 为空，Guest 卡在启动后期。

### 从日志看到的关键线索

**GPA `0xe000` 的写 fault 风暴**。vm.log 里 `gpa=0xe000 write=1` 出现了 **18896 次**。这个页面被反复写 fault，每次都只是标记 `write=1` 但没有触发 `request_page_sync`（说明页面已经在本地，是写保护 fault 而不是缺页）。

这意味着 `0xe000` 这个页面的 **写保护恢复逻辑有问题**。正常流程应该是：
1. 写 fault → CBW 快照 → `mprotect(PROT_READ|PROT_WRITE)` → 返回
2. 后续写不再 fault（直到 harvester 收割脏页重新设为 PROT_READ）

但这里同一个 GPA 连续几十次写 fault，说明 `mprotect` 没有成功放开写权限，或者放开后又被立即收回。Guest 的 BIOS/firmware 可能在这个地址上做循环写操作（BDA 区域 `0xe000-0xf000` 是 BIOS 数据区），每次写都触发 fault → handler 处理 → 但保护没解除 → 再 fault，形成一个**空转热循环**。

**GPA `0x33333000` 同理**，916 次写 fault 风暴。

这两个热页的空转会极大拖慢 Guest 启动速度——本该纳秒级完成的内存写操作，变成了每次都要走信号处理器的微秒级路径，相当于这两个页面上的性能衰退了 1000 倍。

### 最可能的根因

`sigsegv_handler` 里写 fault 分支的 `mprotect` 调用可能没有正确覆盖到 TCG softmmu 的 TLB 缓存。TCG 用的是软件 TLB（`cputlb.c`），即使 host 侧 `mprotect` 放开了页面权限，TCG 的 TLB entry 里可能还是旧的 `PROT_READ` 标记，导致下次写操作仍然被 TCG TLB 拦截触发 SIGSEGV。需要在写 fault 处理后调用 `tlb_flush_page(cpu, gpa)` 或 `tlb_unprotect_code(addr)` 来刷新 TCG 的软件 TLB。

或者更简单的可能：写 fault handler 里做了 CBW 但**忘了调 `mprotect` 升级为 RW**，或者 `mprotect` 的地址/长度参数算错了。

建议 Codex 下一步：在 `sigsegv_handler` 的写 fault 分支加日志，确认 `mprotect(page, 4096, PROT_READ|PROT_WRITE)` 的返回值是否为 0，以及确认 `mprotect` 的 `page` 地址是否真的是 fault 地址对齐到页边界的结果。