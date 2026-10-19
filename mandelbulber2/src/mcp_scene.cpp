/*
 * Mandelbulber v2 — MCP scene, look, animation, and render-job commands.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mcp_scene.h"

#include "mcp_server.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVector>

#include "algebra.hpp"
#include "animation_flight.hpp"
#include "calculate_distance.hpp"
#include "camera_target.hpp"
#include "common_math.h"
#include "animation_frames.hpp"
#include "animation_keyframes.hpp"
#include "cimage.hpp"
#include "color_structures.hpp"
#include "file_image.hpp"
#include "fractal_container.hpp"
#include "fractparams.hpp"
#include "global_data.hpp"
#include "hybrid_fractal_sequences.h"
#include "initparameters.hpp"
#include "interface.hpp"
#include "keyframes.hpp"
#include "object_node_type.h"
#include "objects_tree.h"
#include "primitive_item.h"
#include "primitives.h"
#include "projection_3d.hpp"
#include "render_data.hpp"
#include "render_job.hpp"
#include "render_window.hpp"
#include "rendering_configuration.hpp"
#include "synchronize_interface.hpp"
#include "system_data.hpp"
#include "system_directories.hpp"
#include "undo.h"

#include "formula/definition/all_fractal_list.hpp"
#include "qt/objects_tree_widget.h"

namespace
{
struct sJob
{
	QString id;
	QString kind;
	QString status = QString("running");
	QString message;
	QString error;
	QString outputDirectory;
	QString imagePath;
	QStringList frames;
	double progress = 0;
	int frame = 0;
	int frameCount = 0;
	int width = 0;
	int height = 0;
	double detailCap = 0;
	bool draft = false;
	bool stop = false;
	// Socket-only. Set immediately before Execute so a stop that arrives
	// before the render clears its flag is still honored.
	bool stopBeforeExecute = false;
	bool wantContactSheet = false;
	int frameEdge = 96;
	int columns = 0;
	QString contactSheetPath;
	std::atomic<bool> done{false};
	QMutex mutex;
	std::shared_ptr<cParameterContainer> par;
	std::shared_ptr<cFractalContainer> fractal;
	// Copies taken on the main thread so the worker does not share gKeyframes.
	std::shared_ptr<cKeyframes> keyframes;
	std::shared_ptr<cAnimationFrames> flight;
};

QHash<QString, std::shared_ptr<sJob>> gJobs;
QString gActiveJobId;
QString gLastImage;
QMutex gImageMutex;
int gJobSerial = 0;

void SetLastImage(const QString &path)
{
	QMutexLocker locker(&gImageMutex);
	gLastImage = path;
}

QString LastImage()
{
	QMutexLocker locker(&gImageMutex);
	return gLastImage;
}

bool RenderBusy()
{
	if (gActiveJobId.isEmpty()) return false;
	const std::shared_ptr<sJob> job = gJobs.value(gActiveJobId);
	if (!job || job->done.load())
	{
		gActiveJobId.clear();
		return false;
	}
	return true;
}

QJsonObject Fail(const QString &message)
{
	QJsonObject object;
	object["ok"] = false;
	object["error"] = message;
	return object;
}

QString CleanPath(const QString &path)
{
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool InsideExamples(const QString &path)
{
	const QString examples = CleanPath(systemDirectories.sharedDir + "examples");
	const QString target = CleanPath(path);
	return !examples.isEmpty() && (target == examples || target.startsWith(examples + "/"));
}

QString NodePrefix(int nodeId)
{
	return QString("node_%1_").arg(nodeId, 4, 10, QChar('0'));
}

QString TypeName(enumNodeType type)
{
	switch (type)
	{
		case enumNodeType::fractal: return "fractal";
		case enumNodeType::primitive: return "primitive";
		case enumNodeType::hybrid: return "hybrid";
		case enumNodeType::booleanAdd: return "boolean_add";
		case enumNodeType::booleanMul: return "boolean_mul";
		case enumNodeType::booleanSub: return "boolean_sub";
		default: return "unknown";
	}
}

bool TypeFromName(const QString &name, enumNodeType *type)
{
	const QString key = name.trimmed().toLower();
	if (key == "fractal") *type = enumNodeType::fractal;
	else if (key == "primitive") *type = enumNodeType::primitive;
	else if (key == "hybrid") *type = enumNodeType::hybrid;
	else if (key == "boolean_add" || key == "add" || key == "union") *type = enumNodeType::booleanAdd;
	else if (key == "boolean_mul" || key == "boolean_multiply" || key == "intersect")
		*type = enumNodeType::booleanMul;
	else if (key == "boolean_sub" || key == "subtract") *type = enumNodeType::booleanSub;
	else
		return false;
	return true;
}

bool IsGroup(enumNodeType type)
{
	return type == enumNodeType::hybrid || type == enumNodeType::booleanAdd
				 || type == enumNodeType::booleanMul || type == enumNodeType::booleanSub;
}

QList<cObjectsTree::sNodeListItem> Nodes()
{
	cObjectsTree tree;
	return tree.GetListOfNodes(gPar);
}

cObjectsTree::sNodeListItem *FindNode(QList<cObjectsTree::sNodeListItem> *nodes, int id)
{
	for (cObjectsTree::sNodeListItem &node : *nodes)
	{
		if (node.id == id) return &node;
	}
	return nullptr;
}

void WriteDefinition(int nodeId, const QString &name, enumNodeType type, int parentId, int objectId,
	int displayOrder)
{
	const QString prefix = NodePrefix(nodeId);
	if (!gPar->IfExists(prefix + "definition")) InitNodeParams(nodeId, gPar);
	QString safeName = name;
	safeName.replace(',', ' ');
	gPar->Set(prefix + "definition", QString("%1,%2,%3,%4,%5,%6")
																			.arg(safeName)
																			.arg(nodeId)
																			.arg(int(type))
																			.arg(parentId)
																			.arg(objectId)
																			.arg(displayOrder));
}

int NextNodeId()
{
	int next = 1;
	for (const cObjectsTree::sNodeListItem &node : Nodes())
		next = qMax(next, node.id + 1);
	return next;
}

int NextObjectId(int start)
{
	QSet<int> used;
	for (const cObjectsTree::sNodeListItem &node : Nodes())
		if (node.objectId > 0) used.insert(node.objectId);
	int id = start;
	while (used.contains(id)) ++id;
	return id;
}

bool ParseVec3(const QJsonValue &value, CVector3 *out, QString *error)
{
	if (!value.isArray() || value.toArray().size() != 3)
	{
		*error = "Expected an array of 3 JSON numbers, such as [1, 2, 3].";
		return false;
	}
	const QJsonArray array = value.toArray();
	double component[3];
	for (int i = 0; i < 3; i++)
	{
		if (!array.at(i).isDouble())
		{
			*error = "Vector components must be JSON numbers, not strings.";
			return false;
		}
		component[i] = array.at(i).toDouble();
	}
	*out = CVector3(component[0], component[1], component[2]);
	return true;
}

bool ParseColor(const QJsonValue &value, sRGB *out, QString *error)
{
	if (value.isString())
	{
		QString text = value.toString().trimmed();
		if (text.startsWith('#')) text = text.mid(1);
		if (text.size() != 6)
		{
			*error = "Color strings look like #rrggbb.";
			return false;
		}
		bool ok = false;
		const int rgb = text.toInt(&ok, 16);
		if (!ok)
		{
			*error = "Could not parse that color.";
			return false;
		}
		*out = sRGB(((rgb >> 16) & 255) * 257, ((rgb >> 8) & 255) * 257, (rgb & 255) * 257);
		return true;
	}
	CVector3 channels;
	if (!ParseVec3(value, &channels, error)) return false;
	const bool eightBit = channels.x <= 255 && channels.y <= 255 && channels.z <= 255;
	auto channel = [&](double component) -> int {
		const double scaled = eightBit ? component * 257.0 : component;
		return qBound(0, int(qRound(scaled)), 65535);
	};
	*out = sRGB(channel(channels.x), channel(channels.y), channel(channels.z));
	return true;
}

bool FindFormula(const QString &requested, int *id, QString *internal, QString *display)
{
	for (cAbstractFractal *formula : newFractalList)
	{
		const QString internalName = QString::fromStdString(formula->getInternalName());
		const QString displayName = QString::fromStdString(formula->getNameInComboBox());
		if (internalName.compare(requested, Qt::CaseInsensitive) == 0
			|| displayName.compare(requested, Qt::CaseInsensitive) == 0)
		{
			*id = int(formula->getInternalId());
			*internal = internalName;
			*display = displayName;
			return true;
		}
	}
	return false;
}

void RefreshGui()
{
	if (systemData.noGui || !gMainInterface || !gMainInterface->mainWindow) return;
	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);
	if (cObjectsTreeWidget *tree = gMainInterface->mainWindow->findChild<cObjectsTreeWidget *>())
		tree->UpdateTree(gPar, gParFractal);
	if (gKeyframeAnimation) gKeyframeAnimation->RefreshTable();
	if (gFlightAnimation) gFlightAnimation->RefreshTable();
}

void Remember()
{
	if (!systemData.noGui && gMainInterface && gMainInterface->mainWindow)
		gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);
	if (gUndo)
	{
		gUndo->Store(gPar, gParFractal, gAnimFrames, gKeyframes);
		gUndo->CommitNow();
	}
	RefreshGui();
}

QJsonObject NodeJson(const cObjectsTree::sNodeListItem &node)
{
	QJsonObject object;
	object["id"] = node.id;
	object["name"] = node.name;
	object["type"] = TypeName(node.type);
	object["parent_id"] = node.parentId;
	object["object_id"] = node.objectId;
	object["level"] = node.level;
	const QString prefix = NodePrefix(node.id);
	if (gPar->IfExists(prefix + "position"))
	{
		const CVector3 position = gPar->Get<CVector3>(prefix + "position");
		object["position"] = QJsonArray{position.x, position.y, position.z};
	}
	if (gPar->IfExists(prefix + "scale")) object["scale"] = gPar->Get<double>(prefix + "scale");
	if (gPar->IfExists(prefix + "material")) object["material"] = gPar->Get<int>(prefix + "material");
	if (gPar->IfExists(prefix + "enabled")) object["enabled"] = gPar->Get<bool>(prefix + "enabled");
	if (node.type == enumNodeType::fractal && node.objectId >= 1
		&& node.objectId <= gParFractal->size())
	{
		const int formulaId = gParFractal->at(node.objectId - 1)->Get<int>("formula");
		for (cAbstractFractal *formula : newFractalList)
		{
			if (int(formula->getInternalId()) == formulaId)
			{
				object["formula"] = QString::fromStdString(formula->getInternalName());
				object["formula_iterations"] =
					gParFractal->at(node.objectId - 1)->Get<int>("formula_iterations");
				break;
			}
		}
	}
	return object;
}

QJsonObject TreeResult()
{
	QJsonArray nodes;
	for (const cObjectsTree::sNodeListItem &node : Nodes())
		nodes.append(NodeJson(node));
	QJsonObject result;
	result["nodes"] = nodes;
	result["how_to_combine"] =
		"The picture is this tree, not the nine slots by themselves. A fractal node uses the formula "
		"in its object_id slot. A hybrid node's fractal children are run as one stack, in sibling "
		"order: formula_iterations on each slot is how many times that formula repeats before the "
		"next child. boolean_add unions children, boolean_mul keeps only the overlap, boolean_sub "
		"carves later children out of the first. Primitives (sphere, box, plane, torus, ...) can sit "
		"under a boolean. A primitive cannot be a child of a hybrid. parent_id 0 means the world.";
	return result;
}

void EnsureMaterial(int id)
{
	const QString flag = QString("mat%1_is_defined").arg(id);
	if (!gPar->IfExists(flag) || !gPar->Get<bool>(flag))
	{
		InitMaterialParams(id, gPar);
		gPar->Set(flag, true);
	}
}

void EnsureLight(int id)
{
	const QString flag = QString("light%1_is_defined").arg(id);
	if (!gPar->IfExists(flag))
	{
		InitLightParams(id, gPar);
		gPar->Set(flag, true);
		gPar->Set(QString("light%1_enabled").arg(id), true);
	}
}

void EnsureAnimated(cAnimationFrames *frames, const QStringList &fullNames)
{
	for (const QString &name : fullNames)
		frames->AddAnimatedParameter(name, gPar, gParFractal);
}

QJsonObject KeyframeJson(int index)
{
	const cAnimationFrames::sAnimationFrame frame = gKeyframes->GetFrame(index);
	QJsonObject object;
	object["index"] = index;
	object["frames_per_keyframe"] = frame.numberOfSubFrames;
	auto putVec = [&](const char *key, const QString &param) {
		if (!frame.parameters.IfExists(param)) return;
		const CVector3 value = frame.parameters.Get<CVector3>(param);
		object[key] = QJsonArray{value.x, value.y, value.z};
	};
	putVec("camera", "main_camera");
	putVec("target", "main_target");
	if (frame.parameters.IfExists("main_fov")) object["fov"] = frame.parameters.Get<double>("main_fov");
	return object;
}

QJsonObject AnimationState()
{
	gKeyframes->UpdateFramesIndexesTable();
	QJsonArray keys;
	for (int i = 0; i < gKeyframes->GetNumberOfFrames(); i++)
		keys.append(KeyframeJson(i));
	QJsonObject result;
	result["keyframes"] = keys;
	result["keyframe_count"] = gKeyframes->GetNumberOfFrames();
	result["interpolated_frames"] = gKeyframes->GetTotalNumberOfFrames();
	result["flight_frames"] = gAnimFrames->GetNumberOfFrames();
	result["interpolation"] =
		"Camera, target, and field of view use the parameter's morph (Akima for the camera). "
		"frames_per_keyframe is how many pictures are produced from that key toward the next. The "
		"last key contributes one frame. Flight frames are already one picture each; they are not "
		"interpolated.";
	return result;
}

QString SaveColorPng(const std::shared_ptr<cImage> &image, const QString &basePath)
{
	ImageFileSave::structSaveImageChannel channel(
		ImageFileSave::IMAGE_CONTENT_COLOR, ImageFileSave::IMAGE_CHANNEL_QUALITY_8, QString());
	ImageFileSave::ImageConfig imageConfig;
	imageConfig.insert(ImageFileSave::IMAGE_CONTENT_COLOR, channel);
	ImageFileSavePNG saver(basePath, image, imageConfig);
	const QStringList saved = saver.SaveImage();
	return saved.isEmpty() ? QString() : saved.first();
}

void ApplyDraftLimits(const std::shared_ptr<cParameterContainer> &par, double detailCap)
{
	if (detailCap > 0 && par->Get<double>("detail_level") > detailCap)
		par->Set("detail_level", detailCap);
	const QStringList disable = {"ambient_occlusion_enabled", "DOF_enabled", "volumetric_fog_enabled",
		"raytraced_reflections", "DOF_MC_global_illumination"};
	for (const QString &name : disable)
	{
		if (par->IfExists(name)) par->Set(name, false);
	}
}

void EnsureCopyMaterial(const std::shared_ptr<cParameterContainer> &par)
{
	if (!par->IfExists("mat1_is_defined") || !par->Get<bool>("mat1_is_defined"))
	{
		InitMaterialParams(1, par);
		par->Set("mat1_is_defined", true);
	}
}

bool PrepareCopy(const QJsonObject &params, std::shared_ptr<sJob> job, QString *error)
{
	job->par = std::make_shared<cParameterContainer>(*gPar);
	job->fractal = std::make_shared<cFractalContainer>(*gParFractal);
	const QString quality = params.value("quality").toString("full");
	if (quality == "draft")
	{
		job->draft = true;
		job->detailCap = 0.5;
	}
	else if (quality == "better")
	{
		job->draft = true;
		job->detailCap = 1.0;
	}
	else if (quality == "full")
	{
		job->draft = false;
		job->detailCap = 0;
	}
	else
	{
		*error = "quality must be draft, better, or full.";
		return false;
	}
	int width = params.value("width").toInt(job->par->Get<int>("image_width"));
	int height = params.value("height").toInt(job->par->Get<int>("image_height"));
	if (params.contains("long_side"))
	{
		const int longSide = qBound(32, params.value("long_side").toInt(320), job->draft ? 640 : 4096);
		const double aspect = width > 0 && height > 0 ? double(width) / double(height) : 16.0 / 9.0;
		if (width >= height)
		{
			width = longSide;
			height = qMax(16, int(qRound(longSide / aspect)));
		}
		else
		{
			height = longSide;
			width = qMax(16, int(qRound(longSide * aspect)));
		}
	}
	width = qBound(16, width, 4096);
	height = qBound(16, height, 4096);
	job->width = width;
	job->height = height;
	job->par->Set("image_width", width);
	job->par->Set("image_height", height);
	if (job->draft) ApplyDraftLimits(job->par, job->detailCap);
	EnsureCopyMaterial(job->par);
	Q_UNUSED(error);
	return true;
}

bool RenderOne(const std::shared_ptr<sJob> &job, const QString &basePath, QString *saved, QString *error)
{
	job->par->Set("image_width", job->width);
	job->par->Set("image_height", job->height);
	if (job->draft) ApplyDraftLimits(job->par, job->detailCap);
	auto image = std::make_shared<cImage>(job->width, job->height);
	cRenderJob renderJob(job->par, job->fractal, image, 1, &job->stop);
	renderJob.PreserveStopRequest();
	cRenderingConfiguration config;
	config.DisableRefresh();
	config.DisableProgressiveRender();
	config.DisableNetRender();
	if (gMcpServer)
	{
		QObject::connect(
			&renderJob, &cRenderJob::updateProgressAndStatus, gMcpServer,
			[job](const QString &, const QString &progressText, double progress) {
				QMutexLocker locker(&job->mutex);
				job->message = progressText;
				if (job->frameCount > 1)
					job->progress = (double(job->frame) + progress) / double(job->frameCount);
				else
					job->progress = progress;
			},
			Qt::QueuedConnection);
	}
	if (!renderJob.Init(cRenderJob::still, config))
	{
		*error = QString("The render did not start.");
		return false;
	}
	// After Init, before Execute clears the flag. A real stop_render that won
	// this race is already true and is kept by PreserveStopRequest.
	if (job->stopBeforeExecute) job->stop = true;
	if (!renderJob.Execute())
	{
		*error = job->stop ? QString("The render was stopped.") : QString("The render did not finish.");
		return false;
	}
	*saved = SaveColorPng(image, basePath);
	if (saved->isEmpty())
	{
		*error = "The frame rendered but the PNG could not be written.";
		return false;
	}
	return true;
}

struct sSheet
{
	QString path;
	int width = 0;
	int height = 0;
	int count = 0;
};

// Tiles finished frames into one PNG. No text, so a headless QCoreApplication
// does not need a font. A short white mark keeps the grid readable.
sSheet WriteContactSheet(const QStringList &paths, const QString &base, int frameEdge, int columns)
{
	sSheet sheet;
	frameEdge = qBound(16, frameEdge, 320);
	QVector<QImage> thumbs;
	for (const QString &path : paths)
	{
		QImage image(path);
		if (image.isNull()) continue;
		image = image.convertToFormat(QImage::Format_RGB32)
							 .scaled(frameEdge, frameEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		thumbs.append(image);
	}
	sheet.count = thumbs.size();
	if (thumbs.isEmpty()) return sheet;
	const int cols = columns > 0 ? qBound(1, columns, thumbs.size()) : qMin(thumbs.size(), 6);
	const int rows = (thumbs.size() + cols - 1) / cols;
	const int gap = 4;
	sheet.width = cols * frameEdge + (cols + 1) * gap;
	sheet.height = rows * frameEdge + (rows + 1) * gap;
	QImage canvas(sheet.width, sheet.height, QImage::Format_RGB32);
	canvas.fill(qRgb(24, 24, 28));
	for (int i = 0; i < thumbs.size(); i++)
	{
		const QImage &thumb = thumbs.at(i);
		const int col = i % cols;
		const int row = i / cols;
		const int ox = gap + col * (frameEdge + gap) + (frameEdge - thumb.width()) / 2;
		const int oy = gap + row * (frameEdge + gap) + (frameEdge - thumb.height()) / 2;
		for (int y = 0; y < thumb.height(); y++)
		{
			const QRgb *src = reinterpret_cast<const QRgb *>(thumb.constScanLine(y));
			QRgb *dst = reinterpret_cast<QRgb *>(canvas.scanLine(oy + y));
			memcpy(dst + ox, src, size_t(thumb.width()) * sizeof(QRgb));
		}
		for (int y = 0; y < 3; y++)
		{
			QRgb *dst = reinterpret_cast<QRgb *>(canvas.scanLine(oy + y));
			const int mark = qMin(8, thumb.width());
			for (int x = 0; x < mark; x++)
				dst[ox + x] = qRgb(255, 255, 255);
		}
	}
	sheet.path = base + ".png";
	if (!canvas.save(sheet.path, "PNG")) sheet.path.clear();
	return sheet;
}

void FinishJob(const std::shared_ptr<sJob> &job, const QString &status, const QString &error)
{
	QMutexLocker locker(&job->mutex);
	job->status = status;
	job->error = error;
	if (status == "finished") job->progress = 1;
	if (!job->imagePath.isEmpty()) SetLastImage(job->imagePath);
	job->done.store(true);
}

void RunStill(const std::shared_ptr<sJob> job)
{
	QString saved;
	QString error;
	const QString base = job->outputDirectory;
	if (!RenderOne(job, base, &saved, &error))
		FinishJob(job, job->stop ? "stopped" : "failed", error);
	else
	{
		QMutexLocker locker(&job->mutex);
		job->imagePath = saved;
		job->frames.append(saved);
		locker.unlock();
		FinishJob(job, "finished", QString());
	}
}

void RunAnimation(const std::shared_ptr<sJob> job, const QString &mode, int start, int end)
{
	QDir().mkpath(job->outputDirectory);
	for (int frame = start; frame < end; frame++)
	{
		if (job->stop) break;
		{
			QMutexLocker locker(&job->mutex);
			job->frame = frame - start;
			job->message = QString("Frame %1 of %2").arg(frame - start + 1).arg(end - start);
		}
		if (mode == "keyframe")
			job->keyframes->GetInterpolatedFrameAndConsolidate(frame, job->par, job->fractal);
		else
			job->flight->GetFrameAndConsolidate(frame, job->par, job->fractal);
		const QString base =
			QDir(job->outputDirectory).filePath(QString("frame_%1").arg(frame, 4, 10, QChar('0')));
		QString saved;
		QString error;
		if (!RenderOne(job, base, &saved, &error))
		{
			FinishJob(job, job->stop ? "stopped" : "failed", error);
			return;
		}
		QMutexLocker locker(&job->mutex);
		job->frames.append(saved);
		job->imagePath = saved;
		locker.unlock();
		SetLastImage(saved);
	}
	if (job->wantContactSheet)
	{
		QStringList frames;
		{
			QMutexLocker locker(&job->mutex);
			frames = job->frames;
		}
		if (!frames.isEmpty())
		{
			const QString base = QDir(job->outputDirectory).filePath("contact_sheet");
			const sSheet sheet = WriteContactSheet(frames, base, job->frameEdge, job->columns);
			if (!sheet.path.isEmpty())
			{
				QMutexLocker locker(&job->mutex);
				job->contactSheetPath = sheet.path;
			}
		}
	}
	FinishJob(job, job->stop ? "stopped" : "finished", QString());
}

void Launch(const std::shared_ptr<sJob> &job, const QString &kind,
	const std::function<void(std::shared_ptr<sJob>)> &body)
{
	job->id = QString("job-%1").arg(++gJobSerial);
	job->kind = kind;
	job->status = "running";
	gJobs.insert(job->id, job);
	gActiveJobId = job->id;
	QThread *thread = QThread::create([job, body]() { body(job); });
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start();
}

QJsonObject JobStatus(const std::shared_ptr<sJob> &job)
{
	QMutexLocker locker(&job->mutex);
	QJsonObject result;
	result["id"] = job->id;
	result["kind"] = job->kind;
	result["status"] = job->status;
	result["progress"] = job->progress;
	result["frame"] = job->frame;
	result["frame_count"] = job->frameCount;
	result["message"] = job->message;
	if (!job->error.isEmpty()) result["error"] = job->error;
	if (!job->outputDirectory.isEmpty()) result["output"] = job->outputDirectory;
	if (!job->imagePath.isEmpty()) result["image"] = job->imagePath;
	if (!job->contactSheetPath.isEmpty()) result["contact_sheet"] = job->contactSheetPath;
	QJsonArray frames;
	for (const QString &path : job->frames)
		frames.append(path);
	result["frames"] = frames;
	result["done"] = job->done.load();
	return result;
}

QJsonObject WaitFor(const std::shared_ptr<sJob> &job)
{
	QEventLoop loop;
	QTimer timer;
	timer.setInterval(40);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
		if (job->done.load()) loop.quit();
	});
	timer.start();
	loop.exec();
	QJsonObject result = JobStatus(job);
	const QString status = result.value("status").toString();
	if (status == "failed" || status == "stopped")
	{
		result["ok"] = false;
		if (result.value("error").toString().isEmpty())
			result["error"] = status == "stopped" ? QString("The render was stopped.")
																						 : QString("The render did not finish.");
	}
	return result;
}

QString StripPng(QString path)
{
	if (path.endsWith(".png", Qt::CaseInsensitive)) path.chop(4);
	return path;
}

QJsonObject ObjectsTree()
{
	return TreeResult();
}

QJsonObject AddObject(const QJsonObject &params)
{
	enumNodeType type;
	if (!TypeFromName(params.value("type").toString(), &type))
	{
		return Fail(
			"type must be fractal, hybrid, boolean_add, boolean_mul, boolean_sub, or primitive.");
	}
	const int parentId = params.value("parent_id").toInt(0);
	QList<cObjectsTree::sNodeListItem> nodes = Nodes();
	const cObjectsTree::sNodeListItem *parent = nullptr;
	if (parentId != 0)
	{
		parent = FindNode(&nodes, parentId);
		if (!parent) return Fail(QString("No node with id %1.").arg(parentId));
		if (!IsGroup(parent->type))
			return Fail("parent_id must be 0 or a hybrid or boolean node.");
		if (parent->type == enumNodeType::hybrid && type == enumNodeType::primitive)
			return Fail("A primitive cannot be inside a hybrid. Put it under a boolean or at the world.");
	}

	const int nodeId = NextNodeId();
	int objectId = 0;
	QString name = params.value("name").toString();
	if (type == enumNodeType::fractal)
	{
		objectId = NextObjectId(1);
		if (objectId > gParFractal->size())
			return Fail("All 9 fractal slots are already used by the tree.");
		int formulaId = 0;
		QString internal;
		QString display;
		const QString requested = params.value("formula").toString();
		if (!requested.isEmpty())
		{
			if (!FindFormula(requested, &formulaId, &internal, &display))
				return Fail(QString("No formula named '%1'.").arg(requested));
			gParFractal->at(objectId - 1)->Set("formula", formulaId);
		}
		else
		{
			formulaId = gParFractal->at(objectId - 1)->Get<int>("formula");
			if (formulaId == int(fractal::none))
			{
				FindFormula("mandelbulb", &formulaId, &internal, &display);
				gParFractal->at(objectId - 1)->Set("formula", formulaId);
			}
			else
			{
				for (cAbstractFractal *formula : newFractalList)
				{
					if (int(formula->getInternalId()) == formulaId)
					{
						internal = QString::fromStdString(formula->getInternalName());
						break;
					}
				}
			}
		}
		if (params.contains("formula_iterations"))
			gParFractal->at(objectId - 1)
				->Set("formula_iterations", qBound(1, params.value("formula_iterations").toInt(1), 250));
		if (name.isEmpty()) name = internal.isEmpty() ? QString("fractal %1").arg(objectId) : internal;
	}
	else if (type == enumNodeType::primitive)
	{
		const QString primitiveType = params.value("primitive").toString("sphere");
		if (cPrimitives::PrimitiveNameToEnum(primitiveType) == fractal::objNone)
			return Fail("primitive must be one of: plane, sphere, box, torus, cylinder, cone, ellipsoid, "
									"prism, rectangle, circle, water.");
		const QList<sPrimitiveItem> existing = cPrimitives::GetListOfPrimitives(gPar);
		const int primitiveIndex = cPrimitives::NewPrimitiveIndex(primitiveType, existing);
		const QString fullName = QString("primitive_%1_%2").arg(primitiveType).arg(primitiveIndex);
		sPrimitiveItem primitive(cPrimitives::PrimitiveNameToEnum(primitiveType), primitiveIndex, fullName,
			primitiveType);
		InitPrimitiveParams(primitive, gPar);
		gPar->Set(primitive.Name("enabled"), true);
		objectId = NextObjectId(1000);
		gPar->Set(primitive.Name("object_id"), objectId);
		if (name.isEmpty()) name = gPar->Get<QString>(primitive.Name("name"));
		if (params.contains("radius") && gPar->IfExists(primitive.Name("radius")))
			gPar->Set(primitive.Name("radius"), params.value("radius").toDouble());
	}
	else
	{
		objectId = NextObjectId(100);
		if (name.isEmpty()) name = TypeName(type);
	}

	WriteDefinition(nodeId, name, type, parentId, objectId, nodeId);
	const QString prefix = NodePrefix(nodeId);
	if (params.contains("position"))
	{
		CVector3 position;
		QString error;
		if (!ParseVec3(params.value("position"), &position, &error)) return Fail(error);
		gPar->Set(prefix + "position", position);
	}
	if (params.contains("scale")) gPar->Set(prefix + "scale", params.value("scale").toDouble(1));
	Remember();
	QJsonObject result = TreeResult();
	result["added"] = nodeId;
	result["object_id"] = objectId;
	return result;
}

QJsonObject UpdateObject(const QJsonObject &params)
{
	const int nodeId = params.value("id").toInt(params.value("node_id").toInt(0));
	QList<cObjectsTree::sNodeListItem> nodes = Nodes();
	cObjectsTree::sNodeListItem *node = FindNode(&nodes, nodeId);
	if (!node) return Fail("Pass id of a node from get_objects_tree.");
	const QString prefix = NodePrefix(nodeId);
	if (params.contains("position"))
	{
		CVector3 position;
		QString error;
		if (!ParseVec3(params.value("position"), &position, &error)) return Fail(error);
		gPar->Set(prefix + "position", position);
	}
	if (params.contains("rotation"))
	{
		CVector3 rotation;
		QString error;
		if (!ParseVec3(params.value("rotation"), &rotation, &error)) return Fail(error);
		gPar->Set(prefix + "rotation", rotation);
	}
	if (params.contains("scale")) gPar->Set(prefix + "scale", params.value("scale").toDouble());
	if (params.contains("material")) gPar->Set(prefix + "material", params.value("material").toInt());
	if (params.contains("enabled")) gPar->Set(prefix + "enabled", params.value("enabled").toBool());
	int parentId = node->parentId;
	QString name = node->name;
	if (params.contains("parent_id"))
	{
		parentId = params.value("parent_id").toInt();
		if (parentId == nodeId) return Fail("A node cannot be its own parent.");
		if (parentId != 0)
		{
			const cObjectsTree::sNodeListItem *parent = FindNode(&nodes, parentId);
			if (!parent) return Fail(QString("No node with id %1.").arg(parentId));
			if (!IsGroup(parent->type))
				return Fail("parent_id must be 0 or a hybrid or boolean node.");
			if (parent->type == enumNodeType::hybrid && node->type == enumNodeType::primitive)
				return Fail("A primitive cannot be inside a hybrid.");
		}
	}
	if (params.contains("name")) name = params.value("name").toString();
	if (params.contains("parent_id") || params.contains("name"))
		WriteDefinition(nodeId, name, node->type, parentId, node->objectId, node->displayOrder);
	Remember();
	return TreeResult();
}

QJsonObject DeleteObject(const QJsonObject &params)
{
	const int nodeId = params.value("id").toInt(params.value("node_id").toInt(0));
	QList<cObjectsTree::sNodeListItem> nodes = Nodes();
	cObjectsTree::sNodeListItem *node = FindNode(&nodes, nodeId);
	if (!node) return Fail("Pass id of a node from get_objects_tree.");
	const int parentId = node->parentId;
	const enumNodeType type = node->type;
	const int objectId = node->objectId;
	for (const cObjectsTree::sNodeListItem &child : nodes)
	{
		if (child.parentId != nodeId) continue;
		WriteDefinition(child.id, child.name, child.type, parentId, child.objectId, child.displayOrder);
	}
	const QString prefix = NodePrefix(nodeId);
	const QStringList names = gPar->GetListOfParameters();
	for (const QString &name : names)
	{
		if (name.startsWith(prefix)) gPar->DeleteParameter(name);
	}
	if (type == enumNodeType::primitive)
	{
		const QList<sPrimitiveItem> primitives = cPrimitives::GetListOfPrimitives(gPar);
		for (const sPrimitiveItem &primitive : primitives)
		{
			if (!gPar->IfExists(primitive.Name("object_id"))) continue;
			if (gPar->Get<int>(primitive.Name("object_id")) != objectId) continue;
			const QList<QString> primitiveNames = cPrimitives::GetListOfPrimitiveParams(primitive, gPar);
			for (const QString &primitiveName : primitiveNames)
				gPar->DeleteParameter(primitiveName);
		}
	}
	Remember();
	QJsonObject result = TreeResult();
	result["deleted"] = nodeId;
	result["note"] = "Children were reattached to the deleted node's parent.";
	return result;
}

QJsonObject SetMaterial(const QJsonObject &params)
{
	const int id = params.value("id").toInt(1);
	if (id < 1 || id > 99) return Fail("Material id must be 1 to 99.");
	EnsureMaterial(id);
	const QString prefix = QString("mat%1_").arg(id);
	if (params.contains("surface_color"))
	{
		sRGB color;
		QString error;
		if (!ParseColor(params.value("surface_color"), &color, &error)) return Fail(error);
		gPar->Set(prefix + "surface_color", color);
	}
	if (params.contains("specular") && gPar->IfExists(prefix + "specular"))
		gPar->Set(prefix + "specular", params.value("specular").toDouble());
	if (params.contains("specular_width") && gPar->IfExists(prefix + "specular_width"))
		gPar->Set(prefix + "specular_width", params.value("specular_width").toDouble());
	if (params.contains("reflectance") && gPar->IfExists(prefix + "reflectance"))
		gPar->Set(prefix + "reflectance", params.value("reflectance").toDouble());
	if (params.contains("metallic") && gPar->IfExists(prefix + "metallic"))
		gPar->Set(prefix + "metallic", params.value("metallic").toBool());
	if (params.contains("use_colors_from_palette") && gPar->IfExists(prefix + "use_colors_from_palette"))
		gPar->Set(prefix + "use_colors_from_palette", params.value("use_colors_from_palette").toBool());
	if (params.contains("surface_color_gradient") && gPar->IfExists(prefix + "surface_color_gradient"))
		gPar->Set(prefix + "surface_color_gradient", params.value("surface_color_gradient").toString());
	if (params.contains("name") && gPar->IfExists(prefix + "name"))
		gPar->Set(prefix + "name", params.value("name").toString());
	Remember();
	QJsonObject result;
	result["material"] = id;
	result["defined"] = true;
	result["note"] =
		"Material 1 is the default surface. surface_color is the flat color. The gradient is the "
		"orbit palette used when use_colors_from_palette is on. Iteration fog is separate and is "
		"often the color you actually see; set iteration_fog_color_1 and iteration_fog_enable.";
	return result;
}

QJsonObject SetLight(const QJsonObject &params)
{
	const int id = params.value("id").toInt(1);
	if (id < 1 || id > 16) return Fail("Light id must be 1 to 16. Light 1 is the key light.");
	EnsureLight(id);
	const QString prefix = QString("light%1_").arg(id);
	if (params.contains("enabled") && gPar->IfExists(prefix + "enabled"))
		gPar->Set(prefix + "enabled", params.value("enabled").toBool());
	if (params.contains("intensity") && gPar->IfExists(prefix + "intensity"))
		gPar->Set(prefix + "intensity", params.value("intensity").toDouble());
	if (params.contains("position"))
	{
		CVector3 position;
		QString error;
		if (!ParseVec3(params.value("position"), &position, &error)) return Fail(error);
		if (gPar->IfExists(prefix + "position")) gPar->Set(prefix + "position", position);
		if (gPar->IfExists(prefix + "relative_position")) gPar->Set(prefix + "relative_position", false);
	}
	if (params.contains("color"))
	{
		sRGB color;
		QString error;
		if (!ParseColor(params.value("color"), &color, &error)) return Fail(error);
		if (gPar->IfExists(prefix + "color")) gPar->Set(prefix + "color", color);
	}
	Remember();
	QJsonObject result;
	result["light"] = id;
	result["note"] =
		"Light 1 is the key light and starts tied to the camera. Passing position detaches it. "
		"Raise intensity before brightness if the shape is only a silhouette. Colors are #rrggbb "
		"or [r, g, b].";
	return result;
}

QJsonObject SetCamera(const QJsonObject &params)
{
	QString error;
	if (params.contains("camera"))
	{
		CVector3 camera;
		if (!ParseVec3(params.value("camera"), &camera, &error)) return Fail(error);
		gPar->Set("camera", camera);
	}
	if (params.contains("target"))
	{
		CVector3 target;
		if (!ParseVec3(params.value("target"), &target, &error)) return Fail(error);
		gPar->Set("target", target);
	}
	if (params.contains("rotation"))
	{
		CVector3 rotation;
		if (!ParseVec3(params.value("rotation"), &rotation, &error)) return Fail(error);
		gPar->Set("camera_rotation", rotation);
	}
	if (params.contains("fov")) gPar->Set("fov", params.value("fov").toDouble());
	if (params.contains("distance"))
		gPar->Set("camera_distance_to_target", params.value("distance").toDouble());
	Remember();
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	QJsonObject result;
	result["camera"] = QJsonArray{camera.x, camera.y, camera.z};
	result["target"] = QJsonArray{target.x, target.y, target.z};
	result["fov"] = gPar->Get<double>("fov");
	result["distance"] = gPar->Get<double>("camera_distance_to_target");
	return result;
}

QJsonObject AddKeyframe(const QJsonObject &params)
{
	const int between = params.value("frames_per_keyframe").toInt(4);
	if (between < 1 || between > 10000) return Fail("frames_per_keyframe must be 1 to 10000.");
	EnsureAnimated(gKeyframes.get(),
		{"main_camera", "main_target", "main_camera_rotation", "main_fov", "main_camera_distance_to_target"});
	int index = params.value("index").toInt(-1);
	if (index < 0) index = gKeyframes->GetNumberOfFrames();
	if (index > gKeyframes->GetNumberOfFrames())
		return Fail("index is past the end of the key list.");
	gKeyframes->AddFrame(gPar, gParFractal, between, index);
	gKeyframes->UpdateFramesIndexesTable();
	Remember();
	QJsonObject result = AnimationState();
	result["added"] = index;
	result["note"] =
		"This key stores the camera, target, rotation, fov, and distance as they are right now. "
		"Move the camera, then add another key. frames_per_keyframe defaults to 4 here (the app "
		"default of 100 is a long final, not a sketch). The last key contributes one frame.";
	return result;
}

QJsonObject UpdateKeyframe(const QJsonObject &params)
{
	const int index = params.value("index").toInt(-1);
	if (index < 0 || index >= gKeyframes->GetNumberOfFrames())
		return Fail("index must be an existing key.");
	cAnimationFrames::sAnimationFrame frame = gKeyframes->GetFrame(index);
	if (params.contains("frames_per_keyframe"))
		frame.numberOfSubFrames = params.value("frames_per_keyframe").toInt(frame.numberOfSubFrames);
	const QStringList names = {"camera", "target", "camera_rotation", "fov", "camera_distance_to_target"};
	for (const QString &name : names)
	{
		const QString full = "main_" + name;
		if (!frame.parameters.IfExists(full) || !gPar->IfExists(name)) continue;
		cOneParameter value = gPar->GetAsOneParameter(name);
		value.SetMorphType(frame.parameters.GetAsOneParameter(full).GetMorphType());
		frame.parameters.SetFromOneParameter(full, value);
	}
	gKeyframes->ModifyFrame(index, frame);
	gKeyframes->UpdateFramesIndexesTable();
	Remember();
	return AnimationState();
}

QJsonObject DeleteKeyframe(const QJsonObject &params)
{
	const int index = params.value("index").toInt(-1);
	if (index < 0 || index >= gKeyframes->GetNumberOfFrames())
		return Fail("index must be an existing key.");
	gKeyframes->DeleteFrames(index, index);
	gKeyframes->UpdateFramesIndexesTable();
	Remember();
	return AnimationState();
}

QJsonObject SetFlightPath(const QJsonObject &params)
{
	QString kind = params.value("kind").toString();
	if (kind.isEmpty()) kind = params.value("frames").isArray() ? "frames" : "orbit";
	const bool explicitFrames = params.value("frames").isArray();
	int count = params.value("count").toInt(8);
	if (!explicitFrames && params.value("frames").isDouble()) count = params.value("frames").toInt(count);
	count = qBound(2, count, 360);

	CVector3 orbitTarget = gPar->Get<CVector3>("target");
	CVector3 flyStart = gPar->Get<CVector3>("camera");
	CVector3 flyEnd = flyStart + CVector3(0, 2, 0);
	CVector3 flyTarget = orbitTarget;
	QString error;
	if (params.contains("target") && !ParseVec3(params.value("target"), &orbitTarget, &error))
		return Fail(error);
	flyTarget = orbitTarget;
	if (params.contains("start") && !ParseVec3(params.value("start"), &flyStart, &error)) return Fail(error);
	if (params.contains("end") && !ParseVec3(params.value("end"), &flyEnd, &error)) return Fail(error);
	struct sShot
	{
		CVector3 camera;
		CVector3 target;
		double fov = 0;
	};
	QVector<sShot> shots;
	if (kind == "frames")
	{
		const QJsonArray frames = params.value("frames").toArray();
		if (frames.size() < 2) return Fail("frames needs at least two {camera, target} objects.");
		for (const QJsonValue &item : frames)
		{
			const QJsonObject frame = item.toObject();
			sShot shot;
			shot.camera = gPar->Get<CVector3>("camera");
			shot.target = gPar->Get<CVector3>("target");
			shot.fov = frame.value("fov").toDouble(gPar->Get<double>("fov"));
			if (frame.contains("camera") && !ParseVec3(frame.value("camera"), &shot.camera, &error))
				return Fail(error);
			if (frame.contains("target") && !ParseVec3(frame.value("target"), &shot.target, &error))
				return Fail(error);
			shots.append(shot);
		}
	}
	else if (kind != "orbit" && kind != "flythrough" && kind != "fly")
	{
		return Fail("kind must be orbit, flythrough, or frames.");
	}

	gAnimFrames->ClearAll();
	EnsureAnimated(gAnimFrames.get(),
		{"main_camera", "main_target", "main_camera_rotation", "main_fov", "main_camera_distance_to_target"});

	auto capture = [&](const CVector3 &camera, const CVector3 &target, double fov) {
		auto par = std::make_shared<cParameterContainer>(*gPar);
		auto fractal = std::make_shared<cFractalContainer>(*gParFractal);
		par->Set("camera", camera);
		par->Set("target", target);
		par->Set("fov", fov);
		const double distance = (camera - target).Length();
		par->Set("camera_distance_to_target", distance);
		gAnimFrames->AddFrame(par, fractal, 1);
	};

	if (kind == "orbit")
	{
		const double radius =
			params.value("radius").toDouble(gPar->Get<double>("camera_distance_to_target"));
		const double height = params.value("height").toDouble(gPar->Get<CVector3>("camera").z);
		const double fov = params.value("fov").toDouble(gPar->Get<double>("fov"));
		for (int i = 0; i < count; i++)
		{
			const double angle = 2.0 * M_PI * double(i) / double(count);
			const CVector3 camera(orbitTarget.x + radius * std::sin(angle),
				orbitTarget.y - radius * std::cos(angle), orbitTarget.z + height);
			capture(camera, orbitTarget, fov);
		}
	}
	else if (kind == "flythrough" || kind == "fly")
	{
		const double fov = params.value("fov").toDouble(gPar->Get<double>("fov"));
		for (int i = 0; i < count; i++)
		{
			const double t = count == 1 ? 0 : double(i) / double(count - 1);
			const CVector3 camera = flyStart * (1.0 - t) + flyEnd * t;
			capture(camera, flyTarget, fov);
		}
	}
	else
	{
		for (const sShot &shot : shots)
			capture(shot.camera, shot.target, shot.fov);
	}
	Remember();
	QJsonObject result = AnimationState();
	result["kind"] = kind;
	result["note"] =
		"This replaced the flight path. It did not change keyframes. Render it with "
		"render_animation mode flight. Orbit looks at one target from a circle. A fly-through "
		"moves the eye in a straight line; keep the target on the shape or the frame goes empty.";
	return result;
}

QJsonObject FrameScene(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	CVector3 lookAt(0.0, 0.0, 0.0);
	QString error;
	if (params.contains("target") && !ParseVec3(params.value("target"), &lookAt, &error))
		return Fail(error);

	auto parTemp = std::make_shared<cParameterContainer>(*gPar);
	parTemp->Set("limits_enabled", false);
	parTemp->Set("interior_mode", false);
	const QStringList names = parTemp->GetListOfParameters();
	for (const QString &name : names)
	{
		if ((name.contains("primitive_plane") || name.contains("primitive_water")) && name.contains("enabled"))
			parTemp->Set(name, false);
	}

	CVector3 camera = gPar->Get<CVector3>("camera");
	CVector3 target = gPar->Get<CVector3>("target");
	CVector3 top = gPar->Get<CVector3>("camera_top");
	cCameraTarget cameraTarget(camera, target, top);
	CVector3 forward = cameraTarget.GetForwardVector();
	if (forward.Length() < 1e-8) forward = CVector3(0.0, -1.0, 0.0);
	forward.Normalize();

	const params::enumPerspectiveType persp =
		params::enumPerspectiveType(parTemp->Get<int>("perspective_type"));
	double fov = CalcFOV(parTemp->Get<double>("fov"), persp);
	if (fov < 1e-4) fov = 1e-4;
	double deFactor = parTemp->Get<double>("DE_factor");
	if (deFactor < 0.01) deFactor = 1.0;

	std::shared_ptr<sParamRender> renderParams(new sParamRender(parTemp));
	cObjectsTree objectsTree;
	objectsTree.CreateNodeDataFromParameters(parTemp);
	std::vector<cObjectsTree::sNodeDataForRendering> nodes = objectsTree.GetNodeDataListForRendering();
	cHybridFractalSequences fractals;
	fractals.CreateSequences(parTemp, gParFractal, nodes);
	sRenderData renderData;
	renderData.nodesDataForRendering = nodes;
	renderData.hybridFractalSequences = fractals;
	const int depth = GetObjectTreeRequiredStackDepth(renderData.nodesDataForRendering);
	std::vector<ObjectTreeStackFrame> objectStack(size_t(qMax(1, depth)));

	const int rays = qBound(8, params.value("rays").toInt(36), 100);
	double maxDist = 0.0;
	for (int i = 0; i < rays; i++)
	{
		CVector3 direction(Random(1000) / 500.0 - 1.0, Random(1000) / 500.0 - 1.0, Random(1000) / 500.0 - 1.0);
		if (direction.Length() < 1e-8) continue;
		direction.Normalize();
		double distStep = 1.0;
		double scan = 100.0;
		for (; scan > 0.0; scan -= distStep)
		{
			const CVector3 point = lookAt + direction * scan;
			sDistanceIn in(point, 0, false);
			sDistanceOut out;
			const double dist =
				CalculateDistance(*renderParams, fractals, in, &out, &renderData, &objectStack);
			// A NaN distance would otherwise spin. Treat it as a hit and stop.
			if (!(dist > 0.1)) break;
			distStep = dist * deFactor * 0.5;
			if (distStep > 1.0) distStep = 1.0;
			if (distStep < 1e-3) distStep = 1e-3;
		}
		if (scan > maxDist) maxDist = scan;
	}

	if (maxDist < 0.05)
	{
		return Fail(
			"Could not find a surface within the scan. The formula may be empty, the scale may have "
			"collapsed the shape, or it sits far from the target. For a Mandelbox, set "
			"mandelbox_scale to -1.5 (the stored default of 2 is a solid the default camera sits "
			"inside). Or pass target where the shape should be, then call frame_scene again.");
	}

	double newDist;
	if (persp == params::perspThreePoint)
		newDist = maxDist / fov * 2.0 * std::sqrt(2.0);
	else if (persp == params::perspEquirectangular)
		newDist = maxDist / fov * 4.0;
	else
		newDist = maxDist / fov * 2.0;
	if (newDist < 0.1) newDist = 0.1;

	const CVector3 newCamera = lookAt + forward * newDist * (-1.0);
	gPar->Set("target", lookAt);
	gPar->Set("camera", newCamera);
	gPar->Set("camera_distance_to_target", newDist);
	gPar->Set("view_distance_max", (newDist + maxDist) * 2.0);
	Remember();

	QJsonObject result;
	result["camera"] = QJsonArray{newCamera.x, newCamera.y, newCamera.z};
	result["target"] = QJsonArray{lookAt.x, lookAt.y, lookAt.z};
	result["distance"] = newDist;
	result["measured_radius"] = maxDist;
	result["note"] =
		"The camera was pulled back until the measured surface fits the frame, looking at the "
		"target. The viewing direction was kept. One undo step. The formula was not changed. "
		"render_preview to check. If the frame is still empty, set mandelbox_scale to -1.5.";
	return result;
}

QJsonObject RenderStill(const QJsonObject &params, bool waitForResult)
{
	if (RenderBusy())
		return Fail("A render is already running. Call render_status, or stop_render first.");
	if (gMainInterface && gMainInterface->mainImage && gMainInterface->mainImage->IsUsed())
		return Fail("The open window is already rendering. Wait, or call stop_render.");
	QString output = params.value("path").toString();
	if (output.isEmpty())
	{
		output = QDir(QDir::tempPath())
							 .filePath(QString("mandelbulber-still-%1-%2")
													 .arg(QCoreApplication::applicationPid())
													 .arg(gJobSerial + 1));
	}
	else
	{
		output = StripPng(output);
	}
	if (InsideExamples(output)) return Fail("Refusing to write inside the shared examples directory.");
	QDir().mkpath(QFileInfo(output).absolutePath());
	QString error;
	auto job = std::make_shared<sJob>();
	if (!PrepareCopy(params, job, &error)) return Fail(error);
	job->outputDirectory = output;
	job->frameCount = 1;
	job->stopBeforeExecute = params.value("stop_before_execute").toBool(false);
	const int width = job->width;
	const int height = job->height;
	const bool draft = job->draft;
	Launch(job, waitForResult ? "preview" : "still", [](std::shared_ptr<sJob> live) { RunStill(live); });
	if (!waitForResult)
	{
		QJsonObject result = JobStatus(job);
		result["note"] =
			"The render is running. Poll render_status. get_image returns a PNG once a frame has "
			"been written. stop_render sets the stop flag without a confirmation dialog. The live "
			"session's resolution and effects were not changed; this job used a copy.";
		return result;
	}
	QJsonObject result = WaitFor(job);
	if (result.value("ok").toBool(true) == false && result.contains("error")
		&& result.value("status").toString() != "finished")
		return result;
	result["path"] = job->imagePath;
	result["width"] = width;
	result["height"] = height;
	result["note"] = draft
		? "Draft preview from a copy. The session's resolution and expensive effects were not changed. "
			"The window stayed responsive while this ran."
		: "Still render from a copy of the session. Quality and effects are the session's. The window "
			"stayed responsive while this ran.";
	return result;
}

QJsonObject RenderAnimation(const QJsonObject &params)
{
	if (RenderBusy())
		return Fail("A render is already running. Call render_status, or stop_render first.");
	QString mode = params.value("mode").toString("keyframe");
	if (mode != "keyframe" && mode != "flight")
		return Fail("mode must be keyframe or flight.");
	gKeyframes->UpdateFramesIndexesTable();
	const int available =
		mode == "keyframe" ? gKeyframes->GetTotalNumberOfFrames() : gAnimFrames->GetNumberOfFrames();
	if (available < 1)
		return Fail(mode == "keyframe" ? "Add at least one keyframe first."
																	 : "The flight path is empty. Call set_flight_path first.");
	int start = params.value("start").toInt(params.value("start_frame").toInt(0));
	int end = params.value("end").toInt(params.value("end_frame").toInt(available));
	if (start < 0 || end > available || start >= end)
		return Fail(QString("Frame range must satisfy 0 <= start < end <= %1.").arg(available));
	const int maxFrames = qBound(1, params.value("max_frames").toInt(24), 500);
	if (end - start > maxFrames)
	{
		return Fail(QString("That range is %1 frames. The cap is %2 so a sketch cannot become an "
												"overnight render. Pass a shorter end, or max_frames if you mean it.")
									.arg(end - start)
									.arg(maxFrames));
	}
	QString directory = params.value("directory").toString(params.value("path").toString());
	if (directory.isEmpty())
	{
		directory = QDir(QDir::tempPath())
									.filePath(QString("mandelbulber-anim-%1-%2")
																.arg(QCoreApplication::applicationPid())
																.arg(gJobSerial + 1));
	}
	if (InsideExamples(directory)) return Fail("Refusing to write inside the shared examples directory.");
	QDir().mkpath(directory);
	QString error;
	auto job = std::make_shared<sJob>();
	if (!PrepareCopy(params, job, &error)) return Fail(error);
	job->outputDirectory = directory;
	job->frameCount = end - start;
	job->stopBeforeExecute = params.value("stop_before_execute").toBool(false);
	job->wantContactSheet = params.value("contact_sheet").toBool(false);
	job->frameEdge = qBound(16, params.value("frame_edge").toInt(96), 320);
	job->columns = params.value("columns").toInt(0);
	job->keyframes = std::make_shared<cKeyframes>(*gKeyframes);
	job->flight = std::make_shared<cAnimationFrames>(*gAnimFrames);
	Launch(job, mode, [mode, start, end](std::shared_ptr<sJob> live) {
		RunAnimation(live, mode, start, end);
	});
	QJsonObject result = JobStatus(job);
	result["start"] = start;
	result["end"] = end;
	result["note"] =
		"Frames are written as frame_NNNN.png. Poll render_status, or pass a progress token and this "
		"call waits. get_image returns the latest finished frame, downscaled if you pass max_edge. "
		"get_contact_sheet tiles the frames into one PNG. contact_sheet true also writes "
		"contact_sheet.png when the job ends. This uses a copy, so the open session's resolution is "
		"unchanged. quality draft caps detail and turns the expensive effects off.";
	return result;
}

std::shared_ptr<sJob> FindJob(const QString &requested)
{
	if (!requested.isEmpty()) return gJobs.value(requested);
	if (!gActiveJobId.isEmpty()) return gJobs.value(gActiveJobId);
	std::shared_ptr<sJob> best;
	int bestSerial = -1;
	for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
	{
		const int serial = it.value()->id.mid(4).toInt();
		if (serial < bestSerial) continue;
		QMutexLocker locker(&it.value()->mutex);
		if (it.value()->frames.isEmpty()) continue;
		bestSerial = serial;
		best = it.value();
	}
	return best;
}

QJsonObject GetContactSheet(const QJsonObject &params)
{
	const std::shared_ptr<sJob> job = FindJob(params.value("id").toString());
	if (!job)
		return Fail("No animation frames yet. render_animation first, then get_contact_sheet.");
	QStringList frames;
	QString directory;
	{
		QMutexLocker locker(&job->mutex);
		frames = job->frames;
		directory = job->outputDirectory;
	}
	if (frames.isEmpty())
		return Fail("That job has no finished frames yet. Poll render_status, then try again.");
	const int frameEdge = qBound(16, params.value("frame_edge").toInt(job->frameEdge), 320);
	const int columns = params.value("columns").toInt(0);
	const QString base = QDir(QDir::tempPath())
												 .filePath(QString("mandelbulber-sheet-%1-%2")
																			 .arg(QCoreApplication::applicationPid())
																			 .arg(++gJobSerial));
	const sSheet sheet = WriteContactSheet(frames, base, frameEdge, columns);
	if (sheet.path.isEmpty()) return Fail("The frames were there but the contact sheet could not be written.");
	QJsonObject result;
	result["path"] = sheet.path;
	result["width"] = sheet.width;
	result["height"] = sheet.height;
	result["frames"] = sheet.count;
	result["id"] = job->id;
	if (!directory.isEmpty()) result["output"] = directory;
	result["note"] =
		"One PNG of the finished frames, left to right. frame_edge is the thumbnail size. columns 0 "
		"uses up to 6 across. This is for review; the frame_NNNN.png files are the real frames.";
	return result;
}

QJsonObject Downscaled(const QString &path, int maxEdge)
{
	QImage image(path);
	if (image.isNull()) return Fail(QString("Could not read %1.").arg(path));
	if (maxEdge > 0 && qMax(image.width(), image.height()) > maxEdge)
		image = image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	const QString base = QDir(QDir::tempPath())
												 .filePath(QString("mandelbulber-view-%1-%2")
																			 .arg(QCoreApplication::applicationPid())
																			 .arg(++gJobSerial));
	const QString png = base + ".png";
	if (!image.save(png, "PNG")) return Fail("Could not write the downscaled view.");
	QJsonObject result;
	result["path"] = png;
	result["source"] = path;
	result["width"] = image.width();
	result["height"] = image.height();
	return result;
}
} // namespace

namespace mcpScene
{
bool JobActive()
{
	return RenderBusy();
}

bool Dispatch(const QString &method, const QJsonObject &params, QJsonObject *result)
{
	if (method == "get_objects_tree") *result = ObjectsTree();
	else if (method == "add_object") *result = AddObject(params);
	else if (method == "update_object") *result = UpdateObject(params);
	else if (method == "delete_object") *result = DeleteObject(params);
	else if (method == "set_material") *result = SetMaterial(params);
	else if (method == "set_light") *result = SetLight(params);
	else if (method == "set_camera") *result = SetCamera(params);
	else if (method == "frame_scene") *result = FrameScene(params);
	else if (method == "get_animation") *result = AnimationState();
	else if (method == "add_keyframe") *result = AddKeyframe(params);
	else if (method == "update_keyframe") *result = UpdateKeyframe(params);
	else if (method == "delete_keyframe") *result = DeleteKeyframe(params);
	else if (method == "set_flight_path" || method == "set_flight_frames") *result = SetFlightPath(params);
	else if (method == "render_preview")
	{
		QJsonObject preview = params;
		if (!preview.contains("quality")) preview.insert("quality", "draft");
		const QString quality = preview.value("quality").toString();
		if (quality != "draft" && quality != "better" && quality != "full")
		{
			*result = Fail("quality must be draft, better, or full.");
			return true;
		}
		if (!preview.contains("long_side") && !preview.contains("width"))
			preview.insert("long_side", quality == "better" ? 480 : 320);
		*result = RenderStill(preview, true);
	}
	else if (method == "render_still") *result = RenderStill(params, false);
	else if (method == "render_animation")
	{
		QJsonObject anim = params;
		if (!anim.contains("quality")) anim.insert("quality", "draft");
		*result = RenderAnimation(anim);
	}
	else if (method == "render_status")
	{
		const QString id = params.value("id").toString(gActiveJobId);
		const std::shared_ptr<sJob> job = gJobs.value(id);
		if (!job) *result = Fail("No render job. Pass id, or start one with render_still or render_animation.");
		else
			*result = JobStatus(job);
	}
	else if (method == "stop_render")
	{
		const bool windowRendering =
			gMainInterface && gMainInterface->mainImage && gMainInterface->mainImage->IsUsed();
		if (windowRendering) gMainInterface->stopRequest = true;
		if (!gActiveJobId.isEmpty())
		{
			const std::shared_ptr<sJob> job = gJobs.value(gActiveJobId);
			if (job) job->stop = true;
		}
		QJsonObject object;
		object["stopping"] = true;
		object["note"] =
			"The stop flag is set. There is no confirmation dialog. render_status changes to stopped "
			"or finished when the current tile or frame notices.";
		*result = object;
	}
	else if (method == "get_contact_sheet") *result = GetContactSheet(params);
	else if (method == "get_image")
	{
		QString path = LastImage();
		if (!gActiveJobId.isEmpty())
		{
			const std::shared_ptr<sJob> job = gJobs.value(gActiveJobId);
			if (job)
			{
				QMutexLocker locker(&job->mutex);
				if (!job->imagePath.isEmpty()) path = job->imagePath;
			}
		}
		if (params.contains("path")) path = params.value("path").toString();
		if (path.isEmpty() || !QFileInfo::exists(path))
			*result = Fail("No finished image yet. Render a preview, still, or animation frame first.");
		else
			*result = Downscaled(path, params.value("max_edge").toInt(0));
	}
	else
		return false;
	return true;
}
} // namespace mcpScene
