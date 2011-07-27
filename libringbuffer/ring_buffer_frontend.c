/*
 * ring_buffer_frontend.c
 *
 * (C) Copyright 2005-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer wait-free buffer synchronization. Producer-consumer and flight
 * recorder (overwrite) modes. See thesis:
 *
 * Desnoyers, Mathieu (2009), "Low-Impact Operating System Tracing", Ph.D.
 * dissertation, Ecole Polytechnique de Montreal.
 * http://www.lttng.org/pub/thesis/desnoyers-dissertation-2009-12.pdf
 *
 * - Algorithm presentation in Chapter 5:
 *     "Lockless Multi-Core High-Throughput Buffering".
 * - Algorithm formal verification in Section 8.6:
 *     "Formal verification of LTTng"
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Inspired from LTT and RelayFS:
 *  Karim Yaghmour <karim@opersys.com>
 *  Tom Zanussi <zanussi@us.ibm.com>
 *  Bob Wisniewski <bob@watson.ibm.com>
 * And from K42 :
 *  Bob Wisniewski <bob@watson.ibm.com>
 *
 * Buffer reader semantic :
 *
 * - get_subbuf_size
 * while buffer is not finalized and empty
 *   - get_subbuf
 *     - if return value != 0, continue
 *   - splice one subbuffer worth of data to a pipe
 *   - splice the data from pipe to disk/network
 *   - put_subbuf
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <sys/types.h>
#include <sys/shm.h>
#include <urcu/compiler.h>
#include <urcu/ref.h>

#include "smp.h"
#include "config.h"
#include "backend.h"
#include "frontend.h"
#include "shm.h"

/*
 * Internal structure representing offsets to use at a sub-buffer switch.
 */
struct switch_offsets {
	unsigned long begin, end, old;
	size_t pre_header_padding, size;
	unsigned int switch_new_start:1, switch_new_end:1, switch_old_start:1,
		     switch_old_end:1;
};

__thread unsigned int lib_ring_buffer_nesting;

static
void lib_ring_buffer_print_errors(struct channel *chan,
				  struct lib_ring_buffer *buf, int cpu);

/*
 * Must be called under cpu hotplug protection.
 */
void lib_ring_buffer_free(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);

	lib_ring_buffer_print_errors(chan, buf, buf->backend.cpu);
	free(shmp(buf->commit_hot));
	free(shmp(buf->commit_cold));

	lib_ring_buffer_backend_free(&buf->backend);
}

/**
 * lib_ring_buffer_reset - Reset ring buffer to initial values.
 * @buf: Ring buffer.
 *
 * Effectively empty the ring buffer. Should be called when the buffer is not
 * used for writing. The ring buffer can be opened for reading, but the reader
 * should not be using the iterator concurrently with reset. The previous
 * current iterator record is reset.
 */
void lib_ring_buffer_reset(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned int i;

	/*
	 * Reset iterator first. It will put the subbuffer if it currently holds
	 * it.
	 */
	v_set(config, &buf->offset, 0);
	for (i = 0; i < chan->backend.num_subbuf; i++) {
		v_set(config, &shmp(buf->commit_hot)[i].cc, 0);
		v_set(config, &shmp(buf->commit_hot)[i].seq, 0);
		v_set(config, &shmp(buf->commit_cold)[i].cc_sb, 0);
	}
	uatomic_set(&buf->consumed, 0);
	uatomic_set(&buf->record_disabled, 0);
	v_set(config, &buf->last_tsc, 0);
	lib_ring_buffer_backend_reset(&buf->backend);
	/* Don't reset number of active readers */
	v_set(config, &buf->records_lost_full, 0);
	v_set(config, &buf->records_lost_wrap, 0);
	v_set(config, &buf->records_lost_big, 0);
	v_set(config, &buf->records_count, 0);
	v_set(config, &buf->records_overrun, 0);
	buf->finalized = 0;
}

/**
 * channel_reset - Reset channel to initial values.
 * @chan: Channel.
 *
 * Effectively empty the channel. Should be called when the channel is not used
 * for writing. The channel can be opened for reading, but the reader should not
 * be using the iterator concurrently with reset. The previous current iterator
 * record is reset.
 */
void channel_reset(struct channel *chan)
{
	/*
	 * Reset iterators first. Will put the subbuffer if held for reading.
	 */
	uatomic_set(&chan->record_disabled, 0);
	/* Don't reset commit_count_mask, still valid */
	channel_backend_reset(&chan->backend);
	/* Don't reset switch/read timer interval */
	/* Don't reset notifiers and notifier enable bits */
	/* Don't reset reader reference count */
}

/*
 * Must be called under cpu hotplug protection.
 */
int lib_ring_buffer_create(struct lib_ring_buffer *buf,
			   struct channel_backend *chanb, int cpu,
			   struct shm_header *shm_header)
{
	const struct lib_ring_buffer_config *config = chanb->config;
	struct channel *chan = caa_container_of(chanb, struct channel, backend);
	void *priv = chanb->priv;
	unsigned int num_subbuf;
	size_t subbuf_header_size;
	u64 tsc;
	int ret;

	/* Test for cpu hotplug */
	if (buf->backend.allocated)
		return 0;

	ret = lib_ring_buffer_backend_create(&buf->backend, &chan->backend,
			cpu, shm_header);
	if (ret)
		return ret;

	set_shmp(&buf->commit_hot,
		 zalloc_shm(shm_header,
			sizeof(*buf->commit_hot) * chan->backend.num_subbuf));
	if (!shmp(buf->commit_hot)) {
		ret = -ENOMEM;
		goto free_chanbuf;
	}

	set_shmp(&buf->commit_cold,
		 zalloc_shm(shm_header,
			sizeof(*buf->commit_cold) * chan->backend.num_subbuf));
	if (!shmp(buf->commit_cold)) {
		ret = -ENOMEM;
		goto free_commit;
	}

	num_subbuf = chan->backend.num_subbuf;
	//init_waitqueue_head(&buf->read_wait);

	/*
	 * Write the subbuffer header for first subbuffer so we know the total
	 * duration of data gathering.
	 */
	subbuf_header_size = config->cb.subbuffer_header_size();
	v_set(config, &buf->offset, subbuf_header_size);
	subbuffer_id_clear_noref(config, &shmp(buf->backend.buf_wsb)[0].id);
	tsc = config->cb.ring_buffer_clock_read(shmp(buf->backend.chan));
	config->cb.buffer_begin(buf, tsc, 0);
	v_add(config, subbuf_header_size, &shmp(buf->commit_hot)[0].cc);

	if (config->cb.buffer_create) {
		ret = config->cb.buffer_create(buf, priv, cpu, chanb->name);
		if (ret)
			goto free_init;
	}
	buf->backend.allocated = 1;
	return 0;

	/* Error handling */
free_init:
	/* commit_cold will be freed by shm teardown */
free_commit:
	/* commit_hot will be freed by shm teardown */
free_chanbuf:
	lib_ring_buffer_backend_free(&buf->backend);
	return ret;
}

static void switch_buffer_timer(unsigned long data)
{
	struct lib_ring_buffer *buf = (struct lib_ring_buffer *)data;
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;

	/*
	 * Only flush buffers periodically if readers are active.
	 */
	if (uatomic_read(&buf->active_readers))
		lib_ring_buffer_switch_slow(buf, SWITCH_ACTIVE);

	//TODO timers
	//if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
	//	mod_timer_pinned(&buf->switch_timer,
	//			 jiffies + chan->switch_timer_interval);
	//else
	//	mod_timer(&buf->switch_timer,
	//		  jiffies + chan->switch_timer_interval);
}

static void lib_ring_buffer_start_switch_timer(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;

	if (!chan->switch_timer_interval || buf->switch_timer_enabled)
		return;
	//TODO
	//init_timer(&buf->switch_timer);
	//buf->switch_timer.function = switch_buffer_timer;
	//buf->switch_timer.expires = jiffies + chan->switch_timer_interval;
	//buf->switch_timer.data = (unsigned long)buf;
	//if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
	//	add_timer_on(&buf->switch_timer, buf->backend.cpu);
	//else
	//	add_timer(&buf->switch_timer);
	buf->switch_timer_enabled = 1;
}

static void lib_ring_buffer_stop_switch_timer(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);

	if (!chan->switch_timer_interval || !buf->switch_timer_enabled)
		return;

	//TODO
	//del_timer_sync(&buf->switch_timer);
	buf->switch_timer_enabled = 0;
}

/*
 * Polling timer to check the channels for data.
 */
static void read_buffer_timer(unsigned long data)
{
	struct lib_ring_buffer *buf = (struct lib_ring_buffer *)data;
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;

	CHAN_WARN_ON(chan, !buf->backend.allocated);

	if (uatomic_read(&buf->active_readers)
	    && lib_ring_buffer_poll_deliver(config, buf, chan)) {
		//TODO
		//wake_up_interruptible(&buf->read_wait);
		//wake_up_interruptible(&chan->read_wait);
	}

	//TODO
	//if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
	//	mod_timer_pinned(&buf->read_timer,
	//			 jiffies + chan->read_timer_interval);
	//else
	//	mod_timer(&buf->read_timer,
	//		  jiffies + chan->read_timer_interval);
}

static void lib_ring_buffer_start_read_timer(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;

	if (config->wakeup != RING_BUFFER_WAKEUP_BY_TIMER
	    || !chan->read_timer_interval
	    || buf->read_timer_enabled)
		return;

	//TODO
	//init_timer(&buf->read_timer);
	//buf->read_timer.function = read_buffer_timer;
	//buf->read_timer.expires = jiffies + chan->read_timer_interval;
	//buf->read_timer.data = (unsigned long)buf;

	//if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
	//	add_timer_on(&buf->read_timer, buf->backend.cpu);
	//else
	//	add_timer(&buf->read_timer);
	buf->read_timer_enabled = 1;
}

static void lib_ring_buffer_stop_read_timer(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;

	if (config->wakeup != RING_BUFFER_WAKEUP_BY_TIMER
	    || !chan->read_timer_interval
	    || !buf->read_timer_enabled)
		return;

	//TODO
	//del_timer_sync(&buf->read_timer);
	/*
	 * do one more check to catch data that has been written in the last
	 * timer period.
	 */
	if (lib_ring_buffer_poll_deliver(config, buf, chan)) {
		//TODO
		//wake_up_interruptible(&buf->read_wait);
		//wake_up_interruptible(&chan->read_wait);
	}
	buf->read_timer_enabled = 0;
}

static void channel_unregister_notifiers(struct channel *chan)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	int cpu;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		for_each_possible_cpu(cpu) {
			struct lib_ring_buffer *buf = &shmp(chan->backend.buf)[cpu];

			lib_ring_buffer_stop_switch_timer(buf);
			lib_ring_buffer_stop_read_timer(buf);
		}
	} else {
		struct lib_ring_buffer *buf = shmp(chan->backend.buf);

		lib_ring_buffer_stop_switch_timer(buf);
		lib_ring_buffer_stop_read_timer(buf);
	}
	channel_backend_unregister_notifiers(&chan->backend);
}

static void channel_free(struct channel *chan)
{
	channel_backend_free(&chan->backend);
	free(chan);
}

/**
 * channel_create - Create channel.
 * @config: ring buffer instance configuration
 * @name: name of the channel
 * @priv: ring buffer client private data
 * @buf_addr: pointer the the beginning of the preallocated buffer contiguous
 *            address mapping. It is used only by RING_BUFFER_STATIC
 *            configuration. It can be set to NULL for other backends.
 * @subbuf_size: subbuffer size
 * @num_subbuf: number of subbuffers
 * @switch_timer_interval: Time interval (in us) to fill sub-buffers with
 *                         padding to let readers get those sub-buffers.
 *                         Used for live streaming.
 * @read_timer_interval: Time interval (in us) to wake up pending readers.
 * @shmid: shared memory ID (output)
 *
 * Holds cpu hotplug.
 * Returns NULL on failure.
 */
struct channel *channel_create(const struct lib_ring_buffer_config *config,
		   const char *name, void *priv, void *buf_addr,
		   size_t subbuf_size,
		   size_t num_subbuf, unsigned int switch_timer_interval,
		   unsigned int read_timer_interval,
		   int *shmid)
{
	int ret, cpu;
	struct channel *chan;
	size_t shmsize, bufshmsize;
	struct shm_header *shm_header;
	unsigned long num_subbuf_alloc;

	if (lib_ring_buffer_check_config(config, switch_timer_interval,
					 read_timer_interval))
		return NULL;

	/* Calculate the shm allocation layout */
	shmsize = sizeof(struct shm_header);
	shmsize += sizeof(struct channel);

	/* Per-cpu buffer size: control (prior to backend) */
	bufshmsize = sizeof(struct lib_ring_buffer);
	shmsize += bufshmsize * num_possible_cpus();

	/* Per-cpu buffer size: backend */
	/* num_subbuf + 1 is the worse case */
	num_subbuf_alloc = num_subbuf + 1;
	bufshmsize = sizeof(struct lib_ring_buffer_backend_pages *) * num_subbuf_alloc;
	bufshmsize += subbuf_size * (num_subbuf_alloc);
	bufshmsize += (sizeof(struct lib_ring_buffer_backend_pages) + subbuf_size) * num_subbuf_alloc;
	bufshmsize += sizeof(struct lib_ring_buffer_backend_subbuffer) * num_subbuf;
	shmsize += bufshmsize * num_possible_cpus();

	/* Per-cpu buffer size: control (after backend) */
	bufshmsize += sizeof(struct commit_counters_hot) * num_subbuf;
	bufshmsize += sizeof(struct commit_counters_cold) * num_subbuf;

	/* Allocate shm */
	*shmid = shmget(getpid(), shmsize, IPC_CREAT | IPC_EXCL | 0700);
	if (*shmid < 0) {
		if (errno == EINVAL)
			ERR("shmget() returned EINVAL; maybe /proc/sys/kernel/shmmax should be increased.");
		else
			PERROR("shmget");
		return NULL;
	}

	shm_header = shmat(*shmid, NULL, 0);
	if (shm_header == (void *) -1) {
		perror("shmat");
		goto destroy_shmem;
	}

	/* Already mark the shared memory for destruction. This will occur only
         * when all users have detached.
	 */
	ret = shmctl(*shmid, IPC_RMID, NULL);
	if (ret == -1) {
		perror("shmctl");
		goto destroy_shmem;
	}

	shm_header->magic = SHM_MAGIC;
	shm_header->major = SHM_MAJOR;
	shm_header->major = SHM_MINOR;
	shm_header->bits_per_long = CAA_BITS_PER_LONG;
	shm_header->shm_size = shmsize;
	shm_header->shm_allocated = sizeof(struct shm_header);

	chan = zalloc_shm(shm_header, sizeof(struct channel));
	if (!chan)
		goto destroy_shmem;
	set_shmp(shm_header->chan, chan);

	ret = channel_backend_init(&chan->backend, name, config, priv,
				   subbuf_size, num_subbuf, shm_header);
	if (ret)
		goto destroy_shmem;

	chan->commit_count_mask = (~0UL >> chan->backend.num_subbuf_order);
	//TODO
	//chan->switch_timer_interval = usecs_to_jiffies(switch_timer_interval);
	//chan->read_timer_interval = usecs_to_jiffies(read_timer_interval);
	urcu_ref_init(&chan->ref);
	//TODO
	//init_waitqueue_head(&chan->read_wait);
	//init_waitqueue_head(&chan->hp_wait);

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		/*
		 * In case of non-hotplug cpu, if the ring-buffer is allocated
		 * in early initcall, it will not be notified of secondary cpus.
		 * In that off case, we need to allocate for all possible cpus.
		 */
		for_each_possible_cpu(cpu) {
			struct lib_ring_buffer *buf = &shmp(chan->backend.buf)[cpu];
			lib_ring_buffer_start_switch_timer(buf);
			lib_ring_buffer_start_read_timer(buf);
		}
	} else {
		struct lib_ring_buffer *buf = shmp(chan->backend.buf);

		lib_ring_buffer_start_switch_timer(buf);
		lib_ring_buffer_start_read_timer(buf);
	}

	return chan;

destroy_shmem:
	ret = shmctl(*shmid, IPC_RMID, NULL);
	if (ret == -1) {
		perror("shmctl");
	}
	return NULL;
}

static
void channel_release(struct urcu_ref *ref)
{
	struct channel *chan = caa_container_of(ref, struct channel, ref);
	channel_free(chan);
}

/**
 * channel_destroy - Finalize, wait for q.s. and destroy channel.
 * @chan: channel to destroy
 *
 * Holds cpu hotplug.
 * Call "destroy" callback, finalize channels, wait for readers to release their
 * reference, then destroy ring buffer data. Note that when readers have
 * completed data consumption of finalized channels, get_subbuf() will return
 * -ENODATA. They should release their handle at that point.
 * Returns the private data pointer.
 */
void *channel_destroy(struct channel *chan)
{
	int cpu;
	const struct lib_ring_buffer_config *config = chan->backend.config;
	void *priv;

	channel_unregister_notifiers(chan);

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		for_each_channel_cpu(cpu, chan) {
			struct lib_ring_buffer *buf = &shmp(chan->backend.buf)[cpu];

			if (config->cb.buffer_finalize)
				config->cb.buffer_finalize(buf,
							   chan->backend.priv,
							   cpu);
			if (buf->backend.allocated)
				lib_ring_buffer_switch_slow(buf, SWITCH_FLUSH);
			/*
			 * Perform flush before writing to finalized.
			 */
			cmm_smp_wmb();
			CMM_ACCESS_ONCE(buf->finalized) = 1;
			//wake_up_interruptible(&buf->read_wait);
		}
	} else {
		struct lib_ring_buffer *buf = shmp(chan->backend.buf);

		if (config->cb.buffer_finalize)
			config->cb.buffer_finalize(buf, chan->backend.priv, -1);
		if (buf->backend.allocated)
			lib_ring_buffer_switch_slow(buf, SWITCH_FLUSH);
		/*
		 * Perform flush before writing to finalized.
		 */
		cmm_smp_wmb();
		CMM_ACCESS_ONCE(buf->finalized) = 1;
		//wake_up_interruptible(&buf->read_wait);
	}
	CMM_ACCESS_ONCE(chan->finalized) = 1;
	//wake_up_interruptible(&chan->hp_wait);
	//wake_up_interruptible(&chan->read_wait);
	urcu_ref_put(&chan->ref, channel_release);
	priv = chan->backend.priv;
	return priv;
}

struct lib_ring_buffer *channel_get_ring_buffer(
					const struct lib_ring_buffer_config *config,
					struct channel *chan, int cpu)
{
	if (config->alloc == RING_BUFFER_ALLOC_GLOBAL)
		return shmp(chan->backend.buf);
	else
		return &shmp(chan->backend.buf)[cpu];
}

int lib_ring_buffer_open_read(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);

	if (uatomic_cmpxchg(&buf->active_readers, 0, 1) != 0)
		return -EBUSY;
	urcu_ref_get(&chan->ref);
	cmm_smp_mb();
	return 0;
}

void lib_ring_buffer_release_read(struct lib_ring_buffer *buf)
{
	struct channel *chan = shmp(buf->backend.chan);

	CHAN_WARN_ON(chan, uatomic_read(&buf->active_readers) != 1);
	cmm_smp_mb();
	uatomic_dec(&buf->active_readers);
	urcu_ref_put(&chan->ref, channel_release);
}

/**
 * lib_ring_buffer_snapshot - save subbuffer position snapshot (for read)
 * @buf: ring buffer
 * @consumed: consumed count indicating the position where to read
 * @produced: produced count, indicates position when to stop reading
 *
 * Returns -ENODATA if buffer is finalized, -EAGAIN if there is currently no
 * data to read at consumed position, or 0 if the get operation succeeds.
 */

int lib_ring_buffer_snapshot(struct lib_ring_buffer *buf,
			     unsigned long *consumed, unsigned long *produced)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long consumed_cur, write_offset;
	int finalized;

	finalized = CMM_ACCESS_ONCE(buf->finalized);
	/*
	 * Read finalized before counters.
	 */
	cmm_smp_rmb();
	consumed_cur = uatomic_read(&buf->consumed);
	/*
	 * No need to issue a memory barrier between consumed count read and
	 * write offset read, because consumed count can only change
	 * concurrently in overwrite mode, and we keep a sequence counter
	 * identifier derived from the write offset to check we are getting
	 * the same sub-buffer we are expecting (the sub-buffers are atomically
	 * "tagged" upon writes, tags are checked upon read).
	 */
	write_offset = v_read(config, &buf->offset);

	/*
	 * Check that we are not about to read the same subbuffer in
	 * which the writer head is.
	 */
	if (subbuf_trunc(write_offset, chan) - subbuf_trunc(consumed_cur, chan)
	    == 0)
		goto nodata;

	*consumed = consumed_cur;
	*produced = subbuf_trunc(write_offset, chan);

	return 0;

nodata:
	/*
	 * The memory barriers __wait_event()/wake_up_interruptible() take care
	 * of "raw_spin_is_locked" memory ordering.
	 */
	if (finalized)
		return -ENODATA;
	else
		return -EAGAIN;
}

/**
 * lib_ring_buffer_put_snapshot - move consumed counter forward
 * @buf: ring buffer
 * @consumed_new: new consumed count value
 */
void lib_ring_buffer_move_consumer(struct lib_ring_buffer *buf,
				   unsigned long consumed_new)
{
	struct lib_ring_buffer_backend *bufb = &buf->backend;
	struct channel *chan = shmp(bufb->chan);
	unsigned long consumed;

	CHAN_WARN_ON(chan, uatomic_read(&buf->active_readers) != 1);

	/*
	 * Only push the consumed value forward.
	 * If the consumed cmpxchg fails, this is because we have been pushed by
	 * the writer in flight recorder mode.
	 */
	consumed = uatomic_read(&buf->consumed);
	while ((long) consumed - (long) consumed_new < 0)
		consumed = uatomic_cmpxchg(&buf->consumed, consumed,
					   consumed_new);
}

/**
 * lib_ring_buffer_get_subbuf - get exclusive access to subbuffer for reading
 * @buf: ring buffer
 * @consumed: consumed count indicating the position where to read
 *
 * Returns -ENODATA if buffer is finalized, -EAGAIN if there is currently no
 * data to read at consumed position, or 0 if the get operation succeeds.
 */
int lib_ring_buffer_get_subbuf(struct lib_ring_buffer *buf,
			       unsigned long consumed)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long consumed_cur, consumed_idx, commit_count, write_offset;
	int ret;
	int finalized;

retry:
	finalized = CMM_ACCESS_ONCE(buf->finalized);
	/*
	 * Read finalized before counters.
	 */
	cmm_smp_rmb();
	consumed_cur = uatomic_read(&buf->consumed);
	consumed_idx = subbuf_index(consumed, chan);
	commit_count = v_read(config, &shmp(buf->commit_cold)[consumed_idx].cc_sb);
	/*
	 * Make sure we read the commit count before reading the buffer
	 * data and the write offset. Correct consumed offset ordering
	 * wrt commit count is insured by the use of cmpxchg to update
	 * the consumed offset.
	 */
	/*
	 * Local rmb to match the remote wmb to read the commit count
	 * before the buffer data and the write offset.
	 */
	cmm_smp_rmb();

	write_offset = v_read(config, &buf->offset);

	/*
	 * Check that the buffer we are getting is after or at consumed_cur
	 * position.
	 */
	if ((long) subbuf_trunc(consumed, chan)
	    - (long) subbuf_trunc(consumed_cur, chan) < 0)
		goto nodata;

	/*
	 * Check that the subbuffer we are trying to consume has been
	 * already fully committed.
	 */
	if (((commit_count - chan->backend.subbuf_size)
	     & chan->commit_count_mask)
	    - (buf_trunc(consumed_cur, chan)
	       >> chan->backend.num_subbuf_order)
	    != 0)
		goto nodata;

	/*
	 * Check that we are not about to read the same subbuffer in
	 * which the writer head is.
	 */
	if (subbuf_trunc(write_offset, chan) - subbuf_trunc(consumed_cur, chan)
	    == 0)
		goto nodata;

	/*
	 * Failure to get the subbuffer causes a busy-loop retry without going
	 * to a wait queue. These are caused by short-lived race windows where
	 * the writer is getting access to a subbuffer we were trying to get
	 * access to. Also checks that the "consumed" buffer count we are
	 * looking for matches the one contained in the subbuffer id.
	 */
	ret = update_read_sb_index(config, &buf->backend, &chan->backend,
				   consumed_idx, buf_trunc_val(consumed, chan));
	if (ret)
		goto retry;
	subbuffer_id_clear_noref(config, &buf->backend.buf_rsb.id);

	buf->get_subbuf_consumed = consumed;
	buf->get_subbuf = 1;

	return 0;

nodata:
	/*
	 * The memory barriers __wait_event()/wake_up_interruptible() take care
	 * of "raw_spin_is_locked" memory ordering.
	 */
	if (finalized)
		return -ENODATA;
	else
		return -EAGAIN;
}

/**
 * lib_ring_buffer_put_subbuf - release exclusive subbuffer access
 * @buf: ring buffer
 */
void lib_ring_buffer_put_subbuf(struct lib_ring_buffer *buf)
{
	struct lib_ring_buffer_backend *bufb = &buf->backend;
	struct channel *chan = shmp(bufb->chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long read_sb_bindex, consumed_idx, consumed;

	CHAN_WARN_ON(chan, uatomic_read(&buf->active_readers) != 1);

	if (!buf->get_subbuf) {
		/*
		 * Reader puts a subbuffer it did not get.
		 */
		CHAN_WARN_ON(chan, 1);
		return;
	}
	consumed = buf->get_subbuf_consumed;
	buf->get_subbuf = 0;

	/*
	 * Clear the records_unread counter. (overruns counter)
	 * Can still be non-zero if a file reader simply grabbed the data
	 * without using iterators.
	 * Can be below zero if an iterator is used on a snapshot more than
	 * once.
	 */
	read_sb_bindex = subbuffer_id_get_index(config, bufb->buf_rsb.id);
	v_add(config, v_read(config,
			     &shmp(bufb->array)[read_sb_bindex]->records_unread),
	      &bufb->records_read);
	v_set(config, &shmp(bufb->array)[read_sb_bindex]->records_unread, 0);
	CHAN_WARN_ON(chan, config->mode == RING_BUFFER_OVERWRITE
		     && subbuffer_id_is_noref(config, bufb->buf_rsb.id));
	subbuffer_id_set_noref(config, &bufb->buf_rsb.id);

	/*
	 * Exchange the reader subbuffer with the one we put in its place in the
	 * writer subbuffer table. Expect the original consumed count. If
	 * update_read_sb_index fails, this is because the writer updated the
	 * subbuffer concurrently. We should therefore keep the subbuffer we
	 * currently have: it has become invalid to try reading this sub-buffer
	 * consumed count value anyway.
	 */
	consumed_idx = subbuf_index(consumed, chan);
	update_read_sb_index(config, &buf->backend, &chan->backend,
			     consumed_idx, buf_trunc_val(consumed, chan));
	/*
	 * update_read_sb_index return value ignored. Don't exchange sub-buffer
	 * if the writer concurrently updated it.
	 */
}

/*
 * cons_offset is an iterator on all subbuffer offsets between the reader
 * position and the writer position. (inclusive)
 */
static
void lib_ring_buffer_print_subbuffer_errors(struct lib_ring_buffer *buf,
					    struct channel *chan,
					    unsigned long cons_offset,
					    int cpu)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long cons_idx, commit_count, commit_count_sb;

	cons_idx = subbuf_index(cons_offset, chan);
	commit_count = v_read(config, &shmp(buf->commit_hot)[cons_idx].cc);
	commit_count_sb = v_read(config, &shmp(buf->commit_cold)[cons_idx].cc_sb);

	if (subbuf_offset(commit_count, chan) != 0)
		ERRMSG("ring buffer %s, cpu %d: "
		       "commit count in subbuffer %lu,\n"
		       "expecting multiples of %lu bytes\n"
		       "  [ %lu bytes committed, %lu bytes reader-visible ]\n",
		       chan->backend.name, cpu, cons_idx,
		       chan->backend.subbuf_size,
		       commit_count, commit_count_sb);

	ERRMSG("ring buffer: %s, cpu %d: %lu bytes committed\n",
	       chan->backend.name, cpu, commit_count);
}

static
void lib_ring_buffer_print_buffer_errors(struct lib_ring_buffer *buf,
					 struct channel *chan,
					 void *priv, int cpu)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long write_offset, cons_offset;

	/*
	 * Can be called in the error path of allocation when
	 * trans_channel_data is not yet set.
	 */
	if (!chan)
		return;
	/*
	 * No need to order commit_count, write_offset and cons_offset reads
	 * because we execute at teardown when no more writer nor reader
	 * references are left.
	 */
	write_offset = v_read(config, &buf->offset);
	cons_offset = uatomic_read(&buf->consumed);
	if (write_offset != cons_offset)
		ERRMSG("ring buffer %s, cpu %d: "
		       "non-consumed data\n"
		       "  [ %lu bytes written, %lu bytes read ]\n",
		       chan->backend.name, cpu, write_offset, cons_offset);

	for (cons_offset = uatomic_read(&buf->consumed);
	     (long) (subbuf_trunc((unsigned long) v_read(config, &buf->offset),
				  chan)
		     - cons_offset) > 0;
	     cons_offset = subbuf_align(cons_offset, chan))
		lib_ring_buffer_print_subbuffer_errors(buf, chan, cons_offset,
						       cpu);
}

static
void lib_ring_buffer_print_errors(struct channel *chan,
				  struct lib_ring_buffer *buf, int cpu)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	void *priv = chan->backend.priv;

	ERRMSG("ring buffer %s, cpu %d: %lu records written, "
			  "%lu records overrun\n",
			  chan->backend.name, cpu,
			  v_read(config, &buf->records_count),
			  v_read(config, &buf->records_overrun));

	if (v_read(config, &buf->records_lost_full)
	    || v_read(config, &buf->records_lost_wrap)
	    || v_read(config, &buf->records_lost_big))
		ERRMSG("ring buffer %s, cpu %d: records were lost. Caused by:\n"
		       "  [ %lu buffer full, %lu nest buffer wrap-around, "
		       "%lu event too big ]\n",
		       chan->backend.name, cpu,
		       v_read(config, &buf->records_lost_full),
		       v_read(config, &buf->records_lost_wrap),
		       v_read(config, &buf->records_lost_big));

	lib_ring_buffer_print_buffer_errors(buf, chan, priv, cpu);
}

/*
 * lib_ring_buffer_switch_old_start: Populate old subbuffer header.
 *
 * Only executed when the buffer is finalized, in SWITCH_FLUSH.
 */
static
void lib_ring_buffer_switch_old_start(struct lib_ring_buffer *buf,
				      struct channel *chan,
				      struct switch_offsets *offsets,
				      u64 tsc)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long oldidx = subbuf_index(offsets->old, chan);
	unsigned long commit_count;

	config->cb.buffer_begin(buf, tsc, oldidx);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	cmm_smp_wmb();
	v_add(config, config->cb.subbuffer_header_size(),
	      &shmp(buf->commit_hot)[oldidx].cc);
	commit_count = v_read(config, &shmp(buf->commit_hot)[oldidx].cc);
	/* Check if the written buffer has to be delivered */
	lib_ring_buffer_check_deliver(config, buf, chan, offsets->old,
				      commit_count, oldidx);
	lib_ring_buffer_write_commit_counter(config, buf, chan, oldidx,
					     offsets->old, commit_count,
					     config->cb.subbuffer_header_size());
}

/*
 * lib_ring_buffer_switch_old_end: switch old subbuffer
 *
 * Note : offset_old should never be 0 here. It is ok, because we never perform
 * buffer switch on an empty subbuffer in SWITCH_ACTIVE mode. The caller
 * increments the offset_old value when doing a SWITCH_FLUSH on an empty
 * subbuffer.
 */
static
void lib_ring_buffer_switch_old_end(struct lib_ring_buffer *buf,
				    struct channel *chan,
				    struct switch_offsets *offsets,
				    u64 tsc)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long oldidx = subbuf_index(offsets->old - 1, chan);
	unsigned long commit_count, padding_size, data_size;

	data_size = subbuf_offset(offsets->old - 1, chan) + 1;
	padding_size = chan->backend.subbuf_size - data_size;
	subbuffer_set_data_size(config, &buf->backend, oldidx, data_size);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	cmm_smp_wmb();
	v_add(config, padding_size, &shmp(buf->commit_hot)[oldidx].cc);
	commit_count = v_read(config, &shmp(buf->commit_hot)[oldidx].cc);
	lib_ring_buffer_check_deliver(config, buf, chan, offsets->old - 1,
				      commit_count, oldidx);
	lib_ring_buffer_write_commit_counter(config, buf, chan, oldidx,
					     offsets->old, commit_count,
					     padding_size);
}

/*
 * lib_ring_buffer_switch_new_start: Populate new subbuffer.
 *
 * This code can be executed unordered : writers may already have written to the
 * sub-buffer before this code gets executed, caution.  The commit makes sure
 * that this code is executed before the deliver of this sub-buffer.
 */
static
void lib_ring_buffer_switch_new_start(struct lib_ring_buffer *buf,
				      struct channel *chan,
				      struct switch_offsets *offsets,
				      u64 tsc)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long beginidx = subbuf_index(offsets->begin, chan);
	unsigned long commit_count;

	config->cb.buffer_begin(buf, tsc, beginidx);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	cmm_smp_wmb();
	v_add(config, config->cb.subbuffer_header_size(),
	      &shmp(buf->commit_hot)[beginidx].cc);
	commit_count = v_read(config, &shmp(buf->commit_hot)[beginidx].cc);
	/* Check if the written buffer has to be delivered */
	lib_ring_buffer_check_deliver(config, buf, chan, offsets->begin,
				      commit_count, beginidx);
	lib_ring_buffer_write_commit_counter(config, buf, chan, beginidx,
					     offsets->begin, commit_count,
					     config->cb.subbuffer_header_size());
}

/*
 * lib_ring_buffer_switch_new_end: finish switching current subbuffer
 *
 * The only remaining threads could be the ones with pending commits. They will
 * have to do the deliver themselves.
 */
static
void lib_ring_buffer_switch_new_end(struct lib_ring_buffer *buf,
					    struct channel *chan,
					    struct switch_offsets *offsets,
					    u64 tsc)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long endidx = subbuf_index(offsets->end - 1, chan);
	unsigned long commit_count, padding_size, data_size;

	data_size = subbuf_offset(offsets->end - 1, chan) + 1;
	padding_size = chan->backend.subbuf_size - data_size;
	subbuffer_set_data_size(config, &buf->backend, endidx, data_size);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	cmm_smp_wmb();
	v_add(config, padding_size, &shmp(buf->commit_hot)[endidx].cc);
	commit_count = v_read(config, &shmp(buf->commit_hot)[endidx].cc);
	lib_ring_buffer_check_deliver(config, buf, chan, offsets->end - 1,
				  commit_count, endidx);
	lib_ring_buffer_write_commit_counter(config, buf, chan, endidx,
					     offsets->end, commit_count,
					     padding_size);
}

/*
 * Returns :
 * 0 if ok
 * !0 if execution must be aborted.
 */
static
int lib_ring_buffer_try_switch_slow(enum switch_mode mode,
				    struct lib_ring_buffer *buf,
				    struct channel *chan,
				    struct switch_offsets *offsets,
				    u64 *tsc)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long off;

	offsets->begin = v_read(config, &buf->offset);
	offsets->old = offsets->begin;
	offsets->switch_old_start = 0;
	off = subbuf_offset(offsets->begin, chan);

	*tsc = config->cb.ring_buffer_clock_read(chan);

	/*
	 * Ensure we flush the header of an empty subbuffer when doing the
	 * finalize (SWITCH_FLUSH). This ensures that we end up knowing the
	 * total data gathering duration even if there were no records saved
	 * after the last buffer switch.
	 * In SWITCH_ACTIVE mode, switch the buffer when it contains events.
	 * SWITCH_ACTIVE only flushes the current subbuffer, dealing with end of
	 * subbuffer header as appropriate.
	 * The next record that reserves space will be responsible for
	 * populating the following subbuffer header. We choose not to populate
	 * the next subbuffer header here because we want to be able to use
	 * SWITCH_ACTIVE for periodical buffer flush, which must
	 * guarantee that all the buffer content (records and header
	 * timestamps) are visible to the reader. This is required for
	 * quiescence guarantees for the fusion merge.
	 */
	if (mode == SWITCH_FLUSH || off > 0) {
		if (unlikely(off == 0)) {
			/*
			 * The client does not save any header information.
			 * Don't switch empty subbuffer on finalize, because it
			 * is invalid to deliver a completely empty subbuffer.
			 */
			if (!config->cb.subbuffer_header_size())
				return -1;
			/*
			 * Need to write the subbuffer start header on finalize.
			 */
			offsets->switch_old_start = 1;
		}
		offsets->begin = subbuf_align(offsets->begin, chan);
	} else
		return -1;	/* we do not have to switch : buffer is empty */
	/* Note: old points to the next subbuf at offset 0 */
	offsets->end = offsets->begin;
	return 0;
}

/*
 * Force a sub-buffer switch. This operation is completely reentrant : can be
 * called while tracing is active with absolutely no lock held.
 *
 * Note, however, that as a v_cmpxchg is used for some atomic
 * operations, this function must be called from the CPU which owns the buffer
 * for a ACTIVE flush.
 */
void lib_ring_buffer_switch_slow(struct lib_ring_buffer *buf, enum switch_mode mode)
{
	struct channel *chan = shmp(buf->backend.chan);
	const struct lib_ring_buffer_config *config = chan->backend.config;
	struct switch_offsets offsets;
	unsigned long oldidx;
	u64 tsc;

	offsets.size = 0;

	/*
	 * Perform retryable operations.
	 */
	do {
		if (lib_ring_buffer_try_switch_slow(mode, buf, chan, &offsets,
						    &tsc))
			return;	/* Switch not needed */
	} while (v_cmpxchg(config, &buf->offset, offsets.old, offsets.end)
		 != offsets.old);

	/*
	 * Atomically update last_tsc. This update races against concurrent
	 * atomic updates, but the race will always cause supplementary full TSC
	 * records, never the opposite (missing a full TSC record when it would
	 * be needed).
	 */
	save_last_tsc(config, buf, tsc);

	/*
	 * Push the reader if necessary
	 */
	lib_ring_buffer_reserve_push_reader(buf, chan, offsets.old);

	oldidx = subbuf_index(offsets.old, chan);
	lib_ring_buffer_clear_noref(config, &buf->backend, oldidx);

	/*
	 * May need to populate header start on SWITCH_FLUSH.
	 */
	if (offsets.switch_old_start) {
		lib_ring_buffer_switch_old_start(buf, chan, &offsets, tsc);
		offsets.old += config->cb.subbuffer_header_size();
	}

	/*
	 * Switch old subbuffer.
	 */
	lib_ring_buffer_switch_old_end(buf, chan, &offsets, tsc);
}

/*
 * Returns :
 * 0 if ok
 * -ENOSPC if event size is too large for packet.
 * -ENOBUFS if there is currently not enough space in buffer for the event.
 * -EIO if data cannot be written into the buffer for any other reason.
 */
static
int lib_ring_buffer_try_reserve_slow(struct lib_ring_buffer *buf,
				     struct channel *chan,
				     struct switch_offsets *offsets,
				     struct lib_ring_buffer_ctx *ctx)
{
	const struct lib_ring_buffer_config *config = chan->backend.config;
	unsigned long reserve_commit_diff;

	offsets->begin = v_read(config, &buf->offset);
	offsets->old = offsets->begin;
	offsets->switch_new_start = 0;
	offsets->switch_new_end = 0;
	offsets->switch_old_end = 0;
	offsets->pre_header_padding = 0;

	ctx->tsc = config->cb.ring_buffer_clock_read(chan);
	if ((int64_t) ctx->tsc == -EIO)
		return -EIO;

	if (last_tsc_overflow(config, buf, ctx->tsc))
		ctx->rflags |= RING_BUFFER_RFLAG_FULL_TSC;

	if (unlikely(subbuf_offset(offsets->begin, ctx->chan) == 0)) {
		offsets->switch_new_start = 1;		/* For offsets->begin */
	} else {
		offsets->size = config->cb.record_header_size(config, chan,
						offsets->begin,
						&offsets->pre_header_padding,
						ctx);
		offsets->size +=
			lib_ring_buffer_align(offsets->begin + offsets->size,
					      ctx->largest_align)
			+ ctx->data_size;
		if (unlikely(subbuf_offset(offsets->begin, chan) +
			     offsets->size > chan->backend.subbuf_size)) {
			offsets->switch_old_end = 1;	/* For offsets->old */
			offsets->switch_new_start = 1;	/* For offsets->begin */
		}
	}
	if (unlikely(offsets->switch_new_start)) {
		unsigned long sb_index;

		/*
		 * We are typically not filling the previous buffer completely.
		 */
		if (likely(offsets->switch_old_end))
			offsets->begin = subbuf_align(offsets->begin, chan);
		offsets->begin = offsets->begin
				 + config->cb.subbuffer_header_size();
		/* Test new buffer integrity */
		sb_index = subbuf_index(offsets->begin, chan);
		reserve_commit_diff =
		  (buf_trunc(offsets->begin, chan)
		   >> chan->backend.num_subbuf_order)
		  - ((unsigned long) v_read(config,
					    &shmp(buf->commit_cold)[sb_index].cc_sb)
		     & chan->commit_count_mask);
		if (likely(reserve_commit_diff == 0)) {
			/* Next subbuffer not being written to. */
			if (unlikely(config->mode != RING_BUFFER_OVERWRITE &&
				subbuf_trunc(offsets->begin, chan)
				 - subbuf_trunc((unsigned long)
				     uatomic_read(&buf->consumed), chan)
				>= chan->backend.buf_size)) {
				/*
				 * We do not overwrite non consumed buffers
				 * and we are full : record is lost.
				 */
				v_inc(config, &buf->records_lost_full);
				return -ENOBUFS;
			} else {
				/*
				 * Next subbuffer not being written to, and we
				 * are either in overwrite mode or the buffer is
				 * not full. It's safe to write in this new
				 * subbuffer.
				 */
			}
		} else {
			/*
			 * Next subbuffer reserve offset does not match the
			 * commit offset. Drop record in producer-consumer and
			 * overwrite mode. Caused by either a writer OOPS or too
			 * many nested writes over a reserve/commit pair.
			 */
			v_inc(config, &buf->records_lost_wrap);
			return -EIO;
		}
		offsets->size =
			config->cb.record_header_size(config, chan,
						offsets->begin,
						&offsets->pre_header_padding,
						ctx);
		offsets->size +=
			lib_ring_buffer_align(offsets->begin + offsets->size,
					      ctx->largest_align)
			+ ctx->data_size;
		if (unlikely(subbuf_offset(offsets->begin, chan)
			     + offsets->size > chan->backend.subbuf_size)) {
			/*
			 * Record too big for subbuffers, report error, don't
			 * complete the sub-buffer switch.
			 */
			v_inc(config, &buf->records_lost_big);
			return -ENOSPC;
		} else {
			/*
			 * We just made a successful buffer switch and the
			 * record fits in the new subbuffer. Let's write.
			 */
		}
	} else {
		/*
		 * Record fits in the current buffer and we are not on a switch
		 * boundary. It's safe to write.
		 */
	}
	offsets->end = offsets->begin + offsets->size;

	if (unlikely(subbuf_offset(offsets->end, chan) == 0)) {
		/*
		 * The offset_end will fall at the very beginning of the next
		 * subbuffer.
		 */
		offsets->switch_new_end = 1;	/* For offsets->begin */
	}
	return 0;
}

/**
 * lib_ring_buffer_reserve_slow - Atomic slot reservation in a buffer.
 * @ctx: ring buffer context.
 *
 * Return : -NOBUFS if not enough space, -ENOSPC if event size too large,
 * -EIO for other errors, else returns 0.
 * It will take care of sub-buffer switching.
 */
int lib_ring_buffer_reserve_slow(struct lib_ring_buffer_ctx *ctx)
{
	struct channel *chan = ctx->chan;
	const struct lib_ring_buffer_config *config = chan->backend.config;
	struct lib_ring_buffer *buf;
	struct switch_offsets offsets;
	int ret;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		buf = &shmp(chan->backend.buf)[ctx->cpu];
	else
		buf = shmp(chan->backend.buf);
	ctx->buf = buf;

	offsets.size = 0;

	do {
		ret = lib_ring_buffer_try_reserve_slow(buf, chan, &offsets,
						       ctx);
		if (unlikely(ret))
			return ret;
	} while (unlikely(v_cmpxchg(config, &buf->offset, offsets.old,
				    offsets.end)
			  != offsets.old));

	/*
	 * Atomically update last_tsc. This update races against concurrent
	 * atomic updates, but the race will always cause supplementary full TSC
	 * records, never the opposite (missing a full TSC record when it would
	 * be needed).
	 */
	save_last_tsc(config, buf, ctx->tsc);

	/*
	 * Push the reader if necessary
	 */
	lib_ring_buffer_reserve_push_reader(buf, chan, offsets.end - 1);

	/*
	 * Clear noref flag for this subbuffer.
	 */
	lib_ring_buffer_clear_noref(config, &buf->backend,
				    subbuf_index(offsets.end - 1, chan));

	/*
	 * Switch old subbuffer if needed.
	 */
	if (unlikely(offsets.switch_old_end)) {
		lib_ring_buffer_clear_noref(config, &buf->backend,
					    subbuf_index(offsets.old - 1, chan));
		lib_ring_buffer_switch_old_end(buf, chan, &offsets, ctx->tsc);
	}

	/*
	 * Populate new subbuffer.
	 */
	if (unlikely(offsets.switch_new_start))
		lib_ring_buffer_switch_new_start(buf, chan, &offsets, ctx->tsc);

	if (unlikely(offsets.switch_new_end))
		lib_ring_buffer_switch_new_end(buf, chan, &offsets, ctx->tsc);

	ctx->slot_size = offsets.size;
	ctx->pre_offset = offsets.begin;
	ctx->buf_offset = offsets.begin + offsets.pre_header_padding;
	return 0;
}