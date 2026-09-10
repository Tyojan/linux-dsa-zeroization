#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run on the build machine (for example the TDX machine), not the target.
set -eu

DSA_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DSA_SRC=$(CDPATH= cd -- "$DSA_SCRIPT_DIR/../.." && pwd)
DSA_KDIR=${DSA_KDIR:-$DSA_SRC}
DSA_MODULE_DIR=$DSA_SRC/drivers/dma/idxd
DSA_CC=${DSA_CC:-gcc-11}
DSA_JOBS=${DSA_JOBS:-$(nproc)}

DSA_KDIR=$(CDPATH= cd -- "$DSA_KDIR" && pwd)
test -f "$DSA_KDIR/include/generated/autoconf.h"
test -f "$DSA_KDIR/Module.symvers"
test -f "$DSA_KDIR/arch/x86/include/asm/dsa.h"

# This prototype uses the old, synchronous provider ABI. Do not accidentally
# build it using the discarded kernel's symbol versions or header layout.
awk '
    $2 == "dsa_register_page_clear" { provider = 1 }
    $2 == "idxd_submit_desc_nowait" { submit = 1 }
    $2 == "dsa_free_pages_complete" { new_abi = 1 }
    END { exit !(provider && submit && !new_abi) }
' "$DSA_KDIR/Module.symvers" || {
    echo "DSA_KDIR must contain the old kernel's full Module.symvers." >&2
    exit 1
}
if grep -q 'clear_async' "$DSA_KDIR/arch/x86/include/asm/dsa.h"; then
    echo "DSA_KDIR still contains the discarded asynchronous kernel ABI." >&2
    exit 1
fi

# Force a fresh module link without split BTF tied to another vmlinux.
# An empty override is intentional: CONFIG_DEBUG_INFO_BTF_MODULES=n is
# still nonempty to Kbuild's ifdef and would not disable this step.
rm -f "$DSA_MODULE_DIR/idxd_page_clear.ko"
make -C "$DSA_KDIR" -j"$DSA_JOBS" CC="$DSA_CC" W=1 \
    M="$DSA_MODULE_DIR" CONFIG_DEBUG_INFO_BTF_MODULES= idxd_page_clear.ko

echo "Built: $DSA_MODULE_DIR/idxd_page_clear.ko"
