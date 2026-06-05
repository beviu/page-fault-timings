- `end` is a timestamp collected by a `RDTSC` instruction right after the
  faulting instruction. When the page fault is handled, the CPU returns to the
  faulting instruction which succeeds this time, and then that timestamp is
  collected. It is very close to the end of the page fault.
- `isr_entry` is placed very early in the kernel ISR (Interrupt Service Handler)
  for page fault. At this point, the code is still written in assembly code and
  the CR3 register has not been changed yet for KPTI.
- `c_entry` is after the CR3 register has been updated. At this point, we've just
  entered the C code for page fault handling.
- `c_exit` is just before returning from the C code to the assembly code.
- `iret` is just before the `IRET` instruction which returns from the ISR. It's
  after the CR3 register has been switched back for KPTI.

The rest of them describe specific functions in the kernel code. The `_start` and
`_end` suffix mean that the timestamp is taken at the start of function and
at the end of the function respectively. When looking at the stacked chart, the
area between the `_start` and the `_end` will be the time that the function took.

`first_try_handle_mm_fault_with_mm_lock_end` and
`retry_handle_mm_fault_with_mm_lock_end` is because for UFFD, we realized that
the `handle_mm_fault` function was called twice.

For UFFD, there is also `wake_up_userfaultfd_start` and
`wake_up_userfaultfd_end` which correspond to when the thread blocking on
`read`ing the UFFD is woken up with `wake_up_poll` and `msg_received` which is
right after that `read` returns in that thread.
