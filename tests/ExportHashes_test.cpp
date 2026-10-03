#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "modplatform/helpers/ExportHashes.h"

namespace {
ExportHashes::FileHashes hashesOf(const QByteArray& data)
{
    ExportHashes::FileHashes hashes;
    for (auto algorithm : { Hashing::Algorithm::Murmur2, Hashing::Algorithm::Md5, Hashing::Algorithm::Sha1, Hashing::Algorithm::Sha512 }) {
        hashes.insert(algorithm, Hashing::hash(data, algorithm));
    }
    return hashes;
}

Metadata::ModStruct metadataWith(const QString& hashFormat, const QString& hash)
{
    Metadata::ModStruct metadata;
    metadata.hashFormat = hashFormat;
    metadata.hash = hash;
    return metadata;
}

QJsonArray curseForgeHashes(const QString& sha1, const QString& md5)
{
    QJsonArray hashes;
    if (!sha1.isEmpty()) {
        hashes.append(QJsonObject{ { "value", sha1 }, { "algo", 1 } });
    }
    if (!md5.isEmpty()) {
        hashes.append(QJsonObject{ { "value", md5 }, { "algo", 2 } });
    }
    return hashes;
}
}  // namespace

class ExportHashesTest : public QObject {
    Q_OBJECT

    const QByteArray m_original = "some mod content";
    // only differs from the original in whitespace bytes, which murmur2 ignores
    const QByteArray m_edited = "some\tmod\ncontent";

   private slots:
    void test_editedFileHasSameFingerprint()
    {
        QCOMPARE(hashesOf(m_edited).value(Hashing::Algorithm::Murmur2), hashesOf(m_original).value(Hashing::Algorithm::Murmur2));
        QVERIFY(hashesOf(m_edited).value(Hashing::Algorithm::Sha1) != hashesOf(m_original).value(Hashing::Algorithm::Sha1));
    }

    void test_metadataMatchesUnchangedFile()
    {
        const auto hashes = hashesOf(m_original);
        QVERIFY(ExportHashes::matchesMetadata(metadataWith("sha1", hashes.value(Hashing::Algorithm::Sha1)), hashes));
        QVERIFY(ExportHashes::matchesMetadata(metadataWith("sha512", hashes.value(Hashing::Algorithm::Sha512)), hashes));
        QVERIFY(ExportHashes::matchesMetadata(metadataWith("md5", hashes.value(Hashing::Algorithm::Md5)), hashes));
        QVERIFY(ExportHashes::matchesMetadata(metadataWith("sha1", hashes.value(Hashing::Algorithm::Sha1).toUpper()), hashes));
    }

    void test_metadataRejectsEditedFile()
    {
        const auto original = hashesOf(m_original);
        const auto edited = hashesOf(m_edited);
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("sha1", original.value(Hashing::Algorithm::Sha1)), edited));
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("sha512", original.value(Hashing::Algorithm::Sha512)), edited));
    }

    void test_metadataRejectsUntrustedHashes()
    {
        const auto hashes = hashesOf(m_original);
        // murmur2 can't detect whitespace edits
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("murmur2", hashes.value(Hashing::Algorithm::Murmur2)), hashes));
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("sha1", ""), hashes));
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("", hashes.value(Hashing::Algorithm::Sha1)), hashes));
        // the file hash for the format was not computed
        QVERIFY(!ExportHashes::matchesMetadata(metadataWith("sha256", Hashing::hash(m_original, Hashing::Algorithm::Sha256)), hashes));
    }

    void test_curseForgeFileMatchesUnchangedFile()
    {
        const auto hashes = hashesOf(m_original);
        const auto sha1 = hashes.value(Hashing::Algorithm::Sha1);
        const auto md5 = hashes.value(Hashing::Algorithm::Md5);
        QVERIFY(ExportHashes::matchesCurseForgeFile(curseForgeHashes(sha1, md5), hashes));
        QVERIFY(ExportHashes::matchesCurseForgeFile(curseForgeHashes(sha1, ""), hashes));
        QVERIFY(ExportHashes::matchesCurseForgeFile(curseForgeHashes("", md5), hashes));
    }

    void test_curseForgeFileRejectsSameFingerprintDifferentContent()
    {
        const auto original = hashesOf(m_original);
        const auto edited = hashesOf(m_edited);
        QVERIFY(!ExportHashes::matchesCurseForgeFile(
            curseForgeHashes(original.value(Hashing::Algorithm::Sha1), original.value(Hashing::Algorithm::Md5)), edited));
    }

    void test_curseForgeFileRejectsMissingHashes()
    {
        const auto hashes = hashesOf(m_original);
        QVERIFY(!ExportHashes::matchesCurseForgeFile({}, hashes));
        QJsonArray unknownAlgorithm{ QJsonObject{ { "value", hashes.value(Hashing::Algorithm::Sha1) }, { "algo", 3 } } };
        QVERIFY(!ExportHashes::matchesCurseForgeFile(unknownAlgorithm, hashes));
    }
};

QTEST_GUILESS_MAIN(ExportHashesTest)

#include "ExportHashes_test.moc"
