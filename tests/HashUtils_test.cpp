#include <QBuffer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstring>

#include "modplatform/helpers/HashUtils.h"

namespace {
// A device that returns some data and then either fails or ends before its reported size
class BrokenDevice : public QIODevice {
   public:
    enum class Failure { ReadError, Truncated };

    BrokenDevice(QByteArray data, qint64 readableBytes, Failure failure)
        : m_data(std::move(data)), m_readableBytes(readableBytes), m_failure(failure)
    {}

    qint64 size() const override { return m_data.size(); }
    bool isSequential() const override { return false; }
    bool atEnd() const override { return pos() >= m_readableBytes && m_failure == Failure::Truncated; }

   protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const auto position = pos();
        if (position >= m_readableBytes) {
            return m_failure == Failure::ReadError ? -1 : 0;
        }
        const auto count = std::min(maxSize, m_readableBytes - position);
        memcpy(data, m_data.constData() + position, count);
        return count;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

   private:
    QByteArray m_data;
    qint64 m_readableBytes;
    Failure m_failure;
};

const QList<Hashing::Algorithm> ALGORITHMS{ Hashing::Algorithm::Md5, Hashing::Algorithm::Sha1, Hashing::Algorithm::Sha256,
                                            Hashing::Algorithm::Sha512, Hashing::Algorithm::Murmur2 };
}  // namespace

class HashUtilsTest : public QObject {
    Q_OBJECT

    const QByteArray m_data = QByteArray(200 * 1024, 'x');

   private slots:
    void test_knownHashes()
    {
        QCOMPARE(Hashing::hash(QByteArray("abc"), Hashing::Algorithm::Sha1), "a9993e364706816aba3e25717850c26c9cd0d89d");
        QCOMPARE(Hashing::hash(QByteArray("abc"), Hashing::Algorithm::Md5), "900150983cd24fb0d6963f7d28e17f72");
    }

    void test_readErrorGivesNoHash()
    {
        for (auto algorithm : ALGORITHMS) {
            // fails after 100 KiB, in the middle of the data
            BrokenDevice device(m_data, 100 * 1024, BrokenDevice::Failure::ReadError);
            QVERIFY(device.open(QIODevice::ReadOnly));
            QCOMPARE(Hashing::hash(&device, algorithm), QString());
        }
    }

    void test_truncatedReadGivesNoHash()
    {
        for (auto algorithm : ALGORITHMS) {
            // reports the full size but ends after 100 KiB
            BrokenDevice device(m_data, 100 * 1024, BrokenDevice::Failure::Truncated);
            QVERIFY(device.open(QIODevice::ReadOnly));
            QCOMPARE(Hashing::hash(&device, algorithm), QString());
        }
    }

    void test_completeReadGivesHash()
    {
        for (auto algorithm : ALGORITHMS) {
            BrokenDevice device(m_data, m_data.size(), BrokenDevice::Failure::Truncated);
            QVERIFY(device.open(QIODevice::ReadOnly));
            QCOMPARE(Hashing::hash(&device, algorithm), Hashing::hash(m_data, algorithm));
            QVERIFY(!Hashing::hash(m_data, algorithm).isEmpty());
        }
    }

    void test_file()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("file.jar");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(m_data);
        file.close();

        for (auto algorithm : ALGORITHMS) {
            QCOMPARE(Hashing::hash(path, algorithm), Hashing::hash(m_data, algorithm));
        }
        QCOMPARE(Hashing::hash(dir.filePath("missing.jar"), Hashing::Algorithm::Sha1), QString());
    }

    void test_hasherFailsWithoutHash()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Hashing::Hasher hasher(dir.filePath("missing.jar"), Hashing::Algorithm::Sha1);
        QSignalSpy failed(&hasher, &Task::failed);
        QSignalSpy results(&hasher, &Hashing::Hasher::resultsReady);
        hasher.start();
        QVERIFY(failed.wait());
        QCOMPARE(results.count(), 0);
    }
};

QTEST_GUILESS_MAIN(HashUtilsTest)

#include "HashUtils_test.moc"
