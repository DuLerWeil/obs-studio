#pragma once

#include "obs-ffmpeg-replay-buffer-frame-decoder.h"

#include <obs-module.h>
#include <obs-hotkey.h>
#include <util/deque.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/pipe.h>
#include <util/platform.h>
#include <util/threading.h>

typedef DARRAY(struct encoder_packet) mux_packets_t;

struct ffmpeg_muxer {
	obs_output_t *output;
	os_process_pipe_t *pipe;
	int64_t stop_ts;
	uint64_t total_bytes;
	bool sent_headers;
	volatile bool active;
	volatile bool capturing;
	volatile bool stopping;
	struct dstr path;
	struct dstr printable_path;
	struct dstr muxer_settings;
	struct dstr stream_key;

	/* replay buffer and split file */
	int64_t cur_size;
	int64_t cur_time;
	int64_t max_size;
	int64_t max_time;

	/* replay buffer */
	int64_t save_ts;
	replay_buffer_cache_t *replay_cache;
	replay_buffer_frame_decoder_t *frame_decoder;
	pthread_mutex_t replay_buffer_mutex;
	bool replay_buffer_mutex_initialized;
	/* The GPU encode thread may still hand us a packet while the output is being
	 * destroyed; destroy sets replay_shutdown and waits for replay_callbacks to
	 * reach zero before it frees anything the callback touches. */
	volatile bool replay_shutdown;
	int replay_callbacks;
	pthread_cond_t replay_cond;
	bool replay_cond_initialized;
	pthread_mutex_t save_mutex;
	bool save_mutex_initialized;
	bool selection_export;
	/* Set while the replay save/export mux thread owns stream->pipe, so a
	 * failed helper write only fails that file instead of the live output. */
	bool replay_mux;
	volatile long export_state;
	volatile long export_error;
	struct dstr selection_path;
	obs_hotkey_id hotkey;
	volatile bool muxing;
	mux_packets_t mux_packets;

	/* split file */
	bool found_video;
	bool found_audio[MAX_AUDIO_MIXES];
	int64_t video_pts_offset;
	int64_t audio_dts_offsets[MAX_AUDIO_MIXES];
	bool split_file_ready;
	volatile bool manual_split;

	/* these are accessed both by replay buffer and by HLS */
	pthread_t mux_thread;
	bool mux_thread_joinable;
	struct deque packets;

	/* HLS only */
	int keyint_sec;
	pthread_mutex_t write_mutex;
	os_sem_t *write_sem;
	os_event_t *stop_event;
	bool is_hls;
	int dropped_frames;
	int min_priority;
	int64_t last_dts_usec;

	bool is_network;
	bool split_file;
	bool allow_overwrite;
};

bool stopping(struct ffmpeg_muxer *stream);
bool active(struct ffmpeg_muxer *stream);
void start_pipe(struct ffmpeg_muxer *stream, const char *path);
bool write_packet(struct ffmpeg_muxer *stream, struct encoder_packet *packet);
bool send_headers(struct ffmpeg_muxer *stream);
int deactivate(struct ffmpeg_muxer *stream, int code);
void ffmpeg_mux_stop(void *data, uint64_t ts);
uint64_t ffmpeg_mux_total_bytes(void *data);
