// TW Safe Update — background system-tray service (KDE StatusNotifierItem).
//
// A standalone daemon (run by a systemd user service) that shows a real
// StatusNotifierItem in the system tray. It reads the verdict that
// `twsu-check` writes to ~/.cache/tw-safe-update/status.json and presents it:
//
//   * LEFT click  -> a decorated popup window (verdict, reasons, collisions,
//                    buttons). Update/Details run in a Konsole terminal EMBEDDED
//                    in that same window (a Konsole KPart), not an external one.
//   * RIGHT click -> a small context menu (Check now / Update now / Details / Quit).
//
// The tray icon is hidden (Passive) unless a safe update is ready, shows a
// desktop notification when the state becomes notify-worthy, and never runs
// zypper itself for the real update — that is twsu-update, which asks for your
// password (in the embedded terminal, which is a real PTY).

#include <QApplication>
#include <KStatusNotifierItem>
#include <KParts/ReadOnlyPart>
#include <KParts/PartLoader>
#include <KPluginMetaData>
#include <kde_terminal_interface.h>
#include <QMenu>
#include <QAction>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QDir>
#include <QIcon>
#include <QLabel>
#include <QDateTime>
#include <QFrame>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QPushButton>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCloseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLocalServer>
#include <QLocalSocket>

static QString home() { return QDir::homePath(); }
static QString statusPath() { return home() + "/.cache/tw-safe-update/status.json"; }
static QString binDir()     { return home() + "/.local/bin"; }
static QString notifyStatePath() { return home() + "/.config/tw-safe-update/last-notified"; }

static QString helper(const QString &name) {
    QString p = binDir() + "/" + name;
    return QFile::exists(p) ? p : name;
}
static QString headlineFor(const QString &v) {
    if (v == "SAFE")       return QStringLiteral("Safe to update");
    if (v == "UP_TO_DATE") return QStringLiteral("System is up to date");
    if (v == "CAUTION")    return QStringLiteral("Update needs review");
    if (v == "UNSAFE")     return QStringLiteral("Not safe to update yet");
    if (v == "ERROR")      return QStringLiteral("Could not check");
    return QStringLiteral("Checking…");
}
static QString colorFor(const QString &v) {
    if (v == "SAFE" || v == "UP_TO_DATE") return "#27ae60";
    if (v == "CAUTION")                   return "#e6a700";
    if (v == "UNSAFE" || v == "ERROR")    return "#e74c3c";
    return "palette(text)";
}
static QString sevColor(const QString &s) {
    if (s == "safe")    return "#27ae60";
    if (s == "caution") return "#e6a700";
    if (s == "unsafe")  return "#e74c3c";
    return "#888888";
}

struct Config {
    QString terminal = "konsole";
    QString notify = "safe"; // safe | review | any
    QString icon = "software-update-available"; // freedesktop tray-update icon
    QString show = "safe";   // safe | always : when the tray icon is visible
    void load() {
        QFile f(home() + "/.config/tw-safe-update/tray.conf");
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        for (const QByteArray &raw : f.readAll().split('\n')) {
            QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            QString k = line.left(eq).trimmed(), v = line.mid(eq + 1).trimmed();
            if (k == "terminal") terminal = v;
            else if (k == "notify") notify = v.toLower();
            else if (k == "icon" && !v.isEmpty()) icon = v;
            else if (k == "show") show = v.toLower();
        }
    }
};

// ---- the popup window: status page + an embedded-terminal page -------------
class Panel : public QWidget {
    Q_OBJECT
public:
    Panel() : QWidget(nullptr, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                              | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint) {
        setWindowTitle("TW Update Assistant");
        setWindowIcon(QIcon::fromTheme("system-software-update"));
        resize(420, 480);
        setMinimumSize(340, 300);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        stack_ = new QStackedWidget(this);
        root->addWidget(stack_);
        stack_->addWidget(buildStatusPage());
        stack_->addWidget(buildTerminalPage());
        stack_->setCurrentWidget(statusPage_);
    }

    void setTerminal(const QString &t) { terminal_ = t; }

    void runUpdate()   { recheckOnBack_ = true;  runInTerminal(helper("twsu-update"),  "Updating — enter your password when asked"); }
    void runDetails()  { recheckOnBack_ = false; runInTerminal(helper("twsu-details"), "Details"); }
    void runFlatpaks() { recheckOnBack_ = true;  runInTerminal("flatpak update", "Flatpak updates"); }
    void showStatus() { stack_->setCurrentWidget(statusPage_); }

    void setStatus(const QJsonObject &o, bool checking) {
        const QString v = o.value("verdict").toString("UNKNOWN");
        headline_->setText(checking ? "Checking…" : headlineFor(v));
        headline_->setStyleSheet(QString("font-size:15px;font-weight:bold;color:%1;")
                                 .arg(checking ? QString("palette(text)") : colorFor(v)));
        // Hide the summary when it just repeats the headline (e.g. up-to-date).
        const QString summ = o.value("summary").toString();
        summary_->setText(summ);
        summary_->setVisible(!summ.isEmpty() && summ != headlineFor(v));

        const QString inst = o.value("installed_snapshot").toString();
        const QString latest = o.value("latest_snapshot").toString();
        QString snap;
        if (!inst.isEmpty()) {
            snap = "Snapshot " + inst;
            if (!latest.isEmpty() && latest > inst) snap += "  →  " + latest + " available";
            else snap += "  (latest)";
        }
        snapshot_->setText(snap); snapshot_->setVisible(!snap.isEmpty());

        // "Last checked" line.
        const qint64 ts = o.value("timestamp").toInteger();  // epoch secs exceed 32-bit
        if (ts > 0) {
            const QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
            meta_->setText("Last checked " + dt.toString("ddd d MMM, HH:mm"));
            meta_->setVisible(true);
        } else meta_->setVisible(false);

        update_->setEnabled(v != "UP_TO_DATE" && v != "ERROR" && !checking);

        clearBody();
        for (const QJsonValue &rv : o.value("reasons").toArray()) {
            const QJsonObject r = rv.toObject();
            addBullet(r.value("severity").toString(), r.value("text").toString());
        }
        for (const QJsonValue &cv : o.value("collisions").toArray()) {
            const QJsonObject c = cv.toObject();
            if (c.value("risk").toString() != "collision") continue;
            addCollision(c);
        }
        for (const QJsonValue &pv : o.value("problems").toArray()) {
            const QJsonObject p = pv.toObject();
            addBullet("unsafe", "✗ " + p.value("problem").toString());
        }
        // Reassuring detail when there is nothing to do.
        if (!checking && v == "UP_TO_DATE") {
            addInfo("You are on the latest snapshot — nothing to update.");
            addInfo("Checks run automatically in the background; the tray icon "
                    "only appears when a safe update is ready.");
        } else if (!checking && v == "SAFE") {
            const QJsonObject counts = o.value("counts").toObject();
            int up = counts.value("upgrade").toInt(), nw = counts.value("new").toInt();
            QString sz = o.value("download_size").toString();
            if (up || nw)
                addInfo(QString("%1 upgrades, %2 new%3. Click “Update now” to install.")
                        .arg(up).arg(nw).arg(sz.isEmpty() ? "" : "  ·  " + sz + " download"));
        }

        // Flatpak updates (independent of the zypper verdict).
        const QJsonArray fp = o.value("flatpak_updates").toArray();
        if (!fp.isEmpty()) {
            QStringList apps;
            for (const QJsonValue &fv : fp) apps << fv.toObject().value("app").toString();
            addInfo(QString("%1 Flatpak update%2 available: %3%4")
                    .arg(fp.size()).arg(fp.size() == 1 ? "" : "s")
                    .arg(apps.mid(0, 8).join(", "))
                    .arg(fp.size() > 8 ? " …" : ""));
        }
        flatpak_->setText(fp.isEmpty() ? "Update Flatpaks"
                                       : QString("Update Flatpaks (%1)").arg(fp.size()));
        flatpak_->setVisible(!fp.isEmpty() && !checking);

        bodyLayout_->addStretch();
    }

signals:
    void checkRequested();
    void recheckRequested();   // emitted after the terminal is dismissed

protected:
    // Close just hides (the tray service keeps running; the window reopens on a
    // tray click). Minimize works via the title-bar button.
    void closeEvent(QCloseEvent *e) override { e->ignore(); showStatus(); hide(); }

private:
    QWidget *buildStatusPage() {
        statusPage_ = new QWidget();
        auto *v = new QVBoxLayout(statusPage_);
        v->setContentsMargins(14, 12, 14, 12);
        v->setSpacing(6);

        auto *top = new QHBoxLayout();
        auto *ico = new QLabel();
        ico->setPixmap(QIcon::fromTheme("system-software-update").pixmap(32, 32));
        top->addWidget(ico);
        auto *tv = new QVBoxLayout();
        headline_ = new QLabel(); headline_->setStyleSheet("font-size:15px;font-weight:bold;");
        summary_ = new QLabel(); summary_->setWordWrap(true); summary_->setStyleSheet("color:palette(placeholderText);");
        tv->addWidget(headline_); tv->addWidget(summary_); tv->setSpacing(0);
        top->addLayout(tv, 1);
        v->addLayout(top);

        snapshot_ = new QLabel(); snapshot_->setStyleSheet("color:palette(placeholderText);font-size:11px;");
        v->addWidget(snapshot_);
        meta_ = new QLabel(); meta_->setStyleSheet("color:palette(placeholderText);font-size:11px;");
        v->addWidget(meta_);

        auto *line = new QFrame(); line->setFrameShape(QFrame::HLine); line->setStyleSheet("color:palette(mid);");
        v->addWidget(line);

        auto *scroll = new QScrollArea(); scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *body = new QWidget();
        bodyLayout_ = new QVBoxLayout(body);
        bodyLayout_->setContentsMargins(0, 0, 0, 0); bodyLayout_->setSpacing(8);
        bodyLayout_->addStretch();
        scroll->setWidget(body);
        v->addWidget(scroll, 1);

        auto *btns = new QHBoxLayout();
        auto *check = new QPushButton(QIcon::fromTheme("view-refresh"), "Check now");
        flatpak_ = new QPushButton(QIcon::fromTheme("flatpak"), "Update Flatpaks");
        flatpak_->setVisible(false);
        auto *details = new QPushButton(QIcon::fromTheme("utilities-terminal"), "Details");
        update_ = new QPushButton(QIcon::fromTheme("system-software-update"), "Update now");
        connect(check,    &QPushButton::clicked, this, [this]{ emit checkRequested(); });
        connect(flatpak_, &QPushButton::clicked, this, [this]{ runFlatpaks(); });
        connect(details,  &QPushButton::clicked, this, [this]{ runDetails(); });
        connect(update_,  &QPushButton::clicked, this, [this]{ runUpdate(); });
        btns->addWidget(check); btns->addStretch();
        btns->addWidget(flatpak_); btns->addWidget(details); btns->addWidget(update_);
        v->addLayout(btns);
        return statusPage_;
    }

    QWidget *buildTerminalPage() {
        termPage_ = new QWidget();
        auto *v = new QVBoxLayout(termPage_);
        v->setContentsMargins(6, 6, 6, 6); v->setSpacing(6);
        auto *bar = new QHBoxLayout();
        auto *back = new QToolButton();
        back->setIcon(QIcon::fromTheme("go-previous"));
        back->setText("Back"); back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(back, &QToolButton::clicked, this, [this]{ backToStatus(); });
        termTitle_ = new QLabel();
        termTitle_->setStyleSheet("font-weight:bold;");
        bar->addWidget(back); bar->addSpacing(6); bar->addWidget(termTitle_, 1);
        v->addLayout(bar);
        termHost_ = new QWidget();
        termHostLayout_ = new QVBoxLayout(termHost_);
        termHostLayout_->setContentsMargins(0, 0, 0, 0);
        v->addWidget(termHost_, 1);
        return termPage_;
    }

    void runInTerminal(const QString &program, const QString &title) {
        // A fresh Konsole KPart per open: destroyed on Back, so every open is a
        // clean shell with no leftover state from the previous run. We start a
        // real shell and type the command into it (the Dolphin/Kate pattern) —
        // this reliably shows output, unlike startProgram() on a hidden widget.
        if (termPart_) { delete termPart_; termPart_ = nullptr; termIface_ = nullptr; }
        KPluginMetaData md = KPluginMetaData::findPluginById(
            QStringLiteral("kf6/parts"), QStringLiteral("konsolepart"));
        if (md.isValid()) {
            auto res = KParts::PartLoader::instantiatePart<KParts::ReadOnlyPart>(md, this);
            if (res) {
                termPart_ = res.plugin;
                termIface_ = qobject_cast<TerminalInterface *>(termPart_);
                if (termIface_) termHostLayout_->addWidget(termPart_->widget());
                else { delete termPart_; termPart_ = nullptr; }
            }
        }
        if (!termPart_ || !termIface_) {   // KPart unavailable — external terminal
            if (!QProcess::startDetached(terminal_, {"-e", program})) {
                QMessageBox::warning(this, "TW Update Assistant",
                    "Could not open a terminal to run this command.\n\n"
                    "Install the Konsole terminal part (package “konsole”) so it "
                    "can run inside this window, or set a working “terminal=” in "
                    "~/.config/tw-safe-update/tray.conf.");
            }
            return;
        }
        termTitle_->setText(title);
        stack_->setCurrentWidget(termPage_);
        if (width() < 720 || height() < 480) resize(780, 540);
        show(); raise(); activateWindow();

        // Let the widget get an on-screen surface and real size, then start the
        // shell and type the command into it.
        KParts::ReadOnlyPart *part = termPart_;
        const QString cmd = program;
        QTimer::singleShot(200, this, [this, part, cmd]{
            if (termPart_ != part || !termIface_) return;
            termIface_->showShellInDir(QDir::homePath());
            QTimer::singleShot(250, this, [this, part, cmd]{
                if (termPart_ == part && termIface_)
                    termIface_->sendInput(cmd + QStringLiteral("\n"));
            });
        });
    }

    void backToStatus() {
        // Destroy the terminal so the next open is a clean, fresh run.
        if (termPart_) { delete termPart_; termPart_ = nullptr; termIface_ = nullptr; }
        stack_->setCurrentWidget(statusPage_);
        resize(420, 480);
        // Re-check only after a real update changed the system — not after
        // Details, which reads the cached verdict and changes nothing.
        if (recheckOnBack_) { recheckOnBack_ = false; emit recheckRequested(); }
    }

    void clearBody() {
        QLayoutItem *it;
        while ((it = bodyLayout_->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
    }
    void addBullet(const QString &sev, const QString &text) {
        auto *row = new QWidget();
        auto *h = new QHBoxLayout(row); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
        auto *dot = new QLabel("•"); dot->setStyleSheet(QString("color:%1;font-weight:bold;").arg(sevColor(sev)));
        dot->setAlignment(Qt::AlignTop);
        auto *lbl = new QLabel(text); lbl->setWordWrap(true); lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->addWidget(dot); h->addWidget(lbl, 1);
        bodyLayout_->addWidget(row);
    }
    void addInfo(const QString &text) {
        auto *lbl = new QLabel(text);
        lbl->setWordWrap(true);
        lbl->setStyleSheet("color:palette(placeholderText);");
        bodyLayout_->addWidget(lbl);
    }
    void addCollision(const QJsonObject &c) {
        const QJsonObject inst = c.value("installed").toObject();
        const QJsonObject cand = c.value("candidate").toObject();
        const QString name = c.value("name").toString();
        auto *box = new QFrame();
        box->setStyleSheet("QFrame{border:1px solid #e74c3c;border-radius:6px;}");
        auto *bv = new QVBoxLayout(box); bv->setContentsMargins(8, 6, 8, 6); bv->setSpacing(2);
        auto add = [&](const QString &html){ auto *l = new QLabel(html); l->setWordWrap(true);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse); bv->addWidget(l); };
        add(QString("<b style='color:#e74c3c'>⚠ name collision: %1</b>").arg(name));
        add(QString("<b>yours:</b> %1 %2 — %3").arg(inst.value("vendor").toString(),
            inst.value("version").toString(), inst.value("summary").toString()));
        add(QString("<b>repo:</b> %1 %2 — %3").arg(cand.value("vendor").toString(),
            cand.value("version").toString(), cand.value("summary").toString()));
        add(QString("keep yours: <tt>sudo zypper al %1</tt>").arg(name));
        bodyLayout_->addWidget(box);
    }

    QStackedWidget *stack_;
    QWidget *statusPage_ = nullptr, *termPage_ = nullptr, *termHost_ = nullptr;
    QVBoxLayout *bodyLayout_ = nullptr, *termHostLayout_ = nullptr;
    QLabel *headline_ = nullptr, *summary_ = nullptr, *snapshot_ = nullptr,
           *meta_ = nullptr, *termTitle_ = nullptr;
    QPushButton *update_ = nullptr, *flatpak_ = nullptr;
    KParts::ReadOnlyPart *termPart_ = nullptr;
    TerminalInterface *termIface_ = nullptr;
    bool recheckOnBack_ = false;
    QString terminal_ = "konsole";
};

// ---- the tray item --------------------------------------------------------
class Tray : public QObject {
    Q_OBJECT
public:
    Tray() {
        cfg.load();

        panel = new Panel();
        panel->setTerminal(cfg.terminal);
        panel->setAttribute(Qt::WA_NativeWindow);   // realise the platform window
        panel->winId();                             // so windowHandle() is valid below
        panel->hide();
        connect(panel, &Panel::checkRequested,   this, &Tray::runCheck);
        connect(panel, &Panel::recheckRequested, this, &Tray::runCheck);

        sni = new KStatusNotifierItem("org.opensuse.twsafeupdate", this);
        sni->setCategory(KStatusNotifierItem::SystemServices);
        sni->setStatus(KStatusNotifierItem::Active);
        sni->setIconByName(cfg.icon);
        sni->setTitle("TW Update Assistant");
        sni->setStandardActionsEnabled(false);
        sni->setContextMenu(buildMenu());
        if (panel->windowHandle())
            sni->setAssociatedWindow(panel->windowHandle());

        connect(sni, &KStatusNotifierItem::secondaryActivateRequested,
                this, [this](const QPoint &){ runCheck(); });

        QDBusConnection::sessionBus().connect(
            "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "ActionInvoked",
            this, SLOT(onNotificationAction(uint, QString)));

        // Coalesce watcher bursts (atomic writes fire several events) through one
        // restartable single-shot timer instead of reloading per raw event.
        reloadTimer = new QTimer(this);
        reloadTimer->setSingleShot(true);
        connect(reloadTimer, &QTimer::timeout, this, [this]{ rewatch(); load(); });
        watcher = new QFileSystemWatcher(this);
        rewatch();
        connect(watcher, &QFileSystemWatcher::fileChanged,      this, [this]{ reloadTimer->start(150); });
        connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this]{ reloadTimer->start(150); });

        auto *poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, &Tray::load);
        poll->start(120000);

        load();
    }

    // Bring the window up (used by the launcher entry / a second launch).
    void showWindow() { panel->showStatus(); panel->show(); panel->raise(); panel->activateWindow(); }

private slots:
    void onNotificationAction(uint id, const QString &key) {
        if (id != lastNotifId) return;
        if (key == "update") showUpdate();
        else if (key == "details") showDetails();
        else if (verdict == "SAFE") showUpdate();
        else showDetails();
    }

    void runCheck() {
        if (checkProc) return;
        checking = true; refreshUi();
        checkProc = new QProcess(this);
        connect(checkProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus){
            checkProc->deleteLater(); checkProc = nullptr;
            checking = false; load();
        });
        checkProc->start(helper("twsu-check"), {});
    }

    void showUpdate()  { showPanel(); panel->runUpdate(); }
    void showDetails() { showPanel(); panel->runDetails(); }

private:
    void showPanel() { panel->show(); panel->raise(); panel->activateWindow(); }

    QMenu *buildMenu() {
        menu = new QMenu();
        headerAction = menu->addAction(headlineFor(verdict));
        headerAction->setEnabled(false);
        menu->addSeparator();
        checkAction = menu->addAction(QIcon::fromTheme("view-refresh"), "Check now");
        connect(checkAction, &QAction::triggered, this, &Tray::runCheck);
        updateAction = menu->addAction(QIcon::fromTheme("system-software-update"), "Update now…");
        connect(updateAction, &QAction::triggered, this, &Tray::showUpdate);
        auto *det = menu->addAction(QIcon::fromTheme("utilities-terminal"), "Details…");
        connect(det, &QAction::triggered, this, &Tray::showDetails);
        menu->addSeparator();
        auto *quit = menu->addAction(QIcon::fromTheme("application-exit"), "Quit");
        connect(quit, &QAction::triggered, qApp, &QApplication::quit);
        return menu;
    }

    void rewatch() {
        const QStringList f = watcher->files(), d = watcher->directories();
        if (!f.isEmpty()) watcher->removePaths(f);
        if (!d.isEmpty()) watcher->removePaths(d);
        const QString sp = statusPath();
        watcher->addPath(QFileInfo(sp).absolutePath());
        if (QFile::exists(sp)) watcher->addPath(sp);
    }

    void refreshUi() {
        QString sub = checking ? QStringLiteral("checking…") : headlineFor(verdict);
        if (!summary.isEmpty()) sub += "\n" + summary;
        sni->setToolTip(cfg.icon, "TW Update Assistant", sub);

        bool visible = (cfg.show == "always") || (verdict == "SAFE");
        sni->setStatus(visible ? KStatusNotifierItem::Active : KStatusNotifierItem::Passive);

        if (headerAction) headerAction->setText(checking ? "Checking…" : headlineFor(verdict));
        if (checkAction)  checkAction->setEnabled(!checking);
        if (updateAction) updateAction->setEnabled(verdict != "UP_TO_DATE" && verdict != "ERROR" && !checking);
        panel->setStatus(lastStatus, checking);
    }

    void load() {
        QFile f(statusPath());
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            if (!o.isEmpty()) {
                lastStatus = o;
                verdict = o.value("verdict").toString("UNKNOWN");
                summary = o.value("summary").toString();
                statusHash = o.value("status_hash").toString();
                collisions = o.value("collisions").toArray();
            }
        }
        refreshUi();
        maybeNotify();
    }

    bool hasHardCollision() const {
        for (const QJsonValue &c : collisions)
            if (c.toObject().value("risk").toString() == "collision") return true;
        return false;
    }
    bool shouldNotify() const {
        if (verdict == "SAFE") return true;
        if (verdict == "CAUTION") return cfg.notify == "review" || cfg.notify == "any";
        if (verdict == "UNSAFE" || verdict == "ERROR")
            return cfg.notify == "any" || hasHardCollision();
        return false;
    }
    void maybeNotify() {
        if (statusHash.isEmpty() || !shouldNotify()) return;
        QString last;
        QFile lf(notifyStatePath());
        if (lf.open(QIODevice::ReadOnly)) { last = QString::fromUtf8(lf.readAll()).trimmed(); lf.close(); }
        if (last == statusHash) return;

        QString title = headlineFor(verdict), body = summary;
        bool warn = false;
        if (verdict == "SAFE") body += "\nClick to review and install.";
        else if (hasHardCollision()) {
            warn = true; title = "Update blocked: package-name collision";
            for (const QJsonValue &cv : collisions) {
                const QJsonObject c = cv.toObject();
                if (c.value("risk").toString() != "collision") continue;
                body = c.value("name").toString() +
                       ": your installed package and the repo package are different "
                       "projects. Updating could replace it. Click for details.";
                break;
            }
        } else if (verdict == "UNSAFE" || verdict == "ERROR") warn = true;

        // Only record this state as "already notified" once the notification
        // actually went out — otherwise a transient notification-daemon outage
        // would suppress the alert for this state permanently.
        if (notify(title, body, warn)) {
            QDir().mkpath(QFileInfo(notifyStatePath()).absolutePath());
            if (lf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                lf.write(statusHash.toUtf8()); lf.close();
            }
        }
    }
    bool notify(const QString &title, const QString &body, bool warning) {
        if (!notifIface_)
            notifIface_ = new QDBusInterface(
                "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", QDBusConnection::sessionBus(), this);
        if (!notifIface_->isValid()) return false;
        notifIface_->setTimeout(3000);   // bound the blocking call; don't freeze the UI
        QStringList actions;
        actions << "default" << "Open";
        if (verdict == "SAFE") actions << "update" << "Update now";
        actions << "details" << "Details";
        QVariantMap hints; hints["urgency"] = QVariant::fromValue<uchar>(warning ? 2 : 1);
        QVariantList args;
        args << QStringLiteral("TW Update Assistant") << lastNotifId
             << cfg.icon << title << body
             << actions << hints << int(warning ? 0 : 12000);
        QDBusReply<uint> r = notifIface_->callWithArgumentList(QDBus::Block, "Notify", args);
        if (r.isValid()) { lastNotifId = r.value(); return true; }
        return false;
    }

    Config cfg;
    Panel *panel = nullptr;
    KStatusNotifierItem *sni = nullptr;
    QMenu *menu = nullptr;
    QFileSystemWatcher *watcher = nullptr;
    QTimer *reloadTimer = nullptr;
    QProcess *checkProc = nullptr;
    QDBusInterface *notifIface_ = nullptr;
    QAction *headerAction = nullptr, *checkAction = nullptr, *updateAction = nullptr;
    bool checking = false;
    uint lastNotifId = 0;
    QString verdict = "UNKNOWN", summary, statusHash;
    QJsonObject lastStatus;
    QJsonArray collisions;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("tw-safe-update-tray");
    app.setQuitOnLastWindowClosed(false);

    // Single instance: if one is already running (e.g. the systemd service),
    // a second launch (from the app launcher) asks it to show its window and
    // waits for an ack so the request isn't lost to a startup race.
    const QString sockName = QStringLiteral("twsu-tray-") + qEnvironmentVariable("USER", "u");
    {
        QLocalSocket probe;
        probe.connectToServer(sockName);
        if (probe.waitForConnected(250)) {
            probe.write("show\n");
            probe.flush();
            probe.waitForReadyRead(1000);   // wait for the running instance's ack
            return 0;
        }
    }
    QLocalServer server;
    if (!QLocalServer::removeServer(sockName) || !server.listen(sockName)) {
        // Couldn't own the IPC endpoint. Rather than risk spawning a duplicate
        // tray, bail — the existing instance (if any) keeps running.
        qWarning("twsu-tray: could not create control socket '%s'; exiting to "
                 "avoid a duplicate tray icon.", qPrintable(sockName));
        return 1;
    }

    Tray tray;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&]{
        while (QLocalSocket *c = server.nextPendingConnection()) {
            tray.showWindow();
            c->write("ok\n");                 // ack so the client can exit cleanly
            c->flush();
            QObject::connect(c, &QLocalSocket::disconnected, c, &QLocalSocket::deleteLater);
            QObject::connect(c, &QLocalSocket::errorOccurred, c, &QLocalSocket::deleteLater);
            c->disconnectFromServer();
        }
    });

    return app.exec();
}

#include "twsu-tray.moc"
