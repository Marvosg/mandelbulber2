/*
 * Mandelbulber v2 — MCP scene, look, animation, and render-job commands.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mcp_scene.h"
#include "render_checkpoint.hpp"

#include "mcp_server.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QMutex>
#include <QRegularExpression>
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
#include "scripts.h"
#include "camera_movement_modes.h"
#include "manipulations.h"
#include "synchronize_interface.hpp"
#include "system_data.hpp"
#include "system_directories.hpp"
#include "trace_behind.h"
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
	bool anim = false;
	bool stop = false;
	// Set from the Background renders dock. An agent stop_render leaves this false.
	bool cancelledByUser = false;
	// Only one full job writes the shared "background" checkpoint.
	bool useCheckpoint = false;
	// False until this lane has a free slot. A queued job has not started its thread.
	bool started = false;
	QString lane = QString("agent");
	QString title;
	QString summary;
	QString quality = QString("full");
	QImage thumbnail;
	int thumbGeneration = 0;
	// Pixels of the render that is running. Cleared when it finishes unless a preview is open.
	std::shared_ptr<cImage> liveImage;
	// Finished PNG, loaded only if a preview opens after the buffer was released.
	QImage savedFrame;
	int previewWatchers = 0;
	// Started from the Background renders dock. Uses the session's thread settings.
	bool fromWindow = false;
	qint64 lastThumbMs = 0;
	qint64 startedAt = 0;
	qint64 finishedAt = 0;
	bool cameraInside = false;
	std::function<void(std::shared_ptr<sJob>)> body;
	// Socket-only. Set immediately before Execute so a stop that arrives
	// before the render clears its flag is still honored.
	bool stopBeforeExecute = false;
	bool wantContactSheet = false;
	int frameEdge = 96;
	int columns = 0;
	QString contactSheetPath;
	QJsonObject previewGrid;
	QString note;
	std::atomic<bool> done{false};
	QMutex mutex;
	std::shared_ptr<cParameterContainer> par;
	std::shared_ptr<cFractalContainer> fractal;
	// Copies taken on the main thread so the worker does not share gKeyframes.
	std::shared_ptr<cKeyframes> keyframes;
	std::shared_ptr<cAnimationFrames> flight;
};

QHash<QString, std::shared_ptr<sJob>> gJobs;
QMutex gJobsMutex;
QString gActiveJobId;
bool gMainWaitArmed = false;
void PumpQueues();
void MaybeStoreThumb(
	const std::shared_ptr<sJob> &job, const std::shared_ptr<cImage> &image, bool force);
QString gLastImage;
QMutex gImageMutex;
QJsonObject gPreviewGrid;
QMutex gPreviewMutex;
int gJobSerial = 0;

void SetLastImage(const QString &path)
{
	QMutexLocker locker(&gImageMutex);
	gLastImage = path;
}

// The job thread and the render workers are not the UI thread. Queued so the
// status bar is touched only there. A call made before the thread starts can
// update the bar directly.
void PostBackgroundProgress(const QString &kind, const QString &progressText, double progress)
{
	if (!gMainInterface || !gMainInterface->mainWindow) return;
	QMetaObject::invokeMethod(
		gMainInterface,
		[kind, progressText, progress]() {
			if (gMainInterface)
				gMainInterface->ShowBackgroundRenderProgress(kind, progressText, progress);
		},
		Qt::QueuedConnection);
}

void PostBackgroundFinished()
{
	if (!gMainInterface || !gMainInterface->mainWindow) return;
	QMetaObject::invokeMethod(
		gMainInterface,
		[]() {
			if (gMainInterface) gMainInterface->HideBackgroundRenderProgress();
		},
		Qt::QueuedConnection);
}

QString LastImage()
{
	QMutexLocker locker(&gImageMutex);
	return gLastImage;
}

int CountRunningJobs()
{
	QMutexLocker locker(&gJobsMutex);
	int count = 0;
	for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
	{
		const std::shared_ptr<sJob> job = it.value();
		if (job && job->started && !job->done.load()) count++;
	}
	return count;
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
	const QString rejected =
		"Colors are #rrggbb or [r, g, b]. Components are 0 to 255, or 0 to 65535.";
	if (value.isString())
	{
		const QString text = value.toString().trimmed();
		if (text.size() != 7 || !text.startsWith('#'))
		{
			*error = rejected;
			return false;
		}
		bool ok = false;
		const int rgb = text.mid(1).toInt(&ok, 16);
		if (!ok)
		{
			*error = rejected;
			return false;
		}
		*out = sRGB(((rgb >> 16) & 255) * 257, ((rgb >> 8) & 255) * 257, (rgb & 255) * 257);
		return true;
	}
	if (!value.isArray() || value.toArray().size() != 3)
	{
		*error = rejected;
		return false;
	}
	double channels[3];
	const QJsonArray array = value.toArray();
	for (int i = 0; i < 3; i++)
	{
		if (!array.at(i).isDouble())
		{
			*error = rejected;
			return false;
		}
		channels[i] = array.at(i).toDouble();
		if (channels[i] < 0.0 || channels[i] > 65535.0)
		{
			*error = rejected;
			return false;
		}
	}
	const bool eightBit = channels[0] <= 255.0 && channels[1] <= 255.0 && channels[2] <= 255.0;
	auto channel = [&](double component) -> int {
		const double scaled = eightBit ? component * 257.0 : component;
		return qBound(0, int(qRound(scaled)), 65535);
	};
	*out = sRGB(channel(channels[0]), channel(channels[1]), channel(channels[2]));
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
		mcpScene::CommitUndoAction();
	}
	RefreshGui();
}

bool SetPrimitiveSize(int nodeId, double radius, QString *what, QString *error);

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
	if (node.type == enumNodeType::primitive)
	{
		for (const sPrimitiveItem &primitive : cPrimitives::GetListOfPrimitives(gPar))
		{
			if (!gPar->IfExists(primitive.Name("object_id"))) continue;
			if (gPar->Get<int>(primitive.Name("object_id")) != node.objectId) continue;
			object["primitive"] = primitive.typeName;
			if (gPar->IfExists(primitive.Name("radius")))
				object["radius"] = gPar->Get<double>(primitive.Name("radius"));
			if (gPar->IfExists(primitive.Name("size")))
			{
				const QString sizeName = primitive.Name("size");
				if (gPar->GetVarType(sizeName) == typeVector3)
				{
					const CVector3 size = gPar->Get<CVector3>(sizeName);
					object["size"] = QJsonArray{size.x, size.y, size.z};
				}
				else
					object["size"] = gPar->Get<double>(sizeName);
			}
			break;
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
	job->quality = quality;
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
		const int asked = params.value("long_side").toInt(320);
		const int cap = job->draft ? 640 : 4096;
		const int longSide = qBound(32, asked, cap);
		if (asked > cap)
		{
			job->note = QString("long_side %1 was capped at %2. An MCP still is at most 4096 pixels "
								 "on a side (640 for a draft). antialiasing_size 2 is already 4 samples "
								 "per output pixel.")
							.arg(asked)
							.arg(cap);
		}
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
	const int sizedWidth = width;
	const int sizedHeight = height;
	width = qBound(16, width, 4096);
	height = qBound(16, height, 4096);
	if (width != sizedWidth || height != sizedHeight)
	{
		const QString clampNote =
			QString("Requested %1x%2 was clamped to %3x%4. An MCP still is at most 4096 pixels on a "
					"side. antialiasing_size 2 is already 4 samples per output pixel.")
				.arg(sizedWidth)
				.arg(sizedHeight)
				.arg(width)
				.arg(height);
		job->note = job->note.isEmpty() ? clampNote : job->note + " " + clampNote;
	}
	job->width = width;
	job->height = height;
	job->par->Set("image_width", width);
	job->par->Set("image_height", height);
	if (job->draft) ApplyDraftLimits(job->par, job->detailCap);
	EnsureCopyMaterial(job->par);
	Q_UNUSED(error);
	return true;
}

QJsonArray Vec3(const CVector3 &v);
QString PlaceName(double x, double y);

void GridShape(int width, int height, int *columns, int *rows)
{
	const double aspect = double(qMax(1, width)) / double(qMax(1, height));
	const double target = 48.0;
	*columns = qBound(4, int(std::lround(std::sqrt(target * aspect))), 12);
	*rows = qBound(4, int(std::lround(std::sqrt(target / aspect))), 12);
}

double MedianOf(QVector<double> values)
{
	if (values.isEmpty()) return 0.0;
	std::sort(values.begin(), values.end());
	const int count = values.size();
	if (count % 2) return values.at(count / 2);
	return 0.5 * (values.at(count / 2 - 1) + values.at(count / 2));
}

struct sRankWeights
{
	double fold = 0.45;
	double contrast = 0.35;
	double edge = 0.25;
	double ridge = 0.12;
	double pocket = 0.05;
	double flat = 0.35;
	double sky = 0.0;
	double inside = 0.02;
	QString name = QString("detail");
};

bool RankWeights(const QJsonValue &prefer, sRankWeights *weights, QString *error)
{
	*weights = sRankWeights();
	if (prefer.isUndefined() || prefer.isNull()) return true;
	QJsonObject overrides;
	QString name = QString("detail");
	if (prefer.isString())
		name = prefer.toString().trimmed().toLower();
	else if (prefer.isObject())
	{
		overrides = prefer.toObject();
		if (overrides.contains("preset")) name = overrides.value("preset").toString().trimmed().toLower();
	}
	else
	{
		*error = "prefer is detail, silhouette, pocket, fold, or space, or an object of weights.";
		return false;
	}
	if (name == "detail" || name.isEmpty())
	{
	}
	else if (name == "silhouette")
	{
		weights->fold = 0.2;
		weights->contrast = 0.55;
		weights->edge = 0.75;
		weights->ridge = 0.5;
		weights->pocket = 0.0;
		weights->flat = 0.2;
		weights->sky = 0.2;
		weights->inside = 0.0;
	}
	else if (name == "pocket")
	{
		weights->fold = 0.25;
		weights->contrast = 0.45;
		weights->edge = 0.05;
		weights->ridge = 0.05;
		weights->pocket = 0.6;
		weights->flat = 0.25;
		weights->sky = 0.0;
	}
	else if (name == "fold")
	{
		weights->fold = 0.9;
		weights->contrast = 0.15;
		weights->edge = 0.05;
		weights->ridge = 0.08;
		weights->pocket = 0.08;
		weights->flat = 0.2;
		weights->sky = 0.0;
	}
	else if (name == "space")
	{
		weights->fold = 0.15;
		weights->contrast = 0.2;
		weights->edge = 0.35;
		weights->ridge = 0.25;
		weights->pocket = 0.05;
		weights->flat = 0.3;
		weights->sky = 0.7;
		weights->inside = 0.0;
	}
	else
	{
		*error = "prefer must be detail, silhouette, pocket, fold, or space.";
		return false;
	}
	weights->name = name.isEmpty() ? QString("detail") : name;
	const struct
	{
		const char *key;
		double *dest;
		double maximum;
	} fields[] = {
		{"fold", &weights->fold, 2.0},
		{"contrast", &weights->contrast, 2.0},
		{"edge", &weights->edge, 2.0},
		{"ridge", &weights->ridge, 2.0},
		{"pocket", &weights->pocket, 2.0},
		{"flat", &weights->flat, 1.0},
		{"sky", &weights->sky, 2.0},
		{"inside", &weights->inside, 2.0},
	};
	bool custom = false;
	for (const auto &field : fields)
	{
		if (!overrides.contains(field.key)) continue;
		if (!overrides.value(field.key).isDouble())
		{
			*error = QString("%1 must be a number from 0 to %2.").arg(field.key).arg(field.maximum);
			return false;
		}
		const double value = overrides.value(field.key).toDouble();
		if (value < 0.0 || value > field.maximum)
		{
			*error = QString("%1 must be a number from 0 to %2.").arg(field.key).arg(field.maximum);
			return false;
		}
		*field.dest = value;
		custom = true;
	}
	if (custom && prefer.isObject() && !overrides.contains("preset")) weights->name = QString("custom");
	return true;
}

double ScoreCellObject(const QJsonObject &cell, const sRankWeights &weights)
{
	const QString label = cell.value("label").toString();
	const double fold = cell.value("fold").toDouble();
	const double contrast = qBound(0.0, cell.value("contrast").toDouble(), 1.0);
	if (label == "sky" || !cell.value("hit").toBool(false))
		return cell.value("borders_hit").toBool(false) ? weights.sky : weights.sky * 0.25;
	if (label == "inside") return weights.inside;
	double score = weights.fold * fold + weights.contrast * contrast;
	if (label == "edge") score += weights.edge;
	else if (label == "ridge") score += weights.ridge;
	else if (label == "pocket") score += weights.pocket;
	else if (label == "flat") score *= weights.flat;
	return score;
}

QJsonObject ApplyRank(QJsonObject grid, const sRankWeights &weights)
{
	QJsonArray cells = grid.value("cells").toArray();
	QVector<double> scores;
	scores.reserve(cells.size());
	for (int index = 0; index < cells.size(); index++)
	{
		QJsonObject cell = cells.at(index).toObject();
		const double score = ScoreCellObject(cell, weights);
		cell["score"] = score;
		cells.replace(index, cell);
		scores.append(score);
	}
	QVector<int> order;
	for (int index = 0; index < cells.size(); index++)
		order.append(index);
	std::sort(order.begin(), order.end(), [&](int a, int b) {
		if (scores.at(a) != scores.at(b)) return scores.at(a) > scores.at(b);
		return a < b;
	});
	QJsonArray ranked;
	for (int index : order)
		ranked.append(cells.at(index));
	grid["cells"] = cells;
	grid["ranked"] = ranked;
	if (!ranked.isEmpty()) grid["best"] = ranked.at(0);
	grid["prefer"] = weights.name;
	grid["note"] =
		"Cells are square in the picture. column and row are 0-based, column left to right, row "
		"top to bottom. ranked is every cell, best first, using prefer '"
		+ weights.name
		+ "'. detail favors a silhouette or a fold and leaves sky at the bottom. silhouette favors a "
			"shape sticking into the sky; sky beside that shape is kept, and the protrusion still ranks "
			"first. pocket favors a recess. fold favors surface detail and ignores the sky. space favors "
			"open sky next to a shape. Pass prefer as one of those names, or as weights fold, contrast, "
			"edge, ridge, pocket, flat, sky, and inside. flat is 0 to 1. The others are 0 to 2. Labels "
			"do not change. Nothing is drawn on the image. probe_view with column and row returns the world "
			"point. A later camera or fov change makes this grid stale.";
	return grid;
}

QJsonObject CapturePreviewGrid(const std::shared_ptr<cImage> &image,
	const std::shared_ptr<cParameterContainer> &par)
{
	QJsonObject grid;
	if (!image || image->GetWidth() < 2 || image->GetHeight() < 2) return grid;
	const int width = int(image->GetWidth());
	const int height = int(image->GetHeight());
	int columns = 4;
	int rows = 4;
	GridShape(width, height, &columns, &rows);
	const bool haveDepth =
		image->GetZBuffer().size() >= size_t(width) * size_t(height);
	const bool haveNormal = image->GetImageOptional()->optionalNormalWorld
													&& haveDepth;

	struct sCellStat
	{
		int column = 0;
		int row = 0;
		double x = 0;
		double y = 0;
		double coverage = 0;
		bool hit = false;
		double depth = -1;
		double normalVar = 0;
		bool hasNormal = false;
		CVector3 normal;
		int red = 0;
		int green = 0;
		int blue = 0;
		QString label;
		double fold = 0;
		double contrast = 0;
		bool closer = false;
		bool bordersHit = false;
	};

	QVector<sCellStat> stats;
	stats.reserve(columns * rows);
	QVector<double> hitDepths;
	for (int row = 0; row < rows; row++)
	{
		for (int column = 0; column < columns; column++)
		{
			sCellStat cell;
			cell.column = column;
			cell.row = row;
			cell.x = (column + 0.5) / columns;
			cell.y = (row + 0.5) / rows;
			const int samples = 3;
			int hitCount = 0;
			QVector<double> depths;
			QVector<CVector3> normals;
			CVector3 normalSum;
			int red = 0;
			int green = 0;
			int blue = 0;
			int count = 0;
			for (int sy = 0; sy < samples; sy++)
			{
				for (int sx = 0; sx < samples; sx++)
				{
					const double fx = (column + (sx + 0.5) / samples) / columns;
					const double fy = (row + (sy + 0.5) / samples) / rows;
					const quint64 px = qBound(quint64(0),
						quint64(std::llround(fx * double(width - 1))), quint64(width - 1));
					const quint64 py = qBound(quint64(0),
						quint64(std::llround(fy * double(height - 1))), quint64(height - 1));
					const sRGBFloat color = image->GetPixelImage(px, py);
					red += qBound(0, int(std::lround(qBound(0.0f, color.R, 1.0f) * 255.0f)), 255);
					green += qBound(0, int(std::lround(qBound(0.0f, color.G, 1.0f) * 255.0f)), 255);
					blue += qBound(0, int(std::lround(qBound(0.0f, color.B, 1.0f) * 255.0f)), 255);
					count++;
					if (!haveDepth) continue;
					const float z = image->GetPixelZBuffer(px, py);
					if (!std::isfinite(z) || z < 0.0f || z >= 1e19f) continue;
					hitCount++;
					depths.append(double(z));
					if (!haveNormal) continue;
					const sRGBFloat stored = image->GetPixelNormalWorld(px, py);
					CVector3 normal(stored.R, stored.G, stored.B);
					if (normal.Length() > 0.5)
					{
						normals.append(normal);
						normalSum = normalSum + normal;
					}
				}
			}
			cell.coverage = double(hitCount) / double(samples * samples);
			cell.hit = hitCount > 0;
			if (!depths.isEmpty()) cell.depth = MedianOf(depths);
			if (cell.hit) hitDepths.append(cell.depth);
			if (normals.size() >= 2)
			{
				CVector3 average = normalSum;
				average.Normalize();
				double spread = 0.0;
				for (CVector3 normal : normals)
				{
					normal.Normalize();
					spread += 1.0 - normal.Dot(average);
				}
				cell.normalVar = spread / double(normals.size());
				cell.normal = average;
				cell.hasNormal = true;
			}
			cell.red = red / qMax(1, count);
			cell.green = green / qMax(1, count);
			cell.blue = blue / qMax(1, count);
			stats.append(cell);
		}
	}

	const double medianDepth = MedianOf(hitDepths);
	for (int index = 0; index < stats.size(); index++)
	{
		sCellStat &cell = stats[index];
		if (!cell.hit)
		{
			cell.label = "sky";
			continue;
		}
		if (medianDepth > 0.0 && cell.depth < 0.03 * medianDepth)
		{
			cell.label = "inside";
			continue;
		}
		bool edge = cell.coverage < 0.67;
		QVector<double> neighborDepths;
		const int neighbors[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (const auto &step : neighbors)
		{
			const int column = cell.column + step[0];
			const int row = cell.row + step[1];
			if (column < 0 || row < 0 || column >= columns || row >= rows) continue;
			const sCellStat &other = stats.at(row * columns + column);
			if (!other.hit)
				edge = true;
			else
				neighborDepths.append(other.depth);
		}
		if (!neighborDepths.isEmpty())
		{
			const double neighborMedian = MedianOf(neighborDepths);
			cell.contrast = std::abs(cell.depth - neighborMedian) / qMax(neighborMedian, 1e-9);
			cell.closer = cell.depth < neighborMedian;
		}
		cell.fold = qBound(0.0, cell.normalVar / 0.35, 1.0);
		if (edge)
			cell.label = "edge";
		else if (cell.contrast >= 0.08 && cell.closer)
			cell.label = "ridge";
		else if (cell.contrast >= 0.08)
			cell.label = "pocket";
		else if (cell.normalVar >= 0.12)
			cell.label = "slope";
		else
			cell.label = "flat";
	}
	for (int index = 0; index < stats.size(); index++)
	{
		if (stats.at(index).hit) continue;
		const int neighbors[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (const auto &step : neighbors)
		{
			const int column = stats.at(index).column + step[0];
			const int row = stats.at(index).row + step[1];
			if (column < 0 || row < 0 || column >= columns || row >= rows) continue;
			if (stats.at(row * columns + column).hit)
			{
				stats[index].bordersHit = true;
				break;
			}
		}
	}

	auto pack = [&](const sCellStat &cell) {
		QJsonObject object;
		object["column"] = cell.column;
		object["row"] = cell.row;
		object["x"] = cell.x;
		object["y"] = cell.y;
		object["x0"] = double(cell.column) / columns;
		object["y0"] = double(cell.row) / rows;
		object["x1"] = double(cell.column + 1) / columns;
		object["y1"] = double(cell.row + 1) / rows;
		object["place"] = PlaceName(cell.x, cell.y);
		object["hit"] = cell.hit;
		object["label"] = cell.label;
		object["fold"] = cell.fold;
		object["contrast"] = cell.contrast;
		object["closer"] = cell.closer;
		object["borders_hit"] = cell.bordersHit;
		object["color"] = QString("#%1%2%3")
												.arg(cell.red, 2, 16, QChar('0'))
												.arg(cell.green, 2, 16, QChar('0'))
												.arg(cell.blue, 2, 16, QChar('0'));
		if (cell.hit) object["depth"] = cell.depth;
		if (cell.hasNormal) object["normal"] = Vec3(cell.normal);
		return object;
	};

	QJsonArray cells;
	for (const sCellStat &cell : stats)
		cells.append(pack(cell));
	const CVector3 camera = par->Get<CVector3>("camera");
	const CVector3 target = par->Get<CVector3>("target");
	grid["columns"] = columns;
	grid["rows"] = rows;
	grid["camera"] = Vec3(camera);
	grid["target"] = Vec3(target);
	grid["fov"] = par->Get<double>("fov");
	grid["cells"] = cells;
	grid = ApplyRank(grid, sRankWeights());
	{
		QMutexLocker locker(&gPreviewMutex);
		gPreviewGrid = grid;
	}
	return grid;
}


bool RenderOne(const std::shared_ptr<sJob> &job, const QString &basePath, QString *saved, QString *error)
{
	job->par->Set("image_width", job->width);
	job->par->Set("image_height", job->height);
	if (job->draft) ApplyDraftLimits(job->par, job->detailCap);
	auto image = std::make_shared<cImage>(job->width, job->height);
	{
		QMutexLocker locker(&job->mutex);
		job->liveImage = image;
	}
	struct sReleaseLive
	{
		std::shared_ptr<sJob> job;
		~sReleaseLive()
		{
			if (!job) return;
			QMutexLocker locker(&job->mutex);
			if (job->previewWatchers <= 0) job->liveImage.reset();
		}
	} releaseLive{job};
	cRenderJob renderJob(job->par, job->fractal, image, 1, &job->stop);
	renderJob.PreserveStopRequest();
	if (job->useCheckpoint)
		renderJob.EnableCheckpoint(QString("background"), basePath,
			job->anim ? QString("animation") : QString("still"));
	cRenderingConfiguration config;
	config.DisableRefresh();
	config.DisableProgressiveRender();
	config.DisableNetRender();
	QObject *progressTarget = gMainInterface ? static_cast<QObject *>(gMainInterface)
																					: static_cast<QObject *>(gMcpServer);
	if (progressTarget)
	{
		QObject::connect(
			&renderJob, &cRenderJob::updateProgressAndStatus, progressTarget,
			[job, image](const QString &, const QString &progressText, double progress) {
				QString kind;
				QString message;
				double overall = progress;
				{
					QMutexLocker locker(&job->mutex);
					// The render posts this on the UI thread. A last "100% done" line can
					// arrive after FinishJob and would replace the whole-job time.
					if (job->done.load()) return;
					if (job->frameCount > 1)
					{
						overall = (double(job->frame) + progress) / double(job->frameCount);
						message = QString("Frame %1/%2 - %3")
												.arg(job->frame + 1)
												.arg(job->frameCount)
												.arg(progressText);
					}
					else
					{
						message = progressText;
					}
					job->message = message;
					job->progress = overall;
					kind = job->kind;
				}
				MaybeStoreThumb(job, image, false);
				if (gMainInterface)
					gMainInterface->ShowBackgroundRenderProgress(kind, message, overall);
			},
			Qt::QueuedConnection);
	}
	if (!renderJob.Init(cRenderJob::still, config))
	{
		*error = QString("The render did not start.");
		return false;
	}
	// Init copied the application preference. An MCP render that has not been
	// given an explicit core count or priority uses one core less, at the
	// lowest priority, without writing that preference.
	if (!job->fromWindow) ApplyMcpRenderThreads();
	// After Init, before Execute clears the flag. A real stop_render that won
	// this race is already true and is kept by PreserveStopRequest.
	if (job->stopBeforeExecute) job->stop = true;
	if (!renderJob.Execute())
	{
		bool cancelled = false;
		{
			QMutexLocker locker(&job->mutex);
			cancelled = job->cancelledByUser;
		}
		*error = job->stop ? (cancelled ? QString("The user cancelled this render.")
																		: QString("The render was stopped."))
											 : QString("The render did not finish.");
		return false;
	}
	*saved = SaveColorPng(image, basePath);
	MaybeStoreThumb(job, image, true);
	if (saved->isEmpty())
	{
		*error = "The frame rendered but the PNG could not be written.";
		return false;
	}
	if (!job->anim)
	{
		const QJsonObject grid = CapturePreviewGrid(image, job->par);
		int inside = 0;
		int total = 0;
		for (const QJsonValue &cell : grid.value("cells").toArray())
		{
			total++;
			if (cell.toObject().value("label").toString() == QLatin1String("inside")) inside++;
		}
		const bool cameraInside = total > 0 && inside * 4 >= total;
		QMutexLocker locker(&job->mutex);
		job->previewGrid = grid;
		job->cameraInside = cameraInside;
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

int ByteFromLinear(float value)
{
	if (!(value > 0.f)) return 0;
	if (value > 1.f) value = 1.f;
	value = std::pow(value, 1.f / 2.2f);
	return int(value * 255.f + 0.5f);
}

// A few dozen samples, not a full-frame convert. finalPass reads the compiled
// 16-bit image; during the render the live buffer is linear float.
QImage SampleThumbnail(const std::shared_ptr<cImage> &image, bool finalPass)
{
	if (!image) return {};
	const int width = int(image->GetWidth());
	const int height = int(image->GetHeight());
	if (width < 2 || height < 2) return {};
	const int edge = 72;
	int thumbWidth = edge;
	int thumbHeight = qMax(1, edge * height / width);
	if (height > width)
	{
		thumbHeight = edge;
		thumbWidth = qMax(1, edge * width / height);
	}
	QImage thumb(thumbWidth, thumbHeight, QImage::Format_RGB32);
	for (int y = 0; y < thumbHeight; y++)
	{
		QRgb *row = reinterpret_cast<QRgb *>(thumb.scanLine(y));
		const int sourceY = qMin(height - 1, y * height / thumbHeight);
		for (int x = 0; x < thumbWidth; x++)
		{
			const int sourceX = qMin(width - 1, x * width / thumbWidth);
			int red;
			int green;
			int blue;
			if (finalPass)
			{
				const sRGB16 pixel = image->GetPixelImage16(quint64(sourceX), quint64(sourceY));
				red = pixel.R >> 8;
				green = pixel.G >> 8;
				blue = pixel.B >> 8;
			}
			else
			{
				const sRGBFloat pixel = image->GetPixelImage(quint64(sourceX), quint64(sourceY));
				red = ByteFromLinear(pixel.R);
				green = ByteFromLinear(pixel.G);
				blue = ByteFromLinear(pixel.B);
			}
			row[x] = qRgb(red, green, blue);
		}
	}
	return thumb;
}

void StoreThumb(const std::shared_ptr<sJob> &job, const QImage &thumb)
{
	if (thumb.isNull()) return;
	QMutexLocker locker(&job->mutex);
	job->thumbnail = thumb;
	job->thumbGeneration++;
}

void MaybeStoreThumb(const std::shared_ptr<sJob> &job, const std::shared_ptr<cImage> &image, bool force)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	{
		QMutexLocker locker(&job->mutex);
		if (!force && now - job->lastThumbMs < 700) return;
		job->lastThumbMs = now;
	}
	StoreThumb(job, SampleThumbnail(image, force));
}

bool WindowBlocksMain()
{
	return gMainInterface && gMainInterface->mainImage && gMainInterface->mainImage->IsUsed();
}

void PumpQueues()
{
	if (gMainInterface && QThread::currentThread() != gMainInterface->thread())
	{
		QMetaObject::invokeMethod(gMainInterface, []() { PumpQueues(); }, Qt::QueuedConnection);
		return;
	}
	QList<std::shared_ptr<sJob>> starting;
	bool waitWindow = false;
	{
		QMutexLocker locker(&gJobsMutex);
		auto take = [&](const QString &lane, bool blocked) {
			bool running = false;
			std::shared_ptr<sJob> next;
			int best = std::numeric_limits<int>::max();
			for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
			{
				const std::shared_ptr<sJob> job = it.value();
				if (!job || job->done.load() || job->lane != lane) continue;
				if (job->started)
				{
					running = true;
					continue;
				}
				const int serial = job->id.mid(4).toInt();
				if (serial < best)
				{
					best = serial;
					next = job;
				}
			}
			if (running || !next) return;
			if (blocked)
			{
				waitWindow = true;
				return;
			}
			QMutexLocker jobLock(&next->mutex);
			next->started = true;
			next->startedAt = QDateTime::currentMSecsSinceEpoch();
			next->status = QString("running");
			if (next->message.contains(QString("Waiting")))
				next->message = QObject::tr("Rendering");
			next->useCheckpoint = lane == QLatin1String("main") && !next->draft;
			starting.append(next);
		};
		take(QString("agent"), false);
		take(QString("main"), WindowBlocksMain());
	}
	for (const std::shared_ptr<sJob> &job : starting)
	{
		QThread *thread = QThread::create([job]() {
			if (job->body) job->body(job);
		});
		QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
		thread->start();
	}
	if (waitWindow && !gMainWaitArmed && gMainInterface)
	{
		gMainWaitArmed = true;
		QTimer::singleShot(400, gMainInterface, []() {
			gMainWaitArmed = false;
			PumpQueues();
		});
	}
}

QString FormatWall(qint64 milliseconds);

void FinishJob(const std::shared_ptr<sJob> &job, const QString &status, const QString &error)
{
	{
		QMutexLocker locker(&job->mutex);
		job->status = status;
		job->error = error;
		if (status == "finished")
		{
			job->progress = 1;
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const qint64 elapsed = job->startedAt > 0 ? now - job->startedAt : 0;
			job->message = QObject::tr("Finished, total time: %1").arg(FormatWall(elapsed));
		}
		else if (status == "failed" || status == "stopped" || status == "cancelled")
		{
			job->message = error.isEmpty() ? status : error;
		}
		if (!job->imagePath.isEmpty()) SetLastImage(job->imagePath);
		job->finishedAt = QDateTime::currentMSecsSinceEpoch();
		job->done.store(true);
	}
	PumpQueues();
	PostBackgroundFinished();
}

void RunStill(const std::shared_ptr<sJob> job)
{
	QString saved;
	QString error;
	const QString base = job->outputDirectory;
	if (!RenderOne(job, base, &saved, &error))
	{
		bool cancelled = false;
		{
			QMutexLocker locker(&job->mutex);
			cancelled = job->cancelledByUser;
		}
		FinishJob(job, job->stop ? (cancelled ? QString("cancelled") : QString("stopped")) : QString("failed"),
			error);
	}
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
	job->anim = true;
	renderCheckpoint::sAnimation plan;
	plan.mode = mode;
	plan.start = start;
	plan.end = end;
	plan.frame = start;
	plan.width = job->width;
	plan.height = job->height;
	plan.directory = job->outputDirectory;
	plan.frameOutput =
		QDir(job->outputDirectory).filePath(QString("frame_%1").arg(start, 4, 10, QChar('0')));
	plan.finished = job->frames;
	plan.contactSheet = job->wantContactSheet;
	plan.frameEdge = job->frameEdge;
	plan.columns = job->columns;
	if (job->useCheckpoint && job->flight && job->keyframes)
		renderCheckpoint::StartAnimation(plan, job->par, job->fractal, job->flight, job->keyframes);
	for (int frame = start; frame < end; frame++)
	{
		if (job->stop) break;
		QString frameMessage;
		{
			QMutexLocker locker(&job->mutex);
			job->frame = frame - start;
			frameMessage = QString("Frame %1 of %2").arg(frame - start + 1).arg(end - start);
			job->message = frameMessage;
			job->progress = double(job->frame) / double(qMax(1, job->frameCount));
		}
		PostBackgroundProgress(
			job->kind, frameMessage, double(frame - start) / double(qMax(1, end - start)));
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
			bool cancelled = false;
			{
				QMutexLocker locker(&job->mutex);
				cancelled = job->cancelledByUser;
			}
			FinishJob(job, job->stop ? (cancelled ? QString("cancelled") : QString("stopped")) : QString("failed"),
				error);
			return;
		}
		QMutexLocker locker(&job->mutex);
		job->frames.append(saved);
		job->imagePath = saved;
		locker.unlock();
		SetLastImage(saved);
		if (job->useCheckpoint) renderCheckpoint::AdvanceAnimation(frame + 1, saved);
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
	QString haltError;
	bool cancelled = false;
	if (job->stop)
	{
		QMutexLocker locker(&job->mutex);
		cancelled = job->cancelledByUser;
		haltError = cancelled ? QString("The user cancelled this render.") : QString("The render was stopped.");
	}
	FinishJob(job, job->stop ? (cancelled ? QString("cancelled") : QString("stopped")) : QString("finished"),
		haltError);
	if (!job->stop && job->useCheckpoint) renderCheckpoint::Finish();
}

QString FormulaLabel(const std::shared_ptr<cParameterContainer> &par,
	const std::shared_ptr<cFractalContainer> &fractal)
{
	if (!fractal) return QString();
	QSet<int> drawn;
	if (par)
	{
		const QList<int> drawnSlots = mcpScene::DrawnFormulaSlots(par);
		for (int slot : drawnSlots)
			drawn.insert(slot);
	}
	QStringList names;
	for (int i = 0; i < fractal->size(); i++)
	{
		if (!drawn.isEmpty() && !drawn.contains(i + 1)) continue;
		const std::shared_ptr<cParameterContainer> slot = fractal->at(i);
		if (!slot) continue;
		if (slot->IfExists("fractal_enable") && !slot->Get<bool>("fractal_enable")) continue;
		const int formulaId = slot->Get<int>("formula");
		for (cAbstractFractal *formula : newFractalList)
		{
			if (int(formula->getInternalId()) != formulaId) continue;
			const QString internal = QString::fromStdString(formula->getInternalName());
			if (internal.isEmpty() || internal == QLatin1String("none")) break;
			names.append(QString::fromStdString(formula->getNameInComboBox()));
			break;
		}
	}
	return names.join(QString(" + "));
}

void DescribeJob(const std::shared_ptr<sJob> &job)
{
	QString kindLabel = job->fromWindow ? QObject::tr("Background") : QObject::tr("Still");
	if (job->kind == "preview")
		kindLabel = QObject::tr("Preview");
	else if (job->kind == "keyframe")
		kindLabel = QObject::tr("Keyframe animation");
	else if (job->kind == "flight")
		kindLabel = QObject::tr("Flight animation");
	const QString file = QFileInfo(job->outputDirectory).fileName();
	if (!file.isEmpty() && job->kind != "keyframe" && job->kind != "flight")
		job->title = QString("%1 — %2").arg(kindLabel, file);
	else if (job->frameCount > 1)
		job->title = QString("%1 — %2 frames").arg(kindLabel).arg(job->frameCount);
	else
		job->title = kindLabel;

	double detail = 0;
	if (job->par && job->par->IfExists("detail_level")) detail = job->par->Get<double>("detail_level");
	if (job->detailCap > 0) detail = qMin(detail, job->detailCap);
	QStringList bits;
	bits << QString("%1×%2").arg(job->width).arg(job->height);
	bits << (job->quality.isEmpty() ? QString("full") : job->quality);
	if (detail > 0) bits << QObject::tr("detail %1").arg(QString::number(detail, 'g', 4));
	if (job->frameCount > 1) bits << QObject::tr("%1 frames").arg(job->frameCount);
	QStringList lines;
	const QString formulas = FormulaLabel(job->par, job->fractal);
	if (!formulas.isEmpty()) lines << formulas;
	lines << bits.join(QString(" · "));
	const QString path = QDir::toNativeSeparators(QFileInfo(job->outputDirectory).absoluteFilePath());
	if (!path.isEmpty()) lines << path;
	job->summary = lines.join(QLatin1Char('\n'));
}

void Launch(const std::shared_ptr<sJob> &job, const QString &kind,
	const std::function<void(std::shared_ptr<sJob>)> &body)
{
	job->id = QString("job-%1").arg(++gJobSerial);
	job->kind = kind;
	job->lane = (kind == QLatin1String("preview") || job->quality != QLatin1String("full"))
								? QString("agent")
								: QString("main");
	job->status = QString("queued");
	job->message = job->lane == QLatin1String("main") ? QObject::tr("Waiting in the main queue")
																										 : QObject::tr("Waiting in the agent queue");
	job->body = body;
	DescribeJob(job);
	{
		QMutexLocker locker(&gJobsMutex);
		gJobs.insert(job->id, job);
		gActiveJobId = job->id;
	}
	if (gMainInterface && gMainInterface->mainWindow)
		gMainInterface->ShowBackgroundRenderProgress(kind, job->message, 0);
	PumpQueues();
}

QString FormatWall(qint64 milliseconds)
{
	const double seconds = qMax(0.0, milliseconds / 1000.0);
	if (seconds < 60.0) return QString::number(seconds, 'f', 1) + QString("s");
	const int minutes = int(seconds) / 60;
	return QString("%1m %2s").arg(minutes).arg(QString::number(std::fmod(seconds, 60.0), 'f', 1));
}

QJsonObject ClientGrid(const QJsonObject &grid, bool full)
{
	if (grid.isEmpty()) return grid;
	if (full)
	{
		QJsonObject copy = grid;
		copy["grid"] = QString("full");
		return copy;
	}
	QJsonObject copy;
	const QStringList keep = {"columns", "rows", "best", "prefer", "camera", "target", "fov"};
	for (const QString &key : keep)
	{
		if (grid.contains(key)) copy[key] = grid.value(key);
	}
	const QJsonArray ranked = grid.value("ranked").toArray();
	QJsonArray top;
	for (int i = 0; i < ranked.size() && top.size() < 5; i++)
		top.append(ranked.at(i));
	copy["ranked"] = top;
	copy["ranked_total"] = ranked.size();
	copy["grid"] = QString("best");
	copy["note"] =
		"best and the top five ranked cells. Pass grid full for every cell.";
	return copy;
}

QJsonObject JobStatus(const std::shared_ptr<sJob> &job, bool fullGrid = false)
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
	if (!job->previewGrid.isEmpty()) result["grid"] = ClientGrid(job->previewGrid, fullGrid);
	if (job->cameraInside) result["camera_inside"] = true;
	if (!job->contactSheetPath.isEmpty()) result["contact_sheet"] = job->contactSheetPath;
	if (!job->note.isEmpty()) result["note"] = job->note;
	if (!job->title.isEmpty()) result["title"] = job->title;
	if (!job->summary.isEmpty()) result["summary"] = job->summary;
	result["lane"] = job->lane;
	if (job->status == QLatin1String("queued")) result["queued"] = true;
	if (job->stop && !job->done.load()) result["stopping"] = true;
	if (job->cancelledByUser)
	{
		result["cancelled_by_user"] = true;
		result["note"] =
			"The user cancelled this render from the Background renders window. That stop was intentional.";
		if (job->error.isEmpty()) result["error"] = "The user cancelled this render.";
	}
	QJsonArray frames;
	for (const QString &path : job->frames)
		frames.append(path);
	result["frames"] = frames;
	result["done"] = job->done.load();
	return result;
}

QJsonObject WaitFor(const std::shared_ptr<sJob> &job, bool fullGrid = false)
{
	QEventLoop loop;
	QTimer timer;
	timer.setInterval(40);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
		if (job->done.load()) loop.quit();
	});
	timer.start();
	loop.exec();
	QJsonObject result = JobStatus(job, fullGrid);
	const QString status = result.value("status").toString();
	if (status == "failed" || status == "stopped" || status == "cancelled")
	{
		result["ok"] = false;
		if (result.value("error").toString().isEmpty())
		{
			if (status == "cancelled")
				result["error"] = QString("The user cancelled this render.");
			else if (status == "stopped")
				result["error"] = QString("The render was stopped.");
			else
				result["error"] = QString("The render did not finish.");
		}
	}
	return result;
}

QString StripPng(QString path)
{
	if (path.endsWith(".png", Qt::CaseInsensitive)) path.chop(4);
	return path;
}

// A picture title. Folders stay in path. settings.fract is not a picture name.
QString PictureName(const QString &requested, QString *error)
{
	QString name = requested.trimmed();
	if (name.endsWith(".png", Qt::CaseInsensitive)) name.chop(4);
	name = name.trimmed();
	if (name.isEmpty())
	{
		*error = "name must be a picture title.";
		return QString();
	}
	if (name.contains('/') || name.contains('\\') || name == "." || name == ".."
			|| name.contains(QString("..")))
	{
		*error = "name is the picture title, not a folder. Put the folder in path.";
		return QString();
	}
	name.remove(QRegularExpression("[\\x00-\\x1f]"));
	if (name.trimmed().isEmpty())
	{
		*error = "name must be a picture title.";
		return QString();
	}
	return name.trimmed();
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
	if (params.contains("radius"))
	{
		QString written;
		QString error;
		if (!SetPrimitiveSize(nodeId, params.value("radius").toDouble(), &written, &error))
			return Fail(error);
	}
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
	{
		const QString gradient = params.value("surface_color_gradient").toString().simplified();
		const QStringList parts = gradient.split(' ');
		if (parts.size() < 2 || parts.size() % 2 != 0)
			return Fail("A gradient is position and color pairs, like \"0 fd6029 9000 fff59b\".");
		for (int i = 0; i < parts.size(); i += 2)
		{
			bool positionOk = false;
			parts.at(i).toDouble(&positionOk);
			bool colorOk = false;
			if (parts.at(i + 1).size() != 6) colorOk = false;
			else
				parts.at(i + 1).toInt(&colorOk, 16);
			if (!positionOk || !colorOk)
				return Fail("A gradient is position and color pairs, like \"0 fd6029 9000 fff59b\".");
		}
		gPar->Set(prefix + "surface_color_gradient", gradient);
	}
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

QJsonArray Vec3(const CVector3 &v)
{
	return QJsonArray{v.x, v.y, v.z};
}

QString PlaceName(double x, double y)
{
	const QString horizontal = x < 0.34 ? QString("left") : x > 0.66 ? QString("right") : QString("center");
	const QString vertical = y < 0.34 ? QString("upper") : y > 0.66 ? QString("lower") : QString("middle");
	if (horizontal == "center" && vertical == "middle") return "center";
	if (horizontal == "center") return vertical;
	if (vertical == "middle") return horizontal;
	return vertical + " " + horizontal;
}

bool ScreenPoint(double x, double y, CVector3 *direction, QString *error)
{
	if (!(x >= -1.0 && x <= 2.0 && y >= -1.0 && y <= 2.0))
	{
		*error = "x and y must be from -1 to 2. 0, 0 is the top left. Values outside 0 to 1 are outside the picture.";
		return false;
	}
	const int width = qMax(1, gPar->Get<int>("image_width"));
	const int height = qMax(1, gPar->Get<int>("image_height"));
	const params::enumPerspectiveType persp =
		params::enumPerspectiveType(gPar->Get<int>("perspective_type"));
	double aspect = double(width) / double(height);
	if (persp == params::perspEquirectangular) aspect = 2.0;
	const double reverse = gPar->Get<bool>("legacy_coordinate_system") ? -1.0 : 1.0;

	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const CVector3 top = gPar->Get<CVector3>("camera_top");
	cCameraTarget cameraTarget(camera, target, top);
	CRotationMatrix rotation;
	rotation.SetRotation(cameraTarget.GetRotation());
	const double sweetH = gPar->Get<double>("sweet_spot_horizontal_angle") / 180.0 * M_PI;
	const double sweetV = gPar->Get<double>("sweet_spot_vertical_angle") / 180.0 * M_PI;
	rotation.RotateZ(-sweetH);
	rotation.RotateX(sweetV);

	CVector2<double> normalized;
	normalized.x = (x - 0.5) * aspect;
	normalized.y = (y - 0.5) * (-1.0) * reverse;
	*direction = CalculateViewVector(normalized, CalcFOV(gPar->Get<double>("fov"), persp), persp, rotation);
	if (!(direction->Length() > 1e-8))
	{
		*error = "That spot does not have a view ray.";
		return false;
	}
	direction->Normalize();
	return true;
}

void Crosshair(double *x, double *y)
{
	const double width = qMax(1, gPar->Get<int>("image_width"));
	const double height = qMax(1, gPar->Get<int>("image_height"));
	const params::enumPerspectiveType persp =
		params::enumPerspectiveType(gPar->Get<int>("perspective_type"));
	const double fov = CalcFOV(gPar->Get<double>("fov"), persp);
	const double aspect = persp == params::perspEquirectangular ? 2.0 : width / height;
	const double sweetH = gPar->Get<double>("sweet_spot_horizontal_angle") / 180.0 * M_PI;
	const double sweetV = gPar->Get<double>("sweet_spot_vertical_angle") / 180.0 * M_PI;
	CVector2<double> shift(0.0, 0.0);
	switch (persp)
	{
		case params::perspThreePoint:
			shift.y = std::tan(sweetV) / fov;
			shift.x = std::tan(-sweetH) / fov / std::cos(sweetV) / aspect;
			break;
		case params::perspFishEye:
		case params::perspFishEyeCut:
		{
			CVector3 forward(0.0, 0.0, 1.0);
			forward = forward.RotateAroundVectorByAngle(CVector3(0.0, 1.0, 0.0), -sweetH);
			forward = forward.RotateAroundVectorByAngle(CVector3(1.0, 0.0, 0.0), -sweetV);
			forward.Normalize();
			const double r = std::sqrt(forward.x * forward.x + forward.y * forward.y);
			if (r > 0.0)
			{
				const double r2 = std::asin(r) * 2.0;
				shift.x = (forward.x / fov) * r2 / r / 2.0 / aspect;
				shift.y = (forward.y / fov) * r2 / r / 2.0;
			}
			break;
		}
		case params::perspEquirectangular:
		{
			CVector3 forward(0.0, 0.0, 1.0);
			forward = forward.RotateAroundVectorByAngle(CVector3(0.0, 1.0, 0.0), -sweetH);
			forward = forward.RotateAroundVectorByAngle(CVector3(1.0, 0.0, 0.0), -sweetV);
			shift.x = std::asin(forward.x / std::cos(std::asin(forward.y))) / 0.5 / fov / aspect;
			if (forward.z < 0 && shift.x > 0) shift.x = fov / aspect - shift.x;
			if (forward.z < 0 && shift.x < 0) shift.x = -fov / aspect - shift.x;
			shift.y = std::asin(forward.y) / 0.5 / fov;
			break;
		}
	}
	*x = qBound(0.0, 0.5 * (1.0 + 2.0 * shift.x), 1.0);
	*y = qBound(0.0, 0.5 * (1.0 + 2.0 * shift.y), 1.0);
}

double ViewportDepth(double x, double y)
{
	if (!gMainInterface || !gMainInterface->mainImage || gMainInterface->mainImage->IsUsed()) return -1.0;
	cImage *image = gMainInterface->mainImage.get();
	const quint64 width = image->GetWidth();
	const quint64 height = image->GetHeight();
	if (width < 2 || height < 2) return -1.0;
	if (image->GetZBuffer().size() < size_t(width * height)) return -1.0;
	const quint64 px = qBound(quint64(0), quint64(std::llround(x * double(width - 1))), width - 1);
	const quint64 py = qBound(quint64(0), quint64(std::llround(y * double(height - 1))), height - 1);
	const float depth = image->GetPixelZBuffer(px, py);
	if (!std::isfinite(depth) || !(depth < 1e19f)) return -1.0;
	return double(depth);
}

struct sProbeEngine
{
	std::shared_ptr<sParamRender> params;
	cHybridFractalSequences fractals;
	sRenderData renderData;
	std::vector<ObjectTreeStackFrame> stack;
	double resolution = 1.0 / 600.0;

	sProbeEngine()
	{
		params.reset(new sParamRender(gPar));
		cObjectsTree objectsTree;
		objectsTree.CreateNodeDataFromParameters(gPar);
		const std::vector<cObjectsTree::sNodeDataForRendering> nodes =
			objectsTree.GetNodeDataListForRendering();
		fractals.CreateSequences(gPar, gParFractal, nodes);
		renderData.nodesDataForRendering = nodes;
		renderData.hybridFractalSequences = fractals;
		const int depth = GetObjectTreeRequiredStackDepth(renderData.nodesDataForRendering);
		stack.assign(size_t(qMax(1, depth)), ObjectTreeStackFrame());
		resolution = 1.0 / double(qMax(1, gPar->Get<int>("image_height")));
		params->resolution = resolution;
	}

	double Distance(const CVector3 &point, double *multiplier, int *objectId = nullptr)
	{
		const sDistanceIn in(point, 0, false);
		sDistanceOut out;
		const double dist = CalculateDistance(*params, fractals, in, &out, &renderData, &stack);
		if (multiplier) *multiplier = out.detailLevelMultiplier;
		if (objectId) *objectId = out.objectId;
		return dist;
	}

	int NodeIdFor(int internalId) const
	{
		if (internalId < 0) return -1;
		for (const cObjectsTree::sNodeDataForRendering &node : renderData.nodesDataForRendering)
		{
			if (node.internalObjectId == internalId || node.userObjectId == internalId) return node.id;
		}
		return -1;
	}

	QString NodeNameFor(int nodeId) const
	{
		for (const cObjectsTree::sNodeDataForRendering &node : renderData.nodesDataForRendering)
		{
			if (node.id == nodeId) return node.name;
		}
		return QString();
	}

	double Threshold(const CVector3 &point, double multiplier) const
	{
		double distThresh;
		if (params->constantDEThreshold)
			distThresh = params->DEThresh;
		else
		{
			distThresh = (params->camera - point).Length() * resolution * params->fov
									 / qMax(1e-6, params->detailLevel);
			if (params->perspectiveType == params::perspEquirectangular
					|| params->perspectiveType == params::perspFishEye
					|| params->perspectiveType == params::perspFishEyeCut)
				distThresh *= 0.5;
		}
		if (multiplier > 0.0) distThresh *= multiplier;
		if (!(distThresh > 1e-12)) distThresh = 1e-12;
		return distThresh;
	}
};

struct sSurfaceHit
{
	bool hit = false;
	double depth = 0;
	CVector3 position;
	CVector3 normal;
	CVector3 direction;
	int nodeId = -1;
	QString nodeName;
};

sSurfaceHit MarchRay(sProbeEngine *engine, const CVector3 &direction)
{
	sSurfaceHit hit;
	hit.direction = direction;
	const CVector3 origin = engine->params->camera;
	const double maxScan = qMax(engine->params->viewDistanceMax, 1e-3);
	double deFactor = engine->params->DEFactor;
	if (!(deFactor > 0.01)) deFactor = 1.0;
	double scan = 0.0;
	double dist = 0.0;
	double distThresh = 1e-4;
	double lastStep = distThresh;
	CVector3 point = origin;
	bool found = false;
	for (int i = 0; i < 800; i++)
	{
		point = origin + direction * scan;
		if (point.IsNotANumber()) break;
		double multiplier = 1.0;
		dist = engine->Distance(point, &multiplier);
		distThresh = engine->Threshold(point, multiplier);
		if (!std::isfinite(dist)) break;
		if (dist < distThresh)
		{
			found = true;
			break;
		}
		double step = (dist - 0.5 * distThresh) * deFactor;
		if (!(step > distThresh)) step = distThresh;
		if (step > 3.0) step = 3.0;
		lastStep = step;
		scan += step;
		if (scan > maxScan) break;
	}
	if (found)
	{
		double step = lastStep * 0.5;
		for (int i = 0; i < 24; i++)
		{
			if (dist < distThresh && dist > distThresh * 0.1) break;
			scan += dist > distThresh ? step : -step;
			if (scan < 0.0) scan = 0.0;
			point = origin + direction * scan;
			double multiplier = 1.0;
			dist = engine->Distance(point, &multiplier);
			if (!std::isfinite(dist)) break;
			distThresh = engine->Threshold(point, multiplier);
			step *= 0.5;
		}
	}
	hit.hit = found;
	hit.depth = scan;
	hit.position = origin + direction * scan;
	if (found)
	{
		const double eps = qMax(distThresh, 1e-6);
		const CVector3 axes[3] = {CVector3(eps, 0, 0), CVector3(0, eps, 0), CVector3(0, 0, eps)};
		double gradient[3];
		for (int axis = 0; axis < 3; axis++)
		{
			const double ahead = engine->Distance(hit.position + axes[axis], nullptr);
			const double behind = engine->Distance(hit.position - axes[axis], nullptr);
			gradient[axis] = ahead - behind;
		}
		hit.normal = CVector3(gradient[0], gradient[1], gradient[2]);
		if (hit.normal.Length() > 1e-12)
			hit.normal.Normalize();
		else
			hit.normal = direction * (-1.0);
		if (hit.normal.Dot(direction) > 0.0) hit.normal = hit.normal * (-1.0);
		int objectId = -1;
		engine->Distance(hit.position, nullptr, &objectId);
		hit.nodeId = engine->NodeIdFor(objectId);
		hit.nodeName = engine->NodeNameFor(hit.nodeId);
	}
	return hit;
}

QJsonObject PreviewCellAt(double x, double y, bool *cameraMatches)
{
	QJsonObject grid;
	{
		QMutexLocker locker(&gPreviewMutex);
		grid = gPreviewGrid;
	}
	if (cameraMatches) *cameraMatches = false;
	if (grid.isEmpty() || !gPar) return QJsonObject();
	CVector3 storedCamera;
	CVector3 storedTarget;
	QString error;
	if (!ParseVec3(grid.value("camera"), &storedCamera, &error)) return QJsonObject();
	if (!ParseVec3(grid.value("target"), &storedTarget, &error)) return QJsonObject();
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const double scale = qMax(1.0, (camera - target).Length());
	const bool samePose = (camera - storedCamera).Length() <= 1e-4 * scale
												&& (target - storedTarget).Length() <= 1e-4 * scale
												&& std::abs(grid.value("fov").toDouble() - gPar->Get<double>("fov")) < 1e-3;
	if (cameraMatches) *cameraMatches = samePose;
	if (!samePose) return QJsonObject();
	const int columns = grid.value("columns").toInt();
	const int rows = grid.value("rows").toInt();
	const QJsonArray cells = grid.value("cells").toArray();
	for (const QJsonValue &value : cells)
	{
		const QJsonObject cell = value.toObject();
		const int column = cell.value("column").toInt();
		const int row = cell.value("row").toInt();
		const double x0 = cell.value("x0").toDouble();
		const double y0 = cell.value("y0").toDouble();
		const double x1 = cell.value("x1").toDouble();
		const double y1 = cell.value("y1").toDouble();
		const bool insideX = x >= x0 && (x < x1 || column == columns - 1);
		const bool insideY = y >= y0 && (y < y1 || row == rows - 1);
		if (insideX && insideY) return cell;
	}
	return QJsonObject();
}

QJsonObject SampleJson(const sSurfaceHit &hit, double x, double y, const cCameraTarget &cameraTarget)
{
	const CVector3 delta = hit.position - cameraTarget.GetCamera();
	QJsonObject fromCamera;
	fromCamera["forward"] = delta.Dot(cameraTarget.GetForwardVector());
	fromCamera["right"] = delta.Dot(cameraTarget.GetRightVector());
	fromCamera["up"] = delta.Dot(cameraTarget.GetTopVector());
	QJsonObject sample;
	sample["x"] = x;
	sample["y"] = y;
	sample["place"] = PlaceName(x, y);
	sample["hit"] = hit.hit;
	sample["depth"] = hit.depth;
	sample["position"] = Vec3(hit.position);
	sample["from_camera"] = fromCamera;
	if (hit.hit) sample["normal"] = Vec3(hit.normal);
	if (hit.nodeId >= 0)
	{
		sample["node_id"] = hit.nodeId;
		if (!hit.nodeName.isEmpty()) sample["node_name"] = hit.nodeName;
	}
	const double viewport = ViewportDepth(x, y);
	if (viewport >= 0.0) sample["viewport_depth"] = viewport;
	bool previewMatches = false;
	const QJsonObject preview = PreviewCellAt(x, y, &previewMatches);
	if (!preview.isEmpty())
	{
		sample["column"] = preview.value("column");
		sample["row"] = preview.value("row");
		sample["label"] = preview.value("label");
		sample["score"] = preview.value("score");
		sample["color"] = preview.value("color");
		if (preview.contains("depth")) sample["preview_depth"] = preview.value("depth");
	}
	return sample;
}

bool NamedPoint(const QString &name, double *x, double *y, QString *error)
{
	const QString key = name.trimmed().toLower().replace(' ', '_').replace('-', '_');
	if (key == "crosshair")
	{
		Crosshair(x, y);
		return true;
	}
	const struct
	{
		const char *name;
		double x;
		double y;
	} places[] = {
		{"center", 0.5, 0.5},
		{"left", 0.2, 0.5},
		{"right", 0.8, 0.5},
		{"top", 0.5, 0.2},
		{"upper", 0.5, 0.2},
		{"bottom", 0.5, 0.8},
		{"lower", 0.5, 0.8},
		{"upper_left", 0.2, 0.2},
		{"top_left", 0.2, 0.2},
		{"upper_right", 0.8, 0.2},
		{"top_right", 0.8, 0.2},
		{"lower_left", 0.2, 0.8},
		{"bottom_left", 0.2, 0.8},
		{"lower_right", 0.8, 0.8},
		{"bottom_right", 0.8, 0.8},
	};
	for (const auto &place : places)
	{
		if (key == place.name)
		{
			*x = place.x;
			*y = place.y;
			return true;
		}
	}
	*error =
		"at must be center, crosshair, left, right, top, bottom, upper_left, upper_right, "
		"lower_left, or lower_right.";
	return false;
}

QJsonObject StoredGrid(bool *matches)
{
	QJsonObject grid;
	{
		QMutexLocker locker(&gPreviewMutex);
		grid = gPreviewGrid;
	}
	if (matches) *matches = false;
	if (grid.isEmpty() || !gPar) return QJsonObject();
	CVector3 storedCamera;
	CVector3 storedTarget;
	QString error;
	if (!ParseVec3(grid.value("camera"), &storedCamera, &error)) return grid;
	if (!ParseVec3(grid.value("target"), &storedTarget, &error)) return grid;
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const double scale = qMax(1.0, (camera - target).Length());
	const bool samePose = (camera - storedCamera).Length() <= 1e-4 * scale
												&& (target - storedTarget).Length() <= 1e-4 * scale
												&& std::abs(grid.value("fov").toDouble() - gPar->Get<double>("fov")) < 1e-3;
	if (matches) *matches = samePose;
	return grid;
}

bool ReadIndexRange(const QJsonValue &value, int limit, int *first, int *last, QString *error, const char *name)
{
	auto one = [&](const QJsonValue &item, int *out) {
		if (!item.isDouble()) return false;
		const int index = item.toInt(-1);
		if (index < 0 || index >= limit) return false;
		*out = index;
		return true;
	};
	if (value.isDouble())
	{
		if (!one(value, first))
		{
			*error = QString("%1 is out of range. This grid has %2, and the index is 0-based.").arg(name).arg(limit);
			return false;
		}
		*last = *first;
		return true;
	}
	if (value.isArray() && value.toArray().size() == 2 && one(value.toArray().at(0), first)
			&& one(value.toArray().at(1), last))
	{
		if (*first > *last) std::swap(*first, *last);
		return true;
	}
	*error = QString("%1 is an index, or [first, last], 0-based.").arg(name);
	return false;
}

bool CellRangeCenter(const QJsonObject &params, double *x, double *y, QString *error)
{
	bool matches = false;
	const QJsonObject grid = StoredGrid(&matches);
	if (grid.isEmpty())
	{
		*error = "render_preview first. It ranks the square cells, and columns and rows refer to that grid.";
		return false;
	}
	if (!matches)
	{
		*error = "The preview grid is stale. The camera or fov changed. render_preview, then use columns and rows.";
		return false;
	}
	const int columns = grid.value("columns").toInt();
	const int rows = grid.value("rows").toInt();
	int column0 = 0;
	int column1 = 0;
	int row0 = 0;
	int row1 = 0;
	if (!ReadIndexRange(params.value("columns"), columns, &column0, &column1, error, "columns")) return false;
	if (!ReadIndexRange(params.value("rows"), rows, &row0, &row1, error, "rows")) return false;
	*x = (column0 + column1 + 1) / (2.0 * columns);
	*y = (row0 + row1 + 1) / (2.0 * rows);
	const QJsonObject center = PreviewCellAt(*x, *y, nullptr);
	if (center.value("hit").toBool(false)) return true;
	double bestScore = -1.0;
	bool found = false;
	for (const QJsonValue &value : grid.value("cells").toArray())
	{
		const QJsonObject cell = value.toObject();
		const int column = cell.value("column").toInt();
		const int row = cell.value("row").toInt();
		if (column < column0 || column > column1 || row < row0 || row > row1) continue;
		if (!cell.value("hit").toBool(false)) continue;
		const double score = cell.value("score").toDouble();
		if (!found || score > bestScore)
		{
			found = true;
			bestScore = score;
			*x = cell.value("x").toDouble();
			*y = cell.value("y").toDouble();
		}
	}
	if (found) return true;
	*error = "That part of the picture missed the surface. Pick a range whose label is edge, ridge, or pocket.";
	return false;
}

QJsonObject ProbeView(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	const bool hasX = params.contains("x") || params.contains("y");
	const bool hasAt = params.contains("at") && !params.value("at").toString().isEmpty();
	const bool hasColumn = params.contains("column") || params.contains("row");
	const int grid = params.value("grid").toInt(0);
	if (grid != 0 && (grid < 1 || grid > 5)) return Fail("grid must be from 1 to 5.");
	const int selectors = int(hasX) + int(hasAt) + int(hasColumn) + int(grid > 0);
	if (selectors > 1) return Fail("Pass one of x and y, at, column and row, or grid.");
	if (hasX && (!params.contains("x") || !params.contains("y")))
		return Fail("Pass both x and y.");
	if (hasColumn && (!params.contains("column") || !params.contains("row")))
		return Fail("Pass both column and row.");
	if (!hasX && !hasAt && !hasColumn && grid == 0)
	{
		bool matches = false;
		QJsonObject stored = StoredGrid(&matches);
		if (stored.isEmpty())
			return Fail("render_preview first. It ranks a grid of square cells. Then call probe_view with no arguments, or with column and row.");
		if (params.contains("prefer"))
		{
			sRankWeights weights;
			QString error;
			if (!RankWeights(params.value("prefer"), &weights, &error)) return Fail(error);
			stored = ApplyRank(stored, weights);
			if (matches)
			{
				QMutexLocker locker(&gPreviewMutex);
				gPreviewGrid = stored;
			}
		}
		const bool fullCells = params.value("cells").toString() == QLatin1String("full")
													 || params.value("grid").toString() == QLatin1String("full");
		QJsonObject result = ClientGrid(stored, fullCells);
		result["preview_matches"] = matches;
		if (!matches)
			result["note"] = "The camera or fov changed after this preview, so column and row are stale. render_preview again.";
		else if (!fullCells && result.value("note").toString().isEmpty())
			result["note"] =
				"best and the top five ranked cells. Pass cells full, or grid full, for every cell.";
		return result;
	}
	if (params.contains("prefer"))
		return Fail("prefer re-ranks the preview grid. Call probe_view with prefer and no point.");
	QVector<QPair<double, double>> points;
	if (hasAt)
	{
		double x = 0;
		double y = 0;
		QString error;
		if (!NamedPoint(params.value("at").toString(), &x, &y, &error)) return Fail(error);
		points.append(qMakePair(x, y));
	}
	else if (hasX)
	{
		if (!params.value("x").isDouble() || !params.value("y").isDouble())
			return Fail("x and y must be numbers. 0, 0 is the top left. -1 to 2 reaches outside the picture.");
		points.append(qMakePair(params.value("x").toDouble(), params.value("y").toDouble()));
	}
	else if (hasColumn)
	{
		QJsonObject range;
		range["columns"] = params.value("column");
		range["rows"] = params.value("row");
		double x = 0;
		double y = 0;
		QString error;
		if (!CellRangeCenter(range, &x, &y, &error)) return Fail(error);
		points.append(qMakePair(x, y));
	}
	else
	{
		const int cells = grid;
		for (int row = 0; row < cells; row++)
			for (int column = 0; column < cells; column++)
				points.append(qMakePair((column + 0.5) / cells, (row + 0.5) / cells));
	}

	sProbeEngine engine;
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const CVector3 top = gPar->Get<CVector3>("camera_top");
	const cCameraTarget cameraTarget(camera, target, top);
	QJsonArray samples;
	int nearest = -1;
	double nearestDepth = 0;
	for (int i = 0; i < points.size(); i++)
	{
		CVector3 direction;
		QString error;
		if (!ScreenPoint(points.at(i).first, points.at(i).second, &direction, &error)) return Fail(error);
		const sSurfaceHit hit = MarchRay(&engine, direction);
		if (hit.hit && (nearest < 0 || hit.depth < nearestDepth))
		{
			nearest = i;
			nearestDepth = hit.depth;
		}
		QJsonObject sample = SampleJson(hit, points.at(i).first, points.at(i).second, cameraTarget);
		samples.append(sample);
	}
	if (nearest >= 0)
	{
		QJsonObject marked = samples.at(nearest).toObject();
		marked["nearest"] = true;
		samples.replace(nearest, marked);
	}
	double crossX = 0.5;
	double crossY = 0.5;
	Crosshair(&crossX, &crossY);
	QJsonObject result;
	result["samples"] = samples;
	result["crosshair"] = QJsonObject{{"x", crossX}, {"y", crossY}};
	result["axes"] = QJsonObject{{"forward", Vec3(cameraTarget.GetForwardVector())},
		{"right", Vec3(cameraTarget.GetRightVector())}, {"up", Vec3(cameraTarget.GetTopVector())}};
	if (nearest >= 0) result["nearest"] = samples.at(nearest);
	bool previewMatches = false;
	PreviewCellAt(0.5, 0.5, &previewMatches);
	{
		QMutexLocker locker(&gPreviewMutex);
		if (!gPreviewGrid.isEmpty()) result["preview_matches"] = previewMatches;
	}
	result["note"] =
		"x and y are fractions of the picture, origin at the top left, matching the viewfinder. "
		"0.5, 0.5 is the crosshair when the sweet spot is centered. Values outside 0 to 1 are outside "
		"the picture, which is how look_around points at a shape just off frame. depth is the distance "
		"from the eye to the surface along that ray. from_camera is forward, right, and up. position is "
		"the world point to pass back. node_id is the objects-tree node the ray hit. column and row "
		"are the preview grid cell, 0-based. label and score are that cell's rank. A call with no "
		"arguments returns the ranked grid from the last preview. preview_matches false means the "
		"camera or fov changed. A miss means the ray left view_distance_max without a surface.";
	return result;
}

QJsonObject LookAround(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	sRankWeights weights;
	QString weightError;
	if (!RankWeights(params.value("prefer"), &weights, &weightError)) return Fail(weightError);
	const double xs[5] = {-0.4, 0.15, 0.5, 0.85, 1.4};
	const double ys[3] = {-0.25, 0.5, 1.25};
	const double offsets[5][2] = {{0.0, 0.0}, {0.08, 0.0}, {-0.08, 0.0}, {0.0, 0.08}, {0.0, -0.08}};
	struct sRay
	{
		bool hit = false;
		double depth = 0;
		CVector3 position;
		CVector3 normal;
	};
	struct sCluster
	{
		double x = 0;
		double y = 0;
		bool outside = false;
		QVector<sRay> rays;
		QString label;
		double score = 0;
	};
	sProbeEngine engine;
	QVector<sCluster> clusters;
	QVector<double> depths;
	for (double y : ys)
	{
		for (double x : xs)
		{
			sCluster cluster;
			cluster.x = x;
			cluster.y = y;
			cluster.outside = x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0;
			for (const auto &offset : offsets)
			{
				CVector3 direction;
				QString error;
				sRay ray;
				if (ScreenPoint(x + offset[0], y + offset[1], &direction, &error))
				{
					const sSurfaceHit hit = MarchRay(&engine, direction);
					ray.hit = hit.hit;
					ray.depth = hit.depth;
					ray.position = hit.position;
					ray.normal = hit.normal;
					if (hit.hit) depths.append(hit.depth);
				}
				cluster.rays.append(ray);
			}
			clusters.append(cluster);
		}
	}
	const double medianDepth = MedianOf(depths);
	QJsonArray directions;
	for (int index = 0; index < clusters.size(); index++)
	{
		sCluster &cluster = clusters[index];
		QVector<double> local;
		CVector3 normalSum;
		int normalCount = 0;
		int hits = 0;
		for (const sRay &ray : cluster.rays)
		{
			if (!ray.hit) continue;
			hits++;
			local.append(ray.depth);
			if (ray.normal.Length() > 0.5)
			{
				normalSum = normalSum + ray.normal;
				normalCount++;
			}
		}
		double normalVar = 0.0;
		if (normalCount >= 2)
		{
			CVector3 average = normalSum;
			average.Normalize();
			double spread = 0.0;
			for (const sRay &ray : cluster.rays)
			{
				if (ray.normal.Length() <= 0.5) continue;
				CVector3 normal = ray.normal;
				normal.Normalize();
				spread += 1.0 - normal.Dot(average);
			}
			normalVar = spread / double(normalCount);
		}
		const double depth = local.isEmpty() ? -1.0 : MedianOf(local);
		double contrast = 0.0;
		if (depth > 0.0 && medianDepth > 0.0)
			contrast = std::abs(depth - medianDepth) / qMax(medianDepth, 1e-9);
		QJsonObject measured;
		measured["hit"] = hits > 0;
		measured["fold"] = qBound(0.0, normalVar / 0.35, 1.0);
		measured["contrast"] = qBound(0.0, contrast, 1.0);
		if (hits == 0)
			cluster.label = "sky";
		else if (medianDepth > 0.0 && depth < 0.03 * medianDepth)
			cluster.label = "inside";
		else
		{
			const bool edge = hits < cluster.rays.size();
			const bool closer = depth < medianDepth;
			if (edge)
				cluster.label = "edge";
			else if (contrast >= 0.08 && closer)
				cluster.label = "ridge";
			else if (contrast >= 0.08)
				cluster.label = "pocket";
			else if (normalVar >= 0.12)
				cluster.label = "slope";
			else
				cluster.label = "flat";
		}
		measured["label"] = cluster.label;
		cluster.score = ScoreCellObject(measured, weights);
	}
	for (int index = 0; index < clusters.size(); index++)
	{
		if (clusters.at(index).label != "sky") continue;
		const int column = index % 5;
		const int row = index / 5;
		const int neighbors[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (const auto &step : neighbors)
		{
			const int nextColumn = column + step[0];
			const int nextRow = row + step[1];
			if (nextColumn < 0 || nextRow < 0 || nextColumn >= 5 || nextRow >= 3) continue;
			const int neighbor = nextRow * 5 + nextColumn;
			if (clusters.at(neighbor).label == "sky" || clusters.at(neighbor).label == "inside") continue;
			QJsonObject measured;
			measured["hit"] = false;
			measured["label"] = "sky";
			measured["borders_hit"] = true;
			clusters[index].score = ScoreCellObject(measured, weights);
			break;
		}
	}
	QVector<int> order;
	for (int index = 0; index < clusters.size(); index++)
		order.append(index);
	std::sort(order.begin(), order.end(), [&](int a, int b) {
		if (clusters.at(a).score != clusters.at(b).score) return clusters.at(a).score > clusters.at(b).score;
		return a < b;
	});
	for (int index : order)
	{
		const sCluster &cluster = clusters.at(index);
		const sRay &center = cluster.rays.at(0);
		QJsonObject item;
		item["x"] = cluster.x;
		item["y"] = cluster.y;
		item["outside"] = cluster.outside;
		item["label"] = cluster.label;
		item["score"] = cluster.score;
		item["hit"] = center.hit;
		if (center.hit)
		{
			item["depth"] = center.depth;
			item["position"] = Vec3(center.position);
		}
		directions.append(item);
	}
	QJsonObject result;
	result["directions"] = directions;
	result["prefer"] = weights.name;
	if (!directions.isEmpty()) result["best"] = directions.at(0);
	result["note"] =
		"This does not move the camera. Fifteen directions cover the frame and about half a frame "
		"past each edge. best is the highest score for prefer '"
		+ weights.name
		+ "'. detail favors a silhouette or a fold. silhouette favors a shape sticking into the sky, "
			"and sky beside that shape is not a failure. pocket favors a recess. fold ignores the sky. "
			"space favors open sky next to a shape. outside true means that point is not in the current "
			"picture. Pass its x and y to navigate_camera toward or look_at. Then render_preview.";
	return result;
}

void WriteCamera(const CVector3 &camera, const CVector3 &target, const CVector3 &top)
{
	cCameraTarget cameraTarget(camera, target, top);
	const cCameraTarget::enumRotationMode roll =
		cCameraTarget::enumRotationMode(gPar->Get<int>("camera_straight_rotation"));
	cameraTarget.SetCameraTargetTop(camera, target, top);
	if (roll == cCameraTarget::constantRoll)
		cameraTarget.SetCameraTargetRotation(camera, target, cameraTarget.GetRotation().z);
	gPar->Set("camera", cameraTarget.GetCamera());
	gPar->Set("target", cameraTarget.GetTarget());
	gPar->Set("camera_top", cameraTarget.GetTopVector());
	gPar->Set("camera_rotation", cameraTarget.GetRotation() * (180.0 / M_PI));
	gPar->Set("camera_distance_to_target", cameraTarget.GetDistance());
}

QJsonObject CameraState(const QString &note)
{
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const CVector3 rotation = gPar->Get<CVector3>("camera_rotation");
	QJsonObject result;
	result["camera"] = Vec3(camera);
	result["target"] = Vec3(target);
	result["rotation"] = Vec3(rotation);
	result["fov"] = gPar->Get<double>("fov");
	result["distance"] = gPar->Get<double>("camera_distance_to_target");
	result["note"] = note;
	return result;
}

bool FrameSpot(const QString &name, double *x, double *y, QString *error)
{
	const QString key = name.trimmed().toLower().replace(' ', '_').replace('-', '_');
	const struct
	{
		const char *name;
		double x;
		double y;
	} frames[] = {
		{"center", 0.5, 0.5},
		{"left_third", 1.0 / 3.0, 0.5},
		{"right_third", 2.0 / 3.0, 0.5},
		{"upper_third", 0.5, 1.0 / 3.0},
		{"lower_third", 0.5, 2.0 / 3.0},
		{"upper_left", 1.0 / 3.0, 1.0 / 3.0},
		{"upper_right", 2.0 / 3.0, 1.0 / 3.0},
		{"lower_left", 1.0 / 3.0, 2.0 / 3.0},
		{"lower_right", 2.0 / 3.0, 2.0 / 3.0},
		{"golden_left", 0.382, 0.5},
		{"golden_right", 0.618, 0.5},
		{"golden_upper_left", 0.382, 0.382},
		{"golden_upper_right", 0.618, 0.382},
		{"golden_lower_left", 0.382, 0.618},
		{"golden_lower_right", 0.618, 0.618},
	};
	for (const auto &frame : frames)
	{
		if (key == frame.name)
		{
			*x = frame.x;
			*y = frame.y;
			return true;
		}
	}
	*error =
		"frame must be center, left_third, right_third, upper_third, lower_third, upper_left, "
		"upper_right, lower_left, lower_right, or golden_left, golden_right, golden_upper_left, "
		"golden_upper_right, golden_lower_left, golden_lower_right.";
	return false;
}

bool ResolveSpot(const QJsonValue &raw, sSurfaceHit *hit, QString *error)
{
	if (raw.isArray() && raw.toArray().size() == 3)
	{
		if (!ParseVec3(raw, &hit->position, error)) return false;
		const CVector3 camera = gPar->Get<CVector3>("camera");
		hit->direction = hit->position - camera;
		hit->depth = hit->direction.Length();
		if (!(hit->depth > 1e-8))
		{
			*error = "That point is on the camera.";
			return false;
		}
		hit->direction.Normalize();
		hit->hit = true;
		sProbeEngine engine;
		const double eps = qMax(1e-4, hit->depth * 1e-4);
		const CVector3 axes[3] = {CVector3(eps, 0, 0), CVector3(0, eps, 0), CVector3(0, 0, eps)};
		double gradient[3];
		for (int axis = 0; axis < 3; axis++)
		{
			const double ahead = engine.Distance(hit->position + axes[axis], nullptr);
			const double behind = engine.Distance(hit->position - axes[axis], nullptr);
			gradient[axis] = ahead - behind;
		}
		hit->normal = CVector3(gradient[0], gradient[1], gradient[2]);
		if (hit->normal.Length() > 1e-8)
			hit->normal.Normalize();
		else
			hit->normal = hit->direction * (-1.0);
		return true;
	}
	if (raw.isArray() && raw.toArray().size() == 2 && raw.toArray().at(0).isDouble()
			&& raw.toArray().at(1).isDouble())
	{
		CVector3 direction;
		if (!ScreenPoint(raw.toArray().at(0).toDouble(), raw.toArray().at(1).toDouble(), &direction, error))
			return false;
		sProbeEngine engine;
		*hit = MarchRay(&engine, direction);
		if (!hit->hit)
		{
			*error = "No surface along that ray. probe_view a nearby spot, or frame_scene if the picture is empty.";
			return false;
		}
		if (hit->depth < 1e-5)
		{
			*error = "The eye is inside the surface along that ray. Move backward or call frame_scene, then probe again.";
			return false;
		}
		return true;
	}
	*error = "A spot is [x, y] in the picture, or a world [x, y, z].";
	return false;
}

void OrbitAround(const CVector3 &pivot, const CVector3 &axis, double radians)
{
	const CVector3 camera = gPar->Get<CVector3>("camera");
	const CVector3 target = gPar->Get<CVector3>("target");
	const CVector3 top = gPar->Get<CVector3>("camera_top");
	auto spin = [&](const CVector3 &point) {
		return (point - pivot).RotateAroundVectorByAngle(axis, radians) + pivot;
	};
	WriteCamera(spin(camera), spin(target), top.RotateAroundVectorByAngle(axis, radians));
}

QJsonObject NavigateCamera(const QJsonObject &params, bool commit = true)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	if (params.value("moves").isArray())
	{
		if (commit == false) return Fail("A move list cannot contain another move list.");
		const QStringList extras = {"move", "turn", "orbit", "toward", "look_at", "frame", "zoom", "around",
			"columns", "rows"};
		for (const QString &extra : extras)
		{
			if (params.contains(extra) && !params.value(extra).isNull()
					&& !(params.value(extra).isString() && params.value(extra).toString().isEmpty()))
				return Fail("Pass moves, or one action, not both.");
		}
		const QJsonArray moves = params.value("moves").toArray();
		if (moves.size() < 1 || moves.size() > 20) return Fail("moves must be 1 to 20 actions.");
		int applied = 0;
		for (const QJsonValue &item : moves)
		{
			if (!item.isObject())
			{
				if (applied > 0) Remember();
				return Fail("Each move is an object, such as {\"move\": \"forward\"}.");
			}
			QJsonObject step = item.toObject();
			step.remove("moves");
			const QJsonObject result = NavigateCamera(step, false);
			if (result.value("ok").toBool(true) == false)
			{
				if (applied > 0) Remember();
				QJsonObject failed = result;
				if (applied > 0)
					failed["error"] = result.value("error").toString()
														+ QString(" The first %1 moves were kept. One undo reverts them.").arg(applied);
				return failed;
			}
			applied++;
		}
		Remember();
		return CameraState(QString("Applied %1 moves. One undo step. render_preview.").arg(applied));
	}
	const QString move = params.value("move").toString();
	const QString turn = params.value("turn").toString();
	const QString orbit = params.value("orbit").toString();
	const QString zoom = params.value("zoom").toString();
	const QString frame = params.value("frame").toString();
	const bool toward = params.contains("toward");
	const bool lookAtPoint = params.contains("look_at") && !params.value("look_at").isBool();
	const bool lookAtBool = params.value("look_at").isBool() && params.value("look_at").toBool();
	const bool framing = !frame.isEmpty();
	const bool hasRange = params.contains("columns") || params.contains("rows");
	QJsonValue rangePoint;
	if (hasRange)
	{
		if (!params.contains("columns") || !params.contains("rows"))
			return Fail("Pass both columns and rows. Each is an index, or [first, last], 0-based.");
		if (toward || params.contains("around") || lookAtPoint)
			return Fail("Pass columns and rows, or a screen point, not both.");
		double rangeX = 0.0;
		double rangeY = 0.0;
		QString error;
		if (!CellRangeCenter(params, &rangeX, &rangeY, &error)) return Fail(error);
		rangePoint = QJsonArray{rangeX, rangeY};
	}
	const bool rangeToward = hasRange && orbit.isEmpty() && !framing && move.isEmpty() && turn.isEmpty()
													 && zoom.isEmpty() && !lookAtBool;
	const int chosen = int(!move.isEmpty()) + int(!turn.isEmpty()) + int(!orbit.isEmpty()) + int(toward)
										 + int(rangeToward) + int(lookAtPoint && !framing) + int(lookAtBool) + int(framing)
										 + int(!zoom.isEmpty());
	if (chosen != 1)
		return Fail("Pass one of move, turn, orbit, toward, look_at, zoom, frame, or moves. columns and rows "
								"select the cells for toward, look_at, frame, or orbit.");
	if (hasRange && !framing && orbit.isEmpty() && !rangeToward && !lookAtBool)
		return Fail("columns and rows choose the spot for toward, look_at, frame, or orbit.");
	const int steps = params.value("steps").toInt(1);
	if (steps < 1 || steps > 50) return Fail("steps must be from 1 to 50.");
	if (params.contains("step") && !params.value("step").isDouble()) return Fail("step must be a number.");
	const bool hasStep = params.contains("step");
	const double step = params.value("step").toDouble();
	if (hasStep && !(step > 0.0)) return Fail("step must be greater than 0.");
	auto commitIf = [&]() {
		if (commit) Remember();
	};

	if (!zoom.isEmpty())
	{
		if (zoom != "in" && zoom != "out") return Fail("zoom must be in or out.");
		double fov = gPar->Get<double>("fov");
		const double delta = (hasStep ? step : 5.0) * steps;
		fov += zoom == "in" ? -delta : delta;
		fov = qBound(0.01, fov, 720.0);
		gPar->Set("fov", fov);
		commitIf();
		return CameraState(QString("Field of view is now %1 degrees. One undo step. render_preview.")
												 .arg(fov, 0, 'g', 6));
	}

	if (framing)
	{
		double screenX = 0.5;
		double screenY = 0.5;
		QString error;
		if (!FrameSpot(frame, &screenX, &screenY, &error)) return Fail(error);
		sSurfaceHit hit;
		const QJsonValue raw = hasRange ? rangePoint
			: (lookAtPoint ? params.value("look_at") : QJsonValue(QJsonArray{0.5, 0.5}));
		if (!ResolveSpot(raw, &hit, &error)) return Fail(error);
		const CVector3 top = gPar->Get<CVector3>("camera_top");
		WriteCamera(gPar->Get<CVector3>("camera"), hit.position, top);
		CVector3 desired;
		if (!ScreenPoint(screenX, screenY, &desired, &error)) return Fail(error);
		const double distance = (gPar->Get<CVector3>("camera") - hit.position).Length();
		WriteCamera(hit.position - desired * distance, hit.position, gPar->Get<CVector3>("camera_top"));
		commitIf();
		return CameraState(
			QString("That spot now sits on %1. The eye moved around it and kept the distance. One undo step. "
							"render_preview.")
				.arg(frame));
	}

	if (!orbit.isEmpty() && (params.contains("around") || hasRange))
	{
		if (orbit != "left" && orbit != "right" && orbit != "up" && orbit != "down")
			return Fail("orbit must be left, right, up, or down.");
		if (hasStep && step > 180.0) return Fail("An orbit step is degrees, at most 180.");
		sSurfaceHit hit;
		QString error;
		const QJsonValue aroundRaw = hasRange ? rangePoint : params.value("around");
		if (!ResolveSpot(aroundRaw, &hit, &error)) return Fail(error);
		const CVector3 camera = gPar->Get<CVector3>("camera");
		const CVector3 target = gPar->Get<CVector3>("target");
		const CVector3 top = gPar->Get<CVector3>("camera_top");
		const cCameraTarget basis(camera, target, top);
		const bool horizontal = orbit == "left" || orbit == "right";
		const CVector3 axis = horizontal ? basis.GetTopVector() : basis.GetRightVector();
		const double sign = (orbit == "left" || orbit == "up") ? 1.0 : -1.0;
		const double radians = sign * (hasStep ? step : 15.0) * steps * M_PI / 180.0;
		OrbitAround(hit.position, axis, radians);
		commitIf();
		return CameraState(
			QString("Orbited %1 around that spot, the same idea as dragging the viewfinder. The spot stays "
							"in frame. One undo step. render_preview.")
				.arg(orbit));
	}

	if (toward || lookAtPoint || lookAtBool || rangeToward)
	{
		const QJsonValue raw = toward ? params.value("toward")
			: (lookAtPoint ? params.value("look_at") : rangePoint);
		CVector3 world;
		QString error;
		sSurfaceHit hit;
		bool haveHit = false;
		if (raw.isArray() && raw.toArray().size() == 3)
		{
			if (!ParseVec3(raw, &world, &error)) return Fail(error);
		}
		else if (raw.isArray() && raw.toArray().size() == 2 && raw.toArray().at(0).isDouble()
						 && raw.toArray().at(1).isDouble())
		{
			const double x = raw.toArray().at(0).toDouble();
			const double y = raw.toArray().at(1).toDouble();
			CVector3 direction;
			if (!ScreenPoint(x, y, &direction, &error)) return Fail(error);
			sProbeEngine engine;
			hit = MarchRay(&engine, direction);
			if (!hit.hit)
				return Fail("No surface along that ray. probe_view a nearby spot, or frame_scene if the "
										"picture is empty.");
			world = hit.position;
			haveHit = true;
		}
		else
		{
			return Fail(toward ? "toward must be [x, y]. 0, 0 is the top left. -1 to 2 reaches outside the picture."
												 : "look_at must be [x, y] in the picture, or a world [x, y, z].");
		}
		if (!lookAtPoint && !lookAtBool && !haveHit)
			return Fail("toward must be [x, y]. 0, 0 is the top left. -1 to 2 reaches outside the picture.");

		CVector3 camera = gPar->Get<CVector3>("camera");
		CVector3 target = gPar->Get<CVector3>("target");
		const CVector3 top = gPar->Get<CVector3>("camera_top");
		if (lookAtPoint || lookAtBool)
		{
			if ((world - camera).Length() < 1e-8) return Fail("look_at is on the camera.");
			WriteCamera(camera, world, top);
			commitIf();
			return CameraState(
				"The eye stayed put and the target is now that point. One undo step. render_preview.");
		}
		const double fraction = steps * (hasStep ? step : gPar->Get<double>("camera_movement_step"));
		const bool away = params.value("away").toBool(false);
		if (hit.depth < 1e-5)
			return Fail("The eye is inside the surface along that ray. Move backward or call frame_scene, then probe again.");
		double travel = hit.depth * fraction;
		if (!away && travel > hit.depth * 0.99) travel = hit.depth * 0.99;
		if (away) travel = -travel;
		camera += hit.direction * travel;
		target = world;
		WriteCamera(camera, target, top);
		commitIf();
		return CameraState(away
				? "Backed away along that ray and kept looking at the surface. One undo step. "
					"render_preview."
				: "Moved toward that spot, stopping short of the surface, and looked at it. The step is "
					"a fraction of the depth, the same scale as a navigation click. One undo step. "
					"render_preview.");
	}

	QString button;
	const bool rotating = !turn.isEmpty() || !orbit.isEmpty();
	const QString which = rotating ? (turn.isEmpty() ? orbit : turn) : move;
	if (!rotating)
	{
		if (which == "forward") button = "bu_move_forward";
		else if (which == "backward" || which == "back") button = "bu_move_backward";
		else if (which == "left") button = "bu_move_left";
		else if (which == "right") button = "bu_move_right";
		else if (which == "up") button = "bu_move_up";
		else if (which == "down") button = "bu_move_down";
		else
			return Fail("move must be forward, backward, left, right, up, or down.");
	}
	else
	{
		if (which == "left") button = "bu_rotate_left";
		else if (which == "right") button = "bu_rotate_right";
		else if (which == "up") button = "bu_rotate_up";
		else if (which == "down") button = "bu_rotate_down";
		else if (turn == "roll_left" || turn == "roll left") button = "bu_rotate_roll_left";
		else if (turn == "roll_right" || turn == "roll right") button = "bu_rotate_roll_right";
		else
			return Fail(orbit.isEmpty()
					? "turn must be left, right, up, down, roll_left, or roll_right."
					: "orbit must be left, right, up, or down.");
	}
	if (hasStep && rotating && step > 180.0) return Fail("A turn step is degrees, at most 180.");

	using namespace cameraMovementEnums;
	const int oldDistanceMode = gPar->Get<int>("camera_absolute_distance_mode");
	const int oldMovement = gPar->Get<int>("camera_movement_mode");
	const int oldRotation = gPar->Get<int>("camera_rotation_mode");
	const double oldMoveStep = gPar->Get<double>("camera_movement_step");
	const double oldTurnStep = gPar->Get<double>("camera_rotation_step");
	if (!rotating)
	{
		gPar->Set("camera_absolute_distance_mode", int(relative));
		gPar->Set("camera_movement_mode", int(fixedDistance));
		if (hasStep) gPar->Set("camera_movement_step", step);
	}
	else
	{
		gPar->Set("camera_rotation_mode", int(orbit.isEmpty() ? rotateCamera : rotateAroundTarget));
		if (hasStep) gPar->Set("camera_rotation_step", step);
	}
	cManipulations manipulations;
	manipulations.AssignParameterContainers(gPar, gParFractal);
	for (int i = 0; i < steps; i++)
	{
		if (rotating)
			manipulations.RotateCamera(button);
		else
			manipulations.MoveCamera(button);
	}
	gPar->Set("camera_absolute_distance_mode", oldDistanceMode);
	gPar->Set("camera_movement_mode", oldMovement);
	gPar->Set("camera_rotation_mode", oldRotation);
	gPar->Set("camera_movement_step", oldMoveStep);
	gPar->Set("camera_rotation_step", oldTurnStep);
	commitIf();
	const QString note = rotating
		? QString(orbit.isEmpty()
				? "Looked %1 by the navigation buttons' rotation step. The eye stayed put. One undo step. "
					"render_preview."
				: "Orbited the target %1, the same math as the navigation buttons with rotation set to "
					"around the target. One undo step. render_preview.")
				.arg(which)
		: QString("Moved %1. The eye and the target moved together, so the view direction stayed. "
							"One step is camera_movement_step (default 0.5) times the distance from the eye "
							"to the surface. One undo step. render_preview.")
				.arg(which);
	return CameraState(note);
}

QJsonObject PlaceInView(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	const QString kind = params.value("kind").toString();
	if (kind != "light" && kind != "object" && kind != "focus" && kind != "fog")
		return Fail("kind must be light, object, focus, or fog.");
	const bool hasPosition = params.contains("position");
	const bool hasScreen = params.contains("x") || params.contains("y");
	if (hasPosition == hasScreen)
		return Fail("Pass x and y in the picture, or position from probe_view, not both.");
	if (params.contains("offset") && !params.value("offset").isDouble())
		return Fail("offset must be a number.");
	if (params.contains("offset") && params.value("offset").toDouble() < 0.0)
		return Fail("offset is how far toward the camera. Use behind true to put a light past the surface.");
	const bool behind = params.value("behind").toBool(false);
	if (behind && kind != "light") return Fail("behind is for a light.");
	const QString role = params.value("role").toString();
	if (!role.isEmpty() && kind != "light") return Fail("role is for a light.");
	if (!role.isEmpty() && role != "key" && role != "fill" && role != "rim" && role != "camera"
			&& role != "pair")
		return Fail("role must be key, fill, rim, camera, or pair.");
	if (behind && !role.isEmpty() && role != "rim")
		return Fail("behind is the rim along the view ray. Use role rim, or leave role out.");

	CVector3 direction;
	sSurfaceHit hit;
	QString error;
	if (hasScreen)
	{
		if (!params.value("x").isDouble() || !params.value("y").isDouble())
			return Fail("x and y must be numbers from 0 to 1.");
		if (!ScreenPoint(params.value("x").toDouble(), params.value("y").toDouble(), &direction, &error))
			return Fail(error);
		sProbeEngine engine;
		hit = MarchRay(&engine, direction);
		if (!hit.hit)
			return Fail("No surface at that spot. probe_view first, or pick a sample whose hit is true.");
		if (hit.depth < 1e-5)
			return Fail("The eye is inside the surface at that spot. Move backward or call frame_scene, then probe again.");
	}
	else
	{
		if (!ParseVec3(params.value("position"), &hit.position, &error)) return Fail(error);
		const CVector3 camera = gPar->Get<CVector3>("camera");
		direction = hit.position - camera;
		hit.depth = direction.Length();
		if (!(hit.depth > 1e-8)) return Fail("position is on the camera.");
		direction.Normalize();
		hit.direction = direction;
		hit.hit = true;
	}

	const bool hasOffset = params.contains("offset");
	const double offset = hasOffset ? params.value("offset").toDouble() : hit.depth * (kind == "light" ? 0.08 : 0.0);
	CVector3 point = hit.position - direction * offset;

	if (kind == "focus")
	{
		gPar->Set("DOF_focus", hit.depth);
		Remember();
		QJsonObject result;
		result["depth"] = hit.depth;
		result["DOF_focus"] = hit.depth;
		result["note"] =
			"Sharp focus is now the distance to that spot. DOF_enabled is unchanged; previews turn "
			"depth of field off on a copy. One undo step.";
		return result;
	}
	if (kind == "fog")
	{
		gPar->Set("basic_fog_visibility", hit.depth);
		Remember();
		QJsonObject result;
		result["depth"] = hit.depth;
		result["basic_fog_visibility"] = hit.depth;
		result["note"] =
			"Basic fog becomes opaque at that depth, the same as a viewfinder fog click. Several "
			"times this distance is a light veil. One undo step.";
		return result;
	}
	if (kind == "object")
	{
		const int nodeId = params.value("id").toInt(params.value("node_id").toInt(0));
		QList<cObjectsTree::sNodeListItem> nodes = Nodes();
		if (!FindNode(&nodes, nodeId)) return Fail("Pass id of a node from get_objects_tree.");
		const QString parameter = NodePrefix(nodeId) + "position";
		if (!gPar->IfExists(parameter)) return Fail("That node has no position.");
		cObjectsTree objectsTree;
		objectsTree.CreateNodeDataFromParameters(gPar);
		const CVector3 local = objectsTree.GetParentWorldMatrix(nodeId).InverseAffine().TransformPoint(point);
		gPar->Set(parameter, local);
		Remember();
		QJsonObject result;
		result["id"] = nodeId;
		result["position"] = Vec3(local);
		result["world"] = Vec3(point);
		result["depth"] = hit.depth;
		result["note"] =
			"The node origin is on that surface, stored in the parent node's coordinates, the same "
			"conversion as a viewfinder click. A sphere is centered there, so half of it sits inside "
			"the shape. Pass offset to lift it toward the camera. One undo step. render_preview.";
		return result;
	}

	const int id = params.value("id").toInt(1);
	if (id < 1 || id > 16) return Fail("Light id must be 1 to 16.");
	if (role == "pair" && id + 1 > 16) return Fail("A pair needs id and the next light, so id must be 15 or less.");
	if (hit.normal.Length() < 1e-8 && hasPosition)
	{
		QString ignored;
		ResolveSpot(params.value("position"), &hit, &ignored);
	}
	const CVector3 eye = gPar->Get<CVector3>("camera");
	const cCameraTarget basis(eye, gPar->Get<CVector3>("target"), gPar->Get<CVector3>("camera_top"));
	CVector3 normal = hit.normal;
	if (normal.Length() < 1e-8)
		normal = direction * (-1.0);
	else
		normal.Normalize();
	const double gap = hasOffset ? offset : qMax(0.25 * hit.depth, 1e-3);
	const bool useRim = behind || role == "rim";
	const bool useCamera = role == "camera";
	auto aim = [&](const CVector3 &bias) {
		CVector3 placed = normal + bias;
		if (placed.Length() < 1e-8) placed = normal;
		placed.Normalize();
		return hit.position + placed * gap;
	};
	if (useRim)
	{
		const double search = hasOffset ? qMax(offset, hit.depth * 0.05) : qMax(0.25 * hit.depth, 1e-3);
		const double resolution = 1.0 / double(qMax(1, gPar->Get<int>("image_height")));
		const double distanceLimit = gPar->Get<double>("view_distance_max");
		const double behindDistance = traceBehindFractal(
			gPar, gParFractal, search, direction, hit.depth, resolution, distanceLimit);
		point = hit.position + direction * behindDistance;
	}
	else if (role == "key" || role == "pair")
	{
		point = aim(basis.GetTopVector() * 0.55 + basis.GetRightVector() * 0.85);
	}
	else if (role == "fill")
	{
		point = aim(basis.GetTopVector() * 0.15 + basis.GetRightVector() * (-0.85));
	}
	else if (!useCamera)
	{
		point = hit.position - direction * (hasOffset ? offset : gap * 0.3);
	}

	auto writeLight = [&](int lightId, const CVector3 &where, bool relative, double scale) {
		EnsureLight(lightId);
		const double clearance =
			relative ? hit.depth : cInterface::GetDistanceForPoint(where, gPar, gParFractal);
		const double reach = qMax(clearance, 0.25 * hit.depth);
		double intensity = reach * reach * scale;
		if (!(intensity >= 0.25 * scale)) intensity = 0.25 * scale;
		const QString prefix = QString("light%1_").arg(lightId);
		if (gPar->IfExists(prefix + "position")) gPar->Set(prefix + "position", where);
		if (gPar->IfExists(prefix + "relative_position")) gPar->Set(prefix + "relative_position", relative);
		if (gPar->IfExists(prefix + "enabled")) gPar->Set(prefix + "enabled", true);
		if (gPar->IfExists(prefix + "intensity")) gPar->Set(prefix + "intensity", intensity);
		return intensity;
	};

	double intensity = 0;
	QJsonArray placed;
	if (useCamera)
	{
		const CVector3 relative(gap * 0.8, gap * 0.55, gap * 0.25);
		intensity = writeLight(id, relative, true, 1.0);
		placed.append(QJsonObject{{"light", id}, {"position", Vec3(relative)}, {"relative", true},
			{"intensity", intensity}, {"role", "camera"}});
	}
	else
	{
		const double scale = role == "fill" ? 0.35 : 1.0;
		intensity = writeLight(id, point, false, scale);
		placed.append(QJsonObject{{"light", id}, {"position", Vec3(point)}, {"relative", false},
			{"intensity", intensity}, {"role", role.isEmpty() ? QString(useRim ? "rim" : "front") : role}});
		if (role == "pair")
		{
			const CVector3 fillPoint = aim(basis.GetTopVector() * 0.15 + basis.GetRightVector() * (-0.85));
			const double fillIntensity = writeLight(id + 1, fillPoint, false, 0.35);
			placed.append(QJsonObject{{"light", id + 1}, {"position", Vec3(fillPoint)}, {"relative", false},
				{"intensity", fillIntensity}, {"role", "fill"}});
		}
	}
	Remember();
	QJsonObject result;
	result["light"] = id;
	result["lights"] = placed;
	result["position"] = useCamera ? Vec3(CVector3(gap * 0.8, gap * 0.55, gap * 0.25)) : Vec3(point);
	result["depth"] = hit.depth;
	result["intensity"] = intensity;
	result["behind"] = useRim;
	result["relative"] = useCamera;
	result["note"] = useCamera
		? "The light stays on the camera, up and to the right. position is camera space: x right, y up, "
			"z forward. Intensity is based on a quarter of the depth, so a draft can see it. One undo "
			"step. render_preview."
		: useRim
			? "The light is past the surface along the view ray, so this spot is rim-lit. Intensity uses "
				"at least a quarter of the depth instead of the tiny gap, so it shows up in a draft. One "
				"undo step. render_preview."
			: role == "pair"
				? "Light id is the key, up and camera-right of the surface normal. The next light is a "
					"dimmer fill on the other side. One undo step. render_preview."
				: "The light is off the surface along the normal, biased for this role. Intensity uses at "
					"least a quarter of the depth so a light tucked against the shape still reads. One undo "
					"step. render_preview.";
	return result;
}

QJsonObject MeasureView(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	if (!params.contains("a") || !params.contains("b"))
		return Fail("Pass a and b. Each is [x, y] in the picture, or a world [x, y, z].");
	sSurfaceHit first;
	sSurfaceHit second;
	QString error;
	if (!ResolveSpot(params.value("a"), &first, &error)) return Fail(error);
	if (!ResolveSpot(params.value("b"), &second, &error)) return Fail(error);
	const double distance = (first.position - second.position).Length();
	const CVector3 midpoint = (first.position + second.position) * 0.5;
	QJsonObject result;
	result["distance"] = distance;
	result["midpoint"] = Vec3(midpoint);
	result["a"] = Vec3(first.position);
	result["b"] = Vec3(second.position);
	if (first.nodeId >= 0) result["a_node_id"] = first.nodeId;
	if (second.nodeId >= 0) result["b_node_id"] = second.nodeId;
	result["note"] =
		"distance is the straight line between the two surface points. Use it as a sphere radius "
		"with fit_to_view, or pass diameter true there when the two points are opposite edges.";
	return result;
}

bool SetPrimitiveSize(int nodeId, double radius, QString *what, QString *error)
{
	QList<cObjectsTree::sNodeListItem> nodes = Nodes();
	const cObjectsTree::sNodeListItem *node = FindNode(&nodes, nodeId);
	if (!node)
	{
		*error = "Pass id of a node from get_objects_tree.";
		return false;
	}
	for (const sPrimitiveItem &primitive : cPrimitives::GetListOfPrimitives(gPar))
	{
		if (!gPar->IfExists(primitive.Name("object_id"))) continue;
		if (gPar->Get<int>(primitive.Name("object_id")) != node->objectId) continue;
		if (gPar->IfExists(primitive.Name("radius")))
		{
			gPar->Set(primitive.Name("radius"), radius);
			*what = primitive.Name("radius");
			return true;
		}
		if (gPar->IfExists(primitive.Name("size")))
		{
			gPar->Set(primitive.Name("size"), radius);
			*what = primitive.Name("size");
			return true;
		}
	}
	const QString scale = NodePrefix(nodeId) + "scale";
	if (gPar->IfExists(scale))
	{
		gPar->Set(scale, radius);
		*what = scale;
		return true;
	}
	*error = "That node has no radius, size, or scale.";
	return false;
}

void ApplyLimits(const CVector3 &minimum, const CVector3 &maximum)
{
	if (gMainInterface)
		gMainInterface->SetBoundingBoxAsLimits(minimum, maximum, gPar, gParFractal);
	else
	{
		gPar->Set("limits_enabled", true);
		gPar->Set("limit_min", minimum);
		gPar->Set("limit_max", maximum);
	}
}

QJsonObject FitToView(const QJsonObject &params)
{
	if (!gPar || !gParFractal) return Fail("No session.");
	const QString kind = params.value("kind").toString();
	if (kind != "radius" && kind != "limits") return Fail("kind must be radius or limits.");
	const double padding = params.value("padding").toDouble(1.2);
	if (!(padding > 0.0) || padding > 10.0) return Fail("padding must be greater than 0 and at most 10.");
	if (kind == "radius")
	{
		if (!params.contains("a") || !params.contains("b"))
			return Fail("Pass a and b, the two spots the size should span.");
		sSurfaceHit first;
		sSurfaceHit second;
		QString error;
		if (!ResolveSpot(params.value("a"), &first, &error)) return Fail(error);
		if (!ResolveSpot(params.value("b"), &second, &error)) return Fail(error);
		double radius = (first.position - second.position).Length();
		if (params.value("diameter").toBool(false)) radius *= 0.5;
		if (!(radius > 0.0)) return Fail("Those two spots are the same point.");
		const int nodeId = params.value("id").toInt(0);
		QString written;
		if (!SetPrimitiveSize(nodeId, radius, &written, &error)) return Fail(error);
		Remember();
		QJsonObject result;
		result["id"] = nodeId;
		result["radius"] = radius;
		result["parameter"] = written;
		result["distance"] = (first.position - second.position).Length();
		result["note"] = params.value("diameter").toBool(false)
			? "The radius is half the distance between the two spots. One undo step. render_preview."
			: "The radius is the distance between the two spots. Pass diameter true when they are "
				"opposite edges. One undo step. render_preview.";
		return result;
	}
	CVector3 minimum;
	CVector3 maximum;
	if (params.contains("a") && params.contains("b"))
	{
		sSurfaceHit first;
		sSurfaceHit second;
		QString error;
		if (!ResolveSpot(params.value("a"), &first, &error)) return Fail(error);
		if (!ResolveSpot(params.value("b"), &second, &error)) return Fail(error);
		const CVector3 lo(qMin(first.position.x, second.position.x), qMin(first.position.y, second.position.y),
			qMin(first.position.z, second.position.z));
		const CVector3 hi(qMax(first.position.x, second.position.x), qMax(first.position.y, second.position.y),
			qMax(first.position.z, second.position.z));
		CVector3 half = (hi - lo) * (0.5 * padding);
		const double span = half.Length();
		if (!(span > 1e-8)) half = CVector3(padding, padding, padding) * 0.01;
		const CVector3 center = (lo + hi) * 0.5;
		minimum = center - half;
		maximum = center + half;
	}
	else if (params.contains("x") || params.contains("position") || params.contains("at"))
	{
		QJsonValue raw;
		if (params.contains("position"))
			raw = params.value("position");
		else if (params.contains("x"))
			raw = QJsonArray{params.value("x"), params.value("y")};
		else
		{
			double x = 0.5;
			double y = 0.5;
			QString error;
			if (!NamedPoint(params.value("at").toString(), &x, &y, &error)) return Fail(error);
			raw = QJsonArray{x, y};
		}
		sSurfaceHit hit;
		QString error;
		if (!ResolveSpot(raw, &hit, &error)) return Fail(error);
		const double reach = qMax(hit.depth, 1e-3) * padding;
		minimum = hit.position - CVector3(reach, reach, reach);
		maximum = hit.position + CVector3(reach, reach, reach);
	}
	else
	{
		return Fail("Pass x and y, position, or a and b, so the limits know which feature to wrap.");
	}
	ApplyLimits(minimum, maximum);
	gPar->Set("limits_enabled", true);
	Remember();
	QJsonObject result;
	result["limit_min"] = Vec3(gPar->Get<CVector3>("limit_min"));
	result["limit_max"] = Vec3(gPar->Get<CVector3>("limit_max"));
	result["limits_enabled"] = true;
	result["note"] =
		"Limits wrap the feature, using the same bounding-box scan as the viewfinder click when the "
		"window is open. One undo step. render_preview.";
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

bool AnimationRenderRunning()
{
	QMutexLocker locker(&gJobsMutex);
	for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
	{
		const std::shared_ptr<sJob> job = it.value();
		if (!job || job->done.load()) continue;
		if (job->kind == QLatin1String("keyframe") || job->kind == QLatin1String("flight")) return true;
	}
	return false;
}

int gAnimationEditDepth = 0;

struct sAnimationGuard
{
	bool ok = false;
	QString error;
	sAnimationGuard()
	{
		if (AnimationRenderRunning())
		{
			error =
				"An animation is rendering. Wait for it, or stop_render, before editing keyframes or the flight path.";
			return;
		}
		if (gAnimationEditDepth > 0)
		{
			error = "An animation edit is already in progress.";
			return;
		}
		gAnimationEditDepth++;
		ok = true;
	}
	~sAnimationGuard()
	{
		if (ok) gAnimationEditDepth--;
	}
};

QJsonObject AddKeyframe(const QJsonObject &params)
{
	const sAnimationGuard guard;
	if (!guard.ok) return Fail(guard.error);
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
	const sAnimationGuard guard;
	if (!guard.ok) return Fail(guard.error);
	const int index = params.value("index").toInt(-1);
	if (index < 0 || index >= gKeyframes->GetNumberOfFrames())
		return Fail("index must be an existing key.");
	cAnimationFrames::sAnimationFrame frame = gKeyframes->GetFrame(index);
	const bool hasFrames = params.contains("frames_per_keyframe");
	const bool hasCamera = params.contains("camera");
	const bool hasTarget = params.contains("target");
	const bool hasRotation = params.contains("rotation");
	const bool hasFov = params.contains("fov");
	const bool hasDistance = params.contains("distance");
	const bool poseFields = hasCamera || hasTarget || hasRotation || hasFov || hasDistance;
	if (hasFrames)
	{
		const int between = params.value("frames_per_keyframe").toInt(0);
		if (between < 1 || between > 10000) return Fail("frames_per_keyframe must be 1 to 10000.");
		frame.numberOfSubFrames = between;
	}
	auto copyLive = [&](const QString &name) {
		const QString full = "main_" + name;
		if (!frame.parameters.IfExists(full) || !gPar->IfExists(name)) return;
		cOneParameter value = gPar->GetAsOneParameter(name);
		value.SetMorphType(frame.parameters.GetAsOneParameter(full).GetMorphType());
		frame.parameters.SetFromOneParameter(full, value);
	};
	auto writeVec = [&](const QString &full, const CVector3 &value) {
		if (!frame.parameters.IfExists(full)) return;
		cOneParameter stored = frame.parameters.GetAsOneParameter(full);
		stored.Set(value, valueActual);
		frame.parameters.SetFromOneParameter(full, stored);
	};
	if (!hasFrames && !poseFields)
	{
		const QStringList names = {"camera", "target", "camera_rotation", "fov", "camera_distance_to_target"};
		for (const QString &name : names)
			copyLive(name);
	}
	else if (poseFields)
	{
		QString error;
		if (hasCamera)
		{
			CVector3 camera;
			if (!ParseVec3(params.value("camera"), &camera, &error)) return Fail(error);
			writeVec("main_camera", camera);
		}
		if (hasTarget)
		{
			CVector3 target;
			if (!ParseVec3(params.value("target"), &target, &error)) return Fail(error);
			writeVec("main_target", target);
		}
		if (hasRotation)
		{
			CVector3 rotation;
			if (!ParseVec3(params.value("rotation"), &rotation, &error)) return Fail(error);
			writeVec("main_camera_rotation", rotation);
		}
		if (hasFov && frame.parameters.IfExists("main_fov"))
		{
			cOneParameter stored = frame.parameters.GetAsOneParameter("main_fov");
			stored.Set(params.value("fov").toDouble(), valueActual);
			frame.parameters.SetFromOneParameter("main_fov", stored);
		}
		if (hasDistance && frame.parameters.IfExists("main_camera_distance_to_target"))
		{
			cOneParameter stored = frame.parameters.GetAsOneParameter("main_camera_distance_to_target");
			stored.Set(params.value("distance").toDouble(), valueActual);
			frame.parameters.SetFromOneParameter("main_camera_distance_to_target", stored);
		}
	}
	gKeyframes->ModifyFrame(index, frame);
	gKeyframes->UpdateFramesIndexesTable();
	Remember();
	QJsonObject result = AnimationState();
	result["updated"] = index;
	result["key"] = KeyframeJson(index);
	if (hasFrames && !poseFields)
		result["note"] =
			"Only frames_per_keyframe changed. Camera, target, rotation, fov, and distance on this key were left as stored.";
	return result;
}

QJsonObject DeleteKeyframe(const QJsonObject &params)
{
	const sAnimationGuard guard;
	if (!guard.ok) return Fail(guard.error);
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
	const sAnimationGuard guard;
	if (!guard.ok) return Fail(guard.error);
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
	if (kind == "orbit")
	{
		const double sweep = params.value("sweep").toDouble(360.0);
		if (!(sweep > 0.0) || sweep > 360.0)
			return Fail("sweep is degrees of arc, from just above 0 to 360. 360 is a full circle.");
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
		const double sweep = params.value("sweep").toDouble(360.0);
		const bool fullCircle = sweep >= 359.999;
		const double span = sweep * M_PI / 180.0;
		for (int i = 0; i < count; i++)
		{
			const double angle = fullCircle ? 2.0 * M_PI * double(i) / double(count)
																			 : span * double(i) / double(qMax(1, count - 1));
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
	if (kind == "orbit") result["sweep"] = params.value("sweep").toDouble(360.0);
	result["note"] =
		"This replaced the flight path. It did not change keyframes. Render it with "
		"render_animation mode flight. Orbit looks at one target. sweep is degrees of that arc "
		"and defaults to 360, a full circle. A shorter sweep starts at the current side of the "
		"target and does not repeat the first frame. A fly-through moves the eye in a straight "
		"line; keep the target on the shape or the frame goes empty.";
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

	bool mandelbox = false;
	{
		QList<int> drawnSlots = mcpScene::DrawnFormulaSlots(gPar);
		if (drawnSlots.isEmpty())
		{
			for (int i = 0; i < gParFractal->size(); i++)
				drawnSlots.append(i + 1);
		}
		for (int slot : drawnSlots)
		{
			if (slot < 1 || slot > gParFractal->size()) continue;
			const int formulaId = gParFractal->at(slot - 1)->Get<int>("formula");
			for (cAbstractFractal *formula : newFractalList)
			{
				if (int(formula->getInternalId()) != formulaId) continue;
				const QString internal = QString::fromStdString(formula->getInternalName()).toLower();
				if (internal.contains("mandelbox") || internal.contains("abox") || internal.contains("boxfold"))
					mandelbox = true;
				break;
			}
		}
	}
	const QString boxHint = mandelbox
		? QString(" This formula is a Mandelbox. If the picture is empty, set mandelbox_scale to -1.5 "
							"(the stored default of 2 is a solid the default camera sits inside).")
		: QString();

	const int rays = qBound(8, params.value("rays").toInt(36), 100);
	auto scanShell = [&](double limit, double *maxReliable, int *reliable) {
		*maxReliable = 0.0;
		*reliable = 0;
		for (int i = 0; i < rays; i++)
		{
			CVector3 direction(Random(1000) / 500.0 - 1.0, Random(1000) / 500.0 - 1.0, Random(1000) / 500.0 - 1.0);
			if (direction.Length() < 1e-8) continue;
			direction.Normalize();
			double distStep = 1.0;
			double scan = limit;
			bool first = true;
			bool capped = false;
			for (; scan > 0.0; scan -= distStep)
			{
				const CVector3 point = lookAt + direction * scan;
				sDistanceIn in(point, 0, false);
				sDistanceOut out;
				const double dist =
					CalculateDistance(*renderParams, fractals, in, &out, &renderData, &objectStack);
				// A NaN distance would otherwise spin. Treat it as a hit and stop.
				if (!(dist > 0.1))
				{
					if (first) capped = true;
					break;
				}
				first = false;
				distStep = dist * deFactor * 0.5;
				if (distStep > 1.0) distStep = 1.0;
				if (distStep < 1e-3) distStep = 1e-3;
			}
			// A hit on the first sample is the scan limit, which for pseudo Kleinian is the
			// bailout sphere rather than the cluster.
			if (capped || scan > limit * 0.92) continue;
			if (scan > *maxReliable) *maxReliable = scan;
			(*reliable)++;
		}
	};

	// Outside in. A wide shell can report a halo well inside the bailout, so keep
	// going and use the tightest shell that still finds a surface. Stop once a
	// tighter shell finds nothing: the eye is then inside the shape the outer
	// shell measured.
	const double shells[] = {100.0, 24.0, 8.0, 3.0};
	double maxDist = 0.0;
	bool found = false;
	for (double limit : shells)
	{
		double reliable = 0.0;
		int count = 0;
		scanShell(limit, &reliable, &count);
		if (count == 0)
		{
			if (found) break;
			continue;
		}
		maxDist = reliable;
		found = true;
	}

	const CVector3 currentCamera = gPar->Get<CVector3>("camera");
	if (!found)
	{
		QJsonObject result;
		result["camera"] = QJsonArray{currentCamera.x, currentCamera.y, currentCamera.z};
		result["target"] = QJsonArray{gPar->Get<CVector3>("target").x, gPar->Get<CVector3>("target").y,
			gPar->Get<CVector3>("target").z};
		result["distance"] = gPar->Get<double>("camera_distance_to_target");
		result["measured_radius"] = shells[0];
		result["measurement_unreliable"] = true;
		result["camera_moved"] = false;
		result["note"] =
			"Measurement looks like the escape sphere. The scan hit its outer limit before a closer "
			"surface, so the camera was not moved. Place the camera a few units from the target and "
			"preview."
			+ boxHint;
		return result;
	}

	if (maxDist < 0.05)
	{
		return Fail(
			"Could not find a surface within the scan. The formula may be empty, the scale may have "
			"collapsed the shape, or it sits far from the target. Pass target where the shape should "
			"be, then call frame_scene again."
			+ boxHint);
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
	result["measurement_unreliable"] = false;
	result["camera_moved"] = true;
	result["note"] =
		"The camera was pulled back until the measured surface fits the frame, looking at the "
		"target. The viewing direction was kept. One undo step. The formula was not changed. "
		"render_preview to check."
		+ boxHint;
	return result;
}

QJsonObject RenderStill(const QJsonObject &params, bool waitForResult)
{
	QString output = params.value("path").toString();
	QString pictureName;
	if (params.contains("name"))
	{
		QString nameError;
		pictureName = PictureName(params.value("name").toString(), &nameError);
		if (pictureName.isEmpty()) return Fail(nameError);
	}
	if (output.isEmpty())
	{
		output = QDir(QDir::tempPath())
							 .filePath(pictureName.isEmpty()
													 ? QString("mandelbulber-still-%1-%2")
															 .arg(QCoreApplication::applicationPid())
															 .arg(gJobSerial + 1)
													 : pictureName);
	}
	else
	{
		const bool directory = output.endsWith('/') || output.endsWith('\\') || QFileInfo(output).isDir();
		if (!pictureName.isEmpty())
		{
			const QString folder = directory ? output : QFileInfo(StripPng(output)).absolutePath();
			output = QDir(folder).filePath(pictureName);
		}
		else
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
	const bool fullGrid = params.value("grid").toString() == QLatin1String("full");
	Launch(job, waitForResult ? "preview" : "still", [](std::shared_ptr<sJob> live) { RunStill(live); });
	if (!waitForResult)
	{
		QJsonObject result = JobStatus(job, fullGrid);
		result["note"] =
			"Full stills wait in the main queue and run one at a time. Previews and draft or better "
			"jobs wait in the agent queue, which keeps running while a main render is in progress. "
			"status queued means this job has not started. Poll render_status with this id. Stopping "
			"it from the Background renders window reports status cancelled and cancelled_by_user true. "
			"The live session was not changed; this job used a copy.";
		return result;
	}
	QJsonObject result = WaitFor(job, fullGrid);
	if (result.value("ok").toBool(true) == false && result.contains("error")
		&& result.value("status").toString() != "finished")
		return result;
	result["path"] = job->imagePath;
	result["width"] = width;
	result["height"] = height;
	QString qualityNote;
	if (job->quality == QLatin1String("draft"))
		qualityNote =
			"Draft preview from a copy. The session's resolution and expensive effects were not changed. "
			"The window stayed responsive while this ran.";
	else if (job->quality == QLatin1String("better"))
		qualityNote =
			"Better preview from a copy. Detail is capped at 1. The session's resolution and expensive "
			"effects were not changed. The window stayed responsive while this ran.";
	else
		qualityNote =
			"Still render from a copy of the session. Quality and effects are the session's. The window "
			"stayed responsive while this ran.";
	if (job->cameraInside)
	{
		qualityNote +=
			" The eye is inside the shape (camera_inside). Pull the camera back before a longer render.";
		result["camera_inside"] = true;
	}
	result["note"] = qualityNote;
	Q_UNUSED(draft);
	return result;
}

QJsonObject RenderAnimation(const QJsonObject &params)
{
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
		"unchanged. quality draft caps detail and turns the expensive effects off. A full animation waits "
		"in the main queue and runs one at a time. Draft and better animations wait in the agent queue, "
		"which runs beside the main queue. Stopping one from the Background renders window reports "
		"status cancelled and cancelled_by_user true.";
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

QJsonObject CheckpointJson(const renderCheckpoint::sSummary &item)
{
	QJsonObject object;
	object["source"] = item.source;
	object["kind"] = item.kind;
	object["width"] = item.width;
	object["height"] = item.height;
	object["done_lines"] = item.doneLines;
	object["animation"] = item.animation;
	if (item.animation)
	{
		object["frame"] = item.animFrame;
		object["end"] = item.animEnd;
	}
	if (!item.output.isEmpty()) object["output"] = item.output;
	return object;
}

QJsonArray CheckpointArray()
{
	QJsonArray list;
	const QList<renderCheckpoint::sSummary> found = renderCheckpoint::List();
	for (const renderCheckpoint::sSummary &item : found)
		list.append(CheckpointJson(item));
	return list;
}

QJsonObject ResumeRender(const QJsonObject &params)
{
	const bool accept = params.value("resume").toBool(true);
	QString source = params.value("source").toString();
	if (!source.isEmpty() && source != QLatin1String("background") && source != QLatin1String("window"))
		return Fail("source must be background or window.");

	const QString dialogSource = gMainInterface ? gMainInterface->OpenResumeDialogSource() : QString();
	if (source.isEmpty()) source = dialogSource;
	const QList<renderCheckpoint::sSummary> found = renderCheckpoint::List();
	if (source.isEmpty())
	{
		if (found.size() == 1)
			source = found.first().source;
		else if (found.isEmpty())
			return Fail("No interrupted render to resume.");
		else
			return Fail("More than one checkpoint. Pass source background or window.");
	}
	if (!dialogSource.isEmpty() && source != dialogSource)
	{
		return Fail(QString("A resume dialog is open for the %1 render. Answer that one first.")
									.arg(dialogSource));
	}
	if (gMainInterface && gMainInterface->AnswerResumeDialog(source, accept))
	{
		QJsonObject result;
		result["source"] = source;
		result["accepted"] = accept;
		result["note"] = accept
			? "Accepted the resume dialog. The render starts as that dialog closes. Poll render_status."
			: "Declined the resume dialog. The checkpoint is discarded.";
		return result;
	}

	bool known = false;
	for (const renderCheckpoint::sSummary &item : found)
	{
		if (item.source == source) known = true;
	}
	if (!known) return Fail(QString("No %1 checkpoint to resume.").arg(source));

	if (!accept)
	{
		renderCheckpoint::Discard(source);
		QJsonObject result;
		result["source"] = source;
		result["discarded"] = true;
		result["note"] = "The checkpoint was discarded.";
		return result;
	}

	if (source == QLatin1String("background"))
	{
		if (!mcpScene::ResumeBackground())
		{
			return Fail("The background render could not be resumed. A render may already be running, "
									"or the checkpoint could not be read.");
		}
		QJsonObject result;
		const std::shared_ptr<sJob> job = gJobs.value(gActiveJobId);
		if (job) result = JobStatus(job);
		result["resumed"] = true;
		result["source"] = source;
		result["note"] =
			"Resumed the interrupted background render from its last finished scanline. Poll render_status.";
		return result;
	}

	if (!gMainInterface || !gMainInterface->StartWindowResume())
	{
		return Fail("The window render could not be resumed. A render may already be running, "
								"or the checkpoint could not be read.");
	}
	QJsonObject result = CheckpointJson(renderCheckpoint::Summary(QString("window")));
	result["resumed"] = true;
	result["source"] = source;
	result["note"] =
		"The window render is resuming from its last finished scanline. It uses the open view, not a "
		"background job.";
	return result;
}

QJsonObject NewScene()
{
	if (!gPar || !gParFractal) return Fail("No session.");
	const QString saveTarget = systemData.lastSettingsFile;
	const QStringList names = gPar->GetListOfParameters();
	for (const QString &name : names)
	{
		if (name.startsWith(QLatin1String("node_")) || name.startsWith(QLatin1String("primitive_")))
			gPar->DeleteParameter(name);
	}
	gPar->ResetAllToDefault();
	for (int i = 0; i < gParFractal->size(); i++)
		gParFractal->at(i)->ResetAllToDefault();
	int formulaId = 0;
	QString internal;
	QString display;
	if (!FindFormula(QString("mandelbulb"), &formulaId, &internal, &display))
		return Fail("Mandelbulb is not available in this build.");
	gParFractal->at(0)->Set("formula", formulaId);
	for (int i = 1; i < gParFractal->size(); i++)
		gParFractal->at(i)->Set("formula", int(fractal::none));
	WriteDefinition(1, internal.isEmpty() ? QString("mandelbulb") : internal, enumNodeType::fractal, 0, 1, 1);
	if (gAnimFrames) gAnimFrames->ClearAll();
	if (gKeyframes) gKeyframes->ClearAll();
	systemData.lastSettingsFile = saveTarget;
	Remember();
	QJsonObject result = TreeResult();
	result["loaded_file"] = saveTarget;
	result["save_target_changed"] = false;
	result["note"] =
		"One Mandelbulb at the world, the default camera, and an empty flight path and key list. "
		"One undo step. No file was written. The window's Save target was left unchanged.";
	return result;
}

QMutex gUndoActionMutex;
QString gPendingUndoAction;
QStringList gUndoActions;
QStringList gRedoActions;
} // namespace

namespace mcpScene
{
void NoteUndoAction(const QString &action)
{
	QMutexLocker locker(&gUndoActionMutex);
	gPendingUndoAction = action;
}

void CommitUndoAction()
{
	QMutexLocker locker(&gUndoActionMutex);
	if (gPendingUndoAction.isEmpty()) return;
	gUndoActions.append(gPendingUndoAction);
	gPendingUndoAction.clear();
	gRedoActions.clear();
}

void ClearPendingUndoAction()
{
	QMutexLocker locker(&gUndoActionMutex);
	gPendingUndoAction.clear();
}

QString TakeUndoneAction()
{
	QMutexLocker locker(&gUndoActionMutex);
	const QString action = gUndoActions.isEmpty() ? QString("edit") : gUndoActions.takeLast();
	gRedoActions.append(action);
	return action;
}

QString TakeRedoneAction()
{
	QMutexLocker locker(&gUndoActionMutex);
	const QString action = gRedoActions.isEmpty() ? QString("edit") : gRedoActions.takeLast();
	gUndoActions.append(action);
	return action;
}

QList<int> DrawnFormulaSlots(const std::shared_ptr<cParameterContainer> &par)
{
	QList<int> drawn;
	if (!par) return drawn;
	cObjectsTree tree;
	const QList<cObjectsTree::sNodeListItem> nodes = tree.GetListOfNodes(par);
	QHash<int, int> parentOf;
	QHash<int, bool> enabled;
	for (const cObjectsTree::sNodeListItem &node : nodes)
	{
		parentOf.insert(node.id, node.parentId);
		const QString prefix = QString("node_%1_").arg(node.id, 4, 10, QChar('0'));
		bool on = true;
		if (par->IfExists(prefix + "enabled")) on = par->Get<bool>(prefix + "enabled");
		enabled.insert(node.id, on);
	}
	auto live = [&](int id) {
		int guard = 0;
		while (id != 0 && guard++ < 1000)
		{
			if (enabled.contains(id) && !enabled.value(id)) return false;
			if (!parentOf.contains(id)) break;
			id = parentOf.value(id);
		}
		return true;
	};
	QSet<int> seen;
	for (const cObjectsTree::sNodeListItem &node : nodes)
	{
		if (node.type != enumNodeType::fractal) continue;
		if (node.objectId < 1) continue;
		if (!live(node.id)) continue;
		if (seen.contains(node.objectId)) continue;
		seen.insert(node.objectId);
		drawn.append(node.objectId);
	}
	return drawn;
}

bool JobActive()
{
	QMutexLocker locker(&gJobsMutex);
	for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
	{
		const std::shared_ptr<sJob> job = it.value();
		if (job && job->lane == QLatin1String("agent") && job->started && !job->done.load()) return true;
	}
	return false;
}

int RunningJobCount()
{
	return CountRunningJobs();
}

QList<sBackgroundRender> BackgroundRenders()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	QList<std::shared_ptr<sJob>> jobs;
	{
		QMutexLocker locker(&gJobsMutex);
		for (auto it = gJobs.constBegin(); it != gJobs.constEnd(); ++it)
			if (it.value()) jobs.append(it.value());
	}
	QList<sBackgroundRender> list;
	for (const std::shared_ptr<sJob> &job : jobs)
	{
		QMutexLocker locker(&job->mutex);
		const bool running = job->started && !job->done.load();
		const bool queued = !job->started && !job->done.load();
		if (!running && !queued && (job->finishedAt <= 0 || now - job->finishedAt > 12000)) continue;
		sBackgroundRender view;
		view.id = job->id;
		view.title = job->title.isEmpty() ? job->id : job->title;
		view.summary = job->summary;
		view.message = job->message;
		view.status = job->status;
		view.lane = job->lane;
		view.thumbnail = job->thumbnail;
		view.thumbGeneration = job->thumbGeneration;
		view.progress = job->progress;
		view.running = running;
		view.queued = queued;
		view.stopping = (running || queued) && job->stop;
		view.cancelledByUser = job->cancelledByUser;
		list.append(view);
	}
	std::sort(list.begin(), list.end(), [](const sBackgroundRender &a, const sBackgroundRender &b) {
		return a.id.mid(4).toInt() < b.id.mid(4).toInt();
	});
	return list;
}

bool CancelBackgroundRender(const QString &id)
{
	std::shared_ptr<sJob> job;
	{
		QMutexLocker locker(&gJobsMutex);
		job = gJobs.value(id);
	}
	if (!job || job->done.load()) return false;
	bool finishedNow = false;
	{
		QMutexLocker locker(&job->mutex);
		if (job->done.load()) return false;
		job->cancelledByUser = true;
		job->stopBeforeExecute = true;
		job->stop = true;
		if (!job->started)
		{
			job->status = QString("cancelled");
			job->error = QString("The user cancelled this render.");
			job->finishedAt = QDateTime::currentMSecsSinceEpoch();
			job->done.store(true);
			finishedNow = true;
		}
	}
	if (finishedNow) PumpQueues();
	return true;
}

QJsonArray Checkpoints()
{
	return CheckpointArray();
}

bool Dispatch(const QString &method, const QJsonObject &params, QJsonObject *result)
{
	if (method == "get_objects_tree") *result = ObjectsTree();
	else if (method == "new_scene") *result = NewScene();
	else if (method == "add_object") *result = AddObject(params);
	else if (method == "update_object") *result = UpdateObject(params);
	else if (method == "delete_object") *result = DeleteObject(params);
	else if (method == "set_material") *result = SetMaterial(params);
	else if (method == "set_light") *result = SetLight(params);
	else if (method == "set_camera") *result = SetCamera(params);
	else if (method == "probe_view") *result = ProbeView(params);
	else if (method == "look_around") *result = LookAround(params);
	else if (method == "navigate_camera") *result = NavigateCamera(params);
	else if (method == "place_in_view") *result = PlaceInView(params);
	else if (method == "measure_view") *result = MeasureView(params);
	else if (method == "fit_to_view") *result = FitToView(params);
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
			*result = JobStatus(job, params.value("grid").toString() == QLatin1String("full"));
	}
	else if (method == "resume_render") *result = ResumeRender(params);
	else if (method == "stop_render")
	{
		const QString requested = params.value("id").toString();
		std::shared_ptr<sJob> job;
		if (!requested.isEmpty())
			job = gJobs.value(requested);
		else if (!gActiveJobId.isEmpty())
			job = gJobs.value(gActiveJobId);
		if (!requested.isEmpty() && !job)
		{
			*result = Fail("No render job with that id.");
			return true;
		}
		const bool windowRendering =
			gMainInterface && gMainInterface->mainImage && gMainInterface->mainImage->IsUsed();
		if (windowRendering && requested.isEmpty()) gMainInterface->stopRequest = true;
		QJsonObject object;
		object["stopping"] = true;
		if (job)
		{
			QMutexLocker locker(&job->mutex);
			object["id"] = job->id;
			if (!job->done.load())
			{
				job->stop = true;
				job->stopBeforeExecute = true;
			}
			if (job->cancelledByUser)
			{
				object["cancelled_by_user"] = true;
				object["status"] = job->done.load() ? job->status : QString("cancelled");
				object["ok"] = false;
				object["error"] = "The user cancelled this render.";
				object["note"] =
					"The user cancelled this render from the Background renders window. That stop was "
					"intentional.";
			}
		}
		if (!object.contains("note"))
		{
			object["note"] =
				"The stop flag is set. There is no confirmation dialog. render_status changes to stopped "
				"or finished when the current tile or frame notices. A stop from the Background renders "
				"window is status cancelled, with cancelled_by_user true.";
		}
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

bool ResumeBackground()
{
	const renderCheckpoint::sSummary summary = renderCheckpoint::Summary(QString("background"));
	if (!summary.valid) return false;

	const renderCheckpoint::sAnimation animation =
		renderCheckpoint::ReadAnimation(QString("background"));
	if (animation.valid)
	{
		auto par = std::make_shared<cParameterContainer>(*gPar);
		auto fractal = std::make_shared<cFractalContainer>(*gParFractal);
		auto flight = std::make_shared<cAnimationFrames>(*gAnimFrames);
		auto keyframes = std::make_shared<cKeyframes>(*gKeyframes);
		if (!renderCheckpoint::LoadAnimation(QString("background"), par, fractal, flight, keyframes))
			return false;
		auto job = std::make_shared<sJob>();
		job->par = par;
		job->fractal = fractal;
		job->flight = flight;
		job->keyframes = keyframes;
		job->width = animation.width;
		job->height = animation.height;
		job->outputDirectory = animation.directory;
		job->frames = animation.finished;
		job->frameCount = animation.end - animation.start;
		job->wantContactSheet = animation.contactSheet;
		job->frameEdge = animation.frameEdge;
		job->columns = animation.columns;
		job->draft = false;
		job->anim = true;
		QDir().mkpath(animation.directory);
		renderCheckpoint::Arm(QString("background"), animation.frameOutput);
		const QString mode = animation.mode;
		const int frame = animation.frame;
		const int end = animation.end;
		Launch(job, mode, [mode, frame, end](std::shared_ptr<sJob> live) {
			RunAnimation(live, mode, frame, end);
		});
		return true;
	}

	auto par = std::make_shared<cParameterContainer>(*gPar);
	auto fractal = std::make_shared<cFractalContainer>(*gParFractal);
	if (!renderCheckpoint::LoadSettings(QString("background"), par, fractal)) return false;

	auto job = std::make_shared<sJob>();
	job->par = par;
	job->fractal = fractal;
	job->width = par->Get<int>("image_width");
	job->height = par->Get<int>("image_height");
	job->outputDirectory = summary.output;
	if (job->outputDirectory.isEmpty())
	{
		job->outputDirectory = QDir(QDir::tempPath())
														 .filePath(QString("mandelbulber-still-%1-resume")
																					 .arg(QCoreApplication::applicationPid()));
	}
	job->draft = false;
	job->frameCount = 1;
	QDir().mkpath(QFileInfo(job->outputDirectory).absolutePath());
	renderCheckpoint::Arm(QString("background"), job->outputDirectory);
	Launch(job, QString("still"), [](std::shared_ptr<sJob> live) { RunStill(live); });
	return true;
}

QString QueueWindowStill(QString *error)
{
	if (!gMainInterface || !gPar || !gParFractal)
	{
		if (error) *error = QObject::tr("The window is not ready.");
		return QString();
	}
	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);
	auto job = std::make_shared<sJob>();
	QJsonObject params;
	params["quality"] = QString("full");
	QString copyError;
	if (!PrepareCopy(params, job, &copyError))
	{
		if (error) *error = copyError;
		return QString();
	}
	cScripts::EvaluateAll(job->par, job->fractal);
	int temporaryScale = 1;
	if (gPar->IfExists("temporary_scale"))
		temporaryScale = int(std::pow(2.0, gPar->Get<int>("temporary_scale")));
	if (temporaryScale > 1)
	{
		job->width = qMax(16, job->width / temporaryScale);
		job->height = qMax(16, job->height / temporaryScale);
		job->par->Set("image_width", job->width);
		job->par->Set("image_height", job->height);
		if (job->par->IfExists("detail_level"))
			job->par->Set("detail_level", job->par->Get<double>("detail_level") * temporaryScale);
	}
	QString folder = gPar->Get<QString>("default_image_path");
	if (folder.isEmpty()) folder = systemDirectories.GetImagesFolder();
	QDir().mkpath(folder);
	QString base = QFileInfo(systemData.lastSettingsFile).completeBaseName();
	base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
	if (base.isEmpty()) base = QStringLiteral("render");
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	QString path = QDir(folder).filePath(QString("%1-%2").arg(base, stamp));
	int suffix = 2;
	while (QFileInfo::exists(path + QStringLiteral(".png")))
		path = QDir(folder).filePath(QString("%1-%2-%3").arg(base, stamp).arg(suffix++));
	job->outputDirectory = path;
	job->frameCount = 1;
	job->fromWindow = true;
	job->quality = QString("full");
	Launch(job, QString("still"), [](std::shared_ptr<sJob> live) { RunStill(live); });
	return job->id;
}

void SetPreviewWatch(const QString &id, bool watch)
{
	std::shared_ptr<sJob> job;
	{
		QMutexLocker locker(&gJobsMutex);
		job = gJobs.value(id);
	}
	if (!job) return;
	QMutexLocker locker(&job->mutex);
	job->previewWatchers += watch ? 1 : -1;
	if (job->previewWatchers < 0) job->previewWatchers = 0;
	if (job->previewWatchers == 0 && job->done.load()) job->liveImage.reset();
}

sJobView SampleJobView(
	const QString &id, int srcX, int srcY, int srcW, int srcH, int dstW, int dstH)
{
	sJobView view;
	std::shared_ptr<sJob> job;
	{
		QMutexLocker locker(&gJobsMutex);
		job = gJobs.value(id);
	}
	if (!job) return view;
	view.found = true;
	std::shared_ptr<cImage> image;
	QImage saved;
	QString imagePath;
	bool finalPass = false;
	{
		QMutexLocker locker(&job->mutex);
		view.title = job->title;
		view.progress = job->progress;
		view.running = job->started && !job->done.load();
		image = job->liveImage;
		saved = job->savedFrame;
		imagePath = job->imagePath;
		finalPass = job->done.load() && job->status == QLatin1String("finished");
		if (image)
		{
			view.width = int(image->GetWidth());
			view.height = int(image->GetHeight());
		}
	}
	if (!image && saved.isNull() && !imagePath.isEmpty())
	{
		saved = QImage(imagePath).convertToFormat(QImage::Format_RGB32);
		if (!saved.isNull())
		{
			QMutexLocker locker(&job->mutex);
			job->savedFrame = saved;
			view.width = saved.width();
			view.height = saved.height();
		}
	}
	else if (!saved.isNull())
	{
		view.width = saved.width();
		view.height = saved.height();
	}
	dstW = qBound(1, dstW, 4096);
	dstH = qBound(1, dstH, 4096);
	if (view.width < 2 || view.height < 2 || srcW < 1 || srcH < 1) return view;
	QImage frame(dstW, dstH, QImage::Format_RGB32);
	frame.fill(qRgb(24, 24, 28));
	const bool use16 = finalPass && image && !image->GetImage16().empty();
	for (int y = 0; y < dstH; y++)
	{
		QRgb *row = reinterpret_cast<QRgb *>(frame.scanLine(y));
		const int sourceY = srcY + int((qint64(y) * srcH) / dstH);
		for (int x = 0; x < dstW; x++)
		{
			const int sourceX = srcX + int((qint64(x) * srcW) / dstW);
			if (sourceX < 0 || sourceY < 0 || sourceX >= view.width || sourceY >= view.height) continue;
			int red;
			int green;
			int blue;
			if (!saved.isNull())
			{
				const QRgb pixel = reinterpret_cast<const QRgb *>(saved.constScanLine(sourceY))[sourceX];
				red = qRed(pixel);
				green = qGreen(pixel);
				blue = qBlue(pixel);
			}
			else if (use16)
			{
				const sRGB16 pixel = image->GetPixelImage16(quint64(sourceX), quint64(sourceY));
				red = pixel.R >> 8;
				green = pixel.G >> 8;
				blue = pixel.B >> 8;
			}
			else if (image)
			{
				const sRGBFloat pixel = image->GetPixelImage(quint64(sourceX), quint64(sourceY));
				red = ByteFromLinear(pixel.R);
				green = ByteFromLinear(pixel.G);
				blue = ByteFromLinear(pixel.B);
			}
			else
				continue;
			row[x] = qRgb(red, green, blue);
		}
	}
	view.image = frame;
	return view;
}
} // namespace mcpScene
