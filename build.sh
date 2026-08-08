#!/usr/bin/env bash
set -euo pipefail

exp="${1:?usage: build.sh <experiment-dir-name>}"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_dir="$repo_dir/src/$exp"
out_dir="$repo_dir/out/$exp"

if [[ ! -d "$src_dir" ]]; then
    echo "error: no such experiment: $src_dir" >&2
    exit 1
fi

mkdir -p "$out_dir"

CC=arm-none-eabi-gcc
AS=arm-none-eabi-as
LD=arm-none-eabi-ld
CFLAGS="-marm -march=armv7-a -ffreestanding -fno-builtin -nostdlib -O0 -g -Wall -Wextra -I$repo_dir/src/common"

objs=()

for f in "$repo_dir/src/common"/*.S "$repo_dir/src/common"/*.c "$src_dir"/*.S "$src_dir"/*.c; do
    [[ -e "$f" ]] || continue
    obj="$out_dir/$(basename "$f").o"
    "$CC" $CFLAGS -c "$f" -o "$obj"
    objs+=("$obj")
done

"$CC" $CFLAGS -T "$repo_dir/src/common/link.ld" -nostdlib -Wl,-Map="$out_dir/out.map" \
    "${objs[@]}" -o "$out_dir/out.elf"

echo "built: $out_dir/out.elf"
