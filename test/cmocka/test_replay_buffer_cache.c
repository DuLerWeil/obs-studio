#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

#include <cmocka.h>

#include "obs-ffmpeg-replay-buffer.h"

#include <obs.h>
#include <util/bmem.h>

static void packet_init(struct encoder_packet *packet, enum obs_encoder_type type, bool keyframe, int64_t dts_usec,
			size_t size, size_t track_idx, bool allocate_data)
{
	memset(packet, 0, sizeof(*packet));
	packet->type = type;
	packet->keyframe = keyframe;
	packet->dts_usec = dts_usec;
	packet->sys_dts_usec = dts_usec;
	packet->size = size;
	packet->track_idx = track_idx;
	packet->timebase_den = 1000000;

	if (allocate_data) {
		long *refs = bmalloc(size + sizeof(long));
		*refs = 1;
		packet->data = (uint8_t *)(refs + 1);
	}
}

static bool push_packet(replay_buffer_cache_t *cache, enum obs_encoder_type type, bool keyframe, int64_t dts_usec,
			size_t size, bool allocate_data)
{
	struct encoder_packet packet;
	packet_init(&packet, type, keyframe, dts_usec, size, 0, allocate_data);
	const bool stored = replay_buffer_cache_push(cache, &packet);
	obs_encoder_packet_release(&packet);
	return stored;
}

static void test_push_index_and_range(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60 * 1000000LL);

	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 100, false));
	assert_true(push_packet(cache, OBS_ENCODER_AUDIO, false, 100000, 10, false));
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, false, 200000, 20, false));
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 110, false));
	assert_true(push_packet(cache, OBS_ENCODER_AUDIO, false, 1100000, 10, false));

	assert_int_equal(replay_buffer_cache_get_keyframe_count(cache), 2);

	int64_t start_usec = 0;
	int64_t end_usec = 0;
	assert_true(replay_buffer_cache_get_time_range(cache, &start_usec, &end_usec));
	assert_int_equal(start_usec, 0);
	assert_int_equal(end_usec, 1100000);

	struct replay_buffer_keyframe_info info = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &info));
	assert_int_equal(info.packet_index, 0);
	assert_int_equal(info.dts_usec, 0);
	assert_int_equal(info.duration_usec, 1000000);

	assert_true(replay_buffer_cache_get_keyframe_info(cache, 1, &info));
	assert_int_equal(info.packet_index, 3);
	assert_int_equal(info.dts_usec, 1000000);
	assert_int_equal(info.duration_usec, 100000);

	size_t index = 0;
	assert_true(replay_buffer_cache_find_keyframe(cache, 400000, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_find_keyframe(cache, 700000, &index));
	assert_int_equal(index, 1);
	assert_true(replay_buffer_cache_find_keyframe(cache, -100000, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_find_keyframe(cache, 10000000, &index));
	assert_int_equal(index, 1);

	replay_buffer_cache_destroy(cache);
}

static void test_eviction_keeps_index_in_sync(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 2000000);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 100, false);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 100000, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 1100000, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 2000000, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 3000000, 100, false);

	/* The 0s GOP is dropped when the 3s keyframe arrives and the buffer
	 * keeps at least two keyframes. */
	assert_int_equal(replay_buffer_cache_get_keyframe_count(cache), 3);

	int64_t start_usec = 0;
	int64_t end_usec = 0;
	assert_true(replay_buffer_cache_get_time_range(cache, &start_usec, &end_usec));
	assert_int_equal(start_usec, 1000000);
	assert_int_equal(end_usec, 3000000);

	size_t index = 0;
	assert_true(replay_buffer_cache_find_keyframe(cache, 0, &index));
	assert_int_equal(index, 0);
	struct replay_buffer_keyframe_info info = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, index, &info));
	assert_int_equal(info.dts_usec, 1000000);

	/* The evicted keyframe packet must no longer be reachable, while the one
	 * still buffered resolves through its absolute index. */
	uint64_t packet_index = 0;
	struct encoder_packet packet = {0};
	assert_false(replay_buffer_cache_ref_packet_at(cache, 0, &packet));

	assert_true(replay_buffer_cache_ref_keyframe_at(cache, 1000000, &packet_index, &packet));
	assert_int_equal(packet.dts_usec, 1000000);
	obs_encoder_packet_release(&packet);

	/* Walking from that keyframe by absolute index lands on the next packet. */
	assert_true(replay_buffer_cache_ref_packet_at(cache, packet_index + 1, &packet));
	assert_int_equal(packet.dts_usec, 1100000);
	obs_encoder_packet_release(&packet);

	/* An index that was never handed out is rejected. */
	assert_false(replay_buffer_cache_ref_packet_at(cache, packet_index + 1000, &packet));

	replay_buffer_cache_destroy(cache);
}

static void test_snapshot_survives_clear(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60000000);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 16, true);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 100000, 8, true);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 16, true);

	replay_buffer_snapshot_t *snapshot = replay_buffer_cache_create_snapshot(cache);
	assert_non_null(snapshot);
	assert_int_equal(replay_buffer_snapshot_get_packet_count(snapshot), 3);

	struct encoder_packet *packet = replay_buffer_snapshot_get_packet(snapshot, 2);
	assert_non_null(packet);
	assert_int_equal(packet->dts_usec, 1000000);
	assert_int_equal(packet->size, 16);

	replay_buffer_cache_clear(cache);

	/* The snapshot owns references, so it remains valid after the ring is
	 * cleared. */
	packet = replay_buffer_snapshot_get_packet(snapshot, 0);
	assert_non_null(packet);
	assert_int_equal(packet->dts_usec, 0);

	assert_null(replay_buffer_snapshot_get_packet(snapshot, 3));

	replay_buffer_snapshot_release(snapshot);
	replay_buffer_cache_destroy(cache);
}

static void test_find_prev_next_keyframe(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60 * 1000000LL);

	size_t index = 0;
	struct replay_buffer_keyframe_info info = {0};

	/* An empty cache has no neighbours. */
	assert_false(replay_buffer_cache_find_prev_keyframe(cache, 0, &index));
	assert_false(replay_buffer_cache_find_next_keyframe(cache, 0, &index));

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 16, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 16, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 2000000, 16, false);

	/* The lookup is strict, so a playhead parked on a keyframe still moves. */
	assert_false(replay_buffer_cache_find_prev_keyframe(cache, 0, &index));
	assert_true(replay_buffer_cache_find_prev_keyframe(cache, 1, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_find_prev_keyframe(cache, 1000000, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_find_prev_keyframe(cache, 1500000, &index));
	assert_int_equal(index, 1);
	assert_true(replay_buffer_cache_find_prev_keyframe(cache, 3000000, &index));
	assert_int_equal(index, 2);

	assert_false(replay_buffer_cache_find_next_keyframe(cache, 2000000, &index));
	assert_true(replay_buffer_cache_find_next_keyframe(cache, -1000000, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_find_next_keyframe(cache, 0, &index));
	assert_int_equal(index, 1);
	assert_true(replay_buffer_cache_find_next_keyframe(cache, 500000, &index));
	assert_int_equal(index, 1);
	assert_true(replay_buffer_cache_find_next_keyframe(cache, 1000000, &index));
	assert_int_equal(index, 2);

	/* After eviction the returned indices are relative to the new head. */
	replay_buffer_cache_set_limits(cache, 0, 2000000);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 3000000, 16, false);
	assert_int_equal(replay_buffer_cache_get_keyframe_count(cache), 3);

	assert_true(replay_buffer_cache_find_prev_keyframe(cache, 1500000, &index));
	assert_int_equal(index, 0);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, index, &info));
	assert_int_equal(info.dts_usec, 1000000);

	assert_true(replay_buffer_cache_find_next_keyframe(cache, 1500000, &index));
	assert_int_equal(index, 1);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, index, &info));
	assert_int_equal(info.dts_usec, 2000000);

	replay_buffer_cache_destroy(cache);
}

static void test_zero_time_limit_keeps_history(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	/* A zero size and time limit both mean unlimited. */
	replay_buffer_cache_set_limits(cache, 0, 0);

	for (int i = 0; i < 4; i++)
		push_packet(cache, OBS_ENCODER_VIDEO, true, (int64_t)i * 1000000, 16, false);

	assert_int_equal(replay_buffer_cache_get_keyframe_count(cache), 4);

	int64_t start_usec = 0;
	int64_t end_usec = 0;
	assert_true(replay_buffer_cache_get_time_range(cache, &start_usec, &end_usec));
	assert_int_equal(start_usec, 0);
	assert_int_equal(end_usec, 3000000);

	replay_buffer_cache_destroy(cache);
}

static void test_range_snapshot_snaps_start_and_truncates_end(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60 * 1000000LL);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 16, true);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 20000, 8, true);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 400000, 16, true);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 16, true);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 1000000, 8, true);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 1400000, 16, true);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 1500000, 8, true);

	int64_t actual_start = -1;
	int64_t actual_end = -1;

	/* The start snaps back to the 0s keyframe and the end is the last packet
	 * whose dts is at or before the requested end. */
	assert_true(replay_buffer_cache_resolve_range(cache, 300000, 1200000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 0);
	assert_int_equal(actual_end, 1000000);

	replay_buffer_snapshot_t *snapshot =
		replay_buffer_cache_create_range_snapshot(cache, 300000, 1200000, &actual_start, &actual_end);
	assert_non_null(snapshot);
	assert_int_equal(actual_start, 0);
	assert_int_equal(actual_end, 1000000);
	/* keyframe, audio, video, keyframe, audio */
	assert_int_equal(replay_buffer_snapshot_get_packet_count(snapshot), 5);

	struct encoder_packet *packet = replay_buffer_snapshot_get_packet(snapshot, 0);
	assert_non_null(packet);
	assert_true(packet->type == OBS_ENCODER_VIDEO);
	assert_true(packet->keyframe);
	assert_int_equal(packet->dts_usec, 0);

	packet = replay_buffer_snapshot_get_packet(snapshot, 4);
	assert_non_null(packet);
	assert_int_equal(packet->dts_usec, 1000000);

	/* Nothing from the next GOP leaks into the export. */
	assert_null(replay_buffer_snapshot_get_packet(snapshot, 5));

	/* The snapshot owns packet references, so clearing the ring is safe. */
	replay_buffer_cache_clear(cache);
	packet = replay_buffer_snapshot_get_packet(snapshot, 0);
	assert_non_null(packet);
	assert_int_equal(packet->dts_usec, 0);

	replay_buffer_snapshot_release(snapshot);
	replay_buffer_cache_destroy(cache);
}

static void test_range_boundaries_and_clamping(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60 * 1000000LL);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 16, false);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 500000, 16, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 16, false);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 1500000, 16, false);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 1500000, 8, false);

	int64_t actual_start = -1;
	int64_t actual_end = -1;

	/* Inverted and zero-length intervals resolve to nothing. */
	assert_false(replay_buffer_cache_resolve_range(cache, 1200000, 1100000, &actual_start, &actual_end));
	assert_false(replay_buffer_cache_resolve_range(cache, 1200000, 1200000, &actual_start, &actual_end));

	/* A start past the newest packet clamps back to the GOP that covers it. */
	assert_true(replay_buffer_cache_resolve_range(cache, 2000000, 2500000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 1000000);
	assert_int_equal(actual_end, 1500000);
	assert_true(replay_buffer_cache_resolve_range(cache, 5000000, 6000000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 1000000);
	assert_int_equal(actual_end, 1500000);

	/* Out-of-window requests clamp to the available range. */
	assert_true(replay_buffer_cache_resolve_range(cache, -500000, 9000000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 0);
	assert_int_equal(actual_end, 1500000);

	/* Start snaps to the 1s keyframe; the 1.5s end keeps the last packet. */
	assert_true(replay_buffer_cache_resolve_range(cache, 1200000, 1600000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 1000000);
	assert_int_equal(actual_end, 1500000);

	/* An end that lands exactly on a keyframe includes that keyframe. */
	assert_true(replay_buffer_cache_resolve_range(cache, 0, 1000000, &actual_start, &actual_end));
	assert_int_equal(actual_start, 0);
	assert_int_equal(actual_end, 1000000);

	/* Only the aligned keyframe would be written: not a usable range. */
	assert_false(replay_buffer_cache_resolve_range(cache, 0, 100000, &actual_start, &actual_end));

	replay_buffer_cache_destroy(cache);
}

static void test_info_is_consistent(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 4000000, 30000000);

	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 100, false));
	assert_true(push_packet(cache, OBS_ENCODER_AUDIO, false, 1100000, 10, false));
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, false, 1200000, 20, false));
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, 2000000, 200, false));

	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.start_usec, 1000000);
	assert_int_equal(info.end_usec, 2000000);
	assert_int_equal(info.max_size_bytes, 4000000);
	assert_int_equal(info.max_time_usec, 30000000);
	assert_int_equal(info.bytes, 330);
	assert_int_equal(info.keyframe_count, 2);
	assert_int_equal(info.packet_count, 4);
	assert_int_equal(info.dropped_packets, 0);
	assert_int_equal(info.dropped_keyframes, 0);
	assert_int_equal(info.push_failures, 0);

	struct replay_buffer_keyframe_info frame = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &frame));
	assert_int_equal(frame.packet_index, 0);
	assert_int_equal(frame.dts_usec, 1000000);
	assert_int_equal(frame.duration_usec, 1000000);

	assert_true(replay_buffer_cache_get_keyframe_info(cache, 1, &frame));
	assert_int_equal(frame.packet_index, 3);
	assert_int_equal(frame.dts_usec, 2000000);
	assert_int_equal(frame.duration_usec, 0);

	replay_buffer_cache_destroy(cache);
}

static void test_push_reports_dropped_packets(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60000000);

	/* Audio and non-keyframe video before the first keyframe have no GOP to
	 * belong to; push reports that instead of silently accepting them. */
	assert_false(push_packet(cache, OBS_ENCODER_AUDIO, false, 0, 10, false));
	assert_false(push_packet(cache, OBS_ENCODER_VIDEO, false, 100000, 20, false));

	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.packet_count, 0);
	assert_int_equal(info.keyframe_count, 0);
	assert_int_equal(info.dropped_packets, 2);
	assert_int_equal(info.dropped_bytes, 30);
	assert_int_equal(info.dropped_keyframes, 0);
	assert_int_equal(info.push_failures, 0);

	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, 200000, 100, false));
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, false, 300000, 50, false));

	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.packet_count, 2);
	assert_int_equal(info.keyframe_count, 1);
	assert_int_equal(info.dropped_packets, 2);

	/* clear() starts a new session: counters reset and a packet that is not
	 * preceded by a keyframe is dropped again. */
	replay_buffer_cache_clear(cache);
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.packet_count, 0);
	assert_int_equal(info.keyframe_count, 0);
	assert_int_equal(info.dropped_packets, 0);

	assert_false(push_packet(cache, OBS_ENCODER_VIDEO, false, 400000, 50, false));
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.dropped_packets, 1);
	assert_int_equal(info.dropped_bytes, 50);

	replay_buffer_cache_destroy(cache);
}

static void test_block_growth_and_eviction(void **state)
{
	UNUSED_PARAMETER(state);

	enum { kPackets = 3000, kGop = 100, kSize = 100 };

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	/* No limits: the block table has to grow past the first block. */
	replay_buffer_cache_set_limits(cache, 0, 0);

	for (int i = 0; i < kPackets; i++)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, (i % kGop) == 0, (int64_t)i * 1000, kSize, false));

	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.packet_count, kPackets);
	assert_int_equal(info.keyframe_count, kPackets / kGop);
	assert_int_equal(info.bytes, (int64_t)kPackets * kSize);

	struct encoder_packet packet = {0};
	assert_true(replay_buffer_cache_ref_packet_at(cache, 0, &packet));
	assert_int_equal(packet.dts_usec, 0);
	obs_encoder_packet_release(&packet);

	assert_true(replay_buffer_cache_ref_packet_at(cache, kPackets - 1, &packet));
	assert_int_equal(packet.dts_usec, (int64_t)(kPackets - 1) * 1000);
	obs_encoder_packet_release(&packet);

	/* Force history to roll past the first two blocks. Two GOPs are retained,
	 * so the window ends up at 2800..3000 (201 packets) even though that is
	 * 100 bytes over the configured size: the two-GOP floor wins. */
	replay_buffer_cache_set_limits(cache, 20000, 0);
	assert_true(push_packet(cache, OBS_ENCODER_VIDEO, false, (int64_t)kPackets * 1000, kSize, false));

	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.keyframe_count, 2);
	assert_int_equal(info.packet_count, 201);
	assert_int_equal(info.bytes, 20100);
	assert_int_equal(info.start_usec, 2800000);
	assert_int_equal(info.end_usec, 3000000);
	assert_int_equal(info.dropped_packets, 0);

	/* Indices in the freed blocks no longer resolve; the new head and the
	 * newest packet do. */
	assert_false(replay_buffer_cache_ref_packet_at(cache, 0, &packet));
	assert_false(replay_buffer_cache_ref_packet_at(cache, 1500, &packet));
	assert_false(replay_buffer_cache_ref_packet_at(cache, 2799, &packet));
	assert_true(replay_buffer_cache_ref_packet_at(cache, 2800, &packet));
	assert_int_equal(packet.dts_usec, 2800000);
	obs_encoder_packet_release(&packet);
	assert_true(replay_buffer_cache_ref_packet_at(cache, kPackets, &packet));
	assert_int_equal(packet.dts_usec, (int64_t)kPackets * 1000);
	obs_encoder_packet_release(&packet);

	replay_buffer_cache_destroy(cache);
}

/* Every exposed window has to start on a keyframe, and that keyframe has to
 * reach back over the whole configured length. Both invariants are checked
 * after every push, so an eviction that lands on the wrong side of the limit is
 * caught as soon as it happens. */
static void assert_window_covers_limit(replay_buffer_cache_t *cache, int64_t max_time_usec, int64_t frame_usec)
{
	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);

	if (info.end_usec < max_time_usec)
		return;

	/* The configured value is the window length; the extra GOP the cache keeps
	 * is not allowed to show up as a longer scale. */
	assert_int_equal(info.end_usec - info.start_usec, max_time_usec);

	struct replay_buffer_keyframe_info oldest = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_true(oldest.dts_usec <= info.start_usec);

	/* The oldest GOP can overshoot the limit, but by no more than its own
	 * length: eviction waits for the next keyframe to cross the limit. */
	if (info.keyframe_count > 2) {
		const int64_t retained = info.end_usec - oldest.dts_usec;
		assert_true(retained < max_time_usec + oldest.duration_usec + frame_usec);
	}

	/* Exporting the displayed window has to yield at least the configured
	 * length, even though the start snaps back to the older keyframe. */
	int64_t actual_start = 0;
	int64_t actual_end = 0;
	assert_true(replay_buffer_cache_resolve_range(cache, info.start_usec, info.end_usec, &actual_start, &actual_end));
	assert_true(actual_end - actual_start >= max_time_usec);
}

static void test_full_window_stays_stable_and_exports_full_length(void **state)
{
	UNUSED_PARAMETER(state);

	const int64_t max_time_usec = 60 * 1000000LL;
	const int64_t gop_usec = 4000000;
	const int64_t frame_usec = 100000;

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, max_time_usec);

	bool have_previous = false;
	int64_t previous_start = 0;

	for (int64_t t = 0; t <= 100 * 1000000LL; t += frame_usec) {
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, (t % gop_usec) == 0, t, 100, false));
		assert_window_covers_limit(cache, max_time_usec, frame_usec);

		struct replay_buffer_cache_info info = {0};
		replay_buffer_cache_get_info(cache, &info);

		/* Once full, the left edge advances with the buffer one frame at a
		 * time. Dropping the whole front GOP as soon as it crossed the limit
		 * made it jump backwards by that GOP every few seconds. */
		if (have_previous && info.end_usec >= max_time_usec) {
			assert_true(info.start_usec >= previous_start);
			assert_true(info.start_usec - previous_start <= frame_usec);
		}
		have_previous = true;
		previous_start = info.start_usec;
	}

	/* Only the configured length is exposed, not the surplus GOP. */
	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.end_usec - info.start_usec, max_time_usec);

	replay_buffer_cache_destroy(cache);
}

/* The keyframe interval is not fixed, so the extra GOP has to follow whatever
 * the encoder is doing right now instead of a remembered maximum. */
static void test_gop_interval_change_adapts(void **state)
{
	UNUSED_PARAMETER(state);

	const int64_t max_time_usec = 20 * 1000000LL;
	const int64_t frame_usec = 100000;

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, max_time_usec);

	bool saw_wide_gop = false;

	for (int64_t t = 0; t <= 70 * 1000000LL; t += frame_usec) {
		/* 2 s GOPs up to 24 s, then 8 s GOPs (32/40/48), then 1 s GOPs. */
		bool keyframe;
		if (t <= 24000000)
			keyframe = (t % 2000000) == 0;
		else if (t <= 48000000)
			keyframe = (t % 8000000) == 0;
		else
			keyframe = (t % 1000000) == 0;

		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, keyframe, t, 100, false));
		assert_window_covers_limit(cache, max_time_usec, frame_usec);

		if (t >= 42000000 && t <= 46000000) {
			struct replay_buffer_keyframe_info oldest = {0};
			assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
			if (oldest.duration_usec >= 8000000)
				saw_wide_gop = true;
		}
	}

	/* The 8 s GOPs were the head for a while, which is what the old rule would
	 * have kept over-retaining after the interval shrank again. */
	assert_true(saw_wide_gop);

	/* By now the 1 s GOPs are at the front, so retention is back to about the
	 * configured length plus one short GOP. */
	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	struct replay_buffer_keyframe_info oldest = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_true(oldest.duration_usec <= 1100000);
	assert_true(info.end_usec - oldest.dts_usec < max_time_usec + 1100000);

	replay_buffer_cache_destroy(cache);
}

/* A limit shorter than a GOP cannot be covered by a fractional GOP, so the
 * window degrades to the two-GOP floor without losing the start keyframe. */
static void test_small_max_time_degrades_to_gop_floor(void **state)
{
	UNUSED_PARAMETER(state);

	const int64_t gop_usec = 4000000;
	const int64_t frame_usec = 100000;
	const int64_t limits[] = {5000000, 1000000};

	for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
		const int64_t max_time_usec = limits[i];

		replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
		assert_non_null(cache);
		replay_buffer_cache_set_limits(cache, 0, max_time_usec);

		for (int64_t t = 0; t <= 20 * 1000000LL; t += frame_usec) {
			assert_true(push_packet(cache, OBS_ENCODER_VIDEO, (t % gop_usec) == 0, t, 100, false));
			assert_window_covers_limit(cache, max_time_usec, frame_usec);
		}

		struct replay_buffer_cache_info info = {0};
		replay_buffer_cache_get_info(cache, &info);
		assert_int_equal(info.end_usec - info.start_usec, max_time_usec);
		/* The floor is what keeps a keyframe available for the export. */
		assert_true(info.keyframe_count >= 2);

		replay_buffer_cache_destroy(cache);
	}
}

/* The eviction check used by the timeline: a keyframe remembered by its dts
 * resolves to that same dts while it is buffered, and to the next keyframe (or
 * nothing) once history has rolled past it. */
static void test_evicted_anchor_stops_resolving(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 2000000);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 100, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 2000000, 100, false);

	size_t ordinal = 0;
	struct replay_buffer_keyframe_info info = {0};

	/* While the anchor is buffered, the lookup answers with the anchor itself. */
	assert_true(replay_buffer_cache_find_keyframe(cache, 0, &ordinal));
	assert_true(replay_buffer_cache_get_keyframe_info(cache, ordinal, &info));
	assert_int_equal(info.dts_usec, 0);

	/* The 3s keyframe makes the 0s GOP roll out: the same lookup now answers with
	 * a newer keyframe, which is what marks the remembered anchor as evicted. */
	push_packet(cache, OBS_ENCODER_VIDEO, true, 3000000, 100, false);
	assert_true(replay_buffer_cache_find_keyframe(cache, 0, &ordinal));
	assert_true(replay_buffer_cache_get_keyframe_info(cache, ordinal, &info));
	assert_true(info.dts_usec != 0);

	/* A keyframe that is still buffered keeps answering with its own dts. */
	assert_true(replay_buffer_cache_find_keyframe(cache, 1000000, &ordinal));
	assert_true(replay_buffer_cache_get_keyframe_info(cache, ordinal, &info));
	assert_int_equal(info.dts_usec, 1000000);

	/* An emptied cache has no anchor at all. */
	replay_buffer_cache_clear(cache);
	assert_false(replay_buffer_cache_find_keyframe(cache, 1000000, &ordinal));

	replay_buffer_cache_destroy(cache);
}

/* Keyframe at or before time_usec, the same anchor rule the panel and the export
 * use, expressed against the cache alone. */
static bool anchor_at(replay_buffer_cache_t *cache, int64_t time_usec, int64_t *dts_usec)
{
	size_t ordinal = 0;
	struct replay_buffer_keyframe_info info = {0};

	if (replay_buffer_cache_find_keyframe(cache, time_usec, &ordinal) &&
	    replay_buffer_cache_get_keyframe_info(cache, ordinal, &info) && info.dts_usec <= time_usec) {
		*dts_usec = info.dts_usec;
		return true;
	}

	if (replay_buffer_cache_find_prev_keyframe(cache, time_usec, &ordinal) &&
	    replay_buffer_cache_get_keyframe_info(cache, ordinal, &info)) {
		*dts_usec = info.dts_usec;
		return true;
	}

	return false;
}

/* An export protects only the GOPs the writer has not reached; written history
 * is retired by the normal limits as the watermark advances. */
static void test_pin_protects_only_unwritten_gops(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 2000000);

	for (int64_t t = 0; t <= 9000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	/* Settled head: 7s. The export covers 7s..9s. */
	struct replay_buffer_keyframe_info oldest = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 7000000);

	replay_buffer_cache_set_pin(cache, 7000000, 9000000);

	for (int64_t t = 10000000; t <= 12000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	/* The front GOP is still unwritten, so nothing can be retired even though
	 * the buffer is well over the limit. */
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 7000000);

	/* The timeline keeps the normal window: the pin is about memory, not about
	 * freezing the scale at the export start. */
	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.start_usec, 10000000);
	assert_int_equal(info.end_usec, 12000000);

	/* Writing through 8s releases the 7s GOP; the next one is still pending. */
	replay_buffer_cache_set_pin_progress(cache, 8000000);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 8000000);

	/* The same watermark again must not change anything. */
	replay_buffer_cache_set_pin_progress(cache, 8000000);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 8000000);

	/* Writing through the export end releases the 8s GOP; the GOP holding the
	 * end stays until the export is actually finished. */
	replay_buffer_cache_set_pin_progress(cache, 9000000);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 9000000);

	/* Clearing the pin (idempotently) trims once, all the way back. */
	replay_buffer_cache_clear_pin(cache);
	replay_buffer_cache_clear_pin(cache);

	replay_buffer_cache_get_info(cache, &info);
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_true(oldest.dts_usec >= 10000000);
	assert_int_equal(info.end_usec - info.start_usec, 2000000);

	replay_buffer_cache_destroy(cache);
}

/* GOPs before the export start are not part of it and stay evictable. */
static void test_pin_ignores_gops_before_export_start(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 2000000);

	for (int64_t t = 0; t <= 9000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	/* Export starts at 8s while the cache still holds the 7s GOP. */
	replay_buffer_cache_set_pin(cache, 8000000, 9000000);

	for (int64_t t = 10000000; t <= 12000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	/* 7s is outside the export and was dropped; 8s is protected. */
	struct replay_buffer_keyframe_info oldest = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 8000000);

	replay_buffer_cache_clear_pin(cache);

	replay_buffer_cache_destroy(cache);
}

static void test_clear_resets_pin(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 2000000);

	for (int64_t t = 0; t <= 9000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	replay_buffer_cache_set_pin(cache, 7000000, 9000000);
	replay_buffer_cache_clear(cache);

	/* If the protection had leaked, nothing below would ever be retired. */
	for (int64_t t = 0; t <= 3000000; t += 1000000)
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, true, t, 100, false));

	struct replay_buffer_keyframe_info oldest = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &oldest));
	assert_int_equal(oldest.dts_usec, 1000000);

	struct replay_buffer_cache_info info = {0};
	replay_buffer_cache_get_info(cache, &info);
	assert_int_equal(info.keyframe_count, 3);

	replay_buffer_cache_destroy(cache);
}

/* Emulates the panel rule: while the clip's start keyframe rolls out, the start
 * follows the oldest surviving keyframe until it meets the end keyframe, at
 * which point the clip has collapsed and the panel goes live. */
static void test_selection_start_follows_surviving_keyframes(void **state)
{
	UNUSED_PARAMETER(state);

	const int64_t max_time_usec = 6000000;
	const int64_t end_anchor = 10000000;

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, max_time_usec);

	int64_t start_anchor = 2000000;
	bool advanced = false;
	bool collapsed = false;

	for (int64_t t = 0; t <= 20 * 1000000LL; t += 100000) {
		assert_true(push_packet(cache, OBS_ENCODER_VIDEO, (t % 2000000) == 0, t, 100, false));

		struct replay_buffer_cache_info info = {0};
		replay_buffer_cache_get_info(cache, &info);
		if (info.end_usec < max_time_usec)
			continue;

		/* The start keyframe is gone: realign to the oldest surviving one. */
		int64_t resolved = 0;
		if (!anchor_at(cache, start_anchor, &resolved) || resolved != start_anchor) {
			assert_true(anchor_at(cache, info.start_usec, &resolved));
			assert_true(resolved >= start_anchor);
			start_anchor = resolved;
			advanced = true;
		}

		if (start_anchor == end_anchor) {
			collapsed = true;
			break;
		}

		assert_true(start_anchor < end_anchor);
	}

	assert_true(advanced);
	assert_true(collapsed);

	replay_buffer_cache_destroy(cache);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_push_index_and_range),
		cmocka_unit_test(test_eviction_keeps_index_in_sync),
		cmocka_unit_test(test_snapshot_survives_clear),
		cmocka_unit_test(test_find_prev_next_keyframe),
		cmocka_unit_test(test_zero_time_limit_keeps_history),
		cmocka_unit_test(test_range_snapshot_snaps_start_and_truncates_end),
		cmocka_unit_test(test_range_boundaries_and_clamping),
		cmocka_unit_test(test_info_is_consistent),
		cmocka_unit_test(test_push_reports_dropped_packets),
		cmocka_unit_test(test_block_growth_and_eviction),
		cmocka_unit_test(test_full_window_stays_stable_and_exports_full_length),
		cmocka_unit_test(test_gop_interval_change_adapts),
		cmocka_unit_test(test_small_max_time_degrades_to_gop_floor),
		cmocka_unit_test(test_evicted_anchor_stops_resolving),
		cmocka_unit_test(test_pin_protects_only_unwritten_gops),
		cmocka_unit_test(test_pin_ignores_gops_before_export_start),
		cmocka_unit_test(test_clear_resets_pin),
		cmocka_unit_test(test_selection_start_follows_surviving_keyframes),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
