/*
 * Mandelbulber v2 — MCP formula and parameter catalogue.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mcp_catalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include "fractal_container.hpp"
#include "global_data.hpp"
#include "initparameters.hpp"
#include "one_parameter.hpp"
#include "system_directories.hpp"

#include "formula/definition/all_fractal_list.hpp"

namespace
{
struct sUiParam
{
	QString name;
	QString label;
	QString description;
	bool hasRange = false;
	double minimum = 0;
	double maximum = 0;
};

struct sFormulaInfo
{
	QString internalName;
	QString displayName;
	int internalId = 0;
	QString summary;
	QStringList references;
	QStringList examples;
	QString distanceEstimation;
	QString deFunction;
	double defaultBailout = 0;
	QList<sUiParam> parameters;
	bool parsed = false;
};

QHash<QString, sFormulaInfo> gFormulasByInternal;
QHash<QString, QString> gTooltips;
bool gIndexReady = false;

QString StripHtml(QString text)
{
	text.replace(QRegularExpression("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption), "\n");
	text.replace(QRegularExpression("</p>", QRegularExpression::CaseInsensitiveOption), "\n");
	text.replace(QRegularExpression("</tr>", QRegularExpression::CaseInsensitiveOption), "\n");
	text.replace(QRegularExpression("</h[1-6]>", QRegularExpression::CaseInsensitiveOption), "\n");
	text.replace(QRegularExpression("<[^>]+>"), " ");
	text.replace("&amp;", "&");
	text.replace("&lt;", "<");
	text.replace("&gt;", ">");
	text.replace("&quot;", "\"");
	text.replace("&#039;", "'");
	text.replace("&nbsp;", " ");
	text.replace(QRegularExpression("[ \\t]+"), " ");
	text.replace(QRegularExpression(" *\\n *"), "\n");
	text.replace(QRegularExpression("\\n{3,}"), "\n\n");
	return text.trimmed();
}

QString ParamFromObjectName(const QString &objectName)
{
	const int underscore = objectName.indexOf('_');
	if (underscore <= 0) return QString();
	const QString kind = objectName.left(underscore);
	QString param = objectName.mid(underscore + 1);
	if (kind == "spinboxd3" || kind == "spinbox3" || kind == "spinboxi3" || kind == "spinbox4"
		|| kind == "spinboxd4")
	{
		if (param.endsWith("_x") || param.endsWith("_y") || param.endsWith("_z") || param.endsWith("_w"))
			param.chop(2);
	}
	return param;
}

bool IsEditorClass(const QString &className)
{
	static const QStringList classes = {"MyDoubleSpinBox", "QDoubleSpinBox", "MySpinBox", "QSpinBox",
		"MyCheckBox", "QCheckBox", "MyLineEdit", "QLineEdit", "MyComboBox", "QComboBox",
		"cFormulaComboBox", "MyColorButton", "QPlainTextEdit", "cMyTextEdit", "QTextEdit",
		"cGradientEditWidget", "FileSelectWidget", "cMaterialSelector"};
	return classes.contains(className);
}

struct sParsedUi
{
	QString summary;
	QStringList references;
	QStringList examples;
	QList<sUiParam> parameters;
	QHash<QString, QString> tooltips;
};

sParsedUi ParseUiFile(const QString &path)
{
	sParsedUi out;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return out;

	struct sWidget
	{
		QString className;
		QString objectName;
		QString text;
		QString toolTip;
		bool hasMin = false;
		bool hasMax = false;
		double minimum = 0;
		double maximum = 0;
		int gridId = -1;
		int row = -1;
		int column = -1;
		QString currentProperty;
		bool inString = false;
	};

	QXmlStreamReader xml(&file);
	QList<sWidget> stack;
	struct sGridItem
	{
		int gridId;
		int row;
		int column;
	};
	QList<sGridItem> items;
	QList<int> grids;
	int nextGrid = 1;
	QList<sWidget> finished;

	auto currentItem = [&]() -> sGridItem {
		if (items.isEmpty()) return {-1, -1, -1};
		return items.last();
	};

	while (!xml.atEnd())
	{
		xml.readNext();
		if (xml.isStartElement())
		{
			const QString tag = xml.name().toString();
			if (tag == "layout")
			{
				const QString layoutClass = xml.attributes().value("class").toString();
				if (layoutClass.contains("Grid"))
					grids.append(nextGrid++);
				else
					grids.append(-1);
			}
			else if (tag == "item")
			{
				sGridItem item;
				item.gridId = grids.isEmpty() ? -1 : grids.last();
				item.row = xml.attributes().value("row").toInt();
				item.column = xml.attributes().value("column").toInt();
				if (item.gridId < 0)
				{
					item.row = -1;
					item.column = -1;
				}
				items.append(item);
			}
			else if (tag == "widget")
			{
				sWidget widget;
				widget.className = xml.attributes().value("class").toString();
				widget.objectName = xml.attributes().value("name").toString();
				const sGridItem item = currentItem();
				widget.gridId = item.gridId;
				widget.row = item.row;
				widget.column = item.column;
				stack.append(widget);
			}
			else if (tag == "property" && !stack.isEmpty())
			{
				stack.last().currentProperty = xml.attributes().value("name").toString();
			}
			else if ((tag == "string" || tag == "number" || tag == "double") && !stack.isEmpty())
			{
				stack.last().inString = true;
			}
		}
		else if (xml.isCharacters() && !stack.isEmpty() && stack.last().inString)
		{
			sWidget &widget = stack.last();
			const QString value = xml.text().toString();
			if (widget.currentProperty == "text")
				widget.text += value;
			else if (widget.currentProperty == "toolTip")
				widget.toolTip += value;
			else if (widget.currentProperty == "minimum")
			{
				widget.hasMin = true;
				widget.minimum = value.toDouble();
			}
			else if (widget.currentProperty == "maximum")
			{
				widget.hasMax = true;
				widget.maximum = value.toDouble();
			}
		}
		else if (xml.isEndElement())
		{
			const QString tag = xml.name().toString();
			if (tag == "string" || tag == "number" || tag == "double")
			{
				if (!stack.isEmpty()) stack.last().inString = false;
			}
			else if (tag == "property")
			{
				if (!stack.isEmpty()) stack.last().currentProperty.clear();
			}
			else if (tag == "widget")
			{
				if (!stack.isEmpty())
				{
					finished.append(stack.last());
					stack.removeLast();
				}
			}
			else if (tag == "item")
			{
				if (!items.isEmpty()) items.removeLast();
			}
			else if (tag == "layout")
			{
				if (!grids.isEmpty()) grids.removeLast();
			}
		}
	}

	QHash<QString, QString> labelByCell;
	for (const sWidget &widget : finished)
	{
		if (widget.className == "QLabel" && widget.column == 0 && widget.gridId > 0)
			labelByCell.insert(QString("%1:%2").arg(widget.gridId).arg(widget.row), StripHtml(widget.text));
		if (widget.objectName == "label_information_general")
		{
			const QString plain = StripHtml(widget.text);
			QRegularExpression href("href=\"([^\"]+)\"");
			auto it = href.globalMatch(widget.text);
			while (it.hasNext())
				out.references.append(it.next().captured(1));
			const int examplesAt = plain.indexOf("Examples using this formula");
			QString summary = plain;
			if (examplesAt >= 0)
			{
				summary = plain.left(examplesAt).trimmed();
				const QString rest = plain.mid(examplesAt + QString("Examples using this formula").size());
				const QStringList lines = rest.split('\n');
				for (QString line : lines)
				{
					line = line.trimmed();
					if (line.isEmpty() || line == "..." || line.startsWith("Code")) break;
					if (line.endsWith(".fract")) out.examples.append(line);
					if (out.examples.size() >= 8) break;
				}
			}
			const int codeAt = summary.indexOf("\nCode");
			if (codeAt >= 0) summary = summary.left(codeAt).trimmed();
			out.summary = summary;
		}
	}

	QSet<QString> seen;
	for (const sWidget &widget : finished)
	{
		if (!IsEditorClass(widget.className)) continue;
		const QString param = ParamFromObjectName(widget.objectName);
		if (param.isEmpty() || seen.contains(param)) continue;
		seen.insert(param);
		sUiParam doc;
		doc.name = param;
		doc.description = StripHtml(widget.toolTip);
		doc.label = labelByCell.value(QString("%1:%2").arg(widget.gridId).arg(widget.row));
		if (doc.label.endsWith(':')) doc.label.chop(1);
		doc.hasRange = widget.hasMin && widget.hasMax && widget.minimum < widget.maximum;
		doc.minimum = widget.minimum;
		doc.maximum = widget.maximum;
		out.parameters.append(doc);
		if (!doc.description.isEmpty()) out.tooltips.insert(param, doc.description);
	}
	return out;
}

QString DePhrase(fractal::enumDEType type, fractal::enumDEFunctionType function, double bailout)
{
	QString typeName = "unspecified";
	if (type == fractal::analyticDEType)
		typeName = "analytic (the formula supplies a distance estimate, which is the fast path)";
	else if (type == fractal::deltaDEType)
		typeName =
			"delta (the distance is estimated numerically, which is slower and can miss thin features)";
	QString functionName = "unspecified";
	switch (function)
	{
		case fractal::linearDEFunction: functionName = "linear"; break;
		case fractal::logarithmicDEFunction: functionName = "logarithmic"; break;
		case fractal::pseudoKleinianDEFunction: functionName = "pseudo-Kleinian"; break;
		case fractal::josKleinianDEFunction: functionName = "Jos Kleinian"; break;
		case fractal::customDEFunction: functionName = "custom"; break;
		case fractal::maxAxisDEFunction: functionName = "max-axis"; break;
		default: break;
	}
	return QString("Distance estimation is %1. DE function: %2. Default bailout %3 (used when "
								 "use_default_bailout is on; higher bailout stops the iteration sooner and usually "
								 "makes a smoother, less detailed shape).")
		.arg(typeName)
		.arg(functionName)
		.arg(bailout, 0, 'g', 8);
}

const QHash<QString, QString> &GlossaryMap()
{
	static const QHash<QString, QString> map = {
		{"camera",
			"Eye position as x y z in fractal space. Move it to walk around the shape. The camera looks "
			"at target."},
		{"target", "The point the camera looks at, as x y z. Orbiting keeps the camera aimed here."},
		{"camera_distance_to_target",
			"Distance from the eye to target. Smaller values move into the fractal. This is the control "
			"the navigation gizmo edits when you zoom."},
		{"camera_rotation",
			"Camera pitch, yaw and roll in degrees, as three numbers. (0, 0, 0) is the default "
			"orientation toward target."},
		{"camera_top",
			"Which way is up for the camera, as a 3-vector. Leave this alone unless the view is rolled "
			"the wrong way."},
		{"fov",
			"Field of view in degrees. 40 to 60 looks like a normal lens. Under about 10 is a telephoto "
			"zoom into a detail. Above 90 is wide-angle and exaggerates the foreground."},
		{"perspective_type",
			"Camera projection. The usual choice is the default three-point perspective. Fish-eye and "
			"equirectangular are for special views."},
		{"detail_level",
			"How fine the ray march is. 1.0 means the smallest feature kept is about 2 pixels. Higher "
			"is sharper and much slower. A preview should stay at or below 1.0; 0.25 to 0.5 is a rough "
			"sketch. Do not raise this to 'make it look right' before the shape and camera are settled."},
		{"DE_factor",
			"Step size for the distance estimator. 1.0 is normal. Lower (0.2 to 0.7) is more accurate "
			"and slower, and helps thin or noisy surfaces. Higher skips ahead and can punch holes."},
		{"N",
			"Maximum iterations for the whole ray. This is a quality/depth limit, not the Mandelbulb "
			"power. Raising it lets rays travel deeper into the set and gets slower. Lowering it "
			"changes the shape, so do not cut it just to speed up a preview."},
		{"image_width", "Final image width in pixels. A preview does not change this; it renders a small copy."},
		{"image_height", "Final image height in pixels. Previews keep this aspect ratio."},
		{"view_distance_max",
			"How far a ray is allowed to travel. If the fractal is cut off in the distance, raise this."},
		{"formula_iterations",
			"How many times this formula is applied inside one fractal slot. 1 means once per iteration. "
			"In a hybrid, repeating one formula (4, 8, 16) makes that formula dominate the shape."},
		{"formula_weight",
			"Relative influence of this formula when several formulas are mixed. Higher weight pulls the "
			"shape toward this formula."},
		{"power",
			"The exponent of power-family formulas such as Mandelbulb and Mandelbox. Mandelbulb power 8 "
			"is the classic bulb. 2 is a smoother ball, 3 to 6 are more folded, 9 is this program's "
			"default. Integers are the usual starting point; fractions add twist."},
		{"bailout",
			"Iteration escape radius. When use_default_bailout is on, the formula's own default is used "
			"instead. Larger bailout stops earlier and softens detail."},
		{"use_default_bailout",
			"When on, the formula's built-in bailout is used and the bailout number is ignored. Leave "
			"this on unless you are deliberately matching a published parameter set."},
		{"julia_mode",
			"When on, the slot is a Julia set of the same formula: the shape is seeded from julia_c "
			"instead of the pixel. Julia versions look like pieces of the original fractal."},
		{"julia_c",
			"The Julia constant, three numbers. Small values (around -0.5 to 0.5) keep a recognizable "
			"shape. Large values blow the set apart."},
		{"formula_position", "Moves this formula inside the hybrid, without moving the camera."},
		{"formula_rotation", "Rotates this formula, in degrees, as three numbers."},
		{"formula_scale",
			"Scales this formula inside a hybrid. 1 is unchanged. Use this when one formula in a hybrid "
			"is much larger than the other."},
		{"formula_material_id",
			"Which material number shades this formula. Materials themselves are a later slice; 1 is the "
			"default surface."},
		{"main_light_intensity",
			"Brightness of the key light. If the picture is a silhouette, raise this before touching "
			"exposure."},
		{"main_light_alpha", "Horizontal angle of the key light, in degrees."},
		{"main_light_beta", "Vertical angle of the key light, in degrees. 90 is overhead."},
		{"main_light_colour",
			"Key light color as 16-bit red, green, blue (0 to 65535). You can also send an 8-bit color "
			"or a #rrggbb string and it will be scaled."},
		{"ambient_occlusion_enabled",
			"Soft contact shadows in crevices. Expensive. Previews turn this off on a throwaway copy so "
			"your setting is kept."},
		{"ambient_occlusion_quality", "How many ambient-occlusion samples. Higher is smoother and slower."},
		{"DOF_enabled",
			"Depth of field blur. Expensive, and it hides the shape while you are still framing. Turn it "
			"on only after the camera is right."},
		{"volumetric_fog_enabled",
			"Fog lit by the lights in the volume. Expensive. Different from iteration fog, which is a "
			"cheap coloring of the orbit."},
		{"iteration_fog_enable",
			"Colors the orbit trap as a fog. This is often the look of a Mandelbulber picture and it is "
			"comparatively cheap. Do not turn it off just to speed a preview."},
		{"glow_enabled",
			"A bright halo on the surface. On by default and cheap. Part of why a fresh scene is not a "
			"flat grey ball."},
		{"raytraced_reflections",
			"Real reflections of the fractal on itself. Very expensive. Leave off until the final render."},
		{"DOF_MC_global_illumination",
			"Monte Carlo global illumination. The slowest lighting mode. Not for previews."},
		{"brightness", "Overall exposure after lighting. 1 is neutral. Raise it if the shape is right but dark."},
		{"gamma", "Midtone curve. Around 1 is linear-ish; lower lifts shadows."},
		{"hdr", "Enables the high-dynamic-range color path. Leave it on if the example you loaded uses it."},
	};
	return map;
}

QString AreaOf(const QString &name)
{
	if (name == "fov" || name == "target" || name.startsWith("camera") || name.startsWith("perspective")
		|| name.startsWith("stereo"))
		return "camera";
	if (name.startsWith("main_light") || name.startsWith("aux_light") || name.startsWith("light"))
		return "lights";
	if (name.startsWith("mat")) return "materials";
	if (name.startsWith("anim_") || name.startsWith("flight_") || name.startsWith("keyframe_"))
		return "animation";
	if (name.contains("ambient_occlusion") || name.startsWith("DOF") || name.startsWith("glow")
		|| name.contains("volumetric") || name.contains("fog") || name.contains("reflect")
		|| name.startsWith("hdr") || name.startsWith("ssao") || name.startsWith("brightness")
		|| name == "gamma" || name == "contrast")
		return "effects";
	if (name == "N" || name.startsWith("detail") || name.startsWith("DE_") || name.startsWith("image_")
		|| name.contains("raymarch") || name.startsWith("limit_") || name == "view_distance_max"
		|| name.startsWith("antialias"))
		return "quality";
	return "other";
}

void RememberTooltip(const QString &key, const QString &text)
{
	if (key.isEmpty() || text.isEmpty() || gTooltips.contains(key)) return;
	gTooltips.insert(key, text);
	const QStringList suffixes = {"_x", "_y", "_z", "_w", "_R", "_G", "_B"};
	for (const QString &suffix : suffixes)
	{
		if (!key.endsWith(suffix) || key.size() <= suffix.size()) continue;
		const QString base = key.left(key.size() - suffix.size());
		if (!gTooltips.contains(base)) gTooltips.insert(base, text);
		break;
	}
}

void LoadDockTooltips()
{
	// Live .ui files win when a developer tree still has qt/ next to formula/.
	// An installed app, including the macOS bundle, does not ship those .ui
	// files. data/mcp-dock-tooltips.json is copied with the rest of data/.
	QStringList dirs;
	const QString formulaDir =
		QFileInfo(systemDirectories.sharedDir + "formula").canonicalFilePath();
	if (!formulaDir.isEmpty())
		dirs << QFileInfo(formulaDir).dir().filePath("qt");
	dirs << systemDirectories.sharedDir + "qt";

	for (const QString &dir : dirs)
	{
		if (!QDir(dir).exists()) continue;
		QDirIterator files(dir, QStringList() << "*.ui", QDir::Files);
		while (files.hasNext())
		{
			const sParsedUi parsed = ParseUiFile(files.next());
			for (auto it = parsed.tooltips.constBegin(); it != parsed.tooltips.constEnd(); ++it)
				RememberTooltip(it.key(), it.value());
		}
		break;
	}

	QFile jsonFile(systemDirectories.sharedDir + "data/mcp-dock-tooltips.json");
	if (!jsonFile.open(QIODevice::ReadOnly)) return;
	const QJsonObject object = QJsonDocument::fromJson(jsonFile.readAll()).object();
	for (auto it = object.constBegin(); it != object.constEnd(); ++it)
		RememberTooltip(it.key(), it.value().toString());
}

void BuildIndex()
{
	if (gIndexReady) return;
	gIndexReady = true;
	LoadDockTooltips();
	for (cAbstractFractal *formula : newFractalList)
	{
		sFormulaInfo info;
		info.internalName = QString::fromStdString(formula->getInternalName());
		info.displayName = QString::fromStdString(formula->getNameInComboBox());
		info.internalId = int(formula->getInternalId());
		info.defaultBailout = formula->getDefaultBailout();
		info.distanceEstimation = DePhrase(
			formula->getDeType(), formula->getDeFunctionType(), formula->getDefaultBailout());
		gFormulasByInternal.insert(info.internalName.toLower(), info);
	}
}

sFormulaInfo *FindFormula(const QString &name)
{
	BuildIndex();
	const QString key = name.trimmed().toLower();
	if (gFormulasByInternal.contains(key)) return &gFormulasByInternal[key];
	for (auto it = gFormulasByInternal.begin(); it != gFormulasByInternal.end(); ++it)
	{
		if (it->displayName.compare(name, Qt::CaseInsensitive) == 0) return &(*it);
		if (it->internalName.compare(name, Qt::CaseInsensitive) == 0) return &(*it);
	}
	return nullptr;
}

void EnsureFormulaParsed(sFormulaInfo *info)
{
	if (!info || info->parsed) return;
	info->parsed = true;
	cAbstractFractal *match = nullptr;
	for (cAbstractFractal *formula : newFractalList)
	{
		if (int(formula->getInternalId()) == info->internalId)
		{
			match = formula;
			break;
		}
	}
	if (!match) return;
	const QString path = QString::fromStdString(match->getUiFilename());
	const sParsedUi parsed = ParseUiFile(path);
	info->summary = parsed.summary;
	info->references = parsed.references;
	info->examples = parsed.examples;
	info->parameters = parsed.parameters;
	for (auto it = parsed.tooltips.constBegin(); it != parsed.tooltips.constEnd(); ++it)
	{
		if (!gTooltips.contains(it.key())) gTooltips.insert(it.key(), it.value());
	}
}

} // namespace

namespace
{
bool SameNumber(double a, double b)
{
	return qAbs(a - b) <= 1e-12 * qMax(1.0, qMax(qAbs(a), qAbs(b)));
}
} // namespace

namespace mcpCatalog
{
void EnsureLoaded()
{
	BuildIndex();
}

QString Glossary(const QString &name)
{
	return GlossaryMap().value(name);
}

QString TooltipFor(const QString &name)
{
	BuildIndex();
	return gTooltips.value(name);
}

QJsonObject ParameterRecord(cParameterContainer *container, const QString &name)
{
	QJsonObject object;
	object["name"] = name;
	if (!container || !container->IfExists(name))
	{
		object["known"] = false;
		return object;
	}
	const cOneParameter parameter = container->GetAsOneParameter(name);
	const enumVarType type = parameter.GetValueType();
	QString typeName = "unknown";
	switch (type)
	{
		case typeBool: typeName = "bool"; break;
		case typeInt: typeName = "int"; break;
		case typeDouble: typeName = "double"; break;
		case typeString: typeName = "string"; break;
		case typeVector3: typeName = "vector3"; break;
		case typeVector4: typeName = "vector4"; break;
		case typeRgb: typeName = "rgb"; break;
		default: break;
	}
	object["known"] = true;
	object["type"] = typeName;
	object["is_default"] = parameter.isDefaultValue();

	auto putValue = [&](const char *key, enumValueSelection which) {
		switch (type)
		{
			case typeBool: object[key] = parameter.Get<bool>(which); break;
			case typeInt: object[key] = parameter.Get<int>(which); break;
			case typeDouble: object[key] = parameter.Get<double>(which); break;
			case typeString: object[key] = parameter.Get<QString>(which); break;
			case typeVector3:
			{
				const CVector3 v = parameter.Get<CVector3>(which);
				object[key] = QJsonArray{v.x, v.y, v.z};
				break;
			}
			case typeVector4:
			{
				const CVector4 v = parameter.Get<CVector4>(which);
				object[key] = QJsonArray{v.x, v.y, v.z, v.w};
				break;
			}
			case typeRgb:
			{
				const sRGB c = parameter.Get<sRGB>(which);
				object[key] = QJsonArray{c.R, c.G, c.B};
				break;
			}
			default: break;
		}
	};
	putValue("value", valueActual);
	putValue("default", valueDefault);

	if (type == typeInt)
	{
		const int minValue = parameter.Get<int>(valueMin);
		const int maxValue = parameter.Get<int>(valueMax);
		if (minValue < maxValue)
		{
			object["minimum"] = minValue;
			object["maximum"] = maxValue;
			object["range_enforced"] = true;
		}
		else
		{
			object["range_enforced"] = false;
		}
	}
	else if (type == typeDouble)
	{
		const double minValue = parameter.Get<double>(valueMin);
		const double maxValue = parameter.Get<double>(valueMax);
		if (minValue < maxValue && !SameNumber(minValue, maxValue))
		{
			object["minimum"] = minValue;
			object["maximum"] = maxValue;
			object["range_enforced"] = true;
		}
		else
		{
			object["range_enforced"] = false;
		}
	}
		if (!parameter.GetEnumLookup().isEmpty())
	{
		QJsonArray names;
		for (const QString &item : parameter.GetEnumLookup())
			names.append(item);
		object["enum"] = names;
	}
	const QString glossary = Glossary(name);
	const QString tooltip = TooltipFor(name);
	if (!glossary.isEmpty()) object["description"] = glossary;
	else if (!tooltip.isEmpty()) object["description"] = tooltip;
	if (!tooltip.isEmpty() && tooltip != glossary) object["ui_tooltip"] = tooltip;
	if (type == typeRgb)
	{
		object["color_note"] =
			"Channels are 16-bit, 0 to 65535. An array of values all <= 255, or a #rrggbb string, "
			"is treated as 8-bit and scaled. Send JSON numbers, not a locale-formatted string.";
	}
	else if (type == typeDouble || type == typeVector3 || type == typeVector4)
	{
		object["number_note"] =
			"Send JSON numbers (decimal point). A string is accepted only as a fallback: one decimal "
			"comma is treated as a decimal point. A value that contains both ',' and '.' is rejected.";
	}
	return object;
}

QJsonArray ListFormulas(const QString &query, int limit)
{
	BuildIndex();
	if (limit <= 0) limit = 20;
	if (limit > 50) limit = 50;
	const QString needle = query.trimmed().toLower();
	QJsonArray array;
	for (cAbstractFractal *formula : newFractalList)
	{
		if (formula->getInternalId() == fractal::none) continue;
		const QString internalName = QString::fromStdString(formula->getInternalName());
		const QString displayName = QString::fromStdString(formula->getNameInComboBox());
		sFormulaInfo *info = FindFormula(internalName);
		QString summary;
		if (!needle.isEmpty())
		{
			const bool nameHit = internalName.toLower().contains(needle)
													 || displayName.toLower().contains(needle);
			if (!nameHit && info)
			{
				EnsureFormulaParsed(info);
				summary = info->summary;
				if (!summary.toLower().contains(needle)) continue;
			}
			else if (!nameHit)
			{
				continue;
			}
		}
		if (summary.isEmpty() && info && info->parsed) summary = info->summary;
		QJsonObject object;
		object["internal_name"] = internalName;
		object["display_name"] = displayName;
		object["id"] = int(formula->getInternalId());
		if (!summary.isEmpty())
			object["summary"] = summary.left(280);
		array.append(object);
		if (array.size() >= limit) break;
	}
	return array;
}

QJsonObject DescribeFormula(const QString &name, QString *error)
{
	sFormulaInfo *info = FindFormula(name);
	if (!info)
	{
		if (error) *error = QString("No formula named '%1'. Call list_formulas with a short query.").arg(name);
		return QJsonObject();
	}
	EnsureFormulaParsed(info);
	QJsonObject object;
	object["internal_name"] = info->internalName;
	object["display_name"] = info->displayName;
	object["id"] = info->internalId;
	object["summary"] = info->summary.isEmpty()
												? QString("No info panel text is shipped for this formula.")
												: info->summary;
	object["distance_estimation"] = info->distanceEstimation;
	object["default_bailout"] = info->defaultBailout;
	QJsonArray references;
	for (const QString &reference : info->references)
		references.append(reference);
	object["references"] = references;
	QJsonArray examples;
	for (const QString &example : info->examples)
		examples.append(example);
	object["example_files"] = examples;
	object["how_to_select"] = QString(
		"Call set_formula with slot 1 to 9 and internal_name '%1' (the display name also works). "
		"Slot 1 is the default object. Extra slots do nothing until a hybrid or boolean in the objects "
		"tree uses them; building that tree is the next slice, but the slot's formula integer is what "
		"the ray marcher reads.")
															.arg(info->internalName);

	QJsonArray parameters;
	cParameterContainer *slot = gParFractal->at(0).get();
	for (const sUiParam &uiParam : info->parameters)
	{
		QJsonObject record = ParameterRecord(slot, uiParam.name);
		if (!uiParam.label.isEmpty()) record["label"] = uiParam.label;
		if (!uiParam.description.isEmpty()) record["description"] = uiParam.description;
		if (uiParam.hasRange)
		{
			record["ui_minimum"] = uiParam.minimum;
			record["ui_maximum"] = uiParam.maximum;
		}
		record["where"] = "formula";
		parameters.append(record);
	}
	object["parameters"] = parameters;

	QJsonArray common;
	const QStringList commonNames = {"formula_iterations", "formula_weight", "power", "bailout",
		"use_default_bailout", "julia_mode", "julia_c", "formula_position", "formula_rotation",
		"formula_scale", "formula_material_id"};
	for (const QString &commonName : commonNames)
	{
		if (!slot->IfExists(commonName)) continue;
		QJsonObject record = ParameterRecord(slot, commonName);
		record["where"] = "fractal_slot";
		record["note"] =
			"Shared by every formula slot. Set it with set_parameters and fractal_slot. It is not "
			"unique to this formula, but it is usually what you change next.";
		common.append(record);
	}
	object["common_slot_parameters"] = common;
	return object;
}

QJsonArray ListExamples(const QString &query, int limit)
{
	if (limit <= 0) limit = 20;
	if (limit > 50) limit = 50;
	const QString root = systemDirectories.sharedDir + "examples";
	const QString needle = query.trimmed().toLower();
	QJsonArray array;
	QDirIterator files(root, QStringList() << "*.fract", QDir::Files, QDirIterator::Subdirectories);
	while (files.hasNext())
	{
		const QString full = files.next();
		const QString relative = QDir(root).relativeFilePath(full);
		if (!needle.isEmpty() && !relative.toLower().contains(needle)
			&& !QFileInfo(full).fileName().toLower().contains(needle))
			continue;
		QJsonObject object;
		object["name"] = QFileInfo(full).fileName();
		object["path"] = relative;
		array.append(object);
		if (array.size() >= limit) break;
	}
	return array;
}

QString ResolveExample(const QString &query, QString *error)
{
	const QString root = QFileInfo(systemDirectories.sharedDir + "examples").canonicalFilePath();
	if (root.isEmpty())
	{
		if (error) *error = "The examples directory is not installed next to this Mandelbulber.";
		return QString();
	}
	QStringList hits;
	QDirIterator files(root, QStringList() << "*.fract", QDir::Files, QDirIterator::Subdirectories);
	const QString needle = query.trimmed();
	while (files.hasNext())
	{
		const QString full = files.next();
		const QString relative = QDir(root).relativeFilePath(full);
		if (QFileInfo(full).fileName().compare(needle, Qt::CaseInsensitive) == 0
			|| relative.compare(needle, Qt::CaseInsensitive) == 0
			|| QFileInfo(full).completeBaseName().compare(needle, Qt::CaseInsensitive) == 0)
			hits.append(QFileInfo(full).canonicalFilePath());
	}
	if (hits.size() == 1) return hits.first();
	if (hits.isEmpty())
	{
		if (error)
			*error = QString("No example matches '%1'. Call list_examples and pass a filename.").arg(query);
		return QString();
	}
	if (error)
	{
		QStringList names;
		for (int i = 0; i < hits.size() && i < 8; i++)
			names << QDir(root).relativeFilePath(hits.at(i));
		*error = QString("'%1' matches %2 examples (%3). Pass the relative path.")
							 .arg(query)
							 .arg(hits.size())
							 .arg(names.join(", "));
	}
	return QString();
}

QJsonObject DescribeParameter(const QString &name, int fractalSlot, QString *error)
{
	if (fractalSlot < 1) fractalSlot = 1;
	if (fractalSlot > gParFractal->size())
	{
		if (error) *error = "fractal_slot must be 1 to 9.";
		return QJsonObject();
	}
	cParameterContainer *container = nullptr;
	QString where = "main";
	if (gParFractal->at(fractalSlot - 1)->IfExists(name))
	{
		container = gParFractal->at(fractalSlot - 1).get();
		where = QString("fractal_slot_%1").arg(fractalSlot);
	}
	else if (gPar->IfExists(name))
	{
		container = gPar.get();
	}
	else
	{
		if (error)
			*error = QString("Unknown parameter '%1'. Try list_parameters with a query, or describe_formula "
											 "for formula-specific controls.")
								 .arg(name);
		return QJsonObject();
	}
	QJsonObject record = ParameterRecord(container, name);
	record["where"] = where;
	record["area"] = AreaOf(name);
	return record;
}

QJsonArray ListParameters(const QString &area, int fractalSlot, const QString &query, int limit,
	QString *error)
{
	if (limit <= 0) limit = 30;
	if (limit > 80) limit = 80;
	const QString wanted = area.trimmed().toLower();
	const QString needle = query.trimmed().toLower();
	if (fractalSlot < 1) fractalSlot = 1;
	if (fractalSlot > gParFractal->size())
	{
		if (error) *error = "fractal_slot must be 1 to 9.";
		return QJsonArray();
	}

	QJsonArray array;
	auto consider = [&](cParameterContainer *container, const QString &name, const QString &where) {
		if (array.size() >= limit) return;
		if (!needle.isEmpty() && !name.toLower().contains(needle)
			&& !Glossary(name).toLower().contains(needle) && !TooltipFor(name).toLower().contains(needle))
			return;
		if (!wanted.isEmpty() && wanted != "all" && wanted != "fractal")
		{
			if (AreaOf(name) != wanted) return;
		}
		QJsonObject record = ParameterRecord(container, name);
		record["where"] = where;
		record["area"] = AreaOf(name);
		if (record.value("description").toString().isEmpty() && needle.isEmpty() && wanted != "all")
		{
			// Still list it, but keep unnamed material slots from flooding a broad query.
			if (wanted == "materials" && !name.contains("is_defined") && !name.contains("surface_color")
				&& !name.contains("name"))
				return;
		}
		array.append(record);
	};

	if (wanted == "fractal" || wanted.isEmpty())
	{
		const int formulaId = gParFractal->at(fractalSlot - 1)->Get<int>("formula");
		QString internalName;
		for (cAbstractFractal *formula : newFractalList)
		{
			if (int(formula->getInternalId()) == formulaId)
			{
				internalName = QString::fromStdString(formula->getInternalName());
				break;
			}
		}
		sFormulaInfo *info = FindFormula(internalName);
		if (info) EnsureFormulaParsed(info);
		cParameterContainer *slot = gParFractal->at(fractalSlot - 1).get();
		if (info)
		{
			for (const sUiParam &uiParam : info->parameters)
				consider(slot, uiParam.name, QString("fractal_slot_%1").arg(fractalSlot));
		}
		const QStringList commonNames = {"formula_iterations", "formula_weight", "power", "bailout",
			"use_default_bailout", "julia_mode", "julia_c"};
		for (const QString &commonName : commonNames)
			consider(slot, commonName, QString("fractal_slot_%1").arg(fractalSlot));
		if (wanted == "fractal") return array;
	}

	const QList<QString> names = gPar->GetListOfParameters();
	for (const QString &name : names)
		consider(gPar.get(), name, "main");
	return array;
}

} // namespace mcpCatalog
