.. SPDX-License-Identifier: GPL-2.0

DSA page clearing on free
========================

This experimental implementation offloads page allocator ``init_on_free``
zeroing to Intel DSA. It does not change SLUB object clearing or
``init_on_alloc``. ``clear_pages_with_dsa()`` in ``asm/page_64.h`` is a
synchronous wrapper: it tries a registered provider and otherwise calls the
existing CPU ``clear_pages()`` implementation.

The default synchronous mode honors this contract. The explicitly unsafe
``unsafe_async`` experiment below deliberately violates it.

Configuration
-------------

Build with ``CONFIG_INTEL_IDXD_PAGE_CLEAR=m`` (or ``y`` with built-in IDXD).
This selects the built-in ``CONFIG_X86_DSA_PAGE_CLEAR`` dispatch code, so the
page allocator can also use a modular driver. Boot with ``init_on_free=1``.
The existing page poisoning and KASAN initialization precedence still applies.

Load ``idxd_page_clear`` and configure an unused DSA work queue with:

* mode: ``dedicated``
* type: ``kernel``
* driver_name: ``dsa_page_clear``
* name: a nonempty name, for example ``page_clear``
* a group with an assigned engine, nonzero queue size and priority
* Memory Fill supported by the device and, if per-queue operation
  configuration is supported, enabled in the queue's operation configuration

Bind the configured, enabled DSA parent device to ``idxd`` first, then bind
the work queue to ``dsa_page_clear``. Only one page-clearing work queue may be
bound at a time. For example, after configuring an otherwise unused
``dsa0/wq0.0`` with accel-config and enabling ``dsa0``::

    modprobe idxd_page_clear
    echo wq0.0 > /sys/bus/dsa/drivers/dsa_page_clear/bind

This assumes the queue's ``driver_name`` was set before binding. Device and
queue identifiers depend on the machine; do not reuse a queue owned by
another client.

``/sys/module/idxd_page_clear/parameters/min_pages`` sets the minimum
contiguous range to offload (default: 1 page). Ranges larger than the queue's
transfer limit or the DMA mapping limit use the CPU. The read-only
``/sys/module/idxd_page_clear/parameters/completed_pages`` counter records
pages successfully zeroed by DSA, after DMA unmapping. It can be used to
verify that a workload actually uses the accelerator.

``/sys/module/idxd_page_clear/parameters/cache_control`` controls the
``IDXD_OP_FLAG_CC`` hint on Memory Fill descriptors. It defaults to false
(CC=0, memory-directed writes). Set it to true for CC=1, cache-directed
writes. These are placement hints, not guarantees of cache residency.
The parameter is sampled once when preparing each descriptor; changing it
does not modify descriptors already prepared or submitted. No queue rebind
or module reload is needed::

    echo 0 > /sys/module/idxd_page_clear/parameters/cache_control
    # Run the benchmark with CC=0.
    echo 1 > /sys/module/idxd_page_clear/parameters/cache_control
    # Run the same benchmark with CC=1.

The parameter reads back as ``N`` or ``Y``. It can also be set at module
load time with ``modprobe idxd_page_clear cache_control=1``. Compare runs
with the same ``min_pages`` and queue configuration, and record the delta
of ``completed_pages`` for each run to check the amount of successful
offload. Change the parameter between benchmark runs to avoid mixing modes.

Unsafe early-return experiment with the old kernel
------------------------------------------------

This module-only prototype targets the synchronous provider ABI from commit
``ef0fd3bebb0a``. It does not require the discarded async changes to the page
allocator or a new kernel image. ``unsafe_async`` defaults to false::

    echo 0 > /sys/module/idxd_page_clear/parameters/unsafe_async
    # Normal synchronous mode.
    echo 1 > /sys/module/idxd_page_clear/parameters/unsafe_async
    # UNSAFE: return immediately after a successful DSA submission.

In unsafe mode, the freeing caller does not wait for completion. Pages are
not isolated or retained: the old allocator can reuse them immediately.
An outstanding DMA fill can overwrite another allocation, including its
metadata. This can corrupt results or crash the machine. This is a timing
experiment that discards init_on_free correctness, not an asynchronous
implementation preserving zero-before-reuse.

A worker checks completion solely to retire the descriptor and DMA mapping.
Recycling either while hardware is still using it would invalidate later
submissions. The worker does not verify destination contents, clear failed
fills with the CPU, or free pages. Bounce-buffer unmapping itself can copy
data into a destination already reused by another owner. There is no attempt
to repair that race. Missing completion still uses the one-second panic
timeout, which worker scheduling can delay.

Queue exhaustion, mapping failure or rejected submission falls back to CPU
clearing. Compare counter deltas to distinguish actual offload from fallback:

* ``submitted_pages``: pages in accepted unsafe submissions.
* ``pending_pages``: pages covered by unsafe requests whose resources have
  not been retired; these pages are **not** protected from reuse.
* ``async_fallback_pages``: pages rejected by the unsafe callback and left
  to CPU fallback. This excludes frees bypassing the provider entirely,
  for example atomic-context frees.
* ``async_error_pages``: pages in unsafe fills reporting an error.
* ``completed_pages``: pages in fills reporting success, in either mode;
  this does not guarantee that their next owner was not corrupted.

Disable unsafe mode before changing experiments or unbinding, and observe
the remaining resource retirements::

    echo 0 > /sys/module/idxd_page_clear/parameters/unsafe_async
    while [ "$(cat /sys/module/idxd_page_clear/parameters/pending_pages)" -ne 0 ]; do
        sleep 0.01
    done

Removal stops new callbacks and drains retirement work before destroying
the WQ. Switching the parameter does not cancel already-submitted DMA.

Building only the module on another NFS client
---------------------------------------------

Run ``tools/dsa-page-clear/build-module.sh`` on the build machine, such as
the TDX machine. It builds ``idxd_page_clear.ko`` directly in
``drivers/dma/idxd/``, using the existing Makefile. It does not copy or
install the module, or build a kernel image. For example, with this tree NFS-mounted at
the same path::

    cd /home/sawa/TIFS/DSA_init_on_free/linux
    sh tools/dsa-page-clear/build-module.sh

The output is ``drivers/dma/idxd/idxd_page_clear.ko`` inside the shared
source directory, ready for the separate installation script.
``DSA_KDIR`` can select a different prepared build tree matching the old
running kernel. Its configuration, generated headers and complete
``Module.symvers`` must match that kernel and its existing IDXD modules.
``DSA_CC`` (default ``gcc-11``) and ``DSA_JOBS`` override the compiler and
parallelism.

The script disables module BTF generation for this experiment. Split module
BTF generated against a different ``vmlinux`` can fail validation even when
the kernel release string is identical. Disabling BTF does not fix an ABI
or symbol-CRC mismatch: the old kernel's build metadata is still required.
Install only the resulting ``idxd_page_clear.ko`` with the old matching
kernel and IDXD modules. Do not install the discarded kernel image or its
IDXD modules. This source tree does not contain a recovered copy of the old
kernel image; use the target's boot backup or another known-good build.

Recovering the target after the discarded kernel install
-------------------------------------------------------

If the target reports module BTF validation failures, boot a known-good
distribution kernel or recovery environment. Restore the boot files saved
before the discarded kernel was installed, and remove its module overrides
from depmod's search path. For the earlier install procedure, run on the
target (replace the backup directory with the actual pre-update backup)::

    KREL=7.3.0-rc2-dsa+
    ls -d /var/backups/dsa-kernel-*
    BOOT_BACKUP=/var/backups/dsa-kernel-YYYYMMDD-HHMMSS
    sudo cp -a "$BOOT_BACKUP"/. /boot/
    if [ -d "/lib/modules/$KREL/updates/dsa-async" ]; then
        sudo mv "/lib/modules/$KREL/updates/dsa-async" \
            "${BOOT_BACKUP}-discarded-modules"
    fi
    sudo depmod -a "$KREL"
    sudo update-initramfs -u -k "$KREL"
    sudo update-grub
    sudo reboot

This assumes the original matching modules remain installed outside the
``updates/dsa-async`` override directory. Restoring only the kernel image
while retaining the incompatible overrides is insufficient. If the original
modules or boot backup are unavailable, recover them from the matching old
build before installing the prototype.

Execution and failure handling
------------------------------

The driver uses preallocated descriptors, nonblocking acquisition and
submission, DMA API destination mappings, and a zero-pattern Memory Fill
descriptor. Completion is polled without requesting a completion interrupt.
In synchronous mode the destination is unmapped before returning or
performing CPU fallback.
The completion-record protocol follows the `Intel DSA architecture
specification, section 3.6
<https://cdrdv2-public.intel.com/857060/341204-006-intel-data-streaming-accelerator-spec.pdf>`_.

Interrupt contexts, IRQ-disabled or atomic callers, recursive frees from DMA
mapping, and ``FPI_NOLOCK`` frees use the CPU. PREEMPT_RT is not supported.
The provider runs with preemption disabled, so it must not sleep. Queue
removal and module unloading stop new callbacks and wait for active calls
before releasing the queue and descriptors.

In synchronous mode, an unavailable queue, exhausted descriptors, failed
mapping or rejected submission uses CPU clearing. A completed descriptor reporting an error also
falls back to CPU clearing of the entire range. An accepted descriptor which
does not report completion within one second causes a **kernel panic**:
returning and reusing the page in this case could allow delayed DMA to
overwrite its next owner's data. Device faults or resets can trigger this
case; there is no timeout recovery implementation yet.

The default mode is synchronous offload. Submission,
mapping and polling overhead can outweigh any benefit, especially for 4 KiB
pages. Measure different ``min_pages`` values on the target machine.

Validation on a DSA machine
--------------------------

Build ``CONFIG_TEST_MEMINIT=m`` and run the existing ``test_meminit`` module
with ``init_on_free=1`` before and after binding the work queue. Check the
test results in the kernel log and confirm that ``completed_pages`` increases
when the queue is bound. Repeat after unbinding the queue to exercise CPU
fallback. Test concurrent allocation/free workloads while unbinding and
rebinding the queue, and inspect the log for DMA API or locking errors.

Compilation alone does not validate DMA ordering, fault handling, queue
removal races, or performance; those require the target hardware.
