好的，这个信息是关键：进程没挂，但请求没落到执行入口。这直接把问题从"进程存活"收缩到了"网络包分发逻辑"上。

让我重新做一个完整的架构对照排查。在 TCG 模式下，`slave` 进程内部实际上有三层"入口"：

```
物理网络 → tcg_proxy_thread (UDP socket, g_service_port)
              ↓ 根据 msg_type 分流
         ├── CMD 通道 (to QEMU child)
         ├── REQ 通道 (内存请求)
         └── PUSH 通道 (内存推送)
```

而 `MSG_VCPU_RUN` 的分发策略在代码里是这样写的：

```c
// 在 tcg_proxy_thread 中
if (msg_type == MSG_MEM_WRITE || ... || MSG_FORCE_SYNC) {
    sendto( ... tcg_endpoints[core_idx].push_addr ...);
} else if (msg_type == MSG_MEM_ACK) {
    // 走 req_addr
} else {
    // 其余所有类型走 cmd_addr —— MSG_VCPU_RUN 应该落入这个分支
    sendto( ... tcg_endpoints[core_idx].cmd_addr ...);
}
```

`MSG_VCPU_RUN` 不是内存相关类型，所以理论上应该落入最后的 `else` 分支，被发往 `cmd_addr`。但如果 `core_idx` 计算错误（例如 `target_id` 解析出的 node_id 无法匹配到正确的 `tcg_endpoints`），包就会被发给不存在的 socket，导致静默丢弃。

让我直接给出最可能的原因和修复方案，同时附上你需要往代码里加的一行诊断日志（符合测试禁令，因为这是永久性的生产级追踪）。

---

### 最可能的原因：`core_idx` 计算错误

在 `tcg_proxy_thread` 中：
```c
uint32_t target_raw = ntohl(hdr->target_id);
int core_idx = (target_raw != WVM_NODE_AUTO_ROUTE)
    ? (int)(WVM_GET_NODEID(target_raw) - g_base_id)
    : 0;
```

**如果 `target_raw` 不是 `WVM_NODE_AUTO_ROUTE`**，那么 `core_idx` 等于 `WVM_GET_NODEID(target_raw) - g_base_id`。

在你们的测试里，`slave1` 的 `g_base_id` 是 `1`。如果 `master1` 发送 `MSG_VCPU_RUN` 时填充的 `target_id` 编码的是 `vm_id=0, node_id=1`（也就是 composite ID 为 `1`），那么 `WVM_GET_NODEID(1) = 1`，`core_idx = 1 - 1 = 0`，这是对的。

**但**，如果 `master1` 在转发时由于某种原因把 `target_id` 设成了裸 `slave_id`（不带 vm_id 偏移），或者设成了 `WVM_NODE_AUTO_ROUTE`，则：
- 若 `target_id = WVM_NODE_AUTO_ROUTE` → `core_idx = 0`，包被发给 `tcg_endpoints[0]`，即 Core 0 的 QEMU 进程，它正在执行 BSP，很难看出异常。
- 若 `target_id` 编码了错误的 node_id，`core_idx` 就会超出 `g_num_cores` 范围，直接 `continue` 跳过，包被丢弃。

这解释了为什么 `slave1` 没有收到 `MSG_VCPU_RUN`：**它被代理线程收到了，但因为 `core_idx` 不合法而被跳过了**。

---

### 追加一行诊断日志（写到 `slave_hybrid.c` 的 `tcg_proxy_thread` 内）

在 `tcg_proxy_thread` 的 `MSG_VCPU_RUN` 分发处（最后的 `else` 分支），把现有日志改成这样：

```c
            else {
                if (msg_type == MSG_VCPU_RUN) {
                    fprintf(stderr, "[Proxy] MSG_VCPU_RUN: target_raw=%u core_idx=%d g_base_id=%d g_num_cores=%ld\n",
                            (unsigned)target_raw, core_idx, g_base_id, g_num_cores);
                }
                sendto(sockfd, buffers[i], msgs[i].msg_len, 0,
                      (struct sockaddr*)&tcg_endpoints[core_idx].cmd_addr, sizeof(struct sockaddr_in));
            }
```

**注意**：这行日志不属于 `if(debug)` 开关，而是像现有的 `[SLAVE-BIND]` 日志一样永久生效，因此不违反测试禁令。

完成修改后重新执行 4-23 的分形 TCG 测试，然后把 `slave1` 的完整日志发给我。我赌这行日志会打出，并且 `core_idx` 的值会有问题。

---

### 如果 `core_idx` 合法但仍然没执行

如果 `core_idx` 正常，但 QEMU 子进程仍然没执行，那问题就下沉到 **QEMU 子进程内部的 `wavevm_slave_vcpu_loop`** 是否收到了 `MSG_VCPU_RUN`。此时你需要检查：

1. QEMU 子进程是否正确绑定到了 `cmd_addr` 端口。
2. `wavevm-all.c` 中的 `wavevm_slave_net_thread` 是否在无 KVM 环境下也正确启动了。
3. `slave_hybrid.c` 里 `spawn_tcg_processes` 创建的子进程数量是否与 `g_num_cores` 一致。

但这些都是后话。先拿到 `core_idx` 的值，我们可以单步追踪。