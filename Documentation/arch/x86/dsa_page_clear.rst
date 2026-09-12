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

Batching in synchronous and unsafe modes
---------------------------------------

``batch_size`` under ``/sys/module/idxd_page_clear/parameters/`` sets the
maximum number of MEMFILL descriptors in a batch. It accepts 1..1024 and
defaults to 1, which uses the original direct submission path. It is
independent of ``unsafe_async`` and can be changed without reloading the
module. Each child still represents one contiguous range from one free
request; large ranges are not split into page-sized descriptors. Separate
ranges can be noncontiguous.

The read-only ``batch_capacity`` reports the bound queue's usable limit:
the minimum of the device limit, WQ ``max_batch_size``, 1024, and the number
of software descriptor slots minus one. The effective maximum is the
smaller of ``batch_size`` and ``batch_capacity``. Batching requires BATCH
support in both the device and the WQ operation configuration (if present),
and at least three descriptor slots. Otherwise ``batch_capacity`` is 1 and
requests use direct submission. Configure the WQ's ``max_batch_size`` before
binding; the module parameter does not change hardware queue configuration.

On a capable queue, binding reserves one descriptor for the BATCH parent
and allocates a coherent, 64-byte-aligned child descriptor array. The
reservation remains even with ``batch_size=1``. This prototype allows one
hardware batch in flight at a time; new requests can accumulate while that
batch executes. It reduces portal submissions, but still allocates one
software descriptor and DMA mapping per free request. Locking, collection,
and polling costs remain, so batching does not guarantee a speedup.

``batch_wait_us`` sets a collection window of 0..1000 microseconds from
the oldest queued request, default 0. A dispatcher submits when the
effective maximum is reached or the window has elapsed. Zero collects
only the requests already available. A partial batch is submitted too;
a single remaining request uses a direct MEMFILL for compatibility with
devices that do not support one-child batches. This is a busy-wait window,
not a guarantee on total latency: worker scheduling and preceding batches
can delay submission further.

In synchronous mode, concurrent callers can contribute to the same batch.
Callers help dispatch batches themselves because they run with preemption
disabled and cannot rely on a worker being scheduled. Each caller waits
until its descriptor's result is published after hardware completion, then
unmaps its destination before returning. Submission failure, an unexecuted
child, or a failed child results in CPU clearing by that child's caller.
Serial frees from a single thread cannot accumulate across calls: they
produce single MEMFILLs, and a nonzero collection window adds latency.

In unsafe mode, the callback returns after software queueing, potentially
before hardware submission. The worker or a concurrent synchronous caller
dispatches queued requests and retires resources after completion. The
existing unsafe reuse race still applies. A submission rejected after the
callback returned counts as ``async_error_pages``; CPU repair is no longer
possible at that point. Mode and cache-control choices are retained per
request. Batch size and collection window are sampled per dispatch, so a
change applies to subsequent dispatches, including already queued requests.

For example, on the target machine, as root::

    cd /sys/module/idxd_page_clear/parameters
    cat batch_capacity
    echo 0 > unsafe_async
    echo 16 > batch_size
    echo 0 > batch_wait_us
    # Run a concurrent allocation/free benchmark in synchronous mode.
    echo 2 > batch_wait_us
    # Repeat with a 2-us collection window.
    echo 1 > unsafe_async
    # Repeat the existing unsafe experiment with the same batch settings.

Use counter deltas between benchmark runs:

* ``batch_submissions``: BATCH descriptors accepted by the WQ.
* ``batched_descriptors``: MEMFILL children in those accepted batches.
* ``single_submissions``: directly submitted MEMFILLs, including partial
  batches containing only one request and batching-disabled requests.

``delta(batched_descriptors) / delta(batch_submissions)`` gives the mean
number of children per submitted batch when the denominator is nonzero.
These count submissions, not successful completions; also check
``completed_pages`` and ``async_error_pages``. Hardware batch completion
and child completion ordering follow the `Intel DSA architecture
specification, sections 3.8 and 8.3.2
<https://cdrdv2-public.intel.com/857060/341204-006-intel-data-streaming-accelerator-spec.pdf>`_.

Unsafe early-return experiment with the old kernel
------------------------------------------------

This module-only prototype targets the synchronous provider ABI from commit
``ef0fd3bebb0a``. It does not require the discarded async changes to the page
allocator or a new kernel image. ``unsafe_async`` defaults to false::

    echo 0 > /sys/module/idxd_page_clear/parameters/unsafe_async
    # Normal synchronous mode.
    echo 1 > /sys/module/idxd_page_clear/parameters/unsafe_async
    # UNSAFE: return after software queueing (batch) or submission (direct).

In unsafe mode, the freeing caller does not wait for completion. Pages are
not isolated or retained: the old allocator can reuse them immediately.
An outstanding DMA fill can overwrite another allocation, including its
metadata. This can corrupt results or crash the machine. This is a timing
experiment that discards init_on_free correctness, not an asynchronous
implementation preserving zero-before-reuse.

A worker (or a synchronous batch dispatcher) checks completion to retire
the descriptor and DMA mapping.
Recycling either while hardware is still using it would invalidate later
submissions. The worker does not verify destination contents, clear failed
fills with the CPU, or free pages. Bounce-buffer unmapping itself can copy
data into a destination already reused by another owner. There is no attempt
to repair that race. Missing completion still uses the one-second panic
timeout, which worker scheduling can delay.

Queue exhaustion, mapping failure or rejected direct submission falls back
to CPU clearing. Deferred batch submission failures are counted as errors.
Compare counter deltas to distinguish actual offload from fallback:

* ``submitted_pages``: pages in accepted unsafe submissions.
* ``pending_pages``: pages covered by unsafe requests, including those still
  queued in software, whose resources have not been retired; these pages
  are **not** protected from reuse.
* ``async_fallback_pages``: pages rejected by the unsafe callback and left
  to CPU fallback. This excludes frees bypassing the provider entirely,
  for example atomic-context frees.
* ``async_error_pages``: pages in unsafe fills reporting an error, including
  unexecuted children and rejected deferred submissions.
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

    cd /home/sawa/TIFS/DSA_init_on_free/linux-dsa-main
    sh tools/dsa-page-clear/build-module.sh

The output is ``drivers/dma/idxd/idxd_page_clear.ko`` inside the shared
source directory, ready for the separate installation script.
``DSA_KDIR`` can select a different prepared build tree matching the old
running kernel. Its configuration, generated headers and complete
``Module.symvers`` must match that kernel and its existing IDXD modules.
``DSA_CC`` (default ``gcc``) and ``DSA_JOBS`` override the compiler and
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

For batches, first use ``unsafe_async=0``. Repeat with ``batch_size`` set
to 1, 2, 16 and a value above ``batch_capacity``, and with ``batch_wait_us``
set to 0 and 2. Check that a single-thread workload finishes even with a
large batch size, and that a concurrent workload increments
``batch_submissions`` and ``batched_descriptors``. Check zeroing with
``test_meminit``; its serial tests need not produce multi-child batches.
Exercise partial batches, descriptor exhaustion, changes back to
``batch_size=1`` under load, and unbinding with pending requests. Values 0
and 1025 for ``batch_size``, and 1001 for ``batch_wait_us``, must be rejected.
Only then repeat the unsafe timing experiment; it cannot validate zeroing
correctness. After stopping its workload, disable ``unsafe_async`` and
wait for ``pending_pages`` to reach zero before unbinding.

Compilation alone does not validate DMA ordering, fault handling, queue
removal races, or performance; those require the target hardware.
