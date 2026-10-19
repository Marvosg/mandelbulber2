/*
 * Mandelbulber v2 — local MCP command socket.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mcp_server.h"

#include "mcp_scene.h"

#include "mcp_catalog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "animation_frames.hpp"
#include "cimage.hpp"
#include "file_image.hpp"
#include "files.h"
#include "fractal_container.hpp"
#include "global_data.hpp"
#include "initparameters.hpp"
#include "interface.hpp"
#include "keyframes.hpp"
#include "object_node_type.h"
#include "render_job.hpp"
#include "rendering_configuration.hpp"
#include "settings.hpp"
#include "synchronize_interface.hpp"
#include "system.hpp"
#include "system_data.hpp"
#include "system_directories.hpp"
#include "undo.h"
#include "write_log.hpp"

#include "formula/definition/all_fractal_list.hpp"

cMcpServer *gMcpServer = nullptr;

namespace
{
bool PidAlive(qint64 pid)
{
	if (pid <= 0) return false;
#ifdef Q_OS_WIN
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
	if (!process) return false;
	DWORD code = 0;
	const BOOL ok = GetExitCodeProcess(process, &code);
	CloseHandle(process);
	return ok && code == STILL_ACTIVE;
#else
	return kill(pid_t(pid), 0) == 0;
#endif
}

QString CleanPath(const QString &path)
{
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void PushWidgets()
{
	if (systemData.noGui) return;
	if (!gMainInterface || !gMainInterface->mainWindow) return;
	gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::write);
}

void RememberEdit()
{
	PushWidgets();
	if (!gUndo) return;
	gUndo->Store(gPar, gParFractal, gAnimFrames, gKeyframes);
	gUndo->CommitNow();
}

QJsonObject Fail(const QString &message)
{
	QJsonObject object;
	object["ok"] = false;
	object["error"] = message;
	return object;
}

bool ParseUserDouble(const QJsonValue &value, double *out, QString *error)
{
	if (value.isDouble())
	{
		*out = value.toDouble();
		return true;
	}
	if (!value.isString())
	{
		*error = "Expected a JSON number.";
		return false;
	}
	QString text = value.toString().trimmed();
	if (text.contains(',') && text.contains('.'))
	{
		*error = "This number contains both ',' and '.'. Send a JSON number. A decimal comma is "
						 "accepted only when the string has no dot.";
		return false;
	}
	if (text.contains(',') && !text.contains('.')) text.replace(',', '.');
	bool ok = false;
	*out = QLocale::c().toDouble(text, &ok);
	if (!ok) *error = QString("Cannot parse '%1' as a number.").arg(value.toString());
	return ok;
}

bool ParseUserInt(const QJsonValue &value, int *out, QString *error)
{
	double number = 0;
	if (!ParseUserDouble(value, &number, error)) return false;
	*out = int(qRound(number));
	return true;
}

bool ParseBool(const QJsonValue &value, bool *out, QString *error)
{
	if (value.isBool())
	{
		*out = value.toBool();
		return true;
	}
	if (value.isDouble())
	{
		*out = value.toDouble() != 0;
		return true;
	}
	if (value.isString())
	{
		const QString text = value.toString().trimmed().toLower();
		if (text == "true" || text == "1" || text == "yes" || text == "on")
		{
			*out = true;
			return true;
		}
		if (text == "false" || text == "0" || text == "no" || text == "off")
		{
			*out = false;
			return true;
		}
	}
	*error = "Expected true or false.";
	return false;
}

bool ParseVector(const QJsonValue &value, int count, QVector<double> *out, QString *error)
{
	out->clear();
	if (value.isArray())
	{
		const QJsonArray array = value.toArray();
		if (array.size() != count)
		{
			*error = QString("Expected %1 numbers.").arg(count);
			return false;
		}
		for (const QJsonValue &item : array)
		{
			double number = 0;
			if (!ParseUserDouble(item, &number, error)) return false;
			out->append(number);
		}
		return true;
	}
	if (!value.isString())
	{
		*error = "Expected a JSON array of numbers.";
		return false;
	}
	QString text = value.toString().trimmed();
	if (text.contains('.') && text.contains(','))
	{
		*error = "Vector text contains both ',' and '.'. Send a JSON array instead.";
		return false;
	}
	if (!text.contains(' ') && text.contains(','))
	{
		*error = "Vector text looks like a comma list. Send a JSON array such as [1, 2, 3].";
		return false;
	}
	if (text.contains(',') && !text.contains('.')) text.replace(',', '.');
	const QStringList parts = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
	if (parts.size() != count)
	{
		*error = QString("Expected %1 numbers separated by spaces.").arg(count);
		return false;
	}
	for (const QString &part : parts)
	{
		bool ok = false;
		const double number = QLocale::c().toDouble(part, &ok);
		if (!ok)
		{
			*error = QString("Cannot parse '%1'.").arg(part);
			return false;
		}
		out->append(number);
	}
	return true;
}

bool ParseColor(const QJsonValue &value, sRGB *out, QString *error)
{
	if (value.isString())
	{
		QString text = value.toString().trimmed();
		if (text.startsWith('#'))
		{
			text = text.mid(1);
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
	}
	QVector<double> channels;
	if (!ParseVector(value, 3, &channels, error)) return false;
	const bool eightBit = channels[0] <= 255 && channels[1] <= 255 && channels[2] <= 255;
	auto channel = [&](double component) -> int {
		const double scaled = eightBit ? component * 257.0 : component;
		return qBound(0, int(qRound(scaled)), 65535);
	};
	*out = sRGB(channel(channels[0]), channel(channels[1]), channel(channels[2]));
	return true;
}

bool ApplyValue(cParameterContainer *container, const QString &name, const QJsonValue &value,
	QString *error)
{
	if (!container->IfExists(name))
	{
		*error = QString("Unknown parameter '%1'.").arg(name);
		return false;
	}
	const enumVarType type = container->GetVarType(name);
	switch (type)
	{
		case typeBool:
		{
			bool boolean = false;
			if (!ParseBool(value, &boolean, error)) return false;
			container->Set(name, boolean);
			return true;
		}
		case typeInt:
		{
			const cOneParameter parameter = container->GetAsOneParameter(name);
			if (!parameter.GetEnumLookup().isEmpty() && value.isString())
			{
				const int index = parameter.GetEnumLookup().indexOf(value.toString());
				if (index < 0)
				{
					*error = QString("'%1' is not one of: %2")
										 .arg(value.toString(), parameter.GetEnumLookup().join(", "));
					return false;
				}
				container->Set(name, index);
				return true;
			}
			int integer = 0;
			if (!ParseUserInt(value, &integer, error)) return false;
			container->Set(name, integer);
			return true;
		}
		case typeDouble:
		{
			double number = 0;
			if (!ParseUserDouble(value, &number, error)) return false;
			container->Set(name, number);
			return true;
		}
		case typeString:
		{
			if (!value.isString())
			{
				*error = QString("'%1' is a string.").arg(name);
				return false;
			}
			container->Set(name, value.toString());
			return true;
		}
		case typeVector3:
		{
			QVector<double> components;
			if (!ParseVector(value, 3, &components, error)) return false;
			container->Set(name, CVector3(components[0], components[1], components[2]));
			return true;
		}
		case typeVector4:
		{
			QVector<double> components;
			if (!ParseVector(value, 4, &components, error)) return false;
			container->Set(name, CVector4(components[0], components[1], components[2], components[3]));
			return true;
		}
		case typeRgb:
		{
			sRGB color;
			if (!ParseColor(value, &color, error)) return false;
			container->Set(name, color);
			return true;
		}
		default:
			*error = QString("Parameter '%1' has a type this bridge cannot set.").arg(name);
			return false;
	}
}

bool ResolveParameter(const QString &key, int fractalSlot, cParameterContainer **container,
	QString *name, QString *error)
{
	QRegularExpression prefixed("^fractal(\\d+)_(.+)$");
	const QRegularExpressionMatch match = prefixed.match(key);
	if (match.hasMatch())
	{
		const int slot = match.captured(1).toInt();
		if (slot < 1 || slot > gParFractal->size())
		{
			*error = "Fractal slots are numbered 1 to 9.";
			return false;
		}
		*container = gParFractal->at(slot - 1).get();
		*name = match.captured(2);
		if (!(*container)->IfExists(*name))
		{
			*error = QString("Fractal slot %1 has no parameter '%2'.").arg(slot).arg(*name);
			return false;
		}
		return true;
	}
	if (fractalSlot >= 1 && fractalSlot <= gParFractal->size()
		&& gParFractal->at(fractalSlot - 1)->IfExists(key))
	{
		*container = gParFractal->at(fractalSlot - 1).get();
		*name = key;
		return true;
	}
	if (gPar->IfExists(key))
	{
		*container = gPar.get();
		*name = key;
		return true;
	}
	if (fractalSlot <= 0 && gParFractal->at(0)->IfExists(key))
	{
		*container = gParFractal->at(0).get();
		*name = key;
		return true;
	}
	*error = QString("Unknown parameter '%1'. describe_parameter and list_parameters can look it up.")
						 .arg(key);
	return false;
}

QString FormulaNameFor(int formulaId)
{
	for (cAbstractFractal *formula : newFractalList)
	{
		if (int(formula->getInternalId()) == formulaId)
			return QString::fromStdString(formula->getInternalName());
	}
	return QString::number(formulaId);
}

bool LoadFractFile(const QString &path, QString *error)
{
	cSettings settings(cSettings::formatFullText);
	if (!settings.LoadFromFile(path) || !settings.Decode(gPar, gParFractal, gAnimFrames, gKeyframes))
	{
		*error = QString("Could not read settings from %1.").arg(path);
		return false;
	}
	systemData.lastSettingsFile = path;
	RememberEdit();
	return true;
}

QJsonObject StateSummary()
{
	QJsonObject summary;
	QJsonArray formulas;
	for (int i = 0; i < gParFractal->size(); i++)
	{
		const int formulaId = gParFractal->at(i)->Get<int>("formula");
		if (formulaId == int(fractal::none)) continue;
		QJsonObject slot;
		slot["slot"] = i + 1;
		slot["formula"] = FormulaNameFor(formulaId);
		slot["formula_iterations"] = gParFractal->at(i)->Get<int>("formula_iterations");
		if (gParFractal->at(i)->IfExists("power"))
			slot["power"] = gParFractal->at(i)->Get<double>("power");
		formulas.append(slot);
	}
	summary["formulas"] = formulas;
	summary["camera"] = mcpCatalog::ParameterRecord(gPar.get(), "camera").value("value");
	summary["target"] = mcpCatalog::ParameterRecord(gPar.get(), "target").value("value");
	summary["fov"] = gPar->Get<double>("fov");
	summary["detail_level"] = gPar->Get<double>("detail_level");
	summary["image_width"] = gPar->Get<int>("image_width");
	summary["image_height"] = gPar->Get<int>("image_height");
	QJsonObject effects;
	const QStringList flags = {"ambient_occlusion_enabled", "DOF_enabled", "volumetric_fog_enabled",
		"iteration_fog_enable", "glow_enabled", "raytraced_reflections", "DOF_MC_global_illumination"};
	for (const QString &flag : flags)
	{
		if (gPar->IfExists(flag)) effects[flag] = gPar->Get<bool>(flag);
	}
	summary["effects"] = effects;
	summary["loaded_file"] = systemData.lastSettingsFile;
	summary["note"] =
		"Values are the live session. They are not written to the user's file until save_settings.";
	return summary;
}

QJsonObject ChangedParameters(int limit)
{
	QJsonObject changed;
	int count = 0;
	int hidden = 0;
	auto collect = [&](cParameterContainer *container, const QString &prefix) {
		const QList<QString> names = container->GetListOfParameters();
		for (const QString &name : names)
		{
			if (container->isDefaultValue(name)) continue;
			if (count >= limit)
			{
				hidden++;
				continue;
			}
			changed[prefix + name] = mcpCatalog::ParameterRecord(container, name).value("value");
			count++;
		}
	};
	collect(gPar.get(), QString());
	for (int i = 0; i < gParFractal->size(); i++)
		collect(gParFractal->at(i).get(), QString("fractal%1_").arg(i + 1));
	QJsonObject object;
	object["changed"] = changed;
	object["truncated"] = hidden;
	return object;
}
} // namespace

cMcpServer::cMcpServer(QObject *parent)
		: QObject(parent), server(nullptr), previewSerial(0), owningSession(false)
{
}

cMcpServer::~cMcpServer()
{
	RemoveSessionFile();
}

bool cMcpServer::Start()
{
	sessionPath = systemDirectories.GetDataDirectoryHidden() + "mcp-session.json";
#ifdef Q_OS_WIN
	const QString user = qEnvironmentVariable("USERNAME", "user");
	socketPath = QString("mandelbulber2-%1").arg(user);
#else
	// macOS sockaddr_un.sun_path is 104 bytes including the NUL (108 on Linux).
	// ~/mandelbulber/mandelbulber2-<uid>.sock exceeds that for a long username.
	// The session file stores the path actually used, so the adapter does not guess.
	const QString preferred = systemDirectories.GetDataDirectoryHidden()
														+ QString("mandelbulber2-%1.sock").arg(getuid());
	if (preferred.toUtf8().size() < 100)
	{
		socketPath = preferred;
	}
	else
	{
		const QString inTemp = QDir::temp().filePath(QString("mandelbulber2-%1.sock").arg(getuid()));
		socketPath = inTemp.toUtf8().size() < 100
									 ? inTemp
									 : QString("/tmp/mandelbulber2-%1.sock").arg(getuid());
		WriteLog(QString("MCP socket path is %1 bytes; listening on %2")
								 .arg(preferred.toUtf8().size())
								 .arg(socketPath),
			1);
	}
#endif

	QFile existing(sessionPath);
	if (existing.open(QIODevice::ReadOnly))
	{
		const QJsonObject session = QJsonDocument::fromJson(existing.readAll()).object();
		const qint64 pid = qint64(session.value("pid").toDouble());
		if (PidAlive(pid) && pid != QCoreApplication::applicationPid())
		{
			WriteLog(QString("MCP socket already owned by pid %1").arg(pid), 1);
			return false;
		}
	}

	QLocalServer::removeServer(socketPath);
	server = new QLocalServer(this);
	server->setSocketOptions(QLocalServer::UserAccessOption);
	if (!server->listen(socketPath))
	{
		WriteLog(QString("MCP listen failed: %1").arg(server->errorString()), 1);
		return false;
	}
	connect(server, &QLocalServer::newConnection, this, &cMcpServer::OnNewConnection);
	owningSession = true;
	WriteSessionFile();

	QTextStream out(stdout);
	out << "MCP session file: " << sessionPath << "\n";
	out << "MCP socket: " << socketPath << "\n";
	out.flush();
	WriteLog(QString("MCP server listening on %1").arg(socketPath), 1);
	return true;
}

void cMcpServer::NoteBaseline()
{
	if (!gUndo || !gPar || !gParFractal || !gAnimFrames || !gKeyframes) return;
	gUndo->Store(gPar, gParFractal, gAnimFrames, gKeyframes);
	gUndo->CommitNow();
	// Older undo files loaded at startup stay in the buffer for the window's
	// Undo menu. MCP undo stops at this snapshot.
	gUndo->SetSessionFloor();
}

void cMcpServer::WriteSessionFile()
{
	QJsonObject session;
	session["pid"] = double(QCoreApplication::applicationPid());
	session["socket"] = socketPath;
	session["version"] = QString(MANDELBULBER_VERSION_STRING);
	session["gui"] = !systemData.noGui;
	session["data_directory"] = systemDirectories.GetDataDirectoryHidden();
	session["started"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
	QSaveFile file(sessionPath);
	if (!file.open(QIODevice::WriteOnly)) return;
	file.write(QJsonDocument(session).toJson(QJsonDocument::Indented));
	file.commit();
}

void cMcpServer::RemoveSessionFile()
{
	if (!owningSession) return;
	QFile existing(sessionPath);
	if (existing.open(QIODevice::ReadOnly))
	{
		const QJsonObject session = QJsonDocument::fromJson(existing.readAll()).object();
		const qint64 pid = qint64(session.value("pid").toDouble());
		existing.close();
		if (pid == QCoreApplication::applicationPid()) QFile::remove(sessionPath);
	}
	if (server) server->close();
	QLocalServer::removeServer(socketPath);
	owningSession = false;
}

void cMcpServer::OnNewConnection()
{
	while (server->hasPendingConnections())
	{
		QLocalSocket *socket = server->nextPendingConnection();
		buffers.insert(socket, QByteArray());
		connect(socket, &QLocalSocket::readyRead, this, &cMcpServer::OnReadyRead);
		connect(socket, &QLocalSocket::disconnected, this, &cMcpServer::OnDisconnected);
	}
}

void cMcpServer::OnReadyRead()
{
	auto *socket = qobject_cast<QLocalSocket *>(sender());
	if (!socket) return;
	buffers[socket].append(socket->readAll());
	while (buffers[socket].contains('\n'))
	{
		const int cut = buffers[socket].indexOf('\n');
		const QByteArray line = buffers[socket].left(cut);
		buffers[socket].remove(0, cut + 1);
		if (line.trimmed().isEmpty()) continue;
		const QByteArray response = HandleLine(line);
		socket->write(response);
		socket->write("\n");
		socket->flush();
	}
}

void cMcpServer::OnDisconnected()
{
	auto *socket = qobject_cast<QLocalSocket *>(sender());
	if (!socket) return;
	buffers.remove(socket);
	socket->deleteLater();
}

QByteArray cMcpServer::HandleLine(const QByteArray &line)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
	QJsonObject response;
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
	{
		response["id"] = QJsonValue::Null;
		response["ok"] = false;
		response["error"] = QString("Expected one JSON object per line: %1").arg(parseError.errorString());
		return QJsonDocument(response).toJson(QJsonDocument::Compact);
	}
	const QJsonObject request = document.object();
	response["id"] = request.value("id");
	const QString method = request.value("method").toString();
	const QJsonObject params = request.value("params").toObject();
	QJsonObject result = Dispatch(method, params);
	const bool failed = result.contains("ok") && !result.value("ok").toBool();
	if (failed && !result.contains("applied"))
	{
		response["ok"] = false;
		response["error"] = result.value("error");
		result.remove("ok");
		if (!result.isEmpty()) response["result"] = result;
	}
	else
	{
		response["ok"] = !failed;
		if (failed) response["error"] = result.value("error");
		result.remove("ok");
		response["result"] = result;
	}
	return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QJsonObject cMcpServer::Dispatch(const QString &method, const QJsonObject &params)
{
	if (mcpScene::JobActive())
	{
		const QStringList allowed = {"get_session", "get_state", "list_formulas", "describe_formula",
			"list_parameters", "describe_parameter", "list_examples", "get_objects_tree", "get_animation",
			"get_image", "get_contact_sheet", "render_status", "stop_render"};
		if (!allowed.contains(method))
		{
			return Fail("A render is already running. Poll render_status or get_image, or call "
									"stop_render before changing the session.");
		}
	}

	QJsonObject sceneResult;
	if (mcpScene::Dispatch(method, params, &sceneResult)) return sceneResult;

	if (method == "get_session")
	{
		QJsonObject result;
		result["gui"] = !systemData.noGui;
		result["version"] = QString(MANDELBULBER_VERSION_STRING);
		result["pid"] = double(QCoreApplication::applicationPid());
		result["socket"] = socketPath;
		result["loaded_file"] = systemData.lastSettingsFile;
		const bool rendering =
			(gMainInterface && gMainInterface->mainImage && gMainInterface->mainImage->IsUsed())
			|| mcpScene::JobActive();
		result["render_running"] = rendering;
		result["headless_command"] = "mandelbulber2 --mcp";
		return result;
	}

	if (method == "get_state")
	{
		const QString mode = params.value("mode").toString("summary");
		QJsonObject result;
		if (mode == "changed" || mode == "both") result = ChangedParameters(params.value("limit").toInt(200));
		if (mode == "summary" || mode == "both" || mode.isEmpty())
			result["summary"] = StateSummary();
		return result;
	}

	if (method == "list_formulas")
	{
		QJsonObject result;
		result["formulas"] =
			mcpCatalog::ListFormulas(params.value("query").toString(), params.value("limit").toInt(20));
		result["hint"] =
			"About 460 formulas exist. Search by a word of the name. describe_formula returns the "
			"parameters that formula actually shows.";
		return result;
	}

	if (method == "describe_formula")
	{
		QString error;
		QJsonObject described = mcpCatalog::DescribeFormula(params.value("name").toString(), &error);
		if (!error.isEmpty()) return Fail(error);
		return described;
	}

	if (method == "list_parameters")
	{
		QString error;
		QJsonArray list = mcpCatalog::ListParameters(params.value("area").toString(),
			params.value("fractal_slot").toInt(1), params.value("query").toString(),
			params.value("limit").toInt(30), &error);
		if (!error.isEmpty()) return Fail(error);
		QJsonObject result;
		result["parameters"] = list;
		result["areas"] = QJsonArray{
			"camera", "quality", "lights", "materials", "effects", "animation", "fractal"};
		result["hint"] =
			"area fractal is the formula currently in fractal_slot (default 1) plus the common slot "
			"controls such as power and formula_iterations. Other areas are main-window settings.";
		return result;
	}

	if (method == "describe_parameter")
	{
		QString error;
		QJsonObject described = mcpCatalog::DescribeParameter(params.value("name").toString(),
			params.value("fractal_slot").toInt(1), &error);
		if (!error.isEmpty()) return Fail(error);
		return described;
	}

	if (method == "list_examples")
	{
		QJsonObject result;
		result["examples"] =
			mcpCatalog::ListExamples(params.value("query").toString(), params.value("limit").toInt(20));
		result["hint"] =
			"load_example copies an example into the live session and can be undone. It does not edit "
			"the example file.";
		return result;
	}

	if (method == "set_parameters")
	{
		const QJsonObject values = params.value("parameters").toObject();
		if (values.isEmpty()) return Fail("parameters must be an object of name to value.");
		const int slot = params.value("fractal_slot").toInt(0);
		QJsonArray applied;
		QJsonArray errors;
		for (auto it = values.constBegin(); it != values.constEnd(); ++it)
		{
			cParameterContainer *container = nullptr;
			QString name;
			QString error;
			if (!ResolveParameter(it.key(), slot, &container, &name, &error)
				|| !ApplyValue(container, name, it.value(), &error))
			{
				QJsonObject item;
				item["name"] = it.key();
				item["error"] = error;
				errors.append(item);
				continue;
			}
			QJsonObject item;
			item["name"] = it.key();
			item["value"] = mcpCatalog::ParameterRecord(container, name).value("value");
			applied.append(item);
		}
		if (!applied.isEmpty()) RememberEdit();
		QJsonObject result;
		result["applied"] = applied;
		result["errors"] = errors;
		if (applied.isEmpty())
		{
			QStringList messages;
			for (const QJsonValue &item : errors)
			{
				const QJsonObject object = item.toObject();
				messages << object.value("name").toString() + ": " + object.value("error").toString();
			}
			return Fail(messages.isEmpty() ? QString("Nothing was set.") : messages.join("; "));
		}
		result["ok"] = errors.isEmpty();
		if (!errors.isEmpty())
			result["error"] = "Some parameters were not set. See errors. The ones in applied are live.";
		return result;
	}

	if (method == "set_formula")
	{
		const int slot = params.value("slot").toInt(params.value("fractal_slot").toInt(1));
		if (slot < 1 || slot > gParFractal->size()) return Fail("slot must be 1 to 9.");
		const QString requested = params.value("formula").toString(params.value("name").toString());
		cAbstractFractal *match = nullptr;
		for (cAbstractFractal *formula : newFractalList)
		{
			const QString internalName = QString::fromStdString(formula->getInternalName());
			const QString displayName = QString::fromStdString(formula->getNameInComboBox());
			if (internalName.compare(requested, Qt::CaseInsensitive) == 0
				|| displayName.compare(requested, Qt::CaseInsensitive) == 0)
			{
				match = formula;
				break;
			}
		}
		if (!match) return Fail(QString("No formula named '%1'.").arg(requested));
		const QString internalName = QString::fromStdString(match->getInternalName());
		gParFractal->at(slot - 1)->Set("formula", int(match->getInternalId()));

		const QList<QString> names = gPar->GetListOfParameters();
		for (const QString &name : names)
		{
			if (!name.startsWith("node_") || !name.endsWith("_definition")) continue;
			QStringList parts = gPar->Get<QString>(name).split(',');
			if (parts.size() < 5) continue;
			if (parts.at(2).toInt() != int(enumNodeType::fractal)) continue;
			if (parts.at(4).toInt() != slot) continue;
			const int nodeId = name.mid(5, 4).toInt();
			parts[0] = internalName + " " + QString::number(nodeId);
			gPar->Set(name, parts.join(','));
		}
		RememberEdit();
		QJsonObject result;
		result["slot"] = slot;
		result["formula"] = internalName;
		result["display_name"] = QString::fromStdString(match->getNameInComboBox());
		result["id"] = int(match->getInternalId());
		return result;
	}

	if (method == "load_settings" || method == "load_example")
	{
		QString path = params.value("path").toString();
		if (method == "load_example")
		{
			QString error;
			path = mcpCatalog::ResolveExample(params.value("name").toString(params.value("path").toString()),
				&error);
			if (path.isEmpty()) return Fail(error);
		}
		if (path.isEmpty() || !QFile::exists(path))
			return Fail("Pass path to a .fract file that exists.");
		QString error;
		if (!LoadFractFile(path, &error)) return Fail(error);
		QJsonObject result;
		result["loaded"] = path;
		result["summary"] = StateSummary();
		result["undo"] = "One undo returns to the session from before this load.";
		return result;
	}

	if (method == "save_settings")
	{
		const QString path = params.value("path").toString();
		if (path.isEmpty())
			return Fail("save_settings needs an explicit path. The open file is never written implicitly.");
		const bool overwrite = params.value("overwrite").toBool(false);
		const QString target = CleanPath(path);
		const QString examples = CleanPath(systemDirectories.sharedDir + "examples");
		if (!examples.isEmpty() && (target == examples || target.startsWith(examples + "/")))
			return Fail("Refusing to write inside the shared examples directory.");
		const QString current = systemData.lastSettingsFile.isEmpty()
															? QString()
															: CleanPath(systemData.lastSettingsFile);
		if (!overwrite && !current.isEmpty() && target == current)
		{
			return Fail(
				"That path is the file currently loaded. Pass overwrite true only when the user asked "
				"to replace it. Saving to a new path is the safe default.");
		}
		QDir().mkpath(QFileInfo(target).absolutePath());
		cSettings settings(cSettings::formatFullText);
		settings.CreateText(gPar, gParFractal, gAnimFrames, gKeyframes);
		if (!settings.SaveToFile(target)) return Fail(QString("Could not write %1.").arg(target));
		systemData.lastSettingsFile = target;
		QJsonObject result;
		result["saved"] = target;
		result["note"] = "The live session is unchanged apart from remembering this as the last file.";
		return result;
	}

	if (method == "undo" || method == "redo")
	{
		if (!gUndo) return Fail("Undo is not available.");
		bool frames = false;
		bool keyframes = false;
		const bool isUndo = method == "undo";
		if (isUndo && !gUndo->CanUndo())
			return Fail("Nothing to undo in this session. Undo stops at the scene from when this "
									"process finished starting, including after autosave recovery. The window's "
									"Undo menu can still reach older files.");
		if (!isUndo && !gUndo->CanRedo()) return Fail("Nothing to redo.");
		const bool ok = isUndo ? gUndo->Undo(gPar, gParFractal, gAnimFrames, gKeyframes, &frames, &keyframes)
													 : gUndo->Redo(gPar, gParFractal, gAnimFrames, gKeyframes, &frames, &keyframes);
		if (!ok) return Fail(isUndo ? QString("Undo failed.") : QString("Redo failed."));
		PushWidgets();
		QJsonObject result;
		result["summary"] = StateSummary();
		return result;
	}

	if (method == "widget_roundtrip")
	{
		if (systemData.noGui || !gMainInterface || !gMainInterface->mainWindow)
			return Fail("widget_roundtrip only works while the graphical window is open.");
		gMainInterface->SynchronizeInterface(gPar, gParFractal, qInterface::read);
		const int slot = params.value("fractal_slot").toInt(0);
		cParameterContainer *container = nullptr;
		QString name;
		QString error;
		if (!ResolveParameter(params.value("name").toString(), slot, &container, &name, &error))
			return Fail(error);
		QJsonObject result = mcpCatalog::ParameterRecord(container, name);
		result["note"] =
			"The widgets were read back into the session, the same direction a GUI render uses. If this "
			"value matches the last set_parameters call, the edit survived that read-back.";
		return result;
	}

	return Fail(QString("Unknown method '%1'.").arg(method));
}
