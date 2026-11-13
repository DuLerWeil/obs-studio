/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#include "ffmpeg-mux/ffmpeg-mux.h"
#include "obs-ffmpeg-mux.h"
#include "obs-ffmpeg-formats.h"
#include "obs-ffmpeg-replay-buffer.h"
#include "obs-ffmpeg-replay-buffer-frame-decoder.h"

#ifdef _WIN32
#include "util/windows/win-version.h"
#endif

#include <libavformat/avformat.h>

#define do_log(level, format, ...) \
	blog(level, "[ffmpeg muxer: '%s'] " format, obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

/* Replay selection export state, readable through get_replay_export_status. */
#define REPLAY_EXPORT_IDLE 0
#define REPLAY_EXPORT_RUNNING 1
#define REPLAY_EXPORT_DONE 2
#define REPLAY_EXPORT_FAILED 3

/* Also used as the result of export_replay_selection: 0 means the export
 * started, any other value is the reason it did not. */
#define REPLAY_EXPORT_ERR_NONE 0
#define REPLAY_EXPORT_ERR_INVALID 1
#define REPLAY_EXPORT_ERR_BUSY 2
#define REPLAY_EXPORT_ERR_INACTIVE 3
#define REPLAY_EXPORT_ERR_WRITE 4

static const char *ffmpeg_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegMuxer");
}

#ifndef NEW_MPEGTS_OUTPUT
static const char *ffmpeg_mpegts_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegMpegtsMuxer");
}
#endif

static inline void replay_buffer_clear(struct ffmpeg_muxer *stream)
{
	if (stream->replay_buffer_mutex_initialized)
		pthread_mutex_lock(&stream->replay_buffer_mutex);

	while (stream->packets.size > 0) {
		struct encoder_packet pkt;
		deque_pop_front(&stream->packets, &pkt, sizeof(pkt));
		obs_encoder_packet_release(&pkt);
	}

	deque_free(&stream->packets);
	replay_buffer_frame_decoder_destroy(stream->frame_decoder);
	stream->frame_decoder = NULL;
	replay_buffer_cache_clear(stream->replay_cache);
	stream->cur_size = 0;
	stream->cur_time = 0;
	stream->max_size = 0;
	stream->max_time = 0;
	stream->save_ts = 0;

	if (stream->replay_buffer_mutex_initialized)
		pthread_mutex_unlock(&stream->replay_buffer_mutex);
}

static void ffmpeg_mux_destroy(void *data)
{
	struct ffmpeg_muxer *stream = data;

	replay_buffer_clear(stream);
	replay_buffer_cache_destroy(stream->replay_cache);
	stream->replay_cache = NULL;
	if (stream->replay_buffer_mutex_initialized) {
		pthread_mutex_destroy(&stream->replay_buffer_mutex);
		stream->replay_buffer_mutex_initialized = false;
	}
	if (stream->mux_thread_joinable)
		pthread_join(stream->mux_thread, NULL);
	for (size_t i = 0; i < stream->mux_packets.num; i++)
		obs_encoder_packet_release(&stream->mux_packets.array[i]);
	da_free(stream->mux_packets);
	deque_free(&stream->packets);

	os_process_pipe_destroy(stream->pipe);
	dstr_free(&stream->path);
	dstr_free(&stream->printable_path);
	dstr_free(&stream->selection_path);
	dstr_free(&stream->stream_key);
	dstr_free(&stream->muxer_settings);
	if (stream->save_mutex_initialized)
		pthread_mutex_destroy(&stream->save_mutex);
	bfree(stream);
}

static void split_file_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;

	calldata_set_bool(cd, "split_file_enabled", stream->split_file);
	if (!stream->split_file)
		return;

	os_atomic_set_bool(&stream->manual_split, true);
}

static void *ffmpeg_mux_create(obs_data_t *settings, obs_output_t *output)
{
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	stream->output = output;

	if (obs_output_get_flags(output) & OBS_OUTPUT_SERVICE)
		stream->is_network = true;

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_add(sh, "void file_changed(string next_file)");

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_add(ph, "void split_file(out bool split_file_enabled)", split_file_proc, stream);

	UNUSED_PARAMETER(settings);
	return stream;
}

#ifdef _WIN32
#define FFMPEG_MUX "obs-ffmpeg-mux.exe"
#else
#define FFMPEG_MUX "obs-ffmpeg-mux"
#endif

static inline bool capturing(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->capturing);
}

bool stopping(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->stopping);
}

bool active(struct ffmpeg_muxer *stream)
{
	return os_atomic_load_bool(&stream->active);
}

static void add_video_encoder_params(struct ffmpeg_muxer *stream, os_process_args_t *args, obs_encoder_t *vencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(vencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	video_t *video = obs_get_video();
	const struct video_output_info *info = video_output_get_info(video);

	int codec_tag = (int)obs_data_get_int(settings, "codec_type");
#if __BYTE_ORDER == __LITTLE_ENDIAN
	codec_tag = ((codec_tag >> 24) & 0x000000FF) | ((codec_tag << 8) & 0x00FF0000) |
		    ((codec_tag >> 8) & 0x0000FF00) | ((codec_tag << 24) & 0xFF000000);
#endif

	obs_data_release(settings);

	enum AVColorPrimaries pri = AVCOL_PRI_UNSPECIFIED;
	enum AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
	enum AVColorSpace spc = AVCOL_SPC_UNSPECIFIED;
	switch (info->colorspace) {
	case VIDEO_CS_601:
		pri = AVCOL_PRI_SMPTE170M;
		trc = AVCOL_TRC_SMPTE170M;
		spc = AVCOL_SPC_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		pri = AVCOL_PRI_BT709;
		trc = AVCOL_TRC_BT709;
		spc = AVCOL_SPC_BT709;
		break;
	case VIDEO_CS_SRGB:
		pri = AVCOL_PRI_BT709;
		trc = AVCOL_TRC_IEC61966_2_1;
		spc = AVCOL_SPC_BT709;
		break;
	case VIDEO_CS_2100_PQ:
		pri = AVCOL_PRI_BT2020;
		trc = AVCOL_TRC_SMPTE2084;
		spc = AVCOL_SPC_BT2020_NCL;
		break;
	case VIDEO_CS_2100_HLG:
		pri = AVCOL_PRI_BT2020;
		trc = AVCOL_TRC_ARIB_STD_B67;
		spc = AVCOL_SPC_BT2020_NCL;
	}

	const enum AVColorRange range = (info->range == VIDEO_RANGE_FULL) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	const enum AVChromaLocation chroma_location =
		determine_chroma_location(obs_to_ffmpeg_video_format(info->format), spc);

	const int max_luminance = (trc == AVCOL_TRC_SMPTE2084) ? (int)obs_get_video_hdr_nominal_peak_level()
							       : ((trc == AVCOL_TRC_ARIB_STD_B67) ? 1000 : 0);

	os_process_args_add_arg(args, obs_encoder_get_codec(vencoder));
	os_process_args_add_argf(args, "%d", bitrate);
	os_process_args_add_argf(args, "%d", obs_output_get_width(stream->output));
	os_process_args_add_argf(args, "%d", obs_output_get_height(stream->output));
	os_process_args_add_argf(args, "%d", (int)pri);
	os_process_args_add_argf(args, "%d", (int)trc);
	os_process_args_add_argf(args, "%d", (int)spc);
	os_process_args_add_argf(args, "%d", (int)range);
	os_process_args_add_argf(args, "%d", (int)chroma_location);
	os_process_args_add_argf(args, "%d", max_luminance);
	os_process_args_add_argf(args, "%d", (int)info->fps_num);
	os_process_args_add_argf(args, "%d", (int)info->fps_den);
	os_process_args_add_argf(args, "%d", codec_tag);
}

static void add_audio_encoder_params(os_process_args_t *args, obs_encoder_t *aencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(aencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	audio_t *audio = obs_get_audio();

	obs_data_release(settings);

	os_process_args_add_arg(args, obs_encoder_get_name(aencoder));
	os_process_args_add_argf(args, "%d", bitrate);
	os_process_args_add_argf(args, "%d", (int)obs_encoder_get_sample_rate(aencoder));
	os_process_args_add_argf(args, "%d", (int)obs_encoder_get_frame_size(aencoder));
	os_process_args_add_argf(args, "%d", (int)obs_encoder_get_priming_samples(aencoder));
	os_process_args_add_argf(args, "%d", (int)audio_output_get_channels(audio));
}

static void log_muxer_params(struct ffmpeg_muxer *stream, const char *settings)
{
	int ret;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, settings, "=", " ", 0))) {
		warn("Failed to parse muxer settings: %s\n%s", av_err2str(ret), settings);

		av_dict_free(&dict);
		return;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		info("Using muxer settings:%s", str.array);
		dstr_free(&str);
	}

	av_dict_free(&dict);
}

static void add_stream_key(os_process_args_t *args, struct ffmpeg_muxer *stream)
{
	os_process_args_add_arg(args, dstr_is_empty(&stream->stream_key) ? "" : stream->stream_key.array);
}

static void add_muxer_params(os_process_args_t *args, struct ffmpeg_muxer *stream)
{
	struct dstr mux = {0};

	if (dstr_is_empty(&stream->muxer_settings)) {
		obs_data_t *settings = obs_output_get_settings(stream->output);
		dstr_copy(&mux, obs_data_get_string(settings, "muxer_settings"));
		obs_data_release(settings);
	} else {
		dstr_copy(&mux, stream->muxer_settings.array);
	}

	log_muxer_params(stream, mux.array);
	os_process_args_add_arg(args, mux.array ? mux.array : "");

	dstr_free(&mux);
}

static void build_command_line(struct ffmpeg_muxer *stream, os_process_args_t **args, const char *path)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	obs_encoder_t *aencoders[MAX_AUDIO_MIXES];
	int num_tracks = 0;

	for (;;) {
		obs_encoder_t *aencoder = obs_output_get_audio_encoder(stream->output, num_tracks);
		if (!aencoder)
			break;

		aencoders[num_tracks] = aencoder;
		num_tracks++;
	}

	char *exe = os_get_executable_path_ptr(FFMPEG_MUX);
	*args = os_process_args_create(exe);
	bfree(exe);

	dstr_copy(&stream->path, path);
	os_process_args_add_arg(*args, path);
	os_process_args_add_argf(*args, "%d", vencoder ? 1 : 0);
	os_process_args_add_argf(*args, "%d", num_tracks);

	if (vencoder)
		add_video_encoder_params(stream, *args, vencoder);

	if (num_tracks) {
		os_process_args_add_arg(*args, obs_encoder_get_codec(aencoders[0]));

		for (int i = 0; i < num_tracks; i++) {
			add_audio_encoder_params(*args, aencoders[i]);
		}
	}

	add_stream_key(*args, stream);
	add_muxer_params(*args, stream);
}

void start_pipe(struct ffmpeg_muxer *stream, const char *path)
{
	os_process_args_t *args = NULL;
	build_command_line(stream, &args, path);
	stream->pipe = os_process_pipe_create2(args, "w");
	os_process_args_destroy(args);
}

static void set_file_not_readable_error(struct ffmpeg_muxer *stream, obs_data_t *settings, const char *path)
{
	struct dstr error_message;
	dstr_init_copy(&error_message, obs_module_text("UnableToWritePath"));
#ifdef _WIN32
	/* special warning for Windows 10 users about Defender */
	struct win_version_info ver;
	get_win_ver(&ver);
	if (ver.major >= 10) {
		dstr_cat(&error_message, "\n\n");
		dstr_cat(&error_message, obs_module_text("WarnWindowsDefender"));
	}
#endif
	dstr_replace(&error_message, "%1", path);
	obs_output_set_last_error(stream->output, error_message.array);
	dstr_free(&error_message);
	obs_data_release(settings);
}

inline static void ts_offset_clear(struct ffmpeg_muxer *stream)
{
	stream->found_video = false;
	stream->video_pts_offset = 0;

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		stream->found_audio[i] = false;
		stream->audio_dts_offsets[i] = 0;
	}
}

static inline int64_t packet_pts_usec(struct encoder_packet *packet)
{
	return packet->pts * 1000000 / packet->timebase_den;
}

inline static void ts_offset_update(struct ffmpeg_muxer *stream, struct encoder_packet *packet)
{
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (!stream->found_video) {
			stream->video_pts_offset = packet->pts;
			stream->found_video = true;
		}
		return;
	}

	if (stream->found_audio[packet->track_idx])
		return;

	stream->audio_dts_offsets[packet->track_idx] = packet->dts;
	stream->found_audio[packet->track_idx] = true;
}

static inline void update_encoder_settings(struct ffmpeg_muxer *stream, const char *path)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	const char *ext = strrchr(path, '.');

	/* if using m3u8, repeat headers */
	if (ext && strcmp(ext, ".m3u8") == 0) {
		obs_data_t *settings = obs_encoder_get_settings(vencoder);
		obs_data_set_bool(settings, "repeat_headers", true);
		obs_encoder_update(vencoder, settings);
		obs_data_release(settings);
	}
}

static inline bool ffmpeg_mux_start_internal(struct ffmpeg_muxer *stream, obs_data_t *settings)
{
	const char *path = obs_data_get_string(settings, "path");

	update_encoder_settings(stream, path);

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	if (stream->is_network) {
		obs_service_t *service;
		service = obs_output_get_service(stream->output);
		if (!service)
			return false;
		path = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		stream->split_file = false;
	} else {

		stream->max_time = obs_data_get_int(settings, "max_time_sec") * 1000000LL;
		stream->max_size = obs_data_get_int(settings, "max_size_mb") * (1024 * 1024);
		stream->split_file = obs_data_get_bool(settings, "split_file");
		stream->allow_overwrite = obs_data_get_bool(settings, "allow_overwrite");
		stream->cur_size = 0;
		stream->sent_headers = false;
	}

	ts_offset_clear(stream);

	if (!stream->is_network) {
		/* ensure output path is writable to avoid generic error
		 * message.
		 *
		 * TODO: remove once ffmpeg-mux is refactored to pass
		 * errors back */
		FILE *test_file = os_fopen(path, "wb");
		if (!test_file) {
			set_file_not_readable_error(stream, settings, path);
			return false;
		}

		fclose(test_file);
		os_unlink(path);
	}

	start_pipe(stream, path);

	if (!stream->pipe) {
		obs_output_set_last_error(stream->output, obs_module_text("HelperProcessFailed"));
		warn("Failed to create process pipe");
		return false;
	}

	/* write headers and start capture */
	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	os_atomic_set_bool(&stream->stopping, false);
	stream->total_bytes = 0;
	obs_output_begin_data_capture(stream->output, 0);

	info("Writing file '%s'...", stream->path.array);
	return true;
}

static bool ffmpeg_mux_start(void *data)
{
	struct ffmpeg_muxer *stream = data;

	obs_data_t *settings = obs_output_get_settings(stream->output);
	bool success = ffmpeg_mux_start_internal(stream, settings);
	obs_data_release(settings);

	return success;
}

int deactivate(struct ffmpeg_muxer *stream, int code)
{
	int ret = -1;

	if (stream->is_hls) {
		if (stream->mux_thread_joinable) {
			os_event_signal(stream->stop_event);
			os_sem_post(stream->write_sem);
			pthread_join(stream->mux_thread, NULL);
			stream->mux_thread_joinable = false;
		}
	}

	if (active(stream)) {
		ret = os_process_pipe_destroy(stream->pipe);
		stream->pipe = NULL;

		os_atomic_set_bool(&stream->active, false);
		os_atomic_set_bool(&stream->sent_headers, false);

		info("Output of file '%s' stopped",
		     dstr_is_empty(&stream->printable_path) ? stream->path.array : stream->printable_path.array);
	}

	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else if (stopping(stream)) {
		obs_output_end_data_capture(stream->output);
	}

	if (stream->is_hls) {
		pthread_mutex_lock(&stream->write_mutex);

		while (stream->packets.size) {
			struct encoder_packet packet;
			deque_pop_front(&stream->packets, &packet, sizeof(packet));
			obs_encoder_packet_release(&packet);
		}

		pthread_mutex_unlock(&stream->write_mutex);
	}

	os_atomic_set_bool(&stream->stopping, false);
	return ret;
}

void ffmpeg_mux_stop(void *data, uint64_t ts)
{
	struct ffmpeg_muxer *stream = data;

	if (capturing(stream) || ts == 0) {
		stream->stop_ts = (int64_t)ts / 1000LL;
		os_atomic_set_bool(&stream->stopping, true);
		os_atomic_set_bool(&stream->capturing, false);
	}
}

static void signal_failure(struct ffmpeg_muxer *stream)
{
	char error[1024];
	int ret;
	int code;

	size_t len;

	len = os_process_pipe_read_err(stream->pipe, (uint8_t *)error, sizeof(error) - 1);

	if (len > 0) {
		error[len] = 0;
		warn("ffmpeg-mux: %s", error);
		obs_output_set_last_error(stream->output, error);
	}

	ret = deactivate(stream, 0);

	switch (ret) {
	case FFM_UNSUPPORTED:
		code = OBS_OUTPUT_UNSUPPORTED;
		break;
	default:
		if (stream->is_network) {
			code = OBS_OUTPUT_DISCONNECTED;
		} else {
			code = OBS_OUTPUT_ENCODE_ERROR;
		}
	}

	obs_output_signal_stop(stream->output, code);
	os_atomic_set_bool(&stream->capturing, false);
}

static void find_best_filename(struct dstr *path, bool space)
{
	int num = 2;

	if (!os_file_exists(path->array))
		return;

	const char *ext = strrchr(path->array, '.');
	if (!ext)
		return;

	size_t extstart = ext - path->array;
	struct dstr testpath;
	dstr_init_copy_dstr(&testpath, path);
	for (;;) {
		dstr_resize(&testpath, extstart);
		dstr_catf(&testpath, space ? " (%d)" : "_%d", num++);
		dstr_cat(&testpath, ext);

		if (!os_file_exists(testpath.array)) {
			dstr_free(path);
			dstr_init_move(path, &testpath);
			break;
		}
	}
}

static void generate_filename(struct ffmpeg_muxer *stream, struct dstr *dst, bool overwrite)
{
	obs_data_t *settings = obs_output_get_settings(stream->output);
	const char *dir = obs_data_get_string(settings, "directory");
	const char *fmt = obs_data_get_string(settings, "format");
	const char *ext = obs_data_get_string(settings, "extension");
	bool space = obs_data_get_bool(settings, "allow_spaces");

	char *filename = os_generate_formatted_filename(ext, space, fmt);

	dstr_copy(dst, dir);
	dstr_replace(dst, "\\", "/");
	if (dstr_end(dst) != '/')
		dstr_cat_ch(dst, '/');
	dstr_cat(dst, filename);

	char *slash = strrchr(dst->array, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(dst->array);
		*slash = '/';
	}

	if (!overwrite)
		find_best_filename(dst, space);

	bfree(filename);
	obs_data_release(settings);
}

bool write_packet(struct ffmpeg_muxer *stream, struct encoder_packet *packet)
{
	bool is_video = packet->type == OBS_ENCODER_VIDEO;
	size_t ret;

	struct ffm_packet_info info = {.pts = packet->pts,
				       .dts = packet->dts,
				       .size = (uint32_t)packet->size,
				       .index = (int)packet->track_idx,
				       .type = is_video ? FFM_PACKET_VIDEO : FFM_PACKET_AUDIO,
				       .keyframe = packet->keyframe};

	if (stream->split_file) {
		if (is_video) {
			info.dts -= stream->video_pts_offset;
			info.pts -= stream->video_pts_offset;
		} else {
			info.dts -= stream->audio_dts_offsets[info.index];
			info.pts -= stream->audio_dts_offsets[info.index];
		}
	}

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)&info, sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		/* Replay saves and selection exports run on their own helper
		 * process; a failure there must fail that file only, not stop the
		 * live replay buffer through signal_failure(). */
		if (!stream->replay_mux)
			signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(stream->pipe, packet->data, packet->size);
	if (ret != packet->size) {
		warn("os_process_pipe_write for packet data failed");
		if (!stream->replay_mux)
			signal_failure(stream);
		return false;
	}

	stream->total_bytes += packet->size;

	if (stream->split_file)
		stream->cur_size += packet->size;

	return true;
}

static bool send_audio_headers(struct ffmpeg_muxer *stream, obs_encoder_t *aencoder, size_t idx)
{
	struct encoder_packet packet = {.type = OBS_ENCODER_AUDIO, .timebase_den = 1, .track_idx = idx};

	if (!obs_encoder_get_extra_data(aencoder, &packet.data, &packet.size))
		return false;
	return write_packet(stream, &packet);
}

static bool send_video_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);

	struct encoder_packet packet = {.type = OBS_ENCODER_VIDEO, .timebase_den = 1};

	if (!obs_encoder_get_extra_data(vencoder, &packet.data, &packet.size))
		return false;
	return write_packet(stream, &packet);
}

bool send_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *aencoder;
	size_t idx = 0;

	if (!send_video_headers(stream))
		return false;

	do {
		aencoder = obs_output_get_audio_encoder(stream->output, idx);
		if (aencoder) {
			if (!send_audio_headers(stream, aencoder, idx)) {
				return false;
			}
			idx++;
		}
	} while (aencoder);

	return true;
}

static inline bool should_split(struct ffmpeg_muxer *stream, struct encoder_packet *packet)
{
	/* split at video frame */
	if (packet->type != OBS_ENCODER_VIDEO)
		return false;

	/* don't split group of pictures */
	if (!packet->keyframe)
		return false;

	if (os_atomic_load_bool(&stream->manual_split))
		return true;

	/* reached maximum file size */
	if (stream->max_size > 0 && stream->cur_size + (int64_t)packet->size >= stream->max_size)
		return true;

	/* reached maximum duration */
	if (stream->max_time > 0 && packet->dts_usec - stream->cur_time >= stream->max_time)
		return true;

	return false;
}

static bool send_new_filename(struct ffmpeg_muxer *stream, const char *filename)
{
	size_t ret;
	uint32_t size = (uint32_t)strlen(filename);
	struct ffm_packet_info info = {.type = FFM_PACKET_CHANGE_FILE, .size = size};

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)&info, sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(stream->pipe, (const uint8_t *)filename, size);
	if (ret != size) {
		warn("os_process_pipe_write for packet data failed");
		signal_failure(stream);
		return false;
	}

	return true;
}

static bool prepare_split_file(struct ffmpeg_muxer *stream, struct encoder_packet *packet)
{
	generate_filename(stream, &stream->path, stream->allow_overwrite);
	info("Changing output file to '%s'", stream->path.array);

	if (!send_new_filename(stream, stream->path.array)) {
		warn("Failed to send new file name");
		return false;
	}

	calldata_t cd = {0};
	signal_handler_t *sh = obs_output_get_signal_handler(stream->output);
	calldata_set_string(&cd, "next_file", stream->path.array);
	signal_handler_signal(sh, "file_changed", &cd);
	calldata_free(&cd);

	if (!send_headers(stream))
		return false;

	stream->cur_size = 0;
	stream->cur_time = packet->dts_usec;
	ts_offset_clear(stream);

	return true;
}

static inline bool has_audio(struct ffmpeg_muxer *stream)
{
	return !!obs_output_get_audio_encoder(stream->output, 0);
}

static void push_back_packet(mux_packets_t *packets, struct encoder_packet *packet)
{
	struct encoder_packet pkt;
	obs_encoder_packet_ref(&pkt, packet);
	da_push_back(*packets, &pkt);
}

static void ffmpeg_mux_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;

	if (!active(stream))
		return;

	/* encoder failure */
	if (!packet) {
		deactivate(stream, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (stream->split_file && stream->mux_packets.num) {
		int64_t pts_usec = packet_pts_usec(packet);
		struct encoder_packet *first_pkt = stream->mux_packets.array;
		int64_t first_pts_usec = packet_pts_usec(first_pkt);

		if (pts_usec >= first_pts_usec) {
			if (packet->type != OBS_ENCODER_AUDIO) {
				push_back_packet(&stream->mux_packets, packet);
				return;
			}

			if (!prepare_split_file(stream, first_pkt))
				return;
			stream->split_file_ready = true;
		}
	} else if (stream->split_file && should_split(stream, packet)) {
		if (has_audio(stream)) {
			push_back_packet(&stream->mux_packets, packet);
			return;
		} else {
			if (!prepare_split_file(stream, packet))
				return;
			stream->split_file_ready = true;
		}
	}

	if (!stream->sent_headers) {
		if (!send_headers(stream))
			return;

		stream->sent_headers = true;

		if (stream->split_file)
			stream->cur_time = packet->dts_usec;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			deactivate(stream, 0);
			return;
		}
	}

	if (stream->split_file && stream->split_file_ready) {
		for (size_t i = 0; i < stream->mux_packets.num; i++) {
			struct encoder_packet *pkt = &stream->mux_packets.array[i];
			ts_offset_update(stream, pkt);
			write_packet(stream, pkt);
			obs_encoder_packet_release(pkt);
		}
		da_free(stream->mux_packets);
		stream->split_file_ready = false;
		os_atomic_set_bool(&stream->manual_split, false);
	}

	if (stream->split_file)
		ts_offset_update(stream, packet);

	write_packet(stream, packet);
}

static obs_properties_t *ffmpeg_mux_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "path", obs_module_text("FilePath"), OBS_TEXT_DEFAULT);
	return props;
}

uint64_t ffmpeg_mux_total_bytes(void *data)
{
	struct ffmpeg_muxer *stream = data;
	return stream->total_bytes;
}

struct obs_output_info ffmpeg_muxer = {
	.id = "ffmpeg_muxer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK | OBS_OUTPUT_CAN_PAUSE,
	.get_name = ffmpeg_mux_getname,
	.create = ffmpeg_mux_create,
	.destroy = ffmpeg_mux_destroy,
	.start = ffmpeg_mux_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = ffmpeg_mux_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_properties = ffmpeg_mux_properties,
};

static int connect_time(struct ffmpeg_muxer *stream)
{
	UNUSED_PARAMETER(stream);
	/* TODO */
	return 0;
}

#ifndef NEW_MPEGTS_OUTPUT
static int ffmpeg_mpegts_mux_connect_time(void *data)
{
	struct ffmpeg_muxer *stream = data;
	/* TODO */
	return connect_time(stream);
}

struct obs_output_info ffmpeg_mpegts_muxer = {
	.id = "ffmpeg_mpegts_muxer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK | OBS_OUTPUT_SERVICE,
	.protocols = "SRT;RIST",
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "aac;opus",
	.get_name = ffmpeg_mpegts_mux_getname,
	.create = ffmpeg_mux_create,
	.destroy = ffmpeg_mux_destroy,
	.start = ffmpeg_mux_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = ffmpeg_mux_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_properties = ffmpeg_mux_properties,
	.get_connect_time_ms = ffmpeg_mpegts_mux_connect_time,
};
#endif
/* ------------------------------------------------------------------------ */

static const char *replay_buffer_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("ReplayBuffer");
}

static void save_replay_proc(void *data, calldata_t *cd)
{

	UNUSED_PARAMETER(cd);
	struct ffmpeg_muxer *stream = data;

	if (os_atomic_load_bool(&stream->active)) {
		obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
		if (obs_encoder_paused(vencoder)) {
			info("Could not save buffer because the encoder is paused");
			return;
		}

		stream->save_ts = os_gettime_ns() / 1000LL;
	}
}

static void get_last_replay(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	if (!os_atomic_load_bool(&stream->muxing))
		calldata_set_string(cd, "path", stream->path.array);
}

static void get_replay_buffer_info(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	int64_t start_usec = 0;
	int64_t end_usec = 0;

	replay_buffer_cache_get_time_range(stream->replay_cache, &start_usec, &end_usec);

	calldata_set_int(cd, "start_time_usec", start_usec);
	calldata_set_int(cd, "end_time_usec", end_usec);
	calldata_set_int(cd, "keyframe_count", (long long)replay_buffer_cache_get_keyframe_count(stream->replay_cache));
	/* The configured length limit, not the length buffered so far, so the UI can
	 * draw a fixed timeline instead of one that grows while the buffer fills. */
	calldata_set_int(cd, "max_time_usec", stream->max_time);
}

static void get_replay_keyframe_info(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const long long index = calldata_int(cd, "index");
	struct replay_buffer_keyframe_info frame_info = {0};

	if (index >= 0)
		replay_buffer_cache_get_keyframe_info(stream->replay_cache, (size_t)index, &frame_info);

	calldata_set_int(cd, "seq", (long long)frame_info.seq);
	calldata_set_int(cd, "dts_usec", frame_info.dts_usec);
	calldata_set_int(cd, "duration_usec", frame_info.duration_usec);
}

static void find_replay_keyframe(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const int64_t time_usec = calldata_int(cd, "time_usec");
	struct replay_buffer_keyframe_info frame_info = {0};
	size_t index = 0;

	/* The buffer keeps rolling between the two calls, so the index may have
	 * been evicted. Report no keyframe instead of a zero timestamp. */
	if (replay_buffer_cache_find_keyframe(stream->replay_cache, time_usec, &index) &&
	    replay_buffer_cache_get_keyframe_info(stream->replay_cache, index, &frame_info)) {
		calldata_set_int(cd, "index", (long long)index);
		calldata_set_int(cd, "dts_usec", frame_info.dts_usec);
	} else {
		calldata_set_int(cd, "index", -1);
		calldata_set_int(cd, "dts_usec", 0);
	}
}

static void find_prev_replay_keyframe(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const int64_t time_usec = calldata_int(cd, "time_usec");
	struct replay_buffer_keyframe_info frame_info = {0};
	size_t index = 0;

	if (replay_buffer_cache_find_prev_keyframe(stream->replay_cache, time_usec, &index) &&
	    replay_buffer_cache_get_keyframe_info(stream->replay_cache, index, &frame_info)) {
		calldata_set_int(cd, "index", (long long)index);
		calldata_set_int(cd, "dts_usec", frame_info.dts_usec);
	} else {
		calldata_set_int(cd, "index", -1);
		calldata_set_int(cd, "dts_usec", 0);
	}
}

static void find_next_replay_keyframe(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const int64_t time_usec = calldata_int(cd, "time_usec");
	struct replay_buffer_keyframe_info frame_info = {0};
	size_t index = 0;

	if (replay_buffer_cache_find_next_keyframe(stream->replay_cache, time_usec, &index) &&
	    replay_buffer_cache_get_keyframe_info(stream->replay_cache, index, &frame_info)) {
		calldata_set_int(cd, "index", (long long)index);
		calldata_set_int(cd, "dts_usec", frame_info.dts_usec);
	} else {
		calldata_set_int(cd, "index", -1);
		calldata_set_int(cd, "dts_usec", 0);
	}
}

static void request_replay_full_frame(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const long long seq_int = calldata_int(cd, "seq");
	const uint64_t request_id = (uint64_t)calldata_int(cd, "request_id");
	bool accepted = false;

	if (seq_int >= 0) {
		pthread_mutex_lock(&stream->replay_buffer_mutex);
		if (stream->frame_decoder)
			accepted = replay_buffer_frame_decoder_request(stream->frame_decoder, (uint64_t)seq_int,
								       request_id);
		pthread_mutex_unlock(&stream->replay_buffer_mutex);
	}

	calldata_set_int(cd, "accepted", accepted ? 1 : 0);
}

static void copy_replay_full_frame(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const uint64_t request_id = (uint64_t)calldata_int(cd, "request_id");
	const long long capacity_int = calldata_int(cd, "capacity");
	const size_t capacity = capacity_int > 0 ? (size_t)capacity_int : 0;
	uint8_t *dst = calldata_ptr(cd, "dst");
	int status = REPLAY_BUFFER_FRAME_FAILED;
	uint32_t width = 0;
	uint32_t height = 0;
	size_t size = 0;

	pthread_mutex_lock(&stream->replay_buffer_mutex);
	if (stream->frame_decoder)
		replay_buffer_frame_decoder_copy(stream->frame_decoder, request_id, dst, capacity, &status, &width,
						 &height, &size);
	pthread_mutex_unlock(&stream->replay_buffer_mutex);

	calldata_set_int(cd, "status", status);
	calldata_set_int(cd, "width", width);
	calldata_set_int(cd, "height", height);
	calldata_set_int(cd, "size", (long long)size);
}

static bool replay_buffer_begin_mux(struct ffmpeg_muxer *stream);
static void replay_buffer_prepare_packets(struct ffmpeg_muxer *stream, replay_buffer_snapshot_t *snapshot);
static void replay_buffer_make_temp_path(const char *final_path, struct dstr *temp_path);
static void replay_buffer_free_mux_packets(struct ffmpeg_muxer *stream);
static void *replay_buffer_mux_thread(void *data);

static void snap_replay_selection(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	int64_t actual_start = 0;
	int64_t actual_end = 0;

	const bool valid = replay_buffer_cache_resolve_range(stream->replay_cache, calldata_int(cd, "start_usec"),
							     calldata_int(cd, "end_usec"), &actual_start, &actual_end);

	calldata_set_bool(cd, "valid", valid);
	calldata_set_int(cd, "actual_start_usec", actual_start);
	calldata_set_int(cd, "actual_end_usec", actual_end);
}

static void get_replay_export_status(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;

	calldata_set_int(cd, "state", os_atomic_load_long(&stream->export_state));
	calldata_set_int(cd, "error", os_atomic_load_long(&stream->export_error));
}

static void export_replay_selection(void *data, calldata_t *cd)
{
	struct ffmpeg_muxer *stream = data;
	const int64_t start_usec = calldata_int(cd, "start_usec");
	const int64_t end_usec = calldata_int(cd, "end_usec");
	int64_t actual_start = 0;
	int64_t actual_end = 0;
	int result = REPLAY_EXPORT_ERR_INVALID;

	if (!active(stream)) {
		result = REPLAY_EXPORT_ERR_INACTIVE;
	} else if (start_usec >= end_usec) {
		result = REPLAY_EXPORT_ERR_INVALID;
	} else if (!replay_buffer_cache_resolve_range(stream->replay_cache, start_usec, end_usec, &actual_start,
						      &actual_end)) {
		result = REPLAY_EXPORT_ERR_INVALID;
	} else if (!replay_buffer_begin_mux(stream)) {
		result = REPLAY_EXPORT_ERR_BUSY;
	} else {
		replay_buffer_snapshot_t *snapshot = replay_buffer_cache_create_range_snapshot(
			stream->replay_cache, start_usec, end_usec, &actual_start, &actual_end);

		if (!snapshot) {
			os_atomic_set_bool(&stream->muxing, false);
			result = REPLAY_EXPORT_ERR_INVALID;
		} else {
			stream->selection_export = true;
			replay_buffer_prepare_packets(stream, snapshot);
			replay_buffer_snapshot_release(snapshot);

			/* Write to a temporary file first so a failed export never
			 * leaves a partial file behind. */
			generate_filename(stream, &stream->selection_path, false);
			replay_buffer_make_temp_path(stream->selection_path.array, &stream->path);
			os_unlink(stream->path.array);

			os_atomic_set_long(&stream->export_error, REPLAY_EXPORT_ERR_NONE);
			os_atomic_set_long(&stream->export_state, REPLAY_EXPORT_RUNNING);

			stream->mux_thread_joinable =
				pthread_create(&stream->mux_thread, NULL, replay_buffer_mux_thread, stream) == 0;
			if (!stream->mux_thread_joinable) {
				warn("Failed to create replay selection muxer thread");
				replay_buffer_free_mux_packets(stream);
				stream->selection_export = false;
				os_atomic_set_long(&stream->export_state, REPLAY_EXPORT_FAILED);
				os_atomic_set_long(&stream->export_error, REPLAY_EXPORT_ERR_WRITE);
				os_atomic_set_bool(&stream->muxing, false);
				result = REPLAY_EXPORT_ERR_WRITE;
			} else {
				result = REPLAY_EXPORT_ERR_NONE;
			}
		}
	}

	calldata_set_int(cd, "result", result);
	calldata_set_int(cd, "actual_start_usec", actual_start);
	calldata_set_int(cd, "actual_end_usec", actual_end);
}

static void *replay_buffer_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	stream->output = output;

	if (pthread_mutex_init(&stream->replay_buffer_mutex, NULL) != 0) {
		bfree(stream);
		return NULL;
	}
	stream->replay_buffer_mutex_initialized = true;

	if (pthread_mutex_init(&stream->save_mutex, NULL) != 0) {
		pthread_mutex_destroy(&stream->replay_buffer_mutex);
		stream->replay_buffer_mutex_initialized = false;
		bfree(stream);
		return NULL;
	}
	stream->save_mutex_initialized = true;

	stream->replay_cache = replay_buffer_cache_create(0);
	if (!stream->replay_cache) {
		pthread_mutex_destroy(&stream->save_mutex);
		pthread_mutex_destroy(&stream->replay_buffer_mutex);
		bfree(stream);
		return NULL;
	}

	os_atomic_set_long(&stream->export_state, REPLAY_EXPORT_IDLE);
	os_atomic_set_long(&stream->export_error, REPLAY_EXPORT_ERR_NONE);

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_add(ph, "void save()", save_replay_proc, stream);
	proc_handler_add(ph, "void get_last_replay(out string path)", get_last_replay, stream);
	proc_handler_add(ph,
			 "void get_replay_buffer_info(out int start_time_usec, out int end_time_usec, "
			 "out int keyframe_count, out int max_time_usec)",
			 get_replay_buffer_info, stream);
	proc_handler_add(ph,
			 "void get_replay_keyframe_info(int index, out int seq, out int dts_usec, "
			 "out int duration_usec)",
			 get_replay_keyframe_info, stream);
	proc_handler_add(ph, "void find_replay_keyframe(int time_usec, out int index, out int dts_usec)",
			 find_replay_keyframe, stream);
	proc_handler_add(ph, "void find_prev_replay_keyframe(int time_usec, out int index, out int dts_usec)",
			 find_prev_replay_keyframe, stream);
	proc_handler_add(ph, "void find_next_replay_keyframe(int time_usec, out int index, out int dts_usec)",
			 find_next_replay_keyframe, stream);
	proc_handler_add(ph, "void request_replay_full_frame(int seq, int request_id, out int accepted)",
			 request_replay_full_frame, stream);
	proc_handler_add(ph,
			 "void copy_replay_full_frame(int request_id, int capacity, in ptr dst, "
			 "out int status, out int width, out int height, out int size)",
			 copy_replay_full_frame, stream);
	proc_handler_add(ph,
			 "void snap_replay_selection(int start_usec, int end_usec, out int valid, "
			 "out int actual_start_usec, out int actual_end_usec)",
			 snap_replay_selection, stream);
	proc_handler_add(ph,
			 "void export_replay_selection(int start_usec, int end_usec, out int result, "
			 "out int actual_start_usec, out int actual_end_usec)",
			 export_replay_selection, stream);
	proc_handler_add(ph, "void get_replay_export_status(out int state, out int error)", get_replay_export_status,
			 stream);

	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_add(sh, "void saved()");
	signal_handler_add(sh, "void saving(ptr output)");

	return stream;
}

static void replay_buffer_destroy(void *data)
{
	ffmpeg_mux_destroy(data);
}

static bool replay_buffer_start(void *data)
{
	struct ffmpeg_muxer *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	obs_data_t *s = obs_output_get_settings(stream->output);
	stream->max_time = obs_data_get_int(s, "max_time_sec") * 1000000LL;
	stream->max_size = obs_data_get_int(s, "max_size_mb") * (1024 * 1024);
	obs_data_release(s);

	replay_buffer_cache_set_limits(stream->replay_cache, stream->max_size, stream->max_time);

	pthread_mutex_lock(&stream->replay_buffer_mutex);
	replay_buffer_frame_decoder_destroy(stream->frame_decoder);
	stream->frame_decoder =
		replay_buffer_frame_decoder_create(obs_output_get_video_encoder(stream->output), stream->replay_cache);
	pthread_mutex_unlock(&stream->replay_buffer_mutex);

	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	stream->total_bytes = 0;
	obs_output_begin_data_capture(stream->output, 0);

	return true;
}

static void insert_packet(mux_packets_t *packets, struct encoder_packet *packet, int64_t video_offset,
			  int64_t *audio_offsets, int64_t video_pts_offset, int64_t *audio_dts_offsets)
{
	struct encoder_packet pkt;
	size_t idx;

	obs_encoder_packet_ref(&pkt, packet);

	if (pkt.type == OBS_ENCODER_VIDEO) {
		pkt.dts_usec -= video_offset;
		pkt.dts -= video_pts_offset;
		pkt.pts -= video_pts_offset;
	} else {
		pkt.dts_usec -= audio_offsets[pkt.track_idx];
		pkt.dts -= audio_dts_offsets[pkt.track_idx];
		pkt.pts -= audio_dts_offsets[pkt.track_idx];
	}

	for (idx = packets->num; idx > 0; idx--) {
		struct encoder_packet *p = packets->array + (idx - 1);
		if (p->dts_usec < pkt.dts_usec)
			break;
	}

	da_insert(*packets, idx, &pkt);
}

static void replay_buffer_free_mux_packets(struct ffmpeg_muxer *stream)
{
	for (size_t i = 0; i < stream->mux_packets.num; i++)
		obs_encoder_packet_release(&stream->mux_packets.array[i]);
	da_free(stream->mux_packets);
}

static void replay_buffer_make_temp_path(const char *final_path, struct dstr *temp_path)
{
	const char *slash = strrchr(final_path, '/');
	const char *dot = strrchr(final_path, '.');

	dstr_copy(temp_path, final_path);
	/* Keep the original extension so the ffmpeg helper can still infer the
	 * container from the path. */
	if (dot && (!slash || dot > slash))
		dstr_insert(temp_path, (size_t)(dot - final_path), ".part");
	else
		dstr_cat(temp_path, ".part");
}

static void *replay_buffer_mux_thread(void *data)
{
	struct ffmpeg_muxer *stream = data;
	const bool selection_export = stream->selection_export;
	bool error = false;

	/* This thread owns its own helper process: write failures are reported
	 * through the mux result and must not deactivate the replay buffer. */
	stream->replay_mux = true;

	start_pipe(stream, stream->path.array);

	if (!stream->pipe) {
		warn("Failed to create process pipe");
		error = true;
		goto finish;
	}

	if (!send_headers(stream)) {
		warn("Could not write headers for file '%s'", stream->path.array);
		error = true;
		goto finish;
	}

	for (size_t i = 0; i < stream->mux_packets.num; i++) {
		struct encoder_packet *pkt = &stream->mux_packets.array[i];
		if (!write_packet(stream, pkt)) {
			warn("Could not write packet for file '%s'", stream->path.array);
			error = true;
			goto finish;
		}
	}

finish: {
	/* The muxer exit code is only available after the pipe is closed, so
		 * check it before renaming the temporary file into place. Whole-buffer
		 * saves keep their existing behaviour and ignore the exit code. */
	const int exit_code = os_process_pipe_destroy(stream->pipe);
	stream->pipe = NULL;
	if (selection_export && exit_code != 0) {
		warn("Replay selection muxer exited with code %d", exit_code);
		error = true;
	}
}

	for (size_t i = 0; i < stream->mux_packets.num; i++)
		obs_encoder_packet_release(&stream->mux_packets.array[i]);
	da_free(stream->mux_packets);

	if (selection_export) {
		if (!error) {
			if (os_rename(stream->path.array, stream->selection_path.array) != 0) {
				warn("Failed to rename '%s' to '%s'", stream->path.array, stream->selection_path.array);
				error = true;
			} else {
				dstr_copy(&stream->path, stream->selection_path.array);
				info("Wrote replay selection to '%s'", stream->path.array);
			}
		}

		/* Never leave a partial file behind, neither the temporary one nor
		 * an unusable renamed result. */
		if (error)
			os_unlink(stream->path.array);

		os_atomic_set_long(&stream->export_error, error ? REPLAY_EXPORT_ERR_WRITE : REPLAY_EXPORT_ERR_NONE);
		os_atomic_set_long(&stream->export_state, error ? REPLAY_EXPORT_FAILED : REPLAY_EXPORT_DONE);
		stream->selection_export = false;
	} else if (!error) {
		info("Wrote replay buffer to '%s'", stream->path.array);
	}

	/* muxing must be cleared before signalling: get_last_replay only returns
	 * the path when no mux is in flight. Nothing else may touch the exporter
	 * state after this point. */
	os_atomic_set_bool(&stream->muxing, false);
	stream->replay_mux = false;

	if (!error) {
		calldata_t cd = {0};
		signal_handler_t *sh = obs_output_get_signal_handler(stream->output);
		signal_handler_signal(sh, "saved", &cd);
	}

	return NULL;
}

/* Serializes whole-buffer saves and selection exports. Only the start is
 * guarded: the mux thread runs without the lock and muxing keeps the other
 * path out while it writes. Called from the output thread (save) and from the
 * UI thread (export); never blocks on a running mux. */
static bool replay_buffer_begin_mux(struct ffmpeg_muxer *stream)
{
	bool started = false;

	pthread_mutex_lock(&stream->save_mutex);

	if (!os_atomic_load_bool(&stream->muxing)) {
		if (stream->mux_thread_joinable) {
			pthread_join(stream->mux_thread, NULL);
			stream->mux_thread_joinable = false;
		}

		os_atomic_set_bool(&stream->muxing, true);
		started = true;
	}

	pthread_mutex_unlock(&stream->save_mutex);
	return started;
}

static void replay_buffer_prepare_packets(struct ffmpeg_muxer *stream, replay_buffer_snapshot_t *snapshot)
{
	const size_t num_packets = replay_buffer_snapshot_get_packet_count(snapshot);

	da_reserve(stream->mux_packets, num_packets);

	/* ---------------------------- */
	/* reorder packets */

	bool found_video = false;
	bool found_audio[MAX_AUDIO_MIXES] = {0};
	int64_t video_offset = 0;
	int64_t video_pts_offset = 0;
	int64_t audio_offsets[MAX_AUDIO_MIXES] = {0};
	int64_t audio_dts_offsets[MAX_AUDIO_MIXES] = {0};

	for (size_t i = 0; i < num_packets; i++) {
		struct encoder_packet *pkt = replay_buffer_snapshot_get_packet(snapshot, i);

		if (pkt->type == OBS_ENCODER_VIDEO) {
			if (!found_video) {
				video_pts_offset = pkt->pts;
				video_offset = video_pts_offset * 1000000 / pkt->timebase_den;
				found_video = true;
			}
		} else {
			if (!found_audio[pkt->track_idx]) {
				found_audio[pkt->track_idx] = true;
				audio_offsets[pkt->track_idx] = pkt->dts_usec;
				audio_dts_offsets[pkt->track_idx] = pkt->dts;
			}
		}

		insert_packet(&stream->mux_packets, pkt, video_offset, audio_offsets, video_pts_offset,
			      audio_dts_offsets);
	}
}

static bool replay_buffer_save(struct ffmpeg_muxer *stream)
{
	if (!replay_buffer_begin_mux(stream))
		return false;

	replay_buffer_snapshot_t *snapshot = replay_buffer_cache_create_snapshot(stream->replay_cache);
	if (!snapshot) {
		warn("Failed to create replay buffer snapshot");
		os_atomic_set_bool(&stream->muxing, false);
		return false;
	}

	stream->selection_export = false;
	replay_buffer_prepare_packets(stream, snapshot);
	replay_buffer_snapshot_release(snapshot);

	calldata_t cd = {0};
	calldata_set_ptr(&cd, "output", stream->output);
	signal_handler_t *sh = obs_output_get_signal_handler(stream->output);
	signal_handler_signal(sh, "saving", &cd);
	calldata_free(&cd);

	generate_filename(stream, &stream->path, true);

	stream->mux_thread_joinable = pthread_create(&stream->mux_thread, NULL, replay_buffer_mux_thread, stream) == 0;
	if (!stream->mux_thread_joinable) {
		warn("Failed to create muxer thread");
		replay_buffer_free_mux_packets(stream);
		os_atomic_set_bool(&stream->muxing, false);
		return false;
	}

	return true;
}

static void deactivate_replay_buffer(struct ffmpeg_muxer *stream, int code)
{
	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else if (stopping(stream)) {
		obs_output_end_data_capture(stream->output);
	}

	os_atomic_set_bool(&stream->active, false);
	os_atomic_set_bool(&stream->sent_headers, false);
	os_atomic_set_bool(&stream->stopping, false);
	replay_buffer_clear(stream);
}

static void replay_buffer_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;

	if (!active(stream))
		return;

	/* encoder failure */
	if (!packet) {
		deactivate_replay_buffer(stream, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			deactivate_replay_buffer(stream, 0);
			return;
		}
	}

	replay_buffer_cache_push(stream->replay_cache, packet);

	if (stream->save_ts && packet->sys_dts_usec >= stream->save_ts) {
		if (os_atomic_load_bool(&stream->muxing))
			return;

		/* A selection export may have started between the check above and
		 * here; keep save_ts so the save is retried on the next packet
		 * instead of blocking the output thread. */
		if (replay_buffer_save(stream))
			stream->save_ts = 0;
	}
}

static void replay_buffer_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "max_time_sec", 15);
	obs_data_set_default_int(s, "max_size_mb", 500);
	obs_data_set_default_string(s, "format", "%CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_default_string(s, "extension", "mp4");
	obs_data_set_default_bool(s, "allow_spaces", true);
}

struct obs_output_info replay_buffer = {
	.id = "replay_buffer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK | OBS_OUTPUT_CAN_PAUSE,
	.get_name = replay_buffer_getname,
	.create = replay_buffer_create,
	.destroy = replay_buffer_destroy,
	.start = replay_buffer_start,
	.stop = ffmpeg_mux_stop,
	.encoded_packet = replay_buffer_data,
	.get_total_bytes = ffmpeg_mux_total_bytes,
	.get_defaults = replay_buffer_defaults,
};
