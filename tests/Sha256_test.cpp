#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTest>

#include "contentstore/Sha256.h"

namespace {
using Implementation = Sha256::Implementation;

QString name(Implementation implementation)
{
    switch (implementation) {
        case Implementation::X86:
            return "x86";
        case Implementation::Arm:
            return "arm";
        default:
            return "portable";
    }
}

// The hash of data, added in random pieces
QByteArray hashInPieces(const QByteArray& data, QRandomGenerator& random)
{
    Sha256 hash;
    qsizetype offset = 0;
    while (offset < data.size()) {
        const auto piece = std::min<qsizetype>(data.size() - offset, random.bounded(200) == 0 ? 100000 : random.bounded(130));
        hash.addData(QByteArrayView(data.constData() + offset, piece));
        offset += piece;
    }
    return hash.result();
}
}  // namespace

class Sha256Test : public QObject {
    Q_OBJECT

   private slots:
    void cleanup() { Sha256::setImplementationForTesting(Sha256::implementation()); }

    void test_implementations_data()
    {
        QTest::addColumn<Implementation>("implementation");
        for (const auto implementation : { Implementation::Portable, Implementation::X86, Implementation::Arm }) {
            if (Sha256::isAvailable(implementation)) {
                QTest::newRow(qPrintable(name(implementation))) << implementation;
            }
        }
    }

    void test_implementations()
    {
        QFETCH(Implementation, implementation);
        Sha256::setImplementationForTesting(implementation);
        QCOMPARE(Sha256::implementation(), implementation);

        // the test vectors of FIPS 180-2
        const auto hex = [](const QByteArray& data) {
            Sha256 hash;
            hash.addData(data);
            return hash.hexResult();
        };
        QCOMPARE(hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        QCOMPARE(hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        QCOMPARE(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        QCOMPARE(hex(QByteArray(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        // every length around the block and padding boundaries, and some large ones, in random pieces
        auto random = QRandomGenerator(12345);
        QList<qsizetype> sizes;
        for (qsizetype size = 0; size <= 2000; size++) {
            sizes.append(size);
        }
        sizes << 65536 << 1048576 + 13 << 3 * 1048576 + 55;
        for (const auto size : sizes) {
            QByteArray data(size, Qt::Uninitialized);
            for (auto& byte : data) {
                byte = static_cast<char>(random.bounded(256));
            }
            const auto expected = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
            const auto actual = hashInPieces(data, random);
            if (actual != expected) {
                QFAIL(qPrintable(QString("Wrong hash of %1 bytes").arg(size)));
            }
        }
    }

    void test_throughput()
    {
        const QByteArray data(64 * 1024 * 1024, 'x');
        for (const auto implementation : { Implementation::Portable, Implementation::X86, Implementation::Arm }) {
            if (!Sha256::isAvailable(implementation)) {
                continue;
            }
            Sha256::setImplementationForTesting(implementation);
            QElapsedTimer timer;
            timer.start();
            Sha256 hash;
            hash.addData(data);
            hash.result();
            qInfo() << name(implementation) << ":" << 64 * 1000 / std::max<qint64>(timer.elapsed(), 1) << "MB/s";
        }
        QElapsedTimer timer;
        timer.start();
        QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        qInfo() << "QCryptographicHash:" << 64 * 1000 / std::max<qint64>(timer.elapsed(), 1) << "MB/s";
        qInfo() << "used:" << name(Sha256::implementation());
    }
};

QTEST_GUILESS_MAIN(Sha256Test)

#include "Sha256_test.moc"
