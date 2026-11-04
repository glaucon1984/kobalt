#include "boot_page.h"
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>

namespace kobalt::pocketbook {
namespace {
// Measured on a 300 dpi capture: Kobalt's header runs from row 15 to 37 and the body's first
// ascenders start at row 64, so a 52 px band covers the header with room to spare and nothing of
// the text. The bar sits where the header's text starts (30 px in) at the header's own height.
constexpr int bandHeight = 52, barLeft = 30, barTop = 16, barWidth = 320, barHeight = 20, barBorder = 2;
}

BootPage::BootPage(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}

int BootPage::px(int at300) const {
    const QScreen *screen = QGuiApplication::primaryScreen();
    const qreal dpi = screen ? screen->physicalDotsPerInch() : 300;
    return qMax(1, qRound(at300 * (dpi > 0 ? dpi : 300) / 300.0));
}

QRect BootPage::bandRect() const {
    return QRect(0, 0, width(), px(bandHeight));
}

QRect BootPage::barRect() const {
    return QRect(px(barLeft), px(barTop), px(barWidth), px(barHeight));
}

int BootPage::fillWidth() const {
    const int inset = 2 * px(barBorder);
    return (barRect().width() - 2 * inset) * percent / 100;
}

void BootPage::setPage(const QImage &image) {
    page = QPixmap::fromImage(image);
    darkPaper = !image.isNull() && qGray(image.pixel(0, 0)) < 128;
    percent = 0;
    update();
}

void BootPage::setProgress(int value) {
    value = qBound(0, value, 100);
    if (value <= percent)
        return;
    const int before = fillWidth();
    percent = value;
    if (fillWidth() != before)
        update(barRect());
}

void BootPage::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setClipRect(event->rect());
    if (page.isNull())
        painter.fillRect(rect(), Qt::white);
    else
        painter.drawPixmap(0, 0, page);
    const QColor paper = darkPaper ? Qt::black : Qt::white;
    const QColor ink = darkPaper ? Qt::white : Qt::black;
    painter.fillRect(bandRect(), paper);
    const QRect bar = barRect();
    const int border = px(barBorder);
    painter.fillRect(bar, ink);
    painter.fillRect(bar.adjusted(border, border, -border, -border), paper);
    const QRect inner = bar.adjusted(2 * border, 2 * border, -2 * border, -2 * border);
    painter.fillRect(QRect(inner.x(), inner.y(), fillWidth(), inner.height()), ink);
}
}
