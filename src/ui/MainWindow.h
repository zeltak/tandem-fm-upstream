#pragma once

#include "core/Archive.h"

#include "core/FileOps.h"
#include "core/MultiRename.h"
#include "core/Plugins.h"
#include "core/Workspaces.h"
#include "core/Undo.h"
#include "ui/FuzzyPopup.h"
#include "ui/IconCache.h"
#include "ui/KeyChordPopup.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QUrl>

#include <functional>

class QAbstractButton;
class QMenu;
class QPushButton;
class QSplitter;
class QVBoxLayout;

class QFileSystemWatcher;
class QTimer;

namespace tandem {

class JobManager;
struct MacApp;
class ViewerWindow;
class JobsPanel;
class Panel;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class SettingsDialog;
public:
    // The key that runs a command now, for hints: with modal keys on, its
    // letter (when it has one and `modal`), else its first shortcut.
    QString keyHint(const QString& commandId, bool modal = true) const;
    explicit MainWindow(const QStringList& startPaths);
    // "each" (below each panel), "bottom" (below both) or "right".
    void setPreviewPosition(const QString& code);
    // Zoom of the lists (percent) or the grids (thumbnail size), in both panels; remembered.
    void setZoom(bool grid, int value);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    Panel* active() const { return m_active; }
    Panel* other() const;
    void setActive(Panel* p);

    QAction* addCommand(QMenu* menu, const char* source, const QList<QKeySequence>& keys, std::function<void()> fn);
    QAction* addCommand(QMenu* menu, const QString& text, const QList<QKeySequence>& keys, std::function<void()> fn,
                        const QString& id);
    void createMenus();
    QWidget* createFKeyBar();
    QWidget* createTopBar(QWidget* parent);
    void scheduleFKeyUpdate();
    void updateFKeys();
    void updateTransfers();
    void updateSelectionBars(const QString& target);

    // target: a folder chosen elsewhere (the picker); ask: still show the
    // destination dialog with it filled in; now: start without queueing.
    void copyOrMove(CopyJob::Mode mode, const QString& target = {}, bool ask = true, bool now = false);
    // Pick where to: the tabs of both panels, bookmarks, recent folders, a typed path.
    void copyOrMoveTo(CopyJob::Mode mode, QWidget* anchor = nullptr);
    void deleteSelected(bool permanent);
    void makeFolder(const QString& given = {});  // asks for a name unless given
    void viewCurrent();
    void editCurrent();
    void openFile(const QString& path);
    void handleDrop(const QList<QUrl>& urls, const QString& targetDir, Qt::DropAction action);
    void copyToClipboard(bool cut);
    void copyPathsAsText();
    void copyNamesAsText(bool withoutExtension);
    void copyFolderPath();
    void addCopyAsTextMenu(QMenu* menu);
    void paste();
    void swapPanels();
    void openInPanel(Panel* target);
    void showShortcuts();
    void showContextMenu(Panel* panel, const QPoint& pos, bool onItem);
    void addOpenWithMenu(QMenu* menu, const QStringList& paths);
#ifdef Q_OS_MACOS
    void openWithMacApp(const MacApp& app, const QStringList& paths);
#endif
    void openWithPicker();
    void fillBookmarksMenu(QMenu* menu);
    void setupKeymap();
    void applyKeymap();
    QList<struct ViewerAction> previewActions(Panel* p, const FileEntry& e);
    // Plugins: read on a worker, then their actions become commands.
    void loadPlugins(std::function<void()> then = {});
    void installPluginCommands();
    PluginContext pluginContext(Panel* p) const;
    void runPluginAction(const Plugin& plugin, const PluginAction& action, Panel* panel, const QString& choice = {});
    // Fills `menu` with the action's entries once they are known.
    void fillPluginChoices(QMenu* menu, const Plugin& plugin, const PluginAction& action, Panel* panel);
    void addPluginItems(QMenu* menu, Panel* panel, bool onItem);
    // Workspaces: named sets of both panels' tabs (Alt+W).
    Workspace currentWorkspace() const;
    void openWorkspace(const Workspace& workspace);
    void showWorkspaces();
    void saveWorkspacePicker();
    void saveCurrentAsWorkspace(int id, const QString& name);
    void installWorkspaceCommands();
    void makeFile(const QString& given = {});
    void createNew();  // yazi's a: a file, or a folder when the name ends with /
    void runCommandId(const QString& id);  // modal keys
    void openTerminal(const QString& dir = {});
    void pasteInto(const QString& dir);
    // Copies or moves paths into a folder (or an archive's): paste, shelves.
    void putInto(const QStringList& paths, const QString& dir, bool move);
    // Shelves: put the selection on one, look in one, copy or move one here.
    void putOnShelf(int shelf);  // 0: ask which
    void takeFromShelf(int shelf, bool move);  // 0: ask which
    void shelfPicker();
    void shelfTakePicker();
    QList<KeyChordPopup::Choice> shelfChoices() const;  // "Shelf 2  ·  3 items: a.txt, …"
    void openShelf(int shelf);  // or closes it, when it's the one open
    bool refuseInArchive(Panel* p);
    Panel* previewOwner(QWidget* w) const;
    void returnFocusFromPreview(QWidget* w);
    // Archive changes run as one rewrite job. false (with a message) when the
    // archive's format can't be written.
    bool changeArchive(Panel* p, ArchiveChanges changes, const QString& title);
    bool addToArchive(Panel* p, const QStringList& localPaths, const QString& innerDir, bool move);
    Panel* archivePanelFor(const QString& path) const;
    void editInArchive(Panel* p, const QString& path);
    void watchEdit(const QString& local);
    bool refuseOnServer(Panel* p, const QString& what);
    // Archives: extract; servers: download (cached). Then fn(local path).
    // `local` overrides where a server file is put (edit copies live apart
    // from preview copies, so a preview never overwrites unsaved edits).
    void withLocalCopy(Panel* p, const QString& path, std::function<void(const QString& local)> fn,
                       const QString& local = {});
    void editRemote(Panel* p, const QString& path);
    void editedFileChanged(const QString& local);
    void uploadEdited();
    QFileSystemWatcher* m_editWatch = nullptr;
    QTimer* m_uploadTimer = nullptr;
    QSet<QString> m_changedEdits;
    QHash<QString, QString> m_editedRemote;  // local copy -> server location
    QHash<QString, QPair<QString, QString>> m_editedArchive;  // local copy -> (archive, member)
    // Extracts a file from the panel's archive to the cache (off-thread), then calls fn.
    void withExtracted(Panel* p, const QString& path, std::function<void(const QString& local)> fn);
    void openFrom(Panel* p, const QString& path);
    void pack();
    void multiRename();
    void editPermissions();
    void createLink();
    void renameInEditor();
    bool collectRenameItems(Panel* p, QList<RenameItem>& items, QSet<QString>& existing);
    void startEditorRename(Panel* p, const QList<RenameItem>& items, const QSet<QString>& existing);
    void runRenamePlan(Panel* p, const RenamePlan& plan);
    void undo();
    void pushUndo(UndoEntry entry);
    void updateUndoAction();
    void recordJobUndo(Job* job);
    void notifyFinished(Job* job);
    void unpack(bool here = false);  // here: into the shown folder, without asking
    void commandPalette();
    void jumpToFolder();
    QList<PopupItem> folderItems() const;
    void toggleQuickView();
    void updateQuickView();
    void setIconTheme(const QString& id);
    void repaintFileViews();  // after a change in how files are drawn

    void loadSettings(const QStringList& startPaths);
    void saveSettings();

    Panel* m_left;
    Panel* m_right;
    Panel* m_active = nullptr;
    QSplitter* m_splitter;       // left | right (| preview when on the right)
    QSplitter* m_vsplit;         // panels / preview when below both
    QSplitter* m_leftCol;        // left panel / its own preview
    QSplitter* m_rightCol;       // right panel / its own preview
    QString previewMode() const;  // "each" (default), "bottom", "right"
    void openSettings(int page = 0);
    void showTrash();
    void showServers(const QString& uri = {});
    void findDuplicates();
    void showDiskUsage();
    void showChecksums();
    void closePreview();
    void updatePreviewFor(ViewerWindow* preview, Panel* panel);
    void openCommandBar();
    void compareFolders();  // Shift+F2: mark what differs
    void synchronizeFolders();
    void runCommand(const QString& command, bool inTerminal);
    class CommandBar* m_commandBar = nullptr;
    JobManager* m_jobs;
    JobsPanel* m_jobsPanel;
    QAction* m_hiddenAction = nullptr;
    QAction* m_foldersFirstAction = nullptr;
    QHash<QString, QAction*> m_actions;
    FuzzyPopup* m_popup = nullptr;
    KeyChordPopup* m_chords = nullptr;
    ViewerWindow* m_quickView = nullptr;   // the preview (left one in "each" mode)
    ViewerWindow* m_quickView2 = nullptr;  // the right panel's own preview in "each" mode
    QTimer* m_quickTimer = nullptr;
    // F3's window, linked to the panel it came from: shows that panel's cursor.
    QPointer<ViewerWindow> m_viewer;
    QPointer<Panel> m_viewerPanel;
    bool stepViewer(int direction, bool sameKind);
    void followInViewer();
    bool movesViewerPanel(const QKeyEvent* e) const;
    double m_panelRatio = 0.5;  // left panel's share of the two panels
    QList<IconTheme> m_iconThemes;
    bool m_iconThemesLoaded = false;
    QList<Plugin> m_plugins;
    QList<QAction*> m_pluginActions;
    QMenu* m_pluginMenu = nullptr;
    QList<QAction*> m_workspaceActions;  // one command per workspace
    QMenu* m_workspaceMenu = nullptr;
    QHash<QString, QAbstractButton*> m_fkeys;  // F3 … F8
    class ShelfBar* m_shelfBar = nullptr;
    class ShelfPanel* m_shelfPanel = nullptr;
    bool m_fkeyUpdatePending = false;
    QTimer* m_transferTimer = nullptr;
    QPushButton* m_searchField = nullptr;
    QList<UndoEntry> m_undo;
};

}  // namespace tandem
