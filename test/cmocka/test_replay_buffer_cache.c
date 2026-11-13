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

static void push_packet(replay_buffer_cache_t *cache, enum obs_encoder_type type, bool keyframe, int64_t dts_usec,
			size_t size, bool allocate_data)
{
	struct encoder_packet packet;
	packet_init(&packet, type, keyframe, dts_usec, size, 0, allocate_data);
	replay_buffer_cache_push(cache, &packet);
	obs_encoder_packet_release(&packet);
}

static void test_push_index_and_range(void **state)
{
	UNUSED_PARAMETER(state);

	replay_buffer_cache_t *cache = replay_buffer_cache_create(0);
	assert_non_null(cache);
	replay_buffer_cache_set_limits(cache, 0, 60 * 1000000LL);

	push_packet(cache, OBS_ENCODER_VIDEO, true, 0, 100, false);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 100000, 10, false);
	push_packet(cache, OBS_ENCODER_VIDEO, false, 200000, 20, false);
	push_packet(cache, OBS_ENCODER_VIDEO, true, 1000000, 110, false);
	push_packet(cache, OBS_ENCODER_AUDIO, false, 1100000, 10, false);

	assert_int_equal(replay_buffer_cache_get_keyframe_count(cache), 2);

	int64_t start_usec = 0;
	int64_t end_usec = 0;
	assert_true(replay_buffer_cache_get_time_range(cache, &start_usec, &end_usec));
	assert_int_equal(start_usec, 0);
	assert_int_equal(end_usec, 1100000);

	struct replay_buffer_keyframe_info info = {0};
	assert_true(replay_buffer_cache_get_keyframe_info(cache, 0, &info));
	assert_int_equal(info.seq, 0);
	assert_int_equal(info.dts_usec, 0);
	assert_int_equal(info.duration_usec, 1000000);

	assert_true(replay_buffer_cache_get_keyframe_info(cache, 1, &info));
	assert_int_equal(info.seq, 3);
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

	/* The evicted keyframe packet must no longer be reachable. */
	struct encoder_packet packet = {0};
	assert_false(replay_buffer_cache_ref_keyframe_packet(cache, 0, &packet));
	assert_true(replay_buffer_cache_ref_keyframe_packet(cache, 2, &packet));
	assert_int_equal(packet.dts_usec, 1000000);
	obs_encoder_packet_release(&packet);

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

	/* Inverted, zero-length, past the newest packet and past the window. */
	assert_false(replay_buffer_cache_resolve_range(cache, 1200000, 1100000, &actual_start, &actual_end));
	assert_false(replay_buffer_cache_resolve_range(cache, 1200000, 1200000, &actual_start, &actual_end));
	assert_false(replay_buffer_cache_resolve_range(cache, 2000000, 2500000, &actual_start, &actual_end));
	assert_false(replay_buffer_cache_resolve_range(cache, 5000000, 6000000, &actual_start, &actual_end));

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
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
