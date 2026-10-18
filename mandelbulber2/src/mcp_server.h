/*
 * Mandelbulber v2 — local MCP command socket.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * QLocalServer on a user-only Unix socket (named pipe on Windows).
 * The Python MCP adapter speaks newline-delimited JSON. Every handler
 * runs on the Qt main thread.
 */

#ifndef MANDELBULBER2_SRC_MCP_SERVER_H_
#define MANDELBULBER2_SRC_MCP_SERVER_H_

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>

class QLocalServer;
class QLocalSocket;

class cMcpServer : public QObject
{
	Q_OBJECT
public:
	explicit cMcpServer(QObject *parent = nullptr);
	~cMcpServer() override;

	// Returns false when another live instance already owns the socket.
	bool Start();
	// Undo baseline. Called after autosave recovery so the first undo is the
	// scene the user actually kept, not the empty one from before the dialog.
	void NoteBaseline();

private slots:
	void OnNewConnection();
	void OnReadyRead();
	void OnDisconnected();

private:
	void WriteSessionFile();
	void RemoveSessionFile();
	QByteArray HandleLine(const QByteArray &line);
	QJsonObject Dispatch(const QString &method, const QJsonObject &params);

	QLocalServer *server;
	QHash<QLocalSocket *, QByteArray> buffers;
	QString socketPath;
	QString sessionPath;
	QString lastPreviewPath;
	int previewSerial;
	bool owningSession;
};

extern cMcpServer *gMcpServer;

#endif /* MANDELBULBER2_SRC_MCP_SERVER_H_ */
