#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>

#include <filesystem>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "contentstore/ContentStore.h"
#include "contentstore/ObjectFiles.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
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

bool hardLink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_hard_link(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}
}  // namespace

class IngestTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("ingest_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    QStringList leftovers() const { return QDir(m_store->temporaryDir()).entryList(QDir::Files | QDir::Hidden | QDir::System); }

   private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_store.reset();
        // stored files are read-only
        QDirIterator it(m_dir.path(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        QVERIFY(QDir(m_dir.path()).mkpath("downloads"));
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        FS::Testing::setCallRecorder(nullptr);
    }

    void test_copy()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "mod contents"));
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy);
        QVERIFY2(result, result ? "" : qPrintable(result.error()));
        QCOMPARE(result->hash, sha256Of("mod contents"));
        QVERIFY(!result->reusedObject);

        const auto object = m_store->objectPath(result->hash);
        QCOMPARE(object, QDir(m_store->objectsDir()).filePath(result->hash.left(2) + "/" + result->hash));
        QCOMPARE(readFile(object), "mod contents");
        QVERIFY(!QFileInfo(object).isWritable());
        // the source is untouched
        QCOMPARE(readFile(path("downloads/mod.jar")), "mod contents");
        QVERIFY(QFileInfo(path("downloads/mod.jar")).isWritable());
        QVERIFY(leftovers().isEmpty());

        const auto& entry = m_store->table().entries()[result->hash];
        QCOMPARE(entry.size, 12);
        QCOMPARE(entry.current->id, result->generation);
        QVERIFY(entry.current->identity.sameFile(FS::fileId(object).value()));
    }

    void test_identicalFilesAreStoredOnce()
    {
        QVERIFY(writeFile(path("downloads/a.jar"), "same"));
        QVERIFY(writeFile(path("downloads/b.jar"), "same"));
        QVERIFY(writeFile(path("downloads/c.jar"), "different"));
        const auto a = m_store->ingest(path("downloads/a.jar"), ContentStore::IngestMode::Copy);
        const auto b = m_store->ingest(path("downloads/b.jar"), ContentStore::IngestMode::Copy);
        const auto c = m_store->ingest(path("downloads/c.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(a && b && c);
        QCOMPARE(a->hash, b->hash);
        QVERIFY(b->reusedObject);
        QVERIFY(a->hash != c->hash);
        QCOMPARE(m_store->table().entries().size(), 2);
        QVERIFY(leftovers().isEmpty());
    }

    void test_move()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "moved"));
        const auto sourceId = FS::fileId(path("downloads/mod.jar")).value();
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move);
        QVERIFY(result);
        QVERIFY(!QFileInfo::exists(path("downloads/mod.jar")));
        // the same file, moved without copying
        QCOMPARE(FS::fileId(m_store->objectPath(result->hash)).value(), sourceId);
    }

    void test_linkIn()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "linked"));
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::LinkIn);
        QVERIFY(result);
        const auto object = m_store->objectPath(result->hash);
        // the downloaded file became the stored file
        QCOMPARE(FS::fileId(object).value(), FS::fileId(path("downloads/mod.jar")).value());
        QCOMPARE(FS::hardLinkCount(object), uintmax_t(2));
        QVERIFY(!QFileInfo(object).isWritable());
    }

    void test_linkInReusesExistingObject()
    {
        QVERIFY(writeFile(path("downloads/first.jar"), "same"));
        QVERIFY(writeFile(path("downloads/second.jar"), "same"));
        const auto first = m_store->ingest(path("downloads/first.jar"), ContentStore::IngestMode::Copy);
        const auto second = m_store->ingest(path("downloads/second.jar"), ContentStore::IngestMode::LinkIn);
        QVERIFY(first && second);
        QVERIFY(second->reusedObject);
        // not part of the store, so it isn't left read-only
        QVERIFY(QFileInfo(path("downloads/second.jar")).isWritable());
        QCOMPARE(FS::hardLinkCount(path("downloads/second.jar")), uintmax_t(1));
        QVERIFY(leftovers().isEmpty());
    }

    void test_alreadyStoredFileIsFound()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "stored"));
        const auto stored = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(stored);
        QVERIFY(hardLink(m_store->objectPath(stored->hash), path("downloads/link.jar")));
        const auto again = m_store->ingest(path("downloads/link.jar"), ContentStore::IngestMode::LinkIn);
        QVERIFY(again);
        QVERIFY(again->reusedObject);
        QCOMPARE(again->hash, stored->hash);
    }

    void test_precomputedDigest()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "downloaded"));
        const ContentStore::PrecomputedDigest digest{ sha256Of("downloaded"), FS::identity(path("downloads/mod.jar")).value() };
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move, digest);
        QVERIFY(result);
        QCOMPARE(result->hash, sha256Of("downloaded"));
    }

    void test_wrongDigestOfUnchangedFileIsRejected()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "downloaded"));
        const ContentStore::PrecomputedDigest digest{ sha256Of("something else"), FS::identity(path("downloads/mod.jar")).value() };
        // with Copy the file is hashed anyway, so the wrong digest is caught
        QVERIFY(!m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy, digest));
        QCOMPARE(readFile(path("downloads/mod.jar")), "downloaded");
        QVERIFY(m_store->table().entries().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_digestOfChangedFileIsIgnored()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "first"));
        const ContentStore::PrecomputedDigest digest{ sha256Of("first"), FS::identity(path("downloads/mod.jar")).value() };
        QTest::qSleep(20);
        QVERIFY(writeFile(path("downloads/mod.jar"), "second, longer"));
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move, digest);
        QVERIFY(result);
        QCOMPARE(result->hash, sha256Of("second, longer"));
    }

    void test_metadataChangeRehashesOnce()
    {
        QVERIFY(writeFile(path("downloads/a.jar"), "same"));
        QVERIFY(writeFile(path("downloads/b.jar"), "same"));
        QVERIFY(writeFile(path("downloads/c.jar"), "same"));
        const auto first = m_store->ingest(path("downloads/a.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(first);
        // something outside the store changes the stored file's metadata
        const auto object = m_store->objectPath(first->hash);
        makeWritable(object);
        QVERIFY(ObjectFiles::makeReadOnly(object));

        const auto second = m_store->ingest(path("downloads/b.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(second && second->reusedObject && second->rehashedObject);
        const auto third = m_store->ingest(path("downloads/c.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(third && third->reusedObject && !third->rehashedObject);
    }

    void test_damagedObjectIsNotReused()
    {
        QVERIFY(writeFile(path("downloads/a.jar"), "original"));
        QVERIFY(writeFile(path("downloads/b.jar"), "original"));
        const auto first = m_store->ingest(path("downloads/a.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(first);
        const auto object = m_store->objectPath(first->hash);
        makeWritable(object);
        QVERIFY(writeFile(object, "tampered"));

        QVERIFY(!m_store->ingest(path("downloads/b.jar"), ContentStore::IngestMode::Copy));
        QCOMPARE(readFile(path("downloads/b.jar")), "original");
        QVERIFY(leftovers().isEmpty());
    }

    void test_failedPublishRestoresMovedFile()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "keep me"));
        int replaces = 0;
        // the move into tmp succeeds, the swap into objects fails
        FS::Testing::setFaultHook([&replaces](FS::Testing::Operation operation, const QString&) {
            return operation == FS::Testing::Operation::Replace && replaces++ == 1;
        });
        QVERIFY(!m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move));
        FS::Testing::setFaultHook(nullptr);
        QCOMPARE(readFile(path("downloads/mod.jar")), "keep me");
        QVERIFY(m_store->table().entries().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_publicationOrder()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "ordered"));
        QList<QPair<FS::Testing::Operation, QString>> calls;
        FS::Testing::setCallRecorder(
            [&calls](FS::Testing::Operation operation, const QString& path) { calls.append({ operation, path }); });
        const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy);
        FS::Testing::setCallRecorder(nullptr);
        QVERIFY(result);

        // the data is flushed before the swap into objects, and the directory after it
        using Op = FS::Testing::Operation;
        const auto object = m_store->objectPath(result->hash);
        const auto replace =
            std::find_if(calls.begin(), calls.end(), [&](const auto& call) { return call.first == Op::Replace && call.second == object; });
        QVERIFY(replace != calls.end());
        QVERIFY(std::any_of(calls.begin(), replace, [](const auto& call) { return call.first == Op::FlushFile; }));
        QVERIFY(std::any_of(replace, calls.end(), [&](const auto& call) {
            return call.first == Op::FlushDir && call.second == QFileInfo(object).absolutePath();
        }));
    }

    void test_concurrentIngest()
    {
        constexpr int count = 8;
        for (int i = 0; i < count; i++) {
            QVERIFY(writeFile(path(QString("downloads/%1.jar").arg(i)), "concurrent"));
        }
        QList<QFuture<Result<ContentStore::IngestResult>>> futures;
        for (int i = 0; i < count; i++) {
            futures.append(QtConcurrent::run(
                [this, i] { return m_store->ingest(path(QString("downloads/%1.jar").arg(i)), ContentStore::IngestMode::Copy); }));
        }
        int stored = 0;
        for (auto& future : futures) {
            const auto result = future.takeResult();
            QVERIFY2(result, result ? "" : qPrintable(result.error()));
            stored += result->reusedObject ? 0 : 1;
        }
        QCOMPARE(stored, 1);
        QCOMPARE(m_store->table().entries().size(), 1);
        QVERIFY(leftovers().isEmpty());
    }

    void test_leases()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "leased"));
        const auto hash = sha256Of("leased");
        {
            auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy);
            QVERIFY(result);
            QCOMPARE(m_store->leaseCount(hash), 1);
            auto moved = std::move(result->lease);
            QCOMPARE(m_store->leaseCount(hash), 1);
        }
        QCOMPARE(m_store->leaseCount(hash), 0);
    }

    void test_storeSurvivesReopen()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "persistent"));
        QString hash;
        {
            // the result holds a lease, which must be released before the store is closed
            const auto result = m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy);
            QVERIFY(result);
            hash = result->hash;
        }
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
        QVERIFY(m_store->table().entries().contains(hash));
        QVERIFY(writeFile(path("downloads/again.jar"), "persistent"));
        const auto again = m_store->ingest(path("downloads/again.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(again && again->reusedObject && !again->rehashedObject);
    }

    void test_failedMoveRestoresPermissions()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "keep me writable"));
        int replaces = 0;
        FS::Testing::setFaultHook([&replaces](FS::Testing::Operation operation, const QString&) {
            return operation == FS::Testing::Operation::Replace && replaces++ == 1;
        });
        QVERIFY(!m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move));
        FS::Testing::setFaultHook(nullptr);
        QCOMPARE(readFile(path("downloads/mod.jar")), "keep me writable");
        QVERIFY(QFileInfo(path("downloads/mod.jar")).isWritable());
    }

    void test_digestIsCheckedAgainstStoredContents()
    {
        // the digest matches the file's identity, but the stored contents are hashed anyway
        QVERIFY(writeFile(path("downloads/mod.jar"), "downloaded"));
        const ContentStore::PrecomputedDigest digest{ sha256Of("not what was downloaded"),
                                                      FS::identity(path("downloads/mod.jar")).value() };
        QVERIFY(!m_store->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Move, digest));
        QCOMPARE(readFile(path("downloads/mod.jar")), "downloaded");
        QVERIFY(QFileInfo(path("downloads/mod.jar")).isWritable());
        QVERIFY(m_store->table().entries().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }
};

QTEST_GUILESS_MAIN(IngestTest)

#include "Ingest_test.moc"
