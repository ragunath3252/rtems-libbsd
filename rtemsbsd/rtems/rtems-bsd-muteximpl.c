/**
 * @file
 *
 * @ingroup rtems_bsd_rtems
 *
 * @brief TODO.
 */

/*
 * Copyright (c) 2014, 2015 embedded brains GmbH.  All rights reserved.
 *
 *  embedded brains GmbH
 *  Dornierstr. 4
 *  82178 Puchheim
 *  Germany
 *  <rtems@embedded-brains.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <machine/rtems-bsd-kernel-space.h>
#include <machine/rtems-bsd-muteximpl.h>

#include <rtems/score/schedulerimpl.h>
#include <rtems/score/threaddispatch.h>
#include <rtems/score/threadqimpl.h>

#define INTEND_TO_BLOCK \
    (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_INTEND_TO_BLOCK)

#define BLOCKED \
    (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_BLOCKED)

#define INTERRUPT_SATISFIED \
    (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_INTERRUPT_SATISFIED)

static void
rtems_bsd_mutex_priority_change(Thread_Control *thread,
    Priority_Control new_priority, void *context)
{
	rtems_bsd_mutex *m = context;

	_RBTree_Extract(&m->rivals, &thread->RBNode);
	_RBTree_Insert(&m->rivals, &thread->RBNode,
	    _Thread_queue_Compare_priority, false);
}

void
rtems_bsd_mutex_lock_more(struct lock_object *lock, rtems_bsd_mutex *m,
    Thread_Control *owner, Thread_Control *executing,
    ISR_lock_Context *lock_context)
{
	if (owner == executing) {
		BSD_ASSERT(lock->lo_flags & LO_RECURSABLE);
		++m->nest_level;

		_ISR_lock_Release_and_ISR_enable(&m->lock, lock_context);
	} else {
		Per_CPU_Control *cpu_self;
		bool success;

		_Thread_Lock_set(executing, &m->lock);
		_Thread_Priority_set_change_handler(executing,
		    rtems_bsd_mutex_priority_change, m);
		++executing->resource_count;
		_RBTree_Insert(&m->rivals, &executing->RBNode,
		    _Thread_queue_Compare_priority, false);

		cpu_self = _Thread_Dispatch_disable_critical();

		/* Priority inheritance */
		_Scheduler_Change_priority_if_higher(_Scheduler_Get(owner),
		    owner, executing->current_priority, false);

		_Thread_Wait_flags_set(executing, INTEND_TO_BLOCK);

		_ISR_lock_Release_and_ISR_enable(&m->lock, lock_context);

		_Thread_Set_state(executing, STATES_WAITING_FOR_MUTEX);

		success = _Thread_Wait_flags_try_change(executing,
		    INTEND_TO_BLOCK, BLOCKED);
		if (!success) {
			_Thread_Clear_state(executing,
			    STATES_WAITING_FOR_MUTEX);
		}

		_Thread_Dispatch_enable(cpu_self);
	}
}

void
rtems_bsd_mutex_unlock_more(rtems_bsd_mutex *m, Thread_Control *owner,
    int keep_priority, RBTree_Node *first, ISR_lock_Context *lock_context)
{
	if (first != NULL) {
		Thread_Control *new_owner;
		bool success;

		new_owner = THREAD_RBTREE_NODE_TO_THREAD(first);
		m->owner = new_owner;
		_RBTree_Extract(&m->rivals, &new_owner->RBNode);
		_Thread_Priority_restore_default_change_handler(new_owner);
		_Thread_Lock_restore_default(new_owner);

		success = _Thread_Wait_flags_try_change_critical(new_owner,
		    INTEND_TO_BLOCK, INTERRUPT_SATISFIED);
		if (success) {
			_ISR_lock_Release_and_ISR_enable(&m->lock,
			    lock_context);
		} else {
			Per_CPU_Control *cpu_self;

			cpu_self = _Thread_Dispatch_disable_critical();
			_ISR_lock_Release_and_ISR_enable(&m->lock,
			    lock_context);

			_Thread_Clear_state(new_owner,
			    STATES_WAITING_FOR_MUTEX);

			_Thread_Dispatch_enable(cpu_self);
		}
	} else {
		_ISR_lock_Release_and_ISR_enable(&m->lock, lock_context);
	}

	if (!keep_priority) {
		Per_CPU_Control *cpu_self;

		cpu_self = _Thread_Dispatch_disable();
		_Thread_Change_priority(owner, owner->real_priority, true);
		_Thread_Dispatch_enable(cpu_self);
	}
}
