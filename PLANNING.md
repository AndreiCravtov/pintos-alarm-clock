use synchronization primitives for non-global synchronization (i.e. only some of the threads need a resource)
use interruption disabling for global synchronization (i.e. inside an interrupt handler)

always prefer non-global synchronization if possible, otherwise marks will be deduced

# Some comments from edstem

we CAN disable interrupts for synchronization of `list_insert_ordered` for
a list that is used by BOTH `timer sleep` and `timer interrupt` handlers

# Some key invariants

thread.wakeup_time_ticks -> must be -1 if not in use
sleeping_list -> list of sleeping threads
sleeping_list_min_wakeup_time_ticks -> must be mimimum item of sleeping list, or -1 if empty
