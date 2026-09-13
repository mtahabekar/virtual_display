#include "../monitors.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <iostream>

class Pattern : public QWidget {
public:
    explicit Pattern(int monitor) : id(monitor) {
        setWindowTitle(QString("QUEST-%1 capture test").arg(id + 1));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        elapsed.start();
        timer.setTimerType(Qt::PreciseTimer);
        connect(&timer, &QTimer::timeout, this, [this] { update(); });
        timer.start(16);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.scale(width() / 2560.0, height() / 1440.0);
        const QColor colors[] = {QColor("#153043"), QColor("#273a23"), QColor("#39273c")};
        p.fillRect(QRect(0, 0, 2560, 1440), colors[id]);
        p.setPen(QColor("#ffffff"));
        p.setFont(QFont("DejaVu Sans", 48, QFont::Bold));
        p.drawText(80, 120, QString("QUEST-%1  /  2560 × 1440").arg(id + 1));
        p.setFont(QFont("DejaVu Sans Mono", 22));
        p.drawText(80, 190, QString("Host capture test  •  monitor ID %1  •  elapsed %2 ms").arg(id).arg(elapsed.elapsed()));
        for (int size : {10, 12, 14, 18, 24}) {
            p.setFont(QFont("DejaVu Sans Mono", size));
            p.drawText(80, 250 + size * 15, QString("%1 pt: const monitor = QUEST-%2; 0123456789 []{} <> != #@$%").arg(size).arg(id + 1));
        }
        const QColor bars[] = {Qt::red, Qt::green, Qt::blue, Qt::cyan, Qt::magenta, Qt::yellow, Qt::white, Qt::black};
        for (int i = 0; i < 8; ++i) p.fillRect(80 + i * 280, 720, 280, 180, bars[i]);
        p.setPen(QColor("#7890a0"));
        for (int x = 80; x < 2400; x += 20) p.drawLine(x, 950, x, 1100);
        const int x = 80 + (elapsed.elapsed() / 4) % 2240;
        p.fillRect(x, 1170, 100, 140, QColor("#ffffff"));
        p.setFont(QFont("DejaVu Sans", 16));
        p.drawText(80, 1400, "Ordinary 2D application rendered by KWin. No Quest connection is needed.");
    }
private:
    int id;
    QTimer timer;
    QElapsedTimer elapsed;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) { std::cerr << "Usage: quest-test-pattern MONITOR_ID|all SECONDS\n"; return 2; }
    bool ok; const int seconds = args[2].toInt(&ok);
    if (!ok || seconds < 1 || seconds > 3600) return 2;
    int count = 0;
    for (int id = 0; id < quest::MonitorCount; ++id) {
        if (args[1] != "all" && args[1] != QString::number(id)) continue;
        for (auto *screen : app.screens()) {
            if (screen->name() != QString("Virtual-QUEST-%1").arg(id + 1) && screen->name() != QString("QUEST-%1").arg(id + 1)) continue;
            auto *window = new Pattern(id);
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->winId();
            window->windowHandle()->setScreen(screen);
            window->setGeometry(screen->geometry());
            window->showFullScreen();
            QTimer::singleShot(1000, window, [window, screen] {
                const auto actual = window->windowHandle()->screen();
                std::cout << "Pattern target=" << screen->name().toStdString()
                          << " actual=" << actual->name().toStdString() << std::endl;
                if (actual != screen) QCoreApplication::exit(3);
            });
            ++count;
        }
    }
    if (!count) { std::cerr << "No matching Quest screen\n"; return 1; }
    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
