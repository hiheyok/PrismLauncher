#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <archive.h>
#include <archive_entry.h>

#include "FileSystemPrimitives.h"
#include "archive/ExportToZipTask.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// The entries of an archive, by name, with their permissions
QMap<QString, int> readArchive(const QString& path)
{
    QMap<QString, int> entries;
    auto* reader = archive_read_new();
    archive_read_support_format_all(reader);
    if (archive_read_open_filename(reader, path.toUtf8().constData(), 10240) == ARCHIVE_OK) {
        archive_entry* entry = nullptr;
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            entries[QString::fromUtf8(archive_entry_pathname(entry))] = static_cast<int>(archive_entry_perm(entry));
            archive_read_data_skip(reader);
        }
    }
    archive_read_free(reader);
    return entries;
}
}  // namespace

class ExportZipTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir{ QDir::current().filePath("export_zip_test_XXXXXX") };

    QString path(const QString& name) const { return m_dir.filePath(name); }

    // Exports the instance folder and returns the task, after it finished
    std::unique_ptr<MMCZip::ExportToZipTask> exportInstance()
    {
        QFileInfoList files;
        for (const auto* name : { "mods/shared.jar", "mods/.prism-new-123456", "mods/local.jar", "resourcepacks/gone.zip" }) {
            const QFileInfo info(path(QString("instance/") + name));
            if (info.exists() || info.isSymbolicLink()) {
                files.append(info);
            }
        }
        auto task = std::make_unique<MMCZip::ExportToZipTask>(path("export.zip"), path("instance"), files, "", true);
        QSignalSpy finished(task.get(), &Task::finished);
        task->start();
        if (finished.isEmpty()) {
            finished.wait(10000);
        }
        return task;
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(writeFile(path("instance/mods/shared.jar"), "shared"));
        // shared files are read-only
        QVERIFY(QFile::setPermissions(path("instance/mods/shared.jar"),
                                      QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
        QVERIFY(writeFile(path("instance/mods/local.jar"), "local"));
        // a placement that is in progress
        QVERIFY(writeFile(path("instance/mods/.prism-new-123456"), "temporary"));
    }

    void cleanupTestCase() { QFile::setPermissions(path("instance/mods/shared.jar"), QFile::ReadOwner | QFile::WriteOwner); }

    void test_sharedFilesAreExportedWritable()
    {
        const auto task = exportInstance();
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        const auto entries = readArchive(path("export.zip"));
        QVERIFY(entries.contains("mods/shared.jar"));
        QVERIFY((entries["mods/shared.jar"] & 0200) != 0);
        QVERIFY(entries.contains("mods/local.jar"));
    }

    void test_temporaryFilesAreLeftOut()
    {
        const auto task = exportInstance();
        QVERIFY(task->wasSuccessful());
        QVERIFY(!readArchive(path("export.zip")).contains("mods/.prism-new-123456"));
    }

    void test_linkToAMissingFileIsLeftOut()
    {
        if (!FS::createSymbolicLink(QFileInfo(path("nowhere/gone.zip")).absoluteFilePath(), path("instance/resourcepacks/gone.zip"))) {
            QDir().mkpath(path("instance/resourcepacks"));
            if (!FS::createSymbolicLink(QFileInfo(path("nowhere/gone.zip")).absoluteFilePath(), path("instance/resourcepacks/gone.zip"))) {
                QSKIP("This system doesn't allow creating symbolic links");
            }
        }
        const auto task = exportInstance();
        // the export doesn't fail halfway, and says what it left out
        QVERIFY2(task->wasSuccessful(), qPrintable(task->failReason()));
        const auto entries = readArchive(path("export.zip"));
        QVERIFY(!entries.contains("resourcepacks/gone.zip"));
        QVERIFY(entries.contains("mods/shared.jar"));
        QVERIFY(task->warnings().join('\n').contains("resourcepacks/gone.zip"));
        QFile::remove(path("instance/resourcepacks/gone.zip"));
    }
};

QTEST_GUILESS_MAIN(ExportZipTest)

#include "ExportZip_test.moc"
