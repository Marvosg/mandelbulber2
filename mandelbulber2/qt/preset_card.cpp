#include "preset_card.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QWindow>
#include <QEasingCurve>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QVariantAnimation>

class cPresetStrip;

void presetInstallPanelFix();
cPresetFloater *presetFloaterCreate(QWidget *strip, const QPixmap &pixmap, const QRect &local);
void presetFloaterSetPixmap(cPresetFloater *floater, const QPixmap &pixmap);
void presetFloaterMove(cPresetFloater *floater, QWidget *strip, const QRect &local);
void presetFloaterRaise(cPresetFloater *floater);
void presetFloaterDestroy(cPresetFloater *floater);
bool presetFloaterCoversStrip(cPresetFloater *floater, QWidget *strip, const QRect &stripRect);

#include "thumbnail_widget.h"

#ifndef Q_OS_MACOS
void presetInstallPanelFix() {}
cPresetFloater *presetFloaterCreate(QWidget *, const QPixmap &, const QRect &)
{
	return nullptr;
}
void presetFloaterSetPixmap(cPresetFloater *, const QPixmap &) {}
void presetFloaterMove(cPresetFloater *, QWidget *, const QRect &) {}
void presetFloaterRaise(cPresetFloater *) {}
void presetFloaterDestroy(cPresetFloater *) {}
bool presetFloaterCoversStrip(cPresetFloater *, QWidget *, const QRect &)
{
	return false;
}
#endif

namespace
{
QPoint eventGlobalPos(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return event->globalPosition().toPoint();
#else
	return event->globalPos();
#endif
}

const int kBadgeVisual = 18;
const int kBadgeBox = 22;
const qreal kStroke = 2.0;
const qreal kShadowBlur = 5.0;
const int kShadowAlpha = 28;
const qreal kLiftBlur = 1.25;
const int kLiftAlpha = 8;
const int kOutlineOutset = int(std::ceil(kStroke)) + 1;
// One gutter, used as the gap between thumbnails and as the padding around the row.
// The badge is centered on the thumbnail corner, so the gutter matches that overhang.
const int kGutter = kBadgeBox / 2;
const int kPadLeft = kOutlineOutset;
const int kPadTop = kGutter;
const int kPadRight = kGutter;
const int kPadBottom = kOutlineOutset;
// Negative: the outline pad of the next card tucks under this card's empty right pad,
// so the thumbnail gap stays kGutter instead of the sum of both cards' padding.
const int kCardSpacing = kGutter - kPadLeft - kPadRight;
const int kStripLeft = kGutter - kPadLeft;
const int kStripTop = kGutter - kPadTop;
const int kStripRight = kGutter - kPadRight;
const int kStripBottom = kGutter - kPadBottom;
// Extra room before the first agent card. The user's presets stay on the left of it.
const int kSeparatorGap = 18;
const int kAgentMark = 12;

qreal smoothstep(qreal edge0, qreal edge1, qreal value)
{
	const qreal t = qBound(0.0, (value - edge0) / (edge1 - edge0), 1.0);
	return t * t * (3.0 - 2.0 * t);
}

qreal roundedRectDistance(const QPointF &point, const QRectF &rect, qreal radius)
{
	const qreal rad = qMin(radius, qMin(rect.width(), rect.height()) * 0.5);
	const QPointF half(rect.width() * 0.5 - rad, rect.height() * 0.5 - rad);
	const QPointF delta(std::abs(point.x() - rect.center().x()) - half.x(),
		std::abs(point.y() - rect.center().y()) - half.y());
	const qreal outside = std::hypot(qMax(delta.x(), 0.0), qMax(delta.y(), 0.0));
	const qreal inside = qMin(qMax(delta.x(), delta.y()), 0.0);
	return outside + inside - rad;
}

QColor mixColors(const QColor &a, const QColor &b, qreal t)
{
	t = qBound(0.0, t, 1.0);
	return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
		int(a.blue() + (b.blue() - a.blue()) * t));
}

int nativeButtonCornerRadius(const QWidget *reference)
{
	static int cached = -1;
	if (cached >= 0) return cached;

	QPushButton button(QStringLiteral("Button"));
	button.setAttribute(Qt::WA_DontShowOnScreen);
	if (reference) button.setStyle(reference->style());
	button.ensurePolished();
	const int height = qMax(button.sizeHint().height(), 16);
	button.resize(qMax(button.sizeHint().width(), 72), height);

	QImage image(button.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	button.render(&image);

	int minX = image.width();
	int minY = image.height();
	int maxX = 0;
	int maxY = 0;
	for (int y = 0; y < image.height(); y++)
	{
		const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = 0; x < image.width(); x++)
		{
			if (qAlpha(line[x]) < 40) continue;
			minX = qMin(minX, x);
			minY = qMin(minY, y);
			maxX = qMax(maxX, x);
			maxY = qMax(maxY, y);
		}
	}

	int radius = 0;
	if (maxX > minX && maxY > minY)
	{
		const QRgb *top = reinterpret_cast<const QRgb *>(image.constScanLine(minY));
		for (int x = minX; x <= maxX; x++)
		{
			if (qAlpha(top[x]) >= 40)
			{
				radius = x - minX;
				break;
			}
		}
	}

	const int bezelHeight = qMax(1, maxY - minY);
	if (radius < 2 || radius > bezelHeight / 2) radius = 6;
	cached = radius;
	return cached;
}

class cPresetRemoveButton : public QWidget
{
public:
	explicit cPresetRemoveButton(QWidget *parent) : QWidget(parent)
	{
		setFixedSize(kBadgeBox, kBadgeBox);
		setCursor(Qt::PointingHandCursor);
		setMouseTracking(true);
		setToolTip(tr("Remove preset"));
		hotAnim = new QVariantAnimation(this);
		hotAnim->setDuration(120);
		hotAnim->setEasingCurve(QEasingCurve::OutCubic);
		connect(hotAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
			hot = value.toReal();
			update();
			if (onHot) onHot(hot);
		});
	}

	std::function<void()> onClick;
	std::function<void(qreal)> onHot;
	qreal amount = 0;
	qreal hot = 0;
	bool pressed = false;

	void setAmount(qreal value)
	{
		amount = value;
		update();
	}

	void syncUnderCursor()
	{
		setHot(isVisible() && rect().contains(mapFromGlobal(QCursor::pos())));
	}

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	void enterEvent(QEnterEvent *event) override
#else
	void enterEvent(QEvent *event) override
#endif
	{
		QWidget::enterEvent(event);
		setHot(true);
	}

	void leaveEvent(QEvent *event) override
	{
		QWidget::leaveEvent(event);
		setHot(false);
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton)
		{
			pressed = true;
			event->accept();
			update();
			return;
		}
		QWidget::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		const bool fire =
			pressed && event->button() == Qt::LeftButton && rect().contains(event->pos());
		pressed = false;
		update();
		if (fire && onClick) onClick();
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setOpacity(amount);
		const QRectF disc((width() - kBadgeVisual) / 2.0, (height() - kBadgeVisual) / 2.0, kBadgeVisual,
			kBadgeVisual);
		const QPointF center = disc.center();
		painter.translate(center);
		painter.scale(1.0 + 0.12 * hot, 1.0 + 0.12 * hot);
		painter.translate(-center);

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0, 0, 0, pressed ? 40 : 55));
		painter.drawEllipse(disc.adjusted(0.5, 1.2, 0.5, 1.2));
		const QColor fill = mixColors(pressed ? QColor(236, 236, 236) : QColor(255, 255, 255),
			QColor(198, 36, 36), hot);
		painter.setBrush(fill);
		painter.setPen(QPen(mixColors(QColor(0, 0, 0, 110), QColor(120, 16, 16), hot), 1));
		painter.drawEllipse(disc);

		painter.setPen(QPen(mixColors(QColor(28, 28, 28), QColor(255, 255, 255), hot), 1.7, Qt::SolidLine,
			Qt::RoundCap));
		const qreal cy = disc.center().y();
		painter.drawLine(QPointF(disc.center().x() - 4.0, cy), QPointF(disc.center().x() + 4.0, cy));
	}

private:
	void setHot(bool on)
	{
		hotAnim->stop();
		hotAnim->setStartValue(hot);
		hotAnim->setEndValue(on ? 1.0 : 0.0);
		hotAnim->start();
	}

	QVariantAnimation *hotAnim;
};

class cPresetOutline : public QWidget
{
public:
	explicit cPresetOutline(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
	}

	qreal cardRadius = 6;
	QColor color = Qt::transparent;
	qreal alpha = 0;

	void setAppearance(const QColor &nextColor, qreal nextAlpha)
	{
		color = nextColor;
		alpha = nextAlpha;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (alpha <= 0.001) return;
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QColor stroked = color;
		stroked.setAlphaF(qBound(0.0, alpha, 1.0));
		QPen pen(stroked);
		pen.setWidthF(kStroke);
		painter.setPen(pen);
		painter.setBrush(Qt::NoBrush);
		// Widget is the card plus a full stroke and one spare pixel. The centerline
		// sits half a stroke outside the card, and the radius grows by the same
		// amount so the corners stay concentric.
		const qreal margin = std::ceil(kStroke) + 1.0 - kStroke * 0.5;
		const QRectF path = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
		painter.drawRoundedRect(path, cardRadius + kStroke * 0.5, cardRadius + kStroke * 0.5);
	}
};

class cPresetAgentMark : public QWidget
{
public:
	explicit cPresetAgentMark(QWidget *parent) : QWidget(parent)
	{
		setFixedSize(kAgentMark, kAgentMark);
		setToolTip(tr("Saved by the agent"));
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QColor color = palette().color(QPalette::Active, QPalette::Highlight);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(rect().adjusted(1, 1, -1, -1));
	}
};

} // namespace

class cPresetCard : public QWidget
{
public:
	cPresetCard(cThumbnailWidget *thumb, const QString &filename, int radius, QWidget *parent)
			: QWidget(parent), thumbWidget(thumb), fileName(filename), cornerRadius(radius)
	{
		setAttribute(Qt::WA_NoSystemBackground);
		setAutoFillBackground(false);
		setMouseTracking(true);
		setToolTip(tr("Toolbar settings: ") + filename);

		imageRect = QRect(kPadLeft, kPadTop, thumb->width(), thumb->height());
		setFixedSize(kPadLeft + thumb->width() + kPadRight, kPadTop + thumb->height() + kPadBottom);

		thumb->setParent(this);
		thumb->setAttribute(Qt::WA_TransparentForMouseEvents);
		thumb->SetCornerRadius(radius);
		thumb->setGeometry(imageRect);
		thumb->show();

		outline = new cPresetOutline(this);
		outline->cardRadius = radius;
		outline->setGeometry(
			imageRect.adjusted(-kOutlineOutset, -kOutlineOutset, kOutlineOutset, kOutlineOutset));

		removeButton = new cPresetRemoveButton(this);
		removeButton->move(imageRect.x() + imageRect.width() - kBadgeBox / 2,
			imageRect.y() - kBadgeBox / 2);
		removeButton->hide();
		removeButton->onClick = [this]() {
			if (onRemove) onRemove();
		};
		removeButton->onHot = [this](qreal amount) {
			badgeAmount = amount;
			refreshOutline();
		};

		agentMark = new cPresetAgentMark(this);
		agentMark->setAttribute(Qt::WA_TransparentForMouseEvents);
		agentMark->move(imageRect.x() + 5, imageRect.bottom() - kAgentMark - 4);
		agentMark->hide();

		outline->raise();
		removeButton->raise();
		agentMark->raise();

		hoverAnim = new QVariantAnimation(this);
		hoverAnim->setDuration(160);
		hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
		connect(hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
			setHoverAmount(value.toReal());
		});

		liftAnim = new QVariantAnimation(this);
		liftAnim->setDuration(180);
		liftAnim->setEasingCurve(QEasingCurve::OutCubic);
		connect(liftAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
			liftValue = value.toReal();
			applyRemoveButton();
			refreshOutline();
			if (onAppearance)
				onAppearance();
			else if (parentWidget())
				parentWidget()->update(effectsRect(pos()));
		});
	}

	QString filename() const { return fileName; }
	cThumbnailWidget *thumbnail() const { return thumbWidget; }
	bool isAgent() const { return agent; }
	void setAgent(bool on)
	{
		agent = on;
		agentMark->setVisible(on);
		setToolTip(on ? tr("Agent preset: ") + fileName : tr("Toolbar settings: ") + fileName);
	}
	QRect previewRect() const { return imageRect; }
	int previewRadius() const { return cornerRadius; }
	qreal liftAmount() const { return liftValue; }

	void cacheAppearance()
	{
		// grab() on a unified-toolbar widget skips the thumbnail, so the drag
		// window would show only the shadow. Draw the preview image in explicitly.
		if (!isVisible() || appearanceReady) return;
		removeButton->hide();
		const qreal dpr = devicePixelRatioF();
		QPixmap pix(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
		pix.setDevicePixelRatio(dpr);
		pix.fill(Qt::transparent);
		render(&pix);
		if (cThumbnailWidget *thumb = findChild<cThumbnailWidget *>())
		{
			const QImage preview = thumb->displayedPreview();
			if (!preview.isNull())
			{
				QPainter painter(&pix);
				painter.drawImage(QRectF(imageRect), preview);
			}
		}
		appearance = pix;
		appearanceReady = true;
	}
	void clearAppearance()
	{
		appearance = QPixmap();
		appearanceReady = false;
	}
	bool hasAppearance() const { return appearanceReady; }
	const QPixmap &appearancePixmap() const { return appearance; }
	QRect effectBounds(const QPoint &topLeft) const { return effectsRect(topLeft); }

	// The sprite depends on lift and size, not on where the card sits, so a move
	// can blit it instead of rasterizing a shadow for every card.
	const QImage &shadowSprite(qreal dpr, QPointF *offset) const
	{
		if (shadowImage.isNull() || shadowDpr != dpr || shadowLift != liftValue
				|| shadowRadius != cornerRadius || shadowSize != imageRect.size())
		{
			const qreal blur = kShadowBlur + liftValue * kLiftBlur;
			const int peak = int(kShadowAlpha + liftValue * kLiftAlpha);
			const QRectF image(imageRect);
			const QRectF bounds = image.adjusted(-blur - 1.0, -blur - 1.0, blur + 1.0, blur + 1.0);
			const QSize deviceSize(
				qMax(1, qRound(bounds.width() * dpr)), qMax(1, qRound(bounds.height() * dpr)));
			QImage sprite(deviceSize, QImage::Format_ARGB32_Premultiplied);
			sprite.setDevicePixelRatio(dpr);
			sprite.fill(Qt::transparent);

			for (int y = 0; y < sprite.height(); ++y)
			{
				QRgb *line = reinterpret_cast<QRgb *>(sprite.scanLine(y));
				for (int x = 0; x < sprite.width(); ++x)
				{
					const QPointF point(bounds.left() + (x + 0.5) / dpr, bounds.top() + (y + 0.5) / dpr);
					const qreal dist = roundedRectDistance(point, image, cornerRadius);
					if (dist <= 0.0 || dist >= blur) continue;
					const int alpha = int(std::lround(smoothstep(blur, 0.0, dist) * peak));
					line[x] = qRgba(0, 0, 0, alpha);
				}
			}

			shadowImage = sprite;
			shadowOffset = bounds.topLeft();
			shadowDpr = dpr;
			shadowLift = liftValue;
			shadowRadius = cornerRadius;
			shadowSize = imageRect.size();
		}
		*offset = shadowOffset;
		return shadowImage;
	}

	void setLifted(bool lifted)
	{
		liftedLook = lifted;
		liftAnim->stop();
		liftAnim->setStartValue(liftValue);
		liftAnim->setEndValue(lifted ? 1.0 : 0.0);
		liftAnim->start();
		applyRemoveButton();
		refreshOutline();
		if (!lifted) updatePointer();
	}

	// The strip takes the mouse once the card is hidden, so this widget never
	// sees the release. Clear the drag cursor here or it comes back on show.
	void finishDrag()
	{
		dragging = false;
		updatePointer();
	}

	void setSelected(bool on)
	{
		if (selected == on) return;
		selected = on;
		refreshOutline();
	}

	std::function<void()> onPressed;
	std::function<void(const QPoint &)> onDragStarted;
	std::function<void(const QPoint &)> onDragMoved;
	std::function<void()> onDragFinished;
	std::function<void()> onClicked;
	std::function<void()> onRemove;
	std::function<void()> onRestack;
	std::function<void()> onAppearance;

	bool isHot() const { return cardHot || liftedLook; }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	void enterEvent(QEnterEvent *event) override
#else
	void enterEvent(QEvent *event) override
#endif
	{
		QWidget::enterEvent(event);
		updatePointer();
	}

	void leaveEvent(QEvent *event) override
	{
		QWidget::leaveEvent(event);
		updatePointer();
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton) return;
		if (!imageRect.contains(event->pos())) return;
		pressPos = eventGlobalPos(event);
		dragging = false;
		event->accept();
		if (onPressed) onPressed();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		updatePointer();
		if (!(event->buttons() & Qt::LeftButton)) return;
		const QPoint global = eventGlobalPos(event);
		if (!dragging && (global - pressPos).manhattanLength() >= 8)
		{
			dragging = true;
			if (onDragStarted) onDragStarted(global);
		}
		if (dragging && onDragMoved) onDragMoved(global);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton) return;
		const bool wasDragging = dragging;
		dragging = false;
		if (wasDragging)
		{
			if (onDragFinished) onDragFinished();
		}
		else if (onClicked)
		{
			onClicked();
		}
		updatePointer();
	}

	void moveEvent(QMoveEvent *event) override
	{
		QWidget::moveEvent(event);
		// The shadow is painted on the strip and extends past this widget, so moving
		// the widget alone leaves the old fringe behind.
		if (QWidget *parent = parentWidget())
		{
			parent->update(effectsRect(event->oldPos()));
			parent->update(effectsRect(pos()));
		}
	}

private:
	void updatePointer()
	{
		const QPoint local = mapFromGlobal(QCursor::pos());
		const bool overImage = imageRect.contains(local);
		const QRect badge = removeButton->geometry().adjusted((kBadgeBox - kBadgeVisual) / 2,
			(kBadgeBox - kBadgeVisual) / 2, -(kBadgeBox - kBadgeVisual) / 2,
			-(kBadgeBox - kBadgeVisual) / 2);
		const bool overBadge = !liftedLook && removeButton->isVisible() && badge.contains(local);
		setCardHot(overImage || overBadge);
		if (overImage)
			setCursor(Qt::PointingHandCursor);
		else
			unsetCursor();
	}

	void setCardHot(bool on)
	{
		if (cardHot == on) return;
		cardHot = on;
		hoverAnim->stop();
		hoverAnim->setStartValue(hoverAmount);
		hoverAnim->setEndValue(on ? 1.0 : 0.0);
		hoverAnim->start();
	}

	QRect effectsRect(const QPoint &origin) const
	{
		const qreal blur = kShadowBlur + kLiftBlur + 2.0;
		const QRectF image = QRectF(imageRect).translated(origin);
		return image.adjusted(-blur, -blur, blur, blur).toAlignedRect().adjusted(-2, -2, 2, 2);
	}

	void applyRemoveButton()
	{
		// Lift eases the same way hover does, so the badge fades instead of popping.
		const qreal shown = hoverAmount * (1.0 - liftValue);
		removeButton->setAmount(shown);
		const bool wasVisible = removeButton->isVisible();
		const bool visible = shown > 0.01;
		removeButton->setVisible(visible);
		if (visible && !wasVisible) removeButton->syncUnderCursor();
		if (visible)
			raise();
		else if (!liftedLook && onRestack)
			onRestack();
	}

	void setHoverAmount(qreal amount)
	{
		hoverAmount = amount;
		applyRemoveButton();
		refreshOutline();
		if (onAppearance) onAppearance();
	}

	void refreshOutline()
	{
		const QColor hoverColor = palette().color(QPalette::Active, QPalette::Highlight);
		const QColor danger(198, 36, 36);
		qreal alpha = selected ? 0.38 : 0.0;
		QColor color = hoverColor;
		if (liftedLook)
			alpha = 1.0;
		else if (hoverAmount > 0.0)
			alpha = qMax(alpha, hoverAmount);
		if (!liftedLook && badgeAmount > 0.0)
		{
			color = mixColors(hoverColor, danger, badgeAmount);
			alpha = qMax(alpha, badgeAmount);
		}
		outline->setAppearance(color, alpha);
	}

	void changeEvent(QEvent *event) override
	{
		QWidget::changeEvent(event);
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ActivationChange)
			refreshOutline();
	}

	cPresetOutline *outline;
	cPresetRemoveButton *removeButton;
	cPresetAgentMark *agentMark = nullptr;
	cThumbnailWidget *thumbWidget = nullptr;
	QVariantAnimation *hoverAnim;
	QVariantAnimation *liftAnim;
	QString fileName;
	QRect imageRect;
	QPoint pressPos;
	int cornerRadius;
	qreal hoverAmount = 0;
	qreal badgeAmount = 0;
	qreal liftValue = 0;
	bool cardHot = false;
	bool dragging = false;
	bool liftedLook = false;
	bool selected = false;
	bool agent = false;
	mutable QImage shadowImage;
	mutable QPointF shadowOffset;
	mutable QSize shadowSize;
	mutable qreal shadowDpr = 0;
	mutable qreal shadowLift = -1;
	mutable int shadowRadius = -1;
	QPixmap appearance;
	bool appearanceReady = false;
};

cPresetStrip::cPresetStrip(QWidget *parent)
		: QWidget(parent),
			draggedCard(nullptr),
			cardWidth(0),
			cardHeight(0),
			spacing(kCardSpacing),
			separatorIndex(-1)
{
	setAttribute(Qt::WA_NoSystemBackground);
	setAutoFillBackground(false);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	presetInstallPanelFix();
}

cPresetStrip::~cPresetStrip()
{
	clearDragCursor();
	if (QApplication *app = qApp) app->removeEventFilter(this);
}

void cPresetStrip::sync(const QList<cPresetStripItem> &ordered, bool forceRebuild)
{
	if (forceRebuild)
	{
		for (cPresetCard *card : cards)
		{
			stopAnimation(card);
			stopSlide(card);
			card->deleteLater();
		}
		cards.clear();
		draggedCard = nullptr;
		if (pixmapMode) releaseMouse();
		if (qApp) qApp->removeEventFilter(this);
		destroyFloaters();
		pixmapMode = false;
		clearDragCursor();
	}

	QHash<QString, cPresetCard *> existing;
	for (cPresetCard *card : cards)
		existing.insert(card->filename(), card);

	const int radius = nativeButtonCornerRadius(this);
	QList<cPresetCard *> next;
	for (const cPresetStripItem &item : ordered)
	{
		cPresetCard *card = existing.take(item.filename);
		if (!card)
		{
			if (!item.thumbnail) continue;
			const int thumbLimit = qMin(item.thumbnail->width(), item.thumbnail->height()) / 2 - 2;
			const int cardRadius = qBound(2, radius, qMax(2, thumbLimit));
			card = new cPresetCard(item.thumbnail, item.filename, cardRadius, this);
			card->onPressed = [this, card]() { onPressed(card); };
			card->onDragStarted = [this, card](const QPoint &pos) { onDragStarted(card, pos); };
			card->onDragMoved = [this, card](const QPoint &pos) { onDragMoved(card, pos); };
			card->onDragFinished = [this, card]() { onDragFinished(card); };
			card->onClicked = [this, card]() { onClicked(card); };
			card->onRemove = [this, card]() { emit removeRequested(card->filename()); };
			card->onRestack = [this]() { restack(); };
			card->onAppearance = [this, card]() {
				if (!pixmapMode || floaters.isEmpty()) return;
				updateFloater(card);
			};
			card->show();
		}
		card->setAgent(item.agent);
		next.append(card);
	}

	for (cPresetCard *card : existing)
	{
		stopAnimation(card);
		if (draggedCard == card) draggedCard = nullptr;
		card->deleteLater();
	}

	cards = next;
	separatorIndex = -1;
	for (int i = 0; i < cards.size(); i++)
	{
		if (cards.at(i)->isAgent())
		{
			separatorIndex = i;
			break;
		}
	}
	if (!cards.isEmpty())
	{
		cardWidth = cards.first()->width();
		cardHeight = cards.first()->height();
	}
	relayout(false);
}

QStringList cPresetStrip::filenames() const
{
	QStringList names;
	for (cPresetCard *card : cards)
		names.append(card->filename());
	return names;
}

bool cPresetStrip::isDragging() const
{
	return pixmapMode;
}

cThumbnailWidget *cPresetStrip::thumbnailFor(const QString &filename) const
{
	for (cPresetCard *card : cards)
	{
		if (card->filename() == filename) return card->thumbnail();
	}
	return nullptr;
}

int cPresetStrip::homeX(int index) const
{
	int x = kStripLeft + index * (cardWidth + spacing);
	if (separatorIndex >= 0 && index >= separatorIndex) x += kSeparatorGap;
	return x;
}

void cPresetStrip::paintSeparator(QPainter *painter) const
{
	if (separatorIndex < 0 || cards.isEmpty() || cardHeight <= 0) return;
	int x = kStripLeft + kSeparatorGap / 2;
	if (separatorIndex > 0)
		x = (homeX(separatorIndex - 1) + cardWidth + homeX(separatorIndex)) / 2;
	const int top = kStripTop + cardHeight / 5;
	const int bottom = kStripTop + (cardHeight * 4) / 5;
	QColor color = palette().color(QPalette::Mid);
	color.setAlpha(180);
	QPen pen(color);
	pen.setWidth(2);
	pen.setCapStyle(Qt::RoundCap);
	painter->setPen(pen);
	painter->drawLine(x, top, x, bottom);
}

void cPresetStrip::relayout(bool animate)
{
	for (int i = 0; i < cards.size(); i++)
	{
		cPresetCard *card = cards.at(i);
		if (card == draggedCard) continue;
		const QPoint target(homeX(i), kStripTop);
		if (animate && pixmapMode)
			slideVisual(card, target);
		else if (animate)
			animateTo(card, target);
		else
		{
			stopAnimation(card);
			stopSlide(card);
			card->move(target);
		}
	}
	const int extra = separatorIndex >= 0 ? kSeparatorGap : 0;
	const int width = cards.isEmpty() ? 0
																		: kStripLeft + cards.size() * cardWidth
																				+ (cards.size() - 1) * spacing + extra + kStripRight;
	const int height = cards.isEmpty() ? 0 : kStripTop + cardHeight + kStripBottom;
	const QSize nextSize(qMax(0, width), height);
	// A toolbar relayout here is a full-window hitch. Skip it while a drag is only reordering.
	if (size() != nextSize)
	{
		setFixedSize(nextSize);
		updateGeometry();
	}
	if (!pixmapMode) restack();
}

void cPresetStrip::restack()
{
	static bool busy = false;
	if (busy) return;
	busy = true;
	for (cPresetCard *card : cards)
		card->raise();
	for (cPresetCard *card : cards)
	{
		if (card->isHot()) card->raise();
	}
	if (draggedCard) draggedCard->raise();
	busy = false;
}

void cPresetStrip::animateTo(cPresetCard *card, const QPoint &target)
{
	if (card->pos() == target) return;
	QPropertyAnimation *animation = animations.value(card);
	if (!animation)
	{
		animation = new QPropertyAnimation(card, "pos", this);
		animation->setDuration(220);
		animation->setEasingCurve(QEasingCurve::OutCubic);
		animations.insert(card, animation);
		connect(animation, &QPropertyAnimation::finished, this, [this]() { tryFinishPresentation(); });
	}
	animation->stop();
	animation->setStartValue(card->pos());
	animation->setEndValue(target);
	animation->start();
}

void cPresetStrip::slideVisual(cPresetCard *card, const QPoint &target)
{
	const QPoint from = visualPosition(card);
	if (from == target) return;
	QVariantAnimation *animation = slideAnims.value(card);
	if (!animation)
	{
		animation = new QVariantAnimation(this);
		animation->setDuration(220);
		animation->setEasingCurve(QEasingCurve::OutCubic);
		slideAnims.insert(card, animation);
		connect(animation, &QVariantAnimation::valueChanged, this, [this, card](const QVariant &value) {
			slidePos.insert(card, value.toPoint());
			placeFloater(card);
		});
		connect(animation, &QVariantAnimation::finished, this, [this, card, animation]() {
			slideAnims.remove(card);
			animation->deleteLater();
			tryFinishPresentation();
		});
	}
	animation->stop();
	slidePos.insert(card, from);
	animation->setStartValue(from);
	animation->setEndValue(target);
	animation->start();
}

void cPresetStrip::stopSlide(cPresetCard *card)
{
	if (QVariantAnimation *animation = slideAnims.take(card))
	{
		animation->stop();
		animation->deleteLater();
	}
	slidePos.remove(card);
}

QPoint cPresetStrip::visualPosition(cPresetCard *card) const
{
	if (pixmapMode && card == draggedCard) return dragVisual;
	const auto found = slidePos.constFind(card);
	if (found != slidePos.cend()) return found.value();
	return card->pos();
}

QRect cPresetStrip::paintedBounds(cPresetCard *card, const QPoint &origin) const
{
	return card->effectBounds(origin).united(QRect(origin, card->size()));
}

void cPresetStrip::tryFinishPresentation()
{
	if (!pixmapMode || draggedCard) return;
	for (QVariantAnimation *animation : slideAnims)
	{
		if (animation->state() == QAbstractAnimation::Running) return;
	}
	for (QPropertyAnimation *animation : animations)
	{
		if (animation->state() == QAbstractAnimation::Running) return;
	}

	pixmapMode = false;
	// Take the overlay windows down before the real cards paint, or both shadows show.
	destroyFloaters();
	for (int i = 0; i < cards.size(); ++i)
	{
		cPresetCard *card = cards.at(i);
		stopSlide(card);
		card->move(QPoint(homeX(i), kStripTop));
		card->clearAppearance();
		card->unsetCursor();
		card->show();
	}
	slidePos.clear();
	restack();
	update();
}

void cPresetStrip::stopAnimation(cPresetCard *card)
{
	if (QPropertyAnimation *animation = animations.take(card))
	{
		animation->stop();
		animation->deleteLater();
	}
}

int cPresetStrip::indexAt(int centerX) const
{
	if (cards.isEmpty() || cardWidth <= 0) return 0;
	int best = 0;
	int bestDist = -1;
	for (int i = 0; i < cards.size(); i++)
	{
		const int dist = std::abs(centerX - (homeX(i) + cardWidth / 2));
		if (bestDist < 0 || dist < bestDist)
		{
			best = i;
			bestDist = dist;
		}
	}
	return best;
}

void cPresetStrip::onPressed(cPresetCard *card)
{
	orderAtPress = filenames();
	Q_UNUSED(card);
}

void cPresetStrip::onDragStarted(cPresetCard *card, const QPoint &globalPos)
{
	draggedCard = card;
	stopAnimation(card);
	pixmapMode = true;
	dragOffset = card->pos() - mapFromGlobal(globalPos);
	dragVisual = card->pos();
	card->setLifted(true);
	for (cPresetCard *other : cards)
	{
		stopAnimation(other);
		stopSlide(other);
		other->cacheAppearance();
	}
	if (!ensureFloaters())
	{
		destroyFloaters();
		pixmapMode = false;
		card->raise();
		return;
	}
	// Hiding the card drops its mouse grab. Watch the application so the drag
	// keeps moving after that, including when the cursor is over the overlay.
	qApp->installEventFilter(this);
	QGuiApplication::setOverrideCursor(Qt::ClosedHandCursor);
	dragCursorActive = true;
	for (cPresetCard *other : cards)
		other->hide();
	grabMouse();
}

void cPresetStrip::onDragMoved(cPresetCard *card, const QPoint &globalPos)
{
	if (card != draggedCard) return;
	QPoint next = mapFromGlobal(globalPos) + dragOffset;
	next.setY(kStripTop);
	const int minX = kStripLeft;
	const int maxX = qMax(minX, width() - kStripRight - card->width());
	next.setX(qBound(minX, next.x(), maxX));
	if (next == dragVisual) return;
	dragVisual = next;
	if (pixmapMode)
		placeFloater(card);
	else
	{
		card->move(dragVisual);
		card->raise();
	}

	const int from = cards.indexOf(card);
	int to = indexAt(dragVisual.x() + card->width() / 2);
	// Agent cards stay on their side of the separator.
	if (separatorIndex >= 0 && from >= 0)
	{
		if (card->isAgent())
			to = qMax(to, separatorIndex);
		else if (separatorIndex > 0)
			to = qMin(to, separatorIndex - 1);
	}
	if (from >= 0 && to != from)
	{
		cards.move(from, to);
		relayout(true);
	}
}

void cPresetStrip::onDragFinished(cPresetCard *card)
{
	if (draggedCard != card) return;
	const QPoint droppedAt = dragVisual;
	const int index = cards.indexOf(card);
	draggedCard = nullptr;
	releaseMouse();
	clearDragCursor();
	card->finishDrag();
	if (qApp) qApp->removeEventFilter(this);
	if (index >= 0) slidePos.insert(card, droppedAt);
	card->setLifted(false);
	if (index >= 0)
	{
		const QPoint home(homeX(index), kStripTop);
		if (pixmapMode)
			slideVisual(card, home);
		else
			animateTo(card, home);
	}
	if (pixmapMode) tryFinishPresentation();
	if (filenames() != orderAtPress) emit orderChanged(filenames());
}

void cPresetStrip::mouseMoveEvent(QMouseEvent *event)
{
	if (pixmapMode && draggedCard) onDragMoved(draggedCard, eventGlobalPos(event));
}

void cPresetStrip::mouseReleaseEvent(QMouseEvent *event)
{
	if (pixmapMode && draggedCard && event->button() == Qt::LeftButton)
		onDragFinished(draggedCard);
}

bool cPresetStrip::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == qApp && pixmapMode && draggedCard)
	{
		if (event->type() == QEvent::MouseMove)
		{
			onDragMoved(draggedCard, eventGlobalPos(static_cast<QMouseEvent *>(event)));
			return true;
		}
		// Let the release through. Swallowing it leaves Qt's button grab and the
		// closed-hand cursor in place for the rest of the window.
		if (event->type() == QEvent::MouseButtonRelease
				&& static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
			return false;
	}
	return QWidget::eventFilter(watched, event);
}

void cPresetStrip::clearDragCursor()
{
	if (dragCursorActive)
	{
		QGuiApplication::restoreOverrideCursor();
		dragCursorActive = false;
	}
	unsetCursor();
	// setCursor on a card before this strip has its own window writes the
	// closed hand onto the main window. Drop has to clear that explicitly.
	if (QWidget *top = window())
	{
		if (QWindow *handle = top->windowHandle()) handle->unsetCursor();
	}
	if (QWindow *handle = windowHandle()) handle->unsetCursor();
}

void cPresetStrip::onClicked(cPresetCard *card)
{
	emit presetActivated(card->filename());
}

void cPresetStrip::setSelectedFiles(const QStringList &filenames)
{
	for (cPresetCard *card : cards)
		card->setSelected(filenames.contains(card->filename()));
}

void cPresetStrip::refreshDrag(const QRect &rect)
{
	if (!floaters.isEmpty()) return;
	update(rect);
}

namespace
{
QPixmap composeCard(cPresetCard *card, bool withShadow)
{
	const QRect bounds = card->effectBounds(QPoint(0, 0));
	const qreal dpr = card->devicePixelRatioF();
	QPixmap pix(qMax(1, qRound(bounds.width() * dpr)), qMax(1, qRound(bounds.height() * dpr)));
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);
	QPainter painter(&pix);
	if (withShadow)
	{
		QPointF shadowOffset;
		const QImage &shadow = card->shadowSprite(dpr, &shadowOffset);
		painter.drawImage(shadowOffset - QPointF(bounds.topLeft()), shadow);
	}
	if (card->hasAppearance()) painter.drawPixmap(-bounds.topLeft(), card->appearancePixmap());
	return pix;
}

bool pixmapHasCoverage(const QPixmap &pixmap)
{
	if (pixmap.isNull() || pixmap.size().isEmpty()) return false;
	const QImage image = pixmap.toImage();
	const int x0 = image.width() / 4;
	const int x1 = (image.width() * 3) / 4;
	const int y0 = image.height() / 4;
	const int y1 = (image.height() * 3) / 4;
	int opaque = 0;
	for (int y = y0; y < y1; y += qMax(1, (y1 - y0) / 6))
	{
		for (int x = x0; x < x1; x += qMax(1, (x1 - x0) / 6))
		{
			if (qAlpha(image.pixel(x, y)) > 200) ++opaque;
		}
	}
	return opaque >= 4;
}

} // namespace

bool cPresetStrip::ensureFloaters()
{
	destroyFloaters();
	for (cPresetCard *card : cards)
	{
		const QPixmap face = composeCard(card, card == draggedCard);
		if (!pixmapHasCoverage(face))
		{
			destroyFloaters();
			return false;
		}
		cPresetFloater *floater =
			presetFloaterCreate(this, face, card->effectBounds(visualPosition(card)));
		if (!floater)
		{
			destroyFloaters();
			return false;
		}
		floaters.insert(card, floater);
	}
	if (cPresetFloater *dragged = floaters.value(draggedCard)) presetFloaterRaise(dragged);
	if (!presetFloaterCoversStrip(floaters.value(draggedCard), this, QRect()))
	{
		destroyFloaters();
		return false;
	}
	return true;
}

void cPresetStrip::placeFloater(cPresetCard *card)
{
	cPresetFloater *floater = floaters.value(card);
	if (!floater)
	{
		refreshDrag(paintedBounds(card, visualPosition(card)));
		return;
	}
	presetFloaterMove(floater, this, card->effectBounds(visualPosition(card)));
	if (card == draggedCard) presetFloaterRaise(floater);
}

void cPresetStrip::updateFloater(cPresetCard *card)
{
	cPresetFloater *floater = floaters.value(card);
	if (!floater) return;
	presetFloaterSetPixmap(floater, composeCard(card, card == draggedCard));
	placeFloater(card);
}

void cPresetStrip::destroyFloaters()
{
	for (cPresetFloater *floater : floaters)
		presetFloaterDestroy(floater);
	floaters.clear();
}

void cPresetStrip::paintSnapshots(QPainter *painter, const QRect &dirty)
{
	const qreal dpr = devicePixelRatioF();
	if (draggedCard)
	{
		QPointF offset;
		const QImage &shadow = draggedCard->shadowSprite(dpr, &offset);
		const QPointF topLeft = QPointF(visualPosition(draggedCard)) + offset;
		const QSizeF size(shadow.width() / dpr, shadow.height() / dpr);
		if (dirty.intersects(QRectF(topLeft, size).toAlignedRect())) painter->drawImage(topLeft, shadow);
	}
	cPresetCard *raised = nullptr;
	for (cPresetCard *card : cards)
	{
		if (card == draggedCard)
		{
			raised = card;
			continue;
		}
		if (!card->hasAppearance() || !card->isHidden()) continue;
		const QPoint origin = visualPosition(card);
		if (!dirty.intersects(QRect(origin, card->size()))) continue;
		painter->drawPixmap(origin, card->appearancePixmap());
	}
	if (raised && raised->hasAppearance())
	{
		const QPoint origin = visualPosition(raised);
		if (dirty.intersects(QRect(origin, raised->size())))
			painter->drawPixmap(origin, raised->appearancePixmap());
	}
}

void cPresetStrip::paintEvent(QPaintEvent *event)
{
	if (!floaters.isEmpty()) return;
	QPainter painter(this);
	paintSeparator(&painter);
	paintSnapshots(&painter, event->rect());
}
