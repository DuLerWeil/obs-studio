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
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
