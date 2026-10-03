#include "HashUtils.h"

#include <QBuffer>
#include <QDebug>
#include <QFile>
#include <QtConcurrentRun>

#include <MurmurHash2.h>

namespace Hashing {

Hasher::Ptr createHasher(QString file_path, ModPlatform::ResourceProvider provider)
{
    switch (provider) {
        case ModPlatform::ResourceProvider::MODRINTH:
            return makeShared<Hasher>(file_path,
                                      ModPlatform::ProviderCapabilities::hashType(ModPlatform::ResourceProvider::MODRINTH).first());
        case ModPlatform::ResourceProvider::FLAME:
            return makeShared<Hasher>(file_path, Algorithm::Murmur2);
        default:
            qCritical() << "[Hashing]" << "Unrecognized mod platform!";
            return nullptr;
    }
}

Hasher::Ptr createHasher(QString file_path, QString type)
{
    return makeShared<Hasher>(file_path, type);
}

class QIODeviceReader : public Murmur2::Reader {
   public:
    QIODeviceReader(QIODevice* device) : m_device(device), m_start(device->pos()) {}
    virtual ~QIODeviceReader() = default;
    virtual int read(char* s, int n)
    {
        const auto bytesRead = m_device->read(s, n);
        if (bytesRead < 0) {
            m_failed = true;
            return 0;
        }
        m_bytesRead += bytesRead;
        return static_cast<int>(bytesRead);
    }
    // a failed read ends the hash instead of retrying forever
    virtual bool eof() { return m_failed || m_device->atEnd(); }
    virtual void goToBeginning()
    {
        checkComplete();
        m_bytesRead = 0;
        if (!m_device->seek(m_start)) {
            m_failed = true;
        }
    }
    virtual void close() { m_device->close(); }

    // whether every pass read the whole device without errors
    bool succeeded()
    {
        checkComplete();
        return !m_failed;
    }

   private:
    void checkComplete()
    {
        if (!m_device->isSequential() && m_bytesRead != m_device->size() - m_start) {
            m_failed = true;
        }
    }

    QIODevice* m_device;
    qint64 m_start;
    qint64 m_bytesRead = 0;
    bool m_failed = false;
};

namespace {
// Adds the rest of the device to the hash, failing on a read error or if less than the whole file was read
bool addDevice(QCryptographicHash& hash, QIODevice* device)
{
    constexpr qint64 chunkSize = 64 * 1024;
    QByteArray buffer(chunkSize, Qt::Uninitialized);
    const auto start = device->pos();
    qint64 total = 0;
    while (!device->atEnd()) {
        const auto bytesRead = device->read(buffer.data(), chunkSize);
        if (bytesRead < 0) {
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), bytesRead));
        total += bytesRead;
    }
    return device->isSequential() || total == device->size() - start;
}
}  // namespace

QString algorithmToString(Algorithm type)
{
    switch (type) {
        case Algorithm::Md4:
            return "md4";
        case Algorithm::Md5:
            return "md5";
        case Algorithm::Sha1:
            return "sha1";
        case Algorithm::Sha256:
            return "sha256";
        case Algorithm::Sha512:
            return "sha512";
        case Algorithm::Murmur2:
            return "murmur2";
        // case Algorithm::Unknown:
        default:
            break;
    }
    return "unknown";
}

Algorithm algorithmFromString(QString type)
{
    if (type == "md4")
        return Algorithm::Md4;
    if (type == "md5")
        return Algorithm::Md5;
    if (type == "sha1")
        return Algorithm::Sha1;
    if (type == "sha256")
        return Algorithm::Sha256;
    if (type == "sha512")
        return Algorithm::Sha512;
    if (type == "murmur2")
        return Algorithm::Murmur2;
    return Algorithm::Unknown;
}

QString hash(QIODevice* device, Algorithm type)
{
    if (!device->isOpen() && !device->open(QFile::ReadOnly))
        return "";
    QCryptographicHash::Algorithm alg = QCryptographicHash::Sha1;
    switch (type) {
        case Algorithm::Md4:
            alg = QCryptographicHash::Algorithm::Md4;
            break;
        case Algorithm::Md5:
            alg = QCryptographicHash::Algorithm::Md5;
            break;
        case Algorithm::Sha1:
            alg = QCryptographicHash::Algorithm::Sha1;
            break;
        case Algorithm::Sha256:
            alg = QCryptographicHash::Algorithm::Sha256;
            break;
        case Algorithm::Sha512:
            alg = QCryptographicHash::Algorithm::Sha512;
            break;
        case Algorithm::Murmur2: {  // CF-specific
            auto should_filter_out = [](char c) { return (c == 9 || c == 10 || c == 13 || c == 32); };
            auto reader = std::make_unique<QIODeviceReader>(device);
            auto result = QString::number(Murmur2::hash(reader.get(), 4 * MiB, should_filter_out));
            const bool succeeded = reader->succeeded();
            device->close();
            if (!succeeded) {
                qCritical() << "Failed to read file to create hash!";
                return "";
            }
            return result;
        }
        case Algorithm::Unknown:
            device->close();
            return "";
    }

    QCryptographicHash hash(alg);
    if (!addDevice(hash, device)) {
        qCritical() << "Failed to read file to create hash!";
        device->close();
        return "";
    }

    Q_ASSERT(hash.result().length() == hash.hashLength(alg));
    auto result = hash.result().toHex();
    device->close();
    return result;
}

QString hash(QString fileName, Algorithm type)
{
    QFile file(fileName);
    return hash(&file, type);
}

QString hash(QByteArray data, Algorithm type)
{
    QBuffer buff(&data);
    return hash(&buff, type);
}

void Hasher::executeTask()
{
    m_future = QtConcurrent::run(
        QThreadPool::globalInstance(), [](QString fileName, Algorithm type) { return hash(fileName, type); }, m_path, m_alg);
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, [this] {
        if (m_future.isCanceled()) {
            emitAborted();
        } else if (m_result = m_future.result(); m_result.isEmpty()) {
            emitFailed("Empty hash!");
        } else {
            emit resultsReady(m_result);
            emitSucceeded();
        }
    });
    m_watcher.setFuture(m_future);
}

bool Hasher::abort()
{
    if (m_future.isRunning()) {
        m_future.cancel();
        // NOTE: Here we don't do `emitAborted()` because it will be done when `m_build_zip_future` actually cancels, which may not
        // occur immediately.
        return true;
    }
    return false;
}
}  // namespace Hashing
