/*
 * Mandelbulber v2 — checkpoint and resume for an interrupted still.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MANDELBULBER2_SRC_RENDER_CHECKPOINT_HPP_
#define MANDELBULBER2_SRC_RENDER_CHECKPOINT_HPP_

#include <memory>

#include <QList>
#include <QString>

class cAnimationFrames;
class cFractalContainer;
class cImage;
class cKeyframes;
class cParameterContainer;
class cScheduler;

namespace renderCheckpoint
{
struct sSummary
{
	QString source;
	// still, flight, keyframe, or animation.
	QString kind = QString("still");
	int width = 0;
	int height = 0;
	int progressiveStep = 1;
	int doneLines = 0;
	QString output;
	bool animation = false;
	int animFrame = 0;
	int animEnd = 0;
	bool valid = false;
};

// Lines already finished, and the progressive pass they belong to.
struct sResume
{
	int progressiveStep = 1;
	QList<int> lines;
	bool valid = false;
};

QList<sSummary> List();
sSummary Summary(const QString &source);
void Discard(const QString &source);

// The next Begin() for this source restores the saved image instead of starting over.
// output must match the interrupted frame. An empty output matches any still.
void Arm(const QString &source, const QString &output = QString());
void Disarm(const QString &source);
bool IsArmed(const QString &source);

bool LoadSettings(const QString &source, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal);

// Remembers a background animation so a later launch can render the remaining frames.
struct sAnimation
{
	QString mode;
	int start = 0;
	int end = 0;
	int frame = 0;
	int width = 0;
	int height = 0;
	QString directory;
	QString frameOutput;
	QStringList finished;
	bool contactSheet = false;
	int frameEdge = 96;
	int columns = 0;
	bool valid = false;
};

void StartAnimation(const sAnimation &animation, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal, const std::shared_ptr<cAnimationFrames> &flight,
	const std::shared_ptr<cKeyframes> &keyframes);
sAnimation ReadAnimation(const QString &source);
bool LoadAnimation(const QString &source, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal, const std::shared_ptr<cAnimationFrames> &flight,
	const std::shared_ptr<cKeyframes> &keyframes);
// Records a finished frame and drops its pixel log. The plan stays until Finish().
void AdvanceAnimation(int nextFrame, const QString &pngPath);

// Starts a checkpoint for this render. Restores pixels when this source was armed.
// kind is still, flight, keyframe, or animation.
void Begin(const QString &source, const QString &output, const QString &kind,
	const std::shared_ptr<cImage> &image, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal);
void Save(const std::shared_ptr<cImage> &image, cScheduler *scheduler);
// A finished picture. An animation keeps its plan and only drops the pixel log.
void OnImageDone();
// Drops the checkpoint after the picture, or the whole animation, has been written.
void Finish();
// Hands the restored line list to the renderer once.
sResume TakeResume();
}

#endif /* MANDELBULBER2_SRC_RENDER_CHECKPOINT_HPP_ */
