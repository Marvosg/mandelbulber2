/*
 * Mandelbulber v2 — MCP scene, look, animation, and render-job commands.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MANDELBULBER2_SRC_MCP_SCENE_H_
#define MANDELBULBER2_SRC_MCP_SCENE_H_

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace mcpScene
{
// One background still, preview, or animation the window can show and stop.
struct sBackgroundRender
{
	QString id;
	QString title;
	QString summary;
	QString message;
	QString status;
	// "main" runs one at a time. "agent" is previews and drafts, beside the main queue.
	QString lane;
	QImage thumbnail;
	int thumbGeneration = 0;
	double progress = 0;
	bool running = false;
	bool queued = false;
	bool stopping = false;
	bool cancelledByUser = false;
};

// True when this method was handled. *result is the reply object (with ok:false on errors).
bool Dispatch(const QString &method, const QJsonObject &params, QJsonObject *result);
// True while an agent preview is rendering. A queued or main-queue job does not block edits.
bool JobActive();
// How many background jobs have not finished. Used to share CPU cores.
int RunningJobCount();
// Running jobs, plus ones that finished in the last few seconds.
QList<sBackgroundRender> BackgroundRenders();
// Stop one background job from the window. render_status then reports cancelled.
bool CancelBackgroundRender(const QString &id);
// Queue a full still of the current session. The viewport is not used.
// Returns the job id, or empty and sets *error.
QString QueueWindowStill(QString *error);

// One screen-sized sample of a background job. src is the region of the render
// to show; the image is dst pixels. width and height are the full render.
struct sJobView
{
	QImage image;
	int width = 0;
	int height = 0;
	bool found = false;
	bool running = false;
	QString title;
	double progress = 0;
};

// Keep the render buffer alive while a preview window is open.
void SetPreviewWatch(const QString &id, bool watch);
sJobView SampleJobView(
	const QString &id, int srcX, int srcY, int srcW, int srcH, int dstW, int dstH);
// Continue the background still saved by the last interrupted agent render.
bool ResumeBackground();
// Interrupted stills and animations that the startup dialog can offer.
QJsonArray Checkpoints();
}

#endif /* MANDELBULBER2_SRC_MCP_SCENE_H_ */
