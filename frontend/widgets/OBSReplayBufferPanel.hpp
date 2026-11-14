#pragma once

#include "OBSQTDisplay.hpp"

#include <QElapsedTimer>
#include <QFontMetrics>
#include <QImage>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <mutex>

class OBSBasic;
class QAction;
class QHBoxLayout;
class QLabel;
class QLayout;
class QPainter;
class QTimer;
class QToolButton;

/* Minimal timeline widget used by the replay buffer panel. It deliberately
 * keeps only the geometry/selection state; all proc calls live in the panel, so
 * the widget stays a pure view and can be reused. */
class OBSReplayBufferTimeline : public QWidget {
	Q_OBJECT

public:
	explicit OBSReplayBufferTimeline(QWidget *parent = nullptr);

	void SetRange(int64_t start_usec, int64_t end_usec);
	/* Configured replay buffer length limit; the axis always spans it, so the
	 * scale does not grow while the buffer is still filling up. 0 falls back to
	 * the buffered span (unlimited buffer). */
	void SetMaxDuration(int64_t max_usec);

	void SetPosition(int64_t position_usec);
	void SetSelection(int64_t start_usec, int64_t end_usec);
	void ClearSelection();
	/* Move the clip endpoint nearest to time_usec onto that exact moment. With no
	 * clip yet this starts one at time_usec that runs to the live edge. */
	void MoveSelectionEndpoint(int64_t time_usec);
	/* Drop any wheel zoom and show the whole window again. */
	void ResetZoom();
	/* Pin the axis where it stands. While frozen the ruler slides to the right
	 * as time passes and the axis re-aligns once it has used up the margin. */
	void SetFrozen(bool frozen);

	inline int64_t SelectionStart() const { return selectionStart_; }
	inline int64_t SelectionEnd() const { return selectionEnd_; }
	inline bool HasSelection() const { return selectionEnd_ > selectionStart_; }

	int XForTime(int64_t time_usec) const;

	QSize sizeHint() const override;

signals:
	void PositionChanged(qint64 position_usec, bool finished);
	void SelectionChanged(qint64 start_usec, qint64 end_usec);
	/* Asked for by a double click in the part of the axis reserved past the
	 * live edge, where there is no buffered frame to select. */
	void GoLiveRequested();
	/* Asked for by a double click on the playhead triangle itself. */
	void MarkerDoubleClicked();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	int64_t RawTimeAt(int x) const;
	int64_t TimeAt(int x) const;
	/* True inside the playhead triangle plus a few pixels of slop, so a double
	 * click on the marker is not confused with one on the axis. */
	bool PlayheadHit(const QPoint &pos) const;
	int TrackLeft() const;
	int TrackWidth() const;
	int64_t AxisStart() const;
	int64_t AxisEnd() const;
	int64_t AxisSpan() const;
	int64_t FullAxisSpan() const;
	/* Part of the axis reserved past "now", and the axis end while live. */
	int64_t AxisLead() const;
	int64_t LiveAxisEnd() const;
	int AxisLeft() const;
	int AxisWidth() const;
	double RatioForTime(int64_t time_usec) const;
	void DrawRuler(QPainter &painter, int left, int width, int axisY) const;
	/* Vertical layout, measured from the label font so nothing clips. */
	QFontMetrics ScaleMetrics() const;
	int AxisY() const;
	int PreferredHeight() const;

	int64_t rangeStart_ = 0;
	int64_t rangeEnd_ = 0;
	/* Playhead position before the click that may turn out to be the first half
	 * of a double click, so the pair can be undone. */
	int64_t positionBeforePress_ = 0;
	bool positionPressMoved_ = false;
	/* The press that the double click follows landed on the marker. Kept from the
	 * press, because the marker may have snapped elsewhere before the second
	 * click arrived. */
	bool pressHitMarker_ = false;
	int64_t maxDuration_ = 0;
	/* Wheel zoom: 0 means "fit the whole window". */
	int64_t zoomSpan_ = 0;
	int64_t axisEnd_ = 0;
	int64_t frozenEnd_ = 0;
	int64_t frozenSpan_ = 0;
	bool frozen_ = false;
	int64_t position_ = 0;
	int64_t selectionStart_ = 0;
	int64_t selectionEnd_ = 0;
	/* The end is not a fixed moment: the clip runs from the start to the live
	 * edge and has to keep tracking the buffer end as it rolls. */
	bool selectionFollowsLive_ = false;
	int64_t dragAnchor_ = 0;
	bool draggingPosition_ = false;
	bool draggingSelection_ = false;
	bool hovering_ = false;
};

/* Replay buffer controls docked under the program preview, below the zoom
 * controls and the scroll bars. The widget is a normal member of the preview
 * layout, so it never covers the program output. */
class OBSReplayBufferPanel : public QWidget {
	Q_OBJECT

public:
	/* preview is the program output widget; the reviewed frame is drawn into
	 * its OBS display, so the canvas itself shows the selected moment. */
	OBSReplayBufferPanel(OBSBasic *main, OBSQTDisplay *preview);
	~OBSReplayBufferPanel() override;

	/* Hands the fold button to the preview's own control row, so it sits next to
	 * the horizontal scroll bar instead of the playback row. When a reference
	 * widget is given, the button takes the size of its drop-down arrow. */
	void SetCollapseButtonLayout(QLayout *layout, QWidget *reference = nullptr);

public slots:
	void Start();
	void Stop();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private slots:
	void RefreshInfo();
	void PositionChanged(qint64 position_usec, bool finished);
	void SelectionChanged(qint64 start_usec, qint64 end_usec);
	void MarkerDoubleClicked();
	void PollFullFrame();
	void ToggleLivePause();
	void GoLive();
	void PreviousKeyframe();
	void NextKeyframe();
	void SaveReplay();
	void ExportSelection();
	void PollExport();
	void ToggleCollapsed();

private:
	bool Active() const;
	bool Reviewing() const;
	void ShowPanel(bool show);
	/* Rebuild the fold chevron at the size the control row can hold. */
	void ApplyCollapseIcon();
	void UpdateCollapseButtonSize();
	void HidePanel();
	void UpdatePanelHeight();
	void UpdateCompactness();
	void UpdateCollapsedState();
	void UpdateSecondaryTextColors();
	void UpdateLabels();
	void UpdateControls();
	void UpdateGoLiveState();
	void UpdateReviewState();
	void AttachReviewDisplay(OBSQTDisplay *display);
	void SetReviewActive(bool active);
	void SetReviewFrame(const QImage &image);
	void SetReviewStale(bool stale);
	void ClearReviewFrame();
	void RenderReviewFrame(uint32_t cx, uint32_t cy);
	static void DrawReviewFrame(void *param, uint32_t cx, uint32_t cy);
	void JumpTo(int64_t position_usec);
	/* Resolve the keyframe at the current position and ask the plugin for its
	 * full frame; this is what feeds the canvas. */
	void RequestFrame(bool snap = true);
	void RequestFullFrame(int64_t time_usec);
	void CancelFullFrame();

	void ShowExportError(int error);

	OBSBasic *main_;

	QLabel *cacheLabel_;
	QToolButton *prevButton_;
	QToolButton *nextButton_;
	QToolButton *saveButton_;
	QToolButton *exportButton_;
	QToolButton *goLiveButton_;
	QToolButton *collapseButton_;
	QAction *exportAction_;
	/* Glyph size picked for the fold button when it joins the preview row. */
	int collapseIconSide_ = 12;
	QPointer<QWidget> foldReference_;
	OBSReplayBufferTimeline *timeline_ = nullptr;

	/* The reviewed frame is uploaded as a texture on the graphics thread and
	 * drawn by a display draw callback, so the canvas itself shows it without
	 * any window stacked on top of the preview. */
	QPointer<OBSQTDisplay> preview_;
	obs_display_t *reviewDisplay_ = nullptr;
	gs_texture_t *reviewTexture_ = nullptr;
	QImage reviewPending_;
	std::mutex reviewMutex_;
	bool reviewActive_ = false;
	bool reviewClear_ = false;
	/* The last decode failed, so the canvas still shows an older frame. */
	bool reviewStale_ = false;

	QTimer infoTimer_;
	QTimer fullFrameTimer_;
	QTimer exportTimer_;
	QElapsedTimer fullFrameElapsed_;
	QElapsedTimer exportElapsed_;

	int64_t rangeStartUsec_ = 0;
	int64_t rangeEndUsec_ = 0;
	int64_t positionUsec_ = 0;
	/* Keyframe the playhead and the selection start are parked on, as dts;
	 * -1 means "no anchor" (following live, or nothing selected). A dropped
	 * history makes the saved keyframe disappear while the times can still look
	 * inside the window, so these are re-checked against the cache on refresh. */
	int64_t positionAnchorUsec_ = -1;
	int64_t selectionAnchorUsec_ = -1;
	/* Selection start captured from the timeline, resolved to an anchor on the
	 * next refresh instead of on every drag event. */
	int64_t pendingSelectionStart_ = 0;
	bool selectionAnchorPending_ = false;

	uint64_t fullFrameRequestId_ = 0;
	uint64_t pendingFullFrameId_ = 0;
	bool followLive_ = true;
	bool fullFramePending_ = false;
	bool exportRunning_ = false;
	bool collapsed_ = false;
	bool visible_ = false;
};
