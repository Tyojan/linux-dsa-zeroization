.. SPDX-License-Identifier: GPL-2.0

DSA page clearing on free
========================

This experimental implementation offloads page allocator ``init_on_free``
zeroing to Intel DSA. It does not change SLUB object clearing or
``init_on_alloc``. ``clear_pages_with_dsa()`` in ``asm/page_64.h`` is a
synchronous wrapper: it tries a registered provider and otherwise calls the
existing CPU ``clear_pages()`` implementation.

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
* Memory Fill enabled in the queue's operation configuration

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

Execution and failure handling
------------------------------

The driver uses preallocated descriptors, nonblocking acquisition and
submission, DMA API destination mappings, and a zero-pattern Memory Fill
descriptor. Completion is polled without requesting a completion interrupt.
The destination is unmapped before returning or performing CPU fallback.
The completion-record protocol follows the `Intel DSA architecture
specification, section 3.6
<https://cdrdv2-public.intel.com/857060/341204-006-intel-data-streaming-accelerator-spec.pdf>`_.

Interrupt contexts, IRQ-disabled or atomic callers, recursive frees from DMA
mapping, and ``FPI_NOLOCK`` frees use the CPU. PREEMPT_RT is not supported.
The provider runs with preemption disabled, so it must not sleep. Queue
removal and module unloading stop new callbacks and wait for active calls
before releasing the queue and descriptors.

An unavailable queue, exhausted descriptors, failed mapping or rejected
submission uses CPU clearing. A completed descriptor reporting an error also
falls back to CPU clearing of the entire range. An accepted descriptor which
does not report completion within one second causes a **kernel panic**:
returning and reusing the page in this case could allow delayed DMA to
overwrite its next owner's data. Device faults or resets can trigger this
case; there is no timeout recovery implementation yet.

This is synchronous offload, not asynchronous page recycling. Submission,
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
