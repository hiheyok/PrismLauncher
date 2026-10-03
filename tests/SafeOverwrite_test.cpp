#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "net/FileSink.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool hardLink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_hard_link(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

// Symbolic links need developer mode or administrator rights on Windows
bool symlink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_symlink(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

bool setReadOnly(const QString& path, bool readOnly)
{
    const auto readable = QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther;
    return QFile::setPermissions(path, readOnly ? readable : readable | QFile::WriteOwner | QFile::WriteUser);
}

class FakeReply : public QNetworkReply {
   public:
    explicit FakeReply(int statusCode)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, statusCode);
        open(QIODevice::ReadOnly);
    }
    void abort() override {}

   protected:
    qint64 readData(char*, qint64) override { return -1; }
};

// Downloads data into path like a finished network request would
Result<> download(const QString& path, const QByteArray& data)
{
    Net::FileSink sink(path);
    QNetworkRequest request;
    if (auto initialized = sink.init(request); !initialized) {
        return std::unexpected(initialized.error());
    }
    TRY(sink.write(data))
    FakeReply reply(200);
    return sink.finalize(reply);
}

QStringList leftovers(const QString& dir)
{
    return QDir(dir).entryList({ "*.prism-dl*", "*.prism-new*" }, QDir::Files | QDir::Hidden | QDir::System);
}
}  // namespace

class SafeOverwriteTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("overwrite_test_XXXXXX") };

    QString path(const QString& name) const { return m_dir.filePath(name); }

   private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(FS::deleteContents(m_dir.path()));
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        QDirIterator it(m_dir.path(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            setReadOnly(it.next(), false);
        }
    }

    void test_downloadNewFile()
    {
        QVERIFY(download(path("mod.jar"), "new"));
        QCOMPARE(readFile(path("mod.jar")), "new");
        QVERIFY(leftovers(m_dir.path()).isEmpty());
    }

    void test_downloadOverSymlink()
    {
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        if (!symlink(path("shared.jar"), path("mod.jar"))) {
            QSKIP("Can't create symbolic links here");
        }

        QVERIFY(download(path("mod.jar"), "new"));
        QVERIFY(!QFileInfo(path("mod.jar")).isSymLink());
        QCOMPARE(readFile(path("mod.jar")), "new");
        QCOMPARE(readFile(path("shared.jar")), "shared");
        QVERIFY(leftovers(m_dir.path()).isEmpty());
    }

    void test_downloadOverReadOnlyHardLink()
    {
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        QVERIFY(hardLink(path("shared.jar"), path("mod.jar")));
        QVERIFY(setReadOnly(path("shared.jar"), true));

        QVERIFY(download(path("mod.jar"), "new"));
        QCOMPARE(readFile(path("mod.jar")), "new");
        QCOMPARE(readFile(path("shared.jar")), "shared");
        QVERIFY(!QFileInfo(path("shared.jar")).isWritable());
        QVERIFY(leftovers(m_dir.path()).isEmpty());
    }

    void test_abortedDownloadKeepsTarget()
    {
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        QVERIFY(hardLink(path("shared.jar"), path("mod.jar")));
        QVERIFY(setReadOnly(path("shared.jar"), true));

        {
            Net::FileSink sink(path("mod.jar"));
            QNetworkRequest request;
            QVERIFY(sink.init(request));
            QVERIFY(sink.write("partial"));
            sink.abort();
        }

        QCOMPARE(readFile(path("mod.jar")), "shared");
        QCOMPARE(FS::fileId(path("mod.jar")).value(), FS::fileId(path("shared.jar")).value());
        QVERIFY(leftovers(m_dir.path()).isEmpty());
    }

    void test_failedSwapKeepsTarget()
    {
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        QVERIFY(hardLink(path("shared.jar"), path("mod.jar")));
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::Replace; });

        QVERIFY(!download(path("mod.jar"), "new"));
        QCOMPARE(readFile(path("mod.jar")), "shared");
        QCOMPARE(readFile(path("shared.jar")), "shared");
        QVERIFY(leftovers(m_dir.path()).isEmpty());
    }

    void test_overrideFolder()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("instance/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("override/mods/nested"));
        QVERIFY(writeFile(path("instance/mods/kept.jar"), "kept"));
        QVERIFY(writeFile(path("instance/mods/replaced.jar"), "old"));
        QVERIFY(writeFile(path("override/mods/replaced.jar"), "new"));
        QVERIFY(writeFile(path("override/mods/nested/added.jar"), "added"));

        QVERIFY(FS::overrideFolder(path("instance"), path("override")));
        QCOMPARE(readFile(path("instance/mods/kept.jar")), "kept");
        QCOMPARE(readFile(path("instance/mods/replaced.jar")), "new");
        QCOMPARE(readFile(path("instance/mods/nested/added.jar")), "added");
        QVERIFY(leftovers(path("instance/mods")).isEmpty());
    }

    void test_overrideFolderOverHardLink()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("instance/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("override/mods"));
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        QVERIFY(hardLink(path("shared.jar"), path("instance/mods/mod.jar")));
        QVERIFY(setReadOnly(path("shared.jar"), true));
        QVERIFY(writeFile(path("override/mods/mod.jar"), "new"));

        QVERIFY(FS::overrideFolder(path("instance"), path("override")));
        QCOMPARE(readFile(path("instance/mods/mod.jar")), "new");
        QCOMPARE(readFile(path("shared.jar")), "shared");
        QVERIFY(!QFileInfo(path("shared.jar")).isWritable());
    }

    void test_overrideFolderOverSymlink()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("instance/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("override/mods"));
        QVERIFY(writeFile(path("shared.jar"), "shared"));
        if (!symlink(path("shared.jar"), path("instance/mods/mod.jar"))) {
            QSKIP("Can't create symbolic links here");
        }
        QVERIFY(writeFile(path("override/mods/mod.jar"), "new"));

        QVERIFY(FS::overrideFolder(path("instance"), path("override")));
        QVERIFY(!QFileInfo(path("instance/mods/mod.jar")).isSymLink());
        QCOMPARE(readFile(path("instance/mods/mod.jar")), "new");
        QCOMPARE(readFile(path("shared.jar")), "shared");
    }

    void test_failedOverrideKeepsOriginal()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("instance/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("override/mods"));
        QVERIFY(writeFile(path("instance/mods/mod.jar"), "old"));
        QVERIFY(writeFile(path("override/mods/mod.jar"), "new"));
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::Replace; });

        QVERIFY(!FS::overrideFolder(path("instance"), path("override")));
        QCOMPARE(readFile(path("instance/mods/mod.jar")), "old");
        QVERIFY(leftovers(path("instance/mods")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SafeOverwriteTest)

#include "SafeOverwrite_test.moc"
