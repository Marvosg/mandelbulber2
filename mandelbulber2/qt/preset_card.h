#ifndef MANDELBULBER2_QT_PRESET_CARD_H_
#define MANDELBULBER2_QT_PRESET_CARD_H_

#include <QHash>
#include <QList>
#include <QMouseEvent>
#include <QStringList>
#include <QWidget>

class QPainter;

class QPropertyAnimation;
class QVariantAnimation;
class cThumbnailWidget;
class cPresetCard;
class cPresetFloater;

struct cPresetStripItem
{
	QString filename;
	cThumbnailWidget *thumbnail;
	bool agent = false;
};

// Horizontal row of fractal preset cards. Owns the cards and their thumbnails.
class cPresetStrip : public QWidget
{
	Q_OBJECT

public:
	explicit cPresetStrip(QWidget *parent = nullptr);

	void sync(const QList<cPresetStripItem> &ordered, bool forceRebuild);
	QStringList filenames() const;
	void setSelectedFiles(const QStringList &filenames);
	bool isDragging() const;
	cThumbnailWidget *thumbnailFor(const QString &filename) const;

protected:
	~cPresetStrip() override;
	void paintEvent(QPaintEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

signals:
	void presetActivated(const QString &filename);
	void removeRequested(const QString &filename);
	void orderChanged(const QStringList &filenames);

private:
	void relayout(bool animate);
	void restack();
	void animateTo(cPresetCard *card, const QPoint &target);
	void stopAnimation(cPresetCard *card);
	void slideVisual(cPresetCard *card, const QPoint &target);
	void stopSlide(cPresetCard *card);
	QPoint visualPosition(cPresetCard *card) const;
	QRect paintedBounds(cPresetCard *card, const QPoint &origin) const;
	void tryFinishPresentation();
	void refreshDrag(const QRect &rect);
	int homeX(int index) const;
	void paintSeparator(QPainter *painter) const;
	bool ensureFloaters();
	void placeFloater(cPresetCard *card);
	void updateFloater(cPresetCard *card);
	void destroyFloaters();
	void paintSnapshots(QPainter *painter, const QRect &dirty);
	int indexAt(int centerX) const;

	void onPressed(cPresetCard *card);
	void onDragStarted(cPresetCard *card, const QPoint &globalPos);
	void onDragMoved(cPresetCard *card, const QPoint &globalPos);
	void onDragFinished(cPresetCard *card);
	void clearDragCursor();
	void onClicked(cPresetCard *card);

	QList<cPresetCard *> cards;
	QHash<cPresetCard *, QPropertyAnimation *> animations;
	QHash<cPresetCard *, QVariantAnimation *> slideAnims;
	QHash<cPresetCard *, QPoint> slidePos;
	cPresetCard *draggedCard;
	QPoint dragOffset;
	QPoint dragVisual;
	QStringList orderAtPress;
	QHash<cPresetCard *, cPresetFloater *> floaters;
	int cardWidth;
	int cardHeight;
	int spacing;
	int separatorIndex = -1;
	bool pixmapMode = false;
	bool dragCursorActive = false;
};

#endif /* MANDELBULBER2_QT_PRESET_CARD_H_ */
