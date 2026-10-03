#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"

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

bool isReadOnly(const QString& path)
{
    return !QFileInfo(path).isWritable();
}
}  // namespace

class FileSystemPrimitivesTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("primitives_test_XXXXXX") };

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
        FS::Testing::setCallRecorder(nullptr);
        // read-only files would otherwise stop the directory from being removed
        for (const auto& entry : QDir(m_dir.path()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::System)) {
            setReadOnly(entry.filePath(), false);
        }
    }

    void test_fileId()
    {
        QVERIFY(writeFile(path("a"), "a"));
        QVERIFY(writeFile(path("b"), "a"));
        QVERIFY(hardLink(path("a"), path("a-link")));

        const auto a = FS::fileId(path("a"));
        QVERIFY(a);
        QVERIFY(a->isValid());
        QCOMPARE(FS::fileId(path("a")).value(), a.value());
        QCOMPARE(FS::fileId(path("a-link")).value(), a.value());
        QVERIFY(FS::fileId(path("b")).value() != a.value());
        QVERIFY(!FS::fileId(path("missing")));
    }

    void test_fileIdOfSymlink()
    {
        QVERIFY(writeFile(path("target"), "data"));
        if (!symlink(path("target"), path("link"))) {
            QSKIP("Can't create symbolic links here");
        }
        const auto target = FS::fileId(path("target")).value();
        QVERIFY(FS::fileId(path("link"), false).value() != target);
        QCOMPARE(FS::fileId(path("link"), true).value(), target);
    }

    void test_identity()
    {
        QVERIFY(writeFile(path("file"), "data"));
        const auto before = FS::identity(path("file"));
        QVERIFY(before);
        QCOMPARE(before->size, 4);
        QCOMPARE(before->fileId, FS::fileId(path("file")).value());

        QTest::qSleep(20);
        QVERIFY(writeFile(path("file"), "more data"));
        const auto after = FS::identity(path("file")).value();
        QCOMPARE(after.fileId, before->fileId);
        QCOMPARE(after.size, 9);
        QVERIFY(after.modifiedTime != before->modifiedTime);
        QVERIFY(after.changeTime != before->changeTime);
    }

    void test_identityTimesAreSinceUnixEpoch()
    {
        QVERIFY(writeFile(path("file"), "data"));
        const auto identity = FS::identity(path("file")).value();
        const qint64 now = QDateTime::currentMSecsSinceEpoch() * 1000000;
        constexpr qint64 hour = 3600LL * 1000000000;
        QVERIFY2(qAbs(identity.modifiedTime - now) < hour, qPrintable(QString::number(identity.modifiedTime)));
        QVERIFY2(qAbs(identity.changeTime - now) < hour, qPrintable(QString::number(identity.changeTime)));
    }

    void test_sameVolume()
    {
        QVERIFY(writeFile(path("file"), "data"));
        QVERIFY(FS::sameVolume(path("file"), m_dir.path()));
        // missing paths use their nearest existing ancestor
        QVERIFY(FS::sameVolume(path("missing/deeper/file"), path("file")));
    }

    void test_probeHardLink()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("other"));
        QVERIFY(FS::probeHardLink(m_dir.path(), path("other")));
        // the probe leaves nothing behind
        QCOMPARE(QDir(m_dir.path()).entryList(QDir::Files).size(), 0);
        QCOMPARE(QDir(path("other")).entryList(QDir::Files).size(), 0);
    }

    void test_replaceFile()
    {
        QVERIFY(writeFile(path("source"), "new"));
        QVERIFY(writeFile(path("target"), "old"));
        const auto sourceId = FS::fileId(path("source")).value();

        QVERIFY(FS::replaceFile(path("source"), path("target")));
        QVERIFY(!QFile::exists(path("source")));
        QCOMPARE(readFile(path("target")), "new");
        QCOMPARE(FS::fileId(path("target")).value(), sourceId);
    }

    void test_replaceFileWithoutTarget()
    {
        QVERIFY(writeFile(path("source"), "new"));
        QVERIFY(FS::replaceFile(path("source"), path("target")));
        QCOMPARE(readFile(path("target")), "new");
    }

    void test_replaceFileMissingSourceKeepsTarget()
    {
        QVERIFY(writeFile(path("target"), "old"));
        QVERIFY(!FS::replaceFile(path("missing"), path("target")));
        QCOMPARE(readFile(path("target")), "old");
    }

    void test_replaceFileDoesNotFollowSymlink()
    {
        QVERIFY(writeFile(path("shared"), "shared"));
        if (!symlink(path("shared"), path("target"))) {
            QSKIP("Can't create symbolic links here");
        }
        QVERIFY(writeFile(path("source"), "new"));

        QVERIFY(FS::replaceFile(path("source"), path("target")));
        QVERIFY(!QFileInfo(path("target")).isSymLink());
        QCOMPARE(readFile(path("target")), "new");
        QCOMPARE(readFile(path("shared")), "shared");
    }

    void test_replaceReadOnlyHardLink()
    {
        QVERIFY(writeFile(path("shared"), "shared"));
        QVERIFY(hardLink(path("shared"), path("target")));
        QVERIFY(setReadOnly(path("shared"), true));
        QVERIFY(writeFile(path("source"), "new"));

        QVERIFY(FS::replaceFile(path("source"), path("target")));
        QCOMPARE(readFile(path("target")), "new");
        QCOMPARE(readFile(path("shared")), "shared");
        QVERIFY(isReadOnly(path("shared")));
        QCOMPARE(FS::hardLinkCount(path("shared")), uintmax_t(1));
    }

    void test_deleteLink()
    {
        QVERIFY(writeFile(path("file"), "data"));
        QVERIFY(FS::deleteLink(path("file")));
        QVERIFY(!QFile::exists(path("file")));
        QVERIFY(!FS::deleteLink(path("file")));
    }

    void test_deleteReadOnlyHardLink()
    {
        QVERIFY(writeFile(path("shared"), "shared"));
        QVERIFY(hardLink(path("shared"), path("link")));
        QVERIFY(setReadOnly(path("shared"), true));

        QVERIFY(FS::deleteLink(path("link")));
        QVERIFY(!QFile::exists(path("link")));
        QCOMPARE(readFile(path("shared")), "shared");
        QVERIFY(isReadOnly(path("shared")));
    }

    void test_deleteSymlinkKeepsTarget()
    {
        QVERIFY(writeFile(path("target"), "data"));
        if (!symlink(path("target"), path("link"))) {
            QSKIP("Can't create symbolic links here");
        }
        QVERIFY(FS::deleteLink(path("link")));
        QVERIFY(!QFileInfo(path("link")).isSymLink());
        QCOMPARE(readFile(path("target")), "data");
    }

    void test_deleteTreeWithReadOnlyHardLink()
    {
        QVERIFY(writeFile(path("shared"), "shared"));
        QVERIFY(QDir(m_dir.path()).mkpath("tree/nested"));
        QVERIFY(writeFile(path("tree/file"), "data"));
        QVERIFY(hardLink(path("shared"), path("tree/nested/link")));
        QVERIFY(setReadOnly(path("shared"), true));

        QVERIFY(FS::deleteTree(path("tree")));
        QVERIFY(!QFileInfo::exists(path("tree")));
        QCOMPARE(readFile(path("shared")), "shared");
        QVERIFY(isReadOnly(path("shared")));
        QVERIFY(FS::deleteTree(path("missing")));
    }

    void test_deleteTreeReportsInaccessiblePath()
    {
#if defined(Q_OS_WIN)
        QSKIP("Directory permissions are tested on POSIX");
#else
        QVERIFY(QDir(m_dir.path()).mkpath("locked/tree"));
        QVERIFY(writeFile(path("locked/tree/file"), "data"));
        QVERIFY(QFile::setPermissions(path("locked"), QFile::Permissions()));
        const bool accessible = QFileInfo::exists(path("locked/tree/file"));
        const auto result = FS::deleteTree(path("locked/tree"));
        QVERIFY(QFile::setPermissions(path("locked"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        if (accessible) {
            QSKIP("Permissions aren't enforced for this user");
        }
        QVERIFY(!result);
        QVERIFY(QFileInfo::exists(path("locked/tree/file")));
#endif
    }

    void test_deletePathWithReadOnlyHardLink()
    {
        QVERIFY(writeFile(path("shared"), "shared"));
        QVERIFY(QDir(m_dir.path()).mkpath("instance/mods"));
        QVERIFY(hardLink(path("shared"), path("instance/mods/mod.jar")));
        QVERIFY(setReadOnly(path("shared"), true));

        QVERIFY(FS::deletePath(path("instance")));
        QVERIFY(!QFileInfo::exists(path("instance")));
        QCOMPARE(readFile(path("shared")), "shared");
        QVERIFY(isReadOnly(path("shared")));
    }

    void test_flush()
    {
        QVERIFY(writeFile(path("file"), "data"));
        QVERIFY(FS::flushFile(path("file")));
        QVERIFY(FS::flushDir(m_dir.path()));
        QVERIFY(!FS::flushFile(path("missing")));
    }

    void test_faultHook()
    {
        QVERIFY(writeFile(path("source"), "new"));
        QVERIFY(writeFile(path("target"), "old"));
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::Replace; });

        QVERIFY(!FS::replaceFile(path("source"), path("target")));
        QCOMPARE(readFile(path("target")), "old");
        QCOMPARE(readFile(path("source")), "new");
        QVERIFY(FS::flushFile(path("source")));
    }

    void test_callRecorder()
    {
        QVERIFY(writeFile(path("source"), "new"));
        QList<FS::Testing::Operation> calls;
        FS::Testing::setCallRecorder([&calls](FS::Testing::Operation operation, const QString&) { calls.append(operation); });

        QVERIFY(FS::flushFile(path("source")));
        QVERIFY(FS::replaceFile(path("source"), path("target")));
        QVERIFY(FS::flushDir(m_dir.path()));
        QVERIFY(FS::deleteLink(path("target")));

        const QList<FS::Testing::Operation> expected{ FS::Testing::Operation::FlushFile, FS::Testing::Operation::Replace,
                                                      FS::Testing::Operation::FlushDir, FS::Testing::Operation::Delete };
        QVERIFY(calls == expected);
    }
};

QTEST_GUILESS_MAIN(FileSystemPrimitivesTest)

#include "FileSystemPrimitives_test.moc"
