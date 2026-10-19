#include "preset_card.h"

#include <QApplication>
#include <QBuffer>
#include <QEvent>
#include <QPixmap>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

@interface PresetDragImageView : NSImageView
@end

@implementation PresetDragImageView
- (NSView *)hitTest:(NSPoint)point
{
	Q_UNUSED(point);
	return nil;
}
@end

class cPresetFloater
{
public:
	NSImageView *imageView;
};

static NSImage *imageFromPixmap(const QPixmap &pixmap)
{
	if (pixmap.isNull()) return nil;
	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	if (!pixmap.save(&buffer, "PNG")) return nil;
	NSData *data = [[NSData alloc] initWithBytes:bytes.constData() length:(NSUInteger)bytes.size()];
	NSImage *image = [[NSImage alloc] initWithData:data];
	[data release];
	return image;
}

namespace
{
// A zoomed main window is FullScreenPrimary. AppKit then reports dialogs and
// combo popups as visible and on the active space, but the window server does
// not composite them. Ordering the panel in front with the auxiliary behavior
// puts it on the same screen as the main window.
void raisePanel(QWidget *widget)
{
	if (!widget || !widget->isVisible()) return;
	widget->winId();
	QWindow *handle = widget->windowHandle();
	if (!handle) return;
	NSView *view = (__bridge NSView *)reinterpret_cast<void *>(handle->winId());
	NSWindow *window = view.window;
	if (!window || window == NSApp.mainWindow) return;
	@try
	{
		window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace
			| NSWindowCollectionBehaviorManaged | NSWindowCollectionBehaviorFullScreenAuxiliary
			| NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
		[window orderFrontRegardless];
	}
	@catch (NSException *exception)
	{
		Q_UNUSED(exception);
	}
}

class cPanelSpaceFix : public QObject
{
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() != QEvent::Show) return false;
		QWidget *widget = qobject_cast<QWidget *>(watched);
		if (!widget || !widget->isWindow()) return false;
		const Qt::WindowType type = widget->windowType();
		if (type != Qt::Dialog && type != Qt::Popup && type != Qt::Tool) return false;
		QTimer::singleShot(0, widget, [widget]() { raisePanel(widget); });
		return false;
	}
};
} // namespace

void presetInstallPanelFix()
{
	static bool installed = false;
	if (installed || !qApp) return;
	installed = true;
	qApp->installEventFilter(new cPanelSpaceFix(qApp));
}

static NSView *stripView(QWidget *strip)
{
	if (!strip) return nil;
	strip->setAttribute(Qt::WA_NativeWindow);
	NSView *view = (__bridge NSView *)reinterpret_cast<void *>(strip->winId());
	return view;
}

static NSRect viewRect(NSView *view, const QRect &local)
{
	if (!view) return NSZeroRect;
	NSRect frame = NSMakeRect(local.x(), local.y(), local.width(), local.height());
	if (!view.isFlipped)
		frame.origin.y = NSHeight(view.bounds) - local.y() - local.height();
	return frame;
}

cPresetFloater *presetFloaterCreate(QWidget *strip, const QPixmap &pixmap, const QRect &local)
{
	NSView *parent = stripView(strip);
	NSImage *image = imageFromPixmap(pixmap);
	if (!parent || !image || local.isEmpty())
	{
		[image release];
		return nullptr;
	}

	NSImageView *imageView = [[PresetDragImageView alloc] initWithFrame:viewRect(parent, local)];
	imageView.imageScaling = NSImageScaleAxesIndependently;
	imageView.image = image;
	[image release];
	imageView.wantsLayer = YES;
	[parent addSubview:imageView positioned:NSWindowAbove relativeTo:nil];

	auto *floater = new cPresetFloater;
	floater->imageView = imageView;
	[imageView release];
	return floater;
}

void presetFloaterSetPixmap(cPresetFloater *floater, const QPixmap &pixmap)
{
	if (!floater || !floater->imageView) return;
	NSImage *image = imageFromPixmap(pixmap);
	floater->imageView.image = image;
	[image release];
}

void presetFloaterMove(cPresetFloater *floater, QWidget *strip, const QRect &local)
{
	if (!floater || !floater->imageView || local.isEmpty()) return;
	NSView *parent = stripView(strip);
	if (!parent) return;
	floater->imageView.frame = viewRect(parent, local);
}

void presetFloaterRaise(cPresetFloater *floater)
{
	if (!floater || !floater->imageView || !floater->imageView.superview) return;
	[floater->imageView.superview addSubview:floater->imageView positioned:NSWindowAbove relativeTo:nil];
}

void presetFloaterDestroy(cPresetFloater *floater)
{
	if (!floater) return;
	[floater->imageView removeFromSuperview];
	delete floater;
}

bool presetFloaterCoversStrip(cPresetFloater *floater, QWidget *strip, const QRect &stripRect)
{
	Q_UNUSED(stripRect);
	NSView *parent = stripView(strip);
	if (!floater || !floater->imageView || !floater->imageView.image || !parent) return false;
	return floater->imageView.superview == parent;
}
