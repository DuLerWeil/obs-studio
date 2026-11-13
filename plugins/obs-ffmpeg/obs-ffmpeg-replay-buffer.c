#include "obs-ffmpeg-replay-buffer.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/threading.h>

#include <stdint.h>
#include <string.h>

/* Keeps a bounded window of encoded packets and a keyframe-only index. The
 * index is what makes timeline scrubbing cheap: a time lookup is a binary
 * search over keyframes, and the packet for a keyframe is found by its stable
 * sequence number. */

#define REPLAY_BUFFER_PACKET_COMPACT_THRESHOLD 256
#define REPLAY_BUFFER_KEYFRAME_COMPACT_THRESHOLD 128

struct replay_buffer_keyframe {
	uint64_t seq;
	int64_t dts_usec;
};

struct replay_buffer_cache {
	pthread_mutex_t mutex;

	DARRAY(struct encoder_packet) packets;
	size_t packets_head;

	DARRAY(struct replay_buffer_keyframe) keyframes;
	size_t keyframes_head;

	uint64_t first_seq;
	uint64_t next_seq;
	int64_t first_dts_usec;
	int64_t last_dts_usec;
	int64_t total_size;
	int64_t max_size_bytes;
	int64_t max_time_usec;

	size_t video_track_idx;
	int video_keyframe_count;
};

struct replay_buffer_snapshot {
	DARRAY(struct encoder_packet) packets;
};

static size_t cache_packet_count(const replay_buffer_cache_t *cache)
{
	return cache->packets.num - cache->packets_head;
}

static size_t cache_keyframe_count(const replay_buffer_cache_t *cache)
{
	return cache->keyframes.num - cache->keyframes_head;
}

static struct replay_buffer_keyframe *cache_keyframe_at(replay_buffer_cache_t *cache, size_t index)
{
	return &cache->keyframes.array[cache->keyframes_head + index];
}

static void cache_compact_packets(replay_buffer_cache_t *cache)
{
	if (!cache->packets_head)
		return;
	if (cache->packets_head < REPLAY_BUFFER_PACKET_COMPACT_THRESHOLD &&
	    cache->packets_head * 2 < cache->packets.num)
		return;

	const size_t remaining = cache->packets.num - cache->packets_head;
	if (remaining) {
		memmove(cache->packets.array, cache->packets.array + cache->packets_head,
			remaining * sizeof(*cache->packets.array));
	}

	cache->packets.num = remaining;
	cache->packets_head = 0;
}

static void cache_compact_keyframes(replay_buffer_cache_t *cache)
{
	if (!cache->keyframes_head)
		return;
	if (cache->keyframes_head < REPLAY_BUFFER_KEYFRAME_COMPACT_THRESHOLD &&
	    cache->keyframes_head * 2 < cache->keyframes.num)
		return;

	const size_t remaining = cache->keyframes.num - cache->keyframes_head;
	if (remaining) {
		memmove(cache->keyframes.array, cache->keyframes.array + cache->keyframes_head,
			remaining * sizeof(*cache->keyframes.array));
	}

	cache->keyframes.num = remaining;
	cache->keyframes_head = 0;
}

static void cache_drop_stale_keyframes(replay_buffer_cache_t *cache)
{
	bool removed = false;

	while (cache_keyframe_count(cache) > 0) {
		struct replay_buffer_keyframe *keyframe = &cache->keyframes.array[cache->keyframes_head];
		if (keyframe->seq >= cache->first_seq)
			break;

		cache->keyframes_head++;
		removed = true;
	}

	if (removed)
		cache_compact_keyframes(cache);
}

static bool cache_can_purge(const replay_buffer_cache_t *cache)
{
	return cache_packet_count(cache) > 0 && cache->video_keyframe_count > 2;
}

static bool cache_purge_front(replay_buffer_cache_t *cache)
{
	if (!cache_packet_count(cache))
		return false;

	struct encoder_packet *packet = &cache->packets.array[cache->packets_head];
	const bool was_video_keyframe = packet->type == OBS_ENCODER_VIDEO && packet->keyframe;
	const int64_t packet_size = (int64_t)packet->size;

	if (was_video_keyframe && cache->video_keyframe_count > 0)
		cache->video_keyframe_count--;

	obs_encoder_packet_release(packet);
	cache->packets_head++;
	cache->first_seq++;
	cache->total_size -= packet_size;
	if (cache->total_size < 0)
		cache->total_size = 0;

	cache_drop_stale_keyframes(cache);

	if (!cache_packet_count(cache)) {
		cache->packets.num = 0;
		cache->packets_head = 0;
		cache->first_dts_usec = 0;
		cache->last_dts_usec = 0;
		cache->total_size = 0;
	} else {
		cache->first_dts_usec = cache->packets.array[cache->packets_head].dts_usec;
	}

	return was_video_keyframe;
}

/* Drops one whole GOP so that the buffer always starts at a video keyframe. */
static void cache_purge_gop(replay_buffer_cache_t *cache)
{
	if (!cache_purge_front(cache))
		return;

	while (cache_packet_count(cache) > 0) {
		struct encoder_packet *packet = &cache->packets.array[cache->packets_head];
		if (packet->type == OBS_ENCODER_VIDEO && packet->keyframe)
			break;

		cache_purge_front(cache);
	}

	cache_compact_packets(cache);
}

static struct replay_buffer_keyframe *cache_find_keyframe_by_seq(replay_buffer_cache_t *cache, uint64_t seq)
{
	size_t low = cache->keyframes_head;
	size_t high = cache->keyframes.num;

	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		if (cache->keyframes.array[middle].seq < seq)
			low = middle + 1;
		else
			high = middle;
	}

	if (low >= cache->keyframes.num)
		return NULL;

	struct replay_buffer_keyframe *keyframe = &cache->keyframes.array[low];
	return keyframe->seq == seq ? keyframe : NULL;
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
	da_free(cache->packets);
	da_free(cache->keyframes);
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

	for (size_t i = cache->packets_head; i < cache->packets.num; i++)
		obs_encoder_packet_release(&cache->packets.array[i]);
	da_clear(cache->packets);
	cache->packets_head = 0;

	da_clear(cache->keyframes);
	cache->keyframes_head = 0;

	/* next_seq is intentionally not reset: stale asynchronous frame requests
	 * must not match keyframes from a later session. */
	cache->first_seq = cache->next_seq;
	cache->first_dts_usec = 0;
	cache->last_dts_usec = 0;
	cache->total_size = 0;
	cache->video_keyframe_count = 0;

	pthread_mutex_unlock(&cache->mutex);
}

void replay_buffer_cache_push(replay_buffer_cache_t *cache, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return;

	struct encoder_packet stored;
	obs_encoder_packet_ref(&stored, packet);

	pthread_mutex_lock(&cache->mutex);

	bool may_purge = true;
	if (cache->max_size_bytes > 0) {
		may_purge = cache_can_purge(cache);
		if (may_purge) {
			while (cache_can_purge(cache) &&
			       cache->total_size + (int64_t)packet->size > cache->max_size_bytes)
				cache_purge_gop(cache);

			may_purge = cache_can_purge(cache);
		}
	}

	/* A zero time limit means unlimited, matching max_size_bytes. Without the
	 * guard every push would purge down to the two-GOP floor. */
	if (may_purge && cache->max_time_usec > 0) {
		while (cache_can_purge(cache) && (packet->dts_usec - cache->first_dts_usec) > cache->max_time_usec)
			cache_purge_gop(cache);
	}

	if (!da_push_back(cache->packets, &stored)) {
		/* Out of memory: drop the reference instead of leaking it, and leave
		 * next_seq alone so it keeps matching the packet index. */
		obs_encoder_packet_release(&stored);
		pthread_mutex_unlock(&cache->mutex);
		return;
	}

	const uint64_t seq = cache->next_seq++;

	if (cache_packet_count(cache) == 1) {
		cache->first_dts_usec = packet->dts_usec;
		cache->last_dts_usec = packet->dts_usec;
	} else if (packet->dts_usec > cache->last_dts_usec) {
		cache->last_dts_usec = packet->dts_usec;
	}

	cache->total_size += (int64_t)packet->size;

	if (packet->type == OBS_ENCODER_VIDEO && packet->keyframe) {
		cache->video_keyframe_count++;

		if (packet->track_idx == cache->video_track_idx) {
			struct replay_buffer_keyframe keyframe = {0};
			keyframe.seq = seq;
			keyframe.dts_usec = packet->dts_usec;
			da_push_back(cache->keyframes, &keyframe);
		}
	}

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
	const size_t count = cache_keyframe_count(cache);
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

	const size_t count = cache_keyframe_count(cache);
	if (index < count) {
		struct replay_buffer_keyframe *keyframe = cache_keyframe_at(cache, index);
		info->seq = keyframe->seq;
		info->dts_usec = keyframe->dts_usec;

		if (index + 1 < count) {
			info->duration_usec = cache_keyframe_at(cache, index + 1)->dts_usec - keyframe->dts_usec;
		} else if (cache->last_dts_usec > keyframe->dts_usec) {
			info->duration_usec = cache->last_dts_usec - keyframe->dts_usec;
		}

		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_find_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (cache_keyframe_count(cache) > 0) {
		size_t low = cache->keyframes_head;
		size_t high = cache->keyframes.num;

		while (low < high) {
			const size_t middle = low + (high - low) / 2;
			if (cache->keyframes.array[middle].dts_usec < time_usec)
				low = middle + 1;
			else
				high = middle;
		}

		size_t best = 0;
		if (low >= cache->keyframes.num) {
			best = cache->keyframes.num - 1;
		} else if (low == cache->keyframes_head) {
			best = low;
		} else {
			const int64_t after = cache->keyframes.array[low].dts_usec - time_usec;
			const int64_t before = time_usec - cache->keyframes.array[low - 1].dts_usec;
			/* Prefer the earlier keyframe on a tie: it is never later
			 * than the requested time. */
			best = before <= after ? low - 1 : low;
		}

		*index = best - cache->keyframes_head;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

/* First indexed keyframe whose dts is >= (strict == false) or > (strict ==
 * true) time_usec. Returned as an absolute array index. */
static size_t cache_keyframe_lower_bound(const replay_buffer_cache_t *cache, int64_t time_usec, bool strict)
{
	size_t low = cache->keyframes_head;
	size_t high = cache->keyframes.num;

	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		const int64_t dts_usec = cache->keyframes.array[middle].dts_usec;
		const bool before = strict ? dts_usec <= time_usec : dts_usec < time_usec;
		if (before)
			low = middle + 1;
		else
			high = middle;
	}

	return low;
}

bool replay_buffer_cache_find_prev_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index)
{
	if (!cache || !index)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	const size_t low = cache_keyframe_lower_bound(cache, time_usec, false);
	if (low > cache->keyframes_head) {
		*index = low - 1 - cache->keyframes_head;
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

	const size_t low = cache_keyframe_lower_bound(cache, time_usec, true);
	if (low < cache->keyframes.num) {
		*index = low - cache->keyframes_head;
		found = true;
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

struct replay_buffer_range {
	size_t first_packet;
	int64_t start_usec;
	int64_t end_usec;
};

static bool cache_packet_in_range(const struct encoder_packet *packet, int64_t start_usec, int64_t end_usec)
{
	if (packet->dts_usec > end_usec)
		return false;
	/* Audio can arrive slightly ahead of the video keyframe. Do not start
	 * the export before the aligned keyframe. */
	if (packet->type != OBS_ENCODER_VIDEO && packet->dts_usec < start_usec)
		return false;
	return true;
}

static bool cache_resolve_range_locked(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       struct replay_buffer_range *range)
{
	if (cache_keyframe_count(cache) == 0 || cache_packet_count(cache) == 0)
		return false;
	if (start_usec >= end_usec)
		return false;
	if (end_usec <= cache->first_dts_usec || start_usec >= cache->last_dts_usec)
		return false;

	if (start_usec < cache->first_dts_usec)
		start_usec = cache->first_dts_usec;
	if (end_usec > cache->last_dts_usec)
		end_usec = cache->last_dts_usec;

	/* Find the last keyframe at or before the requested start. */
	size_t low = cache->keyframes_head;
	size_t high = cache->keyframes.num;
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		if (cache->keyframes.array[middle].dts_usec <= start_usec)
			low = middle + 1;
		else
			high = middle;
	}

	const size_t key_index = low == cache->keyframes_head ? cache->keyframes_head : low - 1;
	const struct replay_buffer_keyframe *keyframe = &cache->keyframes.array[key_index];
	const int64_t aligned_start = keyframe->dts_usec;
	if (aligned_start >= end_usec)
		return false;
	if (keyframe->seq < cache->first_seq)
		return false;

	const size_t first_packet = cache->packets_head + (size_t)(keyframe->seq - cache->first_seq);
	if (first_packet >= cache->packets.num)
		return false;

	int64_t actual_end = aligned_start;
	for (size_t i = first_packet; i < cache->packets.num; i++) {
		const struct encoder_packet *packet = &cache->packets.array[i];
		if (!cache_packet_in_range(packet, aligned_start, end_usec))
			continue;
		if (packet->dts_usec > actual_end)
			actual_end = packet->dts_usec;
	}

	if (actual_end <= aligned_start)
		return false;

	range->first_packet = first_packet;
	range->start_usec = aligned_start;
	range->end_usec = actual_end;
	return true;
}

bool replay_buffer_cache_resolve_range(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       int64_t *actual_start_usec, int64_t *actual_end_usec)
{
	if (!cache || !actual_start_usec || !actual_end_usec)
		return false;

	struct replay_buffer_range range = {0};
	pthread_mutex_lock(&cache->mutex);
	const bool found = cache_resolve_range_locked(cache, start_usec, end_usec, &range);
	pthread_mutex_unlock(&cache->mutex);

	if (found) {
		*actual_start_usec = range.start_usec;
		*actual_end_usec = range.end_usec;
	} else {
		*actual_start_usec = 0;
		*actual_end_usec = 0;
	}

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

	struct replay_buffer_range range = {0};
	pthread_mutex_lock(&cache->mutex);

	const bool found = cache_resolve_range_locked(cache, start_usec, end_usec, &range);
	if (found) {
		for (size_t i = range.first_packet; i < cache->packets.num; i++) {
			struct encoder_packet *packet = &cache->packets.array[i];
			if (!cache_packet_in_range(packet, range.start_usec, range.end_usec))
				continue;

			struct encoder_packet ref;
			obs_encoder_packet_ref(&ref, packet);
			if (!da_push_back(snapshot->packets, &ref)) {
				obs_encoder_packet_release(&ref);
				pthread_mutex_unlock(&cache->mutex);
				replay_buffer_snapshot_release(snapshot);
				return NULL;
			}
		}
	}

	pthread_mutex_unlock(&cache->mutex);

	if (!found) {
		bfree(snapshot);
		return NULL;
	}

	if (actual_start_usec)
		*actual_start_usec = range.start_usec;
	if (actual_end_usec)
		*actual_end_usec = range.end_usec;

	return snapshot;
}

bool replay_buffer_cache_ref_keyframe_packet(replay_buffer_cache_t *cache, uint64_t seq, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	struct replay_buffer_keyframe *keyframe = cache_find_keyframe_by_seq(cache, seq);
	if (keyframe && seq >= cache->first_seq) {
		const uint64_t packet_index = seq - cache->first_seq;
		if (packet_index < cache_packet_count(cache)) {
			obs_encoder_packet_ref(packet,
					       &cache->packets.array[cache->packets_head + (size_t)packet_index]);
			found = true;
		}
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_ref_first_keyframe(replay_buffer_cache_t *cache, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (cache_keyframe_count(cache) > 0) {
		const uint64_t seq = cache_keyframe_at(cache, 0)->seq;
		if (seq >= cache->first_seq) {
			const uint64_t packet_index = seq - cache->first_seq;
			if (packet_index < cache_packet_count(cache)) {
				obs_encoder_packet_ref(packet, &cache->packets.array[cache->packets_head + (size_t)packet_index]);
				found = true;
			}
		}
	}

	pthread_mutex_unlock(&cache->mutex);
	return found;
}

bool replay_buffer_cache_ref_packet(replay_buffer_cache_t *cache, uint64_t seq, struct encoder_packet *packet)
{
	if (!cache || !packet)
		return false;

	bool found = false;
	pthread_mutex_lock(&cache->mutex);

	if (seq >= cache->first_seq) {
		const uint64_t packet_index = seq - cache->first_seq;
		if (packet_index < cache_packet_count(cache)) {
			obs_encoder_packet_ref(packet,
					       &cache->packets.array[cache->packets_head + (size_t)packet_index]);
			found = true;
		}
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

	const size_t count = cache_packet_count(cache);
	da_reserve(snapshot->packets, count);

	for (size_t i = 0; i < count; i++) {
		struct encoder_packet packet;
		obs_encoder_packet_ref(&packet, &cache->packets.array[cache->packets_head + i]);
		if (!da_push_back(snapshot->packets, &packet)) {
			/* Out of memory: release what was taken and report failure
			 * instead of handing out a truncated snapshot. */
			obs_encoder_packet_release(&packet);
			pthread_mutex_unlock(&cache->mutex);
			replay_buffer_snapshot_release(snapshot);
			return NULL;
		}
	}

	pthread_mutex_unlock(&cache->mutex);
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
