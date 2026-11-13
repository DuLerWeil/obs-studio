#pragma once

#include "obs-ffmpeg-replay-buffer.h"

#define REPLAY_BUFFER_FRAME_PENDING 0
#define REPLAY_BUFFER_FRAME_READY 1
#define REPLAY_BUFFER_FRAME_FAILED 2

typedef struct replay_buffer_frame_decoder replay_buffer_frame_decoder_t;

/* Starts a low-priority worker that decodes replay buffer keyframes into
 * full-resolution RGBA frames for the review canvas. Returns NULL when the codec
 * is unsupported or no decoder is available; the replay buffer keeps working
 * without frame preview in that case. */
replay_buffer_frame_decoder_t *replay_buffer_frame_decoder_create(obs_encoder_t *video_encoder,
								  replay_buffer_cache_t *cache);

/* Stops and joins the worker, then releases the decoder. Safe to call with
 * NULL. */
void replay_buffer_frame_decoder_destroy(replay_buffer_frame_decoder_t *frame_decoder);

/* Queues a full-resolution decode of the keyframe identified by seq. The worker
 * keeps only the newest request: a later request_id supersedes an older one and
 * the older result is discarded. request_id must be non-zero. Returns false when
 * the worker is stopping. */
bool replay_buffer_frame_decoder_request(replay_buffer_frame_decoder_t *frame_decoder, uint64_t seq,
					 uint64_t request_id);

/* Queries or copies the result of request_id. status is one of
 * REPLAY_BUFFER_FRAME_PENDING/READY/FAILED. With dst == NULL and capacity == 0
 * only the dimensions and size are reported. A ready result is kept until the
 * next result is published or the decoder is destroyed. */
bool replay_buffer_frame_decoder_copy(replay_buffer_frame_decoder_t *frame_decoder, uint64_t request_id, uint8_t *dst,
				      size_t capacity, int *status, uint32_t *width, uint32_t *height, size_t *size);
