# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says, never on the broker's log - the log is kept for reading when a test fails.
#
#   SIGFOX_BROKER   the broker binary (default: coraine on PATH - the coraine package, or
#                 `. <dir>/install/env` after coraine-build)
#   SIGFOX_PORT     its port (default 19032 - clear of a broker on 1026, of coraine's own test ports and of the other bridges' tests)
#   SIGFOX_PLUGIN   the plugin under test (default: iotasigfox.so in this repo)
#   SIGFOX_TEST_DIR scratch directory: the bridge configuration, the broker's pid and log
#
SIGFOX_BROKER="${SIGFOX_BROKER:-coraine}"
SIGFOX_PORT="${SIGFOX_PORT:-19032}"
SIGFOX_PLUGIN="${SIGFOX_PLUGIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/iotasigfox.so}"
SIGFOX_TEST_DIR="${SIGFOX_TEST_DIR:-/tmp/iotasigfox-bridge-test}"


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $SIGFOX_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$SIGFOX_BROKER" --port "$SIGFOX_PORT" --foreground --pretty-print 2 --httpEndpoint "http://localhost:$SIGFOX_PORT" \
                 --database ramDB --bridges "$SIGFOX_PLUGIN" --bridgeConfig "$SIGFOX_TEST_DIR/bridges.json" "$@" \
                 > "$SIGFOX_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$SIGFOX_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$SIGFOX_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$SIGFOX_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $SIGFOX_TEST_DIR/broker.log:" >&2
      tail -20 "$SIGFOX_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $SIGFOX_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$SIGFOX_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}


# -----------------------------------------------------------------------------
#
# The southbound port - where the Sigfox backend calls back (default 17432 - clear of iotagent-sigfox's 17428)
#
SIGFOX_SOUTH_PORT="${SIGFOX_SOUTH_PORT:-17432}"


# -----------------------------------------------------------------------------
#
# callbackGet <path> <query> - a callback as iotagent-sigfox takes it, GET; the status, then the body
#
callbackGet() {
  curl -s -w "\n%{http_code}\n" "localhost:$SIGFOX_SOUTH_PORT/$1?$2" > "$SIGFOX_TEST_DIR/callback.out"
  tail -1 "$SIGFOX_TEST_DIR/callback.out"
  head -n -1 "$SIGFOX_TEST_DIR/callback.out" | sed '/^$/d'
}


# -----------------------------------------------------------------------------
#
# callbackPost <path> <JSON body> - a Sigfox custom callback, POST application/json; the status, then the body
#
callbackPost() {
  curl -s -w "\n%{http_code}\n" -X POST "localhost:$SIGFOX_SOUTH_PORT/$1" -H 'Content-Type: application/json' -d "$2" > "$SIGFOX_TEST_DIR/callback.out"
  tail -1 "$SIGFOX_TEST_DIR/callback.out"
  head -n -1 "$SIGFOX_TEST_DIR/callback.out" | sed '/^$/d'
}


# -----------------------------------------------------------------------------
#
# entityAwait <id> <text> [secs] - wait until the entity (keyValues) has <text>
#
entityAwait() {
  local url="localhost:$SIGFOX_PORT/ngsi-ld/v1/entities/$1?options=keyValues" secs="${3:-5}" i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "$url" | grep -qF -- "$2" && return 0
    sleep 0.1
  done

  echo "entityAwait: $1 is $(curl -s "$url"), without '$2', after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# entityGet <id> [options] - the entity, its members sorted
#
entityGet() {
  curl -s "localhost:$SIGFOX_PORT/ngsi-ld/v1/entities/$1${2:+?options=$2}" \
    | python3 -c 'import sys, json; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# attrPatch <id> <attr> <value as JSON> - PATCH an attribute's value; the HTTP status
#
attrPatch() {
  curl -s -o /dev/null -w "PATCH $2: %{http_code}\n" -X PATCH "localhost:$SIGFOX_PORT/ngsi-ld/v1/entities/$1/attrs/$2" \
       -H 'Content-Type: application/json' -d "{\"value\": $3}"
}
