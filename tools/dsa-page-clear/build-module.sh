#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run on the build machine (for example the TDX machine), not the target.
set -eu

DSA_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DSA_SRC=$(CDPATH= cd -- "$DSA_SCRIPT_DIR/../.." && pwd)
DSA_KDIR=${DSA_KDIR:-$DSA_SRC}
DSA_MODULE_DIR=$DSA_SRC/drivers/dma/idxd
DSA_CC=${DSA_CC:-gcc}
DSA_JOBS=${DSA_JOBS:-$(nproc)}
DSA_KCFLAGS=${KCFLAGS:-}

DSA_KDIR=$(CDPATH= cd -- "$DSA_KDIR" && pwd)
for DSA_REQUIRED_FILE in \
    include/generated/autoconf.h \
    Module.symvers \
    scripts/Makefile.build \
    scripts/Makefile.ubsan \
    scripts/pahole-version.sh; do
    if [ ! -f "$DSA_KDIR/$DSA_REQUIRED_FILE" ]; then
        echo "DSA_KDIR is not a complete prepared kernel build tree: $DSA_KDIR" >&2
        echo "Missing: $DSA_KDIR/$DSA_REQUIRED_FILE" >&2
        echo "Build against the tree used for the running kernel, for example:" >&2
        echo "  DSA_KDIR=/path/to/prepared/linux $0" >&2
        exit 1
    fi
done
if [ ! -f "$DSA_SRC/arch/x86/include/asm/dsa.h" ]; then
    echo "Missing page-clear provider header: $DSA_SRC/arch/x86/include/asm/dsa.h" >&2
    exit 1
fi

# Safe deferred release requires the matching allocator and provider ABI.
awk '
    $2 == "dsa_register_page_clear_v2" { provider = 1 }
    $2 == "idxd_submit_desc_nowait" { submit = 1 }
    $2 == "dsa_free_pages_complete" { complete = 1 }
    END { exit !(provider && submit && complete) }
' "$DSA_KDIR/Module.symvers" || {
    echo "Build the safe-async kernel first; DSA_KDIR needs its full Module.symvers." >&2
    exit 1
}
# Force a fresh module link without split BTF tied to another vmlinux.
# An empty override is intentional: CONFIG_DEBUG_INFO_BTF_MODULES=n is
# still nonempty to Kbuild's ifdef and would not disable this step.
rm -f "$DSA_MODULE_DIR/idxd_page_clear.ko"
# Compile against the matching provider header and allocator symbol CRCs.
DSA_KCFLAGS="$DSA_KCFLAGS -I$DSA_SRC/arch/x86/include"
make -C "$DSA_KDIR" -j"$DSA_JOBS" CC="$DSA_CC" KCFLAGS="$DSA_KCFLAGS" W=1 \
    M="$DSA_MODULE_DIR" CONFIG_DEBUG_INFO_BTF_MODULES= idxd_page_clear.ko

echo "Built: $DSA_MODULE_DIR/idxd_page_clear.ko"
