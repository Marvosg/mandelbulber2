/*
 * Mandelbulber v2 — MCP formula and parameter catalogue.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Human-readable descriptions for an agent that has to explain fractals
 * to someone who does not know the parameter names. Sources, in order:
 * formula .ui files shipped in the share directory (labels, tooltips,
 * info panel, example filenames), the in-memory parameter containers
 * (type, default, min/max, enums), and a small glossary for the camera,
 * quality, light, and effect controls that live in the main window.
 */

#ifndef MANDELBULBER2_SRC_MCP_CATALOG_H_
#define MANDELBULBER2_SRC_MCP_CATALOG_H_

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class cParameterContainer;

namespace mcpCatalog
{
void EnsureLoaded();

QJsonArray ListFormulas(const QString &query, int limit);
QJsonObject DescribeFormula(const QString &name, QString *error);

QJsonArray ListExamples(const QString &query, int limit);
QString ResolveExample(const QString &query, QString *error);

QJsonObject DescribeParameter(
	const QString &name, int fractalSlot, QString *error);
QJsonArray ListParameters(const QString &area, int fractalSlot, const QString &query, int limit,
	QString *error);

QString Glossary(const QString &name);
QString TooltipFor(const QString &name);

QJsonObject ParameterRecord(cParameterContainer *container, const QString &name);
} // namespace mcpCatalog

#endif /* MANDELBULBER2_SRC_MCP_CATALOG_H_ */
