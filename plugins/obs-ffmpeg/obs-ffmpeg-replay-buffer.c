#include "obs-ffmpeg-replay-buffer.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/threading.h>

#include <string.h>

/* Smallest ring the cache allocates; it doubles from here as the buffer fills. */
#define REPLAY_BUFFER_MIN_SLOTS 1024

/* The packet store is a ring so eviction only moves the head pointer: the old
 * flat array had to memmove half of the buffer every time it was compacted. */
struct replay_buffer_packet {
	struct encoder_packet packet;
	/* Absolute index, stored with the packet so a lookup can verify that the
	 * slot it landed on is the one it asked for. */
	uint64_t index;
};

/* One GOP: a video keyframe and every packet that followed it until the next
 * keyframe. Packets of a GOP are contiguous in the ring, so a GOP is described
 * by where it starts and how long it is. */
struct replay_buffer_gop {
	int64_t start_dts_usec;
	uint64_t first_index;
	size_t packet_count;
};

struct replay_buffer_cache {
	pthread_mutex_t mutex;

	struct replay_buffer_packet *slots;
	size_t capacity;
	size_t head;
	size_t count;

	DARRAY(struct replay_buffer_gop) gops;
	size_t gops_head;

	uint64_t first_index;
	uint64_t next_index;
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

static size_t cache_gop_count(const replay_buffer_cache_t *cache)
{
	return cache->gops.num - cache->gops_head;
}

static struct replay_buffer_gop *cache_gop_at(const replay_buffer_cache_t *cache, size_t index)
{
	return &cache->gops.array[cache->gops_head + index];
}

static struct replay_buffer_packet *cache_slot_at(const replay_buffer_cache_t *cache, size_t offset)
{
	return &cache->slots[(cache->head + offset) % cache->capacity];
}

/* Maps an absolute index to its slot and checks the slot really holds it. */
static struct replay_buffer_packet *cache_slot_for_index(const replay_buffer_cache_t *cache, uint64_t index)
{
	if (index < cache->first_index)
		return NULL;

	const uint64_t offset = index - cache->first_index;
	if (offset >= cache->count)
		return NULL;

	struct replay_buffer_packet *slot = cache_slot_at(cache, (size_t)offset);
	return slot->index == index ? slot : NULL;
}

static bool cache_reserve_slots(replay_buffer_cache_t *cache, size_t needed)
{
	if (needed <= cache->capacity)
		return true;

	size_t capacity = cache->capacity ? cache->capacity : REPLAY_BUFFER_MIN_SLOTS;
	while (capacity < needed)
		capacity *= 2;

	struct replay_buffer_packet *slots = bmalloc(sizeof(*slots) * capacity);
	if (!slots)
		return false;

	for (size_t i = 0; i < cache->count; i++)
		slots[i] = *cache_slot_at(cache, i);

	bfree(cache->slots);
	cache->slots = slots;
	cache->capacity = capacity;
	cache->head = 0;
	return true;
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

static void cache_drop_front_gop(replay_buffer_cache_t *cache)
{
	struct replay_buffer_gop *gop = cache_gop_at(cache, 0);

	for (size_t i = 0; i < gop->packet_count && cache->count; i++) {
		struct replay_buffer_packet *slot = cache_slot_at(cache, 0);
		cache->total_size -= (int64_t)slot->packet.size;
		obs_encoder_packet_release(&slot->packet);
		cache->head = (cache->head + 1) % cache->capacity;
		cache->count--;
		cache->first_index++;
	}

	cache->gops_head++;
	cache_compact_gops(cache);

	if (!cache->count) {
		cache->first_dts_usec = 0;
		cache->last_dts_usec = 0;
		cache->total_size = 0;
	} else {
		cache->first_dts_usec = cache_slot_at(cache, 0)->packet.dts_usec;
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
	bfree(cache->slots);
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

	for (size_t i = 0; i < cache->count; i++)
		obs_encoder_packet_release(&cache_slot_at(cache, i)->packet);

	cache->head = 0;
	cache->count = 0;
	da_clear(cache->gops);
	cache->gops_head = 0;

	/* next_index is intentionally not reset: stale asynchronous frame requests
	 * must not match packets from a later session. */
	cache->first_index = cache->next_index;
	cache->first_dts_usec = 0;
	cache->last_dts_usec = 0;
	cache->total_size = 0;

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

	if (!cache_reserve_slots(cache, cache->count + 1)) {
		pthread_mutex_unlock(&cache->mutex);
		return;
	}

	if (starts_gop) {
		struct replay_buffer_gop gop = {0};
		gop.start_dts_usec = packet->dts_usec;
		gop.first_index = cache->next_index;
		gop.packet_count = 1;

		if (!da_push_back(cache->gops, &gop)) {
			pthread_mutex_unlock(&cache->mutex);
			return;
		}
	} else {
		cache_gop_at(cache, cache_gop_count(cache) - 1)->packet_count++;
	}

	struct replay_buffer_packet *slot = cache_slot_at(cache, cache->count);
	obs_encoder_packet_ref(&slot->packet, packet);
	slot->index = cache->next_index;
	cache->count++;
	cache->next_index++;

	if (cache->count == 1) {
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

		if (index + 1 < count) {
			info->duration_usec = cache_gop_at(cache, index + 1)->start_dts_usec - gop->start_dts_usec;
		} else if (cache->last_dts_usec > gop->start_dts_usec) {
			info->duration_usec = cache->last_dts_usec - gop->start_dts_usec;
		}

		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

/* Index of the last GOP whose keyframe is at or before time_usec. */
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

bool replay_buffer_cache_find_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (cache_gop_count(cache) > 0) {
		size_t low = cache_gop_lower_bound(cache, time_usec, false);
		size_t best;
		if (low >= cache->gops.num) {
			best = cache->gops.num - 1;
		} else if (low == cache->gops_head) {
			best = low;
		} else {
			const int64_t after = cache->gops.array[low].start_dts_usec - time_usec;
			const int64_t before = time_usec - cache->gops.array[low - 1].start_dts_usec;
			/* Prefer the earlier keyframe on a tie: it is never later than
			 * the requested time. */
			best = before <= after ? low - 1 : low;
		}

		*index = best - cache->gops_head;
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

	const size_t after = cache_gop_lower_bound(cache, start_usec, true);
	const size_t low = after > cache->gops_head ? after - 1 : cache->gops_head;

	const struct replay_buffer_gop *gop = &cache->gops.array[low];
	const int64_t aligned_start = gop->start_dts_usec;
	if (aligned_start >= end_usec)
		return false;

	bool have_packet = false;
	int64_t actual_end = aligned_start;
	uint64_t last = gop->first_index;

	for (uint64_t index = gop->first_index;; index++) {
		const struct replay_buffer_packet *slot = cache_slot_for_index(cache, index);
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
			const struct replay_buffer_packet *slot = cache_slot_for_index(cache, index);
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

	if (cache_gop_count(cache) > 0) {
		const struct replay_buffer_packet *slot =
			cache_slot_for_index(cache, cache_gop_at(cache, 0)->first_index);
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

	const size_t count = cache_gop_count(cache);
	if (count > 0) {
		size_t low = cache_gop_lower_bound(cache, time_usec, false);
		size_t best;
		if (low >= cache->gops.num) {
			best = cache->gops.num - 1;
		} else if (low == cache->gops_head) {
			best = low;
		} else {
			const int64_t after = cache->gops.array[low].start_dts_usec - time_usec;
			const int64_t before = time_usec - cache->gops.array[low - 1].start_dts_usec;
			best = before <= after ? low - 1 : low;
		}

		const struct replay_buffer_packet *slot =
			cache_slot_for_index(cache, cache->gops.array[best].first_index);
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

	const struct replay_buffer_packet *slot = cache_slot_for_index(cache, index);
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
	da_reserve(snapshot->packets, cache->count);

	for (size_t i = 0; i < cache->count; i++) {
		struct encoder_packet packet;
		obs_encoder_packet_ref(&packet, &cache_slot_at(cache, i)->packet);
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
