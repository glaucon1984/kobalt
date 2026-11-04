#pragma once
#include <QPixmap>
#include <QWidget>

namespace kobalt::pocketbook {
// The boot logo's page while a reopened book loads, with a small loading bar at the top-left
// over the page's header, where the firmware draws its own while it launches an app. The page
// is a capture, not live, and the device answers nothing until the book is open: the bar says
// so, and how far along the open is (the controller's opening progress). Only the bar's
// rectangle is repainted as it fills, so each step is a small partial e-ink update.
class BootPage final : public QWidget {
    Q_OBJECT
public:
    explicit BootPage(QWidget *parent = nullptr);
    // The captured page (logical content, already the size of this widget); the bar starts empty.
    void setPage(const QImage &page);
    // 0-100, never going back.
    void setProgress(int percent);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap page;
    bool darkPaper = false; // a dark-mode capture gets a dark band with a light bar
    int percent = 0;
    // Pixels for a length given at 300 dpi.
    int px(int at300) const;
    // The band over the header (full width, the header's height) and the bar inside it.
    QRect bandRect() const;
    QRect barRect() const;
    int fillWidth() const;
};
}
