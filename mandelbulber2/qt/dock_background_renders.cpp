/*
 * Mandelbulber v2 — main and agent background-render queues.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dock_background_renders.h"

#include "my_progress_bar.h"
#include "src/mcp_scene.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace
{
class cPreviewCanvas : public QWidget
{
public:
	explicit cPreviewCanvas(const QString &jobId, QWidget *parent = nullptr) : QWidget(parent), id(jobId)
	{
		setMinimumSize(320, 180);
		setMouseTracking(true);
		mcpScene::SetPreviewWatch(id, true);
	}
	~cPreviewCanvas() override { mcpScene::SetPreviewWatch(id, false); }

	void Resample();
	bool Running() const { return running; }
	bool Found() const { return found; }
	bool HasFrame() const { return !frame.isNull(); }
	QString Title() const { return title; }
	double Progress() const { return progress; }

protected:
	void paintEvent(QPaintEvent *) override;
	void wheelEvent(QWheelEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void resizeEvent(QResizeEvent *) override;

private:
	double FitZoom() const;
	QRect SourceRect(int viewW, int viewH) const;
	QPointF MapToSource(const QPointF &view) const;

	QString id;
	QImage frame;
	QString title;
	int sourceW = 0;
	int sourceH = 0;
	double zoom = 0;
	double progress = 0;
	QPointF center;
	bool haveCenter = false;
	bool dragging = false;
	bool running = false;
	bool found = false;
	QPointF dragOrigin;
	QPointF centerOrigin;
	qint64 lastSampleMs = 0;
};

double cPreviewCanvas::FitZoom() const
{
	if (sourceW < 2 || sourceH < 2 || width() < 2 || height() < 2) return 1;
	return std::min(double(width()) / double(sourceW), double(height()) / double(sourceH));
}

QRect cPreviewCanvas::SourceRect(int viewW, int viewH) const
{
	if (zoom <= 0 || sourceW < 2) return QRect(0, 0, qMax(1, sourceW), qMax(1, sourceH));
	const double srcW = qMax(1.0, viewW / zoom);
	const double srcH = qMax(1.0, viewH / zoom);
	const int x = int(std::floor(center.x() - srcW / 2.0));
	const int y = int(std::floor(center.y() - srcH / 2.0));
	return QRect(x, y, qMax(1, int(std::ceil(srcW))), qMax(1, int(std::ceil(srcH))));
}

QPointF cPreviewCanvas::MapToSource(const QPointF &view) const
{
	const double z = zoom <= 0 ? FitZoom() : zoom;
	if (zoom <= 0)
	{
		const double drawnW = sourceW * z;
		const double drawnH = sourceH * z;
		const double ox = (width() - drawnW) / 2.0;
		const double oy = (height() - drawnH) / 2.0;
		return QPointF((view.x() - ox) / z, (view.y() - oy) / z);
	}
	return QPointF(center.x() + (view.x() - width() / 2.0) / z, center.y() + (view.y() - height() / 2.0) / z);
}

void cPreviewCanvas::Resample()
{
	const qreal ratio = devicePixelRatioF();
	const int viewW = qMax(1, width());
	const int viewH = qMax(1, height());
	int sampleW = qMax(1, int(std::lround(viewW * ratio)));
	int sampleH = qMax(1, int(std::lround(viewH * ratio)));
	QRect src = sourceW > 1 ? SourceRect(viewW, viewH) : QRect(0, 0, 1, 1);
	if (zoom <= 0 && sourceW > 1 && sourceH > 1)
	{
		const double z = FitZoom() * ratio;
		sampleW = qMax(1, int(std::lround(sourceW * z)));
		sampleH = qMax(1, int(std::lround(sourceH * z)));
		src = QRect(0, 0, sourceW, sourceH);
	}
	mcpScene::sJobView view =
		mcpScene::SampleJobView(id, src.x(), src.y(), src.width(), src.height(), sampleW, sampleH);
	if (view.width > 1 && sourceW != view.width)
	{
		sourceW = view.width;
		sourceH = view.height;
		if (!haveCenter)
		{
			center = QPointF(sourceW / 2.0, sourceH / 2.0);
			haveCenter = true;
		}
		if (zoom <= 0)
		{
			const double z = FitZoom() * ratio;
			sampleW = qMax(1, int(std::lround(sourceW * z)));
			sampleH = qMax(1, int(std::lround(sourceH * z)));
			src = QRect(0, 0, sourceW, sourceH);
		}
		else
			src = SourceRect(viewW, viewH);
		view = mcpScene::SampleJobView(id, src.x(), src.y(), src.width(), src.height(), sampleW, sampleH);
	}
	if (view.width > 1)
	{
		sourceW = view.width;
		sourceH = view.height;
	}
	title = view.title;
	progress = view.progress;
	running = view.running;
	found = view.found;
	frame = view.image;
	if (!frame.isNull()) frame.setDevicePixelRatio(ratio);
	lastSampleMs = QDateTime::currentMSecsSinceEpoch();
	update();
}

void cPreviewCanvas::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(24, 24, 28));
	if (frame.isNull())
	{
		painter.setPen(palette().color(QPalette::PlaceholderText));
		painter.drawText(rect(), Qt::AlignCenter,
			running ? tr("Waiting for the first pixels…") : tr("Waiting for this render to start…"));
		return;
	}
	if (zoom <= 0)
	{
		const QSize box(int(std::lround(frame.width() / frame.devicePixelRatio())),
			int(std::lround(frame.height() / frame.devicePixelRatio())));
		const QPoint origin((width() - box.width()) / 2, (height() - box.height()) / 2);
		painter.drawImage(origin, frame);
	}
	else
		painter.drawImage(rect(), frame);
}

void cPreviewCanvas::wheelEvent(QWheelEvent *event)
{
	if (sourceW < 2) return;
	const QPointF at = MapToSource(event->position());
	double z = zoom <= 0 ? FitZoom() : zoom;
	z *= std::pow(1.2, event->angleDelta().y() / 120.0);
	z = std::clamp(z, std::max(0.02, FitZoom() * 0.25), 32.0);
	zoom = z;
	center.setX(at.x() - (event->position().x() - width() / 2.0) / zoom);
	center.setY(at.y() - (event->position().y() - height() / 2.0) / zoom);
	haveCenter = true;
	Resample();
	event->accept();
}

void cPreviewCanvas::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && zoom > 0)
	{
		dragging = true;
		dragOrigin = event->position();
		centerOrigin = center;
		setCursor(Qt::ClosedHandCursor);
	}
}

void cPreviewCanvas::mouseMoveEvent(QMouseEvent *event)
{
	if (!dragging) return;
	const QPointF delta = event->position() - dragOrigin;
	center = centerOrigin - QPointF(delta.x() / zoom, delta.y() / zoom);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - lastSampleMs >= 40) Resample();
}

void cPreviewCanvas::mouseReleaseEvent(QMouseEvent *)
{
	if (!dragging) return;
	dragging = false;
	unsetCursor();
	Resample();
}

void cPreviewCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (sourceW < 2) return;
	if (zoom > 0 && std::abs(zoom - 1.0) < 0.05)
		zoom = 0;
	else
	{
		const QPointF at = MapToSource(event->position());
		zoom = 1;
		center = at;
		haveCenter = true;
	}
	Resample();
}

void cPreviewCanvas::resizeEvent(QResizeEvent *)
{
	Resample();
}
} // namespace

namespace
{
QLabel *SectionTitle(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	QFont font = label->font();
	font.setBold(true);
	label->setFont(font);
	return label;
}

QLabel *SectionHint(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	QPalette palette = label->palette();
	palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
	label->setPalette(palette);
	return label;
}
}

cDockBackgroundRenders::cDockBackgroundRenders(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(6, 6, 6, 6);
	outer->setSpacing(8);

	auto *renderButton = new QPushButton(tr("Render in background"), this);
	renderButton->setToolTip(
		tr("Render the current scene in the main queue. The viewport stays free."));
	connect(renderButton, &QPushButton::clicked, this, &cDockBackgroundRenders::QueueFromWindow);
	outer->addWidget(renderButton);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *host = new QWidget(scroll);
	auto *list = new QVBoxLayout(host);
	list->setContentsMargins(0, 0, 0, 0);
	list->setSpacing(6);

	list->addWidget(SectionTitle(tr("Main renders"), host));
	list->addWidget(SectionHint(tr("One at a time. The next starts when this one finishes."), host));
	mainIdle = SectionHint(tr("Idle"), host);
	list->addWidget(mainIdle);
	auto *mainHost = new QWidget(host);
	mainLayout = new QVBoxLayout(mainHost);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(6);
	list->addWidget(mainHost);

	list->addWidget(SectionTitle(tr("Agent"), host));
	list->addWidget(SectionHint(tr("Previews and drafts. Runs while a main render is going."), host));
	agentIdle = SectionHint(tr("Idle"), host);
	list->addWidget(agentIdle);
	auto *agentHost = new QWidget(host);
	agentLayout = new QVBoxLayout(agentHost);
	agentLayout->setContentsMargins(0, 0, 0, 0);
	agentLayout->setSpacing(6);
	list->addWidget(agentHost);

	list->addStretch(1);
	scroll->setWidget(host);
	outer->addWidget(scroll);

	timer = new QTimer(this);
	timer->setInterval(250);
	connect(timer, &QTimer::timeout, this, &cDockBackgroundRenders::Refresh);

	setMinimumWidth(260);
	setMinimumHeight(160);
}

QSize cDockBackgroundRenders::sizeHint() const
{
	return QSize(300, 280);
}

QFrame *cDockBackgroundRenders::MakeCard(const QString &id, sRow *row)
{
	auto *card = new QFrame(this);
	card->setObjectName(QStringLiteral("backgroundRenderCard"));
	card->setFrameShape(QFrame::NoFrame);
	card->setAttribute(Qt::WA_StyledBackground, true);
	card->setStyleSheet(QStringLiteral(
		"QFrame#backgroundRenderCard {"
		" border: 1px solid palette(mid);"
		" border-radius: 8px;"
		" background-color: palette(window);"
		"}"));
	auto *cardLayout = new QHBoxLayout(card);
	cardLayout->setContentsMargins(8, 8, 8, 8);
	cardLayout->setSpacing(8);

	auto *thumb = new QLabel(card);
	thumb->setFixedSize(84, 63);
	thumb->setAlignment(Qt::AlignCenter);
	thumb->setStyleSheet(QStringLiteral("background-color: palette(base); border-radius: 4px;"));
	cardLayout->addWidget(thumb, 0, Qt::AlignTop);

	auto *textColumn = new QVBoxLayout();
	textColumn->setSpacing(4);
	auto *header = new QHBoxLayout();
	header->setSpacing(8);
	auto *title = new QLabel(card);
	title->setWordWrap(true);
	title->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QFont font = title->font();
	font.setBold(true);
	title->setFont(font);
	auto *view = new QPushButton(tr("View"), card);
	view->setMinimumWidth(64);
	view->setToolTip(tr("Watch this render while it runs. Scroll to zoom in to the actual pixels."));
	auto *stop = new QPushButton(tr("Stop"), card);
	stop->setMinimumWidth(64);
	header->addWidget(title, 1);
	header->addWidget(view, 0, Qt::AlignTop);
	header->addWidget(stop, 0, Qt::AlignTop);
	textColumn->addLayout(header);

	auto *summary = new QLabel(card);
	summary->setWordWrap(true);
	summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
	textColumn->addWidget(summary);

	auto *bar = new MyProgressBar(card);
	bar->setRange(0, 1000);
	bar->setTextVisible(true);
	bar->setFormat(QStringLiteral("%p%"));
	textColumn->addWidget(bar);

	auto *detail = new QLabel(card);
	detail->setWordWrap(true);
	detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QPalette palette = detail->palette();
	palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
	detail->setPalette(palette);
	textColumn->addWidget(detail);
	cardLayout->addLayout(textColumn, 1);

	connect(stop, &QPushButton::clicked, this, [this, id]() {
		mcpScene::CancelBackgroundRender(id);
		Refresh();
	});
	connect(view, &QPushButton::clicked, this, [this, id]() { OpenPreview(id); });

	row->card = card;
	row->thumb = thumb;
	row->title = title;
	row->summary = summary;
	row->detail = detail;
	row->bar = bar;
	row->stop = stop;
	return card;
}

void cDockBackgroundRenders::UpdateRow(sRow &row, const QString &title, const QString &summary,
	const QString &detail, double progress, bool showStop, const QImage &thumbnail, int thumbGeneration)
{
	row.title->setText(title);
	row.summary->setText(summary);
	row.detail->setText(detail);
	row.bar->setValue(int(qBound(0.0, progress, 1.0) * 1000.0));
	row.bar->setFormat(QStringLiteral("%p%"));
	row.stop->setVisible(showStop);
	row.stop->setEnabled(showStop);
	if (thumbGeneration != row.thumbGeneration && !thumbnail.isNull())
	{
		row.thumb->setPixmap(QPixmap::fromImage(thumbnail).scaled(
			row.thumb->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
		row.thumbGeneration = thumbGeneration;
	}
}

void cDockBackgroundRenders::Refresh()
{
	const QList<mcpScene::sBackgroundRender> jobs = mcpScene::BackgroundRenders();
	QSet<QString> live;
	bool reveal = false;
	bool anyActive = false;
	int mainCount = 0;
	int agentCount = 0;
	for (const mcpScene::sBackgroundRender &job : jobs)
	{
		live.insert(job.id);
		const bool active = job.running || job.queued;
		if (active) anyActive = true;
		if (job.running && !seen.contains(job.id))
		{
			seen.insert(job.id);
			reveal = true;
		}
		if (job.lane == QLatin1String("main"))
			mainCount++;
		else
			agentCount++;

		QString detail = job.message;
		const bool showStop = active && !job.stopping;
		if (active && job.stopping)
			detail = tr("Stopping…");
		else if (job.queued)
			detail = job.lane == QLatin1String("main") ? tr("Waiting in the main queue")
																								 : tr("Waiting in the agent queue");
		else if (!job.running && job.cancelledByUser)
			detail = tr("Cancelled");
		else if (!job.running && job.status == QLatin1String("finished"))
			detail = tr("Finished");
		else if (!job.running && job.status == QLatin1String("failed"))
			detail = job.message.isEmpty() ? tr("Failed") : job.message;
		else if (!job.running && job.status == QLatin1String("stopped"))
			detail = tr("Stopped");

		if (!rows.contains(job.id))
		{
			sRow row;
			row.lane = job.lane;
			QFrame *card = MakeCard(job.id, &row);
			QVBoxLayout *laneLayout = job.lane == QLatin1String("main") ? mainLayout : agentLayout;
			laneLayout->addWidget(card);
			rows.insert(job.id, row);
		}

		UpdateRow(rows[job.id], job.title, job.summary, detail, job.progress, showStop, job.thumbnail,
			job.thumbGeneration);
	}

	const QStringList stale = rows.keys();
	for (const QString &id : stale)
	{
		if (live.contains(id)) continue;
		sRow row = rows.take(id);
		row.card->deleteLater();
	}

	mainIdle->setVisible(mainCount == 0);
	agentIdle->setVisible(agentCount == 0);
	if (anyActive || !rows.isEmpty())
	{
		if (!timer->isActive()) timer->start();
	}
	else
		timer->stop();

	if (reveal)
	{
		QWidget *dock = parentWidget();
		if (!dock || !dock->isVisible()) emit requestShow();
	}
}

void cDockBackgroundRenders::QueueFromWindow()
{
	QString error;
	const QString id = mcpScene::QueueWindowStill(&error);
	if (id.isEmpty())
	{
		QMessageBox::warning(this, tr("Background render"),
			error.isEmpty() ? tr("Could not start the render.") : error);
		return;
	}
	Refresh();
}

void cDockBackgroundRenders::OpenPreview(const QString &id)
{
	if (QWidget *existing = previews.value(id))
	{
		existing->raise();
		existing->activateWindow();
		return;
	}
	auto *dialog = new QDialog(window());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setModal(false);
	dialog->setWindowTitle(tr("Background render"));
	dialog->resize(880, 560);
	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(8, 8, 8, 8);
	auto *canvas = new cPreviewCanvas(id, dialog);
	auto *hint = new QLabel(
		tr("Scroll to zoom. Drag to move. Double-click for actual pixels. The viewport stays free."),
		dialog);
	hint->setWordWrap(true);
	layout->addWidget(canvas, 1);
	layout->addWidget(hint);
	auto *previewTimer = new QTimer(dialog);
	previewTimer->setInterval(350);
	connect(previewTimer, &QTimer::timeout, dialog, [dialog, canvas, previewTimer]() {
		canvas->Resample();
		const QString name = canvas->Title().isEmpty() ? dialog->tr("Background render") : canvas->Title();
		if (canvas->Running())
		{
			const int percent = int(qBound(0.0, canvas->Progress(), 1.0) * 100.0);
			dialog->setWindowTitle(QString("%1 — %2%").arg(name).arg(percent));
		}
		else
			dialog->setWindowTitle(name);
		if (!canvas->Found() || (canvas->HasFrame() && !canvas->Running())) previewTimer->stop();
	});
	canvas->Resample();
	previewTimer->start();
	previews.insert(id, dialog);
	dialog->show();
}
