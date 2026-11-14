// Drives the real main window with keystrokes (run with QT_QPA_PLATFORM=offscreen).

#include "TestLaunchGuard.h"
#include "core/Archive.h"
#include "core/DirWatcher.h"
#include "core/FolderHistory.h"
#include "ui/DirTab.h"
#include "ui/KeyButton.h"
#include "core/Permissions.h"
#include "ui/ShelfPanel.h"
#include "core/Shelves.h"
#include "ui/DialogKeys.h"
#include "ui/Dialogs.h"
#include "ui/EditNames.h"
#include "ui/FileModel.h"
#include "ui/CommandBar.h"
#include "ui/SyncDialog.h"
#include "ui/Network.h"
#include "ui/PathBar.h"
#include "core/Backend.h"
#include "core/Keyring.h"
#include "core/Remote.h"
#include "TestBackend.h"
#include "SshServer.h"
#include "ui/AuthDialogs.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/ViewerWindow.h"
#include "ui/ChecksumDialog.h"
#include "ui/DiskUsageDialog.h"
#include "ui/DuplicatesDialog.h"
#include "ui/FileView.h"
#include "ui/ThumbGrid.h"
#include "core/DesktopApps.h"
#include "ui/FuzzyPopup.h"
#include "ui/Keymap.h"
#include "ui/KeyChordPopup.h"
#include "ui/IconCache.h"
#include "ui/MainWindow.h"
#include "ui/MultiRenameDialog.h"
#include "ui/PermissionsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/ServersDialog.h"
#include "core/Servers.h"
#include "ui/AboutDialog.h"
#include "ui/ShortcutsDialog.h"
#include "ui/TrashDialog.h"
#include "ui/Panel.h"
#include "ui/PathBar.h"
#include "core/Plugins.h"
#include "ui/FileColors.h"
#include "ui/Theme.h"
#include "core/Workspaces.h"
#include "ui/viewer/ImageView.h"
#include "ui/viewer/ViewerWindow.h"

#include <QPlainTextEdit>

#include <sys/stat.h>
#include <QFontDatabase>
#include <QMenuBar>
#include <QMimeData>
#include <QScrollBar>
#include <QStyle>

#include <QApplication>
#include <QLabel>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabBar>
#include <QToolButton>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QImage>
#include <QImageReader>
#include <QCheckBox>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QListWidget>
#include <QKeySequenceEdit>
#include <QTreeWidget>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDialog>
#include <QLineEdit>
#include <thread>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTimer>
#include <QDialogButtonBox>
#include <QVBoxLayout>

using namespace tandem;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

// Answers the next modal dialog once it is open.
QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

void answerNextDialog(std::function<void(QWidget*)> answer)
{
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [answer, attempt](int tries) {
        QWidget* w = QApplication::activeModalWidget();
        if (!w) {
            if (tries > 0)
                QTimer::singleShot(20, [attempt, tries] { (*attempt)(tries - 1); });
            return;
        }
        answer(w);
    };
    QTimer::singleShot(20, [attempt] { (*attempt)(200); });
}

// Picks an entry from the next popup menu (searching submenus too).
void chooseFromNextMenu(const QString& text)
{
    QTimer::singleShot(50, [text] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        std::function<QAction*(QMenu*)> find = [&](QMenu* m) -> QAction* {
            for (QAction* a : m->actions()) {
                if (a->text() == text)
                    return a;
                if (a->menu())
                    if (QAction* sub = find(a->menu()))
                        return sub;
            }
            return nullptr;
        };
        QAction* a = find(menu);
        QVERIFY2(a, qPrintable(text));
        menu->close();
        a->trigger();
    });
}

void rightClick(QAbstractItemView* view, int row)
{
    const QPoint pos = view->visualRect(view->model()->index(row, 0)).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, view->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(view->viewport(), &ev);
}

QByteArray readName(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void acceptDialog(QWidget* w)
{
    if (auto* box = qobject_cast<QMessageBox*>(w)) {
        box->button(QMessageBox::Yes)->click();
        return;
    }
    if (auto* d = qobject_cast<QDialog*>(w))
        d->accept();
}

}  // namespace

class TestUi : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<MainWindow> m_window;
    Panel* m_left = nullptr;
    Panel* m_right = nullptr;

    QString L(const QString& rel = {}) const { return m_tmp.filePath("left" + (rel.isEmpty() ? "" : "/" + rel)); }
    QString R(const QString& rel = {}) const { return m_tmp.filePath("right" + (rel.isEmpty() ? "" : "/" + rel)); }
    FileModel* leftModel() const { return m_left->tab()->model(); }
    FileModel* rightModel() const { return m_right->tab()->model(); }

    void moveCursorTo(Panel* p, const QString& name)
    {
        const int row = p->tab()->model()->rowOf(name);
        QVERIFY2(row >= 0, qPrintable(name));
        p->view()->setCurrentRow(row);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        setKeyringHooksForTests({});  // never the real keyring
        installTestLaunchGuard();      // never real apps
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_tmp.filePath("config")));
        applyTheme(*qApp);
        DirWatcher::instance();
        registerBuiltinBackends();
        installAuthDialogs();
        FolderHistory::instance().setStoragePath(m_tmp.filePath("folders.tsv"));
        QSettings().clear();  // test mode keeps settings in ~/.qttest across runs: start clean
        QFile::remove(Keymap::instance().path());  // and key bindings

        QVERIFY(QDir().mkpath(L("sub/deep")));
        QVERIFY(QDir().mkpath(R()));
        writeFile(L("file1.txt"), "one");
        writeFile(L("file2.txt"), "two two");
        writeFile(L("file10.txt"), "ten ten ten");
        writeFile(L("photo.jpg"), QByteArray(5000, 'p'));
        writeFile(L("sub/inner.txt"), "inner");

        m_window = std::make_unique<MainWindow>(QStringList{L(), R()});
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_left = m_window->findChild<Panel*>("leftPanel");
        m_right = m_window->findChild<Panel*>("rightPanel");
        QVERIFY(m_left && m_right);
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_COMPARE(rightModel()->path(), R());
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
    }

    // The offscreen platform does not reactivate the window after a dialog
    // closes (a real compositor does), so do it before every step.
    void init()
    {
        if (!m_window)
            return;
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        if (!m_right->isActive())
            m_left->view()->setFocus();
    }

    void listingIsSortedNaturally()
    {
        QStringList names;
        for (int r = 0; r < leftModel()->count(); ++r)
            names << leftModel()->entry(r)->name;
        QCOMPARE(names, QStringList({"..", "sub", "file1.txt", "file2.txt", "file10.txt", "photo.jpg"}));
    }

    void enterAndLeaveFolder()
    {
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(m_left->currentName(), QString("sub"));  // cursor returns to where we came from
        QTest::keyClick(m_left->view(), Qt::Key_Left, Qt::AltModifier);  // history back
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void arrowKeysWalkFolders()
    {
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QTest::keyClick(m_left->view(), Qt::Key_Left);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(m_left->currentName(), QString("sub"));

        // → on a file does nothing: no app is started.
        QSignalSpy launched(m_left, &Panel::openFileRequested);
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QTest::qWait(50);
        QCOMPARE(launched.size(), 0);
        QCOMPARE(leftModel()->path(), L());

        // Switched off, the arrows are inert again.
        m_left->setArrowFolders(false);
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QTest::qWait(50);
        QCOMPARE(leftModel()->path(), L());
        m_left->setArrowFolders(true);
    }

    // Text on the accent (primary buttons, selections) stays readable in
    // Tandem's own themes: at least 4.5:1, as WCAG asks for normal text.
    void ownThemesContrast()
    {
        auto luminance = [](const QColor& c) {
            auto ch = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
        };
        auto restore = qScopeGuard([] {
            QSettings().remove("view/theme");
            applyTheme(*qApp);
        });
        for (const char* id : {"tandem-dark", "tandem-light"}) {
            QSettings().setValue("view/theme", id);
            applyTheme(*qApp);
            QCOMPARE(theme().id, QString(id));
            const double a = luminance(theme().accent), b = luminance(theme().accentText);
            const double ratio = (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
            QVERIFY2(ratio >= 4.5, qPrintable(QString("%1: %2:1").arg(id).arg(ratio, 0, 'f', 2)));
        }
    }

    void themes()
    {
        // Every shipped theme loads, and the list knows light from dark.
        const QList<ThemeInfo> all = availableThemes();
        QVERIFY(all.size() >= 16);
        for (const ThemeInfo& i : all) {
            if (i.id.startsWith("tandem-"))
                continue;
            Theme t;
            QString error;
            QVERIFY2(loadThemeFile(":/themes/" + i.id + ".json", t, &error), qPrintable(i.id + ": " + error));
            QCOMPARE(t.dark, t.base.lightness() < 128);
        }

        // A few colours are enough; the rest follow them, unknown keys are ignored.
        QTemporaryDir tmp;
        writeFile(tmp.filePath("mini.json"),
                  "{\"colors\": {\"base\": \"#ffffff\", \"text\": \"#000000\", \"accent\": \"#ff0000\","
                  " \"glow\": \"#123456\"}, \"future\": true}");
        Theme mini;
        QVERIFY(loadThemeFile(tmp.filePath("mini.json"), mini));
        QCOMPARE(mini.id, QString("mini"));
        QVERIFY(!mini.dark);  // told by the background
        QCOMPARE(mini.accent, QColor("#ff0000"));
        QCOMPARE(mini.cursorActive, QColor(255, 0, 0, 50));
        QVERIFY(mini.dimText != mini.text);
        writeFile(tmp.filePath("broken.json"), "{ nope");
        QVERIFY(!loadThemeFile(tmp.filePath("broken.json"), mini));

        // Switching restyles what is already shown.
        QDir().mkpath(QFileInfo(themesFolder()).path());
        auto restore = qScopeGuard([] {
            QSettings().remove("view/theme");
            applyTheme(*qApp);
        });
        QLabel label;
        setThemeStyle(&label, "color: @accent;");
        QSignalSpy changed(&ThemeNotifier::instance(), &ThemeNotifier::changed);
        QSettings().setValue("view/theme", "nord");
        applyTheme(*qApp);
        QCOMPARE(theme().id, QString("nord"));
        QCOMPARE(changed.size(), 1);
        QVERIFY2(label.styleSheet().contains("rgba(136,192,208"), qPrintable(label.styleSheet()));

        // An own file with a shipped theme's name replaces it, and saving it applies it.
        QDir().mkpath(themesFolder());
        QTest::qWait(300);  // the new folder is noticed and watched
        const QString own = themesFolder() + "/nord.json";
        auto removeOwn = qScopeGuard([own] { QFile::remove(own); });
        writeFile(own, "{\"name\": \"My Nord\", \"dark\": true, \"colors\": {\"accent\": \"#00ff00\"}}");
        QTRY_COMPARE(theme().name, QString("My Nord"));  // picked up by watching the folder
        QVERIFY(label.styleSheet().contains("rgba(0,255,0"));
    }

    // A plugin: switched on in Settings, it becomes a command; its answer marks a file.
    void pluginCommand()
    {
        const QString dir = pluginsFolder() + "/marker";
        QVERIFY(QDir().mkpath(dir));
        auto cleanup = qScopeGuard([dir] { QDir(dir).removeRecursively(); });
        writeFile(dir + "/plugin.json",
                  "{\"name\": \"Marker\", \"actions\": [{\"title\": \"Mark the second\", "
                  "\"run\": [\"sh\", \"-c\", \"echo '{\\\"select\\\": [\\\"file2.txt\\\"], "
                  "\\\"message\\\": \\\"marked\\\"}'\"]}]}");

        SettingsDialog settings(m_window.get());
        settings.showPage(5);
        auto* reload = [&] {
            for (auto* b : settings.findChildren<QPushButton*>())
                if (b->text() == tr("Reload"))
                    return b;
            return static_cast<QPushButton*>(nullptr);
        }();
        QVERIFY(reload);
        reload->click();
        QCheckBox* box = nullptr;
        QTRY_VERIFY((box = [&] {
            for (auto* c : settings.findChildren<QCheckBox*>())
                if (c->text() == "Marker" && !c->isHidden())
                    return c;
            return static_cast<QCheckBox*>(nullptr);
        }()));
        QVERIFY(!box->isChecked());  // off until switched on
        box->setChecked(true);
        settings.close();

        QAction* command = nullptr;
        for (QAction* a : m_window->actions())
            if (a->property("commandId").toString() == "plugin-marker-mark-the-second")
                command = a;
        QVERIFY(command);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        moveCursorTo(m_left, "file1.txt");  // something to run on
        command->trigger();
        QTRY_VERIFY(leftModel()->markedNames().contains("file2.txt"));
        leftModel()->markAll(false);

        // Changing the plugin switches it off again.
        writeFile(dir + "/plugin.json", "{\"actions\": [{\"title\": \"Other\", \"run\": \"true\"}]}");
        bool reloaded = false;
        SettingsDialog again(m_window.get());
        for (auto* b : again.findChildren<QPushButton*>())
            if (b->text() == tr("Reload"))
                b->click();
        QTRY_VERIFY((reloaded = std::none_of(m_window->actions().cbegin(), m_window->actions().cend(), [](QAction* a) {
                         return a->property("commandId").toString().startsWith("plugin-marker");
                     })));
    }

    // Workspaces: Alt+W saves the open tabs under a new name; the workspace's
    // own command brings them back in place of the tabs open then.
    void workspaces()
    {
        auto restore = qScopeGuard([this] {
            for (const Workspace& w : Workspaces::all())
                Workspaces::remove(w.id);
            Workspaces::setStartup(StartupMode::Session);
            m_left->replaceTabs({L()}, 0);
            m_right->replaceTabs({R()}, 0);
            QTRY_COMPARE(leftModel()->path(), L());
            QTRY_COMPARE(rightModel()->path(), R());
            m_left->view()->setFocus();
        });
        m_left->replaceTabs({L(), L("sub")}, 1, {1, 4});
        m_right->replaceTabs({R()}, 0);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());

        auto visiblePopup = [this] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    return p;
            return static_cast<FuzzyPopup*>(nullptr);
        };
        QTest::keyClick(m_right->view(), Qt::Key_W, Qt::AltModifier);
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY((popup = visiblePopup()));
        QTest::keyClicks(QApplication::focusWidget(), "Project");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->title.contains("Project"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(Workspaces::byName("Project"));
        const Workspace saved = *Workspaces::byName("Project");
        QCOMPARE(saved.left.paths, QStringList({L(), L("sub")}));
        QCOMPARE(saved.left.numbers, QList<int>({1, 4}));
        QCOMPARE(saved.left.current, 1);
        QCOMPARE(saved.right.paths, QStringList({R()}));
        QVERIFY(saved.rightActive);

        // Its own command, for the palette and a key.
        auto command = [this](int id) {
            for (QAction* a : m_window->actions())
                if (a->property("commandId").toString() == QString("workspace-%1").arg(id))
                    return a;
            return static_cast<QAction*>(nullptr);
        };
        QAction* open = command(saved.id);
        QVERIFY(open);
        QCOMPARE(open->text(), QString("Project"));

        m_left->replaceTabs({R()}, 0);
        m_right->replaceTabs({L(), R(), L("sub")}, 2);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        open->trigger();
        QCOMPARE(m_left->tabPaths(), QStringList({L(), L("sub")}));
        QCOMPARE(m_left->tabSlots(), QList<int>({1, 4}));
        QCOMPARE(m_left->currentTabIndex(), 1);
        QCOMPARE(m_right->tabPaths(), QStringList({R()}));  // replaced, not added to
        QTRY_VERIFY(m_right->isActive());
        QTRY_COMPARE(leftModel()->path(), L("sub"));

        // Settings › Startup: renamed in place, the command follows; deleted, it goes.
        {
            SettingsDialog settings(m_window.get());
            settings.showPage(2);
            QTreeWidget* list = nullptr;
            for (auto* t : settings.findChildren<QTreeWidget*>())
                if (t->topLevelItemCount() == 1 && t->topLevelItem(0)->text(0) == "Project")
                    list = t;
            QVERIFY(list);
            list->topLevelItem(0)->setText(0, "Renamed");
            QTRY_COMPARE(Workspaces::byId(saved.id)->name, QString("Renamed"));
            QTRY_VERIFY(command(saved.id) && command(saved.id)->text() == "Renamed");

            QComboBox* startup = nullptr;
            for (auto* c : settings.findChildren<QComboBox*>())
                if (c->findData(QString("workspace:%1").arg(saved.id)) >= 0)
                    startup = c;
            QVERIFY(startup);
            QCOMPARE(startup->currentData().toString(), QString("session"));  // the default
            startup->setCurrentIndex(startup->findData(QString("workspace:%1").arg(saved.id)));
            emit startup->activated(startup->currentIndex());
            QCOMPARE(Workspaces::startupMode(), StartupMode::Workspace);
        }

        // At start: the workspace, unless folders are given; or home in both panels.
        auto startedWith = [](const QStringList& args) {
            const auto titles = Keymap::actionTitle;  // a window sets it to its own
            auto restore = qScopeGuard([titles] { Keymap::actionTitle = titles; });
            MainWindow other(args);
            Panel* l = other.findChild<Panel*>("leftPanel");
            Panel* r = other.findChild<Panel*>("rightPanel");
            return QStringList{l->tabPaths().join(","), r->tabPaths().join(","), r->isActive() ? "right" : "left"};
        };
        QCOMPARE(startedWith({}), QStringList({L() + "," + L("sub"), R(), "right"}));
        QCOMPARE(startedWith({L("sub")}), QStringList({L("sub"), R(), "right"}));
        Workspaces::setStartup(StartupMode::Home);
        QCOMPARE(startedWith({}), QStringList({QDir::homePath(), QDir::homePath(), "left"}));

        {
            SettingsDialog settings(m_window.get());
            QPushButton* remove = nullptr;
            QTreeWidget* list = nullptr;
            for (auto* b : settings.findChildren<QPushButton*>())
                if (b->text() == "Delete")
                    remove = b;
            for (auto* t : settings.findChildren<QTreeWidget*>())
                if (t->topLevelItemCount() == 1 && t->topLevelItem(0)->text(0) == "Renamed")
                    list = t;
            QVERIFY(remove && list);
            list->setCurrentItem(list->topLevelItem(0));
            remove->click();
            QVERIFY(Workspaces::all().isEmpty());
            QVERIFY(!command(saved.id));
        }
    }

    // Several archives at once: each unpacked next to itself, several
    // top-level items in a folder of their own.
    void unpackSeveralHere()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("one/a")));
        writeFile(tmp.filePath("one/a/x.txt"), "x");
        writeFile(tmp.filePath("y.txt"), "y");
        writeFile(tmp.filePath("z.txt"), "z");
        for (auto [sources, archive] : {std::pair{QStringList{tmp.filePath("one/a")}, QString("first.zip")},
                                        std::pair{QStringList{tmp.filePath("y.txt"), tmp.filePath("z.txt")}, QString("second.tar.gz")}}) {
            PackJob job(sources, L(archive));
            QSignalSpy done(&job, &Job::finished);
            job.start();
            QVERIFY(done.wait(5000));
        }
        auto cleanup = qScopeGuard([this] {
            for (const char* n : {"first.zip", "second.tar.gz", "a", "second"})
                QFileInfo(L(n)).isDir() ? (void)QDir(L(n)).removeRecursively() : (void)QFile::remove(L(n));
        });
        m_left->navigate(L());
        QTRY_VERIFY(leftModel()->rowOf("second.tar.gz") >= 0);
        leftModel()->setMarkedNames({"first.zip", "second.tar.gz"});
        m_left->view()->setFocus();
        QAction* here = nullptr;
        for (QAction* a : m_window->actions())
            if (a->property("commandId").toString() == "unpack-here")
                here = a;
        QVERIFY(here);
        here->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(L("a/x.txt")), 10000);             // one item: as it is
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(L("second/z.txt")), 10000);        // several: own folder
        QVERIFY(QFile::exists(L("second/y.txt")));
    }

    void fileColorsAndFading()
    {
        auto restore = qScopeGuard([] {
            setFileColorsEnabled(false);
            FileView::setInactiveDim(0);
            QSettings().remove("view/theme");
            applyTheme(*qApp);
        });
        QSettings().setValue("view/theme", "nord");
        applyTheme(*qApp);
        auto entry = [](const QString& name, uint32_t mode = S_IFREG | 0644, bool dir = false) {
            FileEntry e;
            e.name = name;
            e.mode = mode;
            e.isDir = dir;
            return e;
        };
        QVERIFY(!fileColor(entry("a.jpg")));  // off unless switched on
        setFileColorsEnabled(true);
        QCOMPARE(*fileColor(entry("a.JPG")), QColor("#b48ead"));
        QCOMPARE(*fileColor(entry("b.tar.gz")), QColor("#bf616a"));
        QCOMPARE(*fileColor(entry("run", S_IFREG | 0755)), QColor("#a3be8c"));
        QCOMPARE(*fileColor(entry("song.flac", S_IFREG | 0755)), QColor("#8fbcbb"));  // the name wins over the x bit
        QVERIFY(!fileColor(entry("notes.txt")));
        QVERIFY(!fileColor(entry("Pictures", S_IFDIR | 0755, true)));

        // A theme's own rules come first; kinds it leaves out are simply not coloured.
        QTemporaryDir tmp;
        QDir().mkpath(themesFolder());
        const QString own = themesFolder() + "/rules.json";
        auto removeOwn = qScopeGuard([own] { QFile::remove(own); });
        writeFile(own, "{\"colors\": {\"base\": \"#000000\"}, \"files\": {\"*.txt\": \"#123456\", "
                       "\"image\": \"#654321\"}}");
        QSettings().setValue("view/theme", "rules");
        applyTheme(*qApp);
        QCOMPARE(*fileColor(entry("notes.txt")), QColor("#123456"));
        QCOMPARE(*fileColor(entry("a.png")), QColor("#654321"));
        QVERIFY(!fileColor(entry("b.zip")));

        // Only the panel that isn't active fades.
        FileView::setInactiveDim(0.3);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        QCOMPARE(m_left->view()->fade(), 0.0);
        QCOMPARE(m_right->view()->fade(), 0.3);
    }

    // Ctrl+= / Ctrl+wheel / Ctrl+0: bigger rows in both lists, remembered.
    void listZoom()
    {
        auto restore = qScopeGuard([this] { m_window->setZoom(false, 100); });
        m_left->view()->setFocus();
        const int row = m_left->view()->sizeHintForRow(0);
        QTest::keyClick(m_left->view(), Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(FileView::listZoom(), 100 + FileView::kZoomStep);
        QTRY_VERIFY(m_left->view()->sizeHintForRow(0) > row);
        QTRY_VERIFY(m_right->view()->sizeHintForRow(0) > row);
        QCOMPARE(QSettings().value("view/listZoom").toInt(), 110);
        QTest::keyClick(m_left->view(), Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(FileView::listZoom(), 100);
        QTRY_COMPARE(m_left->view()->sizeHintForRow(0), row);
    }

    // Fonts from the settings: the interface, the lists (on top of it), fixed width.
    void fonts()
    {
        auto restore = qScopeGuard([] {
            QSettings s;
            for (const char* k : {"view/fontFamily", "view/fontSize", "view/listFontFamily", "view/listFontSize",
                                  "view/monoFontFamily", "view/monoFontSize"})
                s.remove(k);
            applyTheme(*qApp);
        });
        const QString family = QFontDatabase::families().value(0);
        QSettings s;
        s.setValue("view/fontSize", 13);
        s.setValue("view/listFontFamily", family);
        s.setValue("view/monoFontSize", 15);
        applyTheme(*qApp);
        QCOMPARE(QApplication::font().pointSizeF(), 13.0);
        QCOMPARE(theme().listFont.family(), family);
        QCOMPARE(theme().listFont.pointSizeF(), 13.0);  // the size comes from the interface font
        QCOMPARE(monoFont().pointSizeF(), 15.0);
        QVERIFY(QFontInfo(monoFont()).fixedPitch());
    }

    // Interface size: fonts and icons, applied at once, inside Tandem.
    void interfaceSize()
    {
        auto restore = qScopeGuard([] {
            QSettings().remove("view/interfaceScale");
            applyTheme(*qApp);
        });
        const double font = QApplication::font().pointSizeF();
        const int icon = QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize);
        QSettings().setValue("view/interfaceScale", 150);
        applyTheme(*qApp);
        QCOMPARE(interfaceScale(), 1.5);
        QCOMPARE(QApplication::font().pointSizeF(), font * 1.5);
        QCOMPARE(QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize), qRound(icon * 1.5));
        QSettings().setValue("view/interfaceScale", 100);
        applyTheme(*qApp);
        QCOMPARE(QApplication::font().pointSizeF(), font);
        QVERIFY(qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR"));
    }

    // The Bookmarks menu lists bookmarks with their keys, and pins; a click goes there.
    void bookmarksMenu()
    {
        FolderHistory& h = FolderHistory::instance();
        QTRY_VERIFY(h.isLoaded());
        h.setBookmark(L("sub"), "zs");
        h.setPinned(R(), true);
        auto restore = qScopeGuard([&h, this] {
            h.setPinned(L("sub"), false);
            h.setPinned(R(), false);
        });
#ifdef Q_OS_MACOS
        // Text after a tab would become a real shortcut in the macOS menu bar.
        const QString subText = "sub   (zs)";
#else
        const QString subText = "sub\tzs";
#endif
        QMenu* menu = nullptr;
        for (QAction* a : m_window->menuBar()->actions())
            if (a->menu() && a->text().remove('&') == "Bookmarks")
                menu = a->menu();
        QVERIFY(menu);
        emit menu->aboutToShow();
        QAction* sub = nullptr;
        bool pinned = false;
        for (QAction* a : menu->actions()) {
            if (a->text() == subText)
                sub = a;
            pinned |= a->text() == baseName(R());
        }
        QVERIFY(sub);
        QVERIFY(pinned);
#ifdef Q_OS_MACOS
        for (QAction* a : menu->actions())
            QVERIFY2(!a->text().contains('\t'), qPrintable(a->text()));
#endif
        emit menu->aboutToShow();  // filled anew, not twice
        int count = 0;
        for (QAction* a : menu->actions())
            count += a->text() == subText;
        QCOMPARE(count, 1);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        for (QAction* a : menu->actions())
            if (a->text() == subText)
                a->trigger();
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Rows: dates people read at a glance, folders with item counts, no Ext/Attributes columns.
    void rowsAtAGlance()
    {
        const QDateTime now(QDate(2026, 11, 4), QTime(15, 0));
        auto at = [&](int days, int h, int m) { return QDateTime(now.date().addDays(days), QTime(h, m)).toSecsSinceEpoch(); };
        QCOMPARE(FileModel::relativeDate(now.toSecsSinceEpoch() - 20, now), QString("just now"));
        QCOMPARE(FileModel::relativeDate(now.toSecsSinceEpoch() - 12 * 60, now), QString("12 min ago"));
        QVERIFY(FileModel::relativeDate(at(0, 9, 5), now).startsWith("Today "));
        QVERIFY(FileModel::relativeDate(at(-1, 9, 5), now).startsWith("Yesterday "));
        QVERIFY(FileModel::relativeDate(at(-3, 9, 5), now).contains(QLocale().toString(QTime(9, 5), QLocale::ShortFormat)));
        QCOMPARE(FileModel::relativeDate(at(-30, 9, 5), now), QLocale().toString(now.date().addDays(-30), "d MMM"));
        QCOMPARE(FileModel::relativeDate(QDateTime(QDate(2024, 3, 2), QTime(1, 0)).toSecsSinceEpoch(), now),
                 QLocale().toString(QDate(2024, 3, 2), "d MMM yyyy"));

        // A local folder: its subfolders say how many items they hold.
        QVERIFY(QDir().mkpath(L("counted/a")));
        writeFile(L("counted/x.txt"), "x");
        writeFile(L("counted/.hidden"), "h");
        auto cleanup = qScopeGuard([this] { QDir(L("counted")).removeRecursively(); });
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_VERIFY(leftModel()->rowOf("counted") >= 0);  // the watcher reports the new folder
        const int row = leftModel()->rowOf("counted");
        leftModel()->data(leftModel()->index(row, FileModel::ColSize), Qt::DisplayRole);  // asks for the count
        QTRY_COMPARE(leftModel()->data(leftModel()->index(row, FileModel::ColSize), Qt::DisplayRole).toString(),
                     QString("2 items"));  // hidden files aren't counted while they're hidden

        // The extension lives in the name; the columns can come back from the header menu.
        QVERIFY(m_left->view()->isColumnHidden(FileModel::ColExt));
        QVERIFY(m_left->view()->isColumnHidden(FileModel::ColAttr));
    }

    // Owner, group, octal mode, created, accessed and link target: hidden
    // until switched on, filled from the listing.
    void extraColumns()
    {
        QVERIFY(QDir().mkpath(L("extra")));
        writeFile(L("extra/file.txt"), "x");
        QVERIFY(::chmod(QFile::encodeName(L("extra/file.txt")).constData(), 0640) == 0);
        QVERIFY(::symlink("file.txt", QFile::encodeName(L("extra/link")).constData()) == 0);
        auto cleanup = qScopeGuard([this] {
            QDir(L("extra")).removeRecursively();
            m_left->setColumnLayout({});
        });
        m_left->navigate(L("extra"));
        QTRY_COMPARE(leftModel()->path(), L("extra"));
        for (int c = FileModel::ColOwner; c < FileModel::ColCount; ++c)
            QVERIFY(m_left->view()->isColumnHidden(c));

        FileModel* m = leftModel();
        const int file = m->rowOf("file.txt");
        const int link = m->rowOf("link");
        auto text = [m](int row, int col) { return m->data(m->index(row, col), Qt::DisplayRole).toString(); };
        QCOMPARE(text(file, FileModel::ColOctal), QString("640"));
        QCOMPARE(text(link, FileModel::ColLinkTarget), QString("file.txt"));
        QCOMPARE(text(file, FileModel::ColLinkTarget), QString());
        QVERIFY(!text(file, FileModel::ColAccessed).isEmpty());
        // Names are looked up on a worker; until then the cell is empty.
        text(file, FileModel::ColOwner);
        QTRY_COMPARE(text(file, FileModel::ColOwner), userName(::getuid()));
        QTRY_COMPARE(text(file, FileModel::ColGroup), groupName(::getgid()));

        // The layout is kept by name: order, widths, what's hidden.
        m_left->setColumnLayout({"name", "owner 77", "size 120", "-modified", "nonsense 5"});
        QHeaderView* h = m_left->view()->header();
        QCOMPARE(h->logicalIndex(1), int(FileModel::ColOwner));
        QCOMPARE(h->logicalIndex(2), int(FileModel::ColSize));
        QVERIFY(!h->isSectionHidden(FileModel::ColOwner));
        QVERIFY(h->isSectionHidden(FileModel::ColTime));
        QCOMPARE(h->sectionSize(FileModel::ColSize), 120);
        const QStringList layout = m_left->columnLayout();
        QCOMPARE(layout.mid(0, 5), QStringList({"name", "owner 77", "size 120", "-modified", "-ext"}));
        QVERIFY(layout.contains("-link"));  // not named: where it was, hidden
        QCOMPARE(layout.size(), int(FileModel::ColCount));

        // Header state from older versions turns into the same kind of layout.
        QStandardItemModel five(0, 5);
        QHeaderView old(Qt::Horizontal);
        old.setModel(&five);
        old.resizeSection(FileModel::ColSize, 111);
        old.setSectionHidden(FileModel::ColExt, true);
        old.moveSection(old.visualIndex(FileModel::ColTime), 1);
        QCOMPARE(Panel::columnLayoutFromHeaderState(old.saveState()).mid(1),
                 QStringList({"modified " + QString::number(old.sectionSize(FileModel::ColTime)), "-ext", "size 111",
                              "attributes " + QString::number(old.sectionSize(FileModel::ColAttr))}));
        QVERIFY(Panel::columnLayoutFromHeaderState("garbage").isEmpty());

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // The F-key bar says how many go, and where to.
    void fkeysNameTheTarget()
    {
        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_COMPARE(rightModel()->path(), R());
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        auto* f5 = m_window->findChild<QAbstractButton*>("fkey-F5");
        QVERIFY(f5);
        QTRY_COMPARE(f5->accessibleName(), "Copy → " + baseName(R()));
        leftModel()->setMarkedNames({"file1.txt", "file2.txt"});
        QTRY_COMPARE(f5->accessibleName(), "Copy 2 → " + baseName(R()));
        QTRY_COMPARE(m_window->findChild<QAbstractButton*>("fkey-F8")->accessibleName(), QString("Trash 2"));
        leftModel()->markAll(false);
        m_right->view()->setFocus();  // the other way round
        QTRY_COMPARE(f5->accessibleName(), "Copy → " + baseName(L()));
        m_left->view()->setFocus();
    }

    // Marked items get a bar with what they are and what fits them.
    void selectionBar()
    {
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        QFrame* bar = m_left->selectionBar();
        QVERIFY(!bar->isVisible());
        leftModel()->setMarkedNames({"file1.txt", "sub"});
        QTRY_VERIFY(bar->isVisible());
        QStringList texts;
        for (auto* l : bar->findChildren<QLabel*>())
            texts << l->text();
        QVERIFY2(texts.join(' ').contains("1 folder, 1 file"), qPrintable(texts.join(' ')));
        QAbstractButton* unmark = nullptr;
        for (auto* b : bar->findChildren<QAbstractButton*>())
            if (b->accessibleName() == "Unmark")
                unmark = b;
        QVERIFY(unmark);
        unmark->click();
        QTRY_VERIFY(!bar->isVisible());
        QVERIFY(leftModel()->markedNames().isEmpty());
        QVERIFY(!m_right->selectionBar()->isVisible());  // only the active panel has one
    }

    // Copy to any open tab, also one that is not shown: picked from a list.
    void copyToAnOpenTab()
    {
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QTemporaryDir hidden;
        DirTab* extra = m_right->addTab(hidden.path(), false);  // a tab in the background
        auto closeIt = qScopeGuard([this] {
            m_right->cycleTab(1);
            if (m_right->tab()->model()->path() != R())
                m_right->closeCurrentTab();
            QTRY_COMPARE(m_right->tab()->model()->path(), R());
        });
        QVERIFY(extra);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        QTRY_VERIFY(!m_left->tab()->isLoading());
        moveCursorTo(m_left, "file2.txt");
        QCOMPARE(m_left->currentPath(), L("file2.txt"));
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::AltModifier | Qt::ShiftModifier);
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY([&] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    popup = p;
            return popup != nullptr;
        }());
        QTest::keyClicks(QApplication::focusWidget(), QFileInfo(hidden.path()).fileName());
        QCOMPARE(m_left->currentPath(), L("file2.txt"));
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == hidden.path());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ShiftModifier);  // right away
        QTRY_VERIFY(QFile::exists(hidden.filePath("file2.txt")));
        QVERIFY(QFile::exists(L("file2.txt")));  // copied, not moved
    }

    // The selection bar's ▾ next to Move offers the same targets, and moves.
    void moveToFromSelectionBar()
    {
        writeFile(L("to-move.txt"), "m");
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QTemporaryDir hidden;
        DirTab* extra = m_right->addTab(hidden.path(), false);
        auto closeIt = qScopeGuard([this] {
            m_right->cycleTab(1);
            if (m_right->tab()->model()->path() != R())
                m_right->closeCurrentTab();
            QTRY_COMPARE(m_right->tab()->model()->path(), R());
            QFile::remove(L("to-move.txt"));
        });
        QVERIFY(extra);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        QTRY_VERIFY(!m_left->tab()->isLoading());
        QTRY_VERIFY(leftModel()->rowOf("to-move.txt") >= 0);
        leftModel()->setMarkedNames({"to-move.txt"});
        QFrame* bar = m_left->selectionBar();
        QTRY_VERIFY(bar->isVisible());
        QAbstractButton* more = nullptr;
        QTRY_VERIFY((more = bar->findChild<QAbstractButton*>("selection-move-elsewhere")) != nullptr);
        QCOMPARE(more->accessibleName(), QString("Move elsewhere"));
        more->click();
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY([&] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    popup = p;
            return popup != nullptr;
        }());
        QTest::keyClicks(QApplication::focusWidget(), QFileInfo(hidden.path()).fileName());
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == hidden.path());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return, Qt::ShiftModifier);  // right away
        QTRY_VERIFY(QFile::exists(hidden.filePath("to-move.txt")));
        QTRY_VERIFY(!QFile::exists(L("to-move.txt")));  // moved, not copied
    }

    // A transfer shows in the panel it goes to; a drag says what dropping would do.
    void transfersWhereTheyHappen()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("big.bin"), QByteArray(4 << 20, 'b'));
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("big.bin")}, R());
        job.setPaused(true);
        job.start();
        QTRY_VERIFY(job.snapshot().paused);
        m_right->showTransfer(&job, "Copying <b>big.bin</b> here from /tmp");
        auto* strip = m_right->findChild<QFrame*>("transfer");
        QVERIFY(strip && strip->isVisible());
        QVERIFY(strip->findChild<QLabel*>()->text().contains("paused"));
        m_right->showTransfer(nullptr, {});
        QVERIFY(!strip->isVisible());
        job.cancel();
        QTRY_VERIFY(job.isFinished());

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(tmp.filePath("big.bin"))});
        FileView* v = m_left->view();
        const QPoint below(10, v->viewport()->height() - 5);  // past the last row: into the folder itself
        QDragEnterEvent enter(below, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(v->viewport(), &enter);
        QDragMoveEvent move(below, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(v->viewport(), &move);
        QCOMPARE(v->dropText(), "Copy big.bin into " + baseName(L()));
        QDragMoveEvent shifted(below, Qt::CopyAction | Qt::MoveAction, &data, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(v->viewport(), &shifted);
        QCOMPARE(v->dropText(), "Move big.bin into " + baseName(L()));
        QDragLeaveEvent leave;
        QApplication::sendEvent(v->viewport(), &leave);
        QVERIFY(v->dropText().isEmpty());
    }

    // The preview as an inspector: facts and actions under the content, a two-way switch.
    void previewInspector()
    {
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        moveCursorTo(m_left, "file1.txt");
        QSettings().setValue("preview/info", false);
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        auto restore = qScopeGuard([this] {
            QSettings().remove("preview/info");
            m_left->view()->setFocus();
            QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        });
        ViewerWindow* v = nullptr;  // with a preview below each panel, the left one's
        QTRY_VERIFY([&] {
            for (ViewerWindow* w : m_window->findChildren<ViewerWindow*>())
                if (w->currentPath() == L("file1.txt"))
                    v = w;
            return v != nullptr;
        }());
        QStringList facts;
        QTRY_VERIFY([&] {
            facts.clear();
            for (auto* l : v->findChildren<QLabel*>())
                if (l->isVisibleTo(v))
                    facts << l->text();
            return facts.contains("Modified") && facts.contains("Location");
        }());
        QStringList actions;
        for (auto* b : v->findChildren<QAbstractButton*>())
            if (b->isVisibleTo(v))
                actions << b->accessibleName();
        QVERIFY2(actions.contains("Open") && actions.contains("Edit"), qPrintable(actions.join(", ")));
        QToolButton* details = nullptr;
        for (auto* b : v->findChildren<QToolButton*>())
            if (b->text() == "Details")
                details = b;
        QVERIFY(details);
        details->click();
        QTRY_COMPARE(v->mode(), ViewerWindow::Mode::Info);
        details->click();  // checked again: stays on details
        for (auto* b : v->findChildren<QToolButton*>())
            if (b->text() == "Preview")
                b->click();
        QTRY_VERIFY(v->mode() != ViewerWindow::Mode::Info);
    }

    void quickSearch()
    {
        m_left->view()->setCurrentRow(0);
        QTest::keyClicks(m_left->view(), "pho");
        QTRY_COMPARE(m_left->currentName(), QString("photo.jpg"));
        QWidget* search = QApplication::focusWidget();
        QVERIFY(qobject_cast<QLineEdit*>(search));
        QTest::keyClick(search, Qt::Key_Escape);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));
    }

    void markWithInsertAndPattern()
    {
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt", "file2.txt"}));
        QCOMPARE(leftModel()->stats().markedBytes, int64_t(3 + 7));
        leftModel()->markAll(false);
        QCOMPARE(leftModel()->markByPattern("*.txt", true), 3);
        leftModel()->markAll(false);
    }

    // Shift with End/Home marks through the last/first file; with a page key
    // it marks the files passed over, like Shift+Down held. From a marked
    // file it unmarks instead.
    void markWithShiftPageKeys()
    {
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_End, Qt::ShiftModifier);
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt", "file2.txt", "file10.txt", "photo.jpg"}));
        QCOMPARE(m_left->currentName(), QString("photo.jpg"));
        QTest::keyClick(m_left->view(), Qt::Key_Home, Qt::ShiftModifier);
        QCOMPARE(leftModel()->markedNames(), QStringList());
        QCOMPARE(m_left->view()->currentRow(), 0);

        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_PageDown, Qt::ShiftModifier);  // the page reaches the end
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt", "file2.txt", "file10.txt"}));
        QCOMPARE(m_left->currentName(), QString("photo.jpg"));
        QTest::keyClick(m_left->view(), Qt::Key_PageUp, Qt::ShiftModifier);
        QCOMPARE(leftModel()->markedNames().size(), 5);  // everything but ..
        leftModel()->markAll(false);
    }

    void copyWithF5()
    {
        moveCursorTo(m_left, "file1.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_VERIFY(QFile::exists(R("sub/inner.txt")));
        QTRY_VERIFY(rightModel()->rowOf("file1.txt") >= 0);  // watcher updated the other panel
        QTRY_VERIFY(rightModel()->rowOf("sub") >= 0);
        QCOMPARE(leftModel()->markedNames(), QStringList());  // marks cleared after the op
    }

    void moveWithF6()
    {
        writeFile(L("movable.txt"), "m");
        QTRY_VERIFY(leftModel()->rowOf("movable.txt") >= 0);
        moveCursorTo(m_left, "movable.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F6);
        QTRY_VERIFY(QFile::exists(R("movable.txt")));
        QTRY_COMPARE(leftModel()->rowOf("movable.txt"), -1);
        QTRY_VERIFY(rightModel()->rowOf("movable.txt") >= 0);
    }

    void conflictPromptSkip()
    {
        moveCursorTo(m_left, "file1.txt");
        answerNextDialog([](QWidget* w) {
            acceptDialog(w);  // the destination prompt
            answerNextDialog([](QWidget* prompt) {
                for (auto* b : prompt->findChildren<QPushButton*>())
                    if (b->text() == QObject::tr("&Skip"))
                        b->click();
            });
        });
        writeFile(R("file1.txt"), "keep me");
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTest::qWait(500);
        QCOMPARE(QFile(R("file1.txt")).size(), qint64(7));
    }

    void newFolderWithF7()
    {
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("made/nested");
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(L("made/nested")).isDir());
        QTRY_COMPARE(m_left->currentName(), QString("made"));  // cursor jumps to the new folder
    }

    void renameInPlace()
    {
        moveCursorTo(m_left, "file2.txt");
        leftModel()->setData(leftModel()->index(m_left->view()->currentRow(), 0), "renamed.txt", Qt::EditRole);
        QTRY_VERIFY(QFile::exists(L("renamed.txt")));
        QTRY_COMPARE(m_left->currentName(), QString("renamed.txt"));
    }

    // Enter on a file opens it with the default app, which tests only record.
    void openingAFileStartsNothingReal()
    {
        writeFile(L("open-me.txt"), "x");
        QTRY_VERIFY(leftModel()->rowOf("open-me.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "open-me.txt");
        {
            QMutexLocker lock(&launchedMutex());
            launchedForTests().clear();
        }
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_VERIFY([] {
            QMutexLocker lock(&launchedMutex());
            return launchedForTests().join('\n').contains("open-me.txt");
        }());
    }

    // F2 renames in place, like Shift+F6, and has a button in the F-key bar.
    // Enter confirms the new name without also opening the file.
    void renameWithF2()
    {
        writeFile(L("f2.txt"), "x");
        QTRY_VERIFY(leftModel()->rowOf("f2.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "f2.txt");
        QSignalSpy opened(m_left->view(), &FileView::openRequested);
        QTest::keyClick(m_left->view(), Qt::Key_F2);
        QLineEdit* editor = nullptr;
        QTRY_VERIFY((editor = qobject_cast<QLineEdit*>(QApplication::focusWidget())));
        editor->setText("f2-renamed.txt");
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(L("f2-renamed.txt")));
        QVERIFY(!QFile::exists(L("f2.txt")));
        QCOMPARE(opened.count(), 0);
        QVERIFY(m_window->findChild<QAbstractButton*>("fkey-F2"));
    }

    void deletePermanentlyWithShiftF8()
    {
        moveCursorTo(m_left, "made");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8, Qt::ShiftModifier);
        QTRY_VERIFY(!QFile::exists(L("made")));
        QTRY_COMPARE(leftModel()->rowOf("made"), -1);
    }

    void externalChangesShowUp()
    {
        writeFile(L("from-outside.txt"), "hello");
        QTRY_VERIFY(leftModel()->rowOf("from-outside.txt") >= 0);
        QFile::remove(L("from-outside.txt"));
        QTRY_COMPARE(leftModel()->rowOf("from-outside.txt"), -1);
    }

    void tabs()
    {
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L(), L("sub")}));
        QTRY_COMPARE(m_left->path(), L("sub"));
        // Numbered titles; Alt+number switches.
        auto* bar = m_left->findChild<QTabBar*>();
        QVERIFY(bar);
        QCOMPARE(bar->tabText(0).left(1), QString("1"));
        QCOMPARE(bar->tabText(1), QString("2  sub"));
        QTest::keyClick(m_left->view(), Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(m_left->path(), L());
        QTest::keyClick(m_left->view(), Qt::Key_2, Qt::AltModifier);
        QTRY_COMPARE(m_left->path(), L("sub"));

        // Alt+5 creates tab 5 here; numbers stay put and sorted.
        QTest::keyClick(m_left->view(), Qt::Key_5, Qt::AltModifier);
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 2, 5}));
        QCOMPARE(m_left->path(), L("sub"));
        QCOMPARE(bar->tabText(2), QString("5  sub"));
        QTest::keyClick(m_left->view(), Qt::Key_2, Qt::AltModifier);
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);  // close tab 2
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 5}));
        QTest::keyClick(m_left->view(), Qt::Key_T, Qt::ControlModifier);   // takes the free 2
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 2, 5}));
        for (int n : {5, 2}) {
            m_left->selectSlot(n);
            QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);
        }
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L()}));
    }

    // A pinned tab stays in its folder: what it opens goes to a new tab. It
    // comes first, Ctrl+W leaves it, and workspaces neither save nor replace it.
    void pinnedTabs()
    {
        auto restore = qScopeGuard([this] {
            const QList<int> numbers = m_left->tabSlots();
            const QList<bool> pins = m_left->tabPins();
            for (int i = 0; i < numbers.size(); ++i)
                if (pins.value(i)) {
                    m_left->selectSlot(numbers[i]);
                    m_left->togglePinTab();
                }
            for (const Workspace& w : Workspaces::all())
                Workspaces::remove(w.id);
            m_left->replaceTabs({L()}, 0);
            QTRY_COMPARE(leftModel()->path(), L());
            m_left->view()->setFocus();
        });
        m_left->replaceTabs({L()}, 0);
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());
        m_left->togglePinTab();
        auto* bar = m_left->findChild<QTabBar*>();
        QCOMPARE(m_left->tabPins(), QList<bool>({true}));
        QVERIFY(!bar->tabIcon(0).isNull());

        // Entering a folder opens it in a new tab.
        moveCursorTo(m_left, "sub");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L(), L("sub")}));
        QTRY_COMPARE(m_left->path(), L("sub"));
        QCOMPARE(m_left->tabPins(), QList<bool>({true, false}));

        // Ctrl+W leaves it; going up from it opens a tab too.
        QTest::keyClick(m_left->view(), Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(m_left->path(), L());
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(m_left->tabPaths(), QStringList({L(), L("sub")}));
        m_left->goUp();
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L(), L("sub"), parentPath(L())}));
        QTRY_COMPARE(m_left->currentName(), baseName(L()));

        // Pinning moves a tab to the front; its number stays.
        m_left->togglePinTab();  // tab 3
        QCOMPARE(m_left->tabSlots(), QList<int>({1, 3, 2}));
        QCOMPARE(bar->tabText(1).left(1), QString("3"));
        m_left->togglePinTab();
        QCOMPARE(m_left->tabSlots(), QList<int>({1, 2, 3}));
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::ControlModifier);
        QTRY_COMPARE(m_left->tabSlots(), QList<int>({1, 2}));

        // Saved as a workspace without it; opening one keeps it.
        m_left->selectSlot(2);
        auto visiblePopup = [this] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    return p;
            return static_cast<FuzzyPopup*>(nullptr);
        };
        QTest::keyClick(m_left->view(), Qt::Key_W, Qt::AltModifier);
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY((popup = visiblePopup()));
        QTest::keyClicks(QApplication::focusWidget(), "Pinless");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->title.contains("Pinless"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(Workspaces::byName("Pinless"));
        QCOMPARE(Workspaces::byName("Pinless")->left.paths, QStringList({L("sub")}));
        QCOMPARE(Workspaces::byName("Pinless")->left.current, 0);
        m_left->replaceTabs({R()}, 0, {1});  // number 1 is the pinned tab's
        QCOMPARE(m_left->tabPaths(), QStringList({L(), R()}));
        QCOMPARE(m_left->tabPins(), QList<bool>({true, false}));
        QCOMPARE(m_left->tabSlots(), QList<int>({1, 2}));
        QTRY_COMPARE(m_left->path(), R());

        // Restored pinned, in front.
        Panel p;
        p.restoreTabs({L(), R()}, 1, {2, 1}, {}, {false, true});
        QCOMPARE(p.tabPaths(), QStringList({R(), L()}));
        QCOMPARE(p.tabPins(), QList<bool>({true, false}));
        QCOMPARE(p.currentTabIndex(), 0);
        QTRY_COMPARE(p.path(), R());
    }

    void restoredTabsKeepOrder()
    {
        Panel p;
        p.restoreTabs({L(), L("sub"), R()}, 1);
        QCOMPARE(p.tabPaths(), QStringList({L(), L("sub"), R()}));
        QCOMPARE(p.currentTabIndex(), 1);
        QTRY_COMPARE(p.path(), L("sub"));
    }

    void historyRemembersCursorAndMarks()
    {
        moveCursorTo(m_left, "photo.jpg");
        leftModel()->setMarked(leftModel()->rowOf("file1.txt"), true);
        moveCursorTo(m_left, "sub");
        moveCursorTo(m_left, "photo.jpg");
        m_left->navigate(L("sub"));
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        moveCursorTo(m_left, "inner.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(m_left->currentName(), QString("photo.jpg"));
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt"}));
        QTest::keyClick(m_left->view(), Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QCOMPARE(m_left->currentName(), QString("inner.txt"));
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L());
        QCOMPARE(leftModel()->markedNames(), QStringList({"file1.txt"}));
        leftModel()->markAll(false);
    }

    void contextMenuActsOnClickedItemOnly()
    {
        leftModel()->setMarked(leftModel()->rowOf("file1.txt"), true);
        leftModel()->setMarked(leftModel()->rowOf("file10.txt"), true);
        chooseFromNextMenu(QObject::tr("Copy full paths as text"));
        rightClick(m_left->view(), leftModel()->rowOf("photo.jpg"));
        QCOMPARE(QApplication::clipboard()->text(), L("photo.jpg"));
        QCOMPARE(leftModel()->markedNames().size(), 2);  // marks survive

        chooseFromNextMenu(QObject::tr("Copy full paths as text"));
        rightClick(m_left->view(), leftModel()->rowOf("file1.txt"));  // marked: acts on all marks
        QCOMPARE(QApplication::clipboard()->text(), L("file1.txt") + "\n" + L("file10.txt"));
        leftModel()->markAll(false);
    }

    void contextMenuCopiesNamesWithoutPaths()
    {
        chooseFromNextMenu(QObject::tr("Copy names as text"));
        rightClick(m_left->view(), leftModel()->rowOf("photo.jpg"));
        QCOMPARE(QApplication::clipboard()->text(), QString("photo.jpg"));

        leftModel()->setMarked(leftModel()->rowOf("file1.txt"), true);
        leftModel()->setMarked(leftModel()->rowOf("file10.txt"), true);
        chooseFromNextMenu(QObject::tr("Copy names as text"));
        rightClick(m_left->view(), leftModel()->rowOf("file1.txt"));
        QCOMPARE(QApplication::clipboard()->text(), QString("file1.txt\nfile10.txt"));
        leftModel()->markAll(false);
    }

    void contextMenuCopiesNamesWithoutExtension()
    {
        // A folder keeps its whole name: "v1.2" has no extension.
        QVERIFY(QDir().mkdir(L("v1.2")));
        QTRY_VERIFY(leftModel()->rowOf("v1.2") >= 0);
        leftModel()->setMarked(leftModel()->rowOf("photo.jpg"), true);
        leftModel()->setMarked(leftModel()->rowOf("v1.2"), true);
        chooseFromNextMenu(QObject::tr("Copy names without extension as text"));
        rightClick(m_left->view(), leftModel()->rowOf("photo.jpg"));
        const QStringList got = QApplication::clipboard()->text().split('\n');
        QCOMPARE(QSet<QString>(got.begin(), got.end()), QSet<QString>({"photo", "v1.2"}));
        leftModel()->markAll(false);
        QVERIFY(QDir().rmdir(L("v1.2")));
        QTRY_COMPARE(leftModel()->rowOf("v1.2"), -1);
    }

    void contextMenuCopiesFolderPath()
    {
        chooseFromNextMenu(QObject::tr("Copy folder path as text"));
        rightClick(m_left->view(), leftModel()->rowOf("photo.jpg"));
        QCOMPARE(QApplication::clipboard()->text(), leftModel()->path());
    }

    void contextMenuNewFileOnEmptyArea()
    {
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("blank.txt");
            dlg->accept();
        });
        chooseFromNextMenu(QObject::tr("New file…"));
        const QPoint empty(20, m_left->view()->viewport()->height() - 5);
        QContextMenuEvent ev(QContextMenuEvent::Mouse, empty, m_left->view()->viewport()->mapToGlobal(empty));
        QApplication::sendEvent(m_left->view()->viewport(), &ev);
        QTRY_VERIFY(QFile::exists(L("blank.txt")));
        QTRY_COMPARE(m_left->currentName(), QString("blank.txt"));
    }

    void switchPanelsWithTab()
    {
        QTest::keyClick(m_left->view(), Qt::Key_Tab);
        QTRY_VERIFY(m_right->isActive());
        QVERIFY(!m_left->isActive());
        QTest::keyClick(m_right->view(), Qt::Key_Tab);
        QTRY_VERIFY(m_left->isActive());
    }

    void sortBySize()
    {
        QTest::keyClick(m_left->view(), Qt::Key_F6, Qt::ControlModifier);
        QCOMPARE(m_left->sortSpec().column, SortColumn::Size);
        // Biggest file first, folders stay on top.
        int firstFile = -1;
        for (int r = 0; r < leftModel()->count(); ++r)
            if (!leftModel()->entry(r)->isDir) {
                firstFile = r;
                break;
            }
        QCOMPARE(leftModel()->entry(firstFile)->name, QString("photo.jpg"));
        QTest::keyClick(m_left->view(), Qt::Key_F3, Qt::ControlModifier);
        QCOMPARE(m_left->sortSpec().column, SortColumn::Name);
    }

    void missingFolderShowsError()
    {
        m_left->navigate(L("does-not-exist"));
        QTest::qWait(200);
        QCOMPARE(leftModel()->path(), L());  // stays where it was
    }

    void iconThemes()
    {
        const QString base = m_tmp.filePath("icons");
        QVERIFY(QDir().mkpath(base + "/Tandem Test/places/16"));
        QVERIFY(QDir().mkpath(base + "/SomeCursors/cursors"));
        writeFile(base + "/Tandem Test/index.theme",
                  "[Icon Theme]\nName=Tandem Test\nDirectories=places/16\n\n[places/16]\nSize=16\nType=Fixed\n");
        writeFile(base + "/SomeCursors/index.theme", "[Icon Theme]\nName=Some Cursors\n");
        QImage img(16, 16, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(base + "/Tandem Test/places/16/folder.png"));

        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << base);
        const QList<IconTheme> themes = scanIconThemes();
        const auto has = [&](const QString& id) {
            return std::any_of(themes.begin(), themes.end(), [&](const IconTheme& t) { return t.id == id; });
        };
        QVERIFY(has("Tandem Test"));
        QVERIFY(!has("SomeCursors"));  // cursor-only theme

        const QString before = QIcon::themeName();
        IconCache::setTheme("Tandem Test");
        QCOMPARE(QIcon::themeName(), QString("Tandem Test"));
        FileEntry dir;
        dir.isDir = true;
        const QImage got = IconCache::forEntry(dir).pixmap(16).toImage();
        QCOMPARE(got.pixelColor(8, 8), QColor(Qt::red));
        IconCache::setTheme({});  // back to the system theme
        QCOMPARE(QIcon::themeName(), before);
    }

    void archives()
    {
        // Pack with Alt+F5 into the right panel.
        moveCursorTo(m_left, "sub");
        answerNextDialog([this](QWidget* w) {
            auto* edit = w->findChild<QLineEdit*>();
            QVERIFY(edit);
            edit->setText(R("pack.zip"));
            acceptDialog(w);
        });
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::AltModifier);
        QTRY_VERIFY(QFileInfo(R("pack.zip")).size() > 0);
        QTRY_VERIFY(rightModel()->rowOf("pack.zip") >= 0);

        // Enter it like a folder, then a folder inside it.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        moveCursorTo(m_right, "pack.zip");
        QTest::keyClick(m_right->view(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip"));
        QVERIFY(m_right->inArchive());
        QVERIFY(rightModel()->rowOf("sub") >= 0);
        moveCursorTo(m_right, "sub");
        QTest::keyClick(m_right->view(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip/sub"));
        QVERIFY(rightModel()->rowOf("inner.txt") >= 0);

        // F3 views a member (extracted to the cache behind the scenes).
        moveCursorTo(m_right, "inner.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F3);
        ViewerWindow* viewer = nullptr;
        QTRY_VERIFY((viewer = m_window->findChild<ViewerWindow*>()) != nullptr);
        QTRY_COMPARE(viewer->textEdit()->toPlainText(), QString("inner"));
        viewer->close();
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>() == nullptr);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // F5 extracts into the other panel.
        QVERIFY(QDir().mkpath(L("unpacked")));
        m_left->navigate(L("unpacked"));
        QTRY_COMPARE(leftModel()->path(), L("unpacked"));
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F5);
        QTRY_VERIFY(QFile::exists(L("unpacked/inner.txt")));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // Archives are writable: F5 from outside adds, F7 makes a folder,
        // rename and F8 change members. Each is one rewrite of the archive.
        writeFile(L("unpacked/added.txt"), "added");
        QTRY_VERIFY(leftModel()->rowOf("added.txt") >= 0);
        m_left->view()->setFocus();
        moveCursorTo(m_left, "added.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_VERIFY(rightModel()->rowOf("added.txt") >= 0);
        QVERIFY(QFile::exists(L("unpacked/added.txt")));  // copied, not moved
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());

        answerNextDialog([](QWidget* w) {
            w->findChild<QLineEdit*>()->setText("made");
            acceptDialog(w);
        });
        QTest::keyClick(m_right->view(), Qt::Key_F7);
        QTRY_VERIFY(rightModel()->rowOf("made") >= 0);

        m_right->doRename("added.txt", "renamed.txt");
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);
        QVERIFY(rightModel()->rowOf("added.txt") < 0);

        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "inner.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F8);
        QTRY_VERIFY(rightModel()->rowOf("inner.txt") < 0);
        QString err;
        QCOMPARE(readFile(extractToCache(R("pack.zip"), "sub/renamed.txt", &err)), QByteArray("added"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // F4 edits a copy; every save goes back into the archive.
        const QString editorScript = m_tmp.filePath("archive-editor.sh");
        writeFile(editorScript, "#!/bin/sh\nprintf 'edited in zip' > \"$1\"\n");
        QFile::setPermissions(editorScript, QFile::permissions(editorScript) | QFile::ExeOwner);
        QSettings().setValue("tools/editor", editorScript);
        moveCursorTo(m_right, "renamed.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F4);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(extractToCache(R("pack.zip"), "sub/renamed.txt", &err)),
                                  QByteArray("edited in zip"), 10000);
        QSettings().remove("tools/editor");
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();

        // Backspace climbs out of the archive and lands on it.
        QTest::keyClick(m_right->view(), Qt::Key_Backspace);
        QTRY_COMPARE(rightModel()->path(), R("pack.zip"));
        QTest::keyClick(m_right->view(), Qt::Key_Backspace);
        QTRY_COMPARE(rightModel()->path(), R());
        QCOMPARE(m_right->currentName(), QString("pack.zip"));
        QVERIFY(!m_right->inArchive());

        // Alt+F9 unpacks the whole archive into the other panel.
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F9, Qt::AltModifier);
        QTRY_VERIFY(QFile::exists(L("unpacked/sub/deep")));  // single top folder: no extra level

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
    }

    void multiRenameAndUndo()
    {
        QVERIFY(QDir().mkpath(L("mr")));
        for (const char* n : {"b.txt", "a.txt", "c.txt", "keep.md"})
            writeFile(L(QString("mr/") + n), n);
        m_left->navigate(L("mr"));
        QTRY_COMPARE(leftModel()->path(), L("mr"));
        QVERIFY(leftModel()->markByPattern("*.txt", true) == 3);

        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<MultiRenameDialog*>(w);
            QVERIFY(dlg);
            // Typing "{" offers the fields; Tab takes the highlighted one.
            dlg->templateEdit()->clear();
            dlg->templateEdit()->setFocus();
            QTest::keyClicks(dlg->templateEdit(), "img_{n");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());
            QVERIFY(dlg->completionPopup()->currentItem()->text().startsWith("name"));
            QTest::keyClick(dlg->templateEdit(), Qt::Key_Escape);  // closes the list, not the dialog
            QVERIFY(!dlg->completionPopup()->isVisible());
            QVERIFY(dlg->isVisible());
            QTest::keyClicks(dlg->templateEdit(), ":");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());  // counter styles
            dlg->templateEdit()->setText("img_{n}{e");
            dlg->templateEdit()->setCursorPosition(dlg->templateEdit()->text().size());
            dlg->templateEdit()->setFocus();
            QTest::keyClicks(dlg->templateEdit(), "x");
            QTRY_VERIFY(dlg->completionPopup()->isVisible());
            QTest::keyClick(dlg->templateEdit(), Qt::Key_Tab);
            QCOMPARE(dlg->templateEdit()->text(), QString("img_{n}{ext}"));
            QTRY_VERIFY(dlg->renameButton()->isEnabled());
            QCOMPARE(dlg->renameButton()->text(), QString("Rename 3"));
            dlg->renameButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("mr/img_1.txt")));
        QCOMPARE(readName(L("mr/img_1.txt")), QByteArray("a.txt"));  // listing order: a, b, c
        QTRY_COMPARE(readName(L("mr/img_3.txt")), QByteArray("c.txt"));
        QVERIFY(QFile::exists(L("mr/keep.md")));
        QTRY_VERIFY(leftModel()->rowOf("img_2.txt") >= 0);

        // A mask producing a clash keeps the Rename button disabled.
        m_window->activateWindow();  // offscreen: the window stays inactive after a dialog
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        leftModel()->markByPattern("*", true);
        auto clashChecked = std::make_shared<bool>(false);
        answerNextDialog([clashChecked](QWidget* w) {
            auto* dlg = qobject_cast<MultiRenameDialog*>(w);
            QVERIFY(dlg);
            dlg->templateEdit()->setText("same");
            QTest::qWait(100);
            *clashChecked = !dlg->renameButton()->isEnabled();
            dlg->reject();
        });
        QTest::keyClick(m_left->view(), Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(*clashChecked);
        QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
        leftModel()->markAll(false);

        // Undo restores the old names.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("mr/a.txt")));
        QVERIFY(QFile::exists(L("mr/c.txt")));
        QVERIFY(!QFile::exists(L("mr/img_1.txt")));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // GUI editors that hand the file to a window and return get their wait
    // flag, so the names aren't read before they are edited.
    void editorWaitFlags()
    {
        auto restore = qScopeGuard([] { QSettings().remove("tools/nameEditor"); });
        QString error;
        auto argv = [&](const QString& editor) {
            QSettings().setValue("tools/nameEditor", editor);
            return EditNamesSession::editorCommand("/t/names.txt", &error);
        };
        QCOMPARE(argv("code"), QStringList({"code", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("/usr/local/bin/code %f"), QStringList({"/usr/local/bin/code", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("code -w"), QStringList({"code", "-w", "/t/names.txt"}));  // already waits
        QCOMPARE(argv("code --new-window --wait"), QStringList({"code", "--new-window", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("subl"), QStringList({"subl", "-w", "/t/names.txt"}));
        QCOMPARE(argv("zed"), QStringList({"zed", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("kate -b"), QStringList({"kate", "-b", "/t/names.txt"}));  // not one of them
        QCOMPARE(argv("vim"), QStringList({"vim", "/t/names.txt"}));
        // Names Arch packages use.
        QCOMPARE(argv("zeditor"), QStringList({"zeditor", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("code-oss"), QStringList({"code-oss", "--wait", "/t/names.txt"}));
        QCOMPARE(argv("vscodium"), QStringList({"vscodium", "--wait", "/t/names.txt"}));

        // A GUI editor in $VISUAL runs without a terminal around it.
        QSettings().remove("tools/nameEditor");
        const QByteArray oldVisual = qgetenv("VISUAL");
        const bool hadVisual = qEnvironmentVariableIsSet("VISUAL");
        auto restoreVisual = qScopeGuard([&] {
            if (hadVisual)
                qputenv("VISUAL", oldVisual);
            else
                qunsetenv("VISUAL");
        });
        qputenv("VISUAL", "code");
        QCOMPARE(EditNamesSession::editorCommand("/t/names.txt", &error),
                 QStringList({"code", "--wait", "/t/names.txt"}));
    }

    void renameInTextEditor()
    {
        QVERIFY(QDir().mkpath(L("ed")));
        for (const char* n : {"one.txt", "two.txt", "three.log"})
            writeFile(L(QString("ed/") + n), n);
        m_left->navigate(L("ed"));
        QTRY_COMPARE(leftModel()->path(), L("ed"));
        auto useEditor = [](const QString& cmd) { QSettings().setValue("tools/nameEditor", cmd); };

        // The "editor" swaps .txt for .md on every line.
        useEditor("sed -i.bak s/\\.txt$/.md/ %f");
        leftModel()->markByPattern("*", true);
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<RenameConfirmDialog*>(w);
            QVERIFY(dlg);
            QVERIFY(dlg->renameButton()->isEnabled());
            dlg->renameButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("ed/one.md")));
        QVERIFY(QFile::exists(L("ed/two.md")));
        QVERIFY(QFile::exists(L("ed/three.log")));
        QTRY_VERIFY(leftModel()->rowOf("one.md") >= 0);

        // A deleted line: warn, rename nothing.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        useEditor("sed -i.bak 1d %f");
        leftModel()->markByPattern("*", true);
        answerNextDialog([](QWidget* w) {
            auto* box = qobject_cast<QMessageBox*>(w);
            QVERIFY(box);
            QVERIFY(box->text().contains("Expected 3 lines"));
            box->button(QMessageBox::Cancel)->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
        QTest::qWait(200);
        QVERIFY(QFile::exists(L("ed/one.md")));

        // Nothing changed: no dialog at all.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        useEditor("true");
        QTest::keyClick(m_left->view(), Qt::Key_E, Qt::ControlModifier);
        QTest::qWait(400);
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        QSettings().remove("tools/nameEditor");
        leftModel()->markAll(false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void commandPaletteAndJump()
    {
        auto* popup = m_window->findChild<FuzzyPopup*>();
        QVERIFY(popup);

        // Palette: run "Show hidden files" by typing a few letters.
        QSettings().remove("palette/recent");
        QAction* hidden = nullptr;
        for (QAction* a : m_window->actions())
            if (a->text() == QObject::tr("Show hidden files"))
                hidden = a;
        QVERIFY(hidden);
        const bool before = hidden->isChecked();
        QTest::keyClick(m_left->view(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "shw hid");
        QTRY_COMPARE(popup->currentItem()->title.startsWith(QObject::tr("Show hidden files")), true);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(hidden->isChecked(), !before);
        hidden->trigger();  // back to how it was
        QTRY_COMPARE(hidden->isChecked(), before);

        // Used lately: on top of the empty palette, and first among similar
        // matches while typing.
        auto* model = qobject_cast<PopupModel*>(popup->findChild<QListView*>()->model());
        QVERIFY(model);
        auto openPalette = [&] {
            m_left->view()->setFocus();
            QTest::keyClick(m_left->view(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(popup->isVisible());
        };
        openPalette();
        QVERIFY(popup->currentItem()->title.startsWith(QObject::tr("Show hidden files")));
        QCOMPARE(model->header(0), QObject::tr("Recently used"));
        QTest::keyClicks(QApplication::focusWidget(), "zoom");
        QTRY_COMPARE(popup->currentItem()->title, QObject::tr("Zoom in"));  // by name
        QTest::keyClicks(QApplication::focusWidget(), " out");
        QTRY_COMPARE(popup->currentItem()->title, QObject::tr("Zoom out"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!popup->isVisible());
        m_left->zoomBy(0);
        openPalette();
        QCOMPARE(popup->currentItem()->title, QObject::tr("Zoom out"));
        QTest::keyClicks(QApplication::focusWidget(), "zoom");
        QTRY_COMPARE(popup->currentItem()->title, QObject::tr("Zoom out"));  // used lately
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible());
        QSettings().remove("palette/recent");

        // Jump: visited folders are offered; Enter goes there. Ctrl+J in the
        // palette switches to it, keeping what was typed.
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "sub");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(popup->findChild<QLineEdit*>()->placeholderText().startsWith(QObject::tr("Jump to a folder")));
        QCOMPARE(popup->query(), QString("sub"));
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key.startsWith(L("sub")));
        QTest::keyClicks(QApplication::focusWidget(), " dee");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == L("sub/deep"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_P, Qt::ControlModifier);  // pin it
        QVERIFY(FolderHistory::instance().isPinned(L("sub/deep")));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        FolderHistory::instance().setPinned(L("sub/deep"), false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Hints name the keys as they are: with modal keys on, the letters.
    void keyHintsFollowModalKeys()
    {
        QAction* vim = nullptr;
        for (QAction* a : m_window->actions())
            if (a->property("commandId").toString() == "modal-keys-vim-yazi")
                vim = a;
        QVERIFY(vim);
        auto* trash = static_cast<KeyButton*>(m_window->findChild<QAbstractButton*>("fkey-F8"));
        auto* copy = static_cast<KeyButton*>(m_window->findChild<QAbstractButton*>("fkey-F5"));
        QVERIFY(trash && copy);
        QTRY_COMPARE(trash->keys(), QString("F8"));
        vim->trigger();
        auto off = qScopeGuard([vim] {
            if (vim->isChecked())
                vim->trigger();
        });
        QVERIFY(vim->isChecked());
        QTRY_COMPARE(trash->keys(), QString("d"));
        QCOMPARE(copy->keys(), QString("F5"));  // no letter for it: the F-key still works
        QCOMPARE(m_window->keyHint("command-palette"), QString(":"));
        QCOMPARE(m_window->keyHint("command-palette", false), QKeySequence("Ctrl+Shift+P").toString(QKeySequence::NativeText));
        vim->trigger();
        QTRY_COMPARE(trash->keys(), QString("F8"));
    }

    void undoWithCtrlZ()
    {
        qputenv("XDG_DATA_HOME", QFile::encodeName(m_tmp.filePath("data")));  // private trash
        QVERIFY(QDir().mkpath(m_tmp.filePath("data")));
        QVERIFY(QDir().mkpath(L("u")));
        writeFile(L("u/doc.txt"), "doc");
        m_left->navigate(L("u"));
        QTRY_COMPARE(leftModel()->path(), L("u"));
        auto focusLeft = [this] {
            m_window->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
            m_left->view()->setFocus();
        };

        // Move to the right panel, then undo: it comes back.
        moveCursorTo(m_left, "doc.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F6);
        QTRY_VERIFY(QFile::exists(R("doc.txt")));
        QTRY_VERIFY(!QFile::exists(L("u/doc.txt")));
        focusLeft();
        QTest::qWait(100);
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("u/doc.txt")));
        QTRY_VERIFY(!QFile::exists(R("doc.txt")));

        // Trash, then undo: restored.
        QTRY_VERIFY(leftModel()->rowOf("doc.txt") >= 0);
        focusLeft();
        moveCursorTo(m_left, "doc.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8);
        QTRY_VERIFY(!QFile::exists(L("u/doc.txt")));
        focusLeft();
        QTest::qWait(100);
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(QFile::exists(L("u/doc.txt")));
        QCOMPARE(readName(L("u/doc.txt")), QByteArray("doc"));

        // New folder, then undo: gone (it's empty).
        focusLeft();
        answerNextDialog([](QWidget* w) {
            auto* dlg = qobject_cast<QInputDialog*>(w);
            QVERIFY(dlg);
            dlg->setTextValue("fresh/inner");
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(L("u/fresh/inner")).isDir());
        focusLeft();
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFile::exists(L("u/fresh")));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void filterAsYouType()
    {
        QVERIFY(QDir().mkpath(L("flt/inside")));
        for (const char* n : {"alpha.txt", "beta.txt", "gamma.jpg", "alpine.jpg"})
            writeFile(L(QString("flt/") + n), n);
        m_left->navigate(L("flt"));
        QTRY_COMPARE(leftModel()->path(), L("flt"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        auto visible = [this] {
            QStringList out;
            for (int r = 0; r < leftModel()->count(); ++r)
                if (!leftModel()->entry(r)->isParent)
                    out << leftModel()->entry(r)->name;
            return out;
        };
        QTest::keyClick(m_left->view(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "al");
        QTRY_COMPARE(visible(), QStringList({"alpha.txt", "alpine.jpg"}));
        QTest::keyClicks(QApplication::focusWidget(), " *.jpg");
        QTRY_COMPARE(visible(), QStringList({"alpine.jpg"}));
        QCOMPARE(m_left->currentName(), QString("alpine.jpg"));

        // Live updates respect the filter.
        writeFile(L("flt/also.jpg"), "x");     // "al" + jpg: shows up
        writeFile(L("flt/other.jpg"), "x");    // hidden
        QTRY_VERIFY(leftModel()->rowOf("also.jpg") >= 0);
        QTest::qWait(300);
        QCOMPARE(leftModel()->rowOf("other.jpg"), -1);

        // Esc brings everything back.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_VERIFY(leftModel()->rowOf("other.jpg") >= 0);
        QCOMPARE(leftModel()->rowOf("beta.txt") >= 0, true);

        // A new folder starts unfiltered.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "inside");
        QTRY_COMPARE(visible(), QStringList({"inside"}));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        moveCursorTo(m_left, "inside");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("flt/inside"));
        QCOMPARE(leftModel()->filter(), QString());
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L("flt"));
        QVERIFY(leftModel()->rowOf("beta.txt") >= 0);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+A: the permissions editor, with the octal field and extra people.
    void permissionsEditor()
    {
        QVERIFY(QDir().mkpath(L("pe")));
        writeFile(L("pe/doc.txt"), "x");
        ::chmod(QFile::encodeName(L("pe/doc.txt")).constData(), 0600);
        m_left->navigate(L("pe"));
        QTRY_COMPARE(leftModel()->path(), L("pe"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "doc.txt");
        const bool acl = readPermissions(L("pe/doc.txt")).aclSupported;
        auto checked = std::make_shared<bool>(false);
        answerNextDialog([acl, checked](QWidget* w) {
            auto* dlg = qobject_cast<PermissionsDialog*>(w);
            QVERIFY(dlg);
            QCOMPARE(dlg->octalEdit()->text(), QString("600"));
            QVERIFY(!dlg->applyButton()->isEnabled());  // nothing changed yet
            dlg->bitBox(1, 0)->click();                  // group may read
            QCOMPARE(dlg->octalEdit()->text(), QString("640"));
            dlg->octalEdit()->setText("644");            // typed as a number
            emit dlg->octalEdit()->textEdited("644");
            QVERIFY(dlg->bitBox(2, 0)->isChecked());
            if (acl) {
                dlg->addEdit()->setText("nobody");
                QTest::keyClick(dlg->addEdit(), Qt::Key_Return);
            }
            QVERIFY(dlg->applyButton()->isEnabled());
            *checked = true;
            dlg->applyButton()->click();
        });
        QTest::keyClick(m_left->view(), Qt::Key_A, Qt::AltModifier);
        QTRY_VERIFY(*checked);
        QTRY_COMPARE(readPermissions(L("pe/doc.txt")).mode, 0644u);
        if (acl) {
            QTRY_COMPARE(readPermissions(L("pe/doc.txt")).acl.size(), 1);
            QCOMPARE(readPermissions(L("pe/doc.txt")).acl[0].perms, uint32_t(PermRead));
        }
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+Shift+C: compute and save checksums, then check them.
    void checksumRoundTrip()
    {
        QVERIFY(QDir().mkpath(L("ck")));
        writeFile(L("ck/one.iso"), "first");
        writeFile(L("ck/two.iso"), "second");
        writeFile(L("ck/three.iso"), "third");
        QString saved;
        {
            ChecksumDialog make({L("ck/one.iso"), L("ck/two.iso"), L("ck/three.iso")}, m_window.get());
            make.show();
            QVERIFY(!make.isVerifying());
            QTRY_VERIFY(!make.isBusy());
            QCOMPARE(make.list()->topLevelItem(0)->text(1).size(), 64);  // SHA-256 by default
            make.saveOne();
            saved = make.saved();
        }
        QCOMPARE(saved, L("ck/ck.sha256"));
        writeFile(L("ck/two.iso"), "changed!");
        QFile::remove(L("ck/three.iso"));
        ChecksumDialog check({saved}, m_window.get());
        check.show();
        QVERIFY(check.isVerifying());
        QTRY_VERIFY(!check.isBusy());
        QCOMPARE(check.summary(), QObject::tr("%1 OK · %2 different · %3 missing").arg(1).arg(1).arg(1));
    }

    // Ctrl+Shift+U: disk usage, browse down and up, delete with sizes updating.
    void diskUsageWindow()
    {
        QVERIFY(QDir().mkpath(L("du/videos")));
        QVERIFY(QDir().mkpath(L("du/docs")));
        writeFile(L("du/videos/film.mkv"), QByteArray(3 << 20, 'v'));
        writeFile(L("du/videos/clip.mp4"), QByteArray(1 << 20, 'c'));
        writeFile(L("du/docs/a.txt"), "a");
        DiskUsageDialog dlg(L("du"), false, m_window.get());
        dlg.show();
        QTRY_VERIFY(!dlg.isCounting());
        QCOMPARE(dlg.list()->topLevelItem(0)->text(0), QString("videos"));  // biggest first
        dlg.list()->setCurrentItem(dlg.list()->topLevelItem(0));
        QTest::keyClick(dlg.list(), Qt::Key_Return);
        QCOMPARE(dlg.current()->name, QString("videos"));
        QCOMPARE(dlg.list()->topLevelItem(0)->text(0), QString("film.mkv"));
        const int64_t before = dlg.current()->parent->size;
        QSignalSpy deleted(&dlg, &DiskUsageDialog::deleteRequested);
        dlg.list()->setCurrentItem(dlg.list()->topLevelItem(0));
        dlg.deleteSelected(false);
        QCOMPARE(deleted.size(), 1);
        QCOMPARE(deleted.first().at(0).toStringList(), QStringList({L("du/videos/film.mkv")}));
        QVERIFY(dlg.current()->parent->size < before);
        QTest::keyClick(dlg.list(), Qt::Key_Backspace);
        QCOMPARE(dlg.current()->name, L("du"));
        QCOMPARE(dlg.list()->currentItem()->text(0), QString("videos"));  // back on the folder we came from
    }

    // Alt+Shift+F7: duplicates, keep one of each, the rest to the trash.
    void duplicateFinder()
    {
        QVERIFY(QDir().mkpath(L("dup/a")));
        QVERIFY(QDir().mkpath(L("dup/b")));
        writeFile(L("dup/a/song.mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/b/song (1).mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/b/song (2).mp3"), QByteArray(200000, 's'));
        writeFile(L("dup/a/unique.txt"), "only one");
        QFile f(L("dup/a/song.mp3"));  // the oldest copy
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
        f.close();
        m_left->navigate(L("dup"));
        QTRY_COMPARE(leftModel()->path(), L("dup"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        auto checked = std::make_shared<bool>(false);
        answerNextDialog([checked](QWidget* w) {
            auto* dlg = qobject_cast<DuplicatesDialog*>(w);
            QVERIFY(dlg);
            dlg->search();
            QTRY_VERIFY(!dlg->isSearching());
            QCOMPARE(dlg->results()->topLevelItemCount(), 1);
            QCOMPARE(dlg->results()->topLevelItem(0)->childCount(), 3);
            dlg->keep(false);  // the oldest stays
            QCOMPARE(dlg->ticked().size(), 2);
            QVERIFY(!dlg->ticked().contains(dlg->results()->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toString()));
            for (auto* b : dlg->findChildren<QPushButton*>())
                if (b->text().startsWith(QObject::tr("Move")))
                    b->click();
            QCOMPARE(dlg->results()->topLevelItemCount(), 0);  // one left: not a duplicate anymore
            *checked = true;
            dlg->accept();
        });
        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_VERIFY(*checked);
        QTRY_VERIFY(!QFile::exists(L("dup/b/song (1).mp3")) && !QFile::exists(L("dup/b/song (2).mp3")));
        QVERIFY(QFile::exists(L("dup/a/song.mp3")));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Ctrl+Shift+F1: thumbnails in a grid, remembered for the folder.
    void thumbnailGrid()
    {
        QVERIFY(QDir().mkpath(L("photos/inner")));
        const QList<QColor> colors = {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta, Qt::cyan};
        for (int i = 0; i < colors.size(); ++i) {
            QImage img(400, 300, QImage::Format_RGB32);
            img.fill(colors[i]);
            QVERIFY(img.save(L(QString("photos/p%1.jpg").arg(i)), "JPG"));
        }
        writeFile(L("photos/notes.txt"), "text");
        m_left->navigate(L("photos"));
        QTRY_COMPARE(leftModel()->path(), L("photos"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QVERIFY(!m_left->view()->thumbnails());
        QTest::keyClick(m_left->view(), Qt::Key_F1, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(m_left->view()->thumbnails());
        ThumbGrid* grid = m_left->view()->thumbGrid();
        QTRY_VERIFY(grid->isVisible());

        // The pictures show up (made in the background).
        auto colorIn = [&](int row) {
            const QImage shot = grid->grab().toImage();
            const QRect r = grid->cellRect(row);
            return shot.pixelColor(r.left() + 8 + grid->cellSize() / 2, r.top() + 8 + grid->cellSize() / 2);
        };
        const int p2 = leftModel()->rowOf("p2.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(colorIn(p2).blue() - 255) < 40 && colorIn(p2).red() < 60, 10000);

        // Arrows: sideways one, up/down a row.
        m_left->view()->setCurrentRow(0);
        QTest::keyClick(m_left->view(), Qt::Key_Right);
        QCOMPARE(m_left->view()->currentRow(), 1);
        QTest::keyClick(m_left->view(), Qt::Key_Down);
        QCOMPARE(m_left->view()->currentRow(), qMin(1 + grid->columns(), leftModel()->count() - 1));

        // The grid belongs to the tab: it stays while moving around, and a
        // new tab opened from here starts as a grid too.
        QVERIFY(!m_left->view()->verticalScrollBar()->isVisible());  // only the grid's own scrollbar
        moveCursorTo(m_left, "inner");
        QTest::keyClick(m_left->view(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("photos/inner"));
        QVERIFY(m_left->view()->thumbnails());
        QTest::keyClick(m_left->view(), Qt::Key_Backspace);
        QTRY_COMPARE(leftModel()->path(), L("photos"));
        QVERIFY(m_left->view()->thumbnails());
        QCOMPARE(m_left->tabGrids(), QList<bool>{true});  // saved with the tabs

        // Zoom: the slider and Ctrl+= / Ctrl+0 change the grid of both panels.
        const int before = grid->cellSize();
        QTest::keyClick(m_left->view(), Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(grid->cellSize(), before + ThumbGrid::kSizeStep);
        QTest::keyClick(m_left->view(), Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(grid->cellSize(), int(ThumbGrid::kDefaultSize));

        QTest::keyClick(m_left->view(), Qt::Key_F1, Qt::ControlModifier | Qt::ShiftModifier);  // off again
        QVERIFY(!m_left->view()->thumbnails());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void compareRemoteFiles()
    {
        // Diff can't read server locations: Tandem downloads first, and a
        // failed diff never reads as "identical".
        QTemporaryDir serverDir, localDir;
        writeFile(serverDir.filePath("a.txt"), "server\n");
        writeFile(localDir.filePath("a.txt"), "local\n");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        auto unregister = qScopeGuard([] { registerBackend("test", nullptr); });
        const QVariant oldTool = QSettings().value("tools/diff");
        auto restore = qScopeGuard([oldTool] {
            if (oldTool.isValid())
                QSettings().setValue("tools/diff", oldTool);
            else
                QSettings().remove("tools/diff");
        });
        QSettings().setValue("tools/diff", "/nonexistent/tandem-diff");  // use the built-in view
        if (QStandardPaths::findExecutable("diff").isEmpty())
            QSKIP("diff is not installed");

        SyncDialog dlg(localDir.path(), "test://srv/", [](const SyncPlan&) { return QList<Job*>{}; }, m_window.get());
        dlg.show();
        QTRY_VERIFY(!dlg.isComparing());
        dlg.model()->setShown(SyncModel::GroupDiffers, true);
        const int row = dlg.model()->rowOf("a.txt");
        QVERIFY(row >= 0);
        dlg.view()->setCurrentIndex(dlg.model()->index(row, 0));
        QTest::keyClick(dlg.view(), Qt::Key_Return);
        QPlainTextEdit* text = nullptr;
        QTRY_VERIFY((text = dlg.findChild<QPlainTextEdit*>()) != nullptr);
        QVERIFY(text->toPlainText().contains("-local"));
        QVERIFY(text->toPlainText().contains("+server"));
    }

    void backgroundRefreshKeepsLatestCursorAndMarks()
    {
        QTemporaryDir serverDir;
        writeFile(serverDir.filePath("a.txt"), "a");
        writeFile(serverDir.filePath("b.txt"), "b");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        auto unregister = qScopeGuard([] { registerBackend("test", nullptr); });
        m_right->navigate("test://srv/");
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        moveCursorTo(m_right, "a.txt");
        backend->latencyMs = 250;
        m_right->refresh();
        QVERIFY(m_right->tab()->isLoading());
        moveCursorTo(m_right, "b.txt");
        rightModel()->setMarked(rightModel()->rowOf("b.txt"), true);
        QTRY_VERIFY(!m_right->tab()->isLoading());
        QCOMPARE(m_right->currentName(), QString("b.txt"));
        QVERIFY(rightModel()->markedNames().contains("b.txt"));
        backend->latencyMs = 0;
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        QVERIFY(ioPool()->waitForDone(5000));
    }

    // Ctrl+C on a server, Ctrl+V in a local folder (and back) copies the files.
    void clipboardWithServer()
    {
        QTemporaryDir serverDir;
        writeFile(serverDir.filePath("from server.txt"), "s");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        auto unregister = qScopeGuard([] { registerBackend("test", nullptr); });
        QVERIFY(QDir().mkpath(L("clip")));
        writeFile(L("clip/local.txt"), "l");
        m_left->navigate(L("clip"));
        QTRY_COMPARE(leftModel()->path(), L("clip"));
        m_right->navigate("test://srv/");
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        QTRY_VERIFY(rightModel()->rowOf("from server.txt") >= 0);

        moveCursorTo(m_right, "from server.txt");
        m_right->view()->setFocus();
        QTest::keyClick(m_right->view(), Qt::Key_C, Qt::ControlModifier);
        // Other programs get a real server URL, not a local file.
        const QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.first().scheme(), QString("test"));
        QCOMPARE(urls.first().path(), QString("/from server.txt"));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(L("clip/from server.txt")), QByteArray("s"), 5000);

        moveCursorTo(m_left, "local.txt");
        QTest::keyClick(m_left->view(), Qt::Key_C, Qt::ControlModifier);
        m_right->view()->setFocus();
        QTest::keyClick(m_right->view(), Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(serverDir.filePath("local.txt")), QByteArray("l"), 5000);

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        QVERIFY(ioPool()->waitForDone(5000));
    }

    // Servers: saved with their login, imported from the SSH config, offered
    // under their name in Locations; a recent one is saved with Ctrl+S.
    void savedServersWindow()
    {
        ServerList& list = ServerList::instance();
        const QList<SavedServer> before = list.servers();
        QStringList stored;
        setKeyringHooksForTests({[](const QString&, const QString&, const QString&) { return std::optional<StoredLogin>(); },
                                 [&stored](const QString& protocol, const QString& server, const StoredLogin& login) {
                                     stored << protocol + " " + server + " " + login.domain + "\\" + login.user + " " + login.password;
                                     return true;
                                 }});
        auto restore = qScopeGuard([&] {
            QVERIFY(ioPool()->waitForDone(5000));
            setKeyringHooksForTests({});
            list.save(before);
            Network::instance().forgetServer("sftp://zed@other.lan");
        });
        QVERIFY(list.save({}));
        QTemporaryDir tmp;
        writeFile(tmp.filePath("ssh_config"), "Host box\n  HostName box.lan\n  User bob\n  IdentityFile ~/.ssh/box\nHost *\n  User x\n");

        {
            ServersDialog dlg(m_window.get());
            dlg.sshConfigPath = tmp.filePath("ssh_config");
            dlg.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dlg));
            for (auto* b : dlg.findChildren<QPushButton*>())
                if (b->text() == QObject::tr("&Add"))
                    b->click();
            for (auto* b : dlg.findChildren<QToolButton*>())
                if (b->text() == QObject::tr("SMB (Windows share)"))
                    b->click();
            dlg.findChild<QLineEdit*>("name")->setText("Qwertz share");
            dlg.findChild<QLineEdit*>("host")->setText("nas.lan");
            dlg.findChild<QLineEdit*>("user")->setText("bob");
            dlg.findChild<QLineEdit*>("domain")->setText("HOME");
            dlg.findChild<QLineEdit*>("password")->setText("secret");
            dlg.findChild<QLineEdit*>("folder")->setText("media");
            // The port shows the usual one as a hint; empty keeps it.
            auto* port = dlg.findChild<QLineEdit*>("port");
            QCOMPARE(port->placeholderText(), QString("445"));
            port->setText("1445");
            QCOMPARE(dlg.servers().last().port, 1445);
            port->clear();
            QCOMPARE(dlg.servers().last().port, 0);
            QVERIFY(!dlg.findChild<QLineEdit*>("keyFile")->isVisibleTo(&dlg));  // SSH only

            answerNextDialog([](QWidget* w) {
                auto* picks = w->findChild<QListWidget*>();
                QVERIFY(picks);
                QCOMPARE(picks->count(), 1);  // "Host *" isn't a server
                picks->item(0)->setCheckState(Qt::Checked);
                acceptDialog(w);
            });
            dlg.importFromSsh();
            QCOMPARE(dlg.servers().size(), 2);
            QVERIFY(dlg.save());
        }
        QCOMPARE(list.servers().size(), 2);
        QCOMPARE(list.servers()[0].uri(), QString("smb://HOME;bob@nas.lan/media"));
        QCOMPARE(list.servers()[1].uri(), QString("sftp://bob@box.lan"));
        QCOMPARE(list.servers()[1].keyFile, QString("~/.ssh/box"));
        QTRY_COMPARE(stored, QStringList({"smb nas.lan HOME\\bob secret"}));
        QVERIFY(readFile(list.path()).contains("key-file = \"~/.ssh/box\""));
        QVERIFY(!readFile(list.path()).contains("secret"));  // passwords stay in the keyring

        // Locations: saved servers by name; a recent one only when not saved.
        Network::instance().rememberServer("sftp://bob@box.lan");
        Network::instance().rememberServer("sftp://zed@other.lan");
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        auto openLocations = [&]() -> FuzzyPopup* {
            m_left->showLocations();
            FuzzyPopup* popup = nullptr;
            [&] { QTRY_VERIFY([&] {
                for (auto* p : m_window->findChildren<FuzzyPopup*>())
                    if (p->isVisible())
                        popup = p;
                return popup != nullptr;
            }()); }();
            return popup;
        };
        FuzzyPopup* popup = openLocations();
        QVERIFY(popup);
        QTest::keyClicks(QApplication::focusWidget(), "qwertz");
        QTRY_VERIFY(popup->currentItem());
        QCOMPARE(popup->currentItem()->key, QString("saved:smb://HOME;bob@nas.lan/media"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "bob@box");
        QTRY_VERIFY(popup->currentItem());
        QVERIFY(!popup->currentItem()->key.startsWith("recent:"));  // listed as the saved "box"
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "other.lan");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == "recent:sftp://zed@other.lan");
        QString offered;
        answerNextDialog([&offered](QWidget* w) {
            if (auto* d = qobject_cast<ServersDialog*>(w))
                offered = d->servers().last().uri();
            QMetaObject::invokeMethod(w, "reject", Qt::QueuedConnection);
        });
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(offered, QString("sftp://zed@other.lan"));
        QTRY_VERIFY(!QApplication::activeModalWidget());
        QCOMPARE(list.servers().size(), 2);  // cancelled: nothing saved

        // Saved and recent servers carry a type badge, the address without
        // its scheme; Ctrl+E opens the saved one in the servers window.
        popup = openLocations();
        QVERIFY(popup);
        QTest::keyClicks(popup->findChild<QLineEdit*>(), "qwertz");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key.startsWith("saved:"));
        QCOMPARE(popup->currentItem()->badge, QString("SMB"));
        QCOMPARE(popup->currentItem()->subtitle, QString("HOME;bob@nas.lan/media"));
        QString shown;
        answerNextDialog([&shown](QWidget* w) {
            if (auto* d = qobject_cast<ServersDialog*>(w))
                shown = d->list()->currentItem() ? d->list()->currentItem()->text() : QString("?");
            QMetaObject::invokeMethod(w, "reject", Qt::QueuedConnection);
        });
        QTest::keyClick(popup->findChild<QLineEdit*>(), Qt::Key_E, Qt::ControlModifier);
        QTRY_COMPARE(shown, QString("Qwertz share"));
        QTRY_VERIFY(!QApplication::activeModalWidget());
        QCOMPARE(list.servers().size(), 2);  // nothing added

        // The servers window is offered in the Servers section itself.
        popup = openLocations();
        QTest::keyClicks(popup->findChild<QLineEdit*>(), "edit servers");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->title == "Edit servers…");
        QTest::keyClick(popup->findChild<QLineEdit*>(), Qt::Key_Escape);
    }

    // Server folders notice changes made elsewhere (polled while shown).
    void serverFolderRefreshes()
    {
        QTemporaryDir serverDir;
        writeFile(serverDir.filePath("a.txt"), "a");
        writeFile(serverDir.filePath("b.txt"), "b");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        m_right->navigate("test://srv/");
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        QTRY_VERIFY(rightModel()->rowOf("b.txt") >= 0);
        QVERIFY(m_right->tab()->needsPolling());
        m_right->view()->setCurrentRow(rightModel()->rowOf("b.txt"));

        const int before = backend->calls;
        m_right->tab()->poll();  // nothing changed: one listing, no reload
        QTRY_VERIFY(backend->calls > before);
        QTest::qWait(100);
        QCOMPARE(m_right->currentName(), QString("b.txt"));

        writeFile(serverDir.filePath("new-from-elsewhere.txt"), "n");  // someone else
        m_right->tab()->poll();
        QTRY_VERIFY(rightModel()->rowOf("new-from-elsewhere.txt") >= 0);
        QCOMPARE(m_right->currentName(), QString("b.txt"));  // the cursor stays
#ifdef Q_OS_MACOS
        QVERIFY(m_left->tab()->needsPolling()); // catches file content/metadata changes beside kqueue
#else
        QVERIFY(!m_left->tab()->needsPolling()); // local folders have inotify
#endif

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        // A poll can still be reading the mock server after navigation completes.
        QVERIFY(ioPool()->waitForDone(5000));
        registerBackend("test", nullptr);
    }

    // The trash window: lists what's there, restores, deletes for good.
    void trashWindow()
    {
#ifdef Q_OS_MACOS
        QSKIP("macOS Trash browsing is delegated to Finder; native trash/undo is tested separately");
#endif
        QVERIFY(QDir().mkpath(L("tw")));
        writeFile(L("tw/old-report.pdf"), "pdf");
        writeFile(L("tw/gone.txt"), "bye");
        QVERIFY(QFile::moveToTrash(L("tw/old-report.pdf")));
        QVERIFY(QFile::moveToTrash(L("tw/gone.txt")));
        TrashDialog dlg(m_window.get());
        dlg.show();
        QTRY_VERIFY(!dlg.isLoading());
        auto rowOf = [&](const QString& name) -> QTreeWidgetItem* {
            for (int i = 0; i < dlg.list()->topLevelItemCount(); ++i)
                if (dlg.list()->topLevelItem(i)->text(0) == name)
                    return dlg.list()->topLevelItem(i);
            return nullptr;
        };
        QTRY_VERIFY(rowOf("old-report.pdf") && rowOf("gone.txt"));
        QCOMPARE(rowOf("old-report.pdf")->text(1), L("tw"));
        dlg.list()->clearSelection();
        rowOf("old-report.pdf")->setSelected(true);
        dlg.restoreSelected();
        QTRY_COMPARE(readFile(L("tw/old-report.pdf")), QByteArray("pdf"));
        QTRY_VERIFY(!dlg.isLoading() && !rowOf("old-report.pdf"));
        rowOf("gone.txt")->setSelected(true);
        dlg.deleteSelected(false);
        QTRY_VERIFY(!dlg.isLoading() && !rowOf("gone.txt"));
        QVERIFY(!QFile::exists(L("tw/gone.txt")));
    }

    // Ctrl+Shift+F5: a link in the other panel; Ctrl+Z takes it away again.
    // Alt+F5: the format picks the extension (and typing one picks the
    // format); only ZIP and 7-Zip take a password, typed twice.
    void packDialog()
    {
        QSettings().remove("pack/format");
        auto restore = qScopeGuard([] { QSettings().remove("pack/format"); });
        bool checked = false;
        answerNextDialog([&checked](QWidget* w) {
            auto* target = w->findChild<QLineEdit*>("target");
            auto* format = w->findChild<QComboBox*>("format");
            auto* password = w->findChild<QLineEdit*>("password");
            auto* repeat = w->findChild<QLineEdit*>("repeat");
            auto* buttons = w->findChild<QDialogButtonBox*>();
            QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
            QCOMPARE(target->text(), QString("/data/photos.zip"));
            QCOMPARE(target->selectedText(), QString("photos"));
            format->setCurrentIndex(format->findText("tar.zst", Qt::MatchContains));
            QCOMPARE(target->text(), QString("/data/photos.tar.zst"));
            QVERIFY(!password->isEnabled());
            target->setFocus();
            target->selectAll();
            QTest::keyClicks(target, "/data/pics.zip");
            QVERIFY(format->currentText().contains(".zip"));
            QVERIFY(password->isEnabled());
            password->setText("secret");
            QVERIFY(!ok->isEnabled());  // not repeated yet
            repeat->setText("secreT");
            QVERIFY(!ok->isEnabled());
            repeat->setText("secret");
            QVERIFY(ok->isEnabled());
            checked = true;
            ok->click();
        });
        const auto choice = askPack(m_window.get(), "2 items", "/data/photos");
        QVERIFY(checked);
        QVERIFY(choice);
        QCOMPARE(choice->target, QString("/data/pics.zip"));
        QCOMPARE(choice->password, QString("secret"));
        QCOMPARE(QSettings().value("pack/format").toString(), QString(".zip"));
    }

    // Enter in a conflict overwrites only when that loses nothing newer.
    void conflictDefault()
    {
        auto defaultAnswer = [](int64_t srcTime, int64_t dstTime) {
            PromptRequest req;
            req.kind = PromptRequest::Kind::Conflict;
            req.srcPath = "/a/f";
            req.dstPath = "/b/f";
            req.srcTime = srcTime;
            req.dstTime = dstTime;
            PromptDialog dlg(req, nullptr);
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->isDefault())
                    return b->text();
            return QString();
        };
        QCOMPARE(defaultAnswer(2000, 1000), QObject::tr("&Overwrite"));
        QCOMPARE(defaultAnswer(1000, 1000), QObject::tr("&Overwrite"));
        QCOMPARE(defaultAnswer(1000, 2000), QObject::tr("&Skip"));
    }

    // macOS: a button's underlined letter presses it (alone, or with Option
    // in a text field). Installed here by hand; Linux has it built in.
    void dialogLetterKeys()
    {
        std::unique_ptr<QObject> keys(installDialogKeys(qApp));
        auto answerFor = [](QKeyCombination key) {
            PromptRequest req;
            req.kind = PromptRequest::Kind::TrashFailed;
            req.srcPath = "/Volumes/share/f";
            PromptDialog dlg(req, nullptr);
            int result = -1;
            QObject::connect(&dlg, &QDialog::finished, [&result](int r) { result = r; });
            dlg.show();
            dlg.activateWindow();
            if (!QTest::qWaitForWindowActive(&dlg))
                return -2;
            QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &dlg, key.key(),
                            key.keyboardModifiers());
            QTest::qWaitFor([&] { return result != -1; }, 2000);
            return result;
        };
        QCOMPARE(answerFor(Qt::Key_S), int(Answer::Skip));
        QCOMPARE(answerFor(Qt::Key_A), int(Answer::DeletePermanentlyAll));
        QCOMPARE(answerFor(Qt::ALT | Qt::Key_D), int(Answer::DeletePermanently));
        QCOMPARE(answerFor(Qt::Key_C), int(Answer::Cancel));
        QCOMPARE(answerFor(Qt::Key_Return), int(Answer::DeletePermanently));

        // Typing in a field stays typing; Option+letter still presses.
        QDialog dlg;
        auto* edit = new QLineEdit(&dlg);
        auto* ok = new QPushButton("&Save", &dlg);
        int saved = 0;
        QObject::connect(ok, &QPushButton::clicked, [&saved] { ++saved; });
        auto* layout = new QVBoxLayout(&dlg);
        layout->addWidget(edit);
        layout->addWidget(ok);
        dlg.show();
        dlg.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dlg));
        edit->setFocus();
        QTest::keyClicks(edit, "ss");
        QCOMPARE(edit->text(), QString("ss"));
        QCOMPARE(saved, 0);
        QTest::keyClick(edit, Qt::Key_S, Qt::AltModifier);
        QTRY_COMPARE(saved, 1);
    }

    void createLinks()
    {
        QVERIFY(QDir().mkpath(L("lk")));
        writeFile(L("lk/target.txt"), "t");
        m_left->navigate(L("lk"));
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L("lk"));
        QTRY_COMPARE(rightModel()->path(), R());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "target.txt");
        answerNextDialog([](QWidget* w) {
            for (auto* b : w->findChildren<QToolButton*>())
                if (b->text() == QObject::tr("Relative"))
                    b->click();
            acceptDialog(w);
        });
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(QFileInfo(R("target.txt")).isSymLink());
        QCOMPARE(readFile(R("target.txt")), QByteArray("t"));
        QVERIFY(!QFileInfo(R("target.txt")).symLinkTarget().isEmpty());
        QTRY_VERIFY(rightModel()->rowOf("target.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!QFileInfo(R("target.txt")).exists() && !QFileInfo(R("target.txt")).isSymLink());
        QCOMPARE(readFile(L("lk/target.txt")), QByteArray("t"));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // On a server the link is made by a worker, never the UI thread.
    void createLinkOnServer()
    {
        QTemporaryDir serverDir;
        QVERIFY(QDir().mkpath(serverDir.filePath("src")));
        QVERIFY(QDir().mkpath(serverDir.filePath("dst")));
        writeFile(serverDir.filePath("src/target.txt"), "t");
        auto backend = std::make_shared<TestBackend>(serverDir.path());
        registerBackend("test", backend);
        auto unregister = qScopeGuard([] { registerBackend("test", nullptr); });
        m_left->navigate("test://srv/src");
        m_right->navigate("test://srv/dst");
        QTRY_COMPARE(leftModel()->path(), QString("test://srv/src"));
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/dst"));
        QTRY_VERIFY(leftModel()->rowOf("target.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "target.txt");
        backend->uiThreadCalls = 0;
        answerNextDialog([](QWidget* w) { acceptDialog(w); });
        QTest::keyClick(m_left->view(), Qt::Key_F5, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(QFileInfo(serverDir.filePath("dst/target.txt")).isSymLink());
        QCOMPARE(backend->uiThreadCalls.load(), 0);
        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_COMPARE(rightModel()->path(), R());
        QVERIFY(ioPool()->waitForDone(5000));
    }

    // keys.toml: every command listed; saving it changes keys at once, modal ones too.
    void keysToml()
    {
        Keymap& km = Keymap::instance();
        const QString path = km.path();
        QString original;
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            original = QString::fromUtf8(f.readAll());
        }
        QVERIFY2(original.contains("# copy = \"F5\"") && original.contains("# top = \"g g\""), qPrintable(original.left(400)));
        auto write = [&](const QString& text) {
            QSaveFile f(path);  // like an editor: a new file in its place
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(text.toUtf8());
            QVERIFY(f.commit());
        };
        auto restore = qScopeGuard([&] {
            write(original);
            QTRY_VERIFY(km.modalBindings().size() == Keymap::modalDefaults().size());
            QSettings().setValue("input/vimKeys", false);
            m_left->setVimKeys(false);
        });
        QAction* copy = nullptr;
        for (QAction* a : m_window->actions())
            if (a->property("commandId").toString() == "copy")
                copy = a;
        QVERIFY(copy);
        QString text = original;
        text.replace("# copy = \"F5\"", "copy = [\"F2\", \"Ctrl+F5\"]");
        text.replace("# down = \"j\"", "down = \"n\"");  // n was find-next
        write(text);
        QTRY_COMPARE(copy->shortcuts(), QList<QKeySequence>({QKeySequence(Qt::Key_F2), QKeySequence(Qt::CTRL | Qt::Key_F5)}));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_VERIFY(!m_left->tab()->isLoading());
        m_left->setVimKeys(true);
        m_left->view()->setFocus();
        moveCursorTo(m_left, "sub");
        const int row = m_left->view()->currentRow();
        QTest::keyClick(m_left->view(), Qt::Key_N);
        QCOMPARE(m_left->view()->currentRow(), row + 1);
        QTest::keyClick(m_left->view(), Qt::Key_J);  // no longer bound: nothing
        QCOMPARE(m_left->view()->currentRow(), row + 1);
        QTest::keyClicks(m_left->view(), "gg");  // sequences still work
        QCOMPARE(m_left->view()->currentRow(), 0);

        // A typo: the keys from before stay, and the message says where.
        write(text + "\nbroken = \n");
        QTRY_VERIFY(!km.error().isEmpty());
        QVERIFY2(km.error().contains("line"), qPrintable(km.error()));
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F2));
    }

    // Open with: a list of apps (o in modal mode, Shift+Enter otherwise); what
    // was typed can run as a command, after the apps that match.
    void openWithPicker()
    {
        DesktopApps::instance().loadAsync();
        QTRY_VERIFY(DesktopApps::instance().isLoaded());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        QTRY_VERIFY(!m_left->tab()->isLoading());
        m_left->view()->setFocus();
        moveCursorTo(m_left, "file1.txt");
        auto visiblePopup = [&]() -> FuzzyPopup* {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    return p;
            return nullptr;
        };
        QTest::keyClick(m_left->view(), Qt::Key_Return, Qt::ShiftModifier);
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY((popup = visiblePopup()));
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit && edit->placeholderText().contains("file1.txt"));
        QTest::keyClicks(edit, "zzqx-no-such-app");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->title.startsWith("Run"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_VERIFY(!visiblePopup());

        m_left->setVimKeys(true);
        auto off = qScopeGuard([this] { m_left->setVimKeys(false); });
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_O);
        QTRY_VERIFY((popup = visiblePopup()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_VERIFY(!visiblePopup());
    }

    // Ctrl+,: settings; keys can be changed, conflicts resolved, defaults restored.
    void settingsAndKeys()
    {
        SettingsDialog dlg(m_window.get());
        dlg.show();
        dlg.showPage(4);
        auto find = [&](const QString& text) -> QTreeWidgetItem* {
            for (int i = 0; i < dlg.keyList()->topLevelItemCount(); ++i)
                if (dlg.keyList()->topLevelItem(i)->text(0).endsWith(text))
                    return dlg.keyList()->topLevelItem(i);
            return nullptr;
        };
        QAction* copy = nullptr;
        QAction* move = nullptr;
        for (QAction* a : m_window->actions()) {
            if (a->property("commandId").toString() == "copy")
                copy = a;
            if (a->property("commandId").toString() == "move")
                move = a;
        }
        QVERIFY(copy && move);
        QCOMPARE(move->shortcut(), QKeySequence(Qt::Key_F6));
        dlg.keyList()->setCurrentItem(find("Copy…"));
        dlg.keyEdit()->setKeySequence(QKeySequence(Qt::Key_F6));  // Move's key
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F6));
        QVERIFY(move->shortcuts().isEmpty());  // gave it up
        QVERIFY(dlg.keyNote()->text().contains("Move"));
        QCOMPARE(Keymap::instance().commandKeys("copy"), std::optional<QStringList>(QStringList({"F6"})));
        QCOMPARE(Keymap::instance().commandKeys("move"), std::optional<QStringList>(QStringList()));
        // A restart applies them: simulate with the saved values.
        copy->setShortcut(QKeySequence(Qt::Key_F5));
        applyKeyBindings(m_window->actions());
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F6));
        for (auto* b : dlg.findChildren<QPushButton*>())
            if (b->text() == QObject::tr("Reset all keys"))
                b->click();
        QCOMPARE(copy->shortcut(), QKeySequence(Qt::Key_F5));
        QCOMPARE(move->shortcut(), QKeySequence(Qt::Key_F6));
        QVERIFY(!Keymap::instance().commandKeys("copy") && !Keymap::instance().commandKeys("move"));
    }

    // F1: grouped shortcuts in columns, with a filter.
    void shortcutsDialog()
    {
        ShortcutsDialog dlg(m_window.get());
        dlg.show();
        const int all = dlg.visibleEntries();
        QVERIFY(all > 40);
        dlg.filterEdit()->setText("rename");
        QVERIFY(dlg.visibleEntries() >= 2 && dlg.visibleEntries() < all);
        dlg.filterEdit()->setText("F5");
        QVERIFY(dlg.visibleEntries() >= 1);
        dlg.filterEdit()->setText("zzzz");
        QCOMPARE(dlg.visibleEntries(), 0);
    }

    // Help > About Tandem: the mascot, the version, the project's page.
    void aboutDialog()
    {
        bool listed = false;
        for (auto* a : m_window->findChildren<QAction*>())
            listed = listed || a->text() == QStringLiteral("About Tandem");
        QVERIFY(listed);
        const QString oldVersion = QApplication::applicationVersion();
        QApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        AboutDialog dlg(m_window.get());
        QApplication::setApplicationVersion(oldVersion);
        dlg.show();
        auto* mascot = dlg.findChild<QLabel*>(QStringLiteral("mascot"));
        QVERIFY(mascot);
        QVERIFY(!mascot->pixmap().isNull());
        QVERIFY(mascot->pixmap().width() > 100);
        bool version = false, link = false;
        for (auto* l : dlg.findChildren<QLabel*>()) {
            version = version || l->text().contains(QStringLiteral("9.8.7"));
            link = link || l->text().contains(QStringLiteral("github.com/carnager/tandem-fm"));
        }
        QVERIFY(version);
        QVERIFY(link);
    }

    // The keyboard always ends up back in a file list, and Esc closes things.
    void focusStaysWithTheLists()
    {
        QVERIFY(QDir().mkpath(L("fx")));
        for (const char* n : {"apple.txt", "banana.txt", "cherry.txt"})
            writeFile(L(QString("fx/") + n), n);
        m_left->navigate(L("fx"));
        QTRY_COMPARE(leftModel()->path(), L("fx"));
        m_right->navigate(R());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "apple.txt");
        auto focused = [] { return QApplication::focusWidget(); };

        // A click into a preview gives the keys back to that panel's list.
        QSettings().setValue("view/previewPosition", "each");
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* left = nullptr;
        ViewerWindow* right = nullptr;
        QTRY_VERIFY((left = m_window->findChild<ViewerWindow*>("quickView")) != nullptr);
        QTRY_VERIFY((right = m_window->findChild<ViewerWindow*>("quickViewRight")) != nullptr);
        QTRY_COMPARE(left->textEdit()->toPlainText(), QString("apple.txt"));
        QTest::mouseClick(left->textEdit()->viewport(), Qt::LeftButton, {}, QPoint(20, 10));
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(focused(), Qt::Key_Down);
        QCOMPARE(m_left->currentName(), QString("banana.txt"));
        QTest::mouseClick(right, Qt::LeftButton, {}, QPoint(30, 30));  // the other panel's preview
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_right->view()));
        QTRY_VERIFY(m_right->isActive());
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") == nullptr);
        QSettings().setValue("view/preview", false);
        m_left->view()->setFocus();
        QTRY_VERIFY(m_left->isActive());

        // Search bar: ↓ goes to the results, Esc there closes the bar.
        m_left->startSearchBar();
        auto* searchEdit = qobject_cast<QLineEdit*>(focused());
        QVERIFY(searchEdit && searchEdit->isVisible());
        QTest::keyClick(searchEdit, Qt::Key_Down);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QVERIFY(!searchEdit->isVisible());
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));

        // Filter: Enter back to the list, Esc there clears it.
        m_left->startFilter();
        QTest::keyClicks(focused(), "an");
        QTRY_VERIFY(leftModel()->rowOf("apple.txt") < 0);
        QTest::keyClick(focused(), Qt::Key_Return);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QTRY_VERIFY(leftModel()->rowOf("apple.txt") >= 0);
        QCOMPARE(focused(), static_cast<QWidget*>(m_left->view()));

        // Path bar and quick search: Esc returns to the list.
        m_left->focusPathEdit();
        QVERIFY(focused() != m_left->view());
        QTest::keyClick(focused(), Qt::Key_Escape);
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClicks(m_left->view(), "ch");
        QTRY_COMPARE(m_left->currentName(), QString("cherry.txt"));
        QTest::keyClick(focused(), Qt::Key_Escape);
        QTRY_COMPARE(focused(), static_cast<QWidget*>(m_left->view()));
        QTest::keyClick(focused(), Qt::Key_Up);
        QCOMPARE(m_left->currentName(), QString("banana.txt"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Alt+F1: places, drives, servers in a drop-down that also takes a typed path.
    void locations()
    {
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        m_left->showLocations();
        FuzzyPopup* popup = nullptr;
        QTRY_VERIFY([&] {
            for (auto* p : m_window->findChildren<FuzzyPopup*>())
                if (p->isVisible())
                    popup = p;
            return popup != nullptr;
        }());
        QCOMPARE(popup->currentItem()->title, QString("Home"));
        auto* edit = popup->findChild<QLineEdit*>();
        QTest::keyClicks(edit, R());
        QTRY_COMPARE(popup->currentItem()->title, QString("Go to %1").arg(R()));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), R());
        QVERIFY(!popup->isVisible());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // F3 as a quick view: follows the panel (keys and clicks), N / P move the
    // panel's cursor, "same type only" keeps to pictures.
    void viewerFollowsPanel()
    {
        QVERIFY(QDir().mkpath(L("follow/sub")));
        QImage img(16, 16, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(L("follow/a.jpg"), "JPG"));
        writeFile(L("follow/b.txt"), "bee");
        QVERIFY(img.save(L("follow/c.png"), "PNG"));
        writeFile(L("follow/d.txt"), "dee");
        m_left->navigate(L("follow"));
        QTRY_COMPARE(leftModel()->path(), L("follow"));
        QTRY_VERIFY(!m_left->tab()->isLoading());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "a.jpg");
        QTest::keyClick(m_left->view(), Qt::Key_F3);
        auto viewers = [&] {
            QList<ViewerWindow*> out;
            for (ViewerWindow* w : m_window->findChildren<ViewerWindow*>())
                if (w->isWindow())
                    out.append(w);
            return out;
        };
        QTRY_COMPARE(viewers().size(), 1);
        ViewerWindow* v = viewers().first();
        v->setSameKindOnly(false);
        QTRY_COMPARE(v->currentPath(), L("follow/a.jpg"));

        // The cursor moves in Tandem: the viewer shows that file.
        moveCursorTo(m_left, "b.txt");
        QTRY_COMPARE(v->currentPath(), L("follow/b.txt"));
        QTRY_COMPARE(v->textEdit()->toPlainText(), QString("bee"));
        // A click on a file, too; a folder keeps the last file.
        FileView* view = m_left->view();
        const QRect rect = view->visualRect(leftModel()->index(leftModel()->rowOf("c.png"), 0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, rect.center());
        QTRY_COMPARE(v->currentPath(), L("follow/c.png"));
        moveCursorTo(m_left, "sub");
        QTest::qWait(300);
        QCOMPARE(v->currentPath(), L("follow/c.png"));

        // N / P step from the panel's cursor and move it.
        moveCursorTo(m_left, "c.png");
        v->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(v));
        QTest::keyClick(v, Qt::Key_N);
        QTRY_COMPARE(v->currentPath(), L("follow/d.txt"));
        QCOMPARE(m_left->currentPath(), L("follow/d.txt"));
        QTest::keyClick(v, Qt::Key_P);
        QTRY_COMPARE(v->currentPath(), L("follow/c.png"));
        QCOMPARE(m_left->currentPath(), L("follow/c.png"));

        // The panel's keys work in the viewer: ↓ / ↑ (also with a text file
        // focused), and j / k with modal keys.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
        QTRY_COMPARE(v->currentPath(), L("follow/d.txt"));
        QTRY_COMPARE(v->textEdit()->toPlainText(), QString("dee"));
        QVERIFY(v->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up);
        QTRY_COMPARE(m_left->currentPath(), L("follow/c.png"));
        QTRY_COMPARE(v->currentPath(), L("follow/c.png"));
        m_left->view()->setVimKeys(true);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_J);
        QTRY_COMPARE(v->currentPath(), L("follow/d.txt"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_K);
        QTRY_COMPARE(v->currentPath(), L("follow/c.png"));
        m_left->view()->setVimKeys(false);

        // Same type only: from c.png back to a.jpg, past b.txt; nothing after c.png.
        QTest::keyClick(v, Qt::Key_T);
        QVERIFY(v->sameKindOnly());
        QTest::keyClick(v, Qt::Key_P);
        QTRY_COMPARE(v->currentPath(), L("follow/a.jpg"));
        QCOMPARE(m_left->currentPath(), L("follow/a.jpg"));
        QTest::keyClick(v, Qt::Key_N);
        QTRY_COMPARE(v->currentPath(), L("follow/c.png"));
        QTest::keyClick(v, Qt::Key_N);
        QTest::qWait(200);
        QCOMPARE(v->currentPath(), L("follow/c.png"));
        QCOMPARE(ViewerWindow::kindOf("x.webp"), QString("image"));
        QCOMPARE(ViewerWindow::kindOf("x.pdf"), QString("application/pdf"));

        // Typing in the find bar is typing, not stepping.
        moveCursorTo(m_left, "b.txt");
        QTRY_COMPARE(v->currentPath(), L("follow/b.txt"));
        QTest::keyClick(v, Qt::Key_F7);
        QLineEdit* find = v->findChild<QLineEdit*>();
        QTRY_VERIFY(find && find->hasFocus());
        QTest::keyClicks(find, "a b");
        QTest::keyClick(find, Qt::Key_Backspace);
        QCOMPARE(find->text(), QString("a "));
        QCOMPARE(v->currentPath(), L("follow/b.txt"));
        v->setSameKindOnly(false);

        // F3 again reuses the window.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "d.txt");
        QTest::keyClick(m_left->view(), Qt::Key_F3);
        QTRY_COMPARE(v->currentPath(), L("follow/d.txt"));
        QCOMPARE(viewers().size(), 1);
        v->close();
        QTRY_COMPARE(viewers().size(), 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->view()->setFocus();
    }

    void quickView()
    {
        QVERIFY(QDir().mkpath(L("qv")));
        writeFile(L("qv/a.txt"), "first file");
        writeFile(L("qv/b.txt"), "second file");
        m_left->navigate(L("qv"));
        QTRY_COMPARE(leftModel()->path(), L("qv"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "a.txt");
        // Settings live in ~/.qttest in test mode and survive runs: be explicit.
        QSettings().setValue("view/previewPosition", "each");

        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* qv = nullptr;
        QTRY_VERIFY((qv = m_window->findChild<ViewerWindow*>("quickView")) != nullptr);
        QVERIFY(m_right->isVisible());  // the copy/move target stays
        // Default: one preview below each panel, in that panel's column.
        ViewerWindow* qv2 = m_window->findChild<ViewerWindow*>("quickViewRight");
        QVERIFY(qv2);
        QVERIFY(qv->parentWidget() == m_left->parentWidget());
        QVERIFY(qv2->parentWidget() == m_right->parentWidget());
        QVERIFY(qv->mapTo(m_window.get(), QPoint()).y() > m_left->mapTo(m_window.get(), QPoint()).y());
        QTRY_COMPARE(qv->textEdit()->toPlainText(), QString("first file"));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));  // focus stays

        QTest::keyClick(m_left->view(), Qt::Key_Down);
        QTRY_COMPARE(qv->currentPath(), L("qv/b.txt"));
        QTRY_COMPARE(qv->textEdit()->toPlainText(), QString("second file"));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));

        // The right preview shows the right panel's cursor; the left one stays.
        m_right->navigate(L("qv"));
        QTRY_COMPARE(rightModel()->path(), L("qv"));
        moveCursorTo(m_right, "a.txt");
        QTRY_COMPARE(qv2->currentPath(), L("qv/a.txt"));
        QCOMPARE(qv->currentPath(), L("qv/b.txt"));
        m_right->navigate(R());

        // One shared preview below both: follows the active panel.
        m_window->setPreviewPosition("bottom");
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickViewRight") == nullptr);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") != nullptr);
        auto* shared = m_window->findChild<ViewerWindow*>("quickView");
        QCOMPARE(qobject_cast<QSplitter*>(shared->parentWidget())->orientation(), Qt::Vertical);
        QVERIFY(shared->parentWidget() != m_left->parentWidget());

        // On the right: a third column.
        m_window->setPreviewPosition("right");
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") &&
                    qobject_cast<QSplitter*>(m_window->findChild<ViewerWindow*>("quickView")->parentWidget())
                            ->orientation() == Qt::Horizontal);
        QSettings().setValue("view/previewPosition", "each");

        // Photos with nearly the same names: each shows its own picture.
        QVERIFY(QDir().mkpath(L("pics")));
        const QList<QColor> colors = {Qt::red, Qt::green, Qt::blue, Qt::yellow, Qt::magenta};
        for (int i = 0; i < colors.size(); ++i) {
            QImage img(64, 48, QImage::Format_RGB32);
            img.fill(colors[i]);
            QVERIFY(img.save(L(QString("pics/IMG_2024_000%1.jpg").arg(i)), "JPG"));
        }
        m_left->navigate(L("pics"));
        QTRY_COMPARE(leftModel()->path(), L("pics"));
        qv = m_window->findChild<ViewerWindow*>("quickView");
        moveCursorTo(m_left, "IMG_2024_0000.jpg");
        auto shown = [&] {  // what is drawn, not just what was loaded
            QWidget* vp = qv->imageView()->viewport();
            const QImage im = vp->grab().toImage();
            return im.isNull() ? QColor() : im.pixelColor(im.width() / 2, im.height() / 2);
        };
        auto near = [](QColor a, QColor b) {
            return a.isValid() && qAbs(a.red() - b.red()) < 40 && qAbs(a.green() - b.green()) < 40 && qAbs(a.blue() - b.blue()) < 40;
        };
        for (int i = 0; i < colors.size(); ++i) {
            QTRY_COMPARE(qv->currentPath(), L(QString("pics/IMG_2024_000%1.jpg").arg(i)));
            QTRY_VERIFY2(near(shown(), colors[i]), qPrintable(QString("file %1 shows %2").arg(i).arg(shown().name())));
            QTest::keyClick(m_left->view(), Qt::Key_Down);
        }
        QTest::keyClick(m_left->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickView") == nullptr);
        QSettings().setValue("view/preview", false);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void pathSegments()
    {
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        auto* bar = m_left->findChild<PathBar*>();
        QVERIFY(bar);
        auto segment = [bar](const QString& path) -> QToolButton* {
            for (auto* b : bar->findChildren<QToolButton*>("crumb"))
                if (b->property("path").toString() == path)
                    return b;
            return nullptr;
        };
        QTRY_VERIFY(segment(L("sub")) != nullptr);
        QTRY_VERIFY(segment(L("sub"))->isVisible());
        segment(L("sub"))->click();
        QTRY_COMPARE(leftModel()->path(), L("sub"));
        QCOMPARE(m_left->currentName(), QString("deep"));  // lands where it came from

        // Middle click opens a tab in the background.
        QTRY_VERIFY(segment(L()) != nullptr);
        QTest::mouseClick(segment(L()), Qt::MiddleButton);
        QTRY_COMPARE(m_left->tabPaths(), QStringList({L("sub"), L()}));
        QCOMPARE(m_left->path(), L("sub"));
        m_left->cycleTab(1);
        m_left->closeCurrentTab();

        // Ctrl+L still takes a typed path (relative to the current folder).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "deep");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void vimKeys()
    {
        QVERIFY(QDir().mkpath(L("vim/inner")));
        for (const char* n : {"a1.txt", "b2.txt", "c3.txt", "d4.txt"})
            writeFile(L(QString("vim/") + n), n);
        m_left->navigate(L("vim"));
        QTRY_COMPARE(leftModel()->path(), L("vim"));
        QAction* vim = nullptr;
        for (QAction* a : m_window->actions())
            if (a->property("commandId").toString() == "modal-keys-vim-yazi")
                vim = a;
        QVERIFY(vim);
        vim->trigger();
        QVERIFY(vim->isChecked());
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        FileView* v = m_left->view();
        v->setFocus();
        v->setCurrentRow(0);

        QTest::keyClicks(v, "jj");
        QCOMPARE(m_left->currentName(), QString("a1.txt"));  // .., inner, a1
        QTest::keyClicks(v, "k");
        QCOMPARE(m_left->currentName(), QString("inner"));
        QTest::keyClicks(v, "G");
        QCOMPARE(m_left->currentName(), QString("d4.txt"));
        QTest::keyClicks(v, "gg");
        QCOMPARE(m_left->currentName(), QString(".."));
        QTest::keyClicks(v, "u");  // letters don't start quick search in modal mode
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(v));

        // / search, Enter, then n jumps to the next match.
        QTest::keyClicks(v, "/");
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "txt");
        QTRY_COMPARE(m_left->currentName(), QString("a1.txt"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(v));
        QTest::keyClicks(v, "n");
        QCOMPARE(m_left->currentName(), QString("b2.txt"));
        QTest::keyClicks(v, "N");
        QCOMPARE(m_left->currentName(), QString("a1.txt"));

        // v: visual mode marks what the cursor moves over; Esc ends it, the marks stay.
        QTest::keyClicks(v, "vjj");
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt", "b2.txt", "c3.txt"}));
        QCOMPARE(v->visualMode(), 1);
        QTest::keyClicks(v, "k");  // moving back takes marks back
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt", "b2.txt"}));
        m_left->refresh();  // a refresh of the folder keeps visual mode and its range
        QTRY_VERIFY(!m_left->tab()->isLoading());
        QCOMPARE(v->visualMode(), 1);
        QTest::keyClicks(v, "j");
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt", "b2.txt", "c3.txt"}));
        QTest::keyClicks(v, "k");
        QTest::keyClick(v, Qt::Key_Escape);
        QCOMPARE(v->visualMode(), 0);
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt", "b2.txt"}));
        QTest::keyClicks(v, "V");  // V unmarks, from the cursor on
        QCOMPARE(leftModel()->markedNames(), QStringList({"a1.txt"}));
        QTest::keyClick(v, Qt::Key_Escape);

        // y copies to the clipboard, p pastes it into the shown folder.
        moveCursorTo(m_left, "a1.txt");
        QTest::keyClicks(v, "y");
        leftModel()->markAll(false);
        moveCursorTo(m_left, "inner");
        QTest::keyClicks(v, "l");
        QTRY_COMPARE(leftModel()->path(), L("vim/inner"));
        QTest::keyClicks(v, "p");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(L("vim/inner/a1.txt")), 5000);
        QTest::keyClicks(v, "h");
        QTRY_COMPARE(leftModel()->path(), L("vim"));

        // , then a key sorts; t and q open and close a tab.
        QTest::keyClicks(v, ",s");
        QCOMPARE(m_left->sortSpec().column, SortColumn::Size);
        QTest::keyClicks(v, ",n");
        QCOMPARE(m_left->sortSpec().column, SortColumn::Name);
        const int tabs = m_left->tabPaths().size();
        QTest::keyClicks(v, "t");
        QTRY_COMPARE(m_left->tabPaths().size(), tabs + 1);
        QTest::keyClicks(m_left->view(), "q");
        QTRY_COMPARE(m_left->tabPaths().size(), tabs);
        v = m_left->view();

        // l enters, h leaves.
        moveCursorTo(m_left, "inner");
        QTest::keyClicks(v, "l");
        QTRY_COMPARE(leftModel()->path(), L("vim/inner"));
        QTest::keyClicks(v, "h");
        QTRY_COMPARE(leftModel()->path(), L("vim"));

        vim->trigger();
        QVERIFY(!vim->isChecked());
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void gitBadges()
    {
        // This repository itself: the panel shows the branch and model statuses.
        const QString repo = QStringLiteral(TANDEM_SOURCE_DIR);
        if (!QFileInfo::exists(repo + "/.git") || QStandardPaths::findExecutable("git").isEmpty())
            QSKIP("no git checkout");
        m_right->navigate(repo);
        QTRY_COMPARE(rightModel()->path(), repo);
        QTRY_VERIFY(!m_right->tab()->gitBranch().isEmpty());
        QTRY_COMPARE(rightModel()->gitStatus("build"), '!');  // ignored by .gitignore
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
    }

    void bookmarkChords()
    {
        FolderHistory& h = FolderHistory::instance();
        QTRY_VERIFY(h.isLoaded());
        h.setBookmark(L("sub"), "cp");
        h.setBookmark(L("sub/deep"), "cd");
        h.setBookmark(R(), "c");  // also a prefix of the two above
        auto* chords = m_window->findChild<KeyChordPopup*>();
        QVERIFY(chords);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        // A chain: "c" waits (cp, cd and c exist), "p" jumps.
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "c");
        QVERIFY(chords->isVisible());
        QTest::keyClicks(chords, "p");
        QTRY_VERIFY(!chords->isVisible());
        QTRY_COMPARE(leftModel()->path(), L("sub"));

        // Enter takes the exact key even though longer ones start with it.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "c");
        QTest::keyClick(chords, Qt::Key_Return);
        QTRY_COMPARE(leftModel()->path(), R());

        // Assign a new chain to the current folder.
        m_left->navigate(L("sub/deep"));
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(chords->isVisible());
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(chords, Qt::Key_Backspace);  // clear the existing "cd"
        QTest::keyClicks(chords, "zz");
        QTest::keyClick(chords, Qt::Key_Return);
        QTRY_COMPARE(h.pathForKey("zz"), L("sub/deep"));

        // Vim: g + key.
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
        m_left->setVimKeys(true);
        m_left->view()->setFocus();
        QTest::keyClicks(m_left->view(), "gz");
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "z");
        QTRY_COMPARE(leftModel()->path(), L("sub/deep"));
        m_left->setVimKeys(false);

        // Regression: with the right panel focused, the jump goes to the right panel.
        h.setBookmark(L("sub"), "q");
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        const QString leftBefore = leftModel()->path();
        QTest::keyClick(m_right->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QTest::keyClicks(chords, "q");
        QTRY_COMPARE(rightModel()->path(), L("sub"));
        QCOMPARE(leftModel()->path(), leftBefore);
        QVERIFY(m_right->isActive());
        // …and the same for the fuzzy jump.
        auto* popup = m_window->findChild<FuzzyPopup*>();
        QTest::keyClick(m_right->view(), Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(popup->isVisible());
        QTest::keyClicks(QApplication::focusWidget(), "sub dee");
        QTRY_VERIFY(popup->currentItem() && popup->currentItem()->key == L("sub/deep"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), L("sub/deep"));
        QCOMPARE(leftModel()->path(), leftBefore);
        h.setPinned(L("sub"), false);
        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        m_left->view()->setFocus();

        // Regression: with more bookmarks than fit one row, every row gets
        // its full height (rows were squeezed and the text cut off).
        h.setBookmark(L("sub"), "a");
        h.setBookmark(L(), "b");
        QTest::keyClick(m_left->view(), Qt::Key_D, Qt::ControlModifier);
        QTRY_VERIFY(chords->isVisible());
        QApplication::processEvents();
        for (QLabel* label : chords->findChildren<QLabel*>()) {
            if (!label->isVisible())
                continue;
            QVERIFY2(label->height() >= label->sizeHint().height(), qPrintable(label->text()));
            QVERIFY(chords->rect().contains(label->mapTo(chords, label->rect().bottomRight())));
        }
        QTest::keyClick(chords, Qt::Key_Escape);
        QTRY_VERIFY(!chords->isVisible());
        h.setPinned(L(), false);

        for (const QString& p : {L("sub"), L("sub/deep"), R()})
            h.setPinned(p, false);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void searchAsFolder()
    {
        QVERIFY(QDir().mkpath(L("srch/a/b")));
        writeFile(L("srch/a/report.txt"), "quarterly numbers");
        writeFile(L("srch/a/b/report-old.txt"), "old");
        writeFile(L("srch/other.md"), "numbers here too");
        m_left->navigate(L("srch"));
        QTRY_COMPARE(leftModel()->path(), L("srch"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        DirTab* folderTab = m_left->tab();

        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "report");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(m_left->tab()->isSearch());
        DirTab* resultsTab = m_left->tab();
        QVERIFY(resultsTab != folderTab);  // results in a tab of their own
        QTRY_VERIFY(!m_left->tab()->isSearching());
        QTRY_VERIFY(leftModel()->rowOf("a/report.txt") >= 0);
        QVERIFY(leftModel()->rowOf("a/b/report-old.txt") >= 0);
        QCOMPARE(leftModel()->rowOf("other.md"), -1);

        // Results behave like files: the full path is right, delete works.
        moveCursorTo(m_left, "a/b/report-old.txt");
        QCOMPARE(m_left->currentPath(), L("srch/a/b/report-old.txt"));
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F8, Qt::ShiftModifier);
        QTRY_VERIFY(!QFile::exists(L("srch/a/b/report-old.txt")));
        QTRY_COMPARE(leftModel()->rowOf("a/b/report-old.txt"), -1);

        // Contents search (Tab flips the mode).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_F7, Qt::AltModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(QApplication::focusWidget(), "numbers");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(!m_left->tab()->isSearching());
        QTRY_COMPARE(m_left->tab()->searchFound(), 2);
        QCOMPARE(m_left->tab(), resultsTab);  // searching again reuses it
        QVERIFY(leftModel()->rowOf("other.md") >= 0);

        // Esc leaves the results, back in the folder searched.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Escape);
        QTRY_COMPARE(m_left->tab(), folderTab);
        QCOMPARE(leftModel()->path(), L("srch"));
        QVERIFY(leftModel()->rowOf("a") >= 0);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Ctrl+Shift+F: the filter through all subfolders, live; Alt+S takes the
    // words to this folder only and back; marks stay while the words change.
    void deepFilter()
    {
        QVERIFY(QDir().mkpath(L("deep/photos/2024")));
        writeFile(L("deep/photos/2024/beach.raw"), "x");
        writeFile(L("deep/photos/2024/beach.jpg"), "x");
        writeFile(L("deep/notes.txt"), "x");
        m_left->navigate(L("deep"));
        QTRY_COMPARE(leftModel()->path(), L("deep"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        DirTab* folderTab = m_left->tab();
        const QStringList tabsBefore = m_left->tabPaths();
        QTest::keyClick(m_left->view(), Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(m_left->tab()->isLive());
        // The results get a tab of their own, left out of the saved tabs.
        QVERIFY(m_left->tab() != folderTab);
        QCOMPARE(folderTab->model()->path(), L("deep"));
        QCOMPARE(m_left->tabPaths(), tabsBefore);
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        QTest::keyClicks(edit, "ph 24 raw");
        QTRY_COMPARE(leftModel()->rowOf("photos/2024/beach.raw") >= 0, true);
        QCOMPARE(leftModel()->rowOf("photos/2024/beach.jpg"), -1);
        QTRY_COMPARE(m_left->tab()->searchFound(), 1);

        // Into the list, mark it, change the words: the mark stays in view.
        QTest::keyClick(edit, Qt::Key_Down);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(m_left->view()));
        moveCursorTo(m_left, "photos/2024/beach.raw");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QTest::keyClick(m_left->view(), Qt::Key_Escape);  // back to the words
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(edit));
        edit->setText("notes");
        QTRY_VERIFY(leftModel()->rowOf("notes.txt") >= 0);
        QVERIFY(leftModel()->rowOf("photos/2024/beach.raw") >= 0);
        QCOMPARE(leftModel()->markedNames(), QStringList({"photos/2024/beach.raw"}));

        // Alt+S: this folder only, same words; and back.
        QTest::keyClick(edit, Qt::Key_S, Qt::AltModifier);
        QTRY_COMPARE(m_left->tab(), folderTab);
        QTRY_COMPARE(leftModel()->filter(), QString("notes"));
        QCOMPARE(leftModel()->rowOf("photos"), -1);
        QTest::keyClick(edit, Qt::Key_S, Qt::AltModifier);
        QTRY_VERIFY(m_left->tab()->isLive());
        QTRY_VERIFY(leftModel()->rowOf("notes.txt") >= 0);

        QVERIFY(m_left->tab() != folderTab);
        QVERIFY(folderTab->model()->filter().isEmpty());  // the words went along

        // Esc in the words closes the results, back in the folder's tab.
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_COMPARE(m_left->tab(), folderTab);
        QCOMPARE(m_left->tabPaths(), tabsBefore);
        QTRY_COMPARE(leftModel()->path(), L("deep"));
        QVERIFY(leftModel()->filter().isEmpty());
        QVERIFY(leftModel()->rowOf("photos") >= 0);

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    // Shelves: Ctrl+B and a digit puts the marked files on one (b1 with
    // modal keys); a chip shows it; Ctrl+Shift+B copies it here; the chip
    // opens it: Del takes one off, Ctrl+Del clears it.
    void shelves()
    {
        auto cleanup = qScopeGuard([this] {
            for (int n = 1; n <= Shelves::kCount; ++n)
                Shelves::instance().clear(n);
            m_left->setVimKeys(false);
        });
        auto* chords = m_window->findChild<KeyChordPopup*>();
        QVERIFY(chords);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QVERIFY(QDir().mkpath(L("shelf-src")));
        writeFile(L("shelf-src/one.txt"), "1");
        writeFile(L("shelf-src/two.txt"), "2");
        writeFile(L("shelf-src/zz-pic.jpg"), "p");
        m_left->navigate(L("shelf-src"));
        QTRY_COMPARE(leftModel()->path(), L("shelf-src"));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "one.txt");
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        QTest::keyClick(m_left->view(), Qt::Key_Insert);
        // Ctrl+B shows the shelves by number, like the bookmark keys; the digit puts them there.
        QTest::keyClick(m_left->view(), Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(chords->isChoosing());
        QCOMPARE(QApplication::focusWidget(), chords);
        QTest::keyClick(chords, Qt::Key_3);
        QTRY_COMPARE(Shelves::instance().items(3), QStringList({L("shelf-src/one.txt"), L("shelf-src/two.txt")}));
        QVERIFY(leftModel()->markedNames().isEmpty());  // ready for the next ones
        QVERIFY(!chords->isVisible());
        QVERIFY(m_left->view()->hasFocus());
        QTRY_VERIFY(m_window->findChild<QToolButton*>("shelf-3"));

        // Only four shelves: 5 does nothing, Esc closes.
        moveCursorTo(m_left, "zz-pic.jpg");
        QTest::keyClick(m_left->view(), Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_5);
        QVERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_Escape);
        QVERIFY(!chords->isVisible());
        QCOMPARE(Shelves::instance().used(), QList<int>({3}));

        // Modal keys: b opens the same picker, then the shelf's number.
        m_left->setVimKeys(true);
        moveCursorTo(m_left, "zz-pic.jpg");
        QTest::keyClick(m_left->view(), Qt::Key_B);
        QTRY_VERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_1);
        QTRY_COMPARE(Shelves::instance().items(1), QStringList({L("shelf-src/zz-pic.jpg")}));
        m_left->setVimKeys(false);

        // Copy shelf 3 into another folder: Ctrl+Shift+B, then its digit.
        // An empty shelf can't be picked; the picker stays.
        QVERIFY(QDir().mkpath(L("shelved")));
        m_left->navigate(L("shelved"));
        QTRY_COMPARE(leftModel()->path(), L("shelved"));
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_2);
        QVERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_3);
        QVERIFY(!chords->isVisible());
        QVERIFY(m_left->view()->hasFocus());
        QTRY_VERIFY(QFile::exists(L("shelved/one.txt")));
        QTRY_VERIFY(QFile::exists(L("shelved/two.txt")));
        QVERIFY(QFile::exists(L("shelf-src/one.txt")));  // copied, not moved
        QVERIFY(Shelves::instance().items(3).isEmpty());  // like a clipboard: pasted, gone
        QTRY_VERIFY(!m_window->findChild<QToolButton*>("shelf-3"));
        Shelves::instance().add(3, {L("shelf-src/one.txt"), L("shelf-src/two.txt")});
        Shelves::instance().add(1, {});  // shelf 1 stays the one used last

        // Space looks inside: the shelf panel, on the last used shelf (1), with the keys.
        auto* panel = m_window->findChild<ShelfPanel*>();
        QTest::keyClick(m_left->view(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_Space);
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(panel->shelf(), 1);
        QTRY_VERIFY(panel->list()->hasFocus());
        QTest::keyClick(panel->list(), Qt::Key_3);
        QCOMPARE(panel->shelf(), 3);
        QTest::keyClick(panel->list(), Qt::Key_Escape);
        QTRY_VERIFY(!panel->isVisible());

        // Look in it: the chip opens the shelf in the corner, where it stays
        // while the panels have the keys. Files dropped on it go on the
        // shelf; Del takes one off (the file stays); Clear empties it.
        m_window->findChild<QToolButton*>("shelf-3")->click();
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(panel->list()->count(), 2);
        m_left->view()->setFocus();
        QTest::qWait(50);
        QVERIFY(panel->isVisible());

        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(L("shelf-src/zz-pic.jpg"))});
        QDragEnterEvent enter(QPoint(20, 10), Qt::CopyAction | Qt::LinkAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel->list()->viewport(), &enter);
        QDropEvent drop(QPointF(20, 10), Qt::CopyAction | Qt::LinkAction, &data, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel->list()->viewport(), &drop);
        QTRY_COMPARE(Shelves::instance().items(3).size(), 3);
        QTRY_COMPARE(panel->list()->count(), 3);

        panel->list()->setFocus();
        panel->list()->setCurrentRow(0);
        QTest::keyClick(panel->list(), Qt::Key_Delete);
        QTRY_COMPARE(Shelves::instance().items(3), QStringList({L("shelf-src/two.txt"), L("shelf-src/zz-pic.jpg")}));
        QVERIFY(QFile::exists(L("shelf-src/one.txt")));
        panel->findChild<QToolButton*>("shelfClear")->click();
        QTRY_VERIFY(Shelves::instance().items(3).isEmpty());
        QTRY_VERIFY(!panel->isVisible());
        QTRY_VERIFY(!m_window->findChild<QToolButton*>("shelf-3"));

        // Shift with the digit moves; that empties the shelf too.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(chords->isChoosing());
        QTest::keyClick(chords, Qt::Key_1, Qt::ShiftModifier);
        QTRY_VERIFY(QFile::exists(L("shelved/zz-pic.jpg")));
        QTRY_VERIFY(!QFile::exists(L("shelf-src/zz-pic.jpg")));
        QVERIFY(Shelves::instance().items(1).isEmpty());

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void runCommandOnSelection()
    {
        CommandContext ctx;
        ctx.selection = {"/a b/it's.txt", "/c"};
        ctx.current = "/a b/it's.txt";
        ctx.dir = "/a b";
        ctx.otherDir = "/x";
        QCOMPARE(expandCommand("cp %s %D", ctx), QString(R"(cp '/a b/it'\''s.txt' '/c' '/x')"));
        QCOMPARE(expandCommand("echo %n in %d, 100%%", ctx), QString(R"(echo 'it'\''s.txt' in '/a b', 100%)"));
        QCOMPARE(expandCommand("ls", ctx), QString("ls"));

        QVERIFY(QDir().mkpath(L("cmd")));
        QVERIFY(QDir().mkpath(R("cmd-target")));
        writeFile(L("cmd/one file.txt"), "1");
        writeFile(L("cmd/it's.txt"), "2");
        m_left->navigate(L("cmd"));
        m_right->navigate(R("cmd-target"));
        QTRY_COMPARE(leftModel()->path(), L("cmd"));
        QTRY_COMPARE(rightModel()->path(), R("cmd-target"));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        leftModel()->markAll(true);

        QTest::keyClick(m_left->view(), Qt::Key_Semicolon, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "cp %s %D");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(R("cmd-target/one file.txt")));
        QTRY_VERIFY(QFile::exists(R("cmd-target/it's.txt")));
        QTRY_VERIFY(rightModel()->rowOf("it's.txt") >= 0);  // panels update by themselves
        leftModel()->markAll(false);

        // Output goes to a window.
        m_left->view()->setFocus();
        QTest::keyClick(m_left->view(), Qt::Key_Semicolon, Qt::ControlModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        QTest::keyClicks(QApplication::focusWidget(), "echo hello from %d");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QPlainTextEdit* out = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* t = w->findChild<QPlainTextEdit*>(); t && w->isVisible() && t->toPlainText().startsWith("hello"))
                    out = t;
            return out != nullptr;
        }());
        QCOMPARE(out->toPlainText().trimmed(), QString("hello from ") + L("cmd"));
        out->window()->close();

        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void compareAndSync()
    {
        auto at = [](const QString& p, qint64 secs) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
        };
        QVERIFY(QDir().mkpath(L("cmp/sub")));
        QVERIFY(QDir().mkpath(R("cmp")));
        writeFile(L("cmp/same.txt"), "same");    writeFile(R("cmp/same.txt"), "same");
        writeFile(L("cmp/newer.txt"), "new!");   writeFile(R("cmp/newer.txt"), "old");
        writeFile(L("cmp/sub/deep.txt"), "deep");
        writeFile(R("cmp/rightonly.txt"), "r");
        at(L("cmp/same.txt"), 4000);  at(R("cmp/same.txt"), 4000);
        at(L("cmp/newer.txt"), 9000); at(R("cmp/newer.txt"), 3000);
        m_left->navigate(L("cmp"));
        m_right->navigate(R("cmp"));
        QTRY_COMPARE(leftModel()->path(), L("cmp"));
        QTRY_COMPARE(rightModel()->path(), R("cmp"));
        QTRY_VERIFY(leftModel()->rowOf("newer.txt") >= 0 && rightModel()->rowOf("rightonly.txt") >= 0);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();

        // Shift+F2 marks what is newer or missing on the other side.
        QTest::keyClick(m_left->view(), Qt::Key_F2, Qt::ShiftModifier);
        QCOMPARE(QSet<QString>(leftModel()->markedSet()), QSet<QString>({"newer.txt", "sub"}));
        QCOMPARE(QSet<QString>(rightModel()->markedSet()), QSet<QString>({"rightonly.txt"}));
        leftModel()->markAll(false);
        rightModel()->markAll(false);

        // Ctrl+Shift+S: the sync dialog.
        QTest::keyClick(m_left->view(), Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
        SyncDialog* dlg = nullptr;
        QTRY_VERIFY((dlg = m_window->findChild<SyncDialog*>()) != nullptr);
        QTRY_VERIFY(!dlg->isComparing());
        SyncModel* m = dlg->model();
        QCOMPARE(m->rowCount(), 3);  // equal files hidden by default
        QCOMPARE(m->action(m->rowOf("newer.txt")), SyncAction::ToRight);
        QCOMPARE(m->action(m->rowOf("sub")), SyncAction::ToRight);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);

        // Keys change directions: Space leaves "rightonly.txt" alone.
        dlg->view()->setCurrentIndex(m->index(m->rowOf("rightonly.txt"), 0));
        QTest::keyClick(dlg->view(), Qt::Key_Space);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::None);
        QTest::keyClick(dlg->view(), Qt::Key_Left);
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);
        QTest::keyClick(dlg->view(), Qt::Key_Right);  // impossible: nothing on the left
        QCOMPARE(m->action(m->rowOf("rightonly.txt")), SyncAction::ToLeft);

        QTest::keyClick(dlg->view(), Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(readFile(R("cmp/newer.txt")), QByteArray("new!"));
        QTRY_COMPARE(readFile(R("cmp/sub/deep.txt")), QByteArray("deep"));
        QTRY_COMPARE(readFile(L("cmp/rightonly.txt")), QByteArray("r"));
        // It compares again afterwards: nothing left to do.
        QTRY_VERIFY(!dlg->isComparing() && m->rowCount() == 0);
        QTRY_VERIFY(rightModel()->rowOf("sub") >= 0);

        // Mirror right → left removes what the right side doesn't have.
        writeFile(L("cmp/extra.txt"), "x");
        dlg->setMode(SyncMode::MirrorToLeft);
        dlg->compare();
        QTRY_VERIFY(!dlg->isComparing() && m->rowOf("extra.txt") >= 0);
        QCOMPARE(m->action(m->rowOf("extra.txt")), SyncAction::DeleteLeft);
        answerNextDialog(acceptDialog);
        dlg->synchronize();
        QTRY_VERIFY(!QFile::exists(L("cmp/extra.txt")));
        dlg->setMode(SyncMode::TwoWay);
        dlg->close();

        m_left->navigate(L());
        m_right->navigate(R());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void networkMount()
    {
        if (!Network::available() || QStandardPaths::findExecutable("gvfsd-localtest", {"/usr/lib", "/usr/libexec", "/usr/lib/gvfs"}).isEmpty())
            QSKIP("GVfs localtest backend not available");
        QVERIFY(QDir().mkpath(L("net")));
        writeFile(R("tocopy.txt"), "over gvfs");
        m_right->navigate(R());
        QTRY_VERIFY(rightModel()->rowOf("tocopy.txt") >= 0);

        // What the path bar does with "sftp://…": mount through GVfs, then open.
        m_left->connectTo("localtest://" + L("net"));
        const QString viaGvfs = gvfsDirectory() + "/localtest:" + L("net");
        if (!QTest::qWaitFor([&] { return m_left->path() == viaGvfs; }, 15000))
            QSKIP("localtest mount failed in this session");
        QVERIFY(isRemotePath(m_left->path()));
        QVERIFY(Network::instance().canUnmount(m_left->path()));
        QVERIFY(Network::instance().recentServers().contains("localtest://" + L("net")));
        // The path bar shows a readable root instead of the GVfs folder name.
        auto labelled = [&] {
            for (auto* b : m_left->findChild<PathBar*>()->findChildren<QToolButton*>())
                if (b->isVisibleTo(m_left) && b->text().startsWith(QString::fromUtf8("⇄ localtest")))
                    return true;
            return false;
        };
        QTRY_VERIFY(labelled());

        // Copy into the remote folder with F5 (the normal job engine).
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "tocopy.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_right->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(L("net/tocopy.txt")), QByteArray("over gvfs"));
        QTRY_VERIFY(leftModel()->rowOf("tocopy.txt") >= 0);

        // Disconnect: the panel steps out, the mount goes away.
        m_left->disconnectMount(remoteRootOf(m_left->path())->root);
        QTRY_COMPARE(m_left->path(), QDir::homePath());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(viaGvfs), 10000);
        Network::instance().forgetServer("localtest://" + L("net"));

        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());
    }

    void serverLocation()
    {
        QTemporaryDir serverDir;
        const QString srv = serverDir.path();
        QVERIFY(QDir().mkpath(srv + "/www"));
        writeFile(srv + "/www/index.html", "<h1>hi</h1>");
        writeFile(L("upload.txt"), "to the server");
        auto backend = std::make_shared<TestBackend>(srv);
        registerBackend("test", backend);
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());

        // Typed into the path bar like any folder.
        m_right->focusPathEdit();
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        edit->setText("test://srv/www/");
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/www"));
        QVERIFY(m_right->isRemote());
        QVERIFY(rightModel()->rowOf("index.html") >= 0);
        auto crumbs = [&] {
            QStringList t;
            for (auto* b : m_right->findChild<PathBar*>()->findChildren<QToolButton*>())
                if (b->isVisibleTo(m_right))
                    t << b->text();
            return t;
        };
        QTRY_VERIFY(crumbs().contains(QString::fromUtf8("⇄ test://srv")));
        QCOMPARE(m_right->tab()->title(), QString("www"));

        // F5 from local to the server panel; the panel updates without inotify.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "upload.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(srv + "/www/upload.txt"), QByteArray("to the server"));
        QTRY_VERIFY(rightModel()->rowOf("upload.txt") >= 0);

        // F7 and rename on the server.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        answerNextDialog([](QWidget* w) {
            if (auto* d = qobject_cast<QInputDialog*>(w)) {
                d->setTextValue("assets/css");
                d->accept();
            }
        });
        QTest::keyClick(m_right->view(), Qt::Key_F7);
        QTRY_VERIFY(QFileInfo(srv + "/www/assets/css").isDir());
        QTRY_VERIFY(rightModel()->rowOf("assets") >= 0);
        m_right->doRename("upload.txt", "renamed.txt");
        QTRY_VERIFY(QFile::exists(srv + "/www/renamed.txt"));
        QTRY_VERIFY(rightModel()->rowOf("renamed.txt") >= 0);

        // F3: downloaded to the cache, then shown.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "index.html");
        QTest::keyClick(m_right->view(), Qt::Key_F3);
        ViewerWindow* viewer = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* v = qobject_cast<ViewerWindow*>(w); v && v->isVisible())
                    viewer = v;
            return viewer != nullptr;
        }());
        QTRY_COMPARE(viewer->currentPath(), remoteCachePath("test://srv/www/index.html"));
        QCOMPARE(readFile(viewer->currentPath()), QByteArray("<h1>hi</h1>"));
        viewer->close();

        // F4: edit a copy; saving uploads it again.
        const QString editorScript = serverDir.filePath("fake-editor.sh");
        writeFile(editorScript, "#!/bin/sh\nprintf edited > \"$1\"\n");
        QFile::setPermissions(editorScript, QFile::permissions(editorScript) | QFile::ExeOwner);
        QSettings().setValue("tools/editor", editorScript);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        moveCursorTo(m_right, "renamed.txt");
        QTest::keyClick(m_right->view(), Qt::Key_F4);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(srv + "/www/renamed.txt"), QByteArray("edited"), 10000);
        QSettings().remove("tools/editor");

        // F8 on a server: permanent (no trash), says so.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        QString info;
        answerNextDialog([&](QWidget* w) {
            if (auto* box = qobject_cast<QMessageBox*>(w)) {
                info = box->informativeText();
                box->button(QMessageBox::Yes)->click();
            }
        });
        m_right->view()->setFocus();
        moveCursorTo(m_right, "assets");
        QTest::keyClick(m_right->view(), Qt::Key_F8);
        QTRY_VERIFY(!QFile::exists(srv + "/www/assets"));
        QVERIFY(info.contains("no trash"));
        QTRY_COMPARE(rightModel()->rowOf("assets"), -1);

        // Up to the server root, which can't go further.
        m_right->goUp();
        QTRY_COMPARE(rightModel()->path(), QString("test://srv/"));
        m_right->goUp();
        QTest::qWait(100);
        QCOMPARE(rightModel()->path(), QString("test://srv/"));

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
        // A poll can still be reading the mock server after navigation completes.
        QVERIFY(ioPool()->waitForDone(5000));
        registerBackend("test", nullptr);
    }

    // A login asks user and password in one dialog.
    void loginDialog()
    {
        std::optional<QString> answer;
        std::thread asker([&] {
            AuthQuestion q;
            q.kind = AuthQuestion::Kind::Login;
            q.location = QStringLiteral("smb://nas/");
            q.message = QStringLiteral("Log in to nas");
            q.user = QStringLiteral("bob");
            answer = askAuth(q);
        });
        QDialog* dlg = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::allWidgets())
                if (w->objectName() == QLatin1String("authDialog") && w->isVisible())
                    dlg = qobject_cast<QDialog*>(w);
            return dlg != nullptr;
        }());
        const QList<QLineEdit*> edits = dlg->findChildren<QLineEdit*>();
        QCOMPARE(edits.size(), 2);
        QCOMPARE(edits[0]->text(), QStringLiteral("bob"));
        QCOMPARE(edits[1]->echoMode(), QLineEdit::Password);
        edits[0]->setText(QStringLiteral(" WORK\\alice "));
        edits[1]->setText(QStringLiteral("se cret"));
        dlg->accept();
        asker.join();
        QCOMPARE(answer, std::optional<QString>(QStringLiteral("WORK\\alice\nse cret")));
    }

    // "Remember in the keyring": offered with a keyring, and reported to the asker.
    void loginDialogRemembers()
    {
        setKeyringHooksForTests({[](const QString&, const QString&, const QString&) { return std::optional<StoredLogin>{}; },
                                 [](const QString&, const QString&, const StoredLogin&) { return true; }});
        std::optional<QString> answer;
        bool remembered = false;
        std::thread asker([&] {
            AuthQuestion q;
            q.kind = AuthQuestion::Kind::Login;
            q.location = QStringLiteral("smb://nas/");
            q.user = QStringLiteral("bob");
            q.offerRemember = true;
            answer = askAuth(q);
            remembered = lastAnswerRemember();
        });
        QDialog* dlg = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* w : QApplication::allWidgets())
                if (w->objectName() == QLatin1String("authDialog") && w->isVisible())
                    dlg = qobject_cast<QDialog*>(w);
            return dlg != nullptr;
        }());
        auto* box = dlg->findChild<QCheckBox*>();
        QVERIFY(box);
        box->setChecked(true);
        dlg->findChildren<QLineEdit*>().at(1)->setText("pw");
        dlg->accept();
        asker.join();
        QCOMPARE(answer, std::optional<QString>(QStringLiteral("bob\npw")));  // no marker left in it
        QVERIFY(remembered);
        setKeyringHooksForTests({});
    }

    void sftpInPanel()
    {
        if (!hasBackend("sftp"))
            QSKIP("built without libssh");
        SshServer server;
        if (!server.start())
            QSKIP("could not start a test sshd");
        QVERIFY(QDir().mkpath(L("remote/docs")));
        writeFile(L("remote/readme.txt"), "served over sftp");
        writeFile(L("toupload.txt"), "uploaded");
        m_left->navigate(L());
        QTRY_COMPARE(leftModel()->path(), L());

        // Typed address; the unknown host key is asked in a dialog.
        QString question;
        answerNextDialog([&](QWidget* w) {
            question = w->windowTitle();
            for (auto* b : w->findChildren<QPushButton*>())
                if (b->text() == QObject::tr("Trust and connect"))
                    b->click();
        });
        m_right->focusPathEdit();
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(edit);
        const QString loc = server.location(L("remote"));
        edit->setText(loc);
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(rightModel()->path(), loc, 10000);
        QCOMPARE(question, QObject::tr("Unknown server"));
        QVERIFY(m_right->isRemote());
        QVERIFY(rightModel()->rowOf("docs") >= 0);
        QVERIFY(rightModel()->rowOf("readme.txt") >= 0);

        // F5 upload into the sftp panel.
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_left->view()->setFocus();
        moveCursorTo(m_left, "toupload.txt");
        answerNextDialog(acceptDialog);
        QTest::keyClick(m_left->view(), Qt::Key_F5);
        QTRY_COMPARE(readFile(L("remote/toupload.txt")), QByteArray("uploaded"));
        QTRY_VERIFY(rightModel()->rowOf("toupload.txt") >= 0);

        // The preview of a folder on the server: its contents, then the total.
        writeFile(L("remote/docs/a.txt"), "1234");
        QVERIFY(QDir().mkpath(L("remote/docs/deeper")));
        writeFile(L("remote/docs/deeper/b.txt"), "123456");
        QSettings().setValue("view/previewPosition", "each");
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        QTRY_VERIFY(m_right->isActive());
        moveCursorTo(m_right, "docs");
        QTest::keyClick(m_right->view(), Qt::Key_Q, Qt::ControlModifier);
        ViewerWindow* pv = nullptr;
        QTRY_VERIFY((pv = m_window->findChild<ViewerWindow*>("quickViewRight")) != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(pv->currentPath(), loc + "/docs", 10000);
        QTRY_COMPARE_WITH_TIMEOUT(pv->contentsList()->topLevelItemCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(pv->richInfo().contains("10 B"), 10000);  // 4 + 6 bytes, in one request
        QTest::keyClick(m_right->view(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(m_window->findChild<ViewerWindow*>("quickViewRight") == nullptr);
        QSettings().setValue("view/preview", false);

#ifndef Q_OS_MACOS
        // This block requires GNU stat/getfacl on the local test server.
        // Alt+A on the server: its own permissions tools, its users.
        ::chmod(QFile::encodeName(L("remote/readme.txt")).constData(), 0600);
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
        m_right->view()->setFocus();
        m_right->refresh();
        QTRY_VERIFY(rightModel()->rowOf("readme.txt") >= 0);
        moveCursorTo(m_right, "readme.txt");
        auto permsChecked = std::make_shared<bool>(false);
        answerNextDialog([permsChecked](QWidget* w) {
            auto* dlg = qobject_cast<PermissionsDialog*>(w);
            QVERIFY(dlg);
            QCOMPARE(dlg->octalEdit()->text(), QString("600"));
            QVERIFY(dlg->addEdit());  // extra people and groups: the server has getfacl
            dlg->bitBox(1, 1)->click();  // group may write
            *permsChecked = true;
            dlg->applyButton()->click();
        });
        QTest::keyClick(m_right->view(), Qt::Key_A, Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(*permsChecked, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(readPermissions(L("remote/readme.txt")).mode, 0620u, 10000);

#endif
        // A bare address opens the login's home folder.
        m_right->connectTo(server.location().chopped(1));
        QTRY_COMPARE_WITH_TIMEOUT(rightModel()->path(), server.location(QDir::homePath()), 10000);

        m_right->navigate(R());
        QTRY_COMPARE(rightModel()->path(), R());
    }

    void cleanupTestCase()
    {
        // What the tests moved to a trash (/tmp has its own .Trash-<uid>) goes for good.
        QList<TrashItem> ours;
        for (const TrashItem& it : listTrash())
            if (it.originalPath.startsWith(m_tmp.path() + '/'))
                ours << it;
        deleteTrashItems(ours);
        m_window->close();
        m_window.reset();
    }
};

QTEST_MAIN(TestUi)
#include "test_ui.moc"
