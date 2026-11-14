#pragma once

#include "core/FileEntry.h"

#include <QStringView>

#include <vector>

namespace tandem {

// Saved as numbers: new ones go at the end.
enum class SortColumn : uint8_t {
    Name = 0, Ext = 1, Size = 2, Time = 3, Attr = 4,
    Owner = 5, Group = 6, Octal = 7, Created = 8, Accessed = 9, LinkTarget = 10,
};

struct SortSpec {
    SortColumn column = SortColumn::Name;
    bool descending = false;
    bool foldersFirst = true;
    bool operator==(const SortSpec&) const = default;
};

// Locale-aware, case-insensitive, numeric-aware ("file2" < "file10") key.
// Uses a per-thread collator, so it is safe to call from workers.
QCollatorSortKey makeSortKey(const QString& name);

// Extension without the dot; empty for dotfiles like ".bashrc". Compressed
// tarballs keep both parts ("tar.gz").
QStringView extensionOf(const QString& name);

// Directories come first by default; ".." is always the very first row.
bool entryLess(const FileEntry& a, const FileEntry& b, const SortSpec& spec);
void sortEntries(std::vector<FileEntry>& entries, const SortSpec& spec);

}  // namespace tandem
