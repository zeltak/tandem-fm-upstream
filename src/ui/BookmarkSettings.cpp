#include "ui/BookmarkSettings.h"

#include "core/FolderHistory.h"
#include "ui/Keymap.h"
#include "ui/Theme.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace tandem {
namespace {

class BookmarkKeyEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        QLineEdit::keyPressEvent(event);
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            event->accept();  // Apply the edit without activating Settings' Close button.
    }
};

QString spaced(const QString& key)
{
    QStringList tokens;
    for (QChar c : key)
        tokens.append(QString(c));
    return tokens.join(QLatin1Char(' '));
}

bool startsWith(const QStringList& sequence, const QStringList& start)
{
    return sequence.size() >= start.size() && sequence.mid(0, start.size()) == start;
}

}  // namespace

BookmarkSettings::BookmarkSettings(QWidget* parent, std::function<QString()> currentFolder)
    : QWidget(parent), m_currentFolder(std::move(currentFolder))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 14, 10);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("bookmarkFilter"));
    m_filter->setPlaceholderText(tr("Find a favourite or key"));
    layout->addWidget(m_filter);
    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("bookmarkList"));
    m_list->setHeaderLabels({tr("Folder"), tr("Bookmark keys"), tr("Full modal chain")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    // Sized in fill(): ResizeToContents measures while the header paints, and
    // the layout that forces scrolls the list mid-paint, which crashes on macOS.
    m_list->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    layout->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Bookmark keys:"), this));
    m_edit = new BookmarkKeyEdit(this);
    m_edit->setObjectName(QStringLiteral("bookmarkKeys"));
    m_edit->setPlaceholderText(tr("One character per step, for example: w s"));
    row->addWidget(m_edit, 1);
    m_apply = new QPushButton(tr("Apply"), this);
    m_apply->setObjectName(QStringLiteral("bookmarkApply"));
    row->addWidget(m_apply);
    layout->addLayout(row);
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("bookmarkNote"));
    m_note->setWordWrap(true);
    setThemeStyle(m_note, QStringLiteral("color: @dimText;"));
    layout->addWidget(m_note);
    auto* buttons = new QHBoxLayout;
    m_add = new QPushButton(tr("Add current folder"), this);
    m_add->setObjectName(QStringLiteral("bookmarkAdd"));
    m_clear = new QPushButton(tr("Clear key"), this);
    m_clear->setObjectName(QStringLiteral("bookmarkClear"));
    m_remove = new QPushButton(tr("Remove favourite"), this);
    m_remove->setObjectName(QStringLiteral("bookmarkRemove"));
    for (QPushButton* button : {m_apply, m_add, m_clear, m_remove})
        button->setAutoDefault(false);
    buttons->addWidget(m_add);
    buttons->addStretch();
    buttons->addWidget(m_clear);
    buttons->addWidget(m_remove);
    layout->addLayout(buttons);
    auto& history = FolderHistory::instance();
    connect(&history, &FolderHistory::bookmarksChanged, this, [this] { fill(); });
    connect(&Keymap::instance(), &Keymap::changed, this, [this] { fill(); });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { fill(); });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] { selected(); });
    connect(m_edit, &QLineEdit::textChanged, this, [this] { preview(); });
    connect(m_edit, &QLineEdit::returnPressed, this, [this] { apply(); });
    connect(m_apply, &QPushButton::clicked, this, [this] { apply(); });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (!path().isEmpty())
            FolderHistory::instance().setBookmark(path(), {});
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (!path().isEmpty())
            FolderHistory::instance().setPinned(path(), false);
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        const QString current = m_currentFolder();
        if (current.isEmpty())
            return;
        m_filter->clear();
        FolderHistory::instance().setPinned(current, true);
        for (int i = 0; i < m_list->topLevelItemCount(); ++i)
            if (m_list->topLevelItem(i)->text(0) == current)
                m_list->setCurrentItem(m_list->topLevelItem(i));
        m_edit->setFocus();
    });
    fill();
}

QString BookmarkSettings::path() const
{
    return m_list->currentItem() ? m_list->currentItem()->text(0) : QString();
}

QStringList BookmarkSettings::prefix() const
{
    QStringList result;
    for (const auto& binding : Keymap::instance().modalBindings())
        if (binding.action == QLatin1String("bookmark-prefix"))
            result = binding.sequence;
    return result;
}

QString BookmarkSettings::key() const
{
    return m_edit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts).join(QString());
}

void BookmarkSettings::fill()
{
    const QString previous = path();
    const QString draft = m_edit->text();
    const QString previousKey = m_selectedKey;
    QSignalBlocker block(m_list);
    m_list->clear();
    const QString filter = m_filter->text();
    const QString lead = prefix().join(QLatin1Char(' '));
    QTreeWidgetItem* selectedItem = nullptr;
    for (const auto& bookmark : FolderHistory::instance().pinnedBookmarks()) {
        const QString keys = spaced(bookmark.key);
        const QString full = lead.isEmpty() || keys.isEmpty() ? QString() : lead + QLatin1Char(' ') + keys;
        if (!filter.isEmpty() && !(bookmark.path + QLatin1Char(' ') + keys + QLatin1Char(' ') + full)
                                      .contains(filter, Qt::CaseInsensitive))
            continue;
        auto* item = new QTreeWidgetItem(m_list, {bookmark.path, keys, full});
        item->setToolTip(0, bookmark.path);
        if (bookmark.path == previous)
            selectedItem = item;
    }
    m_list->resizeColumnToContents(1);
    m_list->resizeColumnToContents(2);
    m_list->setCurrentItem(selectedItem);
    selected();
    // A new pin or a changed modal prefix must not discard an unfinished edit.
    if (selectedItem && m_selectedKey == previousKey)
        m_edit->setText(draft);
    preview();
}

void BookmarkSettings::selected()
{
    m_selectedKey = FolderHistory::instance().bookmarkKey(path());
    m_edit->setText(spaced(m_selectedKey));
    preview();
}

void BookmarkSettings::preview()
{
    const bool loaded = FolderHistory::instance().isLoaded();
    const bool picked = loaded && !path().isEmpty();
    m_add->setEnabled(loaded);
    m_edit->setEnabled(picked);
    m_clear->setEnabled(picked && !m_selectedKey.isEmpty());
    m_remove->setEnabled(picked);
    m_apply->setEnabled(false);
    if (!picked) {
        m_note->setText(loaded ? tr("Pick a favourite to edit its bookmark keys. Clearing a key keeps the favourite.")
                               : tr("Loading…"));
        return;
    }
    const QStringList tokens = m_edit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token.size() != 1 || !token.front().isPrint() || token.front().isSpace()) {
            m_note->setText(tr("Use single characters separated by spaces, for example: w s."));
            return;
        }
    }
    m_apply->setEnabled(key() != m_selectedKey);
    QStringList notes;
    const QStringList lead = prefix();
    if (!key().isEmpty()) {
        if (lead.isEmpty()) {
            notes << tr("The modal bookmark prefix is disabled. These keys still work in the Bookmarks popup.");
        } else {
            notes << tr("Full modal chain: %1. Enter only the bookmark keys above, without the prefix %2.")
                         .arg((lead + tokens).join(QLatin1Char(' ')), lead.join(QLatin1Char(' ')));
            for (const auto& binding : Keymap::instance().modalBindings()) {
                if (binding.action == QLatin1String("bookmark-prefix"))
                    continue;
                // After the first bookmark character, the popup owns the remaining keys.
                const QStringList first = lead + QStringList{tokens.first()};
                if (startsWith(first, binding.sequence) || startsWith(binding.sequence, first)) {
                    notes << tr("This modal chain conflicts with %1. Change the bookmark keys or that command's modal keys.")
                                 .arg(Keymap::actionTitle ? Keymap::actionTitle(binding.action) : binding.action);
                    break;
                }
            }
        }
        const QString owner = FolderHistory::instance().pathForKey(key());
        if (!owner.isEmpty() && owner != path())
            notes << tr("Applying moves this key from %1. That folder stays a favourite.").arg(owner);
        for (const auto& bookmark : FolderHistory::instance().bookmarks()) {
            if (bookmark.path != path() && bookmark.key.startsWith(key()) && bookmark.key != key()) {
                notes << tr("Press Enter after this chain to choose it instead of a longer bookmark chain.");
                break;
            }
        }
    } else {
        notes << tr("Clearing a key keeps the favourite.");
    }
    m_note->setText(notes.join(QLatin1Char('\n')));
}

void BookmarkSettings::apply()
{
    if (m_apply->isEnabled())
        FolderHistory::instance().setBookmark(path(), key());
}

}  // namespace tandem
