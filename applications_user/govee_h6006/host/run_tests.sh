#!/bin/sh
# Build and run the host tests for the portable core (../core) with the system compiler.
# Usage: nice -n 19 host/run_tests.sh
# The binary goes to host/build/ (ignored by git). The tests also rewrite the
# status fixtures in host/tests/fixtures/, which `govee_remote.py selftest` reads.
set -eu

HOST_DIR=$(cd "$(dirname "$0")" && pwd)
CORE_DIR=$(cd "$HOST_DIR/../core" && pwd)
TESTS_DIR="$HOST_DIR/tests"
BUILD_DIR="$HOST_DIR/build"
CC=${CC:-cc}
CFLAGS="-std=gnu17 -Wall -Wextra -Werror -Wdouble-promotion -Wredundant-decls -Wundef -Wshadow -O1 -g"

mkdir -p "$BUILD_DIR" "$TESTS_DIR/fixtures"

nice -n 19 "$CC" $CFLAGS -I"$CORE_DIR" -I"$TESTS_DIR" -o "$BUILD_DIR/govee_core_tests" \
    "$CORE_DIR/govee_h6006.c" \
    "$CORE_DIR/govee_registry.c" \
    "$CORE_DIR/govee_engine.c" \
    "$CORE_DIR/govee_status.c" \
    "$TESTS_DIR/sim.c" \
    "$TESTS_DIR/test_registry.c" \
    "$TESTS_DIR/test_engine.c" \
    "$TESTS_DIR/test_status.c" \
    "$TESTS_DIR/test_main.c"

nice -n 19 "$BUILD_DIR/govee_core_tests" "$TESTS_DIR/fixtures"
