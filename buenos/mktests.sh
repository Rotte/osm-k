#!/bin/sh

disk=fyams.harddisk

set +e

make -C tests

for src in tests/*c tests/*txt; do
    prog="$(echo "$src" | sed s/.c$//)"
    if [ -f "$prog" ]; then
        util/tfstool delete "$disk" $(basename "$prog")
        util/tfstool write "$disk" "$prog" $(basename "$prog")
    fi
done

