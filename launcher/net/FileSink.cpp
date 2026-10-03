// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "FileSink.h"
#include <expected>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"

#include "net/Logging.h"

namespace Net {

namespace {
// QSaveFile writes through a symbolic link at the target and refuses read-only targets,
// so downloads over them, and over files with other hard links, are written next to them and swapped in
bool needsSwap(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        return true;
    }
    return info.exists() && (!info.isWritable() || FS::hardLinkCount(path) > 1);
}
}  // namespace

auto FileSink::init(QNetworkRequest& request) -> InitResult
{
    auto result = initCache(request);
    if (!result || *result != InitType::Ok) {
        return result;
    }

    // create a new save file and open it for writing
    if (!FS::ensureFilePathExists(m_filename)) {
        qCCritical(taskNetLogC) << "Could not create folder for " + m_filename;
        return std::unexpected("Could not create folder");
    }

    m_wroteAnyData = false;
    m_swapPath.clear();
    m_swapGuard.reset();
    if (needsSwap(m_filename)) {
        m_swapPath = m_filename + ".prism-dl";
        m_swapGuard.reset(new PSaveFile(m_filename));
    }
    m_outputFile.reset(new PSaveFile(m_swapPath.isEmpty() ? m_filename : m_swapPath));
    if (!m_outputFile->open(QIODevice::WriteOnly)) {
        const auto error = QString("Could not open %1 for writing: %2").arg(m_filename).arg(m_outputFile->errorString());
        qCCritical(taskNetLogC) << error;
        return std::unexpected(error);
    }

    initAllValidators();
    return InitType::Ok;
}

auto FileSink::write(const QByteArray& data) -> Result<>
{
    writeAllValidators(data);
    if (m_outputFile->write(data) != data.size()) {
        QString error = QString("Failed writing into %1: %2").arg(m_filename);
        if (m_outputFile->error() == QFileDevice::NoError) {
            error = error.arg("Validators failed");
        } else {
            error = error.arg(m_outputFile->errorString());
        }
        qCCritical(taskNetLogC) << error;
        m_outputFile->cancelWriting();
        m_outputFile.reset();
        m_wroteAnyData = false;
        return std::unexpected(error);
    }

    m_wroteAnyData = true;
    return {};
}

void FileSink::abort()
{
    if (m_outputFile) {
        m_outputFile->cancelWriting();
    }
    failAllValidators();
}

auto FileSink::finalize(QNetworkReply& reply) -> Result<>
{
    bool gotFile = false;
    QVariant statusCodeV = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
    bool validStatus = false;
    int statusCode = statusCodeV.toInt(&validStatus);
    if (validStatus) {
        // this leaves out 304 Not Modified
        gotFile = statusCode == 200 || statusCode == 203;
    }

    // if we wrote any data to the save file, we try to commit the data to the real file.
    // if it actually got a proper file, we write it even if it was empty
    if (gotFile || m_wroteAnyData) {
        // ask validators for data consistency
        // we only do this for actual downloads, not 'your data is still the same' cache hits
        auto result = finalizeAllValidators();
        if (!result) {
            return result;
        }

        // nothing went wrong...
        if (!m_outputFile->commit()) {
            const auto error = QString("Failed to commit changes to %1: %2").arg(m_filename).arg(m_outputFile->errorString());
            qCCritical(taskNetLogC) << error;
            m_outputFile->cancelWriting();
            return std::unexpected(error);
        }
        if (!m_swapPath.isEmpty()) {
            if (auto swapped = FS::replaceFile(m_swapPath, m_filename); !swapped) {
                qCCritical(taskNetLogC) << swapped.error();
                QFile::remove(m_swapPath);
                return std::unexpected(swapped.error());
            }
        }
    }

    // then get rid of the save file
    m_outputFile.reset();
    m_swapGuard.reset();

    return finalizeCache(reply);
}

bool FileSink::hasLocalData()
{
    QFileInfo info(m_filename);
    return info.exists() && info.size() != 0;
}
}  // namespace Net
