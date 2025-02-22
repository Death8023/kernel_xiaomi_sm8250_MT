// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to tagged command queuing
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>

#include "blk.h"

#if defined(CONFIG_OPLUS_FEATURE_UXIO_FIRST)
#include "uxio_first/uxio_first_opt.h"
#endif

/**
 * blk_queue_find_tag - find a request by its tag and queue
 * @q:	 The request queue for the device
 * @tag: The tag of the request
 *
 * Notes:
 *    Should be used when a device returns a tag and you want to match
 *    it with a request.
 *
 *    no locks need be held.
 **/
struct request *blk_queue_find_tag(struct request_queue *q, int tag)
{
	return blk_map_queue_find_tag(q->queue_tags, tag);
}
EXPORT_SYMBOL(blk_queue_find_tag);

/**
 * blk_free_tags - release a given set of tag maintenance info
 * @bqt:	the tag map to free
 *
 * Drop the reference count on @bqt and frees it when the last reference
 * is dropped.
 */
void blk_free_tags(struct blk_queue_tag *bqt)
{
	if (atomic_dec_and_test(&bqt->refcnt)) {
		BUG_ON(find_first_bit(bqt->tag_map, bqt->max_depth) <
							bqt->max_depth);

		kfree(bqt->tag_index);
		bqt->tag_index = NULL;

		kfree(bqt->tag_map);
		bqt->tag_map = NULL;

		kfree(bqt);
	}
}
EXPORT_SYMBOL(blk_free_tags);

/**
 * __blk_queue_free_tags - release tag maintenance info
 * @q:  the request queue for the device
 *
 *  Notes:
 *    blk_cleanup_queue() will take care of calling this function, if tagging
 *    has been used. So there's no need to call this directly.
 **/
void __blk_queue_free_tags(struct request_queue *q)
{
	struct blk_queue_tag *bqt = q->queue_tags;

	if (!bqt)
		return;

	blk_free_tags(bqt);

	q->queue_tags = NULL;
	queue_flag_clear_unlocked(QUEUE_FLAG_QUEUED, q);
}

/**
 * blk_queue_free_tags - release tag maintenance info
 * @q:  the request queue for the device
 *
 *  Notes:
 *	This is used to disable tagged queuing to a device, yet leave
 *	queue in function.
 **/
void blk_queue_free_tags(struct request_queue *q)
{
	queue_flag_clear_unlocked(QUEUE_FLAG_QUEUED, q);
}
EXPORT_SYMBOL(blk_queue_free_tags);

static int
init_tag_map(struct request_queue *q, struct blk_queue_tag *tags, int depth)
{
	struct request **tag_index;
	unsigned long *tag_map;
	int nr_ulongs;

	if (q && depth > q->nr_requests * 2) {
		depth = q->nr_requests * 2;
		printk(KERN_ERR "%s: adjusted depth to %d\n",
		       __func__, depth);
	}

	tag_index = kcalloc(depth, sizeof(struct request *), GFP_ATOMIC);
	if (!tag_index)
		goto fail;

	nr_ulongs = ALIGN(depth, BITS_PER_LONG) / BITS_PER_LONG;
	tag_map = kcalloc(nr_ulongs, sizeof(unsigned long), GFP_ATOMIC);
	if (!tag_map)
		goto fail;

	tags->real_max_depth = depth;
	tags->max_depth = depth;
#if defined(CONFIG_OPLUS_FEATURE_UXIO_FIRST)
	tags->bg_max_depth = BLK_MAX_BG_DEPTH;
#endif
	tags->tag_index = tag_index;
	tags->tag_map = tag_map;

	return 0;
fail:
	kfree(tag_index);
	return -ENOMEM;
}

static struct blk_queue_tag *__blk_queue_init_tags(struct request_queue *q,
						int depth, int alloc_policy)
{
	struct blk_queue_tag *tags;

	tags = kmalloc(sizeof(struct blk_queue_tag), GFP_ATOMIC);
	if (!tags)
		goto fail;

	if (init_tag_map(q, tags, depth))
		goto fail;

	atomic_set(&tags->refcnt, 1);
	tags->alloc_policy = alloc_policy;
	tags->next_tag = 0;
	return tags;
fail:
	kfree(tags);
	return NULL;
}

/**
 * blk_init_tags - initialize the tag info for an external tag map
 * @depth:	the maximum queue depth supported
 * @alloc_policy: tag allocation policy
 **/
struct blk_queue_tag *blk_init_tags(int depth, int alloc_policy)
{
	return __blk_queue_init_tags(NULL, depth, alloc_policy);
}
EXPORT_SYMBOL(blk_init_tags);

/**
 * blk_queue_init_tags - initialize the queue tag info
 * @q:  the request queue for the device
 * @depth:  the maximum queue depth supported
 * @tags: the tag to use
 * @alloc_policy: tag allocation policy
 *
 * Queue lock must be held here if the function is called to resize an
 * existing map.
 **/
int blk_queue_init_tags(struct request_queue *q, int depth,
			struct blk_queue_tag *tags, int alloc_policy)
{
	int rc;

	BUG_ON(tags && q->queue_tags && tags != q->queue_tags);

	if (!tags && !q->queue_tags) {
		tags = __blk_queue_init_tags(q, depth, alloc_policy);

		if (!tags)
			return -ENOMEM;

	} else if (q->queue_tags) {
		rc = blk_queue_resize_tags(q, depth);
		if (rc)
			return rc;
		queue_flag_set(QUEUE_FLAG_QUEUED, q);
		return 0;
	} else
		atomic_inc(&tags->refcnt);

	/*
	 * assign it, all done
	 */
	q->queue_tags = tags;
	queue_flag_set_unlocked(QUEUE_FLAG_QUEUED, q);
	return 0;
}
EXPORT_SYMBOL(blk_queue_init_tags);

/**
 * blk_queue_resize_tags - change the queueing depth
 * @q:  the request queue for the device
 * @new_depth: the new max command queueing depth
 *
 *  Notes:
 *    Must be called with the queue lock held.
 **/
int blk_queue_resize_tags(struct request_queue *q, int new_depth)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	struct request **tag_index;
	unsigned long *tag_map;
	int max_depth, nr_ulongs;

	if (!bqt)
		return -ENXIO;

	/*
	 * if we already have large enough real_max_depth.  just
	 * adjust max_depth.  *NOTE* as requests with tag value
	 * between new_depth and real_max_depth can be in-flight, tag
	 * map can not be shrunk blindly here.
	 */
	if (new_depth <= bqt->real_max_depth) {
		bqt->max_depth = new_depth;
		return 0;
	}

	/*
	 * Currently cannot replace a shared tag map with a new
	 * one, so error out if this is the case
	 */
	if (atomic_read(&bqt->refcnt) != 1)
		return -EBUSY;

	/*
	 * save the old state info, so we can copy it back
	 */
	tag_index = bqt->tag_index;
	tag_map = bqt->tag_map;
	max_depth = bqt->real_max_depth;

	if (init_tag_map(q, bqt, new_depth))
		return -ENOMEM;

	memcpy(bqt->tag_index, tag_index, max_depth * sizeof(struct request *));
	nr_ulongs = ALIGN(max_depth, BITS_PER_LONG) / BITS_PER_LONG;
	memcpy(bqt->tag_map, tag_map, nr_ulongs * sizeof(unsigned long));

	kfree(tag_index);
	kfree(tag_map);
	return 0;
}
EXPORT_SYMBOL(blk_queue_resize_tags);

/**
 * blk_queue_end_tag - end tag operations for a request
 * @q:  the request queue for the device
 * @rq: the request that has completed
 *
 *  Description:
 *    Typically called when end_that_request_first() returns %0, meaning
 *    all transfers have been done for a request. It's important to call
 *    this function before end_that_request_last(), as that will put the
 *    request back on the free list thus corrupting the internal tag list.
 **/
void blk_queue_end_tag(struct request_queue *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	unsigned tag = rq->tag; /* negative tags invalid */

	lockdep_assert_held(q->queue_lock);

	BUG_ON(tag >= bqt->real_max_depth);

	list_del_init(&rq->queuelist);
	rq->rq_flags &= ~RQF_QUEUED;
	rq->tag = -1;
	rq->internal_tag = -1;

	if (unlikely(bqt->tag_index[tag] == NULL))
		printk(KERN_ERR "%s: tag %d is missing\n",
		       __func__, tag);

	bqt->tag_index[tag] = NULL;

	if (unlikely(!test_bit(tag, bqt->tag_map))) {
		printk(KERN_ERR "%s: attempt to clear non-busy tag (%d)\n",
		       __func__, tag);
		return;
	}
	/*
	 * The tag_map bit acts as a lock for tag_index[bit], so we need
	 * unlock memory barrier semantics.
	 */
	clear_bit_unlock(tag, bqt->tag_map);
}

/**
 * blk_queue_start_tag - find a free tag and assign it
 * @q:  the request queue for the device
 * @rq:  the block request that needs tagging
 *
 *  Description:
 *    This can either be used as a stand-alone helper, or possibly be
 *    assigned as the queue &prep_rq_fn (in which case &struct request
 *    automagically gets a tag assigned). Note that this function
 *    assumes that any type of request can be queued! if this is not
 *    true for your device, you must check the request type before
 *    calling this function.  The request will also be removed from
 *    the request queue, so it's the drivers responsibility to readd
 *    it if it should need to be restarted for some reason.
 **/
int blk_queue_start_tag(struct request_queue *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	unsigned max_depth;
	int tag;

	lockdep_assert_held(q->queue_lock);

	if (unlikely((rq->rq_flags & RQF_QUEUED))) {
		printk(KERN_ERR
		       "%s: request %p for device [%s] already tagged %d",
		       __func__, rq,
		       rq->rq_disk ? rq->rq_disk->disk_name : "?", rq->tag);
		BUG();
	}

	/*
	 * Protect against shared tag maps, as we may not have exclusive
	 * access to the tag map.
	 *
	 * We reserve a few tags just for sync IO, since we don't want
	 * to starve sync IO on behalf of flooding async IO.
	 */
	max_depth = bqt->max_depth;
	if (!rq_is_sync(rq) && max_depth > 1) {
		switch (max_depth) {
		case 2:
			max_depth = 1;
			break;
		case 3:
			max_depth = 2;
			break;
		default:
			max_depth -= 2;
		}
		if (q->in_flight[BLK_RW_ASYNC] > max_depth)
			return 1;
	}

	do {
		if (bqt->alloc_policy == BLK_TAG_ALLOC_FIFO) {
			tag = find_first_zero_bit(bqt->tag_map, max_depth);
			if (tag >= max_depth)
				return 1;
		} else {
			int start = bqt->next_tag;
			int size = min_t(int, bqt->max_depth, max_depth + start);
			tag = find_next_zero_bit(bqt->tag_map, size, start);
			if (tag >= size && start + size > bqt->max_depth) {
				size = start + size - bqt->max_depth;
				tag = find_first_zero_bit(bqt->tag_map, size);
			}
			if (tag >= size)
				return 1;
		}

	} while (test_and_set_bit_lock(tag, bqt->tag_map));
	/*
	 * We need lock ordering semantics given by test_and_set_bit_lock.
	 * See blk_queue_end_tag for details.
	 */

	bqt->next_tag = (tag + 1) % bqt->max_depth;
	rq->rq_flags |= RQF_QUEUED;
	rq->tag = tag;
	bqt->tag_index[tag] = rq;
	blk_start_request(rq);
	return 0;
}
EXPORT_SYMBOL(blk_queue_start_tag);
