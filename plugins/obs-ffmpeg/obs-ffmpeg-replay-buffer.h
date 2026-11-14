#pragma once

#include <obs.h>

/* Replay buffer packet cache, organised as a run of GOPs.
 *
 * The buffer can only ever be cut at a keyframe, so the GOP is the natural unit
 * of storage: a packet belongs to the GOP whose keyframe it follows, a packet is
 * addressed by its absolute index (stored with the packet, so a lookup can check
 * it landed where it expected to), and dropping history releases the GOP's
 * packets without shifting the ones that remain.
 *
 * The cache owns references to the encoded packets (via
 * obs_encoder_packet_ref) and can be read concurrently from a UI thread while
 * the encoder thread keeps appending. Only one video track is indexed; the
 * replay buffer output currently configures a single video encoder, so track 0
 * is used by default.
 *
 * Limits are soft by design:
 *   - max_size_bytes / max_time_usec <= 0 means "no limit";
 *   - at least two GOPs are always retained, so the buffered window can exceed
 *     the configured limits by up to two GOPs;
 *   - max_size_bytes counts packet payload only, not the per-slot and per-block
 *     bookkeeping.
 */

struct replay_buffer_keyframe_info {
	/* Absolute packet index of the keyframe, usable with
	 * replay_buffer_cache_ref_packet_at(). */
	uint64_t packet_index;
	int64_t dts_usec;
	/* Time until the next keyframe (or the newest packet for the last GOP).
	 * Never negative. */
	int64_t duration_usec;
};

/* One consistent snapshot of the cache counters. All fields are read under the
 * cache lock, so they describe the same instant. */
struct replay_buffer_cache_info {
	int64_t start_usec;
	int64_t end_usec;
	int64_t max_size_bytes;
	int64_t max_time_usec;
	/* Buffered packet payload, matching total_size in the cache. */
	int64_t bytes;
	size_t keyframe_count;
	size_t packet_count;
	/* Packets that were not stored: before the first keyframe, after a
	 * failed insertion while waiting for a new keyframe, etc. */
	uint64_t dropped_packets;
	uint64_t dropped_bytes;
	uint64_t dropped_keyframes;
	/* Insertion failures (allocation failure), a subset of the drops above. */
	uint64_t push_failures;
};

typedef struct replay_buffer_cache replay_buffer_cache_t;
typedef struct replay_buffer_snapshot replay_buffer_snapshot_t;

/* Creates an empty cache. Returns NULL on allocation failure. */
replay_buffer_cache_t *replay_buffer_cache_create(size_t video_track_idx);
void replay_buffer_cache_destroy(replay_buffer_cache_t *cache);

/* Updates the limits. Called when the output starts. <= 0 disables the
 * corresponding limit. */
void replay_buffer_cache_set_limits(replay_buffer_cache_t *cache, int64_t max_size_bytes, int64_t max_time_usec);

/* Drops all packets and keyframes, keeping the configured limits. Also resets
 * the drop counters and waits for a new video keyframe again. */
void replay_buffer_cache_clear(replay_buffer_cache_t *cache);

/* Called on the encoder/output thread. Takes its own packet reference.
 *
 * Returns false when the packet was not stored. Packets that arrive before the
 * first video keyframe are dropped, because there is no GOP to attach them to
 * yet. After an insertion failure the cache drops the rest of the current GOP
 * and resumes at the next video keyframe, so a partially written GOP is never
 * exported. */
bool replay_buffer_cache_push(replay_buffer_cache_t *cache, struct encoder_packet *packet);

/* Thread-safe timeline queries. Times are absolute microseconds using the same
 * timebase as encoder_packet.dts_usec. */
void replay_buffer_cache_get_info(replay_buffer_cache_t *cache, struct replay_buffer_cache_info *info);
/* Convenience wrappers; use get_info() when several counters must agree. */
bool replay_buffer_cache_get_time_range(replay_buffer_cache_t *cache, int64_t *start_usec, int64_t *end_usec);
size_t replay_buffer_cache_get_keyframe_count(replay_buffer_cache_t *cache);
/* Keyframe ordinals are relative to the oldest GOP still buffered. */
bool replay_buffer_cache_get_keyframe_info(replay_buffer_cache_t *cache, size_t ordinal,
					   struct replay_buffer_keyframe_info *info);
bool replay_buffer_cache_find_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *ordinal);
/* Ordinal of the last keyframe strictly before time_usec. Used by the timeline's
 * previous-keyframe action, which must move even when the playhead is already
 * parked on a keyframe and the nearest-keyframe lookup would return it. */
bool replay_buffer_cache_find_prev_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *ordinal);
/* Ordinal of the first keyframe strictly after time_usec. */
bool replay_buffer_cache_find_next_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *ordinal);
/* Resolves a requested interval to a decodable range. The start is snapped back
 * to the last keyframe at or before start_usec; the end is truncated to the last
 * packet whose dts_usec is <= end_usec. Returns false when the interval is empty
 * or no keyframe covers it. */
bool replay_buffer_cache_resolve_range(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       int64_t *actual_start_usec, int64_t *actual_end_usec);
/* Same resolution as above, but also takes packet references for the interval
 * so the data stays valid while the cache keeps rolling. The returned snapshot
 * starts with the aligned video keyframe. */
replay_buffer_snapshot_t *replay_buffer_cache_create_range_snapshot(replay_buffer_cache_t *cache, int64_t start_usec,
								    int64_t end_usec, int64_t *actual_start_usec,
								    int64_t *actual_end_usec);

/* Refs the oldest video keyframe still buffered, which is where the codec
 * parameter sets live: encoders only repeat them at the very start of the
 * session, so later keyframes cannot be decoded on their own. */
bool replay_buffer_cache_ref_first_keyframe(replay_buffer_cache_t *cache, struct encoder_packet *packet);

/* Refs the video keyframe closest to time_usec and reports its absolute packet
 * index, so the caller can walk the packets that follow it. */
bool replay_buffer_cache_ref_keyframe_at(replay_buffer_cache_t *cache, int64_t time_usec, uint64_t *packet_index,
					 struct encoder_packet *packet);

/* Refs the packet at that absolute index. Returns false when the index has been
 * evicted or does not match the slot it maps to. */
bool replay_buffer_cache_ref_packet_at(replay_buffer_cache_t *cache, uint64_t packet_index,
				       struct encoder_packet *packet);

/* Whole-buffer snapshot used by the save path. */
replay_buffer_snapshot_t *replay_buffer_cache_create_snapshot(replay_buffer_cache_t *cache);
void replay_buffer_snapshot_release(replay_buffer_snapshot_t *snapshot);
size_t replay_buffer_snapshot_get_packet_count(const replay_buffer_snapshot_t *snapshot);
struct encoder_packet *replay_buffer_snapshot_get_packet(const replay_buffer_snapshot_t *snapshot, size_t index);
