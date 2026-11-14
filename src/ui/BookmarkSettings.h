#pragma once

#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace tandem {

// Edits the existing folder pins and their character chains, without file I/O.
class BookmarkSettings : public QWidget {
    Q_OBJECT
public:
    BookmarkSettings(QWidget* parent, std::function<QString()> currentFolder);

private:
    void fill();
    void selected();
    void preview();
    void apply();
    QString path() const;
    QString key() const;
    QStringList prefix() const;

    std::function<QString()> m_currentFolder;
    QTreeWidget* m_list;
    QLineEdit* m_filter;
    QLineEdit* m_edit;
    QLabel* m_note;
    QPushButton* m_apply;
    QPushButton* m_clear;
    QPushButton* m_remove;
    QPushButton* m_add;
    QString m_selectedKey;
};

}  // namespace tandem
