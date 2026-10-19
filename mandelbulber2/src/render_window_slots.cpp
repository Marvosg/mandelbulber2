/**
 * Mandelbulber v2, a 3D fractal generator       ,=#MKNmMMKmmßMNWy,
 *                                             ,B" ]L,,p%%%,,,§;, "K
 * Copyright (C) 2016-24 Mandelbulber Team     §R-==%w["'~5]m%=L.=~5N
 *                                        ,=mm=§M ]=4 yJKA"/-Nsaj  "Bw,==,,
 * This file is part of Mandelbulber.    §R.r= jw",M  Km .mM  FW ",§=ß., ,TN
 *                                     ,4R =%["w[N=7]J '"5=],""]]M,w,-; T=]M
 * Mandelbulber is free software:     §R.ß~-Q/M=,=5"v"]=Qf,'§"M= =,M.§ Rz]M"Kw
 * you can redistribute it and/or     §w "xDY.J ' -"m=====WeC=\ ""%""y=%"]"" §
 * modify it under the terms of the    "§M=M =D=4"N #"%==A%p M§ M6  R' #"=~.4M
 * GNU General Public License as        §W =, ][T"]C  §  § '§ e===~ U  !§[Z ]N
 * published by the                    4M",,Jm=,"=e~  §  §  j]]""N  BmM"py=ßM
 * Free Software Foundation,          ]§ T,M=& 'YmMMpM9MMM%=w=,,=MT]M m§;'§,
 * either version 3 of the License,    TWw [.j"5=~N[=§%=%W,T ]R,"=="Y[LFT ]N
 * or (at your option)                   TW=,-#"%=;[  =Q:["V""  ],,M.m == ]N
 * any later version.                      J§"mr"] ,=,," =="""J]= M"M"]==ß"
 *                                          §= "=C=4 §"eM "=B:m|4"]#F,§~
 * Mandelbulber is distributed in            "9w=,,]w em%wJ '"~" ,=,,ß"
 * the hope that it will be useful,                 . "K=  ,=RMMMßM"""
 * but WITHOUT ANY WARRANTY;                            .'''
 * without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with Mandelbulber. If not, see <http://www.gnu.org/licenses/>.
 *
 * ###########################################################################
 *
 * Authors: Krzysztof Marczak (buddhi1980@gmail.com)
 *
 * RenderWindow class - main program window
 *
 * This file contains implementation of the miscellaneous slots in RenderWindow
 * class. See also header render_window.hpp and whole implementation of class
 * spread over render_window_*.cpp
 */

#include "ui_render_window.h"

#include "animation_flight.hpp"
#include "animation_frames.hpp"
#include "animation_keyframes.hpp"
#include "cimage.hpp"
#include "fractal_container.hpp"
#include "global_data.hpp"
#include "image_scale.hpp"
#include "initparameters.hpp"
#include "interface.hpp"
#include "keyframes.hpp"
#include "manipulations.h"
#include "material_item_model.h"
#include "render_window.hpp"
#include "settings.hpp"
#include "shortcuts.h"
#include "system_data.hpp"
#include "system_directories.hpp"
#include "write_log.hpp"

#include "qt/detached_window.h"
#include "qt/material_editor.h"
#include "qt/mesh_export_dialog.h"
#include "qt/preferences_dialog.h"
#include "qt/preset_card.h"
#include "qt/thumbnail_widget.h"
#include "qt/voxel_export_dialog.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QTimer>

namespace {
// A fitted preview is the size of the view with no bars. If bars are already
// showing, that size is larger than the current viewport and Qt would keep the
// bars. Hide them before applying a size that fits, then restore AsNeeded.
void setMainPreviewWidgetSize(MyScrolledArea *area, QWidget *image, int width, int height)
{
	static bool busy = false;
	if (busy) return;
	busy = true;

	const QSize maxArea = area->maximumViewportSize();
	const bool fits = width <= maxArea.width() && height <= maxArea.height();
	if (fits)
	{
		area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	}
	image->setMinimumSize(width, height);
	area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	busy = false;
}
} // namespace

void RenderWindow::slotResizedScrolledAreaImage(int width, int height) const
{
	if (gMainInterface->mainImage)
	{
		int selectedScale = ui->comboBox_image_preview_scale->currentIndex();

		if (selectedScale == 0)
		{
			double scale = CalcMainImageScale(0.0, width, height, gMainInterface->mainImage);
			gMainInterface->mainImage->CreatePreview(scale, width, height, gMainInterface->renderedImage);
			gMainInterface->mainImage->UpdatePreview();
			const double dpiScale = devicePixelRatioF();
			setMainPreviewWidgetSize(ui->scrollAreaForImage, gMainInterface->renderedImage,
				int(gMainInterface->mainImage->GetPreviewWidth() / dpiScale),
				int(gMainInterface->mainImage->GetPreviewHeight() / dpiScale));
		}
	}
}

void RenderWindow::slotChangedComboImageScale(int index) const
{
	if (gMainInterface->mainImage)
	{
		double scale = ImageScaleComboSelection2Double(index);
		int areaWidth = ui->scrollAreaForImage->VisibleAreaWidth();
		int areaHeight = ui->scrollAreaForImage->VisibleAreaHeight();
		scale = CalcMainImageScale(scale, areaWidth, areaHeight, gMainInterface->mainImage);

		gMainInterface->mainImage->CreatePreview(
			scale, areaWidth, areaHeight, gMainInterface->renderedImage);
		gMainInterface->mainImage->UpdatePreview();

		double dpiScale = devicePixelRatioF();
		setMainPreviewWidgetSize(ui->scrollAreaForImage, gMainInterface->renderedImage,
			int(gMainInterface->mainImage->GetPreviewWidth() / dpiScale),
			int(gMainInterface->mainImage->GetPreviewHeight() / dpiScale));

		gPar->Set("image_preview_scale", index);
	}
}

void RenderWindow::slotMouseMovedOnImage(int x, int y)
{
	(void)x;
	(void)y;
	// qDebug() << "mouse move event";
	// CVector2<int> point(x, y);
}

void RenderWindow::slotMouseClickOnImage(int x, int y, Qt::MouseButton button) const
{
	int index = ui->comboBox_mouse_click_function->currentIndex();
	QList<QVariant> mode = ui->comboBox_mouse_click_function->itemData(index).toList();
	RenderedImage::enumClickMode clickMode = RenderedImage::enumClickMode(mode.at(0).toInt());

	switch (clickMode)
	{
		case RenderedImage::clickMoveCamera:
		case RenderedImage::clickFogVisibility:
		case RenderedImage::clickDOFFocus:
		case RenderedImage::clickPlaceLight:
		case RenderedImage::clickGetJuliaConstant:
		case RenderedImage::clickPlacePrimitive:
		case RenderedImage::clickPlaceObject:
		case RenderedImage::clickPlaceRandomLightCenter:
		case RenderedImage::clickGetPoint:
		case RenderedImage::clickWrapLimitsAroundObject:
		{
			if (gMainInterface->renderedImage->GetEnableClickModes())
			{
				manipulations->SetByMouse(CVector2<double>(x, y), button, mode);
			}
			break;
		}
		case RenderedImage::clickDoNothing:
		case RenderedImage::clickFlightSpeedControl:
			// nothing
			break;
	}
}

void RenderWindow::slotMouseDragStart(int x, int y, Qt::MouseButtons buttons)
{
	int index = ui->comboBox_mouse_click_function->currentIndex();
	QList<QVariant> mode = ui->comboBox_mouse_click_function->itemData(index).toList();
	RenderedImage::enumClickMode clickMode = RenderedImage::enumClickMode(mode.at(0).toInt());

	switch (clickMode)
	{
		case RenderedImage::clickMoveCamera:
		case RenderedImage::clickPlaceLight:
		case RenderedImage::clickPlacePrimitive:
		case RenderedImage::clickPlaceObject:
		{
			if (gMainInterface->renderedImage->GetEnableClickModes())
			{
				manipulations->MouseDragStart(CVector2<double>(x, y), buttons, mode);
			}
			break;
		}
		default:
			// nothing
			break;
	}
}

void RenderWindow::slotMouseDragFinish()
{
	manipulations->MouseDragFinish();
}

void RenderWindow::slotMouseDragDelta(int dx, int dy)
{
	manipulations->MouseDragDelta(dx, dy);
}

void RenderWindow::slotChangedComboMouseClickFunction(int index) const
{
	if (index >= 0) // if list is empty, then index = -1
	{
		QComboBox *comboBox = static_cast<QComboBox *>(sender());
		QList<QVariant> item = comboBox->itemData(index).toList();
		gMainInterface->renderedImage->setClickMode(item);
	}
}

void RenderWindow::slotKeyPressOnImage(QKeyEvent *event)
{
	currentKeyEvents.append(event->key());
	lastKeyEventModifiers = event->modifiers();
	slotKeyHandle();
	buttonPressTimer->start();
}

void RenderWindow::slotKeyReleaseOnImage(QKeyEvent *event)
{
	currentKeyEvents.removeOne(event->key());

	lastKeyEventModifiers = event->modifiers();
	slotKeyHandle();
	buttonPressTimer->stop();
}

void RenderWindow::slotButtonLongPress()
{
	slotKeyHandle();
}

void RenderWindow::slotKeyHandle()
{
	if (currentKeyEvents.size() == 0) return;

	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);

	bool render = false;

	for (int i = 0; i < currentKeyEvents.size(); i++)
	{
		enumShortcuts shortcut =
			cShortcuts::getShortcut(gPar.get(), currentKeyEvents.at(i), lastKeyEventModifiers);

		switch (shortcut)
		{
			case enumShortcuts::moveForward:
				manipulations->MoveCamera("bu_move_forward");
				render = true;
				break;
			case enumShortcuts::moveBackward:
				manipulations->MoveCamera("bu_move_backward");
				render = true;
				break;
			case enumShortcuts::moveLeft:
				manipulations->MoveCamera("bu_move_left");
				render = true;
				break;
			case enumShortcuts::moveRight:
				manipulations->MoveCamera("bu_move_right");
				render = true;
				break;
			case enumShortcuts::moveUp:
				manipulations->MoveCamera("bu_move_up");
				render = true;
				break;
			case enumShortcuts::moveDown:
				manipulations->MoveCamera("bu_move_down");
				render = true;
				break;
			case enumShortcuts::rotateLeft:
				manipulations->RotateCamera("bu_rotate_left");
				render = true;
				break;
			case enumShortcuts::rotateRight:
				manipulations->RotateCamera("bu_rotate_right");
				render = true;
				break;
			case enumShortcuts::rotateUp:
				manipulations->RotateCamera("bu_rotate_up");
				render = true;
				break;
			case enumShortcuts::rotateDown:
				manipulations->RotateCamera("bu_rotate_down");
				render = true;
				break;
			case enumShortcuts::rollLeft:
				manipulations->RotateCamera("bu_rotate_roll_left");
				render = true;
				break;
			case enumShortcuts::rollRight:
				manipulations->RotateCamera("bu_rotate_roll_right");
				render = true;
				break;
			case enumShortcuts::addKeyframe:
				currentKeyEvents.removeOne(currentKeyEvents.at(i)); // long press not allowed
				gKeyframeAnimation->slotAddKeyframe();
				break;
			case enumShortcuts::modifyKeyframe:
				currentKeyEvents.removeOne(currentKeyEvents.at(i)); // long press not allowed
				gKeyframeAnimation->slotModifyKeyframe();
				break;
			case enumShortcuts::render:
				currentKeyEvents.removeOne(currentKeyEvents.at(i)); // long press not allowed
				gMainInterface->StartRender();
				break;
			case enumShortcuts::stop:
				currentKeyEvents.removeOne(currentKeyEvents.at(i)); // long press not allowed
				gMainInterface->StopRender();
				break;

			default: break;
		}
	}

	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);

	if (render)
	{
		gMainInterface->StartRender();
	}
}

// global shortcuts
void RenderWindow::keyPressEvent(QKeyEvent *event)
{
	int key = event->key();
	Qt::KeyboardModifiers modifiers = event->modifiers();

	if (modifiers & Qt::ShiftModifier)
	{
		switch (key)
		{
			case Qt::Key_Escape: gMainInterface->GlobalStopRequest(); break;
			default: break;
		}
	}
}

void RenderWindow::slotMouseWheelRotatedWithKeyOnImage(
	int x, int y, int delta, Qt::KeyboardModifiers keyModifiers) const
{
	if (gMainInterface->renderedImage->GetEnableClickModes())
	{
		int index = ui->comboBox_mouse_click_function->currentIndex();
		QList<QVariant> mode = ui->comboBox_mouse_click_function->itemData(index).toList();
		RenderedImage::enumClickMode clickMode = RenderedImage::enumClickMode(mode.at(0).toInt());
		switch (clickMode)
		{
			case RenderedImage::clickPlaceLight:
			{
				if (keyModifiers & Qt::AltModifier)
				{
					double deltaLog = exp(delta * 0.001);
					double dist = ui->widgetEffects->GetAuxLightManualPlacementDistance();
					dist *= deltaLog;
					ui->widgetEffects->slotSetAuxLightManualPlacementDistance(dist);
				}
				else if (keyModifiers == Qt::NoModifier)
				{
					manipulations->MoveLightByWheel(delta);
				}
				break;
			}
			case RenderedImage::clickPlacePrimitive:
			{
				if (keyModifiers == Qt::NoModifier)
				{
					manipulations->MovePrimitiveByWheel(delta);
				}
				break;
			}
			case RenderedImage::clickPlaceObject:
			{
				if (keyModifiers == Qt::NoModifier)
				{
					manipulations->MoveObjectByWheel(delta, mode);
				}
				break;
			}
			case RenderedImage::clickMoveCamera:
			{
				Qt::MouseButton button = (delta > 0) ? Qt::LeftButton : Qt::RightButton;
				if (ui->toolButton_preciseRotation->isChecked())
				{
					delta /= 10.0;
				}
				mode.append(QVariant(delta));
				manipulations->SetByMouse(CVector2<double>(x, y), button, mode);
				break;
			}
			default: break;
		}
	}
}

void RenderWindow::slotChangedCheckBoxCursorVisibility(int state)
{
	gMainInterface->renderedImage->SetCursorVisibility(state);
}

QString RenderWindow::ToolbarPresetDirectory()
{
	if (gPar && gPar->IfExists("toolbar_path"))
	{
		const QString path = gPar->Get<QString>("toolbar_path");
		if (!path.isEmpty()) return path;
	}
	return systemDirectories.GetToolbarFolder();
}

QString RenderWindow::AgentPresetDirectory()
{
	return QDir(ToolbarPresetDirectory()).filePath(QStringLiteral("agent"));
}

bool RenderWindow::PresetFileIsAgent(const QString &path)
{
	const QString agentRoot = QDir::cleanPath(AgentPresetDirectory());
	const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
	if (clean == agentRoot || clean.startsWith(agentRoot + QLatin1Char('/'))) return true;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;
	const QString head = QString::fromUtf8(file.read(800));
	const QStringList lines = head.split(QLatin1Char('\n'));
	return lines.size() >= 3 && lines.at(2).contains(QStringLiteral("agent preset"));
}

QStringList RenderWindow::OrderedPresetFiles()
{
	struct sItem
	{
		QString path;
		QDateTime modified;
	};
	QList<sItem> user;
	QList<sItem> agent;
	auto take = [&](const QString &directory, bool forceAgent) {
		QDir dir(directory);
		const QStringList names = dir.entryList(QDir::Files);
		for (const QString &name : names)
		{
			const QString path = dir.filePath(name);
			if (QFileInfo(path).suffix().compare(QStringLiteral("fract"), Qt::CaseInsensitive) != 0)
				continue;
			sItem item{path, QFileInfo(path).lastModified()};
			if (forceAgent || PresetFileIsAgent(path))
				agent.append(item);
			else
				user.append(item);
		}
	};
	take(ToolbarPresetDirectory(), false);
	take(AgentPresetDirectory(), true);
	const auto newer = [](const sItem &a, const sItem &b) { return a.modified > b.modified; };
	std::sort(user.begin(), user.end(), newer);
	std::sort(agent.begin(), agent.end(), newer);
	QStringList paths;
	for (const sItem &item : user)
		paths.append(item.path);
	for (const sItem &item : agent)
		paths.append(item.path);
	return paths;
}

bool RenderWindow::MarkAgentPresetFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;
	const QString text = QString::fromUtf8(file.readAll());
	file.close();
	QStringList lines = text.split(QLatin1Char('\n'));
	if (lines.size() < 3) return false;
	if (!lines.at(2).contains(QStringLiteral("agent preset")))
		lines[2] += QStringLiteral(" agent preset");
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	const QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
	return file.write(bytes) == bytes.size();
}

QString RenderWindow::WaitForPresetThumbnail(const QString &filename, int timeoutMs)
{
	if (!presetStrip) return QString();
	cThumbnailWidget *thumb = presetStrip->thumbnailFor(filename);
	if (!thumb) return QString();
	auto cached = [thumb]() -> QString {
		if (!thumb->IsRendered()) return QString();
		const QString path = thumb->GetThumbnailFileName();
		return QFileInfo::exists(path) ? path : QString();
	};
	const QString ready = cached();
	if (!ready.isEmpty()) return ready;
	if (!thumb->CanRenderNow()) return QString();
	thumb->DisableTimer();
	QEventLoop loop;
	QTimer timer;
	timer.setSingleShot(true);
	connect(thumb, &cThumbnailWidget::thumbnailRendered, &loop, &QEventLoop::quit);
	connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	timer.start(qMax(1, timeoutMs));
	thumb->slotRender();
	if (!thumb->IsRendered()) loop.exec();
	return cached();
}

// adds dynamic actions to the toolbar (example settings)
void RenderWindow::slotPopulateToolbar(bool completeRefresh)
{
	static bool busy = false;
	if (busy) return;
	busy = true;

	WriteLog("cInterface::PopulateToolbar(QWidget *window, QToolBar *toolBar) started", 2);
	if (!presetStrip)
	{
		presetStrip = new cPresetStrip(ui->toolBar);
		ui->toolBar->setContentsMargins(0, 0, 0, 0);
		if (QLayout *layout = ui->toolBar->layout()) layout->setContentsMargins(0, 0, 0, 0);
		QWidgetAction *stripAction = new QWidgetAction(this);
		stripAction->setObjectName("presetStrip");
		stripAction->setDefaultWidget(presetStrip);
		ui->toolBar->addAction(stripAction);
		connect(presetStrip, &cPresetStrip::presetActivated, this, &RenderWindow::slotMenuLoadPreset);
		connect(
			presetStrip, &cPresetStrip::removeRequested, this, &RenderWindow::slotMenuRemovePreset);
		connect(presetStrip, &cPresetStrip::orderChanged, this, &RenderWindow::slotSavePresetOrder);

		QTimer *selectionTimer = new QTimer(this);
		selectionTimer->setInterval(250);
		connect(selectionTimer, &QTimer::timeout, this, &RenderWindow::slotUpdatePresetSelection);
		selectionTimer->start();
	}

	const QStringList toolbarFiles = OrderedPresetFiles();
	ui->toolBar->setIconSize(
		QSize(gPar->Get<int>("toolbar_icon_size"), gPar->Get<int>("toolbar_icon_size")));

	QList<QAction *> actions = ui->toolBar->actions();
	for (auto action : actions)
	{
		if (action->objectName() == "actionAdd_Settings_to_Toolbar") continue;
		if (action->objectName() == "presetStrip") continue;
		ui->toolBar->removeAction(action);
		action->deleteLater();
	}

	const QStringList alreadyShown = completeRefresh ? QStringList() : presetStrip->filenames();
	QList<cPresetStripItem> items;
	for (int i = 0; i < toolbarFiles.size(); i++)
	{
		if (systemData.globalStopRequest) break;

		const QString filename = toolbarFiles.at(i);
		const bool agent = PresetFileIsAgent(filename);
		if (alreadyShown.contains(filename))
		{
			items.append(cPresetStripItem{filename, nullptr, agent});
			continue;
		}

		cThumbnailWidget *thumbWidget = nullptr;
		if (QFileInfo(filename).suffix() == QString("fract"))
		{
			WriteLogString("Generating thumbnail for preset", filename, 2);
			cSettings parSettings(cSettings::formatFullText);
			parSettings.BeQuiet(true);

			if (parSettings.LoadFromFile(filename))
			{
				std::shared_ptr<cParameterContainer> par(new cParameterContainer);
				std::shared_ptr<cFractalContainer> parFractal(new cFractalContainer);
				par->SetContainerName("main");
				for (int j = 0; j < parFractal->size(); j++)
					parFractal->at(j)->SetContainerName(QString("fractal") + QString::number(j));
				InitParams(par);
				InitNodeParams(1, par);
				for (int j = 0; j < parFractal->size(); j++)
					InitFractalParams(parFractal->at(j));

				/****************** TEMPORARY CODE FOR MATERIALS *******************/

				InitMaterialParams(1, par);

				/*******************************************************************/

				if (parSettings.Decode(par, parFractal))
				{
					thumbWidget = new cThumbnailWidget(
						gPar->Get<int>("toolbar_icon_size"), gPar->Get<int>("toolbar_icon_size"), 2, this);
					// Leave image_width and image_height at the scene size. AssignParameters
					// scales detail_level by that size, so the small card resolves the same
					// surface as the viewport. Replacing them with the icon size first makes
					// the march stop early, and iteration fog then looks under-iterated.

					thumbWidget->UseOneCPUCore(true);
					par->Set("opencl_mode", gPar->Get<int>("opencl_mode"));
					par->Set("opencl_enabled", gPar->Get<bool>("opencl_enabled"));
					if (!par->Get<bool>("DOF_monte_carlo")) par->Set("opencl_mode", 0);
					if (!gPar->Get<bool>("thumbnails_with_opencl")) par->Set("opencl_enabled", false);
					thumbWidget->AssignParameters(par, parFractal);
				}
			}
		}

		if (thumbWidget) items.append(cPresetStripItem{filename, thumbWidget, agent});
		QApplication::processEvents();
	}

	presetStrip->sync(items, completeRefresh);
	slotUpdatePresetSelection();
	const int barHeight = qMax(presetStrip->height(), ui->toolBar->iconSize().height());
	ui->toolBar->setMinimumHeight(barHeight);
	ui->toolBar->updateGeometry();

	busy = false;
	WriteLog(
		"cInterface::PopulateToolbar(QWidget *window, QToolBar *toolBar) "
		"finished",
		2);
}

void RenderWindow::slotUpdatePresetSelection()
{
	if (!presetStrip || !gPar || !gParFractal || !gInterfaceReadyForSynchronization) return;
	// Hashing the whole scene here stalls the drag animation, which shares this thread.
	if (presetStrip->isDragging()) return;

	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);
	cSettings current(cSettings::formatCondensedText);
	current.BeQuiet(true);
	current.CreateText(gPar, gParFractal);
	const QString currentHash = current.GetHashCode();

	static QString cachedHash;
	static QStringList cachedFiles;
	static QStringList cachedMatches;
	const QStringList files = presetStrip->filenames();
	if (currentHash == cachedHash && files == cachedFiles)
	{
		presetStrip->setSelectedFiles(cachedMatches);
		return;
	}

	QStringList matched;
	for (const QString &filename : files)
	{
		cSettings preset(cSettings::formatFullText);
		preset.BeQuiet(true);
		if (!preset.LoadFromFile(filename)) continue;

		// Applying the file on top of the current scene changes nothing only when
		// every value stored in the preset already matches the scene.
		auto par = std::make_shared<cParameterContainer>(*gPar);
		auto fract = std::make_shared<cFractalContainer>(*gParFractal);
		if (!preset.Decode(par, fract)) continue;

		cSettings applied(cSettings::formatCondensedText);
		applied.BeQuiet(true);
		applied.CreateText(par, fract);
		if (applied.GetHashCode() == currentHash) matched.append(filename);
	}

	cachedHash = currentHash;
	cachedFiles = files;
	cachedMatches = matched;
	presetStrip->setSelectedFiles(matched);
}

void RenderWindow::slotSavePresetOrder(const QStringList &filenames)
{
	// QDir::Time lists newest first, which is the left-to-right toolbar order.
	const QDateTime newest = QDateTime::currentDateTime();
	for (int i = 0; i < filenames.size(); i++)
	{
		QFile file(filenames.at(i));
		if (!file.open(QIODevice::ReadWrite)) continue;
		file.setFileTime(newest.addSecs(-i), QFileDevice::FileModificationTime);
		file.close();
	}
}

void RenderWindow::slotPresetAddToToolbar()
{
	cSettings parSettings(cSettings::formatCondensedText);
	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);
	parSettings.CreateText(gPar, gParFractal, gAnimFrames, gKeyframes);
	QString filename =
		systemDirectories.GetToolbarFolder() + QDir::separator() + parSettings.GetHashCode() + ".fract";
	parSettings.SaveToFile(filename);
	slotPopulateToolbar();
}

void RenderWindow::slotMenuLoadPreset(QString filename)
{
	{
		// Leave the previous scene hash in place so the loaded preset counts as a change.
		cAutoRefreshPause autoRefreshPause(false);
		cSettings parSettings(cSettings::formatFullText);
		parSettings.LoadFromFile(filename);
		gInterfaceReadyForSynchronization = false;
		parSettings.Decode(gPar, gParFractal, gAnimFrames, gKeyframes);
		gMainInterface->materialListModel->Regenerate();
		ui->widgetEffects->RegenerateLights();

		gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);
		gInterfaceReadyForSynchronization = true;

		gMainInterface->ComboMouseClickUpdate();
		systemData.lastSettingsFile = gPar->Get<QString>("default_settings_path") + QDir::separator()
																	+ QFileInfo(filename).fileName();

		gFlightAnimation->RefreshTable();
		gKeyframeAnimation->RefreshTable();
		slotUpdatePresetSelection();
		showDescriptionPopup();
		setWindowTitle(QString("Mandelbulber (") + systemData.lastSettingsFile + ")");
	}
	if (gMainInterface) gMainInterface->RenderViewportIfAutoRefresh();
}

void RenderWindow::slotMenuRemovePreset(QString filename)
{
	QFile::remove(filename);
	slotPopulateToolbar();
}

// adds dynamic actions to the view > window states
void RenderWindow::slotPopulateCustomWindowStates(bool completeRefresh)
{
	WriteLog("cInterface::slotPopulateCustomWindowStates() started", 2);
	QDir customWindowStateDir = QDir(systemDirectories.GetCustomWindowStateFolder());
	customWindowStateDir.setSorting(QDir::Time);
	QStringList geometryFileExtension("*.geometry");
	customWindowStateDir.setNameFilters(geometryFileExtension);
	QStringList customWindowStateFiles =
		customWindowStateDir.entryList(QDir::NoDotAndDotDot | QDir::Files);
	QSignalMapper *mapCustomWindowLoad = new QSignalMapper(this);
	// QSignalMapper *mapCustomWindowRemove = new QSignalMapper(this);

	QList<QAction *> actions = ui->menuSaved_window_layouts->actions();
	QStringList customWindowActions;
	for (auto action : actions)
	{
		if (!action->objectName().startsWith("window_")) continue;
		if (!customWindowStateFiles.contains(action->objectName()) || completeRefresh)
		{
			// preset has been removed
			ui->menuSaved_window_layouts->removeAction(action);
		}
		else
		{
			customWindowActions << action->objectName();
		}
	}

	for (int i = 0; i < customWindowStateFiles.size(); i++)
	{
		QString customWindowStateGeometryFile = customWindowStateFiles.at(i);
		QString customWindowStateFile = customWindowStateGeometryFile.replace(".geometry", "");
		if (customWindowActions.contains("window_" + customWindowStateFile))
		{
			// already present
			continue;
		}
		QString filename =
			systemDirectories.GetCustomWindowStateFolder() + QDir::separator() + customWindowStateFile;

		/*
		QWidgetAction *action = new QWidgetAction(this);
		QLabel *buttonLoad = new QLabel;
		QHBoxLayout *tooltipLayout = new QHBoxLayout;
		QToolButton *buttonRemove = new QToolButton;
		QLabel *label = new QLabel;

		buttonLoad->setStyleSheet("QLabel{ border: none; margin: 2px; padding: 1px;
		}");
		label->setText(QByteArray().fromBase64(QByteArray().append(customWindowStateFile)));
		tooltipLayout->setContentsMargins(5, 0, 0, 0);
		QIcon iconDelete = QIcon::fromTheme("list-remove",
		QIcon(":system/icons/list-remove.svg")); buttonRemove->setIcon(iconDelete);
		buttonRemove->setMaximumSize(QSize(15, 15));
		tooltipLayout->addWidget(buttonRemove);
		tooltipLayout->addWidget(label);
		buttonLoad->setLayout(tooltipLayout);
		// FIXME - when minimum width is not specified the widget is very small
		buttonLoad->setMinimumWidth(300);
		action->setDefaultWidget(buttonLoad);*/

		QAction *action = new QAction(this);
		action->setText(QByteArray().fromBase64(QByteArray().append(customWindowStateFile.toUtf8())));
		action->setObjectName("window_" + customWindowStateFile);

		ui->menuSaved_window_layouts->addAction(action);

		mapCustomWindowLoad->setMapping(action, filename);
		QApplication::connect(action, SIGNAL(triggered()), mapCustomWindowLoad, SLOT(map()));
		QApplication::processEvents();
	}

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
	QApplication::connect(mapCustomWindowLoad, SIGNAL(mapped(QString)), this,
		SLOT(slotMenuLoadCustomWindowState(QString)));
#else
	QApplication::connect(mapCustomWindowLoad, SIGNAL(mappedString(QString)), this,
		SLOT(slotMenuLoadCustomWindowState(QString)));
#endif

	/*QApplication::connect(mapCustomWindowRemove, SIGNAL(mapped(QString)), this,
	SLOT(slotMenuRemoveCustomWindowState(QString)));*/

	WriteLog("cInterface::slotPopulateCustomWindowStates() finished", 2);
}

void RenderWindow::slotCustomWindowStateAddToMenu()
{
	bool ok;
	QString text = QInputDialog::getText(this, tr("Add window settings"),
		tr("Enter a name for the new window settings"), QLineEdit::Normal, "", &ok);
	if (!ok || text.isEmpty())
	{
		WriteLogCout("Cancelled window saving", 2);
		return;
	}
	QString textEncoded = QByteArray().append(text.toUtf8()).toBase64();
	QString basePath = systemDirectories.GetCustomWindowStateFolder() + QDir::separator();
	QString filename = basePath + textEncoded;
	QString filenameGeometry = filename + ".geometry";
	QString filenameState = filename + ".state";
	QFile fileGeometry(filenameGeometry);
	QFile fileState(filenameState);
	if (!fileGeometry.open(QIODevice::WriteOnly) || !fileState.open(QIODevice::WriteOnly))
	{
		qWarning() << "Could not open output files: " << filename << ".[geometry,state]";
		return;
	}
	fileGeometry.write(saveGeometry());
	fileState.write(saveState());
	fileGeometry.close();
	fileState.close();

	slotPopulateCustomWindowStates();
}

void RenderWindow::slotMenuLoadCustomWindowState(QString filename)
{
	QFile fileGeometry(filename + ".geometry");
	QFile fileState(filename + ".state");
	if (!fileGeometry.open(QIODevice::ReadOnly) || !fileState.open(QIODevice::ReadOnly))
	{
		qWarning() << "Could not open input files: " << filename << ".[geometry,state]";
		return;
	}
	// restoreGeometry(fileGeometry.readAll());
	restoreState(fileState.readAll());
}

void RenderWindow::slotCustomWindowRemovePopup()
{
	QDir customWindowStateDir = QDir(systemDirectories.GetCustomWindowStateFolder());
	customWindowStateDir.setSorting(QDir::Time);
	QStringList geometryFileExtension("*.geometry");
	customWindowStateDir.setNameFilters(geometryFileExtension);
	QStringList customWindowStateFiles =
		customWindowStateDir.entryList(QDir::NoDotAndDotDot | QDir::Files);
	QStringList itemsEscaped;
	if (customWindowStateFiles.size() == 0)
	{
		WriteLogCout("Nothing to remove", 2);
		return;
	}
	for (int i = 0; i < customWindowStateFiles.size(); i++)
	{
		QString customWindowStateFile = customWindowStateFiles[i].replace(".geometry", "");
		itemsEscaped.append(
			QByteArray().fromBase64(QByteArray().append(customWindowStateFile.toUtf8())));
	}
	bool ok;

	QString itemEscaped = QInputDialog::getItem(this, tr("Remove window settings"),
		tr("Select window setting to remove"), itemsEscaped, 0, false, &ok);
	if (!ok || itemEscaped.isEmpty())
	{
		WriteLogCout("Cancelled window removing", 2);
		return;
	}
	int index = itemsEscaped.indexOf(itemEscaped);
	QString filename = systemDirectories.GetCustomWindowStateFolder() + QDir::separator()
										 + customWindowStateFiles.at(index);
	slotMenuRemoveCustomWindowState(filename.replace(".geometry", ""));
}

void RenderWindow::slotMenuRemoveCustomWindowState(QString filename)
{
	QFile::remove(filename + ".geometry");
	QFile::remove(filename + ".state");
	slotPopulateCustomWindowStates();
}

void RenderWindow::slotPopulateRecentSettings(bool completeRefresh)
{
	WriteLog("cInterface::slotPopulateRecentSettings() started", 2);
	QFile recentFilesFile(systemDirectories.GetRecentFilesListFile());
	if (!recentFilesFile.open(QFile::ReadOnly | QFile::Text))
	{
		// qDebug() << "cannot open recent file";
		return;
	}
	QTextStream in(&recentFilesFile);
	QString recentFilesFileContent = in.readAll();

#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
	QStringList recentFiles =
		recentFilesFileContent.split(QRegExp("\n|\r\n|\r"), QString::KeepEmptyParts);
#else
	QStringList recentFiles =
		recentFilesFileContent.split(QRegularExpression("\n|\r\n|\r"), Qt::KeepEmptyParts);
#endif

	QSignalMapper *mapRecentFileLoad = new QSignalMapper(this);
	QList<QAction *> actions = ui->menuRecent_Settings_list->actions();
	QStringList recentFileInActions;
	for (auto action : actions)
	{
		if (!action->objectName().startsWith("recent_")) continue;
		if (!recentFileInActions.contains(action->objectName()) || completeRefresh)
		{
			// preset has been removed
			ui->menuRecent_Settings_list->removeAction(action);
		}
		else
		{
			recentFileInActions << action->objectName();
		}
	}

	for (int i = 0; i < recentFiles.size(); i++)
	{
		QString settingsFile = recentFiles.at(i);
		if (recentFileInActions.contains("recent_" + settingsFile))
		{
			// already present
			continue;
		}

		QAction *action = new QAction(this);
		action->setText(QFileInfo(settingsFile).baseName());
		action->setObjectName("recent_" + settingsFile);

		ui->menuRecent_Settings_list->addAction(action);

		mapRecentFileLoad->setMapping(action, settingsFile);
		QApplication::connect(action, SIGNAL(triggered()), mapRecentFileLoad, SLOT(map()));
		QApplication::processEvents();
	}

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
	QApplication::connect(
		mapRecentFileLoad, SIGNAL(mapped(QString)), this, SLOT(slotMenuLoadSettingsFromFile(QString)));
#else
	QApplication::connect(mapRecentFileLoad, SIGNAL(mappedString(QString)), this,
		SLOT(slotMenuLoadSettingsFromFile(QString)));
#endif

	WriteLog("cInterface::slotPopulateRecentSettings() finished", 2);
}

void RenderWindow::slotQuit()
{
	gMainInterface->QuitApplicationDialog();
}

void RenderWindow::closeEvent(QCloseEvent *event)
{
	if (gMainInterface->QuitApplicationDialog())
	{
		event->accept();
	}
	else
	{
		event->ignore();
	}
}

void RenderWindow::changeEvent(QEvent *event)
{
	if (event->type() == QEvent::LanguageChange)
	{
		// retranslate designer form (single inheritance approach)
		// ui->retranslateUi(this);
	}

	// remember to call base class implementation
	QMainWindow::changeEvent(event);
}

void RenderWindow::slotUpdateProgressAndStatus(const QString &text, const QString &progressText,
	double progress, cProgressText::enumProgressType progressType) const
{
	ui->statusbar->showMessage(text, 0);
	MyProgressBar *progressBar = nullptr;
	bool isQueue = false;

	// FIXME: sender can be deleted by another thread (e.g. ImageFileSave exists
	// very short time)
	//  if (sender())
	//  {
	//     isQueue = sender()->objectName() == "Queue";
	//  }

	switch (progressType)
	{
		case cProgressText::progress_IMAGE: progressBar = gMainInterface->progressBar; break;
		case cProgressText::progress_ANIMATION:
			progressBar = gMainInterface->progressBarAnimation;
			break;
		case cProgressText::progress_QUEUE:
			// nothing to be done, no progress bar for queue in GUI
			break;
	}

	if (progressBar)
	{
		if (!progressBar->isVisible())
		{
			progressBar->setVisible(true);
		}
		progressBar->setValue(progress * 1000.0);
		progressBar->setTextVisible(true);
		progressBar->setFormat(progressText);
	}
}

void RenderWindow::slotUpdateProgressHide(cProgressText::enumProgressType progressType)
{
	MyProgressBar *progressBar = nullptr;
	switch (progressType)
	{
		case cProgressText::progress_IMAGE: progressBar = gMainInterface->progressBar; break;
		case cProgressText::progress_ANIMATION:
			progressBar = gMainInterface->progressBarAnimation;
			break;
		case cProgressText::progress_QUEUE:
			// nothing to be done, no progress bar for queue in GUI
			break;
	}

	if (progressBar)
	{
		if (progressBar->isVisible()) progressBar->setVisible(false);
	}
}

void RenderWindow::slotMenuProgramPreferences()
{
	if (!preferencesDialog)
	{
		preferencesDialog = new cPreferencesDialog;
	}

	preferencesDialog->show();
	preferencesDialog->raise();
	preferencesDialog->activateWindow();
}

void RenderWindow::slotExportVoxelLayers()
{
	if (!voxelExportDialog)
	{
		voxelExportDialog = new cVoxelExportDialog;
	}

	voxelExportDialog->show();
	voxelExportDialog->raise();
	voxelExportDialog->activateWindow();
}

void RenderWindow::slotExportMesh()
{
	if (!meshExportDialog)
	{
		meshExportDialog = new cMeshExportDialog;
	}

	meshExportDialog->show();
	meshExportDialog->raise();
	meshExportDialog->activateWindow();
}

void RenderWindow::slotQuestionMessage(const QString &questionTitle, const QString &questionText,
	QMessageBox::StandardButtons buttons, QMessageBox::StandardButton *reply) const
{
	*reply = QMessageBox::question(ui->centralwidget, questionTitle, questionText, buttons);
}

void RenderWindow::slotAutoRefresh()
{
	gMainInterface->PeriodicRefresh();
}

void RenderWindow::slotMaterialSelected(int matIndex) const
{
	gMainInterface->MaterialSelected(matIndex);
}

void RenderWindow::slotMaterialEdited()
{
	SynchronizeInterfaceWindow(gMainInterface->materialEditor, gPar, qInterface::write);
}

void RenderWindow::slotChangedComboGridType(int index)
{
	gMainInterface->renderedImage->SetGridType(RenderedImage::enumGridType(index));
}

void RenderWindow::ResetGlobalStopRequest()
{
	gMainInterface->ResetGlobalStopRequest();
}

bool RenderWindow::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::ToolTip && !gPar->Get<bool>("display_tooltips"))
	{
		{
			return true;
		}
	}
	else if (event->type() == QEvent::ShortcutOverride)
	{
		// buttonPressTimer->stop();
		return QMainWindow::eventFilter(obj, event);
	}
	else
	{
		return QMainWindow::eventFilter(obj, event);
	}
}

void RenderWindow::ToggleFullScreen()
{
	if (!ui->actionDetach_image_from_main_window->isChecked())
	{
		gMainInterface->DetachMainImageWidget();
		gMainInterface->detachedWindow->showFullScreen();
		ui->actionDetach_image_from_main_window->setChecked(true);
	}
	else
	{
		gMainInterface->AttachMainImageWidget();
		ui->actionDetach_image_from_main_window->setChecked(false);
	}
}

void RenderWindow::slotStartRender()
{
	// save setting to history folder
	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);
	cSettings parSettings(cSettings::formatCondensedText);
	parSettings.CreateText(gPar, gParFractal, gAnimFrames, gKeyframes);
	parSettings.SaveToFile(systemDirectories.GetHistoryFileName());

	if (sender()->objectName() == "widgetDockNavigation")
	{
		bool correctionApplied = gMainInterface->CheckForMissingTextures();
		if (correctionApplied)
		{
			gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);
		}
	}

	gMainInterface->StartRender();
}

void RenderWindow::slotCameraMovementModeChanged(int index)
{
	gMainInterface->renderedImage->SetCameraMovementMode(index);
}

void RenderWindow::slotShowMeasuremetDock()
{
	if (!ui->actionShow_measurement_dock->isChecked())
	{
		ui->actionShow_measurement_dock->setChecked(true);
		slotUpdateDocksAndToolbarByAction();
	}
}
