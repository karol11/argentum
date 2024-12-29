#!/bin/bash
cd "$(dirname "${BASH_SOURCE[0]}")/../workdir" && \
../bin/agc -src ../ag -start $1 -o "../apps/$1.o" && \
gcc -no-pie ../apps/$1.o ../libs/libag_runtime.a -L/usr/lib/x86_64-linux-gnu -lm -o "../apps/$1" && \
../apps/$1
