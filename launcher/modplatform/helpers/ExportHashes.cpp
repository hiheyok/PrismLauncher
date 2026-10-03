#include "ExportHashes.h"

#include <QJsonObject>

namespace ExportHashes {

namespace {
bool matches(const FileHashes& fileHashes, Hashing::Algorithm algorithm, const QString& expected)
{
    const auto actual = fileHashes.value(algorithm);
    return !actual.isEmpty() && !expected.isEmpty() && actual.compare(expected, Qt::CaseInsensitive) == 0;
}
}  // namespace

bool matchesMetadata(const Metadata::ModStruct& metadata, const FileHashes& fileHashes)
{
    const auto algorithm = Hashing::algorithmFromString(metadata.hashFormat);
    switch (algorithm) {
        case Hashing::Algorithm::Md5:
        case Hashing::Algorithm::Sha1:
        case Hashing::Algorithm::Sha256:
        case Hashing::Algorithm::Sha512:
            return matches(fileHashes, algorithm, metadata.hash);
        default:
            // murmur2 ignores whitespace bytes, so it can't tell if the file was changed
            return false;
    }
}

bool matchesCurseForgeFile(const QJsonArray& curseForgeHashes, const FileHashes& fileHashes)
{
    for (const auto& entry : curseForgeHashes) {
        const auto hashObj = entry.toObject();
        // CurseForge hash algorithms: 1 is sha1, 2 is md5
        switch (hashObj["algo"].toInt()) {
            case 1:
                if (matches(fileHashes, Hashing::Algorithm::Sha1, hashObj["value"].toString())) {
                    return true;
                }
                break;
            case 2:
                if (matches(fileHashes, Hashing::Algorithm::Md5, hashObj["value"].toString())) {
                    return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

}  // namespace ExportHashes
