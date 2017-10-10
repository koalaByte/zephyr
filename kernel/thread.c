/*
 * Copyright (c) 2010-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Kernel thread support
 *
 * This module provides general purpose thread support.
 */

#include <kernel.h>

#include <toolchain.h>
#include <linker/sections.h>

#include <kernel_structs.h>
#include <misc/printk.h>
#include <sys_clock.h>
#include <drivers/system_timer.h>
#include <ksched.h>
#include <wait_q.h>
#include <atomic.h>
#include <syscall_handler.h>

extern struct _static_thread_data _static_thread_data_list_start[];
extern struct _static_thread_data _static_thread_data_list_end[];

#ifdef CONFIG_USERSPACE
/* Each thread gets assigned an index into a permission bitfield */
static atomic_t thread_index;

static unsigned int thread_index_get(void)
{
	unsigned int retval;

	retval = (int)atomic_inc(&thread_index);
	__ASSERT(retval < 8 * CONFIG_MAX_THREAD_BYTES,
		 "too many threads created, increase CONFIG_MAX_THREAD_BYTES");
	return retval;
}
#endif

#define _FOREACH_STATIC_THREAD(thread_data)              \
	for (struct _static_thread_data *thread_data =   \
	     _static_thread_data_list_start;             \
	     thread_data < _static_thread_data_list_end; \
	     thread_data++)


int k_is_in_isr(void)
{
	return _is_in_isr();
}

/*
 * This function tags the current thread as essential to system operation.
 * Exceptions raised by this thread will be treated as a fatal system error.
 */
void _thread_essential_set(void)
{
	_current->base.user_options |= K_ESSENTIAL;
}

/*
 * This function tags the current thread as not essential to system operation.
 * Exceptions raised by this thread may be recoverable.
 * (This is the default tag for a thread.)
 */
void _thread_essential_clear(void)
{
	_current->base.user_options &= ~K_ESSENTIAL;
}

/*
 * This routine indicates if the current thread is an essential system thread.
 *
 * Returns non-zero if current thread is essential, zero if it is not.
 */
int _is_thread_essential(void)
{
	return _current->base.user_options & K_ESSENTIAL;
}

void k_busy_wait(u32_t usec_to_wait)
{
#if defined(CONFIG_TICKLESS_KERNEL) && \
	    !defined(CONFIG_BUSY_WAIT_USES_ALTERNATE_CLOCK)
int saved_always_on = k_enable_sys_clock_always_on();
#endif
	/* use 64-bit math to prevent overflow when multiplying */
	u32_t cycles_to_wait = (u32_t)(
		(u64_t)usec_to_wait *
		(u64_t)sys_clock_hw_cycles_per_sec /
		(u64_t)USEC_PER_SEC
	);
	u32_t start_cycles = k_cycle_get_32();

	for (;;) {
		u32_t current_cycles = k_cycle_get_32();

		/* this handles the rollover on an unsigned 32-bit value */
		if ((current_cycles - start_cycles) >= cycles_to_wait) {
			break;
		}
	}
#if defined(CONFIG_TICKLESS_KERNEL) && \
	    !defined(CONFIG_BUSY_WAIT_USES_ALTERNATE_CLOCK)
	_sys_clock_always_on = saved_always_on;
#endif
}

#ifdef CONFIG_THREAD_CUSTOM_DATA
void _impl_k_thread_custom_data_set(void *value)
{
	_current->custom_data = value;
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_custom_data_set(u32_t arg1, u32_t arg2, u32_t arg3,
					u32_t arg4, u32_t arg5, u32_t arg6,
					void *ssf)
{
	_SYSCALL_ARG1;

	_impl_k_thread_custom_data_set((void *)arg1);
	return 0;
}
#endif

void *_impl_k_thread_custom_data_get(void)
{
	return _current->custom_data;
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_custom_data_get(u32_t arg1, u32_t arg2, u32_t arg3,
					u32_t arg4, u32_t arg5, u32_t arg6,
					void *ssf)
{
	_SYSCALL_ARG0;

	return (u32_t)_impl_k_thread_custom_data_get();
}
#endif /* CONFIG_USERSPACE */
#endif /* CONFIG_THREAD_CUSTOM_DATA */

#if defined(CONFIG_THREAD_MONITOR)
/*
 * Remove a thread from the kernel's list of active threads.
 */
void _thread_monitor_exit(struct k_thread *thread)
{
	unsigned int key = irq_lock();

	if (thread == _kernel.threads) {
		_kernel.threads = _kernel.threads->next_thread;
	} else {
		struct k_thread *prev_thread;

		prev_thread = _kernel.threads;
		while (thread != prev_thread->next_thread) {
			prev_thread = prev_thread->next_thread;
		}
		prev_thread->next_thread = thread->next_thread;
	}

	irq_unlock(key);
}
#endif /* CONFIG_THREAD_MONITOR */

#ifdef CONFIG_STACK_SENTINEL
/* Check that the stack sentinel is still present
 *
 * The stack sentinel feature writes a magic value to the lowest 4 bytes of
 * the thread's stack when the thread is initialized. This value gets checked
 * in a few places:
 *
 * 1) In k_yield() if the current thread is not swapped out
 * 2) After servicing a non-nested interrupt
 * 3) In _Swap(), check the sentinel in the outgoing thread
 *
 * Item 2 requires support in arch/ code.
 *
 * If the check fails, the thread will be terminated appropriately through
 * the system fatal error handler.
 */
void _check_stack_sentinel(void)
{
	u32_t *stack;

	if (_current->base.thread_state == _THREAD_DUMMY) {
		return;
	}

	stack = (u32_t *)_current->stack_info.start;
	if (*stack != STACK_SENTINEL) {
		/* Restore it so further checks don't trigger this same error */
		*stack = STACK_SENTINEL;
		_k_except_reason(_NANO_ERR_STACK_CHK_FAIL);
	}
}
#endif

/*
 * Common thread entry point function (used by all threads)
 *
 * This routine invokes the actual thread entry point function and passes
 * it three arguments. It also handles graceful termination of the thread
 * if the entry point function ever returns.
 *
 * This routine does not return, and is marked as such so the compiler won't
 * generate preamble code that is only used by functions that actually return.
 */
FUNC_NORETURN void _thread_entry(k_thread_entry_t entry,
				 void *p1, void *p2, void *p3)
{
	entry(p1, p2, p3);

#ifdef CONFIG_MULTITHREADING
	k_thread_abort(k_current_get());
#else
	for (;;) {
		k_cpu_idle();
	}
#endif

	/*
	 * Compiler can't tell that k_thread_abort() won't return and issues a
	 * warning unless we tell it that control never gets this far.
	 */

	CODE_UNREACHABLE;
}

#ifdef CONFIG_MULTITHREADING
void _impl_k_thread_start(struct k_thread *thread)
{
	int key = irq_lock(); /* protect kernel queues */

	if (_has_thread_started(thread)) {
		irq_unlock(key);
		return;
	}

	_mark_thread_as_started(thread);

	if (_is_thread_ready(thread)) {
		_add_thread_to_ready_q(thread);
		if (_must_switch_threads()) {
			_Swap(key);
			return;
		}
	}

	irq_unlock(key);
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_start(u32_t thread, u32_t arg2, u32_t arg3,
			      u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	_SYSCALL_ARG1;

	_SYSCALL_OBJ(thread, K_OBJ_THREAD, ssf);
	_impl_k_thread_start((struct k_thread *)thread);
	return 0;
}
#endif
#endif

#ifdef CONFIG_MULTITHREADING
static void schedule_new_thread(struct k_thread *thread, s32_t delay)
{
#ifdef CONFIG_SYS_CLOCK_EXISTS
	if (delay == 0) {
		k_thread_start(thread);
	} else {
		s32_t ticks = _TICK_ALIGN + _ms_to_ticks(delay);
		int key = irq_lock();

		_add_thread_timeout(thread, NULL, ticks);
		irq_unlock(key);
	}
#else
	ARG_UNUSED(delay);
	k_thread_start(thread);
#endif
}
#endif

void _setup_new_thread(struct k_thread *new_thread,
		       k_thread_stack_t stack, size_t stack_size,
		       k_thread_entry_t entry,
		       void *p1, void *p2, void *p3,
		       int prio, u32_t options)
{
	_new_thread(new_thread, stack, stack_size, entry, p1, p2, p3,
		    prio, options);
#ifdef CONFIG_USERSPACE
	new_thread->base.perm_index = thread_index_get();
	_k_object_init(new_thread);

	/* Any given thread has access to itself */
	k_object_access_grant(new_thread, new_thread);
#endif
}

#ifdef CONFIG_MULTITHREADING
k_tid_t k_thread_create(struct k_thread *new_thread,
			k_thread_stack_t stack,
			size_t stack_size, k_thread_entry_t entry,
			void *p1, void *p2, void *p3,
			int prio, u32_t options, s32_t delay)
{
	__ASSERT(!_is_in_isr(), "Threads may not be created in ISRs");
	_setup_new_thread(new_thread, stack, stack_size, entry, p1, p2, p3,
			  prio, options);

	if (delay != K_FOREVER) {
		schedule_new_thread(new_thread, delay);
	}
	return new_thread;
}
#endif

int _impl_k_thread_cancel(k_tid_t tid)
{
	struct k_thread *thread = tid;

	int key = irq_lock();

	if (_has_thread_started(thread) ||
	    !_is_thread_timeout_active(thread)) {
		irq_unlock(key);
		return -EINVAL;
	}

	_abort_thread_timeout(thread);
	_thread_monitor_exit(thread);

	irq_unlock(key);

	return 0;
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_cancel(u32_t thread, u32_t arg2, u32_t arg3,
			       u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	_SYSCALL_ARG1;

	_SYSCALL_OBJ(thread, K_OBJ_THREAD, ssf);
	return _impl_k_thread_cancel((struct k_thread *)thread);
}
#endif

static inline int is_in_any_group(struct _static_thread_data *thread_data,
				  u32_t groups)
{
	return !!(thread_data->init_groups & groups);
}

void _k_thread_group_op(u32_t groups, void (*func)(struct k_thread *))
{
	unsigned int  key;

	__ASSERT(!_is_in_isr(), "");

	_sched_lock();

	/* Invoke func() on each static thread in the specified group set. */

	_FOREACH_STATIC_THREAD(thread_data) {
		if (is_in_any_group(thread_data, groups)) {
			key = irq_lock();
			func(thread_data->init_thread);
			irq_unlock(key);
		}
	}

	/*
	 * If the current thread is still in a ready state, then let the
	 * "unlock scheduler" code determine if any rescheduling is needed.
	 */
	if (_is_thread_ready(_current)) {
		k_sched_unlock();
		return;
	}

	/* The current thread is no longer in a ready state--reschedule. */
	key = irq_lock();
	_sched_unlock_no_reschedule();
	_Swap(key);
}

void _k_thread_single_start(struct k_thread *thread)
{
	_mark_thread_as_started(thread);

	if (_is_thread_ready(thread)) {
		_add_thread_to_ready_q(thread);
	}
}

void _k_thread_single_suspend(struct k_thread *thread)
{
	if (_is_thread_ready(thread)) {
		_remove_thread_from_ready_q(thread);
	}

	_mark_thread_as_suspended(thread);
}

void _impl_k_thread_suspend(struct k_thread *thread)
{
	unsigned int  key = irq_lock();

	_k_thread_single_suspend(thread);

	if (thread == _current) {
		_Swap(key);
	} else {
		irq_unlock(key);
	}
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_suspend(u32_t thread, u32_t arg2, u32_t arg3,
				u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	_SYSCALL_ARG1;

	_SYSCALL_OBJ(thread, K_OBJ_THREAD, ssf);
	_impl_k_thread_suspend((k_tid_t)thread);
	return 0;
}
#endif

void _k_thread_single_resume(struct k_thread *thread)
{
	_mark_thread_as_not_suspended(thread);

	if (_is_thread_ready(thread)) {
		_add_thread_to_ready_q(thread);
	}
}

void _impl_k_thread_resume(struct k_thread *thread)
{
	unsigned int  key = irq_lock();

	_k_thread_single_resume(thread);

	_reschedule_threads(key);
}

#ifdef CONFIG_USERSPACE
u32_t _handler_k_thread_resume(u32_t thread, u32_t arg2, u32_t arg3,
			       u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	_SYSCALL_ARG1;

	_SYSCALL_OBJ(thread, K_OBJ_THREAD, ssf);
	_impl_k_thread_resume((k_tid_t)thread);
	return 0;
}
#endif

void _k_thread_single_abort(struct k_thread *thread)
{
	if (thread->fn_abort != NULL) {
		thread->fn_abort();
	}

	if (_is_thread_ready(thread)) {
		_remove_thread_from_ready_q(thread);
	} else {
		if (_is_thread_pending(thread)) {
			_unpend_thread(thread);
		}
		if (_is_thread_timeout_active(thread)) {
			_abort_thread_timeout(thread);
		}
	}
	_mark_thread_as_dead(thread);
}

#ifdef CONFIG_MULTITHREADING
void _init_static_threads(void)
{
	unsigned int  key;

	_FOREACH_STATIC_THREAD(thread_data) {
		_setup_new_thread(
			thread_data->init_thread,
			thread_data->init_stack,
			thread_data->init_stack_size,
			thread_data->init_entry,
			thread_data->init_p1,
			thread_data->init_p2,
			thread_data->init_p3,
			thread_data->init_prio,
			thread_data->init_options);

		thread_data->init_thread->init_data = thread_data;
	}

	_sched_lock();

	/*
	 * Non-legacy static threads may be started immediately or after a
	 * previously specified delay. Even though the scheduler is locked,
	 * ticks can still be delivered and processed. Lock interrupts so
	 * that the countdown until execution begins from the same tick.
	 *
	 * Note that static threads defined using the legacy API have a
	 * delay of K_FOREVER.
	 */
	key = irq_lock();
	_FOREACH_STATIC_THREAD(thread_data) {
		if (thread_data->init_delay != K_FOREVER) {
			schedule_new_thread(thread_data->init_thread,
					    thread_data->init_delay);
		}
	}
	irq_unlock(key);
	k_sched_unlock();
}
#endif

void _init_thread_base(struct _thread_base *thread_base, int priority,
		       u32_t initial_state, unsigned int options)
{
	/* k_q_node is initialized upon first insertion in a list */

	thread_base->user_options = (u8_t)options;
	thread_base->thread_state = (u8_t)initial_state;

	thread_base->prio = priority;

	thread_base->sched_locked = 0;

	/* swap_data does not need to be initialized */

	_init_thread_timeout(thread_base);
}

u32_t _k_thread_group_mask_get(struct k_thread *thread)
{
	struct _static_thread_data *thread_data = thread->init_data;

	return thread_data->init_groups;
}

void _k_thread_group_join(u32_t groups, struct k_thread *thread)
{
	struct _static_thread_data *thread_data = thread->init_data;

	thread_data->init_groups |= groups;
}

void _k_thread_group_leave(u32_t groups, struct k_thread *thread)
{
	struct _static_thread_data *thread_data = thread->init_data;

	thread_data->init_groups &= groups;
}

FUNC_NORETURN void k_thread_user_mode_enter(k_thread_entry_t entry,
					    void *p1, void *p2, void *p3)
{
	_current->base.user_options |= K_USER;
	_thread_essential_clear();
#ifdef CONFIG_USERSPACE
	_arch_user_mode_enter(entry, p1, p2, p3);
#else
	/* XXX In this case we do not reset the stack */
	_thread_entry(entry, p1, p2, p3);
#endif
}
