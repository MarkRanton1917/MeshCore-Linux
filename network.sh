#!/usr/bin/env bash
# Runs a network of meshcore-node processes on this machine, one per node, all
# talking over UDP on the loopback interface.
#
# Each node gets its own data directory, so every one of them has its own
# identity and its own contact list; the peer lists in the generated configs are
# the visibility matrix, which is what makes the topology real rather than a
# drawing. Ctrl-C stops the whole network.
#
#   ./network.sh                     three nodes, full mesh
#   ./network.sh -n 5 -t chain       five nodes in a line, 1-2-3-4-5
#   ./network.sh -n 4 -t star -v     hub plus three leaves, debug logging

set -euo pipefail

count=3
topology=mesh
base_port=45601
workdir=./run
advert_ms=10000
level=info
clean=0

usage()
{
  cat <<'TEXT'
usage: network.sh [-n count] [-t mesh|chain|star] [-p base-port] [-d dir]
                  [-a advert-ms] [-v] [-c]

  -n  number of nodes (default 3)
  -t  topology (default mesh)
        mesh   every node hears every other node
        chain  node i hears only i-1 and i+1
        star   node 1 hears everyone, the rest hear only node 1
  -p  udp port of the first node, the rest follow (default 45601)
  -d  working directory for configs, data and logs (default ./run)
  -a  advert interval in milliseconds (default 10000, the node default is
      300000 which is too slow to watch)
  -v  log at debug instead of info
  -c  wipe the working directory first, so every node starts as a stranger
TEXT
}

while getopts "n:t:p:d:a:vch" option; do
  case "$option" in
  n) count=$OPTARG ;;
  t) topology=$OPTARG ;;
  p) base_port=$OPTARG ;;
  d) workdir=$OPTARG ;;
  a) advert_ms=$OPTARG ;;
  v) level=debug ;;
  c) clean=1 ;;
  h)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 2
    ;;
  esac
done

case "$topology" in
mesh | chain | star) ;;
*)
  echo "unknown topology '$topology'" >&2
  exit 2
  ;;
esac

if ! [[ $count =~ ^[0-9]+$ ]] || [ "$count" -lt 2 ]; then
  echo "need at least two nodes" >&2
  exit 2
fi

binary=${MESHCORE_NODE:-build/meshcore-node}
if [ ! -x "$binary" ]; then
  echo "$binary not found; build it first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi
binary=$(realpath "$binary")

if [ "$clean" -eq 1 ]; then
  rm -rf "$workdir"
fi
mkdir -p "$workdir"
workdir=$(realpath "$workdir")

port_of()
{
  echo $((base_port + $1 - 1))
}

# Who node $1 can hear. A node nobody lists is heard by nobody, which is how a
# chain stays a chain instead of collapsing into a mesh over loopback.
peers_of()
{
  local self=$1
  local peers=()
  local other

  case "$topology" in
  mesh)
    for ((other = 1; other <= count; other++)); do
      if [ "$other" -ne "$self" ]; then peers+=("$other"); fi
    done
    ;;
  chain)
    if [ "$self" -gt 1 ]; then peers+=($((self - 1))); fi
    if [ "$self" -lt "$count" ]; then peers+=($((self + 1))); fi
    ;;
  star)
    if [ "$self" -eq 1 ]; then
      for ((other = 2; other <= count; other++)); do
        peers+=("$other")
      done
    else
      peers+=(1)
    fi
    ;;
  esac

  local list=""
  for other in ${peers[@]+"${peers[@]}"}; do
    if [ -n "$list" ]; then list="$list, "; fi
    list="$list\"127.0.0.1:$(port_of "$other")\""
  done
  echo "$list"
}

pids=()

stop()
{
  trap - INT TERM EXIT
  echo
  echo "stopping ${#pids[@]} nodes"
  # SIGTERM, not SIGKILL: the node saves its contacts and room state on the way
  # out, and killing it outright is how you lose them.
  for pid in ${pids[@]+"${pids[@]}"}; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  echo "logs and state are in $workdir"
}

trap stop INT TERM EXIT

echo "topology $topology, $count nodes, ports $(port_of 1)-$(port_of "$count")"

for ((node = 1; node <= count; node++)); do
  name="node-$node"
  home="$workdir/$name"
  mkdir -p "$home/data"
  chmod 700 "$home/data"

  config="$home/meshcore.json"
  cat >"$config" <<JSON
{
  "node": {
    "dir": "$home/data",
    "name": "$name",
    "flush_ms": 30000,
    "advert_ms": $advert_ms
  },
  "log": {
    "level": "$level"
  },
  "radio": {
    "driver": "udp",
    "udp_bind": "127.0.0.1",
    "udp_port": $(port_of "$node"),
    "udp_peers": [$(peers_of "$node")]
  },
  "telemetry": {
    "enabled": true,
    "queue": 256,
    "report_ms": 15000
  }
}
JSON

  # Every line is labelled with the node it came from, and kept in a file as
  # well: with five nodes adverting at once the terminal alone is unreadable.
  "$binary" "$config" > >(tee "$home/node.log" | sed -u "s/^/[$name] /") 2>&1 &
  pids+=($!)

  echo "$name  port $(port_of "$node")  peers [$(peers_of "$node")]"
done

echo "running, Ctrl-C to stop"
wait
