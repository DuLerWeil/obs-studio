#pragma once

#include <obs.h>

/* Replay buffer packet cache with a keyframe-level time index.
 *
 * The cache owns references to the encoded packets (via
 * obs_encoder_packet_ref) and can be read concurrently from a UI thread while
 * the encoder thread keeps appending. Only keyframes of one video track are
 * indexed; the replay buffer output currently configures a single video
 * encoder, so track 0 is used by default. */

struct replay_buffer_keyframe_info {
	uint64_t seq;
	int64_t dts_usec;
	int64_t duration_usec;
};

typedef struct replay_buffer_cache replay_buffer_cache_t;
typedef struct replay_buffer_snapshot replay_buffer_snapshot_t;

/* Creates an empty cache. Returns NULL on allocation failure. */
replay_buffer_cache_t *replay_buffer_cache_create(size_t video_track_idx);
void replay_buffer_cache_destroy(replay_buffer_cache_t *cache);

/* Updates the ring limits. Called when the output starts. */
void replay_buffer_cache_set_limits(replay_buffer_cache_t *cache, int64_t max_size_bytes, int64_t max_time_usec);

/* Drops all packets and keyframes, keeping the configured limits. */
void replay_buffer_cache_clear(replay_buffer_cache_t *cache);

/* Called on the encoder/output thread. Takes its own packet reference. */
void replay_buffer_cache_push(replay_buffer_cache_t *cache, struct encoder_packet *packet);

/* Thread-safe timeline queries. Times are absolute microseconds using the same
 * timebase as encoder_packet.dts_usec. */
bool replay_buffer_cache_get_time_range(replay_buffer_cache_t *cache, int64_t *start_usec, int64_t *end_usec);
size_t replay_buffer_cache_get_keyframe_count(replay_buffer_cache_t *cache);
bool replay_buffer_cache_get_keyframe_info(replay_buffer_cache_t *cache, size_t index,
					   struct replay_buffer_keyframe_info *info);
bool replay_buffer_cache_find_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index);
/* Index of the last keyframe strictly before time_usec. Used by the timeline's
 * previous-keyframe action, which must move even when the playhead is already
 * parked on a keyframe and the nearest-keyframe lookup would return it. */
bool replay_buffer_cache_find_prev_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index);
/* Index of the first keyframe strictly after time_usec. */
bool replay_buffer_cache_find_next_keyframe(replay_buffer_cache_t *cache, int64_t time_usec, size_t *index);
/* Resolves a requested interval to a decodable range. The start is snapped
 * back to the last keyframe at or before start_usec; the end is clamped to the
 * cache window and truncated to the last packet whose dts_usec is <= end_usec.
 * Returns false when the interval is empty or no keyframe covers it. */
bool replay_buffer_cache_resolve_range(replay_buffer_cache_t *cache, int64_t start_usec, int64_t end_usec,
				       int64_t *actual_start_usec, int64_t *actual_end_usec);
/* Same resolution as above, but also takes packet references for the interval
 * so the data stays valid while the cache keeps rolling. The returned snapshot
 * starts with the aligned video keyframe. */
replay_buffer_snapshot_t *replay_buffer_cache_create_range_snapshot(replay_buffer_cache_t *cache, int64_t start_usec,
								    int64_t end_usec, int64_t *actual_start_usec,
								    int64_t *actual_end_usec);

/* Takes a reference to the keyframe packet identified by seq. The caller must
 * call obs_encoder_packet_release() when done. Returns false if the keyframe
 * has already been evicted. */
bool replay_buffer_cache_ref_keyframe_packet(replay_buffer_cache_t *cache, uint64_t seq, struct encoder_packet *packet);

/* Refs the packet with that sequence number, keyframe or not. The caller releases it. */
bool replay_buffer_cache_ref_packet(replay_buffer_cache_t *cache, uint64_t seq, struct encoder_packet *packet);
/* Refs the oldest video keyframe still in the cache, which is where the codec
 * parameter sets live: encoders only repeat them at the very start of the
 * session, so later keyframes cannot be decoded on their own. */
bool replay_buffer_cache_ref_first_keyframe(replay_buffer_cache_t *cache, struct encoder_packet *packet);

/* Whole-buffer snapshot used by the save path. */
replay_buffer_snapshot_t *replay_buffer_cache_create_snapshot(replay_buffer_cache_t *cache);
void replay_buffer_snapshot_release(replay_buffer_snapshot_t *snapshot);
size_t replay_buffer_snapshot_get_packet_count(const replay_buffer_snapshot_t *snapshot);
struct encoder_packet *replay_buffer_snapshot_get_packet(const replay_buffer_snapshot_t *snapshot, size_t index);
