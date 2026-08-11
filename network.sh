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
#   ./network.sh -n 5 -t chain -T 2=repeater,3=repeater,4=repeater
#                                    a board at each end, transit in between
#   ./network.sh -n 3 -T default=room,2=room-repeater
#                                    two boards that can only reach each other
#                                    through the one node that carries

set -euo pipefail

count=3
topology=mesh
base_port=45601
workdir=./run
advert_ms=10000
level=info
clean=0
types=
default_type=room-repeater

usage()
{
  cat <<'TEXT'
usage: network.sh [-n count] [-t mesh|chain|star] [-p base-port] [-d dir]
                  [-a advert-ms] [-T node=type,...] [-v] [-c]

  -n  number of nodes (default 3)
  -t  topology (default mesh)
        mesh   every node hears every other node
        chain  node i hears only i-1 and i+1
        star   node 1 hears everyone, the rest hear only node 1
  -p  udp port of the first node, the rest follow (default 45601)
  -d  working directory for configs, data and logs (default ./run)
  -a  advert interval in milliseconds (default 10000, the node default is
      300000 which is too slow to watch)
  -T  what a node is, as node=type pairs: -T 2=repeater,5=room. May be given
      more than once, and default=type changes what an unnamed node is.
        room           a board of its own, carries nothing for anybody
        repeater       transit only, with no board to post to
        room-repeater  both, and what an unnamed node is
  -v  log at debug instead of info
  -c  wipe the working directory first, so every node starts as a stranger
TEXT
}

while getopts "n:t:p:d:a:T:vch" option; do
  case "$option" in
  n) count=$OPTARG ;;
  t) topology=$OPTARG ;;
  p) base_port=$OPTARG ;;
  d) workdir=$OPTARG ;;
  a) advert_ms=$OPTARG ;;
  T) types="${types:+$types,}$OPTARG" ;;
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

valid_type()
{
  case "$1" in
  room | repeater | room-repeater) return 0 ;;
  esac
  return 1
}

# The whole -T list is settled before a single node starts. A typo found in the
# log of a network that is already running is found too late: by then five
# processes are up and the one that matters is silently the wrong thing.
node_types=()
if [ -n "$types" ]; then
  IFS=, read -ra assignments <<<"$types"
  for assignment in "${assignments[@]}"; do
    node=${assignment%%=*}
    type=${assignment#*=}

    if [ "$node" = "$assignment" ] || [ -z "$node" ] || [ -z "$type" ]; then
      echo "'$assignment' is not node=type" >&2
      exit 2
    fi
    if ! valid_type "$type"; then
      echo "unknown node type '$type', expected room, repeater or room-repeater" >&2
      exit 2
    fi

    if [ "$node" = default ]; then
      default_type=$type
      continue
    fi
    if ! [[ $node =~ ^[0-9]+$ ]] || [ "$node" -lt 1 ] || [ "$node" -gt "$count" ]; then
      echo "'$assignment' names no node in a network of $count" >&2
      exit 2
    fi
    node_types[$node]=$type
  done
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

# What node $1 is: whatever -T said, and otherwise a room-repeater. Carrying is
# the default because a demo network whose middle nodes drop everything is a
# demo of nothing — a `room` is a deliberate choice, made one node at a time.
type_of()
{
  echo "${node_types[$1]:-$default_type}"
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
    "type": "$(type_of "$node")",
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

  echo "$name  $(type_of "$node")  port $(port_of "$node")  peers [$(peers_of "$node")]"
done

echo "running, Ctrl-C to stop"
wait
