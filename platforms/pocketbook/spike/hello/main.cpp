// Spike: a minimal Qt Widgets app on a PocketBook, drawn and driven through inkview by the
// statically linked "pbink" platform plugin. Shows device facts, counts taps and key presses,
// and exercises a popup (QComboBox) so that a second top-level window is composed too.
#include <QtWidgets>
#include <QtPlugin>
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <cstdio>
#include <inkview.h> // has its own extern "C" guards

Q_IMPORT_PLUGIN(PbInkIntegrationPlugin)

static FILE *logFile = nullptr;

static void logHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *level = type == QtDebugMsg ? "D" : type == QtInfoMsg ? "I" : type == QtWarningMsg ? "W" : "E";
    const QByteArray line = QDateTime::currentDateTime().toString("HH:mm:ss.zzz ").toUtf8() + level + " "
                            + msg.toUtf8() + "\n";
    fputs(line.constData(), stderr);
    if (logFile) {
        fputs(line.constData(), logFile);
        fflush(logFile);
    }
    if (type == QtFatalMsg)
        abort();
}

class Page : public QWidget
{
public:
    explicit Page(const QString &openedFile)
    {
        auto layout = new QVBoxLayout(this);
        layout->setContentsMargins(40, 40, 40, 40);
        layout->setSpacing(24);

        auto title = new QLabel("Qt 5.15 on PocketBook");
        QFont big = title->font();
        big.setPointSize(big.pointSize() * 2);
        big.setBold(true);
        title->setFont(big);
        layout->addWidget(title);

        const QScreen *screen = QGuiApplication::primaryScreen();
        auto facts = new QLabel(QString("Model %1, firmware %2\nScreen %3x%4 at %5 dpi, battery %6%%7\n"
                                        "Qt %8, platform %9")
                                    .arg(GetDeviceModel(), GetSoftwareVersion())
                                    .arg(screen->geometry().width()).arg(screen->geometry().height())
                                    .arg(int(screen->physicalDotsPerInch()))
                                    .arg(GetBatteryPower()).arg(IsCharging() ? " (charging)" : "")
                                    .arg(qVersion(), QGuiApplication::platformName()));
        facts->setWordWrap(true);
        layout->addWidget(facts);

        if (!openedFile.isEmpty()) {
            auto opened = new QLabel("Opened with: " + openedFile);
            opened->setWordWrap(true);
            layout->addWidget(opened);
        }

        mCounter = new QLabel("Taps: 0");
        mCounter->setFont(big);
        layout->addWidget(mCounter);

        auto plus = new QPushButton("Tap me (+1)");
        plus->setMinimumHeight(120);
        connect(plus, &QPushButton::clicked, this, [this] { mCounter->setText(QString("Taps: %1").arg(++mTaps)); });
        layout->addWidget(plus);

        mKeys = new QLabel("Last key: none (page buttons, Home, Menu)");
        mKeys->setWordWrap(true);
        layout->addWidget(mKeys);

        auto combo = new QComboBox;
        combo->addItems({"Partial refresh (default)", "Fast refresh", "Full refresh each paint"});
        combo->setMinimumHeight(100);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [](int index) {
            auto fn = QGuiApplication::platformNativeInterface()->platformFunction("pbinkSetRefresh");
            if (fn)
                reinterpret_cast<void (*)(int)>(fn)(index);
        });
        layout->addWidget(combo);

        auto edit = new QLineEdit;
        edit->setPlaceholderText("A line edit (no keyboard yet)");
        edit->setMinimumHeight(100);
        layout->addWidget(edit);

        auto check = new QCheckBox("A checkbox");
        layout->addWidget(check);

        auto slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(40);
        slider->setMinimumHeight(80);
        layout->addWidget(slider);

        layout->addStretch();

        auto row = new QHBoxLayout;
        auto refresh = new QPushButton("Full refresh");
        refresh->setMinimumHeight(120);
        connect(refresh, &QPushButton::clicked, this, [] {
            auto fn = QGuiApplication::platformNativeInterface()->platformFunction("pbinkFullRefresh");
            if (fn)
                reinterpret_cast<void (*)()>(fn)();
        });
        row->addWidget(refresh);
        auto quit = new QPushButton("Quit");
        quit->setMinimumHeight(120);
        connect(quit, &QPushButton::clicked, qApp, &QCoreApplication::quit);
        row->addWidget(quit);
        layout->addLayout(row);
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        mKeys->setText(QString("Last key: 0x%1 (%2)%3")
                           .arg(e->key(), 0, 16)
                           .arg(QKeySequence(e->key()).toString())
                           .arg(e->isAutoRepeat() ? " repeat" : ""));
        if (e->key() == Qt::Key_PageDown || e->key() == Qt::Key_Right)
            mCounter->setText(QString("Taps: %1").arg(++mTaps));
        else if (e->key() == Qt::Key_PageUp || e->key() == Qt::Key_Left)
            mCounter->setText(QString("Taps: %1").arg(--mTaps));
        else if (e->key() == Qt::Key_Home || e->key() == Qt::Key_Back)
            qApp->quit();
        QWidget::keyPressEvent(e);
    }

private:
    QLabel *mCounter;
    QLabel *mKeys;
    int mTaps = 0;
};

int main(int argc, char **argv)
{
    const QString base = QStringLiteral("/mnt/ext1/applications/pbqt");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "pbink");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (base + "/fonts").toUtf8());
    // The launcher already redirects stderr to applications/pbqt/log.txt; set PBQT_LOG to log
    // to a file from here as well (for example when started from a shell).
    if (!qEnvironmentVariableIsEmpty("PBQT_LOG"))
        logFile = fopen(qgetenv("PBQT_LOG").constData(), "a");
    qInstallMessageHandler(logHandler);
    qInfo("pbqt-hello starting, argc %d", argc);

    QApplication app(argc, argv);
    app.setStyle("Fusion");
    QFont font = app.font();
    font.setPointSize(11);
    app.setFont(font);

    Page page(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
    page.showFullScreen();
    const int rc = app.exec();
    qInfo("pbqt-hello exiting with %d", rc);
    if (logFile)
        fclose(logFile);
    return rc;
}
