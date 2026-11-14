#include "TestLaunchGuard.h"
#include "core/Archive.h"
#include "core/Backend.h"
#include "core/ConfigDir.h"
#include "core/DesktopApps.h"
#include "core/DirLister.h"
#include "core/FileOps.h"
#include "core/Search.h"
#include "core/Shelves.h"
#include "core/FolderCompare.h"
#include "core/Remote.h"
#include "TestBackend.h"
#include "SshServer.h"
#include "core/GitStatus.h"
#include "core/Mounts.h"
#include "core/Trash.h"
#include "core/Undo.h"
#include "core/FolderHistory.h"
#include "core/Plugins.h"
#include "core/Fuzzy.h"
#include "core/IoScheduler.h"
#include "core/MultiRename.h"
#include "core/Checksums.h"
#include "core/DiskUsage.h"
#include "core/Duplicates.h"
#include "core/ExifDate.h"
#include "core/Keyring.h"
#include "core/Permissions.h"
#include "core/Sorting.h"

#include <QScopeGuard>
#include <QSettings>

#include "core/TomlSettings.h"
#include "core/Servers.h"
#include "core/Workspaces.h"
#include <QDateTime>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif
#include <thread>
#include <unistd.h>

using namespace tandem;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return "<missing>";
    return f.readAll();
}

bool runJob(Job& job)
{
    QSignalSpy spy(&job, &Job::finished);
    job.start();
    return spy.wait(20000);
}

FileEntry entry(const QString& name, bool dir = false)
{
    FileEntry e;
    e.name = name;
    e.isDir = dir;
    e.key = makeSortKey(name);
    return e;
}

}  // namespace

class TestCore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        setKeyringHooksForTests({});  // never the real keyring
        installTestLaunchGuard();      // never real apps
    }

    void naturalSort()
    {
        std::vector<FileEntry> v = {entry("file10"), entry("File2"), entry("b-dir", true), entry("file1"),
                                    entry("A-dir", true)};
        FileEntry parent = entry("..", true);
        parent.isParent = true;
        v.push_back(parent);
        sortEntries(v, SortSpec{});
        QStringList names;
        for (auto& e : v)
            names << e.name;
        QCOMPARE(names, QStringList({"..", "A-dir", "b-dir", "file1", "File2", "file10"}));
    }

    void extension()
    {
        QCOMPARE(extensionOf("archive.tar.gz").toString(), QString("tar.gz"));
        QCOMPARE(extensionOf("v1.2.gz").toString(), QString("gz"));
        QCOMPARE(extensionOf(".tar.gz").toString(), QString("gz"));
        QCOMPARE(extensionOf(".bashrc").toString(), QString());
        QCOMPARE(extensionOf("noext").toString(), QString());
        QCOMPARE(extensionOf("trailing.").toString(), QString());
    }

    void listing()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("a.txt"), "hello");
        writeFile(tmp.filePath(".hidden"), "x");
        QVERIFY(QDir(tmp.path()).mkdir("sub"));
        QVERIFY(::symlink("a.txt", QFile::encodeName(tmp.filePath("link")).constData()) == 0);
        QVERIFY(::symlink("missing", QFile::encodeName(tmp.filePath("broken")).constData()) == 0);

        ListResult r = listDirectory(tmp.path(), ListOptions{});
        QCOMPARE(r.error, 0);
        QCOMPARE(int(r.entries.size()), 4);
        QCOMPARE(r.entries[0].name, QString("sub"));
        QVERIFY(r.entries[0].isDir);
        for (auto& e : r.entries) {
            if (e.name == "a.txt")
                QCOMPARE(e.size, int64_t(5));
            if (e.name == "link")
                QVERIFY(e.isLink && !e.brokenLink);
            if (e.name == "broken")
                QVERIFY(e.isLink && e.brokenLink);
            // For the extra columns: what links point to, when it was read
            // and (where the filesystem knows it) created.
            QCOMPARE(e.linkTarget, e.name == "link" ? QString("a.txt") : e.name == "broken" ? QString("missing") : QString());
            QVERIFY(e.atime > 0);
            QVERIFY(e.btime <= QDateTime::currentSecsSinceEpoch());
        }
        ListOptions hidden;
        hidden.showHidden = true;
        QCOMPARE(int(listDirectory(tmp.path(), hidden).entries.size()), 5);
        QCOMPARE(listDirectory(tmp.filePath("nope"), {}).error, ENOENT);
    }

    // The extra columns sort too; owners by id, so each one's files stay together.
    void sortExtraColumns()
    {
        auto entry = [](const QString& name, uint32_t uid, uint32_t mode, int64_t btime, int64_t atime, const QString& link) {
            FileEntry e;
            e.name = name;
            e.key = makeSortKey(name);
            e.uid = e.gid = uid;
            e.mode = mode;
            e.btime = btime;
            e.atime = atime;
            e.linkTarget = link;
            return e;
        };
        std::vector<FileEntry> v = {entry("a", 1000, 0100644, 30, 1, "z"), entry("b", 0, 0100755, 10, 3, "x"),
                                    entry("c", 1000, 0104755, 20, 2, "y")};
        auto names = [&v] {
            QString out;
            for (const FileEntry& e : v)
                out += e.name;
            return out;
        };
        const QList<std::pair<SortColumn, QString>> cases = {
            {SortColumn::Owner, "bac"},  {SortColumn::Group, "bac"},    {SortColumn::Octal, "abc"},
            {SortColumn::Created, "bca"}, {SortColumn::Accessed, "acb"}, {SortColumn::LinkTarget, "bca"},
        };
        for (const auto& [column, order] : cases) {
            sortEntries(v, {column, false});
            QCOMPARE(names(), order);
        }
    }

    void copyTree()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath("src");
        QVERIFY(QDir().mkpath(src + "/sub/deeper"));
        QByteArray big(3 * 1024 * 1024 + 17, '\0');
        for (int i = 0; i < big.size(); ++i)
            big[i] = char(i * 31 + 7);
        writeFile(src + "/a.txt", "alpha");
        writeFile(src + "/sub/big.bin", big);
        writeFile(src + "/sub/deeper/c.txt", "gamma");
        QVERIFY(::chmod(QFile::encodeName(src + "/a.txt").constData(), 0640) == 0);
        QVERIFY(::symlink("a.txt", QFile::encodeName(src + "/lnk").constData()) == 0);
        QVERIFY(QDir().mkdir(tmp.filePath("dst")));

        CopyJob job(CopyJob::Mode::Copy, {src}, tmp.filePath("dst"));
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join('\n')));
        const QString d = tmp.filePath("dst/src");
        QCOMPARE(readFile(d + "/a.txt"), QByteArray("alpha"));
        QCOMPARE(readFile(d + "/sub/big.bin"), big);
        QCOMPARE(readFile(d + "/sub/deeper/c.txt"), QByteArray("gamma"));
        QCOMPARE(QFile::symLinkTarget(d + "/lnk"), d + "/a.txt");
        struct stat st;
        QVERIFY(::stat(QFile::encodeName(d + "/a.txt").constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0640));
        QCOMPARE(job.snapshot().bytesDone, job.snapshot().bytesTotal);
        QVERIFY(QFile::exists(src + "/a.txt"));  // source untouched
    }

    void copySingleAsNewName()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("one.txt"), "1");
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("one.txt")}, tmp.filePath("two.txt"));
        QVERIFY(runJob(job));
        QCOMPARE(readFile(tmp.filePath("two.txt")), QByteArray("1"));
    }

    void moveSameFs()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/inner")));
        writeFile(tmp.filePath("a/inner/f.txt"), "f");
        QVERIFY(QDir().mkdir(tmp.filePath("b")));
        CopyJob job(CopyJob::Mode::Move, {tmp.filePath("a")}, tmp.filePath("b"));
        QVERIFY(runJob(job));
        QVERIFY(!QFile::exists(tmp.filePath("a")));
        QCOMPARE(readFile(tmp.filePath("b/a/inner/f.txt")), QByteArray("f"));
    }

    void moveMergesIntoExistingFolder()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/d")));
        QVERIFY(QDir().mkpath(tmp.filePath("dst/d")));
        writeFile(tmp.filePath("src/d/new.txt"), "new");
        writeFile(tmp.filePath("src/d/same.txt"), "from-src");
        writeFile(tmp.filePath("dst/d/same.txt"), "from-dst");
        writeFile(tmp.filePath("dst/d/keep.txt"), "keep");

        int prompts = 0;
        CopyJob job(CopyJob::Mode::Move, {tmp.filePath("src/d")}, tmp.filePath("dst"));
        job.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Overwrite : Answer::Cancel;
        });
        QVERIFY(runJob(job));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("dst/d/same.txt")), QByteArray("from-src"));
        QCOMPARE(readFile(tmp.filePath("dst/d/new.txt")), QByteArray("new"));
        QCOMPARE(readFile(tmp.filePath("dst/d/keep.txt")), QByteArray("keep"));
        QVERIFY(!QFile::exists(tmp.filePath("src/d")));
    }

    void conflictSkipAllAndRename()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkdir(tmp.filePath("s")));
        QVERIFY(QDir().mkdir(tmp.filePath("t")));
        for (const char* n : {"1.txt", "2.txt", "3.txt"}) {
            writeFile(tmp.filePath(QString("s/") + n), "new");
            writeFile(tmp.filePath(QString("t/") + n), "old");
        }
        const QStringList srcs = {tmp.filePath("s/1.txt"), tmp.filePath("s/2.txt"), tmp.filePath("s/3.txt")};

        int prompts = 0;
        CopyJob skip(CopyJob::Mode::Copy, srcs, tmp.filePath("t"));
        skip.setSyncPromptHandler([&](const PromptRequest&) {
            ++prompts;
            return Answer::SkipAll;
        });
        QVERIFY(runJob(skip));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("t/2.txt")), QByteArray("old"));

        CopyJob rename(CopyJob::Mode::Copy, {srcs[0]}, tmp.filePath("t"));
        rename.setSyncPromptHandler([](const PromptRequest&) { return Answer::Rename; });
        QVERIFY(runJob(rename));
        QCOMPARE(readFile(tmp.filePath("t/1 (2).txt")), QByteArray("new"));
        QCOMPARE(readFile(tmp.filePath("t/1.txt")), QByteArray("old"));
    }

    // Overwriting goes through a hidden file beside the target: the old file
    // survives a cancelled or failed copy, and nothing is left behind.
    void overwriteKeepsOldFileUntilDone()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("source"), QByteArray(1024 * 1024, 'n'));
        writeFile(tmp.filePath("target"), "original");
        const auto leftovers = [&] {
            return QDir(tmp.path()).entryList({".target.tandem-*"}, QDir::Files | QDir::Hidden);
        };

        CopyJob cancelled(CopyJob::Mode::Copy, {tmp.filePath("source")}, tmp.filePath("target"));
        cancelled.setSyncPromptHandler([&](const PromptRequest& r) {
            if (r.kind != PromptRequest::Kind::Conflict)
                return Answer::Cancel;
            cancelled.setPaused(true);  // stops before the first byte is written
            return Answer::Overwrite;
        });
        QSignalSpy done(&cancelled, &Job::finished);
        cancelled.start();
        QTRY_VERIFY(cancelled.snapshot().paused);
        QCOMPARE(readFile(tmp.filePath("target")), QByteArray("original"));
        cancelled.cancel();
        QVERIFY(done.wait(5000));
        QCOMPARE(readFile(tmp.filePath("target")), QByteArray("original"));
        QVERIFY(leftovers().isEmpty());

        CopyJob replace(CopyJob::Mode::Copy, {tmp.filePath("source")}, tmp.filePath("target"));
        replace.setSyncPromptHandler([](const PromptRequest&) { return Answer::Overwrite; });
        QVERIFY(runJob(replace));
        QVERIFY(replace.errors().isEmpty());
        QCOMPARE(readFile(tmp.filePath("target")), QByteArray(1024 * 1024, 'n'));
        QVERIFY(leftovers().isEmpty());
    }

    void copyIntoItselfRejected()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/b")));
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("a")}, tmp.filePath("a/b"));
        QVERIFY(runJob(job));
        QCOMPARE(job.errors().size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("a/b/a")));
    }

    void cancelMidCopyRemovesPartialFile()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkdir(tmp.filePath("t")));
        writeFile(tmp.filePath("f.bin"), QByteArray(1024, 'x'));
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("f.bin")}, tmp.filePath("t"));
        job.setPaused(true);
        QSignalSpy spy(&job, &Job::finished);
        job.start();
        QTest::qWait(50);
        job.cancel();
        QVERIFY(spy.wait(5000));
        QCOMPARE(job.snapshot().state, Job::State::Cancelled);
    }

    void copyIntoSymlinkDescendantRejected()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("source/sub")));
        QVERIFY(::symlink(QFile::encodeName(tmp.filePath("source/sub")).constData(),
                          QFile::encodeName(tmp.filePath("alias")).constData()) == 0);
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("source")}, tmp.filePath("alias"));
        QVERIFY(runJob(job));
        QCOMPARE(job.errors().size(), 1);
        QVERIFY(!QFileInfo::exists(tmp.filePath("source/sub/source")));
    }

    void renameNeverClobbers()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("a"), "source");
        writeFile(tmp.filePath("b"), "destination");
        QCOMPARE(renameNoReplace(tmp.filePath("a"), tmp.filePath("b")), EEXIST);
        QCOMPARE(readFile(tmp.filePath("a")), QByteArray("source"));
        QCOMPARE(readFile(tmp.filePath("b")), QByteArray("destination"));
        QVERIFY(::symlink("missing", QFile::encodeName(tmp.filePath("link")).constData()) == 0);
        QCOMPARE(renameNoReplace(tmp.filePath("a"), tmp.filePath("link")), EEXIST);
        QVERIFY(QFileInfo(tmp.filePath("link")).isSymLink());
    }

#ifdef Q_OS_MACOS
    void copyPreservesMacDirectoryMetadata()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("source/sub")));
        const QByteArray value("directory metadata");
        QCOMPARE(::setxattr(QFile::encodeName(tmp.filePath("source/sub")).constData(), "com.tandem.test",
                            value.constData(), value.size(), 0, 0), 0);
        CopyJob job(CopyJob::Mode::Copy, {tmp.filePath("source")}, tmp.filePath("target"));
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join("; ")));
        char buf[128];
        const auto size = ::getxattr(QFile::encodeName(tmp.filePath("target/sub")).constData(),
                                     "com.tandem.test", buf, sizeof buf, 0, 0);
        QVERIFY(size >= 0);
        QCOMPARE(QByteArray(buf, size), value);
    }

    void copyPreservesMacMetadata()
    {
        QTemporaryDir tmp;
        const QString source = tmp.filePath(QString::fromUtf8("café 日本語.txt"));
        const QString target = tmp.filePath("copy");
        writeFile(source, "contents");
        const QByteArray native = QFile::encodeName(source);
        const QByteArray value("finder metadata");
        QCOMPARE(::setxattr(native.constData(), "com.tandem.test", value.constData(), value.size(), 0, 0), 0);
        CopyJob job(CopyJob::Mode::Copy, {source}, target);
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join("; ")));
        QCOMPARE(readFile(target), QByteArray("contents"));
        char buf[128];
        const auto size = ::getxattr(QFile::encodeName(target).constData(), "com.tandem.test", buf, sizeof buf, 0, 0);
        QVERIFY(size >= 0);
        QCOMPARE(QByteArray(buf, size), value);
    }
#endif

    void deletePermanent()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("x/y/z")));
        writeFile(tmp.filePath("x/y/z/f"), "f");
        writeFile(tmp.filePath("x/g"), "g");
        DeleteJob job({tmp.filePath("x")}, false);
        QVERIFY(runJob(job));
        QVERIFY(job.errors().isEmpty());
        QVERIFY(!QFile::exists(tmp.filePath("x")));
        QCOMPARE(job.snapshot().filesDone, int64_t(5));
    }

    void desktopExecExpansion()
    {
        using L = QList<QStringList>;
        QCOMPARE(DesktopApps::commandLines("kate -b %U", {"/a b", "/c"}),
                 L({{"kate", "-b", "file:///a%20b", "file:///c"}}));
        QCOMPARE(DesktopApps::commandLines("mpv --player-operation-mode=pseudo-gui -- %f", {"/x", "/y"}),
                 L({{"mpv", "--player-operation-mode=pseudo-gui", "--", "/x"},
                    {"mpv", "--player-operation-mode=pseudo-gui", "--", "/y"}}));
        QCOMPARE(DesktopApps::commandLines("gimp %i %F", {"/p.png"}), L({{"gimp", "/p.png"}}));
        QCOMPARE(DesktopApps::commandLines("\"my app\" --x=100%%", {"/f"}), L({{"my app", "--x=100%", "/f"}}));
    }

    void terminalCommands()
    {
        const QStringList cmd = {"nvim", "/tmp/f"};
        QCOMPARE(DesktopApps::terminalCommand("kitty", cmd), QStringList({"kitty", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("/usr/bin/foot", cmd), QStringList({"/usr/bin/foot", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("alacritty", cmd), QStringList({"alacritty", "-e", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("wezterm", cmd), QStringList({"wezterm", "start", "--", "nvim", "/tmp/f"}));
        QCOMPARE(DesktopApps::terminalCommand("kitty --class x", cmd),
                 QStringList({"kitty", "--class", "x", "nvim", "/tmp/f"}));
    }

    void defaultAppFromMimeappsList()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("data/applications")));
        QVERIFY(QDir().mkpath(tmp.filePath("config")));
        writeFile(tmp.filePath("data/applications/aaa.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Aaa\nExec=aaa %f\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("data/applications/zzz.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Zzz\nExec=zzz %F\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("data/applications/gone.desktop"),
                  "[Desktop Entry]\nType=Application\nName=Gone\nExec=gone\nMimeType=text/plain;\n");
        writeFile(tmp.filePath("config/mimeapps.list"),
                  "[Default Applications]\ntext/plain=zzz.desktop;\n[Removed Associations]\ntext/plain=gone.desktop;\n");
        // Later tests need the real ones (the MIME database lives in XDG_DATA_DIRS).
        QHash<const char*, QByteArray> saved;
        for (const char* var : {"XDG_DATA_HOME", "XDG_DATA_DIRS", "XDG_CONFIG_HOME", "XDG_CONFIG_DIRS"})
            saved.insert(var, qgetenv(var));
        auto restoreEnv = qScopeGuard([saved] {
            for (auto it = saved.cbegin(); it != saved.cend(); ++it)
                it.value().isEmpty() ? qunsetenv(it.key()) : qputenv(it.key(), it.value());
        });
        qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.filePath("data")));
        qputenv("XDG_DATA_DIRS", QFile::encodeName(tmp.filePath("none")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(tmp.filePath("config")));
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(tmp.filePath("none")));

        DesktopApps& apps = DesktopApps::instance();
        apps.loadAsync();
        QTRY_VERIFY(apps.isLoaded());
        QCOMPARE(apps.defaultFor({"text/plain"}).id, QString("zzz.desktop"));
        const QList<DesktopApp> all = apps.appsFor({"text/plain"});
        QCOMPARE(all.size(), 2);  // "gone" was removed by the user
        QCOMPARE(all[1].id, QString("aaa.desktop"));
        QVERIFY(apps.defaultFor({"image/x-nothing"}).id.isEmpty());

        // Setting a default rewrites only that line; other sections stay.
        QVERIFY(apps.setDefault("text/plain", "aaa.desktop"));
        QVERIFY(apps.setDefault("image/png", "zzz.desktop"));
        QCOMPARE(apps.defaultFor({"text/plain"}).id, QString("aaa.desktop"));
        QCOMPARE(readFile(tmp.filePath("config/mimeapps.list")),
                 QByteArray("[Default Applications]\ntext/plain=aaa.desktop;\nimage/png=zzz.desktop;\n"
                            "[Removed Associations]\ntext/plain=gone.desktop;\n"));
    }

    void archiveRoundTrip_data()
    {
        QTest::addColumn<QString>("ext");
        QTest::newRow("zip") << ".zip";
        QTest::newRow("7z") << ".7z";
        QTest::newRow("tar.gz") << ".tar.gz";
        QTest::newRow("tar.zst") << ".tar.zst";
    }

    void archiveRoundTrip()
    {
        QFETCH(QString, ext);
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/docs/deep")));
        writeFile(tmp.filePath("src/readme.txt"), "hello");
        writeFile(tmp.filePath("src/docs/a.txt"), "aaa");
        writeFile(tmp.filePath("src/docs/deep/b.bin"), QByteArray(200000, 'b'));
        QVERIFY(::symlink("readme.txt", QFile::encodeName(tmp.filePath("src/link")).constData()) == 0);

        const QString arc = tmp.filePath("out" + ext);
        PackJob pack({tmp.filePath("src")}, arc);
        QVERIFY(runJob(pack));
        QVERIFY2(pack.errors().isEmpty(), qPrintable(pack.errors().join('\n')));
        QVERIFY(QFileInfo(arc).size() > 0);
        QVERIFY(!QFile::exists(arc + ".part"));

        // Browse: root, then a folder inside.
        ListResult root = listLocation(arc, {});
        QCOMPARE(root.error, 0);
        QVERIFY(root.inArchive);
        QCOMPARE(root.archivePath, arc);
        QCOMPARE(int(root.entries.size()), 1);
        QCOMPARE(root.entries[0].name, QString("src"));
        QCOMPARE(root.entries[0].size, int64_t(5 + 3 + 200000));  // folder total (links count 0)
        ListResult docs = listLocation(arc + "/src/docs", {});
        QCOMPARE(docs.error, 0);
        QCOMPARE(docs.innerPath, QString("src/docs"));
        QStringList names;
        for (auto& e : docs.entries)
            names << e.name;
        QCOMPARE(names, QStringList({"deep", "a.txt"}));
        QCOMPARE(listLocation(arc + "/src/nope", {}).error, ENOENT);

        // Extract a subfolder somewhere else.
        QVERIFY(QDir().mkdir(tmp.filePath("x")));
        ExtractJob ex(arc, {"src/docs"}, tmp.filePath("x"));
        QVERIFY(runJob(ex));
        QVERIFY2(ex.errors().isEmpty(), qPrintable(ex.errors().join('\n')));
        QCOMPARE(readFile(tmp.filePath("x/docs/a.txt")), QByteArray("aaa"));
        QCOMPARE(readFile(tmp.filePath("x/docs/deep/b.bin")), QByteArray(200000, 'b'));
        QVERIFY(!QFile::exists(tmp.filePath("x/readme.txt")));

        // Single file into the cache (viewer / open).
        QString err;
        const QString cached = extractToCache(arc, "src/readme.txt", &err);
        QVERIFY2(!cached.isEmpty(), qPrintable(err));
        QCOMPARE(readFile(cached), QByteArray("hello"));
    }

    void archivePassword_data()
    {
        QTest::addColumn<QString>("ext");
        QTest::newRow("zip") << ".zip";
        QTest::newRow("7z") << ".7z";
    }

    // Packing with a password; opening asks for it once (again when wrong,
    // never again after Cancel), and keeps it for the session.
    void archivePassword()
    {
        QFETCH(QString, ext);
        if (!canEncryptArchive("x" + ext))
            QSKIP("7-Zip is not installed");
        QVERIFY(!canEncryptArchive("x.tar.gz"));
        forgetArchivePasswords();
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/docs")));
        writeFile(tmp.filePath("src/readme.txt"), "hello");
        writeFile(tmp.filePath("src/docs/big.bin"), QByteArray(300000, 'b'));
        writeFile(tmp.filePath("src/empty"), "");
        QVERIFY(::symlink("readme.txt", QFile::encodeName(tmp.filePath("src/link")).constData()) == 0);
        const QString arc = tmp.filePath("secret" + ext);
        PackJob pack({tmp.filePath("src")}, arc, "pässword");
        QVERIFY(runJob(pack));
        QVERIFY2(pack.errors().isEmpty(), qPrintable(pack.errors().join('\n')));
        QVERIFY(!readFile(arc).contains("hello"));
        QVERIFY(!QFile::exists(arc + ".part"));

        QStringList answers;  // given in turn; nullopt when used up
        QStringList questions;
        setAuthHandler([&](const AuthQuestion& q) -> std::optional<QString> {
            questions << q.message;
            if (q.location != arc || answers.isEmpty())
                return std::nullopt;
            return answers.takeFirst();
        });
        auto restore = qScopeGuard([] {
            setAuthHandler({});
            forgetArchivePasswords();
        });

        // Cancelled: an error, not a stream of questions.
        forgetArchiveIndex(arc);
        QString err;
        QVERIFY(extractToCache(arc, "src/readme.txt", &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY2(questions.size() == 1, qPrintable(questions.join(" | ")));

        // Wrong, then right: asked twice, then never again.
        questions.clear();
        forgetArchiveIndex(arc);
        answers = {"wrong", "pässword"};
        ListResult root = listLocation(arc + "/src", {.showHidden = true});
        QCOMPARE(root.error, 0);
        QCOMPARE(int(root.entries.size()), 4);
        QVERIFY(QDir().mkdir(tmp.filePath("x")));
        ExtractJob ex(arc, {""}, tmp.filePath("x"));
        QVERIFY(runJob(ex));
        QVERIFY2(ex.errors().isEmpty(), qPrintable(ex.errors().join('\n')));
        QCOMPARE(readFile(tmp.filePath("x/src/readme.txt")), QByteArray("hello"));
        QCOMPARE(readFile(tmp.filePath("x/src/docs/big.bin")), QByteArray(300000, 'b'));
        QCOMPARE(readFile(tmp.filePath("x/src/empty")), QByteArray());
        QCOMPARE(QFile::symLinkTarget(tmp.filePath("x/src/link")), tmp.filePath("x/src/readme.txt"));
        const QString cached = extractToCache(arc, "src/docs/big.bin", &err);
        QVERIFY2(!cached.isEmpty(), qPrintable(err));
        QCOMPARE(readFile(cached).size(), 300000);
        QVERIFY2(questions.size() == 2, qPrintable(questions.join(" | ")));
        QVERIFY(questions[1].contains("Wrong"));
    }

    void archiveUpdate_data()
    {
        QTest::addColumn<QString>("ext");
        QTest::newRow("zip") << ".zip";
        QTest::newRow("7z") << ".7z";
        QTest::newRow("tar") << ".tar";
        QTest::newRow("tar.gz") << ".tar.gz";
        QTest::newRow("tar.zst") << ".tar.zst";
    }

    // Changing an existing archive: remove, rename, new folder, add, replace.
    void archiveUpdate()
    {
        QFETCH(QString, ext);
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/docs/deep")));
        writeFile(tmp.filePath("src/readme.txt"), "hello");
        writeFile(tmp.filePath("src/docs/a.txt"), "aaa");
        writeFile(tmp.filePath("src/docs/deep/b.bin"), QByteArray(200000, 'b'));
        QVERIFY(::symlink("readme.txt", QFile::encodeName(tmp.filePath("src/link")).constData()) == 0);
        const QString arc = tmp.filePath("out" + ext);
        PackJob pack({tmp.filePath("src")}, arc);
        QVERIFY(runJob(pack));
        QVERIFY(listLocation(arc + "/src", {}).error == 0);  // index cached now

        QVERIFY(QDir().mkpath(tmp.filePath("more/inner")));
        writeFile(tmp.filePath("more/inner/m.txt"), "more");
        writeFile(tmp.filePath("extra.txt"), "extra");
        ArchiveChanges c;
        c.remove << "src/docs/deep";
        c.rename << qMakePair(QString("src/readme.txt"), QString("src/README.txt"));  // same size: cache must not lie
        c.mkdir << "src/new/sub";
        c.add << qMakePair(tmp.filePath("extra.txt"), QString("src/docs/extra.txt"))
              << qMakePair(tmp.filePath("more"), QString("src/more"));
        c.moveSources = true;
        ArchiveUpdateJob job(arc, c);
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join('\n')));
        QCOMPARE(job.snapshot().state, Job::State::Done);
        QVERIFY(!QFile::exists(tmp.filePath("extra.txt")));  // moved in
        QVERIFY(!QFile::exists(tmp.filePath("more")));
        QCOMPARE(QDir(tmp.path()).entryList({".out*"}, QDir::Hidden | QDir::Files), QStringList());  // no temp left

        auto names = [&](const QString& inner) {
            QStringList n;
            const ListResult l = listLocation(arc + "/" + inner, {.showHidden = true});
            for (const FileEntry& e : l.entries)
                n << e.name + (e.isLink ? "@" : "");
            n.sort();
            return n;
        };
        QCOMPARE(names("src"), QStringList({"README.txt", "docs", "link@", "more", "new"}));
        QCOMPARE(names("src/docs"), QStringList({"a.txt", "extra.txt"}));
        QCOMPARE(names("src/new"), QStringList({"sub"}));
        QString err;
        QCOMPARE(readFile(extractToCache(arc, "src/README.txt", &err)), QByteArray("hello"));
        QCOMPARE(readFile(extractToCache(arc, "src/more/inner/m.txt", &err)), QByteArray("more"));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/extra.txt", &err)), QByteArray("extra"));

        // Adding a name that exists: asked; "rename" keeps both.
        writeFile(tmp.filePath("a.txt"), "second");
        int prompts = 0;
        ArchiveChanges c2;
        c2.add << qMakePair(tmp.filePath("a.txt"), QString("src/docs/a.txt"));
        ArchiveUpdateJob renameJob(arc, c2);
        renameJob.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Rename : Answer::Cancel;
        });
        QVERIFY(runJob(renameJob));
        QCOMPARE(prompts, 1);
        QCOMPARE(names("src/docs"), QStringList({"a (2).txt", "a.txt", "extra.txt"}));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/a (2).txt", &err)), QByteArray("second"));
        QVERIFY(QFile::exists(tmp.filePath("a.txt")));  // a copy, not a move

        // Saving an edited member replaces it without asking.
        writeFile(tmp.filePath("a.txt"), "edited!");
        ArchiveChanges c3;
        c3.add << qMakePair(tmp.filePath("a.txt"), QString("src/docs/a.txt"));
        c3.replaceExisting = true;
        ArchiveUpdateJob replaceJob(arc, c3);
        replaceJob.setSyncPromptHandler([&](const PromptRequest&) { ++prompts; return Answer::Cancel; });
        QVERIFY(runJob(replaceJob));
        QCOMPARE(prompts, 1);
        QCOMPARE(names("src/docs"), QStringList({"a (2).txt", "a.txt", "extra.txt"}));
        QCOMPARE(readFile(extractToCache(arc, "src/docs/a.txt", &err)), QByteArray("edited!"));
    }

    void archiveUpdateRefuses()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("one.txt"), "x");
        QVERIFY(QProcess::execute("gzip", {"-k", tmp.filePath("one.txt")}) == 0);
        QString why;
        QVERIFY(!canUpdateArchive(tmp.filePath("one.txt.gz"), &why));
        QVERIFY(!canUpdateArchive(tmp.filePath("x.rar"), &why));
        QVERIFY(canUpdateArchive(tmp.filePath("x.tar.xz")));
        ArchiveChanges c;
        c.mkdir << "d";
        ArchiveUpdateJob job(tmp.filePath("one.txt.gz"), c);
        job.setSyncPromptHandler([](const PromptRequest&) { return Answer::Cancel; });
        QVERIFY(runJob(job));
        QCOMPARE(job.snapshot().state, Job::State::Failed);
    }

    void extractConflictsAndSmartFolder()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("one")));
        writeFile(tmp.filePath("one/f.txt"), "new");
        writeFile(tmp.filePath("loose.txt"), "loose");
        const QString single = tmp.filePath("single.zip");
        const QString multi = tmp.filePath("multi.tar.xz");
        PackJob p1({tmp.filePath("one")}, single);
        PackJob p2({tmp.filePath("one"), tmp.filePath("loose.txt")}, multi);
        QVERIFY(runJob(p1));
        QVERIFY(runJob(p2));

        QVERIFY(QDir().mkdir(tmp.filePath("dest")));
        ExtractJob s1(single, {""}, tmp.filePath("dest"), true);  // one top folder: no extra level
        QVERIFY(runJob(s1));
        QCOMPARE(readFile(tmp.filePath("dest/one/f.txt")), QByteArray("new"));
        ExtractJob s2(multi, {""}, tmp.filePath("dest"), true);  // several: folder named after it
        QVERIFY(runJob(s2));
        QCOMPARE(readFile(tmp.filePath("dest/multi/loose.txt")), QByteArray("loose"));

        writeFile(tmp.filePath("dest/one/f.txt"), "edited");
        int prompts = 0;
        ExtractJob again(single, {"one/f.txt"}, tmp.filePath("dest/one"));
        again.setSyncPromptHandler([&](const PromptRequest& r) {
            ++prompts;
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Skip : Answer::Cancel;
        });
        QVERIFY(runJob(again));
        QCOMPARE(prompts, 1);
        QCOMPARE(readFile(tmp.filePath("dest/one/f.txt")), QByteArray("edited"));
    }

    void archiveWithoutFolderEntries()
    {
        const QString bsdtar = QStandardPaths::findExecutable("bsdtar");
        if (bsdtar.isEmpty())
            QSKIP("bsdtar not installed");
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/b")));
        writeFile(tmp.filePath("a/b/c.txt"), "c");
        // Only the file is stored; "a" and "a/b" exist implicitly.
        QCOMPARE(QProcess::execute(bsdtar, {"-C", tmp.path(), "-cf", tmp.filePath("t.tar"), "a/b/c.txt"}), 0);
        ListResult r = listLocation(tmp.filePath("t.tar") + "/a", {});
        QCOMPARE(r.error, 0);
        QCOMPARE(int(r.entries.size()), 1);
        QVERIFY(r.entries[0].isDir);
        QCOMPARE(r.entries[0].size, int64_t(1));
    }

    void archivePathSafety()
    {
        QCOMPARE(normalizeArchivePath("./a//b/"), QString("a/b"));
        QCOMPARE(normalizeArchivePath("/etc/passwd"), QString("etc/passwd"));
        QCOMPARE(normalizeArchivePath("a/../../x"), QString());
        QVERIFY(isArchiveName("x.tar.gz"));
        QVERIFY(isArchiveName("X.ZIP"));
        QVERIFY(!isArchiveName("doc.odt"));
        QVERIFY(!isArchiveName(".zip"));
        QCOMPARE(archiveStem("photos.tar.gz"), QString("photos"));
    }

    void renameTemplates()
    {
        const QList<RenameItem> items = {{"IMG_0042.JPG", false, 1700000000, 1600000000}, {"notes.tar.gz", false, 0, 0},
                                         {"Folder.v2", true, 0, 0}};
        auto names = [&](const RenameOptions& o) {
            const RenamePlan p = buildRenamePlan(items, "Trip", o, {});
            QStringList out;
            for (const auto& r : p.rows)
                out << r.newName;
            return p.error.isEmpty() ? out : QStringList{p.error};
        };
        auto with = [](const QString& pattern) {
            RenameOptions o;
            o.pattern = pattern;
            return o;
        };
        QCOMPARE(names({}), QStringList({"IMG_0042.JPG", "notes.tar.gz", "Folder.v2"}));  // unchanged
        QCOMPARE(names(with("{folder} {n:001}{ext}")), QStringList({"Trip 001.JPG", "Trip 002.tar.gz", "Trip 003"}));  // folders: no ext
        QCOMPARE(names(with("{n:10+5}")), QStringList({"10", "15", "20"}));
        QCOMPARE(names(with("{name[-4:]}_{name[0:3]|lower}{ext|lower}")).first(), QString("0042_img.jpg"));
        QCOMPARE(names(with("{name[2]}")).first(), QString("G"));
        QCOMPARE(names(with("{{{n}}}")).first(), QString("{1}"));
        QCOMPARE(names(with("{date:yyyyMMdd}-{name}")).first(),
                 QDateTime::fromSecsSinceEpoch(1700000000).toString("yyyyMMdd") + "-IMG_0042");
        QCOMPARE(names(with("{taken}")).first(), QDateTime::fromSecsSinceEpoch(1600000000).toString("yyyy-MM-dd"));
        QCOMPARE(names(with("{taken:yyyy}")).at(1), QDateTime::fromSecsSinceEpoch(0).toString("yyyy"));  // no EXIF: modified

        RenameOptions o;
        o.find = "img_(\\d+)";
        o.replace = "photo-$1";
        o.regex = true;
        QCOMPARE(names(o).first(), QString("photo-0042.JPG"));
        o.regex = false;
        o.find = ".";
        o.replace = "\\1";  // plain text: no regex magic
        QCOMPARE(names(o).at(1), QString("notes\\1tar\\1gz"));
        o = {};
        o.caseMode = RenameOptions::Case::Lower;
        QCOMPARE(names(o).first(), QString("img_0042.JPG"));  // the extension keeps its case
        o = with("my holiday's best{ext}");
        o.caseMode = RenameOptions::Case::Title;
        QCOMPARE(names(o).first(), QString("My Holiday's Best.JPG"));

        QCOMPARE(names(with("{nope}")), QStringList({"Unknown field {nope}"}));
        QCOMPARE(names(with("{name")), QStringList({"A { is not closed"}));
        QCOMPARE(names(with("{name|shout}")).first(), QString("Unknown filter |shout (lower, upper, title, trim)"));
    }

    void checksums()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("abc.txt"), "abc");
        QCOMPARE(fileChecksum(tmp.filePath("abc.txt"), QCryptographicHash::Sha256),
                 QString("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(fileChecksum(tmp.filePath("abc.txt"), QCryptographicHash::Md5), QString("900150983cd24fb0d6963f7d28e17f72"));
        QVERIFY(fileChecksum(tmp.filePath("missing"), QCryptographicHash::Md5).isEmpty());
        writeFile(tmp.filePath("SHA256SUMS"), "# made by hand\n"
                                              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc.txt\n"
                                              "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *dir/with space.bin\n");
        QString algo;
        QList<ChecksumLine> lines = readChecksumFile(tmp.filePath("SHA256SUMS"), &algo);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[1].name, QString("dir/with space.bin"));
        QCOMPARE(lines[1].hash, lines[0].hash);  // lower-cased
        QCOMPARE(algoForFile("SHA256SUMS")->name, QString("SHA-256"));
        QCOMPARE(algoForFile("x.iso.md5")->name, QString("MD5"));
        writeFile(tmp.filePath("bsd.txt"), "MD5 (abc.txt) = 900150983cd24fb0d6963f7d28e17f72\n");
        lines = readChecksumFile(tmp.filePath("bsd.txt"), &algo);
        QCOMPARE(lines.size(), 1);
        QCOMPARE(algo, QString("MD5"));
        QCOMPARE(algoForHexLength(40)->name, QString("SHA-1"));
        QCOMPARE(checksumLines({{"aa", "b"}}), QString("aa  b\n"));
    }

    void diskUsage()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("big/deeper")));
        QVERIFY(QDir().mkpath(tmp.filePath("small")));
        writeFile(tmp.filePath("big/deeper/a.bin"), QByteArray(2 << 20, 'a'));
        writeFile(tmp.filePath("big/b.bin"), QByteArray(1 << 20, 'b'));
        writeFile(tmp.filePath("small/c.txt"), "c");
        QVERIFY(::link(QFile::encodeName(tmp.filePath("big/b.bin")).constData(),
                       QFile::encodeName(tmp.filePath("small/b-again.bin")).constData()) == 0);
        auto root = measureUsage(tmp.path());
        QCOMPARE(root->files, int64_t(4));
        QCOMPARE(root->children.size(), size_t(2));
        UsageNode* big = root->children[0].get();  // biggest first
        QCOMPARE(big->name, QString("big"));
        QVERIFY(big->size >= (2 << 20));
        // b.bin has two names; whichever is found first counts, the other is 0.
        QVERIFY(root->size >= (3 << 20) && root->size < (4 << 20));
        QCOMPARE(big->children[0]->path(), tmp.filePath("big/deeper"));
        const int64_t before = root->size;
        UsageNode* a = big->children[0]->children[0].get();
        const int64_t aSize = a->size;
        removeUsageNode(a);
        QCOMPARE(root->size, before - aSize);
        QCOMPARE(root->files, int64_t(3));
    }

    void duplicates()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/deep")));
        QVERIFY(QDir().mkpath(tmp.filePath("b")));
        QByteArray big(300 * 1024, 'x');
        writeFile(tmp.filePath("a/photo.jpg"), big);
        writeFile(tmp.filePath("b/photo copy.jpg"), big);
        writeFile(tmp.filePath("a/deep/again.jpg"), big);
        QByteArray almost = big;
        almost[150 * 1024] = 'y';  // same size, start and end: only the full hash tells
        writeFile(tmp.filePath("b/almost.jpg"), almost);
        writeFile(tmp.filePath("a/small.txt"), "same");
        writeFile(tmp.filePath("b/small.txt"), "same");
        writeFile(tmp.filePath("a/empty1"), "");
        writeFile(tmp.filePath("b/empty2"), "");
        writeFile(tmp.filePath("b/.hidden"), "same");
        QVERIFY(::link(QFile::encodeName(tmp.filePath("a/photo.jpg")).constData(),
                       QFile::encodeName(tmp.filePath("a/hardlink.jpg")).constData()) == 0);  // one file, two names

        const QList<DuplicateGroup> g = findDuplicates({tmp.filePath("a"), tmp.filePath("b")}, {});
        QCOMPARE(g.size(), 2);
        QCOMPARE(g[0].size, int64_t(big.size()));  // biggest waste first
        QCOMPARE(g[0].files.size(), 3);            // not almost.jpg, the hard link counted once
        QCOMPARE(g[1].files.size(), 2);            // small.txt twice; empty files and .hidden left out
        DuplicateOptions withHidden;
        withHidden.hidden = true;
        QCOMPARE(findDuplicates({tmp.path()}, withHidden).at(1).files.size(), 3);
        DuplicateOptions bigOnly;
        bigOnly.minSize = 1024;
        QCOMPARE(findDuplicates({tmp.path(), tmp.filePath("a")}, bigOnly).size(), 1);  // a root inside another: once
    }

    void trash()
    {
#ifdef Q_OS_MACOS
        QSKIP("Freedesktop Trash browsing is Linux-only; native Trash round-trip is covered by undoRecords");
#endif
        QTemporaryDir tmp;
        const QByteArray oldData = qgetenv("XDG_DATA_HOME");  // never the real trash
        qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.filePath("data")));
        QVERIFY(QDir().mkpath(tmp.filePath("data")));
        auto restoreEnv = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });
        QVERIFY(QDir().mkpath(tmp.filePath("work/sub")));
        writeFile(tmp.filePath("work/note.txt"), "keep me");
        writeFile(tmp.filePath("work/sub/x.bin"), QByteArray(1000, 'x'));
        QString inTrash, dirInTrash;
        QVERIFY(QFile::moveToTrash(tmp.filePath("work/note.txt"), &inTrash));
        QVERIFY(QFile::moveToTrash(tmp.filePath("work/sub"), &dirInTrash));
        auto find = [&](const QString& original) {
            for (const TrashItem& it : listTrash())
                if (it.originalPath == original)
                    return it;
            return TrashItem{};
        };
        TrashItem note = find(tmp.filePath("work/note.txt"));
        QVERIFY2(!note.name.isEmpty(), qPrintable("not listed: " + inTrash + " roots " + trashRoots().join(',')));
        QCOMPARE(note.size, int64_t(7));
        QVERIFY(note.deletedAt > 0);
        TrashItem sub = find(tmp.filePath("work/sub"));
        QVERIFY(sub.isDir);
        QCOMPARE(sub.size, int64_t(1000));

        // Restore: back in place; a taken place gets a new name.
        writeFile(tmp.filePath("work/note.txt"), "new one");
        QVERIFY(restoreTrashItems({note}).isEmpty());
        QCOMPARE(readFile(tmp.filePath("work/note.txt")), QByteArray("new one"));
        const QStringList restored = QDir(tmp.filePath("work")).entryList({"note (*"});
        QCOMPARE(restored.size(), 1);
        QCOMPARE(readFile(tmp.filePath("work/" + restored.first())), QByteArray("keep me"));
        QVERIFY(!QFile::exists(note.infoPath()));
        // Delete for good.
        QVERIFY(deleteTrashItems({sub}).isEmpty());
        QVERIFY(!QFileInfo::exists(sub.filePath()));
        QVERIFY(find(tmp.filePath("work/sub")).name.isEmpty());
    }

    void links()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("a/b")));
        QVERIFY(QDir().mkpath(tmp.filePath("c")));
        writeFile(tmp.filePath("a/b/f.txt"), "data");
        QCOMPARE(makeLink(tmp.filePath("a/b/f.txt"), tmp.filePath("c/abs"), LinkKind::Symbolic), 0);
        QCOMPARE(QFile::symLinkTarget(tmp.filePath("c/abs")), tmp.filePath("a/b/f.txt"));
        QCOMPARE(makeLink(tmp.filePath("a/b/f.txt"), tmp.filePath("c/rel"), LinkKind::Relative), 0);
        char buf[256] = {};
        QVERIFY(::readlink(QFile::encodeName(tmp.filePath("c/rel")).constData(), buf, sizeof buf - 1) > 0);
        QCOMPARE(QString(buf), QString("../a/b/f.txt"));
        QCOMPARE(readFile(tmp.filePath("c/rel")), QByteArray("data"));
        QCOMPARE(makeLink(tmp.filePath("a/b/f.txt"), tmp.filePath("c/hard"), LinkKind::Hard), 0);
        QCOMPARE(readFile(tmp.filePath("c/hard")), QByteArray("data"));
        QCOMPARE(makeLink(tmp.filePath("a/b/f.txt"), tmp.filePath("c/abs"), LinkKind::Symbolic), EEXIST);
        // Undo removes the links, never the file.
        QVERIFY(removeIfStillEmpty({tmp.filePath("c/abs"), tmp.filePath("c/rel"), tmp.filePath("c/hard")}).isEmpty());
        QCOMPARE(QDir(tmp.filePath("c")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System), QStringList());
        QCOMPARE(readFile(tmp.filePath("a/b/f.txt")), QByteArray("data"));
        QVERIFY(!removeIfStillEmpty({tmp.filePath("a/b/f.txt")}).isEmpty());  // the only name: kept
    }

    void permissions()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("tree/sub")));
        writeFile(tmp.filePath("tree/a.txt"), "a");
        writeFile(tmp.filePath("tree/sub/run.sh"), "#!/bin/sh");
        QFile::setPermissions(tmp.filePath("tree/sub/run.sh"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        ::chmod(QFile::encodeName(tmp.filePath("tree/a.txt")).constData(), 0600);
        ::chmod(QFile::encodeName(tmp.filePath("tree/sub")).constData(), 0700);

        PermInfo i = readPermissions(tmp.filePath("tree/a.txt"));
        QCOMPARE(i.error, 0);
        QCOMPARE(i.mode, 0600u);
        QCOMPARE(symbolicMode(i.mode, false), QString("rw-------"));
        QCOMPARE(symbolicMode(02755, true), QString("rwxr-sr-x"));
        QCOMPARE(symbolicMode(01777, true), QString("rwxrwxrwt"));
        QVERIFY(!i.owner.isEmpty());

        // Bits: only what's mentioned changes.
        PermChange c;
        c.setBits = 0040;
        QCOMPARE(applyPermissions(tmp.filePath("tree/a.txt"), c), 0);
        QCOMPARE(readPermissions(tmp.filePath("tree/a.txt")).mode, 0640u);

        // A tree: group may read. Folders stay openable, files don't become programs,
        // the script keeps its own x.
        PermChange tree;
        tree.setBits = 0050;  // r-x for the group, like ticking both for the top folder
        tree.recursive = true;
        PermissionsJob job({tmp.filePath("tree")}, tree);
        QVERIFY(runJob(job));
        QVERIFY2(job.errors().isEmpty(), qPrintable(job.errors().join('\n')));
        QCOMPARE(readPermissions(tmp.filePath("tree")).mode & 0070, 0050u);
        QCOMPARE(readPermissions(tmp.filePath("tree/sub")).mode, 0750u);
        QCOMPARE(readPermissions(tmp.filePath("tree/a.txt")).mode, 0640u);
        QCOMPARE(readPermissions(tmp.filePath("tree/sub/run.sh")).mode, 0750u);

        // Extra people and groups, where the filesystem has ACLs.
        if (!i.aclSupported)
            QSKIP("no ACLs on this filesystem (or built without libacl)");
        PermChange acl;
        acl.aclSet << AclEntry{false, 65534, QString(), PermRead | PermWrite};
        QCOMPARE(applyPermissions(tmp.filePath("tree/a.txt"), acl), 0);
        i = readPermissions(tmp.filePath("tree/a.txt"));
        QCOMPARE(i.acl.size(), 1);
        QCOMPARE(i.acl[0].perms, uint32_t(PermRead | PermWrite));
        QCOMPARE(i.mode, 0640u);  // the group's own bits, not the mask ls shows
        // A plain chmod keeps the extra entries.
        PermChange more;
        more.setBits = 0004;
        QCOMPARE(applyPermissions(tmp.filePath("tree/a.txt"), more), 0);
        i = readPermissions(tmp.filePath("tree/a.txt"));
        QCOMPARE(i.mode, 0644u);
        QCOMPARE(i.acl.size(), 1);
        PermChange drop;
        drop.aclRemove << AclEntry{false, 65534, QString(), 0};
        QCOMPARE(applyPermissions(tmp.filePath("tree/a.txt"), drop), 0);
        QVERIFY(readPermissions(tmp.filePath("tree/a.txt")).acl.isEmpty());

        // Folder defaults: new items inherit the entry, shown with the rights that apply.
        PermChange def;
        def.defaultEnabled = true;
        def.defaultSet << AclEntry{true, ::getegid(), QString(), PermRead | PermWrite};
        QCOMPARE(applyPermissions(tmp.filePath("tree/sub"), def), 0);
        QVERIFY(readPermissions(tmp.filePath("tree/sub")).hasDefault);
        writeFile(tmp.filePath("tree/sub/new.txt"), "n");
        i = readPermissions(tmp.filePath("tree/sub/new.txt"));
        QCOMPARE(i.acl.size(), 1);
        QVERIFY(i.acl[0].isGroup);
        QVERIFY(i.acl[0].perms & PermRead);
        PermChange off;
        off.defaultEnabled = false;
        QCOMPARE(applyPermissions(tmp.filePath("tree/sub"), off), 0);
        QVERIFY(!readPermissions(tmp.filePath("tree/sub")).hasDefault);
    }

    void exifDate()
    {
        // A minimal JPEG: SOI, APP1 "Exif" with a little-endian TIFF holding
        // IFD0 -> Exif IFD -> DateTimeOriginal.
        QByteArray tiff;
        auto u16 = [&](int v) { tiff.append(char(v & 0xff)).append(char(v >> 8 & 0xff)); };
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) tiff.append(char(v >> (8 * i) & 0xff)); };
        tiff += "II";
        u16(42);
        u32(8);         // IFD0 at 8
        u16(1);         // one entry
        u16(0x8769); u16(4); u32(1); u32(26);  // Exif IFD at 26
        u32(0);
        u16(1);         // Exif IFD: one entry
        u16(0x9003); u16(2); u32(20); u32(44);  // DateTimeOriginal at 44
        u32(0);
        tiff += QByteArray("2021:06:15 14:30:05", 19) + '\0';
        QByteArray jpeg("\xFF\xD8", 2);
        const QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
        jpeg += QByteArray("\xFF\xE1", 2) + char((app1.size() + 2) >> 8) + char((app1.size() + 2) & 0xff) + app1;
        jpeg += QByteArray("\xFF\xDA\x00\x02", 4);
        QTemporaryDir tmp;
        writeFile(tmp.filePath("p.jpg"), jpeg);
        QCOMPARE(exifTakenTime(tmp.filePath("p.jpg")),
                 QDateTime::fromString("2021-06-15 14:30:05", "yyyy-MM-dd HH:mm:ss").toSecsSinceEpoch());
        writeFile(tmp.filePath("plain.txt"), "no exif here");
        QCOMPARE(exifTakenTime(tmp.filePath("plain.txt")), int64_t(0));
    }

    void renameConflictsAndSwap()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("a"), "A");
        writeFile(tmp.filePath("b"), "B");
        writeFile(tmp.filePath("c"), "C");
        writeFile(tmp.filePath("other"), "O");
        const QSet<QString> existing = {"a", "b", "c", "other"};

        // a -> other collides with a file that stays.
        RenameOptions o;
        o.find = "a";
        o.replace = "other";
        RenamePlan p = buildRenamePlan({{"a", false, 0}}, "", o, existing);
        QCOMPARE(p.problems, 1);
        QVERIFY(!p.canRun());
        // Two files to the same name.
        o.pattern = "x";
        o.find.clear();
        p = buildRenamePlan({{"a", false, 0}, {"b", false, 0}}, "", o, existing);
        QCOMPARE(p.problems, 2);

        // Rotate a->b, b->c, c->a: needs temporary names, must not lose data.
        RenamePlan rot;
        rot.rows = {{"a", "b", RenameRow::Status::Ok, {}}, {"b", "c", RenameRow::Status::Ok, {}},
                    {"c", "a", RenameRow::Status::Ok, {}}};
        QStringList errors;
        const auto done = executeRenamePlan(tmp.path(), rot, &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QCOMPARE(done.size(), 3);
        QCOMPARE(readFile(tmp.filePath("b")), QByteArray("A"));
        QCOMPARE(readFile(tmp.filePath("c")), QByteArray("B"));
        QCOMPARE(readFile(tmp.filePath("a")), QByteArray("C"));
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden).size(), 4);  // no temp leftovers

        // Undo = the reverse plan.
        RenamePlan undo;
        for (const auto& [now, before] : done)
            undo.rows.append({now, before, RenameRow::Status::Ok, {}});
        executeRenamePlan(tmp.path(), undo, &errors);
        QVERIFY(errors.isEmpty());
        QCOMPARE(readFile(tmp.filePath("a")), QByteArray("A"));
        QCOMPARE(readFile(tmp.filePath("c")), QByteArray("C"));
    }

    void queuePerDrive()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath("f1"), QByteArray(4096, '1'));
        writeFile(tmp.filePath("f2"), QByteArray(4096, '2'));
        writeFile(tmp.filePath("f3"), QByteArray(4096, '3'));
        QVERIFY(QDir().mkdir(tmp.filePath("out")));
        auto state = [](Job& j) { return j.snapshot().state; };

        CopyJob a(CopyJob::Mode::Copy, {tmp.filePath("f1")}, tmp.filePath("out"));
        a.setPaused(true);  // gets the drive, then sits paused
        QSignalSpy aDone(&a, &Job::finished);
        a.start();
        QTRY_VERIFY(state(a) != Job::State::Pending && state(a) != Job::State::Queued);

        CopyJob b(CopyJob::Mode::Copy, {tmp.filePath("f2")}, tmp.filePath("out"));
        QSignalSpy bDone(&b, &Job::finished);
        b.start();
        CopyJob c(CopyJob::Mode::Copy, {tmp.filePath("f3")}, tmp.filePath("out"));
        c.setPaused(true);
        QSignalSpy cDone(&c, &Job::finished);
        c.start();
        QTRY_COMPARE(state(b), Job::State::Queued);
        QTRY_COMPARE(state(c), Job::State::Queued);

        // A move inside the same filesystem is a rename: never queued.
        writeFile(tmp.filePath("m"), "m");
        CopyJob move(CopyJob::Mode::Move, {tmp.filePath("m")}, tmp.filePath("out"));
        QVERIFY(runJob(move));
        QVERIFY(QFile::exists(tmp.filePath("out/m")));

        // c jumps ahead of b; when a is done, c takes the drive (and pauses), b keeps waiting.
        c.runNext();
        a.setPaused(false);
        QVERIFY(aDone.wait(5000));
        QTRY_VERIFY(state(c) != Job::State::Queued);
        QTest::qWait(300);
        QCOMPARE(state(b), Job::State::Queued);

        // "Start now" lets b run alongside.
        b.startNow();
        QVERIFY(bDone.wait(5000));
        QCOMPARE(readFile(tmp.filePath("out/f2")), QByteArray(4096, '2'));
        c.setPaused(false);
        QVERIFY(cDone.wait(5000));
    }

    void queueDifferentDrivesRunInParallel()
    {
        QTemporaryDir tmp;  // usually tmpfs
        QTemporaryDir local(QDir::currentPath() + "/queue-test-XXXXXX");
        if (IoScheduler::deviceOf(tmp.path()) == IoScheduler::deviceOf(local.path()))
            QSKIP("temp and working directory are on the same filesystem");
        writeFile(tmp.filePath("a"), "a");
        writeFile(local.filePath("b"), "b");
        QVERIFY(QDir().mkdir(tmp.filePath("out")));
        QVERIFY(QDir().mkdir(local.filePath("out")));
        CopyJob first(CopyJob::Mode::Copy, {tmp.filePath("a")}, tmp.filePath("out"));
        first.setPaused(true);
        QSignalSpy firstDone(&first, &Job::finished);
        first.start();
        QTRY_VERIFY(first.snapshot().state != Job::State::Queued && first.snapshot().state != Job::State::Pending);
        CopyJob second(CopyJob::Mode::Copy, {local.filePath("b")}, local.filePath("out"));
        QVERIFY(runJob(second));  // not blocked by the paused job on the other drive
        first.setPaused(false);
        QVERIFY(firstDone.wait(5000));
    }

    void fuzzy()
    {
        QVERIFY(fuzzyScore(u"copy", u"Copy…") >= 0);
        QVERIFY(fuzzyScore(u"cfp", u"Copy full paths as text") >= 0);
        QCOMPARE(fuzzyScore(u"xyz", u"Copy"), -1);
        QVERIFY(fuzzyScore(u"copy", u"Copy…") > fuzzyScore(u"copy", u"Copy full paths as text"));
        QVERIFY(fuzzyScore(u"mr", u"Multi-rename") > fuzzyScore(u"mr", u"Mirror"));
        QVERIFY(fuzzyScore(u"code tan", u"~/Code/tandem-fm") >= 0);
        QCOMPARE(fuzzyScore(u"tan code", u"~/Code/tandem-fm"), -1);  // terms keep their order
        QList<int> pos;
        fuzzyScore(u"tf", u"tandem-fm", &pos);
        QCOMPARE(pos, QList<int>({0, 7}));  // word starts preferred over the first 'f'… there is only one
    }

    void folderHistory()
    {
        QTemporaryDir tmp;
        FolderHistory& h = FolderHistory::instance();
        h.setStoragePath(tmp.filePath("folders.tsv"));
        h.visit("/a");
        h.visit("/a");
        h.visit("/b");
        QList<FolderHistory::Item> items = h.items();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].path, QString("/a"));
        h.setPinned("/b", true);
        QCOMPARE(h.items().first().path, QString("/b"));
        QVERIFY(h.items().first().pinned);
        h.saveNow();
        h.setStoragePath(tmp.filePath("folders.tsv"));  // clears memory
        QVERIFY(h.items().isEmpty());
        h.loadAsync();
        QTRY_COMPARE(h.items().size(), 2);
        QVERIFY(h.isPinned("/b"));
        h.forget("/a");
        QCOMPARE(h.items().size(), 1);
        QVERIFY(FolderHistory::frecency(10, 0, 30) > FolderHistory::frecency(10, 0, 30 * 86400));

        // Notifications arrive after unlocking, and include plain favourites.
        QSignalSpy changed(&h, &FolderHistory::bookmarksChanged);
        bool unlocked = false;
        const auto connection = connect(&h, &FolderHistory::bookmarksChanged, &h, [&] {
            unlocked = !h.pinnedBookmarks().isEmpty();
        });
        h.setBookmark("/plain", {});
        QCOMPARE(changed.size(), 1);
        QVERIFY(unlocked);
        QCOMPARE(h.pinnedBookmarks().last().path, QString("/plain"));
        QVERIFY(h.pinnedBookmarks().last().key.isEmpty());
        disconnect(connection);

        // Bookmarks: unique keys, chains, persistence.
        h.setBookmark("/proj", "cp");
        h.setBookmark("/docs", "cd");
        QCOMPARE(h.pathForKey("cp"), QString("/proj"));
        h.setBookmark("/other", "cp");  // a used key moves
        QCOMPARE(h.pathForKey("cp"), QString("/other"));
        QCOMPARE(h.bookmarkKey("/proj"), QString());
        QVERIFY(h.isPinned("/proj"));   // still pinned, just without a key
        h.saveNow();
        h.setStoragePath(tmp.filePath("folders.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.pathForKey("cd"), QString("/docs"));
        QCOMPARE(h.bookmarks().size(), 2);
        QVERIFY(h.isPinned("/plain"));
        QVERIFY(h.bookmarkKey("/plain").isEmpty());
        h.setPinned("/docs", false);    // unpinning drops the key
        QCOMPARE(h.pathForKey("cd"), QString());

        // Old (v1) files still load; a missing file gets the defaults.
        writeFile(tmp.filePath("old.tsv"), "3\t100\t1\t/pinned old\n1\t50\t0\t/seen\n");
        h.setStoragePath(tmp.filePath("old.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.items().size(), 2);
        QVERIFY(h.isPinned("/pinned old"));
        h.setStoragePath(tmp.filePath("missing.tsv"));
        h.loadAsync();
        QTRY_COMPARE(h.pathForKey("h"), QDir::homePath());
        QCOMPARE(h.pathForKey("/"), QString("/"));
    }

    void undoRecords()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/d")));
        QVERIFY(QDir().mkpath(tmp.filePath("dst/d")));  // merge target: not undoable
        writeFile(tmp.filePath("src/f"), "f");
        writeFile(tmp.filePath("src/d/x"), "x");

        CopyJob copy(CopyJob::Mode::Copy, {tmp.filePath("src/f"), tmp.filePath("src/d")}, tmp.filePath("dst"));
        QVERIFY(runJob(copy));
        QCOMPARE(copy.undoRecords(), (QList<QPair<QString, QString>>{{tmp.filePath("dst/f"), QString()}}));

        QVERIFY(QDir().mkdir(tmp.filePath("moved")));
        CopyJob move(CopyJob::Mode::Move, {tmp.filePath("src/f")}, tmp.filePath("moved"));
        QVERIFY(runJob(move));
        QCOMPARE(move.undoRecords(),
                 (QList<QPair<QString, QString>>{{tmp.filePath("moved/f"), tmp.filePath("src/f")}}));

        // "Keep both" makes a new file: that one can be undone.
        writeFile(tmp.filePath("src/g"), "new");
        writeFile(tmp.filePath("dst/g"), "old");
        CopyJob both(CopyJob::Mode::Copy, {tmp.filePath("src/g")}, tmp.filePath("dst"));
        both.setSyncPromptHandler([](const PromptRequest&) { return Answer::Rename; });
        QVERIFY(runJob(both));
        QCOMPARE(both.undoRecords(), (QList<QPair<QString, QString>>{{tmp.filePath("dst/g (2)"), QString()}}));
        CopyJob moveBoth(CopyJob::Mode::Move, {tmp.filePath("src/g")}, tmp.filePath("dst"));
        moveBoth.setSyncPromptHandler([](const PromptRequest&) { return Answer::Rename; });
        QVERIFY(runJob(moveBoth));
        QCOMPARE(moveBoth.undoRecords(),
                 (QList<QPair<QString, QString>>{{tmp.filePath("dst/g (3)"), tmp.filePath("src/g")}}));

        // Trash and restore, inside a private XDG_DATA_HOME on the same filesystem.
        const QByteArray oldData = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.filePath("data")));
        QVERIFY(QDir().mkpath(tmp.filePath("data")));
        writeFile(tmp.filePath("trashme"), "t");
        DeleteJob trash({tmp.filePath("trashme")}, true);
        QVERIFY(runJob(trash));
        const auto recs = trash.undoRecords();
        QCOMPARE(recs.size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("trashme")));
        QVERIFY(QFile::exists(recs.first().first));
        QVERIFY(restoreFromTrash(recs).isEmpty());
        QCOMPARE(readFile(tmp.filePath("trashme")), QByteArray("t"));
        QVERIFY(!QFile::exists(recs.first().first));
        qputenv("XDG_DATA_HOME", oldData);

        // New folders go away only while empty.
        QVERIFY(QDir().mkpath(tmp.filePath("new/a/b")));
        QVERIFY(QDir().mkpath(tmp.filePath("full")));
        writeFile(tmp.filePath("full/keep"), "k");
        const QStringList errors = removeIfStillEmpty({tmp.filePath("new"), tmp.filePath("full")});
        QCOMPARE(errors.size(), 1);
        QVERIFY(!QFile::exists(tmp.filePath("new")));
        QVERIFY(QFile::exists(tmp.filePath("full/keep")));
    }

    void gitParse()
    {
        const char raw[] = "## main...origin/main [ahead 1]\0"
                           " M src/a.cpp\0"
                           "A  src/new.h\0"
                           "R  src/moved.cpp\0src/old.cpp\0"
                           "UU conflict.txt\0"
                           "?? notes.md\0"
                           "!! build/\0"
                           "!! src/gen/x.o\0"
                           "?? docs/draft/\0";
        const QByteArray out(raw, sizeof(raw) - 1);
        GitInfo root = parseGitStatus(out, "/r", "/r");
        QCOMPARE(root.branch, QString("main"));
        QCOMPARE(root.status.value("src"), 'M');      // strongest inside wins
        QCOMPARE(root.status.value("conflict.txt"), 'U');
        QCOMPARE(root.status.value("notes.md"), '?');
        QCOMPARE(root.status.value("build"), '!');
        QCOMPARE(root.status.value("docs"), '?');
        GitInfo src = parseGitStatus(out, "/r", "/r/src");
        QCOMPARE(src.status.value("a.cpp"), 'M');
        QCOMPARE(src.status.value("new.h"), 'A');
        QCOMPARE(src.status.value("moved.cpp"), 'M');
        QVERIFY(!src.status.contains("old.cpp"));
        QVERIFY(!src.status.contains("gen"));       // an ignored file doesn't mark its folder
        QCOMPARE(parseGitStatus("## No commits yet on dev\0", "/r", "/r").branch, QString("dev"));
    }

    void gitRealRepo()
    {
        const QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty())
            QSKIP("git not installed");
        QTemporaryDir tmp;
        auto run = [&](const QStringList& args) {
            QProcess p;
            p.setWorkingDirectory(tmp.path());
            p.start(git, QStringList{"-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=trunk"} + args);
            p.waitForFinished();
            return p.exitCode();
        };
        QCOMPARE(run({"init", "-q"}), 0);
        QVERIFY(QDir().mkpath(tmp.filePath("sub")));
        writeFile(tmp.filePath("tracked.txt"), "v1");
        writeFile(tmp.filePath("sub/inner.txt"), "v1");
        writeFile(tmp.filePath(".gitignore"), "*.log\n");
        QCOMPARE(run({"add", "."}), 0);
        QCOMPARE(run({"commit", "-q", "-m", "init"}), 0);
        writeFile(tmp.filePath("tracked.txt"), "v2");
        writeFile(tmp.filePath("sub/inner.txt"), "v2");
        writeFile(tmp.filePath("fresh.txt"), "new");
        writeFile(tmp.filePath("debug.log"), "x");
        writeFile(tmp.filePath("staged.txt"), "s");
        QCOMPARE(run({"add", "staged.txt"}), 0);

        const GitInfo info = gitStatusFor(tmp.path());
        QVERIFY(info.inRepo);
        QCOMPARE(info.branch, QString("trunk"));
        QCOMPARE(info.status.value("tracked.txt"), 'M');
        QCOMPARE(info.status.value("sub"), 'M');
        QCOMPARE(info.status.value("fresh.txt"), '?');
        QCOMPARE(info.status.value("debug.log"), '!');
        QCOMPARE(info.status.value("staged.txt"), 'A');
        QVERIFY(!info.status.contains(".gitignore"));
        const GitInfo sub = gitStatusFor(tmp.filePath("sub"));
        QCOMPARE(sub.status.value("inner.txt"), 'M');
        QVERIFY(!gitStatusFor(QDir::rootPath()).inRepo);
    }

    // The live filter: all words in the path, patterns on the name, smart case.
    void pathMatcher()
    {
        QVERIFY(PathMatcher("  ").isEmpty());
        QVERIFY(PathMatcher("ph 24 raw").matches("Photos/2024/x.raw"));
        QVERIFY(!PathMatcher("ph 25 raw").matches("Photos/2024/x.raw"));
        QVERIFY(PathMatcher("photos").matches("Photos/a.jpg"));
        QVERIFY(!PathMatcher("Photos").matches("photos/a.jpg"));  // capitals: exact case
        QVERIFY(PathMatcher("*.jpg").matches("Photos/a.JPG"));
        QVERIFY(!PathMatcher("*.jpg").matches("x.jpg/readme"));   // the name, not a folder on the way
        QVERIFY(PathMatcher("ph *.jpg").matches("Photos/a.jpg"));
        QVERIFY(!PathMatcher("ph *.jpg").matches("Music/a.jpg"));
    }

    void search()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("src/deep")));
        QVERIFY(QDir().mkpath(tmp.filePath(".hidden")));
        QVERIFY(QDir().mkpath(tmp.filePath("build")));
        writeFile(tmp.filePath("src/Fuzzy.cpp"), "int needle = 1;");
        writeFile(tmp.filePath("src/deep/fuzzy.h"), "// nothing");
        writeFile(tmp.filePath(".hidden/fuzzy.txt"), "needle");
        writeFile(tmp.filePath("build/fuzzy.o"), "needle");
        writeFile(tmp.filePath(".gitignore"), "build/\n");
        QVERIFY(QDir().mkdir(tmp.filePath(".git")));  // fd/rg honour .gitignore inside repos
        auto run = [&](SearchSpec s, int limit = 1000) {
            s.root = tmp.path();
            QStringList all;
            const QString err = runSearch(s, nullptr, limit, [&](const QStringList& b) { all << b; });
            all.sort();
            return err.isEmpty() ? all : QStringList{"ERROR " + err};
        };
        QCOMPARE(run({{}, "fuzzy", false, false}), QStringList({"src/Fuzzy.cpp", "src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "*.h", false, false}), QStringList({"src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "needle", true, false}), QStringList({"src/Fuzzy.cpp"}));
        QCOMPARE(run({{}, "needle", true, true}),
                 QStringList({".hidden/fuzzy.txt", "build/fuzzy.o", "src/Fuzzy.cpp"}));
        QCOMPARE(run({{}, "fuzzy", false, true}, 2).size(), 2);  // limit
        QCOMPARE(run({{}, "", false, false}), QStringList());

        QStringList tree;
        QVERIFY(listTree(tmp.path(), false, nullptr, 1000, [&](const QStringList& b) { tree << b; }).isEmpty());
        tree.sort();
        QCOMPARE(tree, QStringList({"src", "src/Fuzzy.cpp", "src/deep", "src/deep/fuzzy.h"}));

        // Without fd/rg/grep: names still work (built-in walk), contents explain why not.
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(tmp.filePath("no-tools")));
        QStringList names = run({{}, "fuzzy", false, false});
        // Hidden skipped; the fallback doesn't read .gitignore, so build/ shows up.
        QCOMPARE(names, QStringList({"build/fuzzy.o", "src/Fuzzy.cpp", "src/deep/fuzzy.h"}));
        QCOMPARE(run({{}, "fuzzy", false, true}).size(), 4);                // incl. hidden & build
        QVERIFY(run({{}, "needle", true, false}).first().startsWith("ERROR"));
        qputenv("PATH", oldPath);

        // A newline is allowed in names: still one result, not two.
        writeFile(tmp.filePath("src/line\nbreak.txt"), "needle");
        QCOMPARE(run({{}, "break", false, false}), QStringList({"src/line\nbreak.txt"}));
        QCOMPARE(run({{}, "needle", true, false}), QStringList({"src/Fuzzy.cpp", "src/line\nbreak.txt"}));
        // Same with grep when rg is missing.
        const QString grep = QStandardPaths::findExecutable("grep");
        if (!grep.isEmpty()) {
            QVERIFY(QDir().mkpath(tmp.filePath("only-grep")));
            QVERIFY(QFile::link(grep, tmp.filePath("only-grep/grep")));
            qputenv("PATH", QFile::encodeName(tmp.filePath("only-grep")));
            const QStringList found = run({{}, "needle", true, false});
            qputenv("PATH", oldPath);
            // grep doesn't read .gitignore, but must still skip hidden folders.
            QCOMPARE(found, QStringList({"build/fuzzy.o", "src/Fuzzy.cpp", "src/line\nbreak.txt"}));
        }
    }

    void savedServers()
    {
        SavedServer nas;
        nas.name = "NAS";
        nas.type = "smb";
        nas.host = "nas.lan";
        nas.user = "bob";
        nas.domain = "WORKGROUP";
        nas.folder = "media/";
        QCOMPARE(nas.uri(), QString("smb://WORKGROUP;bob@nas.lan/media"));
        SavedServer box;
        box.name = "Box \"one\"";
        box.host = "box";
        box.port = 2222;
        box.keyFile = "~/.ssh/box";
        QCOMPARE(box.uri(), QString("sftp://box:2222"));
        box.port = 22;
        QCOMPARE(box.authority(), QString("sftp://box"));  // the default port isn't written

        // The file round-trips; entries without a host are dropped.
        QList<SavedServer> back;
        QVERIFY(parseServersToml(serversToml({nas, box}), &back));
        QCOMPARE(back, (QList<SavedServer>{nas, box}));
        QVERIFY(parseServersToml("[[server]]\nname = \"x\"\n[[server]]\nhost = \"h\"\nfuture = 1\n", &back));
        QCOMPARE(back.size(), 1);
        QCOMPARE(back.first().name, QString("h"));
        QCOMPARE(back.first().type, QString("sftp"));
        QString error;
        QVERIFY(!parseServersToml("[[server]\n", &back, &error));
        QVERIFY(error.startsWith("line 1"));

        // Back from an address, domain and all.
        const SavedServer parsed = serverFromUri("smb://WORKGROUP;bob@nas.lan:4450/media");
        QCOMPARE(parsed.domain, QString("WORKGROUP"));
        QCOMPARE(parsed.user, QString("bob"));
        QCOMPARE(parsed.port, 4450);
        QCOMPARE(parsed.folder, QString("/media"));
        QCOMPARE(serverFromUri("sftp://bob@host:22/").port, 0);

        // Saved, read back by a fresh load, found for locations on that server.
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] {
            useConfigDir(QString());
            ServerList::instance().load();
        });
        ServerList& list = ServerList::instance();
        QVERIFY(list.save({nas, box}));
        QVERIFY(QFile::exists(tmp.filePath("tandem-fm/servers.toml")) || QFile::exists(list.path()));
        QVERIFY(list.load());
        QCOMPARE(list.servers().size(), 2);
        QCOMPARE(list.find("sftp://box/home/x")->name, box.name);
        QCOMPARE(list.find("sftp://box:22/home/x")->name, box.name);
        QVERIFY(!list.find("sftp://box:2222/"));
        QVERIFY(!list.find("sftp://alice@box/"));
        QCOMPARE(list.find(nas.uri() + "/deeper")->name, QString("NAS"));
        QVERIFY(!list.find("/home/box"));
    }

    void sshConfigImport()
    {
        QTemporaryDir tmp;
        QVERIFY(QDir().mkpath(tmp.filePath("conf.d")));
        writeFile(tmp.filePath("config"),
                  "# hosts\n"
                  "Include " + tmp.filePath("conf.d").toUtf8() + "/*.conf\n"
                  "Host web web2\n"
                  "    HostName example.org\n"
                  "    User deploy\n"
                  "    Port 2200\n"
                  "    IdentityFile ~/.ssh/deploy\n"
                  "Host web\n"
                  "    User other\n"   // the first value wins, like ssh
                  "Host *.lan !bad\n"
                  "    User nobody\n"
                  "Match host x\n"
                  "    User matched\n"
                  "Host plain\n"
                  "  Port=22\n"
                  "Host *\n"
                  "    User everyone\n");
        writeFile(tmp.filePath("conf.d/a.conf"), "Host \"quoted\"\n  HostName 10.0.0.1\n");
        const QList<SavedServer> hosts = readSshConfig(tmp.filePath("config"));
        QStringList names;
        for (const SavedServer& h : hosts)
            names << h.name;
        QCOMPARE(names, QStringList({"quoted", "web", "web2", "plain"}));
        QCOMPARE(hosts[0].host, QString("10.0.0.1"));
        QCOMPARE(hosts[1].host, QString("example.org"));
        QCOMPARE(hosts[1].user, QString("deploy"));
        QCOMPARE(hosts[1].port, 2200);
        QCOMPARE(hosts[1].keyFile, QString("~/.ssh/deploy"));
        QCOMPARE(hosts[2].user, QString("deploy"));
        QCOMPARE(hosts[3].host, QString("plain"));
        QCOMPARE(hosts[3].port, 0);
        QVERIFY(hosts[3].user.isEmpty());  // Host * defaults stay ssh's business
        QVERIFY(readSshConfig(tmp.filePath("missing")).isEmpty());
    }

    void folderCompare()
    {
        QTemporaryDir tmp;
        const QString L = tmp.filePath("L"), R = tmp.filePath("R");
        auto at = [](const QString& p, qint64 secs) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime));
        };
        QVERIFY(QDir().mkpath(L + "/sub/deep"));
        QVERIFY(QDir().mkpath(R + "/sub"));
        QVERIFY(QDir().mkpath(L + "/onlyleft"));
        writeFile(L + "/same.txt", "same");          writeFile(R + "/same.txt", "same");
        writeFile(L + "/newer.txt", "new!");         writeFile(R + "/newer.txt", "old");
        writeFile(L + "/older.txt", "old");          writeFile(R + "/older.txt", "new!");
        writeFile(L + "/sametime.txt", "aaaa");      writeFile(R + "/sametime.txt", "bbbb");
        writeFile(L + "/sub/deep/x.txt", "x");
        writeFile(L + "/onlyleft/y.txt", "y");
        writeFile(R + "/sub/r.txt", "r");
        writeFile(L + "/clash", "file");             QVERIFY(QDir().mkpath(R + "/clash"));
        at(L + "/same.txt", 1000);     at(R + "/same.txt", 1001);  // within tolerance
        at(L + "/newer.txt", 5000);    at(R + "/newer.txt", 2000);
        at(L + "/older.txt", 2000);    at(R + "/older.txt", 5000);
        at(L + "/sametime.txt", 3000); at(R + "/sametime.txt", 3000);

        auto diffs = [](const CompareResult& r) {
            QMap<QString, Diff> m;
            for (const CompareItem& i : r.items)
                m[i.rel] = i.diff;
            return m;
        };
        CompareResult res = compareFolders(L, R, {});
        QVERIFY(res.error.isEmpty());
        auto d = diffs(res);
        QCOMPARE(d.value("same.txt"), Diff::Equal);
        QCOMPARE(d.value("newer.txt"), Diff::LeftNewer);
        QCOMPARE(d.value("older.txt"), Diff::RightNewer);
        QCOMPARE(d.value("sametime.txt"), Diff::Equal);  // same size and time: trusted
        QCOMPARE(d.value("sub/deep"), Diff::LeftOnly);    // one-sided folder = one item
        QCOMPARE(d.value("onlyleft"), Diff::LeftOnly);
        QCOMPARE(d.value("sub/r.txt"), Diff::RightOnly);
        QCOMPARE(d.value("clash"), Diff::TypeMismatch);
        QVERIFY(!d.contains("sub"));  // folders on both sides only show their contents

        CompareOptions byContent;
        byContent.contents = true;
        QCOMPARE(diffs(compareFolders(L, R, byContent)).value("sametime.txt"), Diff::Differs);
        CompareOptions flat;
        flat.recursive = false;
        QVERIFY(!diffs(compareFolders(L, R, flat)).contains("sub/r.txt"));
        QVERIFY(!compareFolders(L, tmp.filePath("missing"), {}).error.isEmpty());

        // Two-way sync: newer wins, one-sided items are copied over.
        std::vector<SyncAction> actions;
        for (const CompareItem& i : res.items)
            actions.push_back(defaultAction(i.diff, SyncMode::TwoWay));
        const SyncPlan plan = buildSyncPlan(L, R, res.items, actions);
        QVERIFY(plan.deletions.isEmpty());
        CopyJob job(plan.copies);
        job.setOverwriteExisting();
        QVERIFY(runJob(job));
        QCOMPARE(readFile(R + "/newer.txt"), QByteArray("new!"));
        QCOMPARE(readFile(L + "/older.txt"), QByteArray("new!"));
        QCOMPARE(readFile(R + "/sub/deep/x.txt"), QByteArray("x"));
        QCOMPARE(readFile(L + "/sub/r.txt"), QByteArray("r"));
        QCOMPARE(readFile(R + "/onlyleft/y.txt"), QByteArray("y"));
        QCOMPARE(readFile(L + "/clash"), QByteArray("file"));  // mismatches are left alone
        QCOMPARE(readFile(L + "/sametime.txt"), QByteArray("aaaa"));
        auto after = diffs(compareFolders(L, R, {}));
        for (auto it = after.cbegin(); it != after.cend(); ++it)
            QVERIFY2(it.value() == Diff::Equal || it.key() == "clash", qPrintable(it.key()));

        // Mirror: the right side ends up like the left, extra files deleted.
        writeFile(R + "/extra.txt", "extra");
        CompareResult res2 = compareFolders(L, R, {});
        actions.clear();
        for (const CompareItem& i : res2.items)
            actions.push_back(defaultAction(i.diff, SyncMode::MirrorToRight));
        const SyncPlan mirror = buildSyncPlan(L, R, res2.items, actions);
        QCOMPARE(mirror.deletions, QStringList({R + "/extra.txt"}));
        QVERIFY(mirror.copies.isEmpty());
    }

    void remotePaths()
    {
        QCOMPARE(describeGvfsName("sftp:host=example.com,user=bob").label, QString("sftp://bob@example.com"));
        QCOMPARE(describeGvfsName("sftp:host=example.com,port=2222").label, QString("sftp://example.com:2222"));
        QCOMPARE(describeGvfsName("smb-share:server=nas,share=media,user=me").label, QString("smb://me@nas/media"));
        QCOMPARE(describeGvfsName("dav:host=cloud.org,ssl=true,prefix=%2Fremote.php%2Fdav").label,
                 QString("davs://cloud.org/remote.php/dav"));
        QCOMPARE(describeGvfsName("ftp:host=ftp.gnu.org").scheme, QString("ftp"));
        QCOMPARE(describeGvfsName("localtest:").label, QString("localtest"));

        const QString root = gvfsDirectory() + "/sftp:host=h,user=u";
        QVERIFY(isRemotePath(root + "/srv/x"));
        QVERIFY(!isRemotePath("/home/u"));
        // GVfs sftp/smb folders open natively (when built in).
        registerBuiltinBackends();
        if (hasBackend("sftp"))
            QCOMPARE(nativeLocationFor(root + "/srv/x"), QString("sftp://u@h/srv/x"));
        if (hasBackend("smb"))
            QCOMPARE(nativeLocationFor(gvfsDirectory() + "/smb-share:server=nas,share=media/a b"), QString("smb://nas/media/a b"));
        QCOMPARE(nativeLocationFor(gvfsDirectory() + "/dav:host=x/y"), QString());
        QCOMPARE(nativeLocationFor("/home/u"), QString());
        QCOMPARE(remoteRootOf(root + "/srv/x")->root, root);
        QCOMPARE(remoteRootOf(root)->label, QString("sftp://u@h"));
        QVERIFY(!remoteRootOf(gvfsDirectory()));
        // Each GVfs mount gets its own transfer queue, unlike the shared FUSE device.
        const QString other = gvfsDirectory() + "/smb-share:server=nas,share=x";
        QCOMPARE(IoScheduler::deviceOf(root + "/a"), IoScheduler::deviceOf(root + "/b/c"));
        QVERIFY(IoScheduler::deviceOf(root + "/a") != IoScheduler::deviceOf(other + "/a"));

        const QByteArray realUser = qgetenv("USER");
        qputenv("USER", "me");
        QCOMPARE(normalizeServerUri("sftp://h/srv"), QString("sftp://h/srv"));
        QCOMPARE(normalizeServerUri("ssh://u@h"), QString("sftp://u@h"));
        QCOMPARE(normalizeServerUri("https://cloud/dav"), QString("davs://cloud/dav"));
        QCOMPARE(normalizeServerUri("u@h"), QString("sftp://u@h"));
        QCOMPARE(normalizeServerUri("u@h:/etc"), QString("sftp://u@h/etc"));
        QCOMPARE(normalizeServerUri("u@h:code/x"), QString("sftp://u@h/home/u/code/x"));
        QCOMPARE(normalizeServerUri("h:~"), QString("sftp://h/home/me"));
        QCOMPARE(normalizeServerUri("/home/me"), QString());
        QCOMPARE(normalizeServerUri("~/x"), QString());
        QCOMPARE(normalizeServerUri("file:///x"), QString());
        QCOMPARE(normalizeServerUri("plainname"), QString());
        QCOMPARE(normalizeServerUri("my file@2x.png"), QString());
        qputenv("USER", realUser);
    }

    void locationPaths()
    {
        QVERIFY(isRemoteLocation("sftp://bob@host/srv"));
        QVERIFY(!isRemoteLocation("/home/x"));
        QVERIFY(!isRemoteLocation("relative/x"));
        QCOMPARE(parentPath("sftp://bob@host/srv/www"), QString("sftp://bob@host/srv"));
        QCOMPARE(parentPath("sftp://bob@host/srv"), QString("sftp://bob@host/"));
        QCOMPARE(parentPath("sftp://bob@host/"), QString("sftp://bob@host/"));
        QCOMPARE(baseName("sftp://bob@host/srv/www"), QString("www"));
        QCOMPARE(baseName("sftp://bob@host/"), QString("bob@host"));
        QCOMPARE(joinPath(QString("sftp://h/"), QString("x")), QString("sftp://h/x"));
        QCOMPARE(normalizeLocation("SFTP://h"), QString("sftp://h/"));
        QCOMPARE(normalizeLocation("sftp://h//a/./b/../c/"), QString("sftp://h/a/c"));
        QCOMPARE(expandUserPath("sftp://h/a/../b"), QString("sftp://h/b"));
        QCOMPARE(locationPath("sftp://h/a/b"), QString("/a/b"));
        QCOMPARE(locationAuthority("sftp://u@h:22/a"), QString("sftp://u@h:22"));
    }

    void backendJobs()
    {
        QTemporaryDir tmp;
        const QString local = tmp.filePath("local"), server = tmp.filePath("server");
        QVERIFY(QDir().mkpath(local + "/tree/sub"));
        QVERIFY(QDir().mkpath(server + "/existing"));
        writeFile(local + "/tree/a.txt", "alpha");
        writeFile(local + "/tree/sub/b.bin", QByteArray(3 << 20, 'b'));  // more than one buffer
        writeFile(local + "/single.txt", "single");
        writeFile(server + "/remote.txt", "from server");
        writeFile(server + "/existing/clash.txt", "old");
        QVERIFY(QFile::link("a.txt", local + "/tree/link"));
        auto backend = std::make_shared<TestBackend>(server);
        registerBackend("test", backend);
        const QString S = "test://srv";

        // Listing goes through the backend.
        ListOptions opt;
        ListResult l = listLocation(S + "/", opt);
        QVERIFY(l.remote);
        QCOMPARE(l.error, 0);
        QCOMPARE(l.entries.size(), size_t(2));
        QCOMPARE(l.entries[0].name, QString("existing"));  // sorted, folders first
        QCOMPARE(listLocation(S + "/missing", opt).error, ENOENT);
        QCOMPARE(listLocation("nosuch://x/", opt).error, EPROTONOSUPPORT);

        // Local → server, folders recursively, links kept.
        CopyJob up(CopyJob::Mode::Copy, {local + "/tree", local + "/single.txt"}, S + "/up");
        QVERIFY(runJob(up));
        QVERIFY2(up.errors().isEmpty(), qPrintable(up.errors().join('\n')));
        QCOMPARE(readFile(server + "/up/tree/a.txt"), QByteArray("alpha"));
        QCOMPARE(readFile(server + "/up/tree/sub/b.bin").size(), 3 << 20);
        QCOMPARE(readFile(server + "/up/single.txt"), QByteArray("single"));
        QCOMPARE(QFileInfo(server + "/up/tree/link").symLinkTarget(), server + "/up/tree/a.txt");
        QCOMPARE(QFileInfo(server + "/up/tree/a.txt").lastModified().toSecsSinceEpoch(),
                 QFileInfo(local + "/tree/a.txt").lastModified().toSecsSinceEpoch());  // servers keep seconds
        QCOMPARE(up.snapshot().bytesDone, int64_t(5 + 6 + (3 << 20)));

        // Server → local.
        CopyJob down(CopyJob::Mode::Copy, {S + "/remote.txt"}, local);
        QVERIFY(runJob(down));
        QCOMPARE(readFile(local + "/remote.txt"), QByteArray("from server"));
        QCOMPARE(down.undoRecords().size(), 1);  // local results can be undone

        // Conflict: the usual question; "rename" picks a free name on the server.
        writeFile(local + "/clash.txt", "new");
        CopyJob clash(CopyJob::Mode::Copy, {local + "/clash.txt"}, S + "/existing");
        clash.setSyncPromptHandler([](const PromptRequest& r) {
            return r.kind == PromptRequest::Kind::Conflict ? Answer::Rename : Answer::Cancel;
        });
        QVERIFY(runJob(clash));
        QCOMPARE(readFile(server + "/existing/clash.txt"), QByteArray("old"));
        QCOMPARE(readFile(server + "/existing/clash (2).txt"), QByteArray("new"));
        CopyJob over(CopyJob::Mode::Copy, {local + "/clash.txt"}, S + "/existing");
        over.setOverwriteExisting();
        QVERIFY(runJob(over));
        QCOMPARE(readFile(server + "/existing/clash.txt"), QByteArray("new"));
        const auto leftovers = [&] {
            return QDir(server + "/existing").entryList({".clash.txt.tandem-*"}, QDir::Files | QDir::Hidden);
        };
        QVERIFY(leftovers().isEmpty());

        // An overwrite that doesn't finish leaves the old file on the server.
        writeFile(local + "/clash.txt", QByteArray(1 << 20, 'x'));
        CopyJob cut(CopyJob::Mode::Copy, {local + "/clash.txt"}, S + "/existing");
        cut.setSyncPromptHandler([&](const PromptRequest& r) {
            if (r.kind != PromptRequest::Kind::Conflict)
                return Answer::Cancel;
            cut.setPaused(true);
            return Answer::Overwrite;
        });
        QSignalSpy cutDone(&cut, &Job::finished);
        cut.start();
        QTRY_VERIFY(cut.snapshot().paused);
        cut.cancel();
        QVERIFY(cutDone.wait(5000));
        QCOMPARE(readFile(server + "/existing/clash.txt"), QByteArray("new"));
        QVERIFY(leftovers().isEmpty());

        // Move on the server: a rename, no data copied.
        const int before = backend->calls;
        CopyJob mv(CopyJob::Mode::Move, {S + "/up/tree"}, S + "/moved");
        QVERIFY(runJob(mv));
        QVERIFY(QFile::exists(server + "/moved/sub/b.bin"));
        QVERIFY(!QFile::exists(server + "/up/tree"));
        QVERIFY(backend->calls - before < 10);
        // Move server → local: copied, then removed there.
        CopyJob mvDown(CopyJob::Mode::Move, {S + "/moved"}, local + "/back");
        QVERIFY(runJob(mvDown));
        QCOMPARE(readFile(local + "/back/a.txt"), QByteArray("alpha"));
        QVERIFY(!QFile::exists(server + "/moved"));

        // Cancelling mid-file leaves no half file behind.
        backend->latencyMs = 30;
        writeFile(local + "/big.bin", QByteArray(40 << 20, 'x'));
        CopyJob slow(CopyJob::Mode::Copy, {local + "/big.bin"}, S + "/");
        slow.start();
        QTRY_VERIFY(slow.snapshot().state == Job::State::Running);
        slow.cancel();
        QTRY_VERIFY(slow.isFinished());
        backend->latencyMs = 0;
        QVERIFY(!QFile::exists(server + "/big.bin"));

        // Delete on the server: permanent, recursive.
        DeleteJob del({S + "/up", S + "/existing/clash (2).txt"}, true);
        QVERIFY(runJob(del));
        QVERIFY(!QFile::exists(server + "/up"));
        QVERIFY(!QFile::exists(server + "/existing/clash (2).txt"));

        // Helpers.
        QCOMPARE(makeDirectoriesAt(S + "/a/b/c"), 0);
        QVERIFY(QFileInfo(server + "/a/b/c").isDir());
        QCOMPARE(makeDirectoriesAt(S + "/a/b"), EEXIST);
        QCOMPARE(createEmptyFile(S + "/a/new.txt"), 0);
        QCOMPARE(createEmptyFile(S + "/a/new.txt"), EEXIST);
        QCOMPARE(renameNoReplaceAt(S + "/a/new.txt", S + "/a/renamed.txt"), 0);
        QCOMPARE(renameNoReplaceAt(S + "/a/renamed.txt", local + "/x"), EXDEV);
        QCOMPARE(directorySizeAt(S + "/existing", nullptr), int64_t(3));
        QCOMPARE(downloadFile(S + "/remote.txt", tmp.filePath("cache/r.txt"), nullptr), 0);
        QCOMPARE(readFile(tmp.filePath("cache/r.txt")), QByteArray("from server"));

        // Compare and sync work across local and remote.
        CompareOptions co;
        co.contents = true;
        CompareResult cmp = compareFolders(local + "/tree", S + "/cmp", co);
        QVERIFY(!cmp.error.isEmpty());  // missing on the server
        QVERIFY(QDir().mkpath(server + "/cmp"));
        cmp = compareFolders(local + "/tree", S + "/cmp", co);
        QVERIFY(cmp.error.isEmpty());
        std::vector<SyncAction> actions;
        for (const CompareItem& i : cmp.items)
            actions.push_back(defaultAction(i.diff, SyncMode::MirrorToRight));
        CopyJob sync(buildSyncPlan(local + "/tree", S + "/cmp", cmp.items, actions).copies);
        sync.setOverwriteExisting();
        QVERIFY(runJob(sync));
        for (const CompareItem& i : compareFolders(local + "/tree", S + "/cmp", co).items)
            QVERIFY2(i.diff == Diff::Equal, qPrintable(i.rel));
        QVERIFY(*sameContents(local + "/tree/sub/b.bin", S + "/cmp/sub/b.bin"));
        writeFile(server + "/cmp/sub/b.bin", QByteArray(3 << 20, 'c'));
        QVERIFY(!*sameContents(local + "/tree/sub/b.bin", S + "/cmp/sub/b.bin"));

        registerBackend("test", nullptr);
    }

    void sftpBackend()
    {
        registerBuiltinBackends();
        if (!hasBackend("sftp"))
            QSKIP("built without libssh");
        SshServer server;
        if (!server.start())
            QSKIP("could not start a test sshd");
        QAtomicInt hostKeyQuestions = 0, otherQuestions = 0;
        setAuthHandler([&](const AuthQuestion& q) -> std::optional<QString> {
            if (q.kind == AuthQuestion::Kind::HostKey) {
                ++hostKeyQuestions;
                return QString("yes");
            }
            ++otherQuestions;
            return std::nullopt;
        });
        QTemporaryDir tmp;
        const QString base = tmp.path();  // the "server" is this machine
        QVERIFY(QDir().mkpath(base + "/srv/sub"));
        writeFile(base + "/srv/a.txt", "alpha");
        writeFile(base + "/srv/.hidden", "h");
        QVERIFY(QFile::link("a.txt", base + "/srv/link"));
        QVERIFY(QFile::link(base + "/srv/sub", base + "/srv/dirlink"));
        const QString S = server.location(base + "/srv");

        // Listing: first contact asks about the host key, once.
        ListOptions opt;
        ListResult l = listLocation(S, opt);
        QVERIFY2(l.error == 0, qPrintable(l.errorText + " / " + server.log()));
        QCOMPARE(int(hostKeyQuestions), 1);
        QCOMPARE(int(otherQuestions), 0);  // key login, no password
        QStringList names;
        for (const FileEntry& e : l.entries)
            names << e.name;
        QCOMPARE(names, QStringList({"dirlink", "sub", "a.txt", "link"}));  // folders (and links to them) first
        QVERIFY(l.entries[0].isLink && l.entries[0].isDir);
        QCOMPARE(l.entries[2].size, int64_t(5));
        QCOMPARE(l.entries[2].mtime, QFileInfo(base + "/srv/a.txt").lastModified().toSecsSinceEpoch());
        QVERIFY(l.freeBytes > 0);
        opt.showHidden = true;
        QCOMPARE(listLocation(S, opt).entries.size(), size_t(5));
        QCOMPARE(listLocation(S + "/missing", opt).error, ENOENT);
        QCOMPARE(int(hostKeyQuestions), 1);  // remembered in known_hosts

        // Overwriting a file on the server swaps in a complete copy.
        writeFile(base + "/newer.txt", "newer");
        CopyJob over(CopyJob::Mode::Copy, {base + "/newer.txt"}, S + "/a.txt");
        over.setOverwriteExisting();
        QVERIFY(runJob(over));
        QVERIFY2(over.errors().isEmpty(), qPrintable(over.errors().join('\n')));
        QCOMPARE(readFile(base + "/srv/a.txt"), QByteArray("newer"));
        QVERIFY(QDir(base + "/srv").entryList({".a.txt.tandem-*"}, QDir::Files | QDir::Hidden).isEmpty());
        writeFile(base + "/srv/a.txt", "alpha");

        Backend* b = backendFor(S);
        QCOMPARE(b->homeLocation(S), server.location(QDir::homePath()));

#ifndef Q_OS_MACOS
        // The following fast-path and permissions assertions require a GNU
        // userspace on the test server. A local macOS sshd uses BSD tools.
        // Whole trees in one request (find over an exec channel), same as walking.
        QVERIFY(QDir().mkpath(base + "/srv/sub/deep/er"));
        writeFile(base + "/srv/sub/deep/with space.txt", "12345678");
        Backend::Tree tree;
        QVERIFY(b->listTree(S, false, tree, nullptr));
        auto namesIn = [&](const QString& rel) {
            QStringList n;
            for (const FileEntry& e : tree.value(rel))
                n << e.name + (e.isLink ? "@" : "") + (e.isDir ? "/" : "");
            n.sort();
            return n;
        };
        QCOMPARE(namesIn(""), QStringList({"a.txt", "dirlink@/", "link@", "sub/"}));  // no .hidden
        QCOMPARE(namesIn("sub/deep"), QStringList({"er/", "with space.txt"}));
        QVERIFY(tree.contains("sub/deep/er"));  // empty folders are there too
        for (const FileEntry& e : tree.value("sub/deep"))
            if (!e.isDir)
                QCOMPARE(e.size, int64_t(8));
        QVERIFY(b->listTree(S, true, tree, nullptr));
        QVERIFY(namesIn("").contains(".hidden"));
        QCOMPARE(directorySizeAt(S + "/sub", nullptr), int64_t(8));
        // A comparison against an identical local copy finds nothing.
        CompareResult same = compareFolders(base + "/srv/sub", S + "/sub", {});
        QVERIFY(same.error.isEmpty());
        for (const CompareItem& it : same.items)
            QVERIFY2(it.diff == Diff::Equal, qPrintable(it.rel));
        // Permissions as the server sees them, changed with its own tools.
        writeFile(base + "/srv/perm.txt", "p");
        ::chmod(QFile::encodeName(base + "/srv/perm.txt").constData(), 0600);
        PermInfo remote;
        QVERIFY(b->readPermissions(S + "/perm.txt", remote));
        QCOMPARE(remote.mode, 0600u);
        QCOMPARE(remote.owner, readPermissions(base + "/srv/perm.txt").owner);
        PermChange pc;
        pc.setBits = 0040;
        if (remote.aclSupported && readPermissions(base + "/srv/perm.txt").aclSupported)
            pc.aclSet << AclEntry{false, 65534, QString(), PermRead | PermWrite};
        QString what;
        QCOMPARE(b->applyPermissions(S + "/perm.txt", pc, &what), 0);
        const PermInfo after = readPermissions(base + "/srv/perm.txt");
        QCOMPARE(after.mode, 0640u);
        if (!pc.aclSet.isEmpty()) {
            QCOMPARE(after.acl.size(), 1);
            QVERIFY(b->readPermissions(S + "/perm.txt", remote));
            QCOMPARE(remote.acl.size(), 1);
            QCOMPARE(remote.acl[0].id, 65534u);
            QCOMPARE(remote.mode, 0640u);  // the group's bits, not the mask
        }
        QList<QPair<QString, uint32_t>> users, groups;
        QVERIFY(b->listAccounts(S, users, groups));
        QVERIFY(std::any_of(users.begin(), users.end(), [](const auto& u) { return u.first == qEnvironmentVariable("USER"); }));

#else
        QVERIFY(QDir().mkpath(base + "/srv/sub/deep/er"));
        writeFile(base + "/srv/sub/deep/with space.txt", "12345678");
        Backend::Tree tree;
        const CompareResult same = compareFolders(base + "/srv/sub", S + "/sub", {});
        QVERIFY(same.error.isEmpty());
        QVERIFY(!b->listTree(S, false, tree, nullptr));
        QCOMPARE(directorySizeAt(S + "/sub", nullptr), int64_t(8));
#endif
        qputenv("TANDEM_SFTP_NO_EXEC", "1");  // servers without a shell: walked folder by folder
        QVERIFY(!b->listTree(S, false, tree, nullptr));
        QCOMPARE(directorySizeAt(S + "/sub", nullptr), int64_t(8));
        QCOMPARE(compareFolders(base + "/srv/sub", S + "/sub", {}).items.size(), same.items.size());
        qunsetenv("TANDEM_SFTP_NO_EXEC");
        QVERIFY(QDir(base + "/srv/sub/deep").removeRecursively());
        FileStat st;
        QCOMPARE(b->stat(S + "/link", st), 0);
        QVERIFY(st.isLink);

        // Transfers both ways, bigger than the in-flight window.
        QByteArray big(9 * 1024 * 1024 + 123, Qt::Uninitialized);
        for (int i = 0; i < big.size(); ++i)
            big[i] = char((i * 7919) >> 3);
        writeFile(base + "/big.bin", big);
        QElapsedTimer t;
        t.start();
        CopyJob up(CopyJob::Mode::Copy, {base + "/big.bin", base + "/srv/sub"}, S + "/up");
        QVERIFY(runJob(up));
        QVERIFY2(up.errors().isEmpty(), qPrintable(up.errors().join('\n')));
        const qint64 upMs = t.restart();
        QCOMPARE(readFile(base + "/srv/up/big.bin"), big);
        QVERIFY(QFileInfo(base + "/srv/up/sub").isDir());
        QCOMPARE(QFileInfo(base + "/srv/up/big.bin").lastModified().toSecsSinceEpoch(),
                 QFileInfo(base + "/big.bin").lastModified().toSecsSinceEpoch());
        CopyJob down(CopyJob::Mode::Copy, {S + "/up/big.bin"}, base + "/down.bin");
        QVERIFY(runJob(down));
        const qint64 downMs = t.elapsed();
        QCOMPARE(readFile(base + "/down.bin"), big);
        QVERIFY(*sameContents(base + "/big.bin", S + "/up/big.bin"));
        qInfo("sftp to localhost: 9 MiB up %lld ms, down %lld ms", upMs, downMs);

        // Big files go over several connections at once ("lanes"), both ways.
        QByteArray huge(40 * 1024 * 1024 + 4321, Qt::Uninitialized);
        for (int i = 0; i < huge.size(); i += 4)
            *reinterpret_cast<quint32*>(huge.data() + qMin(i, int(huge.size()) - 4)) = quint32(i * 2654435761u);
        writeFile(base + "/huge.bin", huge);
        CopyJob hugeUp(CopyJob::Mode::Copy, {base + "/huge.bin"}, S + "/huge-up.bin");
        QVERIFY(runJob(hugeUp));
        QVERIFY2(hugeUp.errors().isEmpty(), qPrintable(hugeUp.errors().join('\n')));
        QVERIFY(readFile(base + "/srv/huge-up.bin") == huge);
        CopyJob hugeDown(CopyJob::Mode::Copy, {S + "/huge-up.bin"}, base + "/huge-down.bin");
        QVERIFY(runJob(hugeDown));
        QVERIFY(readFile(base + "/huge-down.bin") == huge);
        QCOMPARE(hugeDown.snapshot().bytesDone, int64_t(huge.size()));
        // Cancelling a laned download stops cleanly and leaves no partial file.
        CopyJob stopped(CopyJob::Mode::Copy, {S + "/huge-up.bin"}, base + "/stopped.bin");
        stopped.start();
        stopped.cancel();
        QTRY_VERIFY(stopped.isFinished());
        QVERIFY(!QFile::exists(base + "/stopped.bin"));
        QCOMPARE(listLocation(S, opt).error, 0);  // connections still usable

        // Links are copied as links.
        QVERIFY(QDir().mkpath(base + "/fromserver"));
        CopyJob links(CopyJob::Mode::Copy, {S + "/link"}, base + "/fromserver");
        QVERIFY(runJob(links));
        QCOMPARE(QFileInfo(base + "/fromserver/link").symLinkTarget(), base + "/fromserver/a.txt");

        // Rename never replaces; mkdir/exists; delete trees.
        QCOMPARE(renameNoReplaceAt(S + "/a.txt", S + "/up/big.bin"), EEXIST);
        QCOMPARE(renameNoReplaceAt(S + "/a.txt", S + "/b.txt"), 0);
        QVERIFY(QFile::exists(base + "/srv/b.txt"));
        QCOMPARE(makeDirectoriesAt(S + "/x/y/z"), 0);
        QCOMPARE(b->mkdir(S + "/x", 0755), EEXIST);
        QCOMPARE(createEmptyFile(S + "/x/new"), 0);
        QCOMPARE(createEmptyFile(S + "/x/new"), EEXIST);
        DeleteJob del({S + "/up", S + "/x"}, true);
        QVERIFY(runJob(del));
        QVERIFY(!QFile::exists(base + "/srv/up"));
        QVERIFY(!QFile::exists(base + "/srv/x"));

        // Many parallel listings share a few connections.
        std::vector<std::thread> threads;
        std::atomic<int> ok{0};
        for (int i = 0; i < 12; ++i)
            threads.emplace_back([&] {
                if (listLocation(S, ListOptions{}).error == 0)
                    ++ok;
            });
        for (auto& th : threads)
            th.join();
        QCOMPARE(ok.load(), 12);

        // The server goes away and comes back: pooled connections are dead,
        // the next call reconnects instead of failing.
        QVERIFY(server.restart());
        QTest::qWait(200);  // let the pooled connections notice they're closed
        const ListResult again = listLocation(S, opt);
        QVERIFY2(again.error == 0, qPrintable(again.errorText));
        QCOMPARE(b->stat(S + "/b.txt", st), 0);

        // Wrong key: a clear error, no hang.
        qputenv("TANDEM_SSH_IDENTITY", (base + "/nokey").toUtf8());
        const ListResult denied = listLocation(server.location(base) .replace("127.0.0.1", "localhost"), opt);
        QVERIFY(denied.error != 0);
        QVERIFY(!denied.errorText.isEmpty());

        setAuthHandler({});
    }

    // A key file saved with the server is used to log in.
    void sftpSavedKeyFile()
    {
        registerBuiltinBackends();
        if (!hasBackend("sftp"))
            QSKIP("built without libssh");
        SshServer server;
        if (!server.start())
            QSKIP("could not start a test sshd");
        setAuthHandler([](const AuthQuestion& q) -> std::optional<QString> {
            if (q.kind == AuthQuestion::Kind::HostKey)
                return QString("yes");
            return std::nullopt;
        });
        const QByteArray key = qgetenv("TANDEM_SSH_IDENTITY");
        QTemporaryDir tmp;
        qputenv("TANDEM_SSH_IDENTITY", QFile::encodeName(tmp.filePath("nokey")));
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([key] {
            qputenv("TANDEM_SSH_IDENTITY", key);
            useConfigDir(QString());
            ServerList::instance().load();
            setAuthHandler({});
        });
        const QString user = qEnvironmentVariable("USER");
        const QString home = QString("sftp://%1@localhost:%2/").arg(user).arg(server.port());
        ListOptions opt;
        QVERIFY(listLocation(home + "tmp", opt).error != 0);  // no key it knows

        SavedServer saved;
        saved.name = "test";
        saved.host = "127.0.0.1";
        saved.port = server.port();
        saved.user = user;
        saved.keyFile = QFile::decodeName(key);
        QVERIFY(ServerList::instance().save({saved}));
        const ListResult l = listLocation(saved.authority() + "/tmp", opt);
        QVERIFY2(l.error == 0, qPrintable(l.errorText + " / " + server.log()));
    }

    // --config-dir: settings and folder history in one chosen folder, as TOML.
    void configDir()
    {
        QTemporaryDir tmp;
        QStringList args = {"/some/path", "--config-dir", tmp.filePath("sync"), "other"};
        setupConfigDir(args);
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        QCOMPARE(args, QStringList({"/some/path", "other"}));
        QCOMPARE(tandem::configDir(), tmp.filePath("sync"));
        QSettings().setValue("test/key", 42);
        const QString file = QSettings().fileName();  // sync/<organization>/<application>.toml
        QVERIFY2(file.startsWith(tmp.filePath("sync/")) && file.endsWith(".toml"), qPrintable(file));
        QSettings().sync();
        QVERIFY(QFile::exists(file));
        QCOMPARE(QSettings(file, tomlFormat()).value("test/key").toInt(), 42);
        FolderHistory::instance().setStoragePath(QString());  // an earlier test set its own
        QCOMPARE(FolderHistory::instance().storagePath(), QFileInfo(file).path() + "/folders.tsv");

        args = {"--config-dir=" + tmp.filePath("other")};
        setupConfigDir(args);
        QVERIFY(args.isEmpty());
        QCOMPARE(tandem::configDir(), tmp.filePath("other"));
    }

    // Workspaces: kept in the settings file, sorted, names unique, ids stable.
    // Shelves: no doubles, order kept, moves follow, kept in the settings.
    void shelves()
    {
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] {
            for (int n = 1; n <= Shelves::kCount; ++n)
                Shelves::instance().clear(n);
            useConfigDir(QString());
        });
        Shelves& s = Shelves::instance();
        QSignalSpy changed(&s, &Shelves::changed);
        QCOMPARE(s.add(2, {"/a/x", "/b/y"}), 2);
        QCOMPARE(s.add(2, {"/b/y", "/c/z"}), 1);  // y is there already
        QCOMPARE(s.items(2), QStringList({"/a/x", "/b/y", "/c/z"}));
        QCOMPARE(s.used(), QList<int>({2}));
        QCOMPARE(s.lastUsed(), 2);
        QCOMPARE(QSettings().value("shelves/2").toStringList(), s.items(2));
        s.remove(2, {"/a/x"});
        QCOMPARE(s.items(2), QStringList({"/b/y", "/c/z"}));
        QCOMPARE(s.add(0, {"/q"}), 0);  // no shelf 0
        QCOMPARE(s.add(10, {"/q"}), 0);
        s.clear(2);
        QVERIFY(s.used().isEmpty());
        QVERIFY(!QSettings().contains("shelves/2"));
        QCOMPARE(changed.count(), 4);

        // There used to be nine: what was on 5–9 moves onto an empty shelf,
        // then onto the last one, once.
        {
            QSettings st;
            st.setValue("shelves/1", QStringList{"/one"});
            st.setValue("shelves/3", QStringList{"/three"});
            st.setValue("shelves/6", QStringList{"/six"});
            st.setValue("shelves/8", QStringList{"/eight", "/one"});
            st.setValue("shelves/9", QStringList{"/nine", "/eight"});
            st.setValue("shelves/last", 9);
        }
        s.reload();
        QCOMPARE(s.items(1), QStringList({"/one"}));
        QCOMPARE(s.items(2), QStringList({"/six"}));
        QCOMPARE(s.items(3), QStringList({"/three"}));
        QCOMPARE(s.items(4), QStringList({"/eight", "/one", "/nine"}));
        QCOMPARE(s.lastUsed(), 4);
        QSettings st;
        QCOMPARE(st.value("shelves/4").toStringList(), s.items(4));
        for (int old = 5; old <= 9; ++old)
            QVERIFY(!st.contains(QStringLiteral("shelves/%1").arg(old)));
        QCOMPARE(st.value("shelves/last").toInt(), 4);
    }

    void workspaces()
    {
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        QVERIFY(Workspaces::all().isEmpty());
        QCOMPARE(Workspaces::startupMode(), StartupMode::Session);  // as before

        Workspace photos;
        photos.name = "  Photos ";
        photos.left = {{"/p/a", "/p/b"}, {1, 3}, {true, false}, 1};
        photos.right = {{QDir::homePath() + "/x"}, {1}, {false}, 0};
        photos.rightActive = true;
        const int photosId = Workspaces::save(photos);
        Workspace code;
        code.name = "code";
        code.left.paths = code.right.paths = {"/c"};
        const int codeId = Workspaces::save(code);
        QVERIFY(photosId > 0 && codeId > 0 && photosId != codeId);

        const QList<Workspace> all = Workspaces::all();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].name, QString("code"));  // by name
        const Workspace back = *Workspaces::byName("PHOTOS");
        QCOMPARE(back.name, QString("Photos"));
        QCOMPARE(back.left.paths, QStringList({"/p/a", "/p/b"}));
        QCOMPARE(back.left.numbers, QList<int>({1, 3}));
        QCOMPARE(back.left.grids, QList<bool>({true, false}));
        QCOMPARE(back.left.current, 1);
        QVERIFY(back.rightActive);
        QCOMPARE(Workspaces::describe(back), QString("/p/a, /p/b  │  ~/x"));

        // The file itself holds them (TOML), not just a cache.
        QSettings().sync();
        QCOMPARE(QSettings(QSettings().fileName(), tomlFormat()).value("workspaces/size").toInt(), 2);

        QVERIFY(!Workspaces::rename(codeId, "photos"));  // taken
        QVERIFY(!Workspaces::rename(codeId, "  "));
        QVERIFY(Workspaces::rename(codeId, "Work"));
        QCOMPARE(Workspaces::byName("work")->id, codeId);  // same id: its key stays

        Workspaces::setStartup(StartupMode::Workspace, photosId);
        QCOMPARE(Workspaces::startupMode(), StartupMode::Workspace);
        QCOMPARE(Workspaces::startupWorkspace(), photosId);
        Workspaces::remove(photosId);
        QCOMPARE(Workspaces::startupMode(), StartupMode::Session);  // its startup goes with it
        QCOMPARE(Workspaces::all().size(), 1);
        Workspaces::setStartup(StartupMode::Home);
        QCOMPARE(Workspaces::startupMode(), StartupMode::Home);
        Workspaces::setStartup(StartupMode::Session);
        QVERIFY(!QSettings().contains("startup/open"));
    }

    // A workspace written by hand needs no [workspaces] size.
    void workspacesWrittenByHand()
    {
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        const QString file = QSettings().fileName();
        QVERIFY(QDir().mkpath(QFileInfo(file).path()));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[workspaces.1]\nid = 1\nname = \"Photos\"\n\n[workspaces.1.left]\ntabs = [\"/p\"]\n\n"
                    "[workspaces.2]\nid = 2\nname = \"Code\"\n");
        }
        const QList<Workspace> all = Workspaces::all();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[1].name, QString("Photos"));
        QCOMPARE(all[1].left.paths, QStringList({"/p"}));
    }

    // Settings as TOML: typed values, edits change single lines, comments stay,
    // window state goes to its own file, and a broken hand edit is never overwritten.
    void tomlSettings()
    {
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        const QString file = QSettings().fileName();
        QVERIFY(QDir().mkpath(QFileInfo(file).path()));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("# my notes\n\n[view]\n# a comment about the theme\ntheme = \"nord\"  \nlistZoom = 100\n");
        }
        {
            QSettings s;
            QCOMPARE(s.value("view/theme").toString(), QString("nord"));
            QCOMPARE(s.value("view/listZoom").toInt(), 100);
            s.setValue("view/listZoom", 120);
            s.setValue("view/fileColors", true);
            s.setValue("tools/editor", "nvim \"%f\"");
            s.setValue("network/recent", QStringList{"sftp://a", "smb://b"});  // state
            s.setValue("window/geometry", QByteArray("\x01\x02\xff", 3));      // state, bytes
            s.setValue("left/tabSlots", QVariantList{1, 2});
        }
        const QString text = [&] {
            QFile f(file);
            f.open(QIODevice::ReadOnly);
            return QString::fromUtf8(f.readAll());
        }();
        QVERIFY2(text.contains("# my notes") && text.contains("# a comment about the theme"), qPrintable(text));
        QVERIFY2(text.contains("listZoom = 120") && text.contains("fileColors = true"), qPrintable(text));
        QVERIFY2(text.contains("theme = \"nord\"  "), qPrintable(text));  // untouched line, as written
        QVERIFY2(text.contains("[tools]\neditor = \"nvim \\\"%f\\\"\""), qPrintable(text));
        QVERIFY2(!text.contains("geometry") && !text.contains("recent"), qPrintable(text));
        QVERIFY(QFile::exists(stateFilePath()));
        QCOMPARE(stateFilePath(), QFileInfo(file).path() + "/state.toml");  // with the settings
        {
            QSettings s;
            QCOMPARE(s.value("window/geometry").toByteArray(), QByteArray("\x01\x02\xff", 3));
            QCOMPARE(s.value("network/recent").toStringList(), QStringList({"sftp://a", "smb://b"}));
            QCOMPARE(s.value("left/tabSlots").toList().value(1).toInt(), 2);
            QCOMPARE(s.value("tools/editor").toString(), QString("nvim \"%f\""));
            s.remove("view/listZoom");
        }
        {
            QFile f(file);
            f.open(QIODevice::ReadOnly);
            const QString t = QString::fromUtf8(f.readAll());
            QVERIFY2(!t.contains("listZoom") && t.contains("# a comment about the theme"), qPrintable(t));
        }
        // A typo by hand: Tandem reads nothing from it, says where, and leaves it alone.
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[view]\ntheme = nord\n");
        }
        {
            QSettings s;
            s.setValue("view/showHidden", true);
            s.sync();
            QVERIFY(s.status() != QSettings::NoError);
        }
        QVERIFY2(tomlSettingsError().contains("line 2"), qPrintable(tomlSettingsError()));
        QFile f(file);
        f.open(QIODevice::ReadOnly);
        QCOMPARE(f.readAll(), QByteArray("[view]\ntheme = nord\n"));
    }

    // A state.toml from r98–r101, straight in the config folder, moves next to the settings.
    void misplacedStateMoves()
    {
        QTemporaryDir tmp;
        {
            QFile f(tmp.filePath("state.toml"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[window]\nactive = \"right\"\n");
        }
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        QVERIFY(!QFile::exists(tmp.filePath("state.toml")));
        QVERIFY(QFile::exists(stateFilePath()));
        QCOMPARE(QFileInfo(stateFilePath()).path(), QFileInfo(QSettings().fileName()).path());
    }

    // Line edits: a commented-out default is reused; removing can leave it commented.
    void tomlLines()
    {
        const QString base = "[keys]\n# copy = \"F5\"\nmove = \"F6\"\n\n[modal]\ndown = \"j\"\n";
        QString t = setTomlLine(base, "keys", "copy", QString("[\"F2\"]"));
        QCOMPARE(t, QString("[keys]\ncopy = [\"F2\"]\nmove = \"F6\"\n\n[modal]\ndown = \"j\"\n"));
        t = setTomlLine(t, "keys", "copy", std::nullopt, "\"F5\"");
        QCOMPARE(t, base);
        t = setTomlLine(base, "keys", "trash", QString("\"F8\""));  // new: after the table's last line
        QCOMPARE(t, QString("[keys]\n# copy = \"F5\"\nmove = \"F6\"\ntrash = \"F8\"\n\n[modal]\ndown = \"j\"\n"));
        t = setTomlLine(base, "plugins/pocket link", "x", QString("1"));  // new table, quoted key
        QVERIFY2(t.endsWith("[plugins.\"pocket link\"]\nx = 1\n"), qPrintable(t));
        QVariantMap m;
        QVERIFY(parseToml(t, &m));
        QCOMPARE(m.value("plugins/pocket link/x").toInt(), 1);
        QCOMPARE(m.value("modal/down").toString(), QString("j"));
    }

    // The old INI file moves into the TOML files once, typed, keys handed on.
    void settingsMigration()
    {
        QTemporaryDir tmp;
        useConfigDir(tmp.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        const QString ini = QSettings(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::organizationName(),
                                      QCoreApplication::applicationName()).fileName();
        QVERIFY(QDir().mkpath(QFileInfo(ini).path()));
        {
            QFile f(ini);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[view]\nfileColors=true\nlistZoom=109\ntheme=nord\n[left]\ntabSlots=1, 2\n"
                    "tabs=/a, /b\n[keys]\ncopy=F2\n");
        }
        QVariantMap keys;
        migrateOldSettings(QSettings::IniFormat, QString(), &keys);
        QCOMPARE(keys.value("keys/copy").toString(), QString("F2"));
        QVERIFY(!QFile::exists(ini));
        QVERIFY(QFile::exists(ini + ".bak"));
        QSettings s;
        QCOMPARE(s.value("view/fileColors").typeId(), QMetaType::Bool);
        QCOMPARE(s.value("view/listZoom").typeId(), QMetaType::Int);
        QCOMPARE(s.value("left/tabs").toStringList(), QStringList({"/a", "/b"}));
        QCOMPARE(s.value("left/tabSlots").toList().value(1).typeId(), QMetaType::Int);
        QVERIFY(!s.contains("keys/copy"));
        migrateOldSettings(QSettings::IniFormat, QString(), &keys);  // once: nothing left to do
    }

    void pluginManifest()
    {
        Plugin p;
        p.id = "demo";
        p.dir = "/opt/demo";
        QVERIFY(parsePluginManifest("{ "
            "            \"tandem\": 1, \"name\": \"Demo\", \"description\": \"Does things\", \"someday\": {\"x\": 1}, "
            "            \"actions\": [ "
            "                {\"title\": \"Send to phone\", \"run\": [\"./send\", \"--to\", \"%c\", \"%s\"], \"choices\": \"pocketlink devices\", "
            "                 \"types\": [\"image/*\", \"*.pdf\"], \"keys\": [\"Ctrl+Alt+P\"], \"glitter\": true}, "
            "                {\"id\": \"here\", \"title\": \"Here\", \"run\": \"echo %d\", \"on\": \"folder\", \"local\": false}, "
            "                {\"title\": \"No command\"} "
            "            ]}", p));
        QCOMPARE(p.name, QString("Demo"));
        QCOMPARE(p.actions.size(), 2);  // the incomplete one is left out
        const PluginAction& send = p.actions[0];
        QCOMPARE(send.id, QString("send-to-phone"));
        QCOMPARE(send.choices, QStringList({"pocketlink", "devices"}));
        QCOMPARE(send.keys, QStringList({"Ctrl+Alt+P"}));
        QVERIFY(send.localOnly && !send.forFolder);
        QVERIFY(p.actions[1].forFolder && !p.actions[1].localOnly);

        Plugin newer;
        QVERIFY(!parsePluginManifest("{\"tandem\": 99, \"actions\": [{\"title\": \"x\", \"run\": \"x\"}]}", newer));
        QVERIFY(!newer.error.isEmpty());
        QVERIFY(!parsePluginManifest("{ broken", newer));

        // Placeholders, no shell: %s gives one argument per path.
        PluginContext ctx;
        ctx.selection = {"/data/a b.jpg", "/data/c.pdf"};
        ctx.current = "/data/a b.jpg";
        ctx.dir = "/data";
        ctx.otherDir = "/other";
        QCOMPARE(expandPluginArgs(p, send.run, ctx, "Pixel 10"),
                 QStringList({"/opt/demo/send", "--to", "Pixel 10", "/data/a b.jpg", "/data/c.pdf"}));
        QCOMPARE(expandPluginArgs(p, {"x", "%n|%f|%d|%D|100%%|%q"}, ctx),
                 QStringList({"x", "a b.jpg|/data/a b.jpg|/data|/other|100%|%q"}));

        // Which actions fit: types by name, local only, folder actions.
        QVERIFY(pluginApplies(send, ctx));
        ctx.selection << "/data/notes.txt";
        QVERIFY(!pluginApplies(send, ctx));  // every item must match
        ctx.selection = {"/data/IMG.JPEG"};
        QVERIFY(pluginApplies(send, ctx));
        ctx.local = false;
        QVERIFY(!pluginApplies(send, ctx));
        QVERIFY(pluginApplies(p.actions[1], ctx));
        ctx.selection.clear();
        ctx.local = true;
        QVERIFY(!pluginApplies(send, ctx));
        // "folders": false keeps an action away from folders.
        PluginAction filesOnly;
        filesOnly.run = {"x"};
        filesOnly.folders = false;
        ctx.selection = {"/data/Photos"};
        ctx.hasFolder = true;
        QVERIFY(!pluginApplies(filesOnly, ctx));
        ctx.hasFolder = false;
        QVERIFY(pluginApplies(filesOnly, ctx));
        ctx.selection.clear();

        // Trust: on, changed, off (kept in the settings: a throwaway folder here).
        QTemporaryDir cfg;
        useConfigDir(cfg.path());
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        p.hash = "aaa";
        setPluginEnabled(p, true);
        QCOMPARE(pluginTrust(p), PluginTrust::On);
        p.hash = "bbb";
        QCOMPARE(pluginTrust(p), PluginTrust::Changed);
        setPluginEnabled(p, false);
        QCOMPARE(pluginTrust(p), PluginTrust::Off);

        PluginRequest r;
        QVERIFY(parsePluginRequest("{\"message\": \"Sent\", \"select\": [\"a\", \"b\"], \"refresh\": true}", r));
        QCOMPARE(r.message, QString("Sent"));
        QCOMPARE(r.select, QStringList({"a", "b"}));
        QVERIFY(r.refresh);
        QVERIFY(!parsePluginRequest("just text", r));
    }

    void pluginRuns()
    {
        QTemporaryDir tmp;
        auto restore = qScopeGuard([] { useConfigDir(QString()); });
        useConfigDir(tmp.filePath("cfg"));
        const QString dir = pluginsFolder() + "/demo";
        QVERIFY(QDir().mkpath(dir));
        writeFile(dir + "/plugin.json", "{\"name\": \"Demo\", \"actions\": [ "
            "            {\"title\": \"Report\", \"run\": [\"./report.sh\", \"%s\"], \"choices\": [\"./devices.sh\"]}, "
            "            {\"title\": \"Fail\", \"run\": [\"sh\", \"-c\", \"echo broken >&2; exit 3\"]}]}");
        writeFile(dir + "/report.sh",
                  "#!/bin/sh\necho \"working on $# files\"\necho '{\"select\": [\"a.txt\"], \"refresh\": true}'\n"
                  "echo \"plugin dir: $TANDEM_PLUGIN_DIR\"\n");
        writeFile(dir + "/devices.sh", "#!/bin/sh\nprintf 'Pixel 10\\tpixel\\nTablet\\n'\n");
        QFile::setPermissions(dir + "/report.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(dir + "/devices.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QVERIFY(QDir().mkpath(pluginsFolder() + "/misnamed"));
        writeFile(pluginsFolder() + "/misnamed/tandem-plugin.json", "{}");

        const QList<Plugin> found = discoverPlugins();
        const auto misnamed = std::find_if(found.begin(), found.end(), [](const Plugin& p) { return p.id == "misnamed"; });
        QVERIFY(misnamed != found.end());
        QVERIFY2(misnamed->error.contains("tandem-plugin.json"), qPrintable(misnamed->error));

        // A plugin that only runs existing programs is a single file.
        writeFile(pluginsFolder() + "/single.json", "{\"actions\": [{\"title\": \"Hi\", \"run\": \"echo hi\"}]}");
        const QList<Plugin> again = discoverPlugins();
        const auto single = std::find_if(again.begin(), again.end(), [](const Plugin& p) { return p.id == "single"; });
        QVERIFY(single != again.end());
        QVERIFY2(single->error.isEmpty(), qPrintable(single->error));
        QCOMPARE(single->dir, pluginsFolder());
        const auto it = std::find_if(found.begin(), found.end(), [](const Plugin& p) { return p.id == "demo"; });
        QVERIFY(it != found.end());
        const Plugin& p = *it;
        QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        QVERIFY(!p.system && !p.hash.isEmpty());

        PluginContext ctx;
        ctx.selection = {tmp.filePath("a.txt"), tmp.filePath("b.txt")};
        ctx.dir = tmp.path();
        QList<PluginRequest> requests;
        bool ok = false, done = false;
        QString text;
        auto* run = new PluginRun(p, p.actions[0], ctx, QString(), nullptr);
        connect(run, &PluginRun::request, this, [&](const PluginRequest& r) { requests << r; });
        connect(run, &PluginRun::finished, this, [&](bool success, const QString& t) {
            ok = success;
            text = t;
            done = true;
        });
        run->start();
        QTRY_VERIFY(done);
        QVERIFY(ok);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests[0].select, QStringList({"a.txt"}));
        QCOMPARE(text, "plugin dir: " + dir);  // the last plain line

        done = false;
        auto* fail = new PluginRun(p, p.actions[1], ctx, QString(), nullptr);
        connect(fail, &PluginRun::finished, this, [&](bool success, const QString& t) {
            ok = success;
            text = t;
            done = true;
        });
        fail->start();
        QTRY_VERIFY(done);
        QVERIFY(!ok);
        QCOMPARE(text, QString("broken"));

        QList<QPair<QString, QString>> entries;
        done = false;
        QObject context;
        listPluginChoices(p, p.actions[0], ctx, &context, [&](const QList<QPair<QString, QString>>& e, const QString& error) {
            QVERIFY(error.isEmpty());
            entries = e;
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0], qMakePair(QString("Pixel 10"), QString("pixel")));
        QCOMPARE(entries[1], qMakePair(QString("Tablet"), QString("Tablet")));
    }

    // A phone held by another program: who has it, from a fake /proc.
    void mtpHolders()
    {
#ifdef Q_OS_MACOS
        QSKIP("MTP through GVfs/KIO and /proc is Linux-only");
#endif
        QCOMPARE(usbNodeFromMtpError("Unable to open MTP device \u201c001,019\u201d"), QString("/dev/bus/usb/001/019"));
        QCOMPARE(usbNodeFromMtpError("Konnte MTP-Ger\u00e4t \u00bb003,007\u00ab nicht \u00f6ffnen"), QString("/dev/bus/usb/003/007"));
        QCOMPARE(usbNodeFromMtpError("Permission denied"), QString());

        QTemporaryDir proc;
        const QString node = "/dev/bus/usb/001/019";
        auto process = [&](const QString& pid, const QString& comm, const QString& fdTarget) {
            QVERIFY(QDir().mkpath(proc.filePath(pid + "/fd")));
            QFile f(proc.filePath(pid + "/comm"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(comm.toUtf8() + "\n");
            QVERIFY(QFile::link(fdTarget, proc.filePath(pid + "/fd/3")));
        };
        process("100", "kiod6", node);
        process("200", "adb", node);             // doesn't block MTP
        process("300", "gvfsd-mtp", node);       // GVfs itself
        process("400", "simple-mtpfs", node);
        process("500", "bash", "/dev/null");     // something else
        QStringList who = usbDeviceHolders(node, proc.path());
        who.sort();
        QCOMPARE(who, QStringList({"Dolphin (kiod6)", "simple-mtpfs"}));
        QVERIFY(usbDeviceHolders("/dev/bus/usb/009/009", proc.path()).isEmpty());
    }

    void helpers()
    {
        QTemporaryDir tmp;
        QCOMPARE(makeDirectories(tmp.filePath("n1/n2/n3")), 0);
        QVERIFY(QFileInfo(tmp.filePath("n1/n2/n3")).isDir());
        QCOMPARE(makeDirectories(tmp.filePath("n1")), EEXIST);
        writeFile(tmp.filePath("n1/f.txt"), "12345");
        writeFile(tmp.filePath("n1/n2/g.txt"), "678");
        QCOMPARE(directorySize(tmp.filePath("n1"), nullptr), int64_t(8));
        QCOMPARE(renameNoReplace(tmp.filePath("n1/f.txt"), tmp.filePath("n1/n2/g.txt")), EEXIST);
        QCOMPARE(renameNoReplace(tmp.filePath("n1/f.txt"), tmp.filePath("n1/h.txt")), 0);
        QCOMPARE(QString::fromLocal8Bit(uniqueName(QFile::encodeName(tmp.filePath("n1/h.txt")))),
                 tmp.filePath("n1/h (2).txt"));
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "test_core.moc"
