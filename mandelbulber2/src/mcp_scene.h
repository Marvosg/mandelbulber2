/*
 * Mandelbulber v2 — MCP scene, look, animation, and render-job commands.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MANDELBULBER2_SRC_MCP_SCENE_H_
#define MANDELBULBER2_SRC_MCP_SCENE_H_

#include <QJsonObject>
#include <QString>

namespace mcpScene
{
// True when this method was handled. *result is the reply object (with ok:false on errors).
bool Dispatch(const QString &method, const QJsonObject &params, QJsonObject *result);
// A preview, still, or animation is in progress. Mutating commands should wait.
bool JobActive();
}

#endif /* MANDELBULBER2_SRC_MCP_SCENE_H_ */
