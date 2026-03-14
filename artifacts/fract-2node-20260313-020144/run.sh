#!/usr/bin/env bash
set -euo pipefail
ROOT=/workspaces/WaveVM_Frontier-X
ART_DIR="$(cd "$(dirname "$0")" && pwd)"

CFG="$ART_DIR/fract_2node.conf"
cat > "$CFG" <<'EOCFG'
# NODE <ID> <IP> <PORT> <CORES> <RAM_GB>
# PORT should be the local sidecar (gateway) data port
NODE 0 127.0.0.1 19120 1 1
NODE 1 127.0.0.1 19220 1 1
EOCFG

# L2 core gateway routes to L1 gateways
L2_CFG="$ART_DIR/l2_routes.txt"
cat > "$L2_CFG" <<'EOCFG'
ROUTE 0 1 127.0.0.1 19320
ROUTE 1 1 127.0.0.1 19420
EOCFG

# L1 gateways route to local sidecars
L1A_CFG="$ART_DIR/l1a_routes.txt"
cat > "$L1A_CFG" <<'EOCFG'
ROUTE 0 1 127.0.0.1 19120
EOCFG

L1B_CFG="$ART_DIR/l1b_routes.txt"
cat > "$L1B_CFG" <<'EOCFG'
ROUTE 1 1 127.0.0.1 19220
EOCFG

SIDECAR_A_CFG="$ART_DIR/sidecar_a_routes.txt"
cat > "$SIDECAR_A_CFG" <<'EOCFG'
# Pin all IDs to L2A to avoid auto-learn overriding upstream.
ROUTE 0 2 127.0.0.1 19320
EOCFG

SIDECAR_B_CFG="$ART_DIR/sidecar_b_routes.txt"
cat > "$SIDECAR_B_CFG" <<'EOCFG'
# Pin all IDs to L2B to avoid auto-learn overriding upstream.
ROUTE 0 2 127.0.0.1 19420
EOCFG

# Cleanup
pkill -f "wavevm_node_master 1024 19100" 2>/dev/null || true
pkill -f "wavevm_node_master 1024 19200" 2>/dev/null || true
pkill -f "wavevm_node_slave 19105" 2>/dev/null || true
pkill -f "wavevm_node_slave 19205" 2>/dev/null || true
pkill -f "qemu-system-x86_64.*hostfwd=tcp::2226-:22" 2>/dev/null || true
pkill -f "wavevm_gateway 19120" 2>/dev/null || true
pkill -f "wavevm_gateway 19220" 2>/dev/null || true
pkill -f "wavevm_gateway 19320" 2>/dev/null || true
pkill -f "wavevm_gateway 19420" 2>/dev/null || true
pkill -f "wavevm_gateway 19520" 2>/dev/null || true
rm -f /tmp/wvm_user_0.sock /tmp/wvm_user_1.sock 2>/dev/null || true

QPATH="$ROOT/wavevm-qemu/build-native:$PATH"

# Start L2 core gateway (root): upstream -> self
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19520 127.0.0.1 19520 "$L2_CFG" 19521) >"$ART_DIR/gw_l2.log" 2>&1 &
GL2=$!

# Start L1 gateways: upstream -> L2
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19320 127.0.0.1 19520 "$L1A_CFG" 19321) >"$ART_DIR/gw_l1a.log" 2>&1 &
GL1A=$!
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19420 127.0.0.1 19520 "$L1B_CFG" 19421) >"$ART_DIR/gw_l1b.log" 2>&1 &
GL1B=$!

# Start sidecar gateways on compute nodes: upstream -> L1
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19120 127.0.0.1 19320 "$SIDECAR_A_CFG" 19121) >"$ART_DIR/gw_sidecar_a.log" 2>&1 &
GSA=$!
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19220 127.0.0.1 19420 "$SIDECAR_B_CFG" 19221) >"$ART_DIR/gw_sidecar_b.log" 2>&1 &
GSB=$!

# Slaves
(env PATH="$QPATH" WVM_SHM_FILE=/wvm_fract_node0 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19105 2 2048 0 19121) >"$ART_DIR/slave0.log" 2>&1 &
S0=$!
(env PATH="$QPATH" WVM_SHM_FILE=/wvm_fract_node1 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19205 1 1024 1 19221) >"$ART_DIR/slave1.log" 2>&1 &
S1=$!

# Masters
(env PATH="$QPATH" WVM_INSTANCE_ID=0 WVM_SHM_FILE=/wvm_fract_node0 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 2048 19100 "$CFG" 0 19121 19105 1) >"$ART_DIR/master0.log" 2>&1 &
M0=$!
(env PATH="$QPATH" WVM_INSTANCE_ID=1 WVM_SHM_FILE=/wvm_fract_node1 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 1024 19200 "$CFG" 1 19221 19205 1) >"$ART_DIR/master1.log" 2>&1 &
M1=$!

sleep 8

# QEMU
(env PATH="$QPATH" WVM_INSTANCE_ID=0 stdbuf -oL -eL "$ROOT/wavevm-qemu/build-native/qemu-system-x86_64" \
  -accel wavevm -machine q35 -m 3072 -smp 3 \
  -object memory-backend-ram,id=ram0,size=2048M \
  -object memory-backend-ram,id=ram1,size=1024M \
  -numa node,memdev=ram0,cpus=0-1,nodeid=0 \
  -numa node,memdev=ram1,cpus=2,nodeid=1 \
  -drive file="$ROOT/artifacts/images/cirros-0.6.2-x86_64-disk.img",if=virtio,format=qcow2 \
  -netdev user,id=ne,hostfwd=tcp::2226-:22 -device e1000,netdev=ne \
  -display none -vga none \
  -serial file:"$ART_DIR/vm-serial.log" -monitor none) >"$ART_DIR/vm.log" 2>&1 &
Q=$!

sleep 180
LISTEN=$(ss -ltnp | rg ':2226' || true)
BANNER=$(timeout 6 bash -lc 'exec 3<>/dev/tcp/127.0.0.1/2226; head -c 120 <&3' || true)
TIMEOUTS=$(rg -n "RPC Timeout\] Type: 5" "$ART_DIR/master0.log" | wc -l || true)

echo "ARTIFACT_DIR=$ART_DIR"
echo "PIDS GL2=$GL2 GL1A=$GL1A GL1B=$GL1B GSA=$GSA GSB=$GSB S0=$S0 S1=$S1 M0=$M0 M1=$M1 Q=$Q"
echo "PORT_2226_LISTEN=${LISTEN:-none}"
echo "VCPU_TIMEOUT_COUNT=$TIMEOUTS"
if [[ -n "$BANNER" ]]; then
  echo "SSH_BANNER=$BANNER"
  echo "RESULT=PASS_BANNER"
else
  echo "SSH_BANNER="
  echo "RESULT=NO_BANNER"
fi
