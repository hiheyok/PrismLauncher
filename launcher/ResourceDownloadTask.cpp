// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022-2023 flowln <flowlnlnln@gmail.com>
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ResourceDownloadTask.h"

#include <utility>

#include "Application.h"

#include <QUuid>

#include "FileSystem.h"
#include "contentstore/SharedContent.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ResourceFolderModel.h"

#include "minecraft/mod/ShaderPackFolderModel.h"
#include "modplatform/ModIndex.h"
#include "modplatform/helpers/HashUtils.h"
#include "net/ApiRequest.h"
#include "net/ChecksumValidator.h"
#include "tasks/FunctionTask.h"

namespace {
Net::ModrinthDownloadMeta createModrinthMeta(MinecraftInstance* instance, QString reason, QString dependentOn)
{
    auto* profile = instance->getPackProfile();
    if (!profile) {
        return {};
    }

    auto loaders = profile->getModLoadersList();

    return { .reason = std::move(reason),
             .gameVersion = profile->getComponentVersion("net.minecraft"),
             .loader = !loaders.isEmpty() ? ModPlatform::getModLoaderAsString(loaders.first()) : "",
             .dependentOn = std::move(dependentOn) };
}
}  // namespace

ResourceDownloadTask::ResourceDownloadTask(ModPlatform::IndexedPack::Ptr pack,
                                           ModPlatform::IndexedVersion version,
                                           ResourceFolderModel* packs,
                                           bool isIndexed,
                                           QString downloadReason,
                                           QString dependentOn)
    : m_pack(std::move(pack))
    , m_pack_version(std::move(version))
    , m_pack_model(packs)
    , m_downloadReason(std::move(downloadReason))
    , m_dependentOn(std::move(dependentOn))
{
    if (isIndexed) {
        m_update_task.reset(new LocalResourceUpdateTask(m_pack_model->indexDir(), *m_pack, m_pack_version));
        connect(m_update_task.get(), &LocalResourceUpdateTask::hasOldResource, this, &ResourceDownloadTask::hasOldResource);

        addTask(m_update_task);
    }

    addTask(makeShared<FunctionTask>([this] { return prepareDownload(); }));

    m_filesNetJob.reset(new NetJob(tr("Resource download"), APPLICATION->network()));
    m_filesNetJob->setStatus(tr("Downloading resource:\n%1").arg(m_pack_version.downloadUrl));
    connect(m_filesNetJob.get(), &NetJob::progress, this, &ResourceDownloadTask::downloadProgressChanged);
    connect(m_filesNetJob.get(), &NetJob::stepProgress, this, &ResourceDownloadTask::propagateStepProgress);
    connect(m_filesNetJob.get(), &NetJob::failed, this, &ResourceDownloadTask::downloadFailed);

    addTask(m_filesNetJob);
    addTask(makeShared<FunctionTask>([this] { return finishDownload(); }));
}

QString ResourceDownloadTask::gameRelativePath(const QString& fileName) const
{
    auto* instance = m_pack_model->instance();
    return instance ? QDir(instance->gameRoot()).relativeFilePath(m_pack_model->dir().absoluteFilePath(fileName)) : QString();
}

Result<> ResourceDownloadTask::prepareDownload()
{
    // the task can be restarted after failing, but the download only needs to be added once
    if (m_downloadPrepared) {
        return {};
    }
    m_downloadPrepared = true;

    // Shared, unless the user keeps this file, or the one it updates, local. Then it goes to the store's temporary folder,
    // and is linked into place once it is stored.
    const auto destination = m_pack_model->dir().absoluteFilePath(getFilename());
    auto* instance = m_pack_model->instance();
    const auto oldFilename = std::get<1>(to_delete);
    if (auto* store = SharedContent::storeFor(instance);
        store && QDir().mkpath(m_pack_model->dir().absolutePath()) &&
        QDir(instance->gameRoot()).relativeFilePath(destination).count('/') == 1 &&
        !SharedContent::isExcluded(instance, gameRelativePath(getFilename())) &&
        (oldFilename.isEmpty() || !SharedContent::isExcluded(instance, gameRelativePath(oldFilename)))) {
        m_sharedDownload = QDir(store->temporaryDir()).filePath("download-" + QUuid::createUuid().toString(QUuid::Id128));
    }

    auto action = Net::ApiRequest::makeFile(m_pack_version.downloadUrl, m_sharedDownload.isEmpty() ? destination : m_sharedDownload,
                                            Net::Request::Option::NoOptions,
                                            createModrinthMeta(m_pack_model->instance(), m_downloadReason, m_dependentOn));
    if (!m_pack_version.hashType.isEmpty() && !m_pack_version.hash.isEmpty()) {
        switch (Hashing::algorithmFromString(m_pack_version.hashType)) {
            case Hashing::Algorithm::Md4:
                action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Md4, m_pack_version.hash));
                break;
            case Hashing::Algorithm::Md5:
                action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Md5, m_pack_version.hash));
                break;
            case Hashing::Algorithm::Sha1:
                action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha1, m_pack_version.hash));
                break;
            case Hashing::Algorithm::Sha256:
                action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha256, m_pack_version.hash));
                break;
            case Hashing::Algorithm::Sha512:
                action->addValidator(new Net::ChecksumValidator(QCryptographicHash::Algorithm::Sha512, m_pack_version.hash));
                break;
            default:
                break;
        }
    }
    m_filesNetJob->addNetAction(action);
    return {};
}

Result<> ResourceDownloadTask::finishDownload()
{
    m_filesNetJob.reset();
    auto* instance = m_pack_model->instance();
    const auto destination = m_pack_model->dir().absoluteFilePath(getFilename());
    if (!m_sharedDownload.isEmpty()) {
        auto* store = SharedContent::storeFor(instance);
        if (!store) {
            return std::unexpected(tr("The shared store can no longer be used, so %1 wasn't installed").arg(getFilename()));
        }
        const auto placed =
            SharedContent::installFile(*store, SharedContent::destination(*store, instance->id(), instance->gameRoot(), destination),
                                       m_sharedDownload, ContentStore::IngestMode::Move);
        if (!placed) {
            QFile::remove(m_sharedDownload);
            return std::unexpected(tr("Could not install %1: %2").arg(getFilename(), placed.error()));
        }
    }

    auto oldName = std::get<0>(to_delete);
    auto oldFilename = std::get<1>(to_delete);

    if (oldName.isEmpty() || oldFilename == m_pack_version.fileName) {
        return {};
    }

    // a file kept local stays local under its new name
    SharedContent::renameExclusion(instance, gameRelativePath(oldFilename), gameRelativePath(getFilename()));
    m_pack_model->uninstallResource(oldFilename, true);

    // also rename the shader config file
    if (dynamic_cast<ShaderPackFolderModel*>(m_pack_model) != nullptr) {
        QFileInfo oldConfig(m_pack_model->dir(), oldFilename + ".txt");
        QFileInfo newConfig(m_pack_model->dir(), getFilename() + ".txt");

        if (oldConfig.exists() && !newConfig.exists()) {
            bool success = FS::move(oldConfig.filePath(), newConfig.filePath());

            if (!success) {
                emit logWarning(tr("Failed to rename shader config from '%1' to '%2'").arg(oldConfig.fileName(), newConfig.fileName()));
            }
        }
    }
    return {};
}

void ResourceDownloadTask::downloadFailed(QString reason)
{
    m_filesNetJob.reset();
    emitFailed(std::move(reason));
}

void ResourceDownloadTask::downloadProgressChanged(qint64 current, qint64 total)
{
    emit progress(current, total);
}

// This indirection is done so that we don't delete a mod before being sure it was
// downloaded successfully!
void ResourceDownloadTask::hasOldResource(const QString& name, const QString& filename)
{
    to_delete = { name, filename };
}

void ResourceDownloadTask::setOldResource(const QString& name, const QString& filename)
{
    to_delete = { name, filename };
}
