#!/bin/sh
set -e

if [ "$(uname -s)" != "Linux" ]; then
    echo "build-server.sh: the simulation uses epoll and timerfd, run this under WSL" >&2
    exit 1
fi

if ! command -v clang > /dev/null 2>&1; then
    echo "build-server.sh: clang not found, install it with: sudo apt install clang" >&2
    exit 1
fi

root=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$root/build"

common="-std=c23 -O2 -Wall -Wextra"

case "$1" in
    debug)
        echo "server.c (debug)"
        clang $common -g -DSERENITAS_DEBUG -o "$root/build/server" "$root/server.c" -lm
        echo "build/server"
        ;;
    tests)
        echo "tests.c"
        clang $common -g -Wno-unused-function -o "$root/build/tests" "$root/tests.c" -lm
        echo "build/tests"
        ;;
    "")
        echo "server.c"
        clang $common -o "$root/build/server" "$root/server.c" -lm
        echo "build/server"
        ;;
    *)
        echo "build-server.sh: unknown target '$1', expected debug or tests" >&2
        exit 1
        ;;
esac
