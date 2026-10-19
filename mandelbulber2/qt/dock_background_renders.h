/*
 * Mandelbulber v2 — main and agent background-render queues.
 * Copyright (C) 2026 Mandelbulber Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MANDELBULBER2_QT_DOCK_BACKGROUND_RENDERS_H_
#define MANDELBULBER2_QT_DOCK_BACKGROUND_RENDERS_H_

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;
class MyProgressBar;

class cDockBackgroundRenders : public QWidget
{
	Q_OBJECT
public:
	explicit cDockBackgroundRenders(QWidget *parent = nullptr);
	void Refresh();
	QSize sizeHint() const override;

public slots:
	void QueueFromWindow();
	void OpenPreview(const QString &id);

signals:
	// A new background job started while this dock was closed.
	void requestShow();

private:
	struct sRow
	{
		QWidget *card = nullptr;
		QLabel *thumb = nullptr;
		QLabel *title = nullptr;
		QLabel *summary = nullptr;
		QLabel *detail = nullptr;
		MyProgressBar *bar = nullptr;
		QPushButton *stop = nullptr;
		int thumbGeneration = -1;
		QString lane;
	};

	QFrame *MakeCard(const QString &id, sRow *row);
	void UpdateRow(sRow &row, const QString &title, const QString &summary, const QString &detail,
		double progress, bool showStop, const QImage &thumbnail, int thumbGeneration);

	QLabel *mainIdle = nullptr;
	QLabel *agentIdle = nullptr;
	QVBoxLayout *mainLayout = nullptr;
	QVBoxLayout *agentLayout = nullptr;
	QHash<QString, sRow> rows;
	QSet<QString> seen;
	QHash<QString, QPointer<QWidget>> previews;
	QTimer *timer = nullptr;
};

#endif /* MANDELBULBER2_QT_DOCK_BACKGROUND_RENDERS_H_ */
