/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <WCDB/Assertion.hpp>
#include <WCDB/FileManager.hpp>
#include <WCDB/Frame.hpp>
#include <WCDB/Notifier.hpp>
#include <WCDB/Pager.hpp>
#include <WCDB/Path.hpp>
#include <WCDB/Serialization.hpp>
#include <WCDB/String.hpp>
#include <WCDB/Wal.hpp>

namespace WCDB {

namespace Repair {

#pragma mark - Initialize
Wal::Wal(Pager *pager)
: PagerRelated(pager)
, m_fileHandle(Path::addExtention(m_pager->getPath(), "-wal"))
, m_salt({ 0, 0 })
, m_isNativeChecksum(false)
, m_maxAllowedFrame(std::numeric_limits<int>::max())
, m_maxFrame(0)
, m_shm(this)
, m_shmLegality(true)
, m_fileSize(0)
, m_truncate(std::numeric_limits<uint32_t>::max())
{
}

const std::string &Wal::getPath() const
{
    return m_fileHandle.path;
}

MappedData Wal::acquireData(off_t offset, size_t size)
{
    WCTInnerAssert(m_fileHandle.isOpened());
    MappedData data = m_fileHandle.map(offset, size);
    if (data.size() != size) {
        if (data.size() > 0) {
            markAsCorrupted((int) ((offset - headerSize) / getFrameSize() + 1),
                            String::formatted("Acquired wal data with size: %d is less than the expected size: %d.",
                                              data.size(),
                                              size));
        } else {
            assignWithSharedThreadedError();
        }
        return MappedData::emptyData();
    }
    return data;
}

#pragma mark - Page
bool Wal::containsPage(int pageno) const
{
    WCTInnerAssert(isInitialized());
    return m_pages2Frames.find(pageno) != m_pages2Frames.end();
}

MappedData Wal::acquirePageData(int pageno)
{
    return acquirePageData(pageno, 0, getPageSize());
}

MappedData Wal::acquirePageData(int pageno, off_t offset, size_t size)
{
    WCTInnerAssert(isInitialized());
    WCTInnerAssert(containsPage(pageno));
    WCTInnerAssert(offset + size <= getPageSize());
    return acquireData(headerSize + getFrameSize() * (m_pages2Frames[pageno] - 1)
                       + Frame::headerSize + offset,
                       size);
}

int Wal::getMaxPageno() const
{
    if (m_pages2Frames.empty()) {
        return 0;
    }
    return m_pages2Frames.rbegin()->first;
}

#pragma mark - Wal
void Wal::setShmLegality(bool flag)
{
    WCTInnerAssert(!isInitialized());
    m_shmLegality = flag;
}

MappedData Wal::acquireFrameData(int frameno)
{
    WCTInnerAssert(isInitializing());
    return acquireData(headerSize + getFrameSize() * (frameno - 1), getFrameSize());
}

void Wal::setMaxAllowedFrame(int maxAllowedFrame)
{
    WCTInnerAssert(!isInitialized());
    m_maxAllowedFrame = maxAllowedFrame;
}

int Wal::getFrameCount() const
{
    WCTInnerAssert(isInitialized());
    return m_maxFrame;
}

int Wal::getPageSize() const
{
    return m_pager->getPageSize();
}

int Wal::getFrameSize() const
{
    return Frame::headerSize + getPageSize();
}

bool Wal::isNativeChecksum() const
{
    WCTInnerAssert(isInitializing() || isInitialized());
    return m_isNativeChecksum;
}

const std::pair<uint32_t, uint32_t> &Wal::getSalt() const
{
    WCTInnerAssert(isInitializing() || isInitialized());
    return m_salt;
}

bool Wal::isBigEndian()
{
    static bool s_isBigEndian = []() -> bool {
        short int n = 0x1;
        char *p = (char *) &n;
        return p[0] != 1;
    }();
    return s_isBigEndian;
}

std::pair<uint32_t, uint32_t>
Wal::calculateChecksum(const MappedData &data, const std::pair<uint32_t, uint32_t> &checksum) const
{
    WCTInnerAssert(data.size() >= 8);
    WCTInnerAssert((data.size() & 0x00000007) == 0);

    const uint32_t *iter = reinterpret_cast<const uint32_t *>(data.buffer());
    const uint32_t *end
    = reinterpret_cast<const uint32_t *>(data.buffer() + data.size());

    std::pair<uint32_t, uint32_t> result = checksum;

    if (isNativeChecksum()) {
        do {
            result.first += *iter++ + result.second;
            result.second += *iter++ + result.first;
        } while (iter < end);
    } else {
        do {
#define BYTESWAP32(x)                                                          \
    ((((x) &0x000000FF) << 24) + (((x) &0x0000FF00) << 8)                      \
     + (((x) &0x00FF0000) >> 8) + (((x) &0xFF000000) >> 24))
            result.first += BYTESWAP32(iter[0]) + result.second;
            result.second += BYTESWAP32(iter[1]) + result.first;
            iter += 2;
        } while (iter < end);
    }

    return result;
}

bool Wal::doInitialize()
{
    WCTInnerAssert(m_pager->isInitialized() || m_pager->isInitializing());

    int maxWalFrame = m_maxAllowedFrame;
    if (m_shmLegality) {
        if (!m_shm.initialize()) {
            return false;
        }
        if (m_shm.getBackfill() >= m_shm.getMaxFrame()) {
            // dispose all wal frames since they are already checkpointed.
            return true;
        }
        maxWalFrame = std::min(maxWalFrame, (int) m_shm.getMaxFrame());
    }

    bool succeed;
    std::tie(succeed, m_fileSize) = FileManager::getFileSize(getPath());
    if (m_fileSize == 0) {
        if (!succeed) {
            assignWithSharedThreadedError();
        }
        return succeed;
    }
    const int frameCountInFile = ((int) m_fileSize - headerSize) / getFrameSize();
    maxWalFrame = std::min(frameCountInFile, maxWalFrame);

    if (!m_fileHandle.open(FileHandle::Mode::ReadOnly)) {
        assignWithSharedThreadedError();
        return false;
    }
    FileManager::setFileProtectionCompleteUntilFirstUserAuthenticationIfNeeded(getPath());

    MappedData data = acquireData(0, headerSize);
    if (data.empty()) {
        return false;
    }
    Deserialization deserialization(data);
    uint32_t magic = deserialization.advance4BytesUInt();
    if ((magic & 0xFFFFFFFE) != 0x377F0682) {
        markAsCorrupted(0, String::formatted("Magic number: 0x%x is illegal.", magic));
        return false;
    }
    m_isNativeChecksum = (magic & 0x00000001) == isBigEndian();
    deserialization.seek(16);
    WCTInnerAssert(deserialization.canAdvance(4));
    m_salt.first = deserialization.advance4BytesUInt();
    WCTInnerAssert(deserialization.canAdvance(4));
    m_salt.second = deserialization.advance4BytesUInt();

    std::pair<uint32_t, uint32_t> checksum = { 0, 0 };
    checksum = calculateChecksum(data.subdata(headerSize - 2 * sizeof(uint32_t)), checksum);

    std::pair<uint32_t, uint32_t> deserializedChecksum;
    WCTInnerAssert(deserialization.canAdvance(4));
    deserializedChecksum.first = deserialization.advance4BytesUInt();
    WCTInnerAssert(deserialization.canAdvance(4));
    deserializedChecksum.second = deserialization.advance4BytesUInt();

    if (checksum != deserializedChecksum) {
        markAsCorrupted(0,
                        String::formatted("Mismatched wal checksum: %u, %u to %u, %u.",
                                          checksum.first,
                                          checksum.second,
                                          deserializedChecksum.first,
                                          deserializedChecksum.second));
        return false;
    }

    std::map<int, int> committedRecords;
    for (int frameno = 1; frameno <= maxWalFrame; ++frameno) {
        Frame frame(frameno, this);
        if (!frame.initialize()) {
            return false;
        }
        checksum = frame.calculateChecksum(checksum);
        if (checksum != frame.getChecksum()) {
            if (m_shmLegality) {
                //If the frame checksum is mismatched and shm is legal, it mean to be corrupted.
                markAsCorrupted(frameno,
                                String::formatted("Mismatched frame checksum: %u, %u to %u, %u.",
                                                  frame.getChecksum().first,
                                                  frame.getChecksum().second,
                                                  checksum.first,
                                                  checksum.second));
                return false;
            } else {
                //If the frame checksum is mismatched and shm is not legal, it mean to be disposed.
                break;
            }
        }
        checksum = frame.getChecksum();
        committedRecords[frame.getPageNumber()] = frameno;
        if (frame.getTruncate() != 0) {
            m_truncate = frame.getTruncate();
            m_maxFrame = frameno;
            for (const auto &element : committedRecords) {
                m_pages2Frames[element.first] = element.second;
            }
            committedRecords.clear();
        }
    }
    if (!committedRecords.empty()) {
        for (const auto &iter : committedRecords) {
            m_disposedPages.emplace(iter.first);
        }
    }
    // all those frames that are uncommitted or exceeds the max allowed count will be disposed.
    return true;
}

#pragma mark - Error
void Wal::hint() const
{
    if (!isInitialized()) {
        return;
    }
    Error error;
    error.level = Error::Level::Notice;
    error.setCode(Error::Code::Notice, "Repair");
    error.message = "Wal hint.";
    error.infos.set("Truncate", m_truncate);
    error.infos.set("MaxFrame", m_maxFrame);
    error.infos.set("OriginFileSize", m_fileSize);
    bool succeed;
    size_t fileSize;
    std::tie(succeed, fileSize) = FileManager::getFileSize(getPath());
    if (succeed) {
        error.infos.set("CurrentFileSize", fileSize);
    }
    Notifier::shared()->notify(error);

    //Pages to frames
    if (!m_pages2Frames.empty()) {
        Error error;
        error.level = Error::Level::Notice;
        error.setCode(Error::Code::Notice, "Repair");
        std::ostringstream stream;
        int i = 0;
        for (const auto &iter : m_pages2Frames) {
            if (!stream.str().empty()) {
                stream << "; ";
            }
            stream << iter.first << ", " << iter.second;
            ++i;
            if (i % 20 == 0) {
                error.message = "Wal pages hint " + std::to_string(i / 20);
                error.infos.set("Pages2Frames", stream.str());
                Notifier::shared()->notify(error);
                stream.str("");
                stream.clear();
            }
        }
        if (!stream.str().empty()) {
            error.infos.set("Pages2Frames", stream.str());
            Notifier::shared()->notify(error);
        }
    }

    //Disposed
    if (!m_disposedPages.empty()) {
        Error error;
        error.level = Error::Level::Notice;
        error.setCode(Error::Code::Notice, "Repair");
        std::ostringstream stream;
        int i = 0;
        for (const auto &page : m_disposedPages) {
            if (!stream.str().empty()) {
                stream << ", ";
            }
            stream << page;
            ++i;
            if (i % 20 == 0) {
                error.message = "Wal disposed hint " + std::to_string(i / 20);
                error.infos.set("Disposed", stream.str());
                Notifier::shared()->notify(error);
                stream.str("");
                stream.clear();
            }
        }
        if (!stream.str().empty()) {
            error.infos.set("Disposed", stream.str());
            Notifier::shared()->notify(error);
        }
    }

    m_shm.hint();
}

void Wal::markAsCorrupted(int frame, const std::string &message)
{
    Error error;
    error.setCode(Error::Code::Corrupt, "Repair");
    error.message = message;
    error.infos.set("Path", getPath());
    error.infos.set("Frame", frame);
    Notifier::shared()->notify(error);
    setError(std::move(error));
}

void Wal::markAsError(Error::Code code)
{
    Error error;
    error.setCode(code, "Repair");
    error.infos.set("Path", getPath());
    Notifier::shared()->notify(error);
    setError(std::move(error));
}

#pragma mark - Dispose
int Wal::getDisposedPage() const
{
    WCTInnerAssert(isInitialized());
    return (int) m_disposedPages.size();
}

void Wal::dispose()
{
    WCTInnerAssert(isInitialized() || isInitializeFalied());
    for (const auto &element : m_pages2Frames) {
        m_disposedPages.emplace(element.first);
    }
    m_pages2Frames.clear();
    m_truncate = std::numeric_limits<uint32_t>::max();
    m_fileSize = 0;
    m_maxFrame = 0;
}

} //namespace Repair

} //namespace WCDB
