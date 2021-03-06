/*
 * ROW (Read Over Write) I/O scheduler.
 *
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* See Documentation/block/row-iosched.txt */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/blktrace_api.h>
#include <linux/hrtimer.h>

/*
 * enum row_queue_prio - Priorities of the ROW queues
 *
 * This enum defines the priorities (and the number of queues)
 * the requests will be disptributed to. The higher priority -
 * the bigger is the dispatch quantum given to that queue.
 * ROWQ_PRIO_HIGH_READ - is the higher priority queue.
 *
 */
enum row_queue_prio {
	ROWQ_PRIO_HIGH_READ = 0,
	ROWQ_PRIO_HIGH_SWRITE,
	ROWQ_PRIO_REG_READ,
	ROWQ_PRIO_REG_SWRITE,
	ROWQ_PRIO_REG_WRITE,
	ROWQ_PRIO_LOW_READ,
	ROWQ_PRIO_LOW_SWRITE,
	ROWQ_MAX_PRIO,
};

/**
 * struct row_queue_params - ROW queue parameters
 * @idling_enabled: Flag indicating whether idling is enable on
 *			the queue
 * @quantum: Number of requests to be dispatched from this queue
 *			in a dispatch cycle
 *
 */
struct row_queue_params {
	bool idling_enabled;
	int quantum;
};

/*
 * This array holds the default values of the different configurables
 * for each ROW queue. Each row of the array holds the following values:
 * {idling_enabled, quantum}
 * Each row corresponds to a queue with the same index (according to
 * enum row_queue_prio)
 */
static const struct row_queue_params row_queues_def[] = {
/* idling_enabled, quantum */
	{true, 100},	/* ROWQ_PRIO_HIGH_READ */
	{false, 5},	/* ROWQ_PRIO_HIGH_SWRITE */
	{true, 75},	/* ROWQ_PRIO_REG_READ */
	{false, 4},	/* ROWQ_PRIO_REG_SWRITE */
	{false, 4},	/* ROWQ_PRIO_REG_WRITE */
	{false, 3},	/* ROWQ_PRIO_LOW_READ */
	{false, 2}	/* ROWQ_PRIO_LOW_SWRITE */
};

/* Default values for idling on read queues (in msec) */
#define ROW_IDLE_TIME_MSEC 10
#define ROW_READ_FREQ_MSEC 25

/**
 * struct rowq_idling_data -  parameters for idling on the queue
 * @last_insert_time:	time the last request was inserted
 *			to the queue
 * @begin_idling:	flag indicating wether we should idle
 *
 */
struct rowq_idling_data {
	ktime_t			last_insert_time;
	bool			begin_idling;
};

/**
 * struct row_queue - requests grouping structure
 * @rdata:		parent row_data structure
 * @fifo:		fifo of requests
 * @prio:		queue priority (enum row_queue_prio)
 * @nr_dispatched:	number of requests already dispatched in
 *			the current dispatch cycle
 * @slice:		number of requests to dispatch in a cycle
 * @nr_req:		number of requests in queue
 * @dispatch quantum:	number of requests this queue may
 *			dispatch in a dispatch cycle
 * @idle_data:		data for idling on queues
 *
 */
struct row_queue {
	struct row_data		*rdata;
	struct list_head	fifo;
	enum row_queue_prio	prio;

	unsigned int		nr_dispatched;
	unsigned int		slice;

	unsigned int		nr_req;
	int			disp_quantum;

	/* used only for READ queues */
	struct rowq_idling_data	idle_data;
};

/**
 * struct idling_data - data for idling on empty rqueue
 * @idle_time:		idling duration (jiffies)
 * @freq:		min time between two requests that
 *			triger idling (msec)
 * @idle_work:		pointer to struct delayed_work
 *
 */
struct idling_data {
	s64				idle_time_ms;
	s64				freq_ms;

	struct hrtimer			hr_timer;
	struct work_struct		idle_work;
};

/**
 * struct row_queue - Per block device rqueue structure
 * @dispatch_queue:	dispatch rqueue
 * @row_queues:		array of priority request queues
 * @curr_queue:		index in the row_queues array of the
 *			currently serviced rqueue
 * @read_idle:		data for idling after READ request
 * @nr_reqs: nr_reqs[0] holds the number of all READ requests in
 *			scheduler, nr_reqs[1] holds the number of all WRITE
 *			requests in scheduler
 * @cycle_flags:	used for marking unserved queueus
 *
 */
struct row_data {
	struct request_queue		*dispatch_queue;

	struct row_queue row_queues[ROWQ_MAX_PRIO];

	enum row_queue_prio		curr_queue;

	struct idling_data		read_idle;
	unsigned int			nr_reqs[2];

	unsigned int			cycle_flags;
};

#define RQ_ROWQ(rq) ((struct row_queue *) ((rq)->elevator_private[0]))

#define row_log(q, fmt, args...)   \
	blk_add_trace_msg(q, "%s():" fmt , __func__, ##args)
#define row_log_rowq(rdata, rowq_id, fmt, args...)		\
	blk_add_trace_msg(rdata->dispatch_queue, "rowq%d " fmt, \
		rowq_id, ##args)

static inline void row_mark_rowq_unserved(struct row_data *rd,
					 enum row_queue_prio qnum)
{
	rd->cycle_flags |= (1 << qnum);
}

static inline void row_clear_rowq_unserved(struct row_data *rd,
					  enum row_queue_prio qnum)
{
	rd->cycle_flags &= ~(1 << qnum);
}

static inline int row_rowq_unserved(struct row_data *rd,
				   enum row_queue_prio qnum)
{
	return rd->cycle_flags & (1 << qnum);
}

static inline void __maybe_unused row_dump_queues_stat(struct row_data *rd)
{
	int i;

	row_log(rd->dispatch_queue, " Queues status:");
	for (i = 0; i < ROWQ_MAX_PRIO; i++)
		row_log(rd->dispatch_queue,
			"queue%d: dispatched= %d, nr_req=%d", i,
			rd->row_queues[i].nr_dispatched,
			rd->row_queues[i].nr_req);
}

/******************** Static helper functions ***********************/
static void kick_queue(struct work_struct *work)
{
	struct idling_data *read_data =
		container_of(work, struct idling_data, idle_work);
	struct row_data *rd =
		container_of(read_data, struct row_data, read_idle);

	blk_run_queue(rd->dispatch_queue);
}


static enum hrtimer_restart row_idle_hrtimer_fn(struct hrtimer *hr_timer)
{
	struct idling_data *read_data =
		container_of(hr_timer, struct idling_data, hr_timer);
	struct row_data *rd =
		container_of(read_data, struct row_data, read_idle);

	row_log_rowq(rd, rd->curr_queue, "Performing delayed work");
	/* Mark idling process as done */
	rd->row_queues[rd->curr_queue].idle_data.begin_idling = false;

	if (!(rd->nr_reqs[0] + rd->nr_reqs[1]))
		row_log(rd->dispatch_queue, "No requests in scheduler");
	else
		kblockd_schedule_work(rd->dispatch_queue,
			&read_data->idle_work);
	return HRTIMER_NORESTART;
}

/*
 * row_restart_disp_cycle() - Restart the dispatch cycle
 * @rd:	pointer to struct row_data
 *
 * This function restarts the dispatch cycle by:
 * - Setting current queue to ROWQ_PRIO_HIGH_READ
 * - For each queue: reset the number of requests dispatched in
 *   the cycle
 */
static inline void row_restart_disp_cycle(struct row_data *rd)
{
	int i;

	for (i = 0; i < ROWQ_MAX_PRIO; i++)
		rd->row_queues[i].nr_dispatched = 0;

	rd->curr_queue = ROWQ_PRIO_HIGH_READ;
	row_log(rd->dispatch_queue, "Restarting cycle");
}

static inline void row_get_next_queue(struct row_data *rd)
{
	rd->curr_queue++;
	if (rd->curr_queue == ROWQ_MAX_PRIO)
		row_restart_disp_cycle(rd);
}

/******************* Elevator callback functions *********************/

/*
 * row_add_request() - Add request to the scheduler
 * @q:	requests queue
 * @rq:	request to add
 *
 */
static void row_add_request(struct request_queue *q,
			    struct request *rq)
{
	struct row_data *rd = (struct row_data *)q->elevator->elevator_data;
	struct row_queue *rqueue = RQ_ROWQ(rq);
	s64 diff_ms;

	list_add_tail(&rq->queuelist, &rqueue->fifo);
	rd->nr_reqs[rq_data_dir(rq)]++;
	rqueue->nr_req++;
	rq_set_fifo_time(rq, jiffies); /* for statistics*/

	if (row_queues_def[rqueue->prio].idling_enabled) {
		if (hrtimer_active(&rd->read_idle.hr_timer))
			hrtimer_try_to_cancel(&rd->read_idle.hr_timer);
		diff_ms = ktime_to_ms(ktime_sub(ktime_get(),
				rqueue->idle_data.last_insert_time));
		if (unlikely(diff_ms < 0)) {
			pr_err("ROW BUG: %s diff_ms < 0", __func__);
			rqueue->idle_data.begin_idling = false;
			return;
		}
		if (diff_ms < rd->read_idle.freq_ms) {
			rqueue->idle_data.begin_idling = true;
			row_log_rowq(rd, rqueue->prio, "Enable idling");
		} else {
			rqueue->idle_data.begin_idling = false;
			row_log_rowq(rd, rqueue->prio, "Disable idling (%ldms)",
				(long)diff_ms);
		}

		rqueue->idle_data.last_insert_time = ktime_get();
	}
	row_log_rowq(rd, rqueue->prio,
			"added request (total on queue=%d)", rqueue->nr_req);
}

/*
 * row_remove_request() -  Remove given request from scheduler
 * @q:	requests queue
 * @rq:	request to remove
 *
 */
static void row_remove_request(struct request_queue *q,
			       struct request *rq)
{
	struct row_data *rd = (struct row_data *)q->elevator->elevator_data;
	struct row_queue *rqueue = RQ_ROWQ(rq);

	rq_fifo_clear(rq);
	rqueue->nr_req--;
	rd->nr_reqs[rq_data_dir(rq)]--;
}

/*
 * row_dispatch_insert() - move request to dispatch queue
 * @rd:	pointer to struct row_data
 *
 * This function moves the next request to dispatch from
 * rd->curr_queue to the dispatch queue
 *
 */
static void row_dispatch_insert(struct row_data *rd)
{
	struct request *rq;

	rq = rq_entry_fifo(rd->row_queues[rd->curr_queue].fifo.next);
	row_remove_request(rd->dispatch_queue, rq);
	elv_dispatch_add_tail(rd->dispatch_queue, rq);
	rd->row_queues[rd->curr_queue].nr_dispatched++;
	row_clear_rowq_unserved(rd, rd->curr_queue);
	row_log_rowq(rd, rd->curr_queue, " Dispatched request nr_disp = %d",
		     rd->row_queues[rd->curr_queue].nr_dispatched);
}

/*
 * row_choose_queue() -  choose the next queue to dispatch from
 * @rd:	pointer to struct row_data
 *
 * Updates rd->curr_queue. Returns 1 if there are requests to
 * dispatch, 0 if there are no requests in scheduler
 *
 */
static int row_choose_queue(struct row_data *rd)
{
	int prev_curr_queue = rd->curr_queue;

	if (!(rd->nr_reqs[0] + rd->nr_reqs[1])) {
		row_log(rd->dispatch_queue, "No more requests in scheduler");
		return 0;
	}

	row_get_next_queue(rd);

	/*
	 * Loop over all queues to find the next queue that is not empty.
	 * Stop when you get back to curr_queue
	 */
	while (list_empty(&rd->row_queues[rd->curr_queue].fifo)
	       && rd->curr_queue != prev_curr_queue) {
		/* Mark rqueue as unserved */
		row_mark_rowq_unserved(rd, rd->curr_queue);
		row_get_next_queue(rd);
	}

	return 1;
}

/*
 * row_dispatch_requests() - selects the next request to dispatch
 * @q:		requests queue
 * @force:	ignored
 *
 * Return 0 if no requests were moved to the dispatch queue.
 *	  1 otherwise
 *
 */
static int row_dispatch_requests(struct request_queue *q, int force)
{
	struct row_data *rd = (struct row_data *)q->elevator->elevator_data;
	int ret = 0, currq, i;

	currq = rd->curr_queue;

	/*
	 * Find the first unserved queue (with higher priority then currq)
	 * that is not empty
	 */
	for (i = 0; i < currq; i++) {
		if (row_rowq_unserved(rd, i) &&
		    !list_empty(&rd->row_queues[i].fifo)) {
			row_log_rowq(rd, currq,
				" Preemting for unserved rowq%d. (nr_req=%u)",
				i, rd->row_queues[currq].nr_req);
			rd->curr_queue = i;
			row_dispatch_insert(rd);
			ret = 1;
			goto done;
		}
	}

	if (rd->row_queues[currq].nr_dispatched >=
	    rd->row_queues[currq].disp_quantum) {
		rd->row_queues[currq].nr_dispatched = 0;
		row_log_rowq(rd, currq, "Expiring rqueue");
		ret = row_choose_queue(rd);
		if (ret)
			row_dispatch_insert(rd);
		goto done;
	}

	/* Dispatch from curr_queue */
	if (list_empty(&rd->row_queues[currq].fifo)) {
		/* check idling */
		if (hrtimer_active(&rd->read_idle.hr_timer)) {
			if (force) {
				if(hrtimer_try_to_cancel(
					&rd->read_idle.hr_timer) >= 0) {
					row_log_rowq(rd, currq,
					"Canceled delayed work - forced dispatch");
				}
			} else {
				row_log_rowq(rd, currq,
						 "Delayed work pending. Exiting");
				goto done;
			}
		}

		if (!force && row_queues_def[currq].idling_enabled &&
		    rd->row_queues[currq].idle_data.begin_idling) {
			hrtimer_start(&rd->read_idle.hr_timer,
				ktime_set(0,
				   rd->read_idle.idle_time_ms * NSEC_PER_MSEC),
				   HRTIMER_MODE_REL);
			row_log_rowq(rd, currq,
				     "Scheduled delayed work. exiting");
			goto done;
		} else {
			row_log_rowq(rd, currq,
				     "Currq empty. Choose next queue");
			ret = row_choose_queue(rd);
			if (!ret)
				goto done;
		}
	}

	ret = 1;
	row_dispatch_insert(rd);

done:
	return ret;
}

/*
 * row_init_queue() - Init scheduler data structures
 * @q:	requests queue
 *
 * Return pointer to struct row_data to be saved in elevator for
 * this dispatch queue
 *
 */
static void *row_init_queue(struct request_queue *q)
{

	struct row_data *rdata;
	int i;

	rdata = kmalloc_node(sizeof(*rdata),
			     GFP_KERNEL | __GFP_ZERO, q->node);
	if (!rdata)
		return NULL;

	memset(rdata, 0, sizeof(*rdata));
	for (i = 0; i < ROWQ_MAX_PRIO; i++) {
		INIT_LIST_HEAD(&rdata->row_queues[i].fifo);
		rdata->row_queues[i].disp_quantum = row_queues_def[i].quantum;
		rdata->row_queues[i].rdata = rdata;
		rdata->row_queues[i].prio = i;
		rdata->row_queues[i].idle_data.begin_idling = false;
		rdata->row_queues[i].idle_data.last_insert_time =
			ktime_set(0, 0);
	}

	/*
	 * Currently idling is enabled only for READ queues. If we want to
	 * enable it for write queues also, note that idling frequency will
	 * be the same in both cases
	 */
	rdata->read_idle.idle_time_ms = ROW_IDLE_TIME_MSEC;
	rdata->read_idle.freq_ms = ROW_READ_FREQ_MSEC;
	hrtimer_init(&rdata->read_idle.hr_timer,
		CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rdata->read_idle.hr_timer.function = &row_idle_hrtimer_fn;

	INIT_WORK(&rdata->read_idle.idle_work, kick_queue);

	rdata->curr_queue = ROWQ_PRIO_HIGH_READ;
	rdata->dispatch_queue = q;

	return rdata;
}

/*
 * row_exit_queue() - called on unloading the RAW scheduler
 * @e:	poiner to struct elevator_queue
 *
 */
static void row_exit_queue(struct elevator_queue *e)
{
	struct row_data *rd = (struct row_data *)e->elevator_data;
	int i;

	for (i = 0; i < ROWQ_MAX_PRIO; i++)
		BUG_ON(!list_empty(&rd->row_queues[i].fifo));
	if (hrtimer_cancel(&rd->read_idle.hr_timer))
		pr_err("ROW BUG: idle timer was active!");
	kfree(rd);
}

/*
 * row_merged_requests() - Called when 2 requests are merged
 * @q:		requests queue
 * @rq:		request the two requests were merged into
 * @next:	request that was merged
 */
static void row_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	struct row_queue   *rqueue = RQ_ROWQ(next);

	list_del_init(&next->queuelist);
	rqueue->nr_req--;

	rqueue->rdata->nr_reqs[rq_data_dir(rq)]--;
}

/*
 * get_queue_type() - Get queue type for a given request
 *
 * This is a helping function which purpose is to determine what
 * ROW queue the given request should be added to (and
 * dispatched from leter on)
 *
 * TODO: Right now only 3 queues are used REG_READ, REG_WRITE
 * and REG_SWRITE
 */
static enum row_queue_prio get_queue_type(struct request *rq)
{
	const int data_dir = rq_data_dir(rq);
	const bool is_sync = rq_is_sync(rq);

	if (data_dir == READ)
		return ROWQ_PRIO_REG_READ;
	else if (is_sync)
		return ROWQ_PRIO_REG_SWRITE;
	else
		return ROWQ_PRIO_REG_WRITE;
}

/*
 * row_set_request() - Set ROW data structures associated with this request.
 * @q:		requests queue
 * @rq:		pointer to the request
 * @gfp_mask:	ignored
 *
 */
static int
row_set_request(struct request_queue *q, struct request *rq, gfp_t gfp_mask)
{
	struct row_data *rd = (struct row_data *)q->elevator->elevator_data;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	rq->elevator_private[0] =
		(void *)(&rd->row_queues[get_queue_type(rq)]);
	spin_unlock_irqrestore(q->queue_lock, flags);

	return 0;
}

/********** Helping sysfs functions/defenitions for ROW attributes ******/
static ssize_t row_var_show(int var, char *page)
{
	return snprintf(page, 100, "%d\n", var);
}

static ssize_t row_var_store(int *var, const char *page, size_t count)
{
	int err;
	err = kstrtoul(page, 10, (unsigned long *)var);

	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR)					\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct row_data *rowd = e->elevator_data;			\
	int __data = __VAR;						\
	return row_var_show(__data, (page));				\
}
SHOW_FUNCTION(row_hp_read_quantum_show,
	rowd->row_queues[ROWQ_PRIO_HIGH_READ].disp_quantum);
SHOW_FUNCTION(row_rp_read_quantum_show,
	rowd->row_queues[ROWQ_PRIO_REG_READ].disp_quantum);
SHOW_FUNCTION(row_hp_swrite_quantum_show,
	rowd->row_queues[ROWQ_PRIO_HIGH_SWRITE].disp_quantum);
SHOW_FUNCTION(row_rp_swrite_quantum_show,
	rowd->row_queues[ROWQ_PRIO_REG_SWRITE].disp_quantum);
SHOW_FUNCTION(row_rp_write_quantum_show,
	rowd->row_queues[ROWQ_PRIO_REG_WRITE].disp_quantum);
SHOW_FUNCTION(row_lp_read_quantum_show,
	rowd->row_queues[ROWQ_PRIO_LOW_READ].disp_quantum);
SHOW_FUNCTION(row_lp_swrite_quantum_show,
	rowd->row_queues[ROWQ_PRIO_LOW_SWRITE].disp_quantum);
SHOW_FUNCTION(row_read_idle_show, rowd->read_idle.idle_time_ms);
SHOW_FUNCTION(row_read_idle_freq_show, rowd->read_idle.freq_ms);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)				\
static ssize_t __FUNC(struct elevator_queue *e,				\
		const char *page, size_t count)				\
{									\
	struct row_data *rowd = e->elevator_data;			\
	int __data;							\
	int ret = row_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = __data;						\
	return ret;							\
}
STORE_FUNCTION(row_hp_read_quantum_store,
&rowd->row_queues[ROWQ_PRIO_HIGH_READ].disp_quantum, 1, INT_MAX);
STORE_FUNCTION(row_rp_read_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_REG_READ].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_hp_swrite_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_HIGH_SWRITE].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_rp_swrite_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_REG_SWRITE].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_rp_write_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_REG_WRITE].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_lp_read_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_LOW_READ].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_lp_swrite_quantum_store,
			&rowd->row_queues[ROWQ_PRIO_LOW_SWRITE].disp_quantum,
			1, INT_MAX);
STORE_FUNCTION(row_read_idle_store, &rowd->read_idle.idle_time_ms, 1, INT_MAX);
STORE_FUNCTION(row_read_idle_freq_store, &rowd->read_idle.freq_ms, 1, INT_MAX);

#undef STORE_FUNCTION

#define ROW_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, row_##name##_show, \
				      row_##name##_store)

static struct elv_fs_entry row_attrs[] = {
	ROW_ATTR(hp_read_quantum),
	ROW_ATTR(rp_read_quantum),
	ROW_ATTR(hp_swrite_quantum),
	ROW_ATTR(rp_swrite_quantum),
	ROW_ATTR(rp_write_quantum),
	ROW_ATTR(lp_read_quantum),
	ROW_ATTR(lp_swrite_quantum),
	ROW_ATTR(read_idle),
	ROW_ATTR(read_idle_freq),
	__ATTR_NULL
};

static struct elevator_type iosched_row = {
	.ops = {
		.elevator_merge_req_fn		= row_merged_requests,
		.elevator_dispatch_fn		= row_dispatch_requests,
		.elevator_add_req_fn		= row_add_request,
		.elevator_former_req_fn		= elv_rb_former_request,
		.elevator_latter_req_fn		= elv_rb_latter_request,
		.elevator_set_req_fn		= row_set_request,
		.elevator_init_fn		= row_init_queue,
		.elevator_exit_fn		= row_exit_queue,
	},

	.elevator_attrs = row_attrs,
	.elevator_name = "row",
	.elevator_owner = THIS_MODULE,
};

static int __init row_init(void)
{
	elv_register(&iosched_row);
	return 0;
}

static void __exit row_exit(void)
{
	elv_unregister(&iosched_row);
}

module_init(row_init);
module_exit(row_exit);

MODULE_LICENSE("GPLv2");
MODULE_DESCRIPTION("Read Over Write IO scheduler");
