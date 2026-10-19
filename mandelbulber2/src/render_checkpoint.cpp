/*
 * Mandelbulber v2 — checkpoint and resume for an interrupted render.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Finished scanlines of the final pass are appended, not rewritten. A 2560x1440
 * frame is about 60 KB per line, so a checkpoint costs only the lines that
 * finished since the previous one. Coarse progressive passes are not stored;
 * they are short next to the final pass. An animation also stores its frame
 * plan, so the remaining frames continue after the interrupted one.
 */

#include "render_checkpoint.hpp"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>

#include "animation_frames.hpp"
#include "cimage.hpp"
#include "fractal_container.hpp"
#include "keyframes.hpp"
#include "parameters.hpp"
#include "scheduler.hpp"
#include "settings.hpp"
#include "system_directories.hpp"
#include "write_log.hpp"

namespace
{
const char *kSources[] = {"window", "background"};
const qint32 kVersion = 2;

QMutex gMutex;
QSet<QString> gArmed;
QString gArmedOutput;
QString gActive;
QString gOutput;
QString gKind;
bool gHasAnim = false;
QSet<int> gFlushed;
renderCheckpoint::sResume gResume;

QString Directory(const QString &source)
{
	return systemDirectories.GetDataDirectoryHidden() + "render-checkpoint/" + source;
}

bool WriteBlock(QDataStream &out, const void *data, int bytes)
{
	return out.writeRawData(reinterpret_cast<const char *>(data), bytes) == bytes;
}

bool ReadBlock(QDataStream &in, void *data, int bytes)
{
	return in.readRawData(reinterpret_cast<char *>(data), bytes) == bytes;
}

void RemoveDirectory(const QString &source)
{
	QDir(Directory(source)).removeRecursively();
}

int RowBytes(int width)
{
	return width * (int(sizeof(sRGBFloat)) + int(sizeof(float)) + int(sizeof(quint16)) * 2
									 + int(sizeof(sRGB8)));
}

bool WriteRow(QDataStream &out, cImage &image, int y)
{
	const int width = int(image.GetWidth());
	const qint64 origin = qint64(y) * width;
	return WriteBlock(out, image.GetImageFloat().data() + origin, width * int(sizeof(sRGBFloat)))
				 && WriteBlock(out, image.GetZBuffer().data() + origin, width * int(sizeof(float)))
				 && WriteBlock(out, image.GetAlphaBuf().data() + origin, width * int(sizeof(quint16)))
				 && WriteBlock(out, image.GetOpacity().data() + origin, width * int(sizeof(quint16)))
				 && WriteBlock(out, image.GetColor().data() + origin, width * int(sizeof(sRGB8)));
}

bool ReadRow(QDataStream &in, cImage *image, int y)
{
	const int width = int(image->GetWidth());
	const qint64 origin = qint64(y) * width;
	return ReadBlock(in, image->GetImageFloat().data() + origin, width * int(sizeof(sRGBFloat)))
				 && ReadBlock(in, image->GetZBuffer().data() + origin, width * int(sizeof(float)))
				 && ReadBlock(in, image->GetAlphaBuf().data() + origin, width * int(sizeof(quint16)))
				 && ReadBlock(in, image->GetOpacity().data() + origin, width * int(sizeof(quint16)))
				 && ReadBlock(in, image->GetColor().data() + origin, width * int(sizeof(sRGB8)));
}

void WriteMeta(const QString &source, int width, int height, int step, int done)
{
	QJsonObject meta;
	meta["source"] = source;
	meta["kind"] = gKind;
	meta["width"] = width;
	meta["height"] = height;
	meta["progressive_step"] = step;
	meta["done_lines"] = done;
	meta["output"] = gOutput;
	QSaveFile metaFile(Directory(source) + "/meta.json");
	if (!metaFile.open(QIODevice::WriteOnly)) return;
	metaFile.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
	metaFile.commit();
}

renderCheckpoint::sAnimation ReadAnimFile(const QString &source)
{
	renderCheckpoint::sAnimation animation;
	QFile file(Directory(source) + "/anim.json");
	if (!file.open(QIODevice::ReadOnly)) return animation;
	const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
	animation.mode = json.value("mode").toString();
	animation.start = json.value("start").toInt();
	animation.end = json.value("end").toInt();
	animation.frame = json.value("frame").toInt();
	animation.width = json.value("width").toInt();
	animation.height = json.value("height").toInt();
	animation.directory = json.value("directory").toString();
	animation.frameOutput = json.value("frame_output").toString();
	animation.contactSheet = json.value("contact_sheet").toBool();
	animation.frameEdge = json.value("frame_edge").toInt(96);
	animation.columns = json.value("columns").toInt();
	const QJsonArray finished = json.value("finished").toArray();
	for (const QJsonValue &path : finished)
		animation.finished.append(path.toString());
	animation.valid = (animation.mode == QLatin1String("keyframe") || animation.mode == QLatin1String("flight"))
										&& animation.end > animation.frame && animation.width > 0 && animation.height > 0
										&& QFile::exists(Directory(source) + "/animation.fract");
	return animation;
}

void WriteAnimFile(const QString &source, const renderCheckpoint::sAnimation &animation)
{
	QJsonObject json;
	json["mode"] = animation.mode;
	json["start"] = animation.start;
	json["end"] = animation.end;
	json["frame"] = animation.frame;
	json["width"] = animation.width;
	json["height"] = animation.height;
	json["directory"] = animation.directory;
	json["frame_output"] = animation.frameOutput;
	json["contact_sheet"] = animation.contactSheet;
	json["frame_edge"] = animation.frameEdge;
	json["columns"] = animation.columns;
	QJsonArray finished;
	for (const QString &path : animation.finished)
		finished.append(path);
	json["finished"] = finished;
	QSaveFile file(Directory(source) + "/anim.json");
	if (!file.open(QIODevice::WriteOnly)) return;
	file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
	file.commit();
}

// Counts finished lines in the append log. A truncated final record is ignored.
int CountLoggedLines(const QString &path, int *widthOut, int *heightOut)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return 0;
	QDataStream in(&file);
	in.setByteOrder(QDataStream::LittleEndian);
	in.setVersion(QDataStream::Qt_5_12);
	char magic[4];
	if (!ReadBlock(in, magic, 4)) return 0;
	if (magic[0] == 'M' && magic[1] == 'B' && magic[2] == 'L' && magic[3] == '1')
	{
		qint32 version = 0;
		qint32 width = 0;
		qint32 height = 0;
		qint32 step = 0;
		qint32 done = 0;
		in >> version >> width >> height >> step >> done;
		if (in.status() != QDataStream::Ok || done < 1) return 0;
		if (widthOut) *widthOut = width;
		if (heightOut) *heightOut = height;
		return done;
	}
	if (!(magic[0] == 'M' && magic[1] == 'B' && magic[2] == 'L' && magic[3] == '2')) return 0;
	qint32 version = 0;
	qint32 width = 0;
	qint32 height = 0;
	in >> version >> width >> height;
	if (in.status() != QDataStream::Ok || version != kVersion || width < 1 || height < 1) return 0;
	if (widthOut) *widthOut = width;
	if (heightOut) *heightOut = height;
	const int rowBytes = RowBytes(width);
	QSet<int> lines;
	QByteArray skip(rowBytes, 0);
	while (true)
	{
		qint32 line = -1;
		qint32 step = 0;
		in >> line >> step;
		if (in.status() != QDataStream::Ok || line < 0 || line >= height) break;
		if (!ReadBlock(in, skip.data(), rowBytes)) break;
		if (step == 1) lines.insert(line);
	}
	return lines.size();
}

renderCheckpoint::sSummary ReadSummary(const QString &source)
{
	renderCheckpoint::sSummary summary;
	summary.source = source;
	const QString dir = Directory(source);
	const renderCheckpoint::sAnimation animation = ReadAnimFile(source);
	QFile metaFile(dir + "/meta.json");
	if (metaFile.open(QIODevice::ReadOnly))
	{
		const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
		summary.kind = meta.value("kind").toString(QString("still"));
		summary.output = meta.value("output").toString();
		summary.width = meta.value("width").toInt();
		summary.height = meta.value("height").toInt();
		summary.progressiveStep = meta.value("progressive_step").toInt(1);
	}
	int loggedWidth = 0;
	int loggedHeight = 0;
	const int logged = CountLoggedLines(dir + "/image.bin", &loggedWidth, &loggedHeight);
	if (logged > 0)
	{
		summary.doneLines = logged;
		summary.width = loggedWidth;
		summary.height = loggedHeight;
	}
	if (animation.valid)
	{
		summary.animation = true;
		summary.kind = QString("animation");
		summary.animFrame = animation.frame;
		summary.animEnd = animation.end;
		summary.width = animation.width;
		summary.height = animation.height;
		summary.output = animation.frameOutput;
		summary.valid = true;
		return summary;
	}
	const bool hasSettings = QFile::exists(dir + "/settings.fract");
	summary.valid = logged > 0 && hasSettings && summary.width > 0 && summary.height > 0;
	return summary;
}

void TruncateLog(const QString &source)
{
	QFile::remove(Directory(source) + "/image.bin");
	gFlushed.clear();
}

bool RestoreImage(const std::shared_ptr<cImage> &image, renderCheckpoint::sResume *resume)
{
	QFile file(Directory(gActive) + "/image.bin");
	if (!file.open(QIODevice::ReadOnly)) return false;
	QDataStream in(&file);
	in.setByteOrder(QDataStream::LittleEndian);
	in.setVersion(QDataStream::Qt_5_12);
	char magic[4];
	if (!ReadBlock(in, magic, 4)) return false;
	const int width = int(image->GetWidth());
	const int height = int(image->GetHeight());
	const qint64 count = qint64(width) * height;
	if (qint64(image->GetImageFloat().size()) != count || qint64(image->GetZBuffer().size()) != count
			|| qint64(image->GetAlphaBuf().size()) != count || qint64(image->GetOpacity().size()) != count
			|| qint64(image->GetColor().size()) != count)
		return false;

	resume->lines.clear();
	if (magic[0] == 'M' && magic[1] == 'B' && magic[2] == 'L' && magic[3] == '1')
	{
		qint32 version = 0;
		qint32 fileWidth = 0;
		qint32 fileHeight = 0;
		qint32 step = 0;
		qint32 done = 0;
		in >> version >> fileWidth >> fileHeight >> step >> done;
		if (in.status() != QDataStream::Ok || fileWidth != width || fileHeight != height || done < 1)
			return false;
		for (qint32 i = 0; i < done; i++)
		{
			qint32 line = -1;
			in >> line;
			if (line < 0 || line >= height) return false;
			resume->lines.append(line);
		}
		const int floatBytes = int(count * qint64(sizeof(sRGBFloat)));
		const int zBytes = int(count * qint64(sizeof(float)));
		const int alphaBytes = int(count * qint64(sizeof(quint16)));
		const int colorBytes = int(count * qint64(sizeof(sRGB8)));
		if (!ReadBlock(in, image->GetImageFloat().data(), floatBytes)
				|| !ReadBlock(in, image->GetZBuffer().data(), zBytes)
				|| !ReadBlock(in, image->GetAlphaBuf().data(), alphaBytes)
				|| !ReadBlock(in, image->GetOpacity().data(), alphaBytes)
				|| !ReadBlock(in, image->GetColor().data(), colorBytes))
			return false;
		resume->progressiveStep = step > 0 ? step : 1;
	}
	else if (magic[0] == 'M' && magic[1] == 'B' && magic[2] == 'L' && magic[3] == '2')
	{
		qint32 version = 0;
		qint32 fileWidth = 0;
		qint32 fileHeight = 0;
		in >> version >> fileWidth >> fileHeight;
		if (in.status() != QDataStream::Ok || version != kVersion || fileWidth != width
				|| fileHeight != height)
			return false;
		QSet<int> seen;
		while (true)
		{
			qint32 line = -1;
			qint32 step = 0;
			in >> line >> step;
			if (in.status() != QDataStream::Ok || line < 0 || line >= height) break;
			if (!ReadRow(in, image.get(), line)) break;
			if (step == 1) seen.insert(line);
		}
		if (seen.isEmpty()) return false;
		resume->lines = seen.values();
		resume->progressiveStep = 1;
	}
	else
		return false;

	resume->valid = !resume->lines.isEmpty();
	if (!resume->valid) return false;
	QList<int> previewLines = resume->lines;
	image->CompileImage(&previewLines);
	image->ConvertTo8bitChar();
	if (image->GetPreviewWidth() > 0) image->UpdatePreview(&previewLines);
	gFlushed = QSet<int>(resume->lines.begin(), resume->lines.end());
	return true;
}

void WriteSettings(const QString &path, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal, const std::shared_ptr<cAnimationFrames> &flight,
	const std::shared_ptr<cKeyframes> &keyframes)
{
	auto saved = std::make_shared<cParameterContainer>(*par);
	auto savedFractal = std::make_shared<cFractalContainer>(*fractal);
	if (saved->IfExists("temporary_scale")) saved->Set("temporary_scale", 0);
	cSettings settings(cSettings::formatFullText);
	settings.BeQuiet(true);
	settings.CreateText(saved, savedFractal, flight, keyframes);
	settings.SaveToFile(path);
}
}

namespace renderCheckpoint
{
QList<sSummary> List()
{
	QList<sSummary> found;
	for (const char *source : kSources)
	{
		const sSummary summary = ReadSummary(QString(source));
		if (summary.valid) found.append(summary);
	}
	return found;
}

sSummary Summary(const QString &source)
{
	return ReadSummary(source);
}

void Discard(const QString &source)
{
	QMutexLocker locker(&gMutex);
	gArmed.remove(source);
	if (gActive == source)
	{
		gActive.clear();
		gHasAnim = false;
	}
	RemoveDirectory(source);
}

void Arm(const QString &source, const QString &output)
{
	QMutexLocker locker(&gMutex);
	gArmed.insert(source);
	gArmedOutput = output;
}

void Disarm(const QString &source)
{
	QMutexLocker locker(&gMutex);
	gArmed.remove(source);
	gArmedOutput.clear();
}

bool IsArmed(const QString &source)
{
	QMutexLocker locker(&gMutex);
	return gArmed.contains(source);
}

bool LoadSettings(const QString &source, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal)
{
	const QString path = Directory(source) + "/settings.fract";
	if (!QFile::exists(path)) return false;
	cSettings settings(cSettings::formatFullText);
	settings.BeQuiet(true);
	if (!settings.LoadFromFile(path)) return false;
	return settings.Decode(par, fractal);
}

void StartAnimation(const sAnimation &animation, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal, const std::shared_ptr<cAnimationFrames> &flight,
	const std::shared_ptr<cKeyframes> &keyframes)
{
	const QString source = QString("background");
	bool keep = false;
	{
		QMutexLocker locker(&gMutex);
		gHasAnim = true;
		gActive = source;
		keep = gArmed.contains(source) && QFile::exists(Directory(source) + "/anim.json");
	}
	if (keep) return;
	QDir().mkpath(Directory(source));
	WriteSettings(Directory(source) + "/animation.fract", par, fractal, flight, keyframes);
	WriteAnimFile(source, animation);
}

sAnimation ReadAnimation(const QString &source)
{
	return ReadAnimFile(source);
}

bool LoadAnimation(const QString &source, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal, const std::shared_ptr<cAnimationFrames> &flight,
	const std::shared_ptr<cKeyframes> &keyframes)
{
	const QString path = Directory(source) + "/animation.fract";
	if (!QFile::exists(path)) return false;
	cSettings settings(cSettings::formatFullText);
	settings.BeQuiet(true);
	if (!settings.LoadFromFile(path)) return false;
	return settings.Decode(par, fractal, flight, keyframes);
}

void AdvanceAnimation(int nextFrame, const QString &pngPath)
{
	const QString source = QString("background");
	sAnimation animation = ReadAnimFile(source);
	if (!animation.valid) return;
	animation.frame = nextFrame;
	if (!pngPath.isEmpty()) animation.finished.append(pngPath);
	animation.frameOutput = QDir(animation.directory)
														 .filePath(QString("frame_%1").arg(nextFrame, 4, 10, QChar('0')));
	WriteAnimFile(source, animation);
	QMutexLocker locker(&gMutex);
	TruncateLog(source);
}

void Begin(const QString &source, const QString &output, const QString &kind,
	const std::shared_ptr<cImage> &image, const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal)
{
	bool resume = false;
	bool leaveUntouched = false;
	{
		QMutexLocker locker(&gMutex);
		if (kind != QLatin1String("animation")) gHasAnim = false;
		const bool armed = gArmed.contains(source);
		if (armed && !gArmedOutput.isEmpty() && gArmedOutput != output)
			leaveUntouched = true;
		else if (armed)
		{
			resume = true;
			gArmed.remove(source);
		}
		if (!leaveUntouched)
		{
			gActive = source;
			gOutput = output;
			gKind = kind.isEmpty() ? QString("still") : kind;
			gResume = sResume();
		}
	}
	if (leaveUntouched) return;
	if (!resume)
	{
		QMutexLocker locker(&gMutex);
		if (gHasAnim)
			TruncateLog(source);
		else
			RemoveDirectory(source);
	}
	QDir().mkpath(Directory(source));
	if (!gHasAnim) WriteSettings(Directory(source) + "/settings.fract", par, fractal, nullptr, nullptr);

	if (resume && image)
	{
		sResume restored;
		if (RestoreImage(image, &restored))
		{
			QMutexLocker locker(&gMutex);
			if (gActive == source) gResume = restored;
			WriteLog(QString("Render checkpoint: restored %1 lines of %2 (%3)")
									 .arg(restored.lines.size())
									 .arg(image->GetHeight())
									 .arg(source),
				1);
		}
		else
		{
			QMutexLocker locker(&gMutex);
			TruncateLog(source);
			WriteLog(QString("Render checkpoint: could not restore %1, rendering this frame from the start")
									 .arg(source),
				1);
		}
	}
}

void Save(const std::shared_ptr<cImage> &image, cScheduler *scheduler)
{
	QString source;
	{
		QMutexLocker locker(&gMutex);
		source = gActive;
	}
	if (source.isEmpty() || !image || !scheduler) return;
	// The final pass is the expensive one. Earlier progressive passes are short.
	if (scheduler->GetProgressiveStep() != 1) return;

	const QList<int> lines = scheduler->SnapshotDoneLines();
	QList<int> fresh;
	for (int line : lines)
	{
		if (!gFlushed.contains(line)) fresh.append(line);
	}
	if (fresh.isEmpty()) return;

	const int width = int(image->GetWidth());
	const int height = int(image->GetHeight());
	const qint64 count = qint64(width) * height;
	if (width < 1 || height < 1 || qint64(image->GetImageFloat().size()) != count
			|| qint64(image->GetZBuffer().size()) != count || qint64(image->GetAlphaBuf().size()) != count
			|| qint64(image->GetOpacity().size()) != count || qint64(image->GetColor().size()) != count)
		return;

	const QString path = Directory(source) + "/image.bin";
	const qint64 headerBytes = 16;
	const bool existed = QFile::exists(path) && QFile(path).size() >= headerBytes;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | (existed ? QIODevice::Append : QIODevice::Truncate))) return;
	QDataStream out(&file);
	out.setByteOrder(QDataStream::LittleEndian);
	out.setVersion(QDataStream::Qt_5_12);
	if (!existed)
	{
		const char magic[4] = {'M', 'B', 'L', '2'};
		if (!WriteBlock(out, magic, 4)) return;
		out << kVersion << qint32(width) << qint32(height);
	}
	for (int line : fresh)
	{
		if (line < 0 || line >= height) continue;
		// One record, then the file. A kill mid-write drops only that record.
		QByteArray record;
		QDataStream row(&record, QIODevice::WriteOnly);
		row.setByteOrder(QDataStream::LittleEndian);
		row.setVersion(QDataStream::Qt_5_12);
		row << qint32(line) << qint32(1);
		if (!WriteRow(row, *image, line)) return;
		if (!WriteBlock(out, record.constData(), record.size())) return;
		gFlushed.insert(line);
	}
	file.flush();
	WriteMeta(source, width, height, 1, gFlushed.size());
}

void OnImageDone()
{
	bool dropAll = false;
	QString source;
	{
		QMutexLocker locker(&gMutex);
		source = gActive;
		dropAll = !gHasAnim;
		if (!dropAll) TruncateLog(source);
	}
	if (dropAll && !source.isEmpty()) Finish();
}

void Finish()
{
	QString source;
	{
		QMutexLocker locker(&gMutex);
		source = gActive.isEmpty() ? QString("background") : gActive;
		if (gActive.isEmpty() && !gHasAnim) source.clear();
		gActive.clear();
		gOutput.clear();
		gKind.clear();
		gHasAnim = false;
		gFlushed.clear();
		gResume = sResume();
	}
	if (!source.isEmpty()) RemoveDirectory(source);
}

sResume TakeResume()
{
	QMutexLocker locker(&gMutex);
	const sResume resume = gResume;
	gResume = sResume();
	return resume;
}
}
