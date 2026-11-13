#include "OBSReplayBufferPanel.hpp"

#include "OBSBasic.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <qt-wrappers.hpp>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

#include "moc_OBSReplayBufferPanel.cpp"

namespace {

constexpr int kPanelSpacing = 4;
/* Ruler labels sit above the axis; the two markers and their time readouts hang
 * below it. The vertical layout is derived from the label font, so the text is
 * never clipped when the UI is scaled up. */
constexpr int kEdgeMargin = 2;
constexpr int kLabelGap = 2;
/* Space between the outer buttons of the timeline row and the panel edge. */
constexpr int kRowSideMargin = 12;
/* Shortest gap between two frame requests while the playhead is dragged. The
 * plugin keeps only the newest request, so this is about sparing the decoder,
 * not about queueing. */
constexpr int kDragFrameIntervalMs = 80;
constexpr int kTrackInset = 2;
constexpr int kGrooveHeight = 4;
constexpr int kMarkerHeight = 7;
constexpr int kMarkHalfWidth = 4;
constexpr int kTickHeight = 12;
constexpr int kRulerMinSpacing = 80;
/* Floor for the wheel zoom, so a notch cannot collapse the scale to nothing. */
constexpr int64_t kMinZoomSpan = 2000000;

/* Same tick colour the mixer's volume scale uses (VolumeSlider::paintEvent),
 * so the two scales read as the same control. */
constexpr int kRulerTickRed = 91;
constexpr int kRulerTickGreen = 98;
constexpr int kRulerTickBlue = 115;

/* Values of the copy_replay_full_frame status output. */
constexpr int kFullFramePending = 0;
constexpr int kFullFrameReady = 1;
constexpr int kFullFrameFailed = 2;

/* Values of get_replay_export_status/export_replay_selection. */
constexpr int kExportRunning = 1;
constexpr int kExportDone = 2;

constexpr int kExportErrNone = 0;
constexpr int kExportErrInvalid = 1;
constexpr int kExportErrBusy = 2;
constexpr int kExportErrInactive = 3;
constexpr int kExportErrWrite = 4;

QString FormatTime(int64_t usec)
{
	const int64_t total = qMax<int64_t>(int64_t(0), usec) / 1000000;
	const int64_t hours = total / 3600;
	const int64_t minutes = (total / 60) % 60;
	const int64_t seconds = total % 60;

	if (hours > 0) {
		return QStringLiteral("%1:%2:%3")
			.arg(hours)
			.arg(minutes, 2, 10, QChar('0'))
			.arg(seconds, 2, 10, QChar('0'));
	}

	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

/* Offset from the live edge in the scale's notation: "00:00" for now, "-mm:ss"
 * for anything older and "+mm:ss" for the reserved part past it. Shared by the
 * ruler and the two marker readouts. */
QString FormatOffset(int64_t usec)
{
	if (usec < 0) {
		return QStringLiteral("+%1").arg(FormatTime(-usec));
	}

	return usec == 0 ? FormatTime(0) : QStringLiteral("-%1").arg(FormatTime(usec));
}

/* Icon from the active theme, falling back to the built-in resources when the
 * theme does not ship that file. */
QIcon ThemeIcon(const char *name)
{
	const QString file = QLatin1String(name);
	const QString dir = obs_frontend_is_theme_dark() ? QStringLiteral("Dark/") : QStringLiteral("Light/");

	QIcon icon(QStringLiteral("theme:") + dir + file);
	if (icon.isNull()) {
		icon = QIcon(QStringLiteral(":/res/images/") + file);
	}
	/* A third-party theme may ship neither, and an icon button without an icon
	 * would collapse to nothing. */
	if (icon.isNull()) {
		icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
	}
	return icon;
}

/* Type of the scale: the ruler labels and the marker readouts share it, so the
 * axis reads as one control. */
QFont ScaleFont(const QFont &base)
{
	QFont font = base;
	if (font.pointSizeF() > 0.0) {
		font.setPointSizeF(8.0);
	} else {
		font.setPixelSize(11);
	}

	font.setBold(true);
	return font;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Timeline                                                                  */
/* ------------------------------------------------------------------------- */

OBSReplayBufferTimeline::OBSReplayBufferTimeline(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setMinimumHeight(PreferredHeight());
	setToolTip(QTStr("Basic.Main.Preview.ReplayBuffer.TimelineHint"));
}

void OBSReplayBufferTimeline::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);

	/* The height follows the label font, so a style or font change has to
	 * re-measure it instead of keeping the old size hint. */
	if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
		setMinimumHeight(PreferredHeight());
		updateGeometry();
	}
}

QSize OBSReplayBufferTimeline::sizeHint() const
{
	return QSize(400, PreferredHeight());
}

QFontMetrics OBSReplayBufferTimeline::ScaleMetrics() const
{
	return QFontMetrics(ScaleFont(font()));
}

int OBSReplayBufferTimeline::AxisY() const
{
	/* One label row above the ticks, which is where the ruler baseline lives. */
	return kEdgeMargin + ScaleMetrics().height() + kLabelGap + kTickHeight / 2;
}

int OBSReplayBufferTimeline::PreferredHeight() const
{
	/* Marker triangles, then their time readout, then the bottom margin. */
	const int markBottom = AxisY() + kGrooveHeight / 2 + kMarkerHeight;
	return markBottom + kLabelGap + ScaleMetrics().height() + kEdgeMargin;
}

int OBSReplayBufferTimeline::TrackLeft() const
{
	return kTrackInset;
}

int OBSReplayBufferTimeline::TrackWidth() const
{
	return qMax(1, width() - kTrackInset * 2);
}

void OBSReplayBufferTimeline::DrawRuler(QPainter &painter, int left, int width, int axisY) const
{
	/* The step is derived from the fixed axis, so the ticks keep their
	 * positions while the buffer is still filling up. */
	const int64_t duration = AxisSpan();
	if (duration <= 0 || width <= 0) {
		return;
	}

	/* Ticks are anchored on the live edge and step backwards, because the
	 * useful reading is "how long ago was this moment", not an absolute time. */
	static const int kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};

	const double seconds = (double)duration / 1000000.0;
	const double pixelsPerSecond = (double)width / seconds;

	int step = kSteps[(int)(sizeof(kSteps) / sizeof(kSteps[0])) - 1];
	for (int candidate : kSteps) {
		if ((double)candidate * pixelsPerSecond >= (double)kRulerMinSpacing) {
			step = candidate;
			break;
		}
	}

	const QFont rulerFont = ScaleFont(font());
	painter.setFont(rulerFont);

	const QFontMetrics metrics(rulerFont);
	/* Sit the label just above the tick band, leaving the widget margin free. */
	const int baseline = axisY - kTickHeight / 2 - kLabelGap - metrics.descent();

	const QColor tickColor(kRulerTickRed, kRulerTickGreen, kRulerTickBlue);
	const QColor textColor = palette().color(QPalette::Disabled, QPalette::WindowText);

	painter.save();

	/* The axis reserves part of its width past "now", so start on the step grid
	 * there and let the reserved part carry its own +mm:ss labels. */
	const int64_t step_usec = (int64_t)step * 1000000;
	const int64_t lead_usec = qMax<int64_t>(int64_t(0), AxisEnd() - rangeEnd_);
	const int64_t first_offset = -(lead_usec / step_usec) * step_usec;

	/* Ticks are computed from the live edge, so a label always means the same
	 * offset and both markers stay aligned with it as the buffer rolls. */
	for (int64_t offset = first_offset;; offset += step_usec) {
		const int64_t tickTime = rangeEnd_ - offset;
		if (tickTime < AxisStart()) {
			break;
		}

		/* Ahead of the zoom window (the window ends at axisEnd_, which sits
		 * behind "now" when zoomed). */
		if (tickTime > AxisEnd()) {
			continue;
		}

		const int x = XForTime(tickTime);
		if (x < left || x > left + width) {
			continue;
		}

		painter.fillRect(x, axisY - kTickHeight / 2, 1, kTickHeight, tickColor);

		const QString label = FormatOffset(offset);
		const int textWidth = metrics.horizontalAdvance(label);
		const int maxX = this->width() - textWidth - 2;
		const int textX = maxX > 2 ? qBound(2, x - textWidth / 2, maxX) : 2;

		painter.setPen(textColor);
		painter.drawText(textX, baseline, label);
	}

	painter.restore();
}

int64_t OBSReplayBufferTimeline::FullAxisSpan() const
{
	const int64_t buffered = rangeEnd_ - rangeStart_;
	if (maxDuration_ > 0 && maxDuration_ > buffered) {
		return maxDuration_;
	}
	return buffered;
}

int64_t OBSReplayBufferTimeline::AxisLead() const
{
	/* Reserve a sixth of the configured window past "now", so the axis never
	 * ends right under the newest frame: a 30 minute limit spans -35 to +5. */
	return FullAxisSpan() / 6;
}

int64_t OBSReplayBufferTimeline::LiveAxisEnd() const
{
	return rangeEnd_ + AxisLead();
}

int64_t OBSReplayBufferTimeline::AxisSpan() const
{
	if (frozen_) {
		return frozenSpan_;
	}

	const int64_t full = FullAxisSpan() + AxisLead() * 2;
	if (zoomSpan_ > 0 && zoomSpan_ < full) {
		return zoomSpan_;
	}
	return full;
}

int64_t OBSReplayBufferTimeline::AxisEnd() const
{
	if (frozen_) {
		return frozenEnd_;
	}

	return zoomSpan_ > 0 ? axisEnd_ : LiveAxisEnd();
}

int64_t OBSReplayBufferTimeline::AxisStart() const
{
	return AxisEnd() - AxisSpan();
}

int OBSReplayBufferTimeline::AxisLeft() const
{
	/* The widget shares its row with the step buttons, so the scale fills the
	 * whole track instead of keeping a margin on each side. The frozen axis has
	 * the reserved part past "now" to slide into, which is where the ruler
	 * drifts once a historical moment is parked. */
	return TrackLeft();
}

int OBSReplayBufferTimeline::AxisWidth() const
{
	return TrackWidth();
}

double OBSReplayBufferTimeline::RatioForTime(int64_t time_usec) const
{
	const int64_t span = AxisSpan();
	if (span <= 0) {
		return 0.0;
	}

	return (double)(time_usec - AxisStart()) / (double)span;
}

int OBSReplayBufferTimeline::XForTime(int64_t time_usec) const
{
	const double ratio = qBound(0.0, RatioForTime(time_usec), 1.0);
	return AxisLeft() + (int)(ratio * (double)AxisWidth());
}

int64_t OBSReplayBufferTimeline::RawTimeAt(int x) const
{
	const int64_t span = AxisSpan();
	if (span <= 0) {
		return rangeStart_;
	}

	const int left = AxisLeft();
	const int width = AxisWidth();
	const int clamped = qBound(left, x, left + width);
	const double ratio = (double)(clamped - left) / (double)width;
	return AxisStart() + (int64_t)(ratio * (double)span);
}

int64_t OBSReplayBufferTimeline::TimeAt(int x) const
{
	/* The left part of a fixed axis may still be unbuffered and the right part
	 * is reserved past "now"; a click there must land on a frame that exists. */
	return qBound(rangeStart_, RawTimeAt(x), rangeEnd_);
}

void OBSReplayBufferTimeline::SetFrozen(bool frozen)
{
	if (frozen_ == frozen) {
		return;
	}

	if (frozen) {
		frozenEnd_ = LiveAxisEnd();
		frozenSpan_ = FullAxisSpan() + AxisLead() * 2;
	} else {
		/* Back to the live window, without a leftover zoom. */
		zoomSpan_ = 0;
		axisEnd_ = LiveAxisEnd();
	}

	frozen_ = frozen;
	update();
}

void OBSReplayBufferTimeline::SetRange(int64_t start_usec, int64_t end_usec)
{
	if (end_usec < start_usec) {
		end_usec = start_usec;
	}

	rangeStart_ = start_usec;
	rangeEnd_ = end_usec;
	position_ = qBound(rangeStart_, position_, rangeEnd_);

	if (frozen_) {
		/* Re-align once the ruler has slid through the whole margin, and let go
		 * when the parked moment has been rolled out of the buffer. */
		if (frozenEnd_ <= start_usec) {
			frozen_ = false;
		} else if (end_usec - frozenEnd_ >= frozenSpan_ / 10) {
			frozenEnd_ = end_usec + AxisLead();
			frozenSpan_ = FullAxisSpan() + AxisLead() * 2;
		}
	}

	/* A zoomed window may fall behind the rolling buffer; drop the zoom instead
	 * of showing a window that has no data left in it. */
	if (!frozen_ && zoomSpan_ > 0) {
		if (zoomSpan_ >= FullAxisSpan() + AxisLead() * 2) {
			zoomSpan_ = 0;
		} else {
			axisEnd_ = qBound(rangeStart_, axisEnd_, LiveAxisEnd());
		}
	}

	if (selectionEnd_ > selectionStart_) {
		selectionStart_ = qBound(rangeStart_, selectionStart_, rangeEnd_);
		selectionEnd_ = qBound(rangeStart_, selectionEnd_, rangeEnd_);
		if (selectionEnd_ <= selectionStart_) {
			selectionStart_ = 0;
			selectionEnd_ = 0;
		}
	}

	update();
}

void OBSReplayBufferTimeline::SetMaxDuration(int64_t max_usec)
{
	if (maxDuration_ == max_usec) {
		return;
	}

	maxDuration_ = max_usec;
	update();
}

void OBSReplayBufferTimeline::SetPosition(int64_t position_usec)
{
	position_ = qBound(rangeStart_, position_usec, rangeEnd_);
	update();
}

void OBSReplayBufferTimeline::SetSelection(int64_t start_usec, int64_t end_usec)
{
	selectionStart_ = qBound(rangeStart_, start_usec, rangeEnd_);
	selectionEnd_ = qBound(rangeStart_, end_usec, rangeEnd_);
	if (selectionEnd_ < selectionStart_) {
		std::swap(selectionStart_, selectionEnd_);
	}
	if (selectionEnd_ <= selectionStart_) {
		selectionStart_ = 0;
		selectionEnd_ = 0;
	}
	update();
}

void OBSReplayBufferTimeline::ClearSelection()
{
	selectionStart_ = 0;
	selectionEnd_ = 0;
	update();
}

void OBSReplayBufferTimeline::paintEvent(QPaintEvent *event)
{
	UNUSED_PARAMETER(event);

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::TextAntialiasing);

	const int left = TrackLeft();
	const int width = TrackWidth();
	const int axisY = AxisY();

	/* Same roles the mixer's volume scale uses: the groove is the dark meter
	 * background and the filled stretch is the blue level colour. */
	const QColor axisColor = palette().color(QPalette::Dark);
	const QColor bufferedColor = palette().color(QPalette::Link);
	QColor markerColor = palette().color(QPalette::WindowText);
	if (hovering_) {
		markerColor = markerColor.lighter(115);
	}

	/* Selection band first, so the axis and markers stay on top of it. */
	if (HasSelection()) {
		const int selectionLeft = XForTime(selectionStart_);
		const int selectionRight = XForTime(selectionEnd_);
		QColor selectionColor = palette().color(QPalette::Highlight);
		selectionColor.setAlpha(hovering_ ? 110 : 80);
		painter.setPen(Qt::NoPen);
		painter.setBrush(selectionColor);
		painter.drawRect(QRectF(selectionLeft, axisY - 7, selectionRight - selectionLeft, 14));
	}

	/* Ticks first: the groove then covers their middle, leaving only the ends
	 * visible, exactly like the mixer's volume scale. */
	DrawRuler(painter, left, width, axisY);

	/* The axis borrows the mixer's volume scale: a black groove with the
	 * buffered stretch painted in the level colour, same roles as the slider. */
	painter.setPen(Qt::NoPen);
	painter.setBrush(axisColor);
	painter.drawRoundedRect(QRectF(AxisLeft(), axisY - kGrooveHeight / 2.0, AxisWidth(), kGrooveHeight),
				kGrooveHeight / 2.0, kGrooveHeight / 2.0);

	const int bufferedLeft = XForTime(rangeStart_);
	const int bufferedRight = XForTime(rangeEnd_);
	if (bufferedRight > bufferedLeft) {
		painter.setBrush(bufferedColor);
		painter.drawRoundedRect(QRectF(bufferedLeft, axisY - kGrooveHeight / 2.0, bufferedRight - bufferedLeft,
					       kGrooveHeight),
					kGrooveHeight / 2.0, kGrooveHeight / 2.0);
	}

	/* Both markers are upright triangles under the axis: the buffered start in
	 * the level colour and the playback point in the text colour. */
	const int bufferedX = XForTime(rangeStart_);
	const int positionX = XForTime(position_);
	const int markTop = axisY + kGrooveHeight / 2;
	const int markBottom = markTop + kMarkerHeight;

	auto drawMarker = [&](int x, const QColor &color) {
		QPolygonF triangle;
		triangle << QPointF(x, markTop) << QPointF(x - kMarkHalfWidth, markBottom)
			 << QPointF(x + kMarkHalfWidth, markBottom);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawPolygon(triangle);
	};

	drawMarker(bufferedX, bufferedColor);
	drawMarker(positionX, markerColor);

	/* The time each marker sits at, printed right under it and moving with it. */
	const QFont labelFont = ScaleFont(font());
	const QFontMetrics labelMetrics(labelFont);
	/* Anchor the readout to the bottom edge so its descent always fits. */
	const int labelBaseline = height() - kEdgeMargin - labelMetrics.descent();
	const QColor labelColor = palette().color(QPalette::Disabled, QPalette::WindowText);

	painter.setFont(labelFont);

	const auto labelRect = [&](int x, const QString &text) {
		const int textWidth = labelMetrics.horizontalAdvance(text);
		const int maxX = this->width() - textWidth - 2;
		const int left = maxX > 2 ? qBound(2, x - textWidth / 2, maxX) : 2;
		return QRect(left, labelBaseline - labelMetrics.ascent(), textWidth, labelMetrics.height());
	};

	const QString bufferedText = FormatOffset(rangeEnd_ - rangeStart_);
	const QString positionText = FormatOffset(rangeEnd_ - position_);

	const QRect positionLabel = labelRect(positionX, positionText);
	QRect bufferedLabel = labelRect(bufferedX, bufferedText);

	/* Keep the two readouts apart when the markers are close together. */
	if (bufferedLabel.intersects(positionLabel)) {
		bufferedLabel.moveRight(positionLabel.left() - 4);
		if (bufferedLabel.left() < 2) {
			bufferedLabel.moveLeft(2);
		}
	}

	painter.setPen(labelColor);
	painter.drawText(bufferedLabel.left(), labelBaseline, bufferedText);
	painter.drawText(positionLabel.left(), labelBaseline, positionText);
}

void OBSReplayBufferTimeline::mousePressEvent(QMouseEvent *event)
{
	/* Swallow the right press so the double click that follows is delivered to
	 * this widget instead of bubbling up to the parent. */
	if (event->button() == Qt::RightButton) {
		event->accept();
		return;
	}

	if (event->button() != Qt::LeftButton || rangeEnd_ <= rangeStart_) {
		QWidget::mousePressEvent(event);
		return;
	}

	const int64_t time = TimeAt(event->pos().x());

	if (event->modifiers() & Qt::ShiftModifier) {
		draggingSelection_ = true;
		dragAnchor_ = time;
		selectionStart_ = time;
		selectionEnd_ = time;
		emit SelectionChanged(0, 0);
	} else {
		draggingPosition_ = true;
		position_ = time;
		update();
		emit PositionChanged(position_, false);
	}
}

void OBSReplayBufferTimeline::mouseMoveEvent(QMouseEvent *event)
{
	if (draggingPosition_) {
		position_ = TimeAt(event->pos().x());
		update();
		emit PositionChanged(position_, false);
	} else if (draggingSelection_) {
		const int64_t time = TimeAt(event->pos().x());
		selectionStart_ = std::min(dragAnchor_, time);
		selectionEnd_ = std::max(dragAnchor_, time);
		update();
		emit SelectionChanged(selectionStart_, selectionEnd_);
	} else if (!hovering_) {
		hovering_ = true;
		update();
	}

	QWidget::mouseMoveEvent(event);
}

void OBSReplayBufferTimeline::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		QWidget::mouseReleaseEvent(event);
		return;
	}

	if (draggingPosition_) {
		draggingPosition_ = false;
		emit PositionChanged(position_, true);
	} else if (draggingSelection_) {
		draggingSelection_ = false;
		emit SelectionChanged(selectionStart_, selectionEnd_);
	}
}

void OBSReplayBufferTimeline::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (rangeEnd_ <= rangeStart_) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

	/* The axis reserves room past "now" for the ruler to slide into; a click
	 * there is not on the buffer, so it drives the live edge instead of the
	 * clip selection. */
	if (RawTimeAt(event->pos().x()) > rangeEnd_) {
		if (event->button() == Qt::LeftButton) {
			emit GoLiveRequested();
		} else if (event->button() == Qt::RightButton) {
			ClearSelection();
			emit SelectionChanged(0, 0);
		} else {
			QWidget::mouseDoubleClickEvent(event);
			return;
		}

		event->accept();
		return;
	}

	const int64_t time = TimeAt(event->pos().x());

	/* Left double click: the clip starts here and runs to the current moment.
	 * Right double click: the clip ends here, keeping the start already set. */
	if (event->button() == Qt::LeftButton) {
		selectionStart_ = time;
		selectionEnd_ = rangeEnd_;
	} else if (event->button() == Qt::RightButton) {
		if (selectionEnd_ <= selectionStart_) {
			selectionStart_ = rangeStart_;
		}
		selectionEnd_ = time;
		if (selectionEnd_ < selectionStart_) {
			std::swap(selectionStart_, selectionEnd_);
		}
	} else {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

	update();
	emit SelectionChanged(selectionStart_, selectionEnd_);
	event->accept();
}

void OBSReplayBufferTimeline::wheelEvent(QWheelEvent *event)
{
	const int delta = event->angleDelta().y();
	if (delta == 0 || rangeEnd_ <= rangeStart_) {
		event->ignore();
		return;
	}

	const int64_t full = FullAxisSpan() + AxisLead() * 2;
	if (full <= 0) {
		event->ignore();
		return;
	}

	const int left = AxisLeft();
	const int width = AxisWidth();
	if (width <= 0) {
		event->ignore();
		return;
	}

	/* Keep the moment under the cursor where it is, the way zooming a waveform
	 * works: the cursor is the pivot, not the edge. */
	const int x = qBound(left, (int)event->position().x(), left + width);
	const double ratio = (double)(x - left) / (double)width;

	const int64_t span = AxisSpan();
	const int64_t pivot = AxisStart() + (int64_t)(ratio * (double)span);
	const double factor = delta > 0 ? 0.75 : 1.35;
	const int64_t next = qBound(kMinZoomSpan, (int64_t)((double)span * factor), full);

	if (frozen_) {
		/* Zooming a pinned axis only changes how much of it is shown; the window
		 * still ends at the pinned end, so the markers stay put. */
		if (next == span) {
			event->ignore();
			return;
		}

		frozenSpan_ = next;
		frozenEnd_ = qMin(rangeEnd_, pivot + (int64_t)((1.0 - ratio) * (double)next));
		update();
		event->accept();
		return;
	}

	zoomSpan_ = next >= full ? 0 : next;
	if (zoomSpan_ == 0) {
		axisEnd_ = LiveAxisEnd();
	} else {
		int64_t end = pivot - (int64_t)(ratio * (double)next) + next;
		end = qBound(rangeStart_, end, LiveAxisEnd());
		axisEnd_ = end;
	}

	update();
	event->accept();
}

void OBSReplayBufferTimeline::ResetZoom()
{
	const bool changed = zoomSpan_ != 0;
	zoomSpan_ = 0;
	axisEnd_ = LiveAxisEnd();

	if (changed) {
		update();
	}
}

void OBSReplayBufferTimeline::leaveEvent(QEvent *event)
{
	hovering_ = false;
	update();
	QWidget::leaveEvent(event);
}

/* ------------------------------------------------------------------------- */
/* Docked panel                                                              */
/* ------------------------------------------------------------------------- */

OBSReplayBufferPanel::OBSReplayBufferPanel(OBSBasic *main, OBSQTDisplay *preview)
	: QWidget(nullptr),
	  main_(main),
	  preview_(preview)
{
	setObjectName(QStringLiteral("replayBufferPanel"));
	/* The panel lives in the preview column's layout, below the zoom controls
	 * and the scroll bars, so it never covers the program output and needs no
	 * native window of its own. It may be squeezed when the column is short,
	 * but never grows past the height of its own rows. */
	setAutoFillBackground(false);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	/* The scale and its two markers carry the timing, so the only text left is
	 * the transient export status. */
	cacheLabel_ = new QLabel(this);
	cacheLabel_->hide();

	UpdateSecondaryTextColors();

	const int iconSize = style()->pixelMetric(QStyle::PM_SmallIconSize);
	/* The row sits under the preview, so its icons stay one step smaller than the
	 * preview's own controls. */
	const int navIconSize = qMax(10, iconSize - 4);

	auto makeButton = [this](const QIcon &icon, const QString &tooltip, int size) {
		QToolButton *button = new QToolButton(this);
		button->setAutoRaise(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setIconSize(QSize(size, size));
		button->setIcon(icon);
		button->setToolTip(tooltip);
		return button;
	};

	/* One icon family for the whole strip: the theme's own media glyphs, so the
	 * steps, the saves and "go live" share stroke weight and corner rounding
	 * instead of mixing Qt's filled pixmaps with outline SVGs. */
	prevButton_ = makeButton(ThemeIcon("media/media_previous.svg"),
				 QTStr("Basic.Main.Preview.ReplayBuffer.PreviousKeyframe"), navIconSize);
	nextButton_ = makeButton(ThemeIcon("media/media_next.svg"),
				 QTStr("Basic.Main.Preview.ReplayBuffer.NextKeyframe"), navIconSize);

	/* Two states: pause the live view where it is, or jump back to the newest
	 * frame; UpdateGoLiveState() picks the glyph and the tooltip. */
	goLiveButton_ = makeButton(ThemeIcon("media/media_pause.svg"),
				   QTStr("Basic.Main.Preview.ReplayBuffer.PauseLive"), navIconSize);

	/* Two explicit buttons replace the old options menu: exporting the whole
	 * buffer and exporting the marked clip. Their icons share the save arrow and
	 * tray; a full bar marks the whole buffer, framing corners mark a selection. */
	saveButton_ = makeButton(ThemeIcon("replay-save-all.svg"), QTStr("Basic.Main.SaveReplay"), navIconSize);
	connect(saveButton_, &QToolButton::clicked, this, &OBSReplayBufferPanel::SaveReplay);

	exportAction_ = new QAction(QTStr("Basic.Main.Preview.ReplayBuffer.ExportSelection"), this);
	exportAction_->setEnabled(false);
	exportAction_->setIcon(ThemeIcon("replay-save-selection.svg"));
	/* Icon-only, so the action text doubles as the tooltip. */
	exportAction_->setToolTip(exportAction_->text());
	connect(exportAction_, &QAction::triggered, this, &OBSReplayBufferPanel::ExportSelection);

	exportButton_ = new QToolButton(this);
	exportButton_->setAutoRaise(true);
	exportButton_->setFocusPolicy(Qt::NoFocus);
	exportButton_->setIconSize(QSize(navIconSize, navIconSize));
	exportButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
	exportButton_->setDefaultAction(exportAction_);

	collapseButton_ = new QToolButton(this);
	collapseButton_->setAutoRaise(true);
	collapseButton_->setFocusPolicy(Qt::NoFocus);
	collapseButton_->setIconSize(QSize(iconSize, iconSize));
	connect(collapseButton_, &QToolButton::clicked, this, &OBSReplayBufferPanel::ToggleCollapsed);

	timeline_ = new OBSReplayBufferTimeline(this);

	/* The scale takes the whole left side and every control is grouped at the
	 * right end: keyframe steps, live pause/resume, export the marked clip,
	 * export the whole buffer. */
	auto *timelineRow = new QHBoxLayout;
	/* Keep the outer icons off the window edge; the scale fills what is left. */
	timelineRow->setContentsMargins(kRowSideMargin, 0, kRowSideMargin, 0);
	timelineRow->setSpacing(4);
	timelineRow->addWidget(timeline_, 1);
	timelineRow->addWidget(cacheLabel_);
	timelineRow->addWidget(prevButton_);
	timelineRow->addWidget(nextButton_);
	timelineRow->addWidget(goLiveButton_);
	timelineRow->addWidget(exportButton_);
	timelineRow->addWidget(saveButton_);

	auto *layout = new QVBoxLayout(this);
	/* No side margins: the panel is docked directly under the preview column
	 * and has to line up with the zoom row and the scroll bars above it. */
	layout->setContentsMargins(0, 4, 0, 6);
	layout->setSpacing(kPanelSpacing);
	layout->addLayout(timelineRow);

	infoTimer_.setInterval(1000);
	connect(&infoTimer_, &QTimer::timeout, this, &OBSReplayBufferPanel::RefreshInfo);

	fullFrameTimer_.setInterval(50);
	connect(&fullFrameTimer_, &QTimer::timeout, this, &OBSReplayBufferPanel::PollFullFrame);

	exportTimer_.setInterval(200);
	connect(&exportTimer_, &QTimer::timeout, this, &OBSReplayBufferPanel::PollExport);

	selectionSettleTimer_.setSingleShot(true);
	selectionSettleTimer_.setInterval(200);
	connect(&selectionSettleTimer_, &QTimer::timeout, this, &OBSReplayBufferPanel::SnapSelection);

	connect(timeline_, &OBSReplayBufferTimeline::PositionChanged, this, &OBSReplayBufferPanel::PositionChanged);
	connect(timeline_, &OBSReplayBufferTimeline::SelectionChanged, this, &OBSReplayBufferPanel::SelectionChanged);
	connect(timeline_, &OBSReplayBufferTimeline::GoLiveRequested, this, &OBSReplayBufferPanel::GoLive);
	connect(goLiveButton_, &QToolButton::clicked, this, &OBSReplayBufferPanel::ToggleLivePause);
	connect(prevButton_, &QToolButton::clicked, this, &OBSReplayBufferPanel::PreviousKeyframe);
	connect(nextButton_, &QToolButton::clicked, this, &OBSReplayBufferPanel::NextKeyframe);

	UpdateCollapsedState();
	/* The fold button lives in the preview's control row now; it only appears
	 * once the replay buffer actually runs. */
	ShowPanel(false);
}

void OBSReplayBufferPanel::SetCollapseButtonLayout(QLayout *layout, QWidget *reference)
{
	if (!layout || !collapseButton_) {
		return;
	}

	layout->addWidget(collapseButton_);

	foldReference_ = reference;
	UpdateCollapseButtonSize();
}

void OBSReplayBufferPanel::UpdateCollapseButtonSize()
{
	if (!collapseButton_) {
		return;
	}

	/* Match the combo's drop-down button: a square of the control height minus
	 * its padding, with a pre-scaled glyph so the style sheet's icon-size cannot
	 * blow it up again.
	 *
	 * A fresh widget reports Qt's default 640x480 box until the preview column is
	 * laid out, so an unshown reference is measured through its size hint and the
	 * result is clamped: a bogus height would hand the button a huge fixed size,
	 * which squeezed the whole panel out of the column. */
	int height = foldReference_ ? foldReference_->sizeHint().height() : 0;
	if (foldReference_ && foldReference_->isVisible()) {
		height = foldReference_->height();
	}

	const int side = qBound(14, height - 8, 34);
	if (collapseButton_->width() == side && collapseButton_->height() == side) {
		return;
	}

	collapseIconSide_ = qMax(8, side - 4);
	collapseButton_->setFixedSize(side, side);
	ApplyCollapseIcon();
}

void OBSReplayBufferPanel::ApplyCollapseIcon()
{
	const QIcon base = ThemeIcon(collapsed_ ? "up.svg" : "down.svg");
	collapseButton_->setIconSize(QSize(collapseIconSide_, collapseIconSide_));
	collapseButton_->setIcon(QIcon(base.pixmap(QSize(collapseIconSide_, collapseIconSide_))));
}

OBSReplayBufferPanel::~OBSReplayBufferPanel()
{
	/* Stop drawing into the preview before this object goes away. The display
	 * may already be gone (OBSBasic::OBSClose destroys it first); in that case
	 * its callback list went away with it. */
	if (preview_) {
		obs_display_t *display = preview_->GetDisplay();
		if (display && display == reviewDisplay_) {
			obs_display_remove_draw_callback(display, DrawReviewFrame, this);
		}
	}
	reviewDisplay_ = nullptr;

	{
		std::lock_guard<std::mutex> lock(reviewMutex_);
		reviewActive_ = false;
		reviewPending_ = QImage();
	}

	if (reviewTexture_) {
		obs_enter_graphics();
		gs_texture_destroy(reviewTexture_);
		obs_leave_graphics();
		reviewTexture_ = nullptr;
	}

	/* The fold button is parented to the preview's control row, so it would
	 * outlive the panel it drives; take it down with us. */
	if (collapseButton_ && collapseButton_->parent() != this) {
		collapseButton_->deleteLater();
	}
}

void OBSReplayBufferPanel::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	UpdateCompactness();
}

void OBSReplayBufferPanel::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::PaletteChange) {
		UpdateSecondaryTextColors();
	}
}

void OBSReplayBufferPanel::Start()
{
	if (visible_) {
		return;
	}

	visible_ = true;
	/* A new replay buffer session starts at the live edge even when the
	 * previous one was left parked in the middle of the buffer. */
	followLive_ = true;
	positionUsec_ = 0;
	timeline_->ResetZoom();

	UpdateCollapsedState();
	infoTimer_.start();
	RefreshInfo();

	/* Show the strip even if the first tick had no data yet. */
	if (Active()) {
		ShowPanel(true);
	}
}

void OBSReplayBufferPanel::Stop()
{
	visible_ = false;
	HidePanel();
}

bool OBSReplayBufferPanel::Active() const
{
	return main_ && main_->ReplayBufferActive();
}

bool OBSReplayBufferPanel::Reviewing() const
{
	/* Same 200 ms window as followLive_: at the live edge the program preview
	 * already shows this moment, so the canvas does not need a frozen frame. */
	return visible_ && Active() && rangeEndUsec_ > rangeStartUsec_ && positionUsec_ < rangeEndUsec_ - 200000;
}

void OBSReplayBufferPanel::UpdateReviewState()
{
	const bool reviewing = Reviewing();

	/* Attach lazily so this callback is registered after the one that draws
	 * the live preview, which makes the reviewed frame land on top of it. It is
	 * re-checked every time because the preview recreates its display when the
	 * window moves between monitors or the scaling mode changes, and the old
	 * callback list dies with it. */
	if (reviewing) {
		AttachReviewDisplay(preview_);
	}

	SetReviewActive(reviewing);

	/* Parked on a historical moment the axis is pinned, so the ruler slides and
	 * the playhead keeps its screen position; going live releases it again. */
	timeline_->SetFrozen(reviewing);
}

void OBSReplayBufferPanel::AttachReviewDisplay(OBSQTDisplay *display)
{
	if (!display) {
		return;
	}

	obs_display_t *handle = display->GetDisplay();
	if (!handle || handle == reviewDisplay_) {
		return;
	}

	reviewDisplay_ = handle;
	obs_display_add_draw_callback(handle, DrawReviewFrame, this);
}

void OBSReplayBufferPanel::SetReviewActive(bool active)
{
	std::lock_guard<std::mutex> lock(reviewMutex_);

	if (reviewActive_ == active) {
		return;
	}

	reviewActive_ = active;
	/* The texture itself is released by the draw callback on the graphics
	 * thread, where the graphics context is guaranteed to be current. */
	if (!active) {
		reviewPending_ = QImage();
	}
}

void OBSReplayBufferPanel::SetReviewFrame(const QImage &image)
{
	if (image.isNull()) {
		return;
	}

	QImage frame = image;
	if (frame.format() != QImage::Format_RGBA8888) {
		frame = frame.convertToFormat(QImage::Format_RGBA8888);
	}

	std::lock_guard<std::mutex> lock(reviewMutex_);
	reviewActive_ = true;
	reviewClear_ = false;
	reviewPending_ = frame;
	blog(LOG_INFO, "[replay buffer] review: stored %dx%d frame", frame.width(), frame.height());
}

void OBSReplayBufferPanel::ClearReviewFrame()
{
	std::lock_guard<std::mutex> lock(reviewMutex_);
	reviewPending_ = QImage();
	reviewClear_ = true;
}

void OBSReplayBufferPanel::DrawReviewFrame(void *param, uint32_t cx, uint32_t cy)
{
	if (param) {
		static_cast<OBSReplayBufferPanel *>(param)->RenderReviewFrame(cx, cy);
	}
}

/* Runs on the graphics thread with the graphics context current: the reviewed
 * frame is uploaded as a texture and drawn into the preview's own surface, so
 * no window is stacked on top of the canvas. */
void OBSReplayBufferPanel::RenderReviewFrame(uint32_t cx, uint32_t cy)
{
	QImage pending;
	bool active;
	bool clear;

	{
		std::lock_guard<std::mutex> lock(reviewMutex_);
		active = reviewActive_;
		clear = reviewClear_;
		reviewClear_ = false;
		if (!reviewPending_.isNull()) {
			pending = reviewPending_;
			reviewPending_ = QImage();
		}
	}

	if (!active) {
		if (reviewTexture_) {
			gs_texture_destroy(reviewTexture_);
			reviewTexture_ = nullptr;
		}
		return;
	}

	/* A cleared moment drops the frame; if a new one arrived in the same tick
	 * the texture is rebuilt right below. */
	if (clear && reviewTexture_) {
		gs_texture_destroy(reviewTexture_);
		reviewTexture_ = nullptr;
	}

	if (!pending.isNull()) {
		/* Always rebuild the texture from the new frame. Reusing it with
		 * gs_texture_set_image only works for GS_DYNAMIC textures: the D3D11
		 * backend maps with D3D11_MAP_WRITE_DISCARD and silently gives up on
		 * a default-usage texture, which left the first reviewed frame on the
		 * canvas. A frame only arrives once per click, so the cost is noise. */
		if (reviewTexture_) {
			gs_texture_destroy(reviewTexture_);
			reviewTexture_ = nullptr;
		}

		const uint8_t *data = pending.constBits();
		reviewTexture_ =
			gs_texture_create((uint32_t)pending.width(), (uint32_t)pending.height(), GS_RGBA, 1, &data, 0);
		blog(LOG_INFO, "[replay buffer] review: uploaded %dx%d frame", pending.width(), pending.height());
	}

	if (!reviewTexture_ || !cx || !cy) {
		return;
	}

	if (!gs_texture_get_width(reviewTexture_) || !gs_texture_get_height(reviewTexture_)) {
		return;
	}

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height) {
		return;
	}

	/* Draw with the program's own geometry: the viewport and projection
	 * OBSBasic::RenderMain uses for the live picture. Scaling the frame to fit
	 * the whole display instead made the reviewed moment jump in size whenever
	 * the preview was zoomed or scrolled. */
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	main_->GetDisplayRect(x, y, width, height);
	if (width <= 0 || height <= 0) {
		/* The main window has not laid the preview out yet. */
		x = 0;
		y = 0;
		width = (int)cx;
		height = (int)cy;
	}

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
	gs_set_viewport(x, y, width, height);

	const bool linearSrgb = gs_get_linear_srgb();
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linearSrgb);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (linearSrgb) {
		gs_effect_set_texture_srgb(image, reviewTexture_);
	} else {
		gs_effect_set_texture(image, reviewTexture_);
	}

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(reviewTexture_, 0, ovi.base_width, ovi.base_height);
	}

	gs_enable_framebuffer_srgb(previousSrgb);
	gs_projection_pop();
	gs_viewport_pop();
}

void OBSReplayBufferPanel::ShowPanel(bool show)
{
	const bool changed = isHidden() == show;
	setVisible(show);
	if (show && changed) {
		/* Showing the strip changes its height hint, and the fold button can only
		 * take its final size once the preview row is laid out. */
		UpdatePanelHeight();
		UpdateCollapseButtonSize();
	}
	/* The fold button is parented to the preview's own control row, so it has to
	 * follow the panel explicitly instead of being hidden along with it. */
	collapseButton_->setVisible(show);
}

void OBSReplayBufferPanel::HidePanel()
{
	infoTimer_.stop();
	fullFrameTimer_.stop();
	selectionSettleTimer_.stop();
	exportTimer_.stop();

	CancelFullFrame();

	timeline_->ClearSelection();
	selectionStartUsec_ = 0;
	selectionEndUsec_ = 0;
	exportRunning_ = false;
	followLive_ = true;
	positionUsec_ = 0;
	timeline_->SetMaxDuration(0);
	exportAction_->setEnabled(false);

	/* Stop() must leave the canvas showing the live program again even when
	 * the panel itself is hidden right after this. */
	SetReviewActive(false);

	ShowPanel(false);
}

void OBSReplayBufferPanel::UpdatePanelHeight()
{
	/* Hiding rows changes the layout's size hint, but the preview column only
	 * re-reads it once the geometry is invalidated explicitly. */
	if (QLayout *panelLayout = layout()) {
		panelLayout->invalidate();
	}

	updateGeometry();
}

void OBSReplayBufferPanel::UpdateCompactness()
{
	/* Fold controls by priority instead of squeezing them when the preview
	 * column gets narrow. The thresholds follow the docked panel width, which
	 * is the preview width plus the zoom row underneath it. */
	const bool expanded = !collapsed_;
	const bool wideEnough = this->width() >= 520;
	prevButton_->setVisible(expanded && wideEnough);
	nextButton_->setVisible(expanded && wideEnough);
}

void OBSReplayBufferPanel::UpdateCollapsedState()
{
	collapseButton_->setToolTip(QTStr(collapsed_ ? "Basic.Main.Preview.ReplayBuffer.Expand"
						     : "Basic.Main.Preview.ReplayBuffer.Collapse"));
	ApplyCollapseIcon();

	timeline_->setVisible(!collapsed_);
	prevButton_->setVisible(!collapsed_);
	nextButton_->setVisible(!collapsed_);
	saveButton_->setVisible(!collapsed_);
	exportButton_->setVisible(!collapsed_);
	goLiveButton_->setVisible(!collapsed_);

	/* The panel height follows the rows that are still visible, so folding it
	 * gives the preview its space back instead of leaving an empty strip. */
	UpdateCompactness();
	UpdatePanelHeight();
}

void OBSReplayBufferPanel::UpdateSecondaryTextColors()
{
	QPalette secondary = palette();
	secondary.setColor(QPalette::WindowText, palette().color(QPalette::Disabled, QPalette::WindowText));
	cacheLabel_->setPalette(secondary);
}

void OBSReplayBufferPanel::ToggleCollapsed()
{
	collapsed_ = !collapsed_;
	UpdateCollapsedState();
}

void OBSReplayBufferPanel::RefreshInfo()
{
	if (!visible_) {
		return;
	}

	if (!Active()) {
		/* Keep the timer polling: hiding through HidePanel() from here would stop
		 * it, and a tick that landed before the output reported itself active used
		 * to take the panel down for the rest of the session. The strip is hidden
		 * without touching the timers so nothing stale stays on screen. */
		ShowPanel(false);
		SetReviewActive(false);
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		ShowPanel(false);
		SetReviewActive(false);
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	calldata_t cd = {0};
	proc_handler_call(ph, "get_replay_buffer_info", &cd);

	const int64_t start = calldata_int(&cd, "start_time_usec");
	const int64_t end = calldata_int(&cd, "end_time_usec");
	const long long count = calldata_int(&cd, "keyframe_count");
	const int64_t maxTime = calldata_int(&cd, "max_time_usec");
	calldata_free(&cd);

	if (count <= 0 || end <= start) {
		/* The buffer is running but has nothing to show yet. Keep an empty scale
		 * on screen instead of hiding the strip and popping it in later. */
		rangeStartUsec_ = 0;
		rangeEndUsec_ = 0;
		positionUsec_ = 0;
		timeline_->SetMaxDuration(maxTime);
		timeline_->SetRange(0, 0);
		timeline_->SetPosition(0);
		UpdateControls();
		ShowPanel(true);
		SetReviewActive(false);
		return;
	}

	rangeStartUsec_ = start;
	rangeEndUsec_ = end;

	if (followLive_ || positionUsec_ < rangeStartUsec_ || positionUsec_ > rangeEndUsec_) {
		positionUsec_ = rangeEndUsec_;
	}

	/* The axis is built from the configured limit, not from the length that
	 * happens to be buffered, so the scale does not grow while it fills up. */
	timeline_->SetMaxDuration(maxTime);
	timeline_->SetRange(rangeStartUsec_, rangeEndUsec_);
	timeline_->SetPosition(positionUsec_);
	/* SetRange clamps or clears a selection that fell out of the rolling
	 * window; keep the values used by the export action in sync. */
	selectionStartUsec_ = timeline_->SelectionStart();
	selectionEndUsec_ = timeline_->SelectionEnd();

	UpdateLabels();
	UpdateControls();

	/* The panel is docked, so it does not have to compete with the preview for
	 * space: show it whenever the buffer is active and let the column layout
	 * decide how much room the rows get. */
	if (!isVisible()) {
		UpdatePanelHeight();
		ShowPanel(true);
	}

	/* The rolling window may have dropped the reviewed position and pulled the
	 * playhead back to the live edge; the canvas has to follow that. */
	UpdateReviewState();
}

void OBSReplayBufferPanel::UpdateLabels()
{
	/* The axis and its markers carry the timing; the only text left is the
	 * transient export status, and it takes no room when idle. */
	cacheLabel_->setText(QTStr("Basic.Main.Preview.ReplayBuffer.Exporting"));
	cacheLabel_->setVisible(exportRunning_);
}

void OBSReplayBufferPanel::UpdateControls()
{
	const bool usable = Active() && !exportRunning_;

	timeline_->setEnabled(usable);
	prevButton_->setEnabled(usable);
	nextButton_->setEnabled(usable);
	saveButton_->setEnabled(usable);
	UpdateGoLiveState();
	exportAction_->setEnabled(usable && timeline_->HasSelection());
}

void OBSReplayBufferPanel::UpdateGoLiveState()
{
	const bool usable = Active() && !exportRunning_;
	const bool parked = Reviewing();

	goLiveButton_->setEnabled(usable);
	goLiveButton_->setIcon(ThemeIcon(parked ? "media/media_play.svg" : "media/media_pause.svg"));
	goLiveButton_->setToolTip(QTStr(parked ? "Basic.Main.Preview.ReplayBuffer.GoLiveHint"
					       : "Basic.Main.Preview.ReplayBuffer.PauseLive"));
}

void OBSReplayBufferPanel::ToggleLivePause()
{
	if (!Active() || exportRunning_) {
		return;
	}

	if (Reviewing()) {
		GoLive();
		return;
	}

	/* Park a moment behind the live edge so the frozen frame is actually shown:
	 * right at rangeEnd the program preview still owns the canvas. */
	followLive_ = false;
	positionUsec_ = qMax(rangeStartUsec_, rangeEndUsec_ - 500000);
	timeline_->SetPosition(positionUsec_);
	UpdateLabels();
	UpdateGoLiveState();
	UpdateReviewState();
	RequestFrame();
}

void OBSReplayBufferPanel::PositionChanged(qint64 position_usec, bool finished)
{
	if (!visible_ || !Active()) {
		return;
	}

	positionUsec_ = qBound(rangeStartUsec_, position_usec, rangeEndUsec_);
	followLive_ = positionUsec_ >= rangeEndUsec_ - 200000;

	UpdateLabels();
	UpdateGoLiveState();
	/* Selecting a historical moment also puts that moment on the big canvas;
	 * back at the live edge the program preview takes over again. */
	UpdateReviewState();

	/* Refresh the picture while the playhead moves, not just on release. The
	 * plugin keeps only the newest request, so a short throttle is enough to
	 * keep the decoder from being flooded. */
	if (finished) {
		dragFrameElapsed_.invalidate();
		RequestFrame();
	} else if (!dragFrameElapsed_.isValid() || dragFrameElapsed_.elapsed() >= kDragFrameIntervalMs) {
		dragFrameElapsed_.start();
		/* Snapping the playhead to a keyframe mid-drag would yank it out from
		 * under the cursor, so only the release snaps. */
		RequestFrame(false);
	}
}

void OBSReplayBufferPanel::SelectionChanged(qint64 start_usec, qint64 end_usec)
{
	if (end_usec <= start_usec) {
		selectionStartUsec_ = 0;
		selectionEndUsec_ = 0;
		exportAction_->setEnabled(false);
		return;
	}

	selectionStartUsec_ = start_usec;
	selectionEndUsec_ = end_usec;
	selectionSettleTimer_.start();
}

void OBSReplayBufferPanel::SnapSelection()
{
	if (!visible_ || !Active()) {
		return;
	}

	if (selectionEndUsec_ <= selectionStartUsec_) {
		exportAction_->setEnabled(false);
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	calldata_t cd = {0};
	calldata_set_int(&cd, "start_usec", selectionStartUsec_);
	calldata_set_int(&cd, "end_usec", selectionEndUsec_);
	proc_handler_call(ph, "snap_replay_selection", &cd);

	const bool valid = calldata_bool(&cd, "valid");
	const int64_t actualStart = calldata_int(&cd, "actual_start_usec");
	const int64_t actualEnd = calldata_int(&cd, "actual_end_usec");
	calldata_free(&cd);

	if (!valid || actualEnd <= actualStart) {
		exportAction_->setEnabled(false);
		return;
	}

	selectionStartUsec_ = actualStart;
	selectionEndUsec_ = actualEnd;
	timeline_->SetSelection(selectionStartUsec_, selectionEndUsec_);
	exportAction_->setEnabled(!exportRunning_ && Active());
}

void OBSReplayBufferPanel::RequestFrame(bool snap)
{
	if (!visible_ || !Active()) {
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	/* The keyframe index is the only place that knows which packet belongs to
	 * the requested time; resolve it there and decode that packet. */
	calldata_t query = {0};
	calldata_set_int(&query, "time_usec", positionUsec_);
	proc_handler_call(ph, "find_replay_keyframe", &query);

	const long long index = calldata_int(&query, "index");
	const int64_t dts = calldata_int(&query, "dts_usec");
	calldata_free(&query);

	if (index < 0) {
		if (snap) {
			blog(LOG_INFO, "[replay buffer] review %.3fs: no keyframe at this position",
			     (double)positionUsec_ / 1000000.0);
		}
		/* Nothing decodable at this position; drop the stale frame so the
		 * canvas falls back to the live picture. */
		ClearReviewFrame();
		return;
	}

	if (snap) {
		blog(LOG_INFO, "[replay buffer] review %.3fs: keyframe %lld at %.3fs",
		     (double)positionUsec_ / 1000000.0, index, (double)dts / 1000000.0);
	}

	/* Snap the playhead to the keyframe that will actually be shown, so the
	 * position label, the canvas and the previous/next keyframe actions all
	 * refer to the same frame. */
	const int64_t snapped = qBound(rangeStartUsec_, dts, rangeEndUsec_);
	if (snap && snapped != positionUsec_) {
		positionUsec_ = snapped;
		followLive_ = snapped >= rangeEndUsec_ - 200000;
		timeline_->SetPosition(snapped);
		UpdateLabels();
		UpdateGoLiveState();
		UpdateReviewState();
	}

	RequestFullFrame(dts);
}
void OBSReplayBufferPanel::RequestFullFrame(int64_t time_usec)
{
	if (!visible_ || !Active()) {
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);
	const uint64_t requestId = ++fullFrameRequestId_;

	calldata_t cd = {0};
	calldata_set_int(&cd, "time_usec", time_usec);
	calldata_set_int(&cd, "request_id", (long long)requestId);
	proc_handler_call(ph, "request_replay_full_frame", &cd);
	const long long accepted = calldata_int(&cd, "accepted");
	calldata_free(&cd);

	if (!accepted) {
		blog(LOG_INFO, "[replay buffer] review: the decoder refused the request");
		return;
	}

	fullFramePending_ = true;
	pendingFullFrameId_ = requestId;
	blog(LOG_INFO, "[replay buffer] review: decode request %llu queued", (unsigned long long)requestId);
	fullFrameElapsed_.restart();
	fullFrameTimer_.start();
}

void OBSReplayBufferPanel::CancelFullFrame()
{
	fullFramePending_ = false;
	fullFrameTimer_.stop();
}

void OBSReplayBufferPanel::PollFullFrame()
{
	if (!fullFramePending_) {
		fullFrameTimer_.stop();
		return;
	}

	if (!visible_ || !Active()) {
		CancelFullFrame();
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		CancelFullFrame();
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	calldata_t query = {0};
	calldata_set_int(&query, "request_id", (long long)pendingFullFrameId_);
	calldata_set_int(&query, "capacity", 0);
	calldata_set_ptr(&query, "dst", nullptr);
	proc_handler_call(ph, "copy_replay_full_frame", &query);

	const int status = (int)calldata_int(&query, "status");
	const int width = (int)calldata_int(&query, "width");
	const int height = (int)calldata_int(&query, "height");
	const long long size = calldata_int(&query, "size");
	calldata_free(&query);

	if (status == kFullFramePending) {
		if (fullFrameElapsed_.elapsed() > 3000) {
			blog(LOG_INFO, "[replay buffer] review: decode timed out");
			CancelFullFrame();
		}
		return;
	}

	blog(LOG_INFO, "[replay buffer] review: copy status %d %dx%d (%lld bytes)", status, width, height, size);

	fullFrameTimer_.stop();
	fullFramePending_ = false;

	if (status == kFullFrameFailed || size <= 0 || width <= 0 || height <= 0) {
		return;
	}

	QImage image(width, height, QImage::Format_RGBA8888);
	if (image.isNull()) {
		return;
	}

	calldata_t copy = {0};
	calldata_set_int(&copy, "request_id", (long long)pendingFullFrameId_);
	calldata_set_int(&copy, "capacity", (long long)image.sizeInBytes());
	calldata_set_ptr(&copy, "dst", image.bits());
	proc_handler_call(ph, "copy_replay_full_frame", &copy);

	const int copyStatus = (int)calldata_int(&copy, "status");
	const long long copied = calldata_int(&copy, "size");
	calldata_free(&copy);

	if (copyStatus != kFullFrameReady || copied <= 0) {
		blog(LOG_INFO, "[replay buffer] review: copy returned status %d (%lld bytes)", copyStatus, copied);
		return;
	}

	/* The canvas only shows this frame while the playhead is off the live
	 * edge; at the live edge the program preview is already correct. */
	if (Reviewing()) {
		SetReviewFrame(image);
	} else {
		blog(LOG_INFO, "[replay buffer] review: frame ready but the panel is not reviewing");
	}
}

void OBSReplayBufferPanel::GoLive()
{
	if (!Active() || exportRunning_) {
		return;
	}

	positionUsec_ = rangeEndUsec_;
	followLive_ = true;
	/* Going live restores the whole scale as well as the playhead. */
	timeline_->ResetZoom();
	timeline_->SetPosition(positionUsec_);
	UpdateLabels();
	UpdateGoLiveState();
	/* Dropping the frozen frame puts the live program back on the canvas. */
	UpdateReviewState();
}

void OBSReplayBufferPanel::PreviousKeyframe()
{
	if (!visible_ || !Active() || exportRunning_) {
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	calldata_t cd = {0};
	calldata_set_int(&cd, "time_usec", positionUsec_);
	proc_handler_call(ph, "find_prev_replay_keyframe", &cd);
	const int index = (int)calldata_int(&cd, "index");
	const int64_t dts = calldata_int(&cd, "dts_usec");
	calldata_free(&cd);

	if (index < 0) {
		return;
	}

	JumpTo(dts);
}

void OBSReplayBufferPanel::NextKeyframe()
{
	if (!visible_ || !Active() || exportRunning_) {
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	calldata_t cd = {0};
	calldata_set_int(&cd, "time_usec", positionUsec_);
	proc_handler_call(ph, "find_next_replay_keyframe", &cd);
	const int index = (int)calldata_int(&cd, "index");
	const int64_t dts = calldata_int(&cd, "dts_usec");
	calldata_free(&cd);

	if (index < 0) {
		return;
	}

	JumpTo(dts);
}

void OBSReplayBufferPanel::JumpTo(int64_t position_usec)
{
	positionUsec_ = qBound(rangeStartUsec_, position_usec, rangeEndUsec_);
	followLive_ = positionUsec_ >= rangeEndUsec_ - 200000;

	timeline_->SetPosition(positionUsec_);
	UpdateLabels();
	UpdateGoLiveState();
	/* Previous/next keyframe also move the reviewed moment, so the canvas has
	 * to follow them exactly like a drag does. */
	UpdateReviewState();
	RequestFrame();
}

void OBSReplayBufferPanel::SaveReplay()
{
	if (Active()) {
		main_->ReplayBufferSave();
	}
}

void OBSReplayBufferPanel::ExportSelection()
{
	if (!visible_ || !Active() || exportRunning_) {
		return;
	}

	if (selectionEndUsec_ <= selectionStartUsec_) {
		ShowExportError(kExportErrInvalid);
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	calldata_t cd = {0};
	calldata_set_int(&cd, "start_usec", selectionStartUsec_);
	calldata_set_int(&cd, "end_usec", selectionEndUsec_);
	proc_handler_call(ph, "export_replay_selection", &cd);

	const int result = (int)calldata_int(&cd, "result");
	const int64_t actualStart = calldata_int(&cd, "actual_start_usec");
	const int64_t actualEnd = calldata_int(&cd, "actual_end_usec");
	calldata_free(&cd);

	if (result != kExportErrNone) {
		ShowExportError(result);
		return;
	}

	selectionStartUsec_ = actualStart;
	selectionEndUsec_ = actualEnd;
	timeline_->SetSelection(selectionStartUsec_, selectionEndUsec_);

	exportRunning_ = true;
	exportTimer_.start();
	UpdateControls();
	UpdateLabels();
}

void OBSReplayBufferPanel::PollExport()
{
	if (!exportRunning_) {
		exportTimer_.stop();
		return;
	}

	if (!Active()) {
		exportRunning_ = false;
		exportTimer_.stop();
		UpdateControls();
		UpdateLabels();
		return;
	}

	OBSOutputAutoRelease output = main_->GetReplayBufferOutput();
	if (!output) {
		exportRunning_ = false;
		exportTimer_.stop();
		return;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(output);

	calldata_t cd = {0};
	proc_handler_call(ph, "get_replay_export_status", &cd);
	const int state = (int)calldata_int(&cd, "state");
	const int error = (int)calldata_int(&cd, "error");
	calldata_free(&cd);

	if (state == kExportRunning) {
		return;
	}

	exportRunning_ = false;
	exportTimer_.stop();
	UpdateControls();
	UpdateLabels();

	if (state != kExportDone) {
		ShowExportError(error);
		return;
	}

	calldata_t pathCd = {0};
	proc_handler_call(ph, "get_last_replay", &pathCd);
	const char *lastPath = calldata_string(&pathCd, "path");
	const std::string path = lastPath ? lastPath : "";
	calldata_free(&pathCd);

	QMessageBox::information(main_, QTStr("Basic.Main.Preview.ReplayBuffer.Title"),
				 QTStr("Basic.Main.Preview.ReplayBuffer.ExportDone").arg(QT_UTF8(path.c_str())));
}

void OBSReplayBufferPanel::ShowExportError(int error)
{
	QString message;

	switch (error) {
	case kExportErrInvalid:
		message = QTStr("Basic.Main.Preview.ReplayBuffer.ExportInvalid");
		break;
	case kExportErrBusy:
		message = QTStr("Basic.Main.Preview.ReplayBuffer.ExportBusy");
		break;
	case kExportErrInactive:
		message = QTStr("Basic.Main.Preview.ReplayBuffer.NotRunning");
		break;
	case kExportErrWrite:
	default:
		message = QTStr("Basic.Main.Preview.ReplayBuffer.ExportWriteFailed");
		break;
	}

	QMessageBox::warning(main_, QTStr("Basic.Main.Preview.ReplayBuffer.Title"), message);
}
