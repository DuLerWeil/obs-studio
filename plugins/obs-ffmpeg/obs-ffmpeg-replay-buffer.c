#include "obs-ffmpeg-replay-buffer.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/threading.h>

#include <string.h>

/* Packets live in blocks of this many slots. Memory grows one block at a time
 * and is handed back one block at a time once history has rolled past it, so the
 * footprint follows the buffer in the same fixed step instead of doubling. */
#define REPLAY_BUFFER_BLOCK_PACKETS 1024

/* The block pointer table grows in this fixed step as well. */
#define REPLAY_BUFFER_BLOCK_STEP 16

/* Written when a slot owns a reference and cleared when it is released, so a
 * slot can only ever be released once. */
#define REPLAY_BUFFER_SLOT_LIVE 0x52425331u

/* The packet store is addressed by absolute packet index. Its block is
 * "index / REPLAY_BUFFER_BLOCK_PACKETS" and the slot inside it is the remainder,
 * so no position is ever derived from a counter: the cache keeps only the first
 * and the next index, and both only move when a packet is stored or a whole GOP
 * is dropped. Every slot also carries the index it holds, so a lookup either
 * finds that exact packet or nothing at all.
 *
 * Blocks are allocated and released in fixed steps, which keeps the footprint
 * proportional to the buffered history: no doubling, no re-placing of live
 * packets when the store grows. */
struct replay_buffer_packet {
	struct encoder_packet packet;
	uint64_t index;
	uint32_t magic;
};

/* One GOP: a video keyframe and every packet that followed it until the next
 * keyframe. Its extent is defined by where the next GOP starts, not by a count
 * that would have to be kept in step with every push. */
struct replay_buffer_gop {
	int64_t start_dts_usec;
	uint64_t first_index;
};

struct replay_buffer_cache {
	pthread_mutex_t mutex;

	/* blocks[0] holds absolute block "first_block"; free blocks are handed back
	 * from the front as history rolls. */
	struct replay_buffer_packet **blocks;
	size_t blocks_num;
	size_t blocks_capacity;
	uint64_t first_block;

	uint64_t first_index;
	uint64_t next_index;

	DARRAY(struct replay_buffer_gop) gops;
	size_t gops_head;

	int64_t first_dts_usec;
	int64_t last_dts_usec;
	int64_t total_size;
	int64_t max_size_bytes;
	int64_t max_time_usec;

	size_t video_track_idx;
};

struct replay_buffer_snapshot {
	DARRAY(struct encoder_packet) packets;
};

static size_t cache_packet_count(const replay_buffer_cache_t *cache)
{
	return (size_t)(cache->next_index - cache->first_index);
}

static size_t cache_gop_count(const replay_buffer_cache_t *cache)
{
	return cache->gops.num - cache->gops_head;
}

static struct replay_buffer_gop *cache_gop_at(const replay_buffer_cache_t *cache, size_t index)
{
	return &cache->gops.array[cache->gops_head + index];
}

static uint64_t cache_block_of(uint64_t index)
{
	return index / REPLAY_BUFFER_BLOCK_PACKETS;
}

/* Raw slot for an absolute index, without checking what the slot holds. */
static struct replay_buffer_packet *cache_slot_at(replay_buffer_cache_t *cache, uint64_t index)
{
	const size_t block = (size_t)(cache_block_of(index) - cache->first_block);
	return &cache->blocks[block][index % REPLAY_BUFFER_BLOCK_PACKETS];
}

/* Slot for an absolute index, but only when it really holds that packet. */
static struct replay_buffer_packet *cache_live_slot(replay_buffer_cache_t *cache, uint64_t index)
{
	if (index < cache->first_index || index >= cache->next_index)
		return NULL;

	struct replay_buffer_packet *slot = cache_slot_at(cache, index);
	if (slot->magic != REPLAY_BUFFER_SLOT_LIVE || slot->index != index)
		return NULL;

	return slot;
}

/* Releases the reference a slot owns, but only when the slot still holds the
 * index it is being released for. Anything else is reported and dropped, so a
 * stale slot can never reach obs_encoder_packet_release(). */
static void cache_release_slot(replay_buffer_cache_t *cache, uint64_t index)
{
	struct replay_buffer_packet *slot = cache_slot_at(cache, index);

	if (slot->magic != REPLAY_BUFFER_SLOT_LIVE || slot->index != index) {
		blog(LOG_WARNING, "[replay buffer] slot for index %llu is stale (magic 0x%x, holds %llu), dropping it",
		     (unsigned long long)index, slot->magic, (unsigned long long)slot->index);
		memset(&slot->packet, 0, sizeof(slot->packet));
		slot->magic = 0;
		return;
	}

	slot->magic = 0;
	cache->total_size -= (int64_t)slot->packet.size;
	obs_encoder_packet_release(&slot->packet);
}

/* Makes sure the block that holds index exists. Blocks are zeroed, so their
 * slots never look live before a packet is stored in them. */
static bool cache_ensure_packet(replay_buffer_cache_t *cache, uint64_t index)
{
	const uint64_t block = cache_block_of(index);

	/* Indices only move forward, so a block before the table start means the
	 * caller asked for history that is already gone. */
	if (block < cache->first_block)
		return false;

	const size_t offset = (size_t)(block - cache->first_block);

	if (offset < cache->blocks_num)
		return true;

	if (cache->blocks_num == cache->blocks_capacity) {
		const size_t capacity = cache->blocks_capacity + REPLAY_BUFFER_BLOCK_STEP;
		struct replay_buffer_packet **blocks = brealloc(cache->blocks, sizeof(*blocks) * capacity);
		if (!blocks)
			return false;

		cache->blocks = blocks;
		cache->blocks_capacity = capacity;
	}

	struct replay_buffer_packet *fresh = bzalloc(sizeof(*fresh) * REPLAY_BUFFER_BLOCK_PACKETS);
	if (!fresh)
		return false;

	cache->blocks[cache->blocks_num++] = fresh;
	return true;
}

/* Hands back the blocks whose packets have all been dropped. */
static void cache_release_blocks(replay_buffer_cache_t *cache)
{
	if (cache->first_index >= cache->next_index) {
		for (size_t i = 0; i < cache->blocks_num; i++)
			bfree(cache->blocks[i]);
		cache->blocks_num = 0;
		cache->first_block = cache_block_of(cache->first_index);
		return;
	}

	const uint64_t keep = cache_block_of(cache->first_index);

	while (cache->first_block < keep && cache->blocks_num > 1) {
		bfree(cache->blocks[0]);
		memmove(cache->blocks, cache->blocks + 1, (cache->blocks_num - 1) * sizeof(*cache->blocks));
		cache->blocks_num--;
		cache->first_block++;
	}
}

static void cache_compact_gops(replay_buffer_cache_t *cache)
{
	if (!cache->gops_head)
		return;

	const size_t remaining = cache->gops.num - cache->gops_head;
	if (remaining)
		memmove(cache->gops.array, cache->gops.array + cache->gops_head,
			remaining * sizeof(*cache->gops.array));

	cache->gops.num = remaining;
	cache->gops_head = 0;
}

/* Drops the oldest GOP. Its extent runs from its own keyframe to the keyframe of
 * the next GOP, or to the newest packet when it is the last one buffered. */
static void cache_drop_front_gop(replay_buffer_cache_t *cache)
{
	const uint64_t start = cache_gop_at(cache, 0)->first_index;
	const uint64_t end = cache_gop_count(cache) > 1 ? cache_gop_at(cache, 1)->first_index : cache->next_index;

	for (uint64_t index = start; index < end && index < cache->next_index; index++)
		cache_release_slot(cache, index);

	cache->first_index = end;

	/* The GOP index follows the packets, not the other way round: every entry
	 * whose packets are gone is dropped, so the two can never disagree even if a
	 * caller dropped a GOP more than once. */
	while (cache_gop_count(cache) > 0 && cache_gop_at(cache, 0)->first_index < cache->first_index)
		cache->gops_head++;

	cache_compact_gops(cache);
	cache_release_blocks(cache);

	if (cache->first_index >= cache->next_index) {
		cache->first_dts_usec = 0;
		cache->last_dts_usec = 0;
		cache->total_size = 0;
	} else {
		struct replay_buffer_packet *slot = cache_live_slot(cache, cache->first_index);
		cache->first_dts_usec = slot ? slot->packet.dts_usec : 0;
	}
}

/* Two GOPs are always kept: one to serve as the start of an export, one to hold
 * whatever is being written right now. */
static void cache_trim(replay_buffer_cache_t *cache)
{
	const bool have_size_limit = cache->max_size_bytes > 0;
	const bool have_time_limit = cache->max_time_usec > 0;
	if (!have_size_limit && !have_time_limit)
		return;

	while (cache_gop_count(cache) > 2) {
		const bool over_size = have_size_limit && cache->total_size > cache->max_size_bytes;
		const bool over_time = have_time_limit &&
				       (cache->last_dts_usec - cache_gop_at(cache, 0)->start_dts_usec) >
					       cache->max_time_usec;
		if (!over_size && !over_time)
			break;

		cache_drop_front_gop(cache);
	}
}

replay_buffer_cache_t *replay_buffer_cache_create(size_t video_track_idx)
{
	replay_buffer_cache_t *cache = bzalloc(sizeof(*cache));
	if (!cache)
		return NULL;

	if (pthread_mutex_init(&cache->mutex, NULL) != 0) {
		bfree(cache);
		return NULL;
	}

	cache->video_track_idx = video_track_idx;
	return cache;
}

void replay_buffer_cache_destroy(replay_buffer_cache_t *cache)
{
	if (!cache)
		return;

	replay_buffer_cache_clear(cache);
	bfree(cache->blocks);
	da_free(cache->gops);
	pthread_mutex_destroy(&cache->mutex);
	bfree(cache);
}

void replay_buffer_cache_set_limits(replay_buffer_cache_t *cache, int64_t max_size_bytes, int64_t max_time_usec)
{
	if (!cache)
		return;

	pthread_mutex_lock(&cache->mutex);
	cache->max_size_bytes = max_size_bytes;
	cache->max_time_usec = max_time_usec;
	pthread_mutex_unlock(&cache->mutex);
}

void replay_buffer_cache_clear(replay_buffer_cache_t *cache)
{
	if (!cache)
		return;

	pthread_mutex_lock(&cache->mutex);

	for (uint64_t index = cache->first_index; index < cache->next_index; index++)
		cache_release_slot(cache, index);

	/* next_index is intentionally not reset: stale asynchronous frame requests
	 * must not match packets from a later session. */
	cache->first_index = cache->next_index;
	da_clear(cache->gops);
	cache->gops_head = 0;
	cache->first_dts_usec = 0;
	cache->last_dts_usec = 0;
	cache->total_size = 0;
	cache_release_blocks(cache);

	pthread_mutex_unlock(&cache->mutex);
}

void replay_buffer_cache_push(replay_buffer_cache_t *cache, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return;

	const bool starts_gop = packet->type == OBS_ENCODER_VIDEO && packet->keyframe &&
				packet->track_idx == cache->video_track_idx;

	pthread_mutex_lock(&cache->mutex);

	/* Packets before the first video keyframe have no GOP to belong to and can
	 * never be exported, because every export starts on a keyframe. */
	if (!cache_gop_count(cache) && !starts_gop) {
		pthread_mutex_unlock(&cache->mutex);
		return;
	}

	if (!cache_ensure_packet(cache, cache->next_index)) {
		pthread_mutex_unlock(&cache->mutex);
		return;
	}

	struct replay_buffer_packet *slot = cache_slot_at(cache, cache->next_index);
	if (slot->magic == REPLAY_BUFFER_SLOT_LIVE) {
		/* A live slot must never be overwritten: its reference would leak. */
		blog(LOG_WARNING, "[replay buffer] slot for index %llu is still live while pushing",
		     (unsigned long long)cache->next_index);
		cache_release_slot(cache, cache->next_index);
	}

	obs_encoder_packet_ref(&slot->packet, packet);
	slot->index = cache->next_index;
	slot->magic = REPLAY_BUFFER_SLOT_LIVE;

	if (starts_gop) {
		struct replay_buffer_gop gop = {0};
		gop.start_dts_usec = packet->dts_usec;
		gop.first_index = cache->next_index;

		if (!da_push_back(cache->gops, &gop)) {
			obs_encoder_packet_release(&slot->packet);
			slot->magic = 0;
			pthread_mutex_unlock(&cache->mutex);
			return;
		}
	}

	cache->next_index++;

	if (cache_packet_count(cache) == 1) {
		cache->first_dts_usec = packet->dts_usec;
		cache->last_dts_usec = packet->dts_usec;
	} else if (packet->dts_usec > cache->last_dts_usec) {
		cache->last_dts_usec = packet->dts_usec;
	}

	cache->total_size += (int64_t)packet->size;
	cache_trim(cache);

	pthread_mutex_unlock(&cache->mutex);
}

bool replay_buffer_cache_get_time_range(replay_buffer_cache_t *cache, int64_t *start_usec, int64_t *end_usec)
{
	if (!cache || !start_usec || !end_usec)
		return false;

	pthread_mutex_lock(&cache->mutex);
	*start_usec = cache->first_dts_usec;
	*end_usec = cache->last_dts_usec;
	pthread_mutex_unlock(&cache->mutex);

	return true;
}

size_t replay_buffer_cache_get_keyframe_count(replay_buffer_cache_t *cache)
{
	if (!cache)
		return 0;

	pthread_mutex_lock(&cache->mutex);
	const size_t count = cache_gop_count(cache);
	pthread_mutex_unlock(&cache->mutex);

	return count;
}

bool replay_buffer_cache_get_keyframe_info(replay_buffer_cache_t *cache, size_t index,
					   struct replay_buffer_keyframe_info *info)
{
	if (!cache || !info)
		return false;

	memset(info, 0, sizeof(*info));

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	const size_t count = cache_gop_count(cache);
	if (index < count) {
		const struct replay_buffer_gop *gop = cache_gop_at(cache, index);
		info->index = gop->first_index;
		info->dts_usec = gop->start_dts_usec;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

/* Index of the first GOP whose keyframe is after time_usec (strict) or at/after
 * it, as an absolute position in the gops array. */
static size_t cache_gop_lower_bound(const replay_buffer_cache_t *cache, int64_t time_usec, bool strict)
{
	size_t low = cache->gops_head;
	size_t high = cache->gops.num;

	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		const int64_t dts_usec = cache->gops.array[middle].start_dts_usec;
		const bool before = strict ? dts_usec <= time_usec : dts_usec < time_usec;
		if (before)
			low = middle + 1;
		else
			high = middle;
	}

	return low;
}

/* Nearest GOP to time_usec, preferring the earlier keyframe on a tie: it is
 * never later than the requested time. */
static size_t cache_nearest_gop(const replay_buffer_cache_t *cache, int64_t time_usec)
{
	size_t low = cache_gop_lower_bound(cache, time_usec, false);

	if (low >= cache->gops.num)
		return cache->gops.num - 1;
	if (low == cache->gops_head)
		return low;

	const int64_t after = cache->gops.array[low].start_dts_usec - time_usec;
	const int64_t before = time_usec - cache->gops.array[low - 1].start_dts_usec;
	return before <= after ? low - 1 : low;
}

bool replay_buffer_cache_find_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (cache_gop_count(cache) > 0) {
		*index = cache_nearest_gop(cache, time_usec) - cache->gops_head;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_find_prev_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	const size_t low = cache_gop_lower_bound(cache, time_usec, false);
	if (low > cache->gops_head) {
		*index = low - 1 - cache->gops_head;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_find_next_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	const size_t low = cache_gop_lower_bound(cache, time_usec, true);
	if (low < cache->gops.num) {
		*index = low - cache->gops_head;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

/* Resolves a range while holding the lock. Returns absolute packet indices. */
static bool cache_resolve_range_locked(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       uint64_t *first_index, uint64_t *last_index, int64_t *actual_start_usec,
				       int64_t *actual_end_usec)
{
	if (cache_gop_count(cache) == 0 || end_usec <= start_usec)
		return false;

	/* Start from the GOP that covers the requested start. */
	const size_t after = cache_gop_lower_bound(cache, start_usec, true);
	const size_t low = after > cache->gops_head ? after - 1 : cache->gops_head;
	const struct replay_buffer_gop *gop = &cache->gops.array[low];

	const int64_t aligned_start = gop->start_dts_usec;
	if (aligned_start >= end_usec)
		return false;

	bool have_packet = false;
	int64_t actual_end = aligned_start;
	uint64_t last = gop->first_index;

	for (uint64_t index = gop->first_index; index < cache->next_index; index++) {
		struct replay_buffer_packet *slot = cache_live_slot(cache, index);
		if (!slot)
			break;
		if (slot->packet.dts_usec > end_usec)
			break;

		last = index;
		have_packet = true;
		if (slot->packet.dts_usec > actual_end)
			actual_end = slot->packet.dts_usec;
	}

	if (!have_packet || actual_end <= aligned_start)
		return false;

	*first_index = gop->first_index;
	*last_index = last;
	*actual_start_usec = aligned_start;
	*actual_end_usec = actual_end;
	return true;
}

bool replay_buffer_cache_resolve_range(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       int64_t *actual_start_usec, int64_t *actual_end_usec)
{
	if (!cache || !actual_start_usec || !actual_end_usec)
		return false;

	uint64_t first = 0;
	uint64_t last = 0;
	int64_t start = 0;
	int64_t end = 0;

	pthread_mutex_lock(&cache->mutex);
	const bool found = cache_resolve_range_locked(cache, start_usec, end_usec, &first, &last, &start, &end);
	pthread_mutex_unlock(&cache->mutex);

	*actual_start_usec = found ? start : 0;
	*actual_end_usec = found ? end : 0;
	return found;
}

replay_buffer_snapshot_t *replay_buffer_cache_create_range_snapshot(replay_buffer_cache_t *cache, int64_t start_usec,
								    int64_t end_usec, int64_t *actual_start_usec,
								    int64_t *actual_end_usec)
{
	if (actual_start_usec)
		*actual_start_usec = 0;
	if (actual_end_usec)
		*actual_end_usec = 0;
	if (!cache)
		return NULL;

	replay_buffer_snapshot_t *snapshot = bzalloc(sizeof(*snapshot));
	if (!snapshot)
		return NULL;

	uint64_t first = 0;
	uint64_t last = 0;
	int64_t start = 0;
	int64_t end = 0;

	pthread_mutex_lock(&cache->mutex);
	const bool found = cache_resolve_range_locked(cache, start_usec, end_usec, &first, &last, &start, &end);

	if (found) {
		for (uint64_t index = first; index <= last; index++) {
			struct replay_buffer_packet *slot = cache_live_slot(cache, index);
			if (!slot)
				break;

			struct encoder_packet ref;
			obs_encoder_packet_ref(&ref, &slot->packet);
			if (!da_push_back(snapshot->packets, &ref)) {
				obs_encoder_packet_release(&ref);
				pthread_mutex_unlock(&cache->mutex);
				replay_buffer_snapshot_release(snapshot);
				return NULL;
			}
		}
	}

	pthread_mutex_unlock(&cache->mutex);

	if (!found || !replay_buffer_snapshot_get_packet_count(snapshot)) {
		replay_buffer_snapshot_release(snapshot);
		return NULL;
	}

	if (actual_start_usec)
		*actual_start_usec = start;
	if (actual_end_usec)
		*actual_end_usec = end;

	return snapshot;
}

bool replay_buffer_cache_ref_first_keyframe(replay_buffer_cache_t *cache, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	const size_t count = cache_gop_count(cache);
	if (count > 0) {
		struct replay_buffer_packet *slot = cache_live_slot(cache, cache_gop_at(cache, 0)->first_index);
		if (slot) {
			obs_encoder_packet_ref(packet, &slot->packet);
			found = true;
		}
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_ref_keyframe_at(replay_buffer_cache_t *cache, int64_t time_usec, uint64_t *index,
					 struct encoder_packet *packet)
{
	if (!cache || !index || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (cache_gop_count(cache) > 0) {
		const size_t best = cache_nearest_gop(cache, time_usec);
		struct replay_buffer_packet *slot = cache_live_slot(cache, cache->gops.array[best].first_index);

		if (slot && slot->packet.type == OBS_ENCODER_VIDEO && slot->packet.keyframe) {
			obs_encoder_packet_ref(packet, &slot->packet);
			*index = slot->index;
			found = true;
		}
	}

	pthread_mutex_unlock(&cache->mutex);

	if (found) {
		blog(LOG_DEBUG, "[replay buffer] keyframe at index %llu (dts %.3fs, requested %.3fs)",
		     (unsigned long long)*index, (double)packet->dts_usec / 1000000.0, (double)time_usec / 1000000.0);
	}

	return found;
}

bool replay_buffer_cache_ref_packet_at(replay_buffer_cache_t *cache, uint64_t index, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	struct replay_buffer_packet *slot = cache_live_slot(cache, index);
	if (slot) {
		obs_encoder_packet_ref(packet, &slot->packet);
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

replay_buffer_snapshot_t *replay_buffer_cache_create_snapshot(replay_buffer_cache_t *cache)
{
	if (!cache)
		return NULL;

	replay_buffer_snapshot_t *snapshot = bzalloc(sizeof(*snapshot));
	if (!snapshot)
		return NULL;

	pthread_mutex_lock(&cache->mutex);
	da_reserve(snapshot->packets, cache_packet_count(cache));

	for (uint64_t index = cache->first_index; index < cache->next_index; index++) {
		struct replay_buffer_packet *slot = cache_live_slot(cache, index);
		if (!slot)
			continue;

		struct encoder_packet packet;
		obs_encoder_packet_ref(&packet, &slot->packet);
		if (!da_push_back(snapshot->packets, &packet)) {
			obs_encoder_packet_release(&packet);
			pthread_mutex_unlock(&cache->mutex);
			replay_buffer_snapshot_release(snapshot);
			return NULL;
		}
	}

	pthread_mutex_unlock(&cache->mutex);

	if (!replay_buffer_snapshot_get_packet_count(snapshot)) {
		replay_buffer_snapshot_release(snapshot);
		return NULL;
	}

	return snapshot;
}

void replay_buffer_snapshot_release(replay_buffer_snapshot_t *snapshot)
{
	if (!snapshot)
		return;

	for (size_t i = 0; i < snapshot->packets.num; i++)
		obs_encoder_packet_release(&snapshot->packets.array[i]);

	da_free(snapshot->packets);
	bfree(snapshot);
}

size_t replay_buffer_snapshot_get_packet_count(const replay_buffer_snapshot_t *snapshot)
{
	return snapshot ? snapshot->packets.num : 0;
}

struct encoder_packet *replay_buffer_snapshot_get_packet(const replay_buffer_snapshot_t *snapshot, size_t index)
{
	if (!snapshot || index >= snapshot->packets.num)
		return NULL;

	return &snapshot->packets.array[index];
}
