#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QLockFile>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <cstdio>
#include <unistd.h>

using Status = QMap<QString, QString>;

static Status parseStatus(const QByteArray &bytes)
{
    Status status;
    if (bytes.size() > 8192) return status;
    for (const QByteArray &line : bytes.split('\n')) {
        const qsizetype split = line.indexOf('=');
        if (split > 0)
            status.insert(QString::fromUtf8(line.left(split)), QString::fromUtf8(line.mid(split + 1)));
    }
    return status;
}

static int number(const Status &status, const QString &key)
{
    bool valid = false;
    const int result = status.value(key).toInt(&valid);
    return valid ? result : -1;
}

static QString temperatureText(const Status &status)
{
    const int temp = number(status, "cpu_temp");
    return temp < 0 ? "Unavailable" : QString::number(temp / 1000.0, 'f', 1) + " °C";
}

static QString rpmText(const Status &status)
{
    const int rpm = number(status, "cpu_rpm");
    return rpm < 0 ? "Unavailable" : QString::number(rpm) + " RPM";
}

static QString tooltip(const Status &status)
{
    const bool active = status.value("daemon") == "online" && number(status, "enabled") == 1;
    const bool ac = number(status, "ac") == 1;
    const QString mode = number(status, "cpu_fan_mode") == 0 ? "Firmware maximum" :
                         number(status, "cpu_fan_mode") == 2 ? "Firmware automatic" : "Unavailable";
    return QString("Fan Monitor\nFan: %1\nCPU: %2\n%3 · %4\n%5\n"
                   "Maximum-fan trigger: 80 °C\nDesired: 7900–8000 RPM; exact RPM locking unavailable.")
        .arg(rpmText(status), temperatureText(status),
             ac ? "Plugged in" : number(status, "ac") == 0 ? "Battery" : "Power source unknown",
             mode, active ? "Fan control service active" : "Read-only monitoring: fan control service inactive");
}

static QPixmap iconPixmap(const Status &status)
{
    const int temp = number(status, "cpu_temp");
    const int rpm = number(status, "cpu_rpm");
    QPixmap image(128, 128);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(temp >= 90000 ? QColor("#b91c1c") :
                     temp >= 80000 ? QColor("#92400e") : QColor("#115e59"));
    painter.drawRoundedRect(2, 2, 124, 124, 20, 20);
    painter.setPen(Qt::white);
    QFont font("DejaVu Sans");
    font.setBold(true);
    font.setPixelSize(42);
    painter.setFont(font);
    const QString speed = rpm < 0 ? "?" : QString::number(rpm / 1000.0, 'f', 1) + "k";
    painter.drawText(QRect(4, 8, 120, 62), Qt::AlignCenter, speed);
    font.setPixelSize(34);
    painter.setFont(font);
    painter.drawText(QRect(4, 69, 120, 48), Qt::AlignCenter,
                     temp < 0 ? "CPU ?" : QString::number(temp / 1000) + "°");
    return image;
}

class FanMonitor final : public QObject {
    QSystemTrayIcon tray{this};
    QMenu menu;
    QProcess probe{this};
    QTimer timer{this}, timeout{this};
    QActionGroup profiles{this};
    QAction *summary, *policy, *service;
    const QString directory = QCoreApplication::applicationDirPath();
    const QString socket;

    QStringList clientArguments(const QStringList &arguments) const
    {
        return socket.isEmpty() ? arguments : QStringList{"--socket", socket} + arguments;
    }

    void render(const Status &status)
    {
        tray.setIcon(QIcon(iconPixmap(status)));
        tray.setToolTip(tooltip(status));
        summary->setText("Fan: " + rpmText(status) + "  |  CPU: " + temperatureText(status));
        const bool active = status.value("daemon") == "online" && number(status, "enabled") == 1;
        policy->setText(!active ? "Firmware control — maximum fan request inactive" :
                        number(status, "ac") == 1 ? "Plugged in: maximum fan requested" :
                        number(status, "fan_temperature_boost") == 1 ? "80 °C cooling trigger active" :
                        number(status, "ac") == 0 ? "On battery" : "Power source unknown");
        service->setText(active ? "Fan control service active" : "Monitoring only — service inactive");
        profiles.setEnabled(status.value("daemon") == "online");
        for (QAction *action : profiles.actions())
            action->setChecked(active && action->data().toString() == status.value("profile"));
    }

    void refresh()
    {
        if (probe.state() != QProcess::NotRunning) return;
        probe.start(directory + "/fanctl", clientArguments({"status"}));
        timeout.start(4000);
    }

    void selectProfile(const QString &profile)
    {
        auto *process = new QProcess(this);
        connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
            if (code != 0) {
                QString error = QString::fromUtf8(process->readAllStandardError());
                if (error.isEmpty()) error = QString::fromUtf8(process->readAllStandardOutput());
                tray.showMessage("Profile could not be applied", error,
                                 QSystemTrayIcon::Warning, 5000);
            }
            process->deleteLater();
            refresh();
        });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                tray.showMessage("Fan Control Center", process->errorString(), QSystemTrayIcon::Warning);
                process->deleteLater();
            }
        });
        process->start(directory + "/fanctl", clientArguments({"profile", profile}));
        QTimer::singleShot(5000, process, [process] {
            if (process->state() != QProcess::NotRunning) process->kill();
        });
    }

public:
    explicit FanMonitor(QString testSocket) : socket(std::move(testSocket))
    {
        summary = menu.addAction("Reading fan RPM and CPU temperature…");
        policy = menu.addAction("Power source: reading…");
        service = menu.addAction("Connecting to fan control service…");
        summary->setEnabled(false); policy->setEnabled(false); service->setEnabled(false);
        menu.addSeparator();
        auto open = [this] { QProcess::startDetached(directory + "/fan-control-gui", {}); };
        menu.addAction("Open Fan Control Center", this, open);
        const QStringList ids{"power-saving", "medium", "performance", "automatic"};
        const QStringList labels{"Power Saving", "Medium Throttle", "Performance", "Automatic"};
        for (qsizetype i = 0; i < ids.size(); ++i) {
            QAction *action = menu.addAction(labels[i]);
            action->setCheckable(true);
            action->setData(ids[i]);
            profiles.addAction(action);
            connect(action, &QAction::triggered, this, [this, id = ids[i]] { selectProfile(id); });
        }
        profiles.setExclusive(true);
        menu.addSeparator();
        menu.addAction("Quit monitor", qApp, &QApplication::quit)
            ->setToolTip("The background fan control service keeps running.");
        tray.setContextMenu(&menu);
        connect(&tray, &QSystemTrayIcon::activated, this,
                [open](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) open();
                });
        connect(&probe, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            timeout.stop();
            if (code == 0) render(parseStatus(probe.readAllStandardOutput()));
            else {
                render({});
                tray.setToolTip("Fan monitoring unavailable: " + QString::fromUtf8(probe.readAllStandardError()));
            }
        });
        connect(&probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                timeout.stop(); render({});
                tray.setToolTip("Fan monitoring unavailable: " + probe.errorString());
            }
        });
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [this] { probe.kill(); });
        connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
        render({});
        tray.show();
        timer.start(2000);
        if (socket.isEmpty()) {
            auto *starter = new QProcess(this);
            connect(starter, &QProcess::finished, this, [this, starter](int, QProcess::ExitStatus) {
                starter->deleteLater();
                refresh();
            });
            connect(starter, &QProcess::errorOccurred, starter, &QObject::deleteLater);
            starter->start(directory + "/fanctl", {"--start-user-daemon"});
        }
        refresh();
    }
};

static int selfTest(const QString &imagePath)
{
    const Status sample = parseStatus("daemon=online\nenabled=1\ncpu_temp=80000\ncpu_rpm=6300\n"
                                     "ac=1\ncpu_fan_mode=0\nfan_desired_min_rpm=7900\n");
    const QString tip = tooltip(sample);
    if (!tip.contains("Fan: 6300 RPM") || !tip.contains("CPU: 80.0 °C") ||
        !tip.contains("exact RPM locking unavailable") || rpmText({}) != "Unavailable" ||
        number(parseStatus("cpu_rpm=bad\n"), "cpu_rpm") != -1) {
        std::fputs("FAIL: monitor telemetry formatting or unavailable readings\n", stderr);
        return 1;
    }
    const QPixmap icon = iconPixmap(sample);
    if (icon.isNull() || icon.size() != QSize(128, 128) ||
        (!imagePath.isEmpty() && !icon.save(imagePath, "PNG"))) {
        std::fputs("FAIL: monitor icon rendering\n", stderr);
        return 1;
    }
    std::puts("PASS monitor uses measured RPM, formats temperature, handles missing sensors and renders the taskbar icon");
    return 0;
}

int main(int argc, char **argv)
{
    if (geteuid() == 0) { std::fputs("Run the fan monitor as your desktop user.\n", stderr); return 1; }
    QApplication app(argc, argv);
    app.setApplicationName("fan-monitor");
    app.setApplicationDisplayName("Fan Monitor");
    app.setDesktopFileName("fan-monitor");
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    QString socket, imagePath;
    bool testing = false;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        if (arguments[i] == "--self-test") testing = true;
        else if (arguments[i] == "--socket" && i + 1 < arguments.size()) socket = arguments[++i];
        else if (arguments[i] == "--screenshot" && i + 1 < arguments.size()) imagePath = arguments[++i];
        else { std::fputs("Unknown monitor argument\n", stderr); return 1; }
    }
    if (testing) return selfTest(imagePath);
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/fan-control-monitor.lock");
    if (!lock.tryLock(100)) {
        if (lock.error() == QLockFile::LockFailedError) return 0;
        std::fputs("Cannot create the user-session monitor lock.\n", stderr);
        return 1;
    }
    FanMonitor monitor(socket);
    return app.exec();
}
