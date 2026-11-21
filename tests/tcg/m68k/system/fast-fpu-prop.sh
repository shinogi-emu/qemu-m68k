#!/bin/sh
# Check the "fast-fpu" CPU property default on each m68k machine and the
# explicit overrides.  Usage: fast-fpu-prop.sh /path/to/qemu-system-m68k
QEMU=${1:?usage: $0 qemu-system-m68k}
fail=0

# q800 and next-cube want a ROM, the ColdFire boards a kernel, when there is
# no -kernel: any file will do, as nothing runs (-S).
fw=$(mktemp)
trap 'rm -f "$fw"' EXIT
printf '\000\000\020\000\000\000\000\000' > "$fw"

cpu_path() {
    # The full QOM path of the first object with a CPU type, rebuilt from
    # the tree's indentation (two spaces a level): /machine/unattached/
    # device[0] on most boards, /machine/cpu on q800.
    path=$(printf 'info qom-tree\nquit\n' |
           "$QEMU" "$@" -display none -S -monitor stdio 2>/dev/null |
           tr -d '\r' |
           awk '/^ *\/[^ ]* \(/ {
                    match($0, /^ */); d = RLENGTH / 2; p[d] = $1
                    if ($2 ~ /-m68k-cpu\)$/) {
                        s = ""; for (i = 0; i <= d; i++) s = s p[i]
                        print s; exit
                    }
                }')
    echo "${path:-/machine/unattached/device[0]}"
}

check() {
    expect=$1; shift
    path=$(cpu_path "$@")
    out=$(printf 'qom-get %s fast-fpu\nquit\n' "$path" |
          "$QEMU" "$@" -display none -S -monitor stdio 2>&1 | tr -d '\r')
    got=$(echo "$out" | grep -E '^(true|false)$' | head -n 1)
    if [ "$got" = "$expect" ]; then
        echo "PASS: $* -> $got"
    else
        err=$(echo "$out" | grep -E 'rror|not found' | head -n 1)
        echo "FAIL: $* -> '${got}' (expected $expect) $err"
        fail=1
    fi
}

# virt has it on, for every CPU type and in every version of the machine.
check true  -M virt
check true  -M virt -cpu m68060
check true  -M virt -cpu m68030
check true  -M virt -cpu cfv4e
check true  -M virt-10.2
check false -M virt -cpu m68040,fast-fpu=off
check false -M virt -cpu m68060,fast-fpu=off
check false -M virt -global m68k-cpu.fast-fpu=off
# The other machines have it off.
check false -M q800 -bios "$fw"
check true  -M q800 -bios "$fw" -cpu m68040,fast-fpu=on
check false -M next-cube -bios "$fw"
check false -M mcf5208evb -kernel "$fw"

exit $fail
