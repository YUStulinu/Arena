#!/bin/bash
# The network evaluation (Linux, as root): the match server and two native
# bot clients in three network namespaces linked by veth pairs, with netem
# (delay, jitter, loss) on both ends of each link. Every scenario plays
# several matches; the server reports desyncs, the clients rollbacks,
# prediction errors, stalls and bandwidth. Only the test namespaces are
# touched; they are deleted at the end.
#
#   sudo eval/netem.sh [MATCHES]        (after building engine/ and server/ for Linux)
set -e
cd "$(dirname "$0")/.."
MATCHES=${1:-4}
OUT=results/netem
mkdir -p $OUT run
SERVER=server/target/release/arena-server
CLIENT=engine/build/native/arena_client
cleanup() {
  for ns in arena_srv arena_c0 arena_c1; do ip netns del $ns 2>/dev/null || true; done
}
trap cleanup EXIT
cleanup
for ns in arena_srv arena_c0 arena_c1; do ip netns add $ns; ip -n $ns link set lo up; done
for k in 0 1; do
  ip link add ac$k type veth peer name as$k
  ip link set ac$k netns arena_c$k
  ip link set as$k netns arena_srv
  ip -n arena_c$k addr add 10.77.$k.2/24 dev ac$k
  ip -n arena_srv addr add 10.77.$k.1/24 dev as$k
  ip -n arena_c$k link set ac$k up
  ip -n arena_srv link set as$k up
done

# scenario: name delay_ms jitter_ms loss_pct (applied on each direction of each client's link)
SCENARIOS="lan:0:0:0 20ms:20:5:0 50ms_1pct:50:10:1 100ms_3pct:100:20:3 150ms_5pct:150:40:5"
for sc in $SCENARIOS; do
  IFS=: read name delay jitter loss <<< "$sc"
  for k in 0 1; do
    for dev in "arena_c$k ac$k" "arena_srv as$k"; do
      set -- $dev
      ip netns exec $1 tc qdisc del dev $2 root 2>/dev/null || true
      if [ "$delay" != "0" ] || [ "$loss" != "0" ]; then
        ip netns exec $1 tc qdisc add dev $2 root netem delay ${delay}ms ${jitter}ms distribution normal loss ${loss}%
      fi
    done
  done
  echo "== $name: delay ${delay} ms +- ${jitter} ms, loss ${loss}% (each direction)"
  ip netns exec arena_srv python3 eval/netem_run.py --scenario $name --matches $MATCHES --out $OUT/$name.json \
     --server-bin $SERVER --client-bin $CLIENT --ns0 arena_c0 --ns1 arena_c1 --addr0 10.77.0.1 --addr1 10.77.1.1
done
