#pragma once

#include <QDialog>

class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

namespace tandem {

class MainWindow;

// Ctrl+,: appearance, behaviour, external tools and key bindings. Changes
// apply right away; theme and language on the next start.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(MainWindow* window);

    void showPage(int index);  // 0 appearance, 1 behaviour, 2 startup, 3 tools, 4 keys, 5 plugins, 6 bookmarks
    // For tests.
    QTreeWidget* keyList() const { return m_keys; }
    QKeySequenceEdit* keyEdit() const { return m_keyEdit; }
    QLabel* keyNote() const { return m_keyNote; }

private:
    QWidget* appearancePage();
    QWidget* themeChooser();
    QWidget* behaviourPage();
    QWidget* startupPage();
    void fillWorkspaces(int select = 0);
    QWidget* toolsPage();
    QWidget* keysPage();
    QWidget* pluginsPage();
    void fillPlugins();
    void fillKeys();
    void keySelected();
    void assignKeys(const QKeySequence& seq);

    MainWindow* m_window;
    QListWidget* m_nav;
    QStackedWidget* m_pages;
    QTreeWidget* m_keys = nullptr;
    QLineEdit* m_keyFilter = nullptr;
    QKeySequenceEdit* m_keyEdit = nullptr;
    QLabel* m_keyNote = nullptr;
    QPushButton* m_keyClear = nullptr;
    QPushButton* m_keyReset = nullptr;
    bool m_loadingKey = false;
    QWidget* m_pluginList = nullptr;
    QComboBox* m_startup = nullptr;
    QTreeWidget* m_workspaces = nullptr;
    QList<QPushButton*> m_workspaceEdits;  // need a workspace picked
    QLabel* m_workspaceNote = nullptr;
    bool m_fillingWorkspaces = false;
};

// Keys the user changed, kept in keys.toml per command id; applied at start and on every save.
// From keys.toml; returns entries whose keys it can't read ("copy = \"Fx\"").
QStringList applyKeyBindings(const QList<class QAction*>& actions);
void saveKeyBinding(QAction* action);  // drops the setting when it's the default again

}  // namespace tandem
