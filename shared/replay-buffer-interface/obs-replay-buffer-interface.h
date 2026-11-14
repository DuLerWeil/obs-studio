#pragma once

/* Constants shared by the replay buffer output (plugins/obs-ffmpeg) and the
 * frontend that drives it through the output's proc_handler.
 *
 * The proc protocol is private to OBS: it is not part of the libobs ABI and is
 * not installed for third-party plugins. Keeping the names and numbers in one
 * header at least keeps the two sides from drifting apart. */

/* proc_handler operation names. */
#define REPLAY_BUFFER_PROC_SAVE "save"
#define REPLAY_BUFFER_PROC_GET_LAST_REPLAY "get_last_replay"
#define REPLAY_BUFFER_PROC_GET_INFO "get_replay_buffer_info"
#define REPLAY_BUFFER_PROC_FIND_KEYFRAME "find_replay_keyframe"
#define REPLAY_BUFFER_PROC_FIND_PREV_KEYFRAME "find_prev_replay_keyframe"
#define REPLAY_BUFFER_PROC_FIND_NEXT_KEYFRAME "find_next_replay_keyframe"
#define REPLAY_BUFFER_PROC_REQUEST_FULL_FRAME "request_replay_full_frame"
#define REPLAY_BUFFER_PROC_COPY_FULL_FRAME "copy_replay_full_frame"
#define REPLAY_BUFFER_PROC_EXPORT_SELECTION "export_replay_selection"
#define REPLAY_BUFFER_PROC_GET_EXPORT_STATUS "get_replay_export_status"

/* Returned by the find_* keyframe procs when no keyframe matches. */
#define REPLAY_BUFFER_INVALID_ORDINAL (-1)

/* get_replay_export_status.state */
enum replay_buffer_export_state {
	REPLAY_BUFFER_EXPORT_IDLE = 0,
	REPLAY_BUFFER_EXPORT_RUNNING = 1,
	REPLAY_BUFFER_EXPORT_DONE = 2,
	REPLAY_BUFFER_EXPORT_FAILED = 3,
};

/* export_replay_selection.result and get_replay_export_status.error */
enum replay_buffer_export_error {
	REPLAY_BUFFER_EXPORT_ERR_NONE = 0,
	REPLAY_BUFFER_EXPORT_ERR_INVALID = 1,
	REPLAY_BUFFER_EXPORT_ERR_BUSY = 2,
	REPLAY_BUFFER_EXPORT_ERR_INACTIVE = 3,
	REPLAY_BUFFER_EXPORT_ERR_WRITE = 4,
};

/* copy_replay_full_frame.status */
enum replay_buffer_frame_status {
	REPLAY_BUFFER_FRAME_PENDING = 0,
	REPLAY_BUFFER_FRAME_READY = 1,
	REPLAY_BUFFER_FRAME_FAILED = 2,
};
