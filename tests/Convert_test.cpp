#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/StoreFormat.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString sha256Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeOwner | QFile::ExeUser);
}

bool sameFile(const QString& first, const QString& second)
{
    const auto a = FS::fileId(first);
    const auto b = FS::fileId(second);
    return a && b && *a == *b;
}

// The writer version of a header file of the store
int writerVersionIn(const QString& path, const QString& field = {})
{
    auto json = QJsonDocument::fromJson(readFile(path)).object();
    if (!field.isEmpty()) {
        json = json[field].toObject();
    }
    return json["minWriterVersion"].toInt();
}

constexpr bool g_replacesUserFiles =
#if defined(Q_OS_WIN)
    true;
#else
    false;
#endif

using Step = ContentStore::PlacementStep;
using Outcome = ContentStore::ConvertOutcome;
}  // namespace

class ConvertTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("convert_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }
    QString file(const QString& relativePath) const { return path("a/" + relativePath); }

    ContentStore::Destination destination(const QString& relativePath) const
    {
        return { m_store->instanceOwner("a"), path("a"), relativePath };
    }

    QString store(const QByteArray& data)
    {
        const auto source = path("downloads/" + sha256Of(data));
        writeFile(source, data);
        const auto stored = m_store->ingest(source, ContentStore::IngestMode::Copy);
        return stored ? stored->hash : QString();
    }

    void reopen()
    {
        FS::Testing::setFaultHook(nullptr);
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    // The store's format as a launcher that only shares new files sees it
    StoreAccess accessForVersion1() const { return m_store->format().accessFor(1); }

   private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void init()
    {
        m_store.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        for (const auto* dir : { "data", "downloads", "a/mods", "elsewhere" }) {
            QVERIFY(QDir(m_dir.path()).mkpath(dir));
        }
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void test_userFileBecomesTheStoredFile()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        const auto before = FS::fileId(file("mods/mod.jar"));

        const auto converted = m_store->convert(destination("mods/mod.jar"));
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(converted->outcome, Outcome::Shared);
        // no copy: the user's file itself is the stored file now
        QCOMPARE(FS::fileId(file("mods/mod.jar")), before);
        QVERIFY(sameFile(file("mods/mod.jar"), m_store->objectPath(sha256Of("the user's mod"))));
        QVERIFY(!QFileInfo(file("mods/mod.jar")).isWritable());
        QCOMPARE(readFile(file("mods/mod.jar")), "the user's mod");
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, sha256Of("the user's mod"));
        QVERIFY(m_store->table().freezes().isEmpty());
        QVERIFY(QDir(path("a/mods")).entryList({ ".prism-*" }, QDir::AllEntries | QDir::Hidden).isEmpty());
    }

    void test_sharedFileStaysAsItIs()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "mod"));
        QVERIFY(m_store->convert(destination("mods/mod.jar")));
        const auto again = m_store->convert(destination("mods/mod.jar"));
        QVERIFY(again);
        QCOMPARE(again->outcome, Outcome::AlreadyShared);
    }

    void test_linkNobodyRecordedIsRecorded()
    {
        const auto hash = store("stored");
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), file("mods/linked.jar")));
        const auto converted = m_store->convert(destination("mods/linked.jar"));
        QVERIFY(converted);
        QCOMPARE(converted->outcome, Outcome::Shared);
        QCOMPARE(m_store->table().ref(destination("mods/linked.jar").key())->hash, hash);
    }

    void test_fileIdenticalToAStoredOne()
    {
        const auto hash = store("common mod");
        QVERIFY(writeFile(file("mods/mod.jar"), "common mod"));
        const auto permissions = QFile::permissions(file("mods/mod.jar"));

        const auto converted = m_store->convert(destination("mods/mod.jar"));
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        if (g_replacesUserFiles) {
            QCOMPARE(converted->outcome, Outcome::Shared);
            QVERIFY(sameFile(file("mods/mod.jar"), m_store->objectPath(hash)));
        } else {
            // replacing the user's file needs the backup protocol here
            QCOMPARE(converted->outcome, Outcome::Skipped);
            QVERIFY(!sameFile(file("mods/mod.jar"), m_store->objectPath(hash)));
            QCOMPARE(QFile::permissions(file("mods/mod.jar")), permissions);
        }
        QCOMPARE(readFile(file("mods/mod.jar")), "common mod");
        QVERIFY(m_store->table().freezes().isEmpty());
    }

    void test_copiedInsteadOfLinked()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        const auto before = FS::fileId(file("mods/mod.jar"));
        const auto permissions = QFile::permissions(file("mods/mod.jar"));
        // the user's file can't be linked into the store, so the store copies it
        FS::Testing::setFaultHook([](FS::Testing::Operation operation, const QString& link) {
            return operation == FS::Testing::Operation::HardLink && QFileInfo(link).fileName().startsWith("object.ingest-");
        });
        const auto converted = m_store->convert(destination("mods/mod.jar"));
        FS::Testing::setFaultHook(nullptr);
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        if (g_replacesUserFiles) {
            QCOMPARE(converted->outcome, Outcome::Shared);
            QVERIFY(sameFile(file("mods/mod.jar"), m_store->objectPath(sha256Of("the user's mod"))));
        } else {
            // the copy would replace the user's file, which needs the backup protocol here
            QCOMPARE(converted->outcome, Outcome::Skipped);
            QCOMPARE(FS::fileId(file("mods/mod.jar")), before);
            QCOMPARE(QFile::permissions(file("mods/mod.jar")), permissions);
        }
        QCOMPARE(readFile(file("mods/mod.jar")), "the user's mod");
        QVERIFY(m_store->table().freezes().isEmpty());
    }

    void test_otherHardLinksAreLeftAlone()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "shared with another program"));
        QVERIFY(FS::createHardLink(file("mods/mod.jar"), path("elsewhere/mod.jar")));
        const auto permissions = QFile::permissions(file("mods/mod.jar"));

        const auto converted = m_store->convert(destination("mods/mod.jar"));
        QVERIFY(converted);
        QCOMPARE(converted->outcome, Outcome::Skipped);
        QVERIFY(sameFile(file("mods/mod.jar"), path("elsewhere/mod.jar")));
        QCOMPARE(QFile::permissions(path("elsewhere/mod.jar")), permissions);
        QVERIFY(m_store->table().entries().isEmpty());
    }

    void test_adoptingHardLinkedFilesCopiesThem()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "shared with another program"));
        QVERIFY(FS::createHardLink(file("mods/mod.jar"), path("elsewhere/mod.jar")));
        const auto permissions = QFile::permissions(path("elsewhere/mod.jar"));

        ContentStore::ConvertOptions options;
        options.adoptHardLinked = true;
        const auto converted = m_store->convert(destination("mods/mod.jar"), options);
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(converted->outcome, g_replacesUserFiles ? Outcome::Shared : Outcome::Skipped);
        // the other link is never made read-only or changed
        QCOMPARE(QFile::permissions(path("elsewhere/mod.jar")), permissions);
        QVERIFY(QFileInfo(path("elsewhere/mod.jar")).isWritable());
        QCOMPARE(readFile(path("elsewhere/mod.jar")), "shared with another program");
        QCOMPARE(sameFile(file("mods/mod.jar"), path("elsewhere/mod.jar")), !g_replacesUserFiles);
    }

    void test_noEditDuringACopy()
    {
        if (!g_replacesUserFiles) {
            QSKIP("Copy mode conversions need the backup protocol on this system");
        }
        QVERIFY(writeFile(file("mods/mod.jar"), "original"));
        QVERIFY(FS::createHardLink(file("mods/mod.jar"), path("elsewhere/mod.jar")));
        const auto target = file("mods/mod.jar");
        // edited after it was copied, before it is replaced
        bool tried = false;
        bool edited = false;
        FS::Testing::setFaultHook([&](FS::Testing::Operation operation, const QString& linkPath) {
            if (!tried && operation == FS::Testing::Operation::HardLink && QFileInfo(linkPath).fileName().startsWith(".prism-new-")) {
                tried = true;
                QFile file(target);
                edited = file.open(QIODevice::Append) && file.write(" and an edit") > 0;
            }
            return false;
        });
        ContentStore::ConvertOptions options;
        options.adoptHardLinked = true;
        const auto converted = m_store->convert(destination("mods/mod.jar"), options);
        QVERIFY(tried);
        // the file is pinned from the copy until it is replaced, so no other program can write to it meanwhile
        QVERIFY(!edited);
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(readFile(file("mods/mod.jar")), "original");
        QCOMPARE(readFile(path("elsewhere/mod.jar")), "original");
    }

    void test_failedPlacementRestoresPermissions()
    {
        if (!g_replacesUserFiles) {
            QSKIP("Replacing conversions need the backup protocol on this system");
        }
        store("common mod");
        QVERIFY(writeFile(file("mods/mod.jar"), "common mod"));
        const auto before = FS::fileId(file("mods/mod.jar"));
        const auto permissions = QFile::permissions(file("mods/mod.jar"));
        const auto target = QFileInfo(file("mods/mod.jar")).absoluteFilePath();
        FS::Testing::setFaultHook([&target](FS::Testing::Operation operation, const QString& replaced) {
            return operation == FS::Testing::Operation::Replace && QFileInfo(replaced).absoluteFilePath() == target;
        });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        QCOMPARE(FS::fileId(file("mods/mod.jar")), before);
        QCOMPARE(QFile::permissions(file("mods/mod.jar")), permissions);
        QVERIFY(QFileInfo(file("mods/mod.jar")).isWritable());
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QVERIFY(m_store->table().freezes().isEmpty());
    }

    void test_fileReplacedAfterItWasStored()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "original"));
        const auto target = file("mods/mod.jar");
        // an editor saves by renaming its new file over the path once the old one became the stored file
        m_store->setInterruptionForTesting([this, &target](Step step) {
            if (step == Step::Ingested) {
                writeFile(path("elsewhere/saved.tmp"), "new editor contents");
                FS::replaceFile(path("elsewhere/saved.tmp"), target);
            }
            return false;
        });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        m_store->setInterruptionForTesting(nullptr);
        // the editor's file is kept, writable and unlinked
        QCOMPARE(readFile(target), "new editor contents");
        QVERIFY(QFileInfo(target).isWritable());
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QVERIFY(m_store->table().freezes().isEmpty());
    }

#if defined(Q_OS_WIN)
    void test_noWriterDuringTheConversion()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "original"));
        const auto target = file("mods/mod.jar");
        // another program tries to open the file for writing while it becomes the stored file
        bool tried = false;
        std::unique_ptr<QFile> writer;
        FS::Testing::setFaultHook([&](FS::Testing::Operation operation, const QString&) {
            if (!tried && operation == FS::Testing::Operation::Replace) {
                tried = true;
                writer = std::make_unique<QFile>(target);
                if (!writer->open(QIODevice::ReadWrite)) {
                    writer.reset();
                }
            }
            return false;
        });
        const auto converted = m_store->convert(destination("mods/mod.jar"));
        FS::Testing::setFaultHook(nullptr);
        QVERIFY(tried);
        QVERIFY2(!writer, "a writer could open the file during the conversion");
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(converted->outcome, Outcome::Shared);
        QCOMPARE(readFile(m_store->objectPath(sha256Of("original"))), "original");
    }

    void test_fileReplacedDuringTheConversion()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "original"));
        const auto target = file("mods/mod.jar");
        // an editor saves by renaming a new file over the path, which the pin allows, and keeps it open for writing
        bool replaced = false;
        std::unique_ptr<QFile> writer;
        FS::Testing::setFaultHook([&](FS::Testing::Operation operation, const QString& link) {
            if (!replaced && operation == FS::Testing::Operation::HardLink && QFileInfo(link).fileName().startsWith("object.ingest-")) {
                replaced =
                    writeFile(path("elsewhere/saved.tmp"), "edited") && FS::replaceFile(path("elsewhere/saved.tmp"), target).has_value();
                writer = std::make_unique<QFile>(target);
                if (!writer->open(QIODevice::ReadWrite)) {
                    writer.reset();
                }
            }
            return false;
        });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        FS::Testing::setFaultHook(nullptr);
        QVERIFY(replaced);
        QVERIFY(writer);
        // the editor's file isn't stored, made read-only or linked, and writing to it changes no stored file
        QVERIFY(m_store->table().entries().isEmpty());
        QVERIFY(m_store->table().freezes().isEmpty());
        QVERIFY(QFileInfo(target).isWritable());
        QVERIFY(writer->write("CHANGED!") > 0);
        writer->close();
        QVERIFY(!QDirIterator(path("store/objects"), QDir::Files, QDirIterator::Subdirectories).hasNext());
    }

    void test_fileOpenForWritingIsSkipped()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "being written"));
        QFile writer(file("mods/mod.jar"));
        QVERIFY(writer.open(QIODevice::ReadWrite));
        const auto converted = m_store->convert(destination("mods/mod.jar"));
        writer.close();
        QVERIFY(converted);
        QCOMPARE(converted->outcome, Outcome::Skipped);
        QVERIFY(QFileInfo(file("mods/mod.jar")).isWritable());
        QVERIFY(m_store->table().entries().isEmpty());
    }
#endif

    // Crashes

    void test_crashAfterFreezingRestoresPermissions()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        const auto permissions = QFile::permissions(file("mods/mod.jar"));
        m_store->setInterruptionForTesting([](Step step) { return step == Step::Frozen; });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        QCOMPARE(m_store->table().freezes().size(), 1);
        // as the ingest would, before the launcher stopped
        QFile::setPermissions(file("mods/mod.jar"), QFile::ReadOwner | QFile::ReadUser);

        reopen();
        QCOMPARE(QFile::permissions(file("mods/mod.jar")), permissions);
        QVERIFY(m_store->table().freezes().isEmpty());
        QCOMPARE(readFile(file("mods/mod.jar")), "the user's mod");
        // nothing of the conversion is left, so the store is for every launcher again
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
    }

    void test_crashAfterTheFileBecameStored()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        m_store->setInterruptionForTesting([](Step step) { return step == Step::Ingested; });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        reopen();
        // the conversion completed as far as the file goes: it is the stored file, read-only
        QVERIFY(m_store->table().freezes().isEmpty());
        QVERIFY(sameFile(file("mods/mod.jar"), m_store->objectPath(sha256Of("the user's mod"))));
        QVERIFY(!QFileInfo(file("mods/mod.jar")).isWritable());
        // and the next scan of the instances records the link, as at startup
        ContentStore::ReconcileOptions options;
        options.knownOwners.insert(m_store->instanceOwner("a"), QFileInfo(path("a")).absoluteFilePath());
        QCOMPARE(m_store->reconcile(options)->adopted, 1);
        QVERIFY(m_store->table().ref(destination("mods/mod.jar").key()));
    }

    // Format versions

    void test_conversionNeedsVersion2UntilFinished()
    {
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        m_store->setInterruptionForTesting([](Step step) { return step == Step::Ingested; });
        QVERIFY(!m_store->convert(destination("mods/mod.jar")));
        // a conversion in progress is state a launcher that only shares new files can't change safely
        QCOMPARE(accessForVersion1(), StoreAccess::ReadOnly);
        QCOMPARE(writerVersionIn(path("store/format.json")), 2);
        QCOMPARE(writerVersionIn(path("store/refs.json"), "format"), 2);
        QCOMPARE(m_store->table().freezes().size(), 1);

        m_store->setInterruptionForTesting(nullptr);
        reopen();
        // finished when it opened: every header is back to version 1, and the snapshot has no conversion state
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
        QCOMPARE(writerVersionIn(path("store/format.json")), 1);
        QCOMPARE(writerVersionIn(path("store/refs.json"), "format"), 1);
        QVERIFY(!QString::fromUtf8(readFile(path("store/refs.json"))).contains("freezes"));
    }

    void test_finishedConversionsLowerTheVersion()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        QVERIFY(m_store->convert(destination("mods/mod.jar")));
        QCOMPARE(accessForVersion1(), StoreAccess::ReadOnly);
        QVERIFY(m_store->finishConversions());
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
        QCOMPARE(writerVersionIn(path("store/format.json")), 1);
        reopen();
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, sha256Of("the user's mod"));
    }

    void test_crashWhileLoweringKeepsTheStoreProtected()
    {
        QVERIFY(writeFile(file("mods/mod.jar"), "the user's mod"));
        QVERIFY(m_store->convert(destination("mods/mod.jar")));
        // format.json is written last; the launcher stops before that
        const auto formatFile = QFileInfo(path("store/format.json")).absoluteFilePath();
        FS::Testing::setFaultHook([&formatFile](FS::Testing::Operation operation, const QString& target) {
            return operation == FS::Testing::Operation::Replace && QFileInfo(target).absoluteFilePath() == formatFile;
        });
        QVERIFY(!m_store->finishConversions());
        FS::Testing::setFaultHook(nullptr);
        // the snapshot already says 1, but format.json still says 2, and the most restrictive header wins
        QCOMPARE(writerVersionIn(path("store/refs.json"), "format"), 1);
        QCOMPARE(writerVersionIn(path("store/format.json")), 2);
        const auto formatOnDisk = StoreFormatFile::read(path("store"));
        QVERIFY(formatOnDisk && formatOnDisk->has_value());
        QCOMPARE(formatOnDisk->value().accessFor(1), StoreAccess::ReadOnly);

        // the next start finishes lowering it
        reopen();
        QCOMPARE(writerVersionIn(path("store/format.json")), 1);
        QCOMPARE(accessForVersion1(), StoreAccess::Writable);
    }
};

QTEST_GUILESS_MAIN(ConvertTest)

#include "Convert_test.moc"
