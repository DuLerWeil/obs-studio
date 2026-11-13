#include "obs-ffmpeg-replay-buffer-frame-decoder.h"

#include "obs-ffmpeg-compat.h"

#include <obs.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

struct replay_buffer_decoder {
	AVCodecContext *decoder;
	SwsContext *scaler;
	bool failed;
	bool has_extradata;
	int failures;
};

struct replay_buffer_frame_decoder {
	obs_encoder_t *encoder;
	replay_buffer_cache_t *cache;

	/* A single latest request: a slow decode must never queue up behind older
	 * requests while the timeline is being dragged. */
	pthread_t thread;
	bool thread_started;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool stopping;
	bool has_request;
	uint64_t seq;
	uint64_t request_id;
	uint64_t latest_id;
	bool has_result;
	uint64_t result_id;
	bool result_failed;
	uint8_t *rgba;
	uint32_t width;
	uint32_t height;
	size_t size;
	struct replay_buffer_decoder decoder;
};

/* Encoders report their codec with whatever spelling they like - Quartz reports
 * "h264", the QSV plugin reports "H.264" - so compare without case and accept
 * the usual aliases. */
static enum AVCodecID codec_id_from_name(const char *codec)
{
	if (!codec)
		return AV_CODEC_ID_NONE;

	char lower[32];
	size_t len = strlen(codec);
	if (len >= sizeof(lower))
		len = sizeof(lower) - 1;
	for (size_t i = 0; i < len; i++)
		lower[i] = (char)tolower((unsigned char)codec[i]);
	lower[len] = 0;

	if (strcmp(lower, "h264") == 0 || strcmp(lower, "h.264") == 0 || strcmp(lower, "avc") == 0 ||
	    strcmp(lower, "avc1") == 0)
		return AV_CODEC_ID_H264;
	if (strcmp(lower, "hevc") == 0 || strcmp(lower, "h265") == 0 || strcmp(lower, "h.265") == 0 ||
	    strcmp(lower, "hev1") == 0 || strcmp(lower, "hvc1") == 0)
		return AV_CODEC_ID_HEVC;
	if (strcmp(lower, "av1") == 0 || strcmp(lower, "av01") == 0)
		return AV_CODEC_ID_AV1;
	if (strcmp(lower, "prores") == 0)
		return AV_CODEC_ID_PRORES;
	if (strcmp(lower, "vp9") == 0)
		return AV_CODEC_ID_VP9;
	if (strcmp(lower, "vp8") == 0)
		return AV_CODEC_ID_VP8;

	return AV_CODEC_ID_NONE;
}

/* Everything OBS can hand the replay buffer decodes through libavcodec, so the
 * known names are mapped explicitly and anything else is looked up by name. */
static const AVCodec *find_decoder_for_codec(const char *codec_name)
{
	if (!codec_name || !*codec_name)
		return NULL;

	const enum AVCodecID codec_id = codec_id_from_name(codec_name);
	if (codec_id != AV_CODEC_ID_NONE) {
		const AVCodec *codec = avcodec_find_decoder(codec_id);
		if (codec)
			return codec;
	}

	return avcodec_find_decoder_by_name(codec_name);
}

static void decoder_thread_set_low_priority(void)
{
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
	/* On Linux nice values are per-thread; on other POSIX systems this is
	 * still a harmless best-effort hint. */
	setpriority(PRIO_PROCESS, 0, 10);
#endif
}

static void decoder_report_failure(struct replay_buffer_decoder *decoder, int code)
{
	if (decoder->failures < 3)
		blog(LOG_INFO, "[replay buffer] keyframe decode failed (%d)", code);

	decoder->failures++;
}

static bool decoder_open(replay_buffer_frame_decoder_t *frame_decoder, struct replay_buffer_decoder *decoder)
{
	if (decoder->decoder)
		return true;
	if (decoder->failed)
		return false;

	const char *codec_name = obs_encoder_get_codec(frame_decoder->encoder);
	const AVCodec *codec = find_decoder_for_codec(codec_name);
	if (!codec) {
		blog(LOG_WARNING, "[replay buffer] no keyframe decoder for codec '%s'",
		     codec_name ? codec_name : "(null)");
		decoder->failed = true;
		return false;
	}

	AVCodecContext *context = avcodec_alloc_context3(codec);
	if (!context) {
		decoder->failed = true;
		return false;
	}

	uint8_t *extra_data = NULL;
	size_t extra_size = 0;
	if (obs_encoder_get_extra_data(frame_decoder->encoder, &extra_data, &extra_size) && extra_data && extra_size) {
		context->extradata = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (context->extradata) {
			memcpy(context->extradata, extra_data, extra_size);
			context->extradata_size = (int)extra_size;
			decoder->has_extradata = true;
		}
	}

	const uint32_t width = obs_encoder_get_width(frame_decoder->encoder);
	const uint32_t height = obs_encoder_get_height(frame_decoder->encoder);
	if (width && height) {
		context->width = (int)width;
		context->height = (int)height;
	}

	/* Keep one decoding thread: the worker runs below the encoder priority and
	 * must not compete with the recording encoder. */
	context->thread_count = 1;

	const int ret = avcodec_open2(context, codec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "[replay buffer] failed to open '%s' decoder (%d)", codec_name, ret);
		avcodec_free_context(&context);
		decoder->failed = true;
		return false;
	}

	decoder->decoder = context;
	return true;
}

static bool decoder_decode(replay_buffer_frame_decoder_t *frame_decoder, struct replay_buffer_decoder *decoder,
			   const struct encoder_packet *packets, size_t count, uint8_t **out_rgba, uint32_t *out_width,
			   uint32_t *out_height)
{
	if (!packets || !count)
		return false;

	if (!packets[0].data || !packets[0].size || packets[0].size > (size_t)INT_MAX)
		return false;
	if (!decoder_open(frame_decoder, decoder))
		return false;

	AVCodecContext *context = decoder->decoder;

	/* Every request starts from a keyframe, so a clean decoder state is enough;
	 * there is no need to rebuild the decoder for each GOP. */
	avcodec_flush_buffers(context);

	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;

	bool got_frame = false;
	for (size_t i = 0; i < count && !got_frame; i++) {
		if (!packets[i].data || !packets[i].size || packets[i].size > (size_t)INT_MAX)
			break;

		AVPacket *av_packet = av_packet_alloc();
		if (!av_packet)
			break;

		if (av_new_packet(av_packet, (int)packets[i].size) < 0) {
			av_packet_free(&av_packet);
			break;
		}

		memcpy(av_packet->data, packets[i].data, packets[i].size);
		av_packet->pts = packets[i].pts;
		av_packet->dts = packets[i].dts;
		/* Without the flag a decoder that was just flushed may drop the packet
		 * instead of starting a new GOP (HEVC in particular). */
		if (packets[i].keyframe)
			av_packet->flags |= AV_PKT_FLAG_KEY;

		const int send_ret = avcodec_send_packet(context, av_packet);
		av_packet_free(&av_packet);

		if (send_ret < 0) {
			decoder_report_failure(decoder, send_ret);
			continue;
		}

		for (;;) {
			const int ret = avcodec_receive_frame(context, frame);
			if (ret == 0) {
				got_frame = true;
				break;
			}
			if (ret == AVERROR(EAGAIN))
				break;
			decoder_report_failure(decoder, ret);
			break;
		}
	}

	if (!got_frame) {
		/* Drain: some streams only output the first frame once the decoder is
		 * told there is no more input. */
		avcodec_send_packet(context, NULL);

		for (;;) {
			const int ret = avcodec_receive_frame(context, frame);
			if (ret == 0) {
				got_frame = true;
				break;
			}
			if (ret == AVERROR(EAGAIN))
				break;
			break;
		}
	}

	if (!got_frame) {
		av_frame_free(&frame);
		/* Some encoders only expose extradata after the first packet. If it
		 * was missing, allow the next keyframe to reopen the decoder with the
		 * now-available extradata. */
		if (!decoder->has_extradata)
			avcodec_free_context(&decoder->decoder);
		return false;
	}

	const int width = frame->width;
	const int height = frame->height;
	if (width <= 0 || height <= 0) {
		av_frame_free(&frame);
		return false;
	}

	const size_t row_size = (size_t)width * 4;
	if ((size_t)height > SIZE_MAX / row_size) {
		av_frame_free(&frame);
		return false;
	}

	decoder->scaler = sws_getCachedContext(decoder->scaler, frame->width, frame->height,
					       (enum AVPixelFormat)frame->format, width, height, AV_PIX_FMT_RGBA,
					       SWS_BILINEAR, NULL, NULL, NULL);
	if (!decoder->scaler) {
		av_frame_free(&frame);
		return false;
	}

	const size_t size = row_size * (size_t)height;
	uint8_t *rgba = bmalloc(size);
	if (!rgba) {
		av_frame_free(&frame);
		return false;
	}

	uint8_t *dst_data[4] = {rgba, NULL, NULL, NULL};
	int dst_linesize[4] = {(int)row_size, 0, 0, 0};
	const int scaled = sws_scale(decoder->scaler, (const uint8_t *const *)frame->data, frame->linesize, 0,
				     frame->height, dst_data, dst_linesize);
	av_frame_free(&frame);

	if (scaled <= 0) {
		bfree(rgba);
		return false;
	}

	*out_rgba = rgba;
	*out_width = (uint32_t)width;
	*out_height = (uint32_t)height;
	return true;
}
static void frame_process(replay_buffer_frame_decoder_t *frame_decoder, uint64_t seq, uint64_t request_id)
{
	uint8_t *rgba = NULL;
	uint32_t width = 0;
	uint32_t height = 0;
	bool decoded = false;

	/* Feed the requested keyframe plus the packets after it: with B-frames and an
	 * open GOP (NVENC emits CRA frames) the decoder may not output anything until
	 * it has seen a good part of the GOP, and a lone keyframe then decodes to
	 * nothing. */
	struct encoder_packet packets[33];
	size_t count = 0;
	const size_t max_packets = sizeof(packets) / sizeof(packets[0]);
	for (size_t i = 0; i < max_packets; i++) {
		struct encoder_packet packet;
		if (!replay_buffer_cache_ref_packet(frame_decoder->cache, seq + i, &packet))
			break;
		if (i == 0 && !(packet.type == OBS_ENCODER_VIDEO && packet.keyframe)) {
			obs_encoder_packet_release(&packet);
			break;
		}
		packets[count++] = packet;
	}

	if (count) {
		decoded =
			decoder_decode(frame_decoder, &frame_decoder->decoder, packets, count, &rgba, &width, &height);
		for (size_t i = 0; i < count; i++)
			obs_encoder_packet_release(&packets[i]);
	}

	pthread_mutex_lock(&frame_decoder->mutex);
	/* A newer request may have arrived while this frame was decoding; drop the
	 * stale result instead of publishing it to the UI. */
	if (!frame_decoder->stopping && request_id == frame_decoder->latest_id) {
		bfree(frame_decoder->rgba);
		frame_decoder->rgba = decoded ? rgba : NULL;
		frame_decoder->width = decoded ? width : 0;
		frame_decoder->height = decoded ? height : 0;
		frame_decoder->size = decoded ? (size_t)width * 4 * (size_t)height : 0;
		frame_decoder->result_id = request_id;
		frame_decoder->result_failed = !decoded;
		frame_decoder->has_result = true;
		rgba = NULL;
	}
	pthread_mutex_unlock(&frame_decoder->mutex);

	if (rgba)
		bfree(rgba);

	blog(LOG_INFO, "[replay buffer] frame %llu: %s (%zu packets)", (unsigned long long)request_id,
	     decoded ? "decoded" : "failed", count);
}

static void *frame_thread(void *param)
{
	replay_buffer_frame_decoder_t *frame_decoder = param;

	os_set_thread_name("replay-buffer: frame");
	decoder_thread_set_low_priority();

	for (;;) {
		pthread_mutex_lock(&frame_decoder->mutex);

		while (!frame_decoder->stopping && !frame_decoder->has_request)
			pthread_cond_wait(&frame_decoder->cond, &frame_decoder->mutex);

		if (frame_decoder->stopping) {
			pthread_mutex_unlock(&frame_decoder->mutex);
			break;
		}

		const uint64_t seq = frame_decoder->seq;
		const uint64_t request_id = frame_decoder->request_id;
		frame_decoder->has_request = false;
		pthread_mutex_unlock(&frame_decoder->mutex);

		blog(LOG_INFO, "[replay buffer] frame %llu: decoding seq %llu", (unsigned long long)request_id,
		     (unsigned long long)seq);
		frame_process(frame_decoder, seq, request_id);
	}

	return NULL;
}

bool replay_buffer_frame_decoder_request(replay_buffer_frame_decoder_t *frame_decoder, uint64_t seq,
					 uint64_t request_id)
{
	if (!frame_decoder || request_id == 0)
		return false;

	bool accepted = false;
	pthread_mutex_lock(&frame_decoder->mutex);

	if (!frame_decoder->stopping) {
		/* Keep only the newest request in the slot. */
		frame_decoder->seq = seq;
		frame_decoder->request_id = request_id;
		frame_decoder->latest_id = request_id;
		frame_decoder->has_request = true;

		/* The previous result belongs to an older interaction and is no longer
		 * useful; release it early to keep at most one frame. */
		if (frame_decoder->has_result && frame_decoder->result_id != request_id) {
			bfree(frame_decoder->rgba);
			frame_decoder->rgba = NULL;
			frame_decoder->has_result = false;
		}

		accepted = true;
		pthread_cond_signal(&frame_decoder->cond);
	}

	pthread_mutex_unlock(&frame_decoder->mutex);
	return accepted;
}

bool replay_buffer_frame_decoder_copy(replay_buffer_frame_decoder_t *frame_decoder, uint64_t request_id, uint8_t *dst,
				      size_t capacity, int *status, uint32_t *width, uint32_t *height, size_t *size)
{
	if (!frame_decoder || !status || !width || !height || !size)
		return false;

	*status = REPLAY_BUFFER_FRAME_PENDING;
	*width = 0;
	*height = 0;
	*size = 0;

	pthread_mutex_lock(&frame_decoder->mutex);

	if (frame_decoder->has_result && frame_decoder->result_id == request_id) {
		if (frame_decoder->result_failed) {
			*status = REPLAY_BUFFER_FRAME_FAILED;
		} else {
			*status = REPLAY_BUFFER_FRAME_READY;
			*width = frame_decoder->width;
			*height = frame_decoder->height;
			*size = frame_decoder->size;

			if (dst) {
				if (capacity >= frame_decoder->size)
					memcpy(dst, frame_decoder->rgba, frame_decoder->size);
				else
					*status = REPLAY_BUFFER_FRAME_FAILED;
			}
		}
	}

	pthread_mutex_unlock(&frame_decoder->mutex);
	return true;
}

static void replay_buffer_frame_decoder_free_decoder(struct replay_buffer_decoder *decoder)
{
	if (decoder->decoder)
		avcodec_free_context(&decoder->decoder);
	if (decoder->scaler)
		sws_freeContext(decoder->scaler);
}

replay_buffer_frame_decoder_t *replay_buffer_frame_decoder_create(obs_encoder_t *video_encoder,
								  replay_buffer_cache_t *cache)
{
	if (!video_encoder || !cache)
		return NULL;

	const char *codec_name = obs_encoder_get_codec(video_encoder);
	if (!find_decoder_for_codec(codec_name)) {
		blog(LOG_WARNING, "[replay buffer] decoder for codec '%s' is not available",
		     codec_name ? codec_name : "(null)");
		return NULL;
	}

	replay_buffer_frame_decoder_t *frame_decoder = bzalloc(sizeof(*frame_decoder));
	if (!frame_decoder)
		return NULL;

	frame_decoder->encoder = obs_encoder_get_ref(video_encoder);
	if (!frame_decoder->encoder) {
		bfree(frame_decoder);
		return NULL;
	}

	frame_decoder->cache = cache;

	if (pthread_mutex_init(&frame_decoder->mutex, NULL) != 0)
		goto fail_encoder;
	if (pthread_cond_init(&frame_decoder->cond, NULL) != 0)
		goto fail_mutex;

	if (pthread_create(&frame_decoder->thread, NULL, frame_thread, frame_decoder) != 0)
		goto fail_cond;

	frame_decoder->thread_started = true;
	return frame_decoder;

fail_cond:
	pthread_cond_destroy(&frame_decoder->cond);
fail_mutex:
	pthread_mutex_destroy(&frame_decoder->mutex);
fail_encoder:
	obs_encoder_release(frame_decoder->encoder);
	bfree(frame_decoder);
	return NULL;
}

void replay_buffer_frame_decoder_destroy(replay_buffer_frame_decoder_t *frame_decoder)
{
	if (!frame_decoder)
		return;

	pthread_mutex_lock(&frame_decoder->mutex);
	frame_decoder->stopping = true;
	frame_decoder->has_request = false;
	pthread_cond_broadcast(&frame_decoder->cond);
	pthread_mutex_unlock(&frame_decoder->mutex);

	if (frame_decoder->thread_started)
		pthread_join(frame_decoder->thread, NULL);

	replay_buffer_frame_decoder_free_decoder(&frame_decoder->decoder);

	if (frame_decoder->rgba)
		bfree(frame_decoder->rgba);

	pthread_cond_destroy(&frame_decoder->cond);
	pthread_mutex_destroy(&frame_decoder->mutex);
	obs_encoder_release(frame_decoder->encoder);
	bfree(frame_decoder);
}
