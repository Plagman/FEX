// SPDX-License-Identifier: MIT

#include "FEXCore/Core/DiskCache.h"
#include "FEXCore/Utils/LogManager.h"
#include "Interface/Context/Context.h"
#include "FEXCore/HLE/SyscallHandler.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <cstdint>
#include <cstring>
#include <xxhash.h>

namespace FEXCore {

#define FOZ_REF_MAGIC_SIZE 16

static const uint8_t stream_reference_magic_and_version[FOZ_REF_MAGIC_SIZE] = {
   0x81, 'F', 'O', 'S',
   'S', 'I', 'L', 'I',
   'Z', 'E', 'D', 'B',
   0, 0, 0, FOSSILIZE_FORMAT_VERSION, /* 4 bytes to use for versioning. */
};

struct __attribute__((packed)) mesa_index_db_file_entry {
    uint64_t hash;
    uint32_t size;
    uint64_t last_access_time;
    uint64_t cache_db_file_offset;
};

bool DiskCacheFOZFile::OpenExisting() {
    File::FileModes Modes = File::FileModes::READ;
    if (!ReadOnly) {
        Modes = Modes | File::FileModes::WRITE;
    }
    FD = fextl::make_unique<File::File>(FileName.c_str(), Modes);

    if (!FD->IsValid()) {
        return false;
    }

    uint8_t magic[FOZ_REF_MAGIC_SIZE];
    if (FD->Read(magic, FOZ_REF_MAGIC_SIZE) != FOZ_REF_MAGIC_SIZE) {
        return false;
    }

    if (memcmp(magic, stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE - 1)) {
        return false;
    }

    int version = magic[FOZ_REF_MAGIC_SIZE - 1];
    if (version > FOSSILIZE_FORMAT_VERSION || version < FOSSILIZE_FORMAT_MIN_COMPAT_VERSION) {
        return false;
    }

    return true;
}

bool DiskCacheFOZFile::CreateNew() {
    File::FileModes Modes = File::FileModes::READ | File::FileModes::WRITE |
                            File::FileModes::CREATE | File::FileModes::TRUNCATE; 

    FD = fextl::make_unique<File::File>(FileName.c_str(), Modes);

    if (!FD->IsValid()) {
        return false;
    }

    if (FD->Write(stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE) != FOZ_REF_MAGIC_SIZE) {
        return false;
    }

    // could readyforappend here and infer cursor position but whatever

    return true;
}

bool DiskCacheFOZFile::Open(const fextl::string &FOZFileName, bool ReadOnly) {
    FileName = FOZFileName;

    if (!OpenExisting()) {
        if (ReadOnly) {
            FD.reset();
            return false;
        } else {
            if (!CreateNew()) {
                FD.reset();
                return false;
            }
        }
    }

    this->ReadOnly = ReadOnly;
    return true;
}

bool DiskCacheFOZFile::ReadNextBlob(foz_payload_key &OutKey, foz_payload_header &OutHeader, fextl::vector<uint8_t> &OutBlob) {
    if (FD->Read(OutKey.bytes, sizeof(OutKey.bytes)) != sizeof(OutKey.bytes)) {
        return false;
    }
    if (FD->Read(&OutHeader, sizeof(OutHeader)) != sizeof(OutHeader)) {
        return false;
    }
    OutBlob.resize(OutHeader.payload_size);
    if (FD->Read(OutBlob.data(), OutBlob.size()) != (ssize_t)OutBlob.size()) {
        return false;
    }
    return true;
}

bool DiskCacheFOZFile::ReadBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    ReadyForAppend = false;

    ssize_t SeekRet = FD->Seek(Offset, File::SeekOp::BEGIN);
    if (SeekRet < 0) {
        return false;
    }
    if (FD->Read(OutBlob.data(), OutBlob.size()) != (ssize_t)OutBlob.size()) {
        return false;
    }

    return true;
}

bool DiskCacheFOZFile::WriteBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t &OutBlobOffset) {
    if (!ReadyForAppend) {
        ssize_t SeekRet = FD->Seek(0, File::SeekOp::END);
        if (SeekRet < 0) {
            return false;
        }
        ReadyForAppend = true;
        AppendCursor = (uint64_t)SeekRet;
    }

    if (FD->Write(Key.bytes, sizeof(Key.bytes)) != sizeof(Key.bytes)) {
        return false;
    }
    AppendCursor += sizeof(Key.bytes);

    uint64_t TotalBlobSize = 0;
    for (const std::span<const uint8_t> &Chunk : BlobChunks) {
        TotalBlobSize += Chunk.size();
    }

    foz_payload_header ScratchHeader {
        .payload_size = (uint32_t)TotalBlobSize,
        .format = FOSSILIZE_COMPRESSION_NONE,
        .crc = 0, // todo? maybe
        .uncompressed_size = (uint32_t)TotalBlobSize
    };

    if (FD->Write(&ScratchHeader, sizeof(ScratchHeader)) != sizeof(ScratchHeader)) {
        return false;
    }
    AppendCursor += sizeof(ScratchHeader);

    OutBlobOffset = AppendCursor;

    for (const std::span<const uint8_t> &Chunk : BlobChunks) {
        if (Chunk.size() == 0) {
            continue;
        }
        if (FD->Write(Chunk.data(), Chunk.size()) != (ssize_t)Chunk.size()) {
            return false;
        }
        AppendCursor += Chunk.size();
    }

    return true;
}

bool DiskCacheIndexedDB::Open(const fextl::string &CacheDBName, bool ReadOnly) {
    if (!CacheFOZ.Open(CacheDBName + ".foz", ReadOnly)) {
        return false;
    }
    if (!IndexFOZ.Open(CacheDBName + "_idx.foz", ReadOnly)) {
        return false;
    }

    this->ReadOnly = ReadOnly;
    return true;
}

void DiskCacheIndexedDB::PopulateIndex(DiskCacheIndex &CacheIndex) {
    foz_payload_key Key;
    foz_payload_header Header;
    fextl::vector<uint8_t> Blob;

    while (IndexFOZ.ReadNextBlob(Key, Header, Blob)) {
        if (Blob.size() != sizeof(mesa_index_db_file_entry)) {
            break;
        }
        mesa_index_db_file_entry *IndexEntry = (mesa_index_db_file_entry *)Blob.data();
        if (IndexEntry->hash != XXH3_64bits(Key.bytes, FOSSILIZE_BLOB_HASH_LENGTH)) {
            break;
        }
        CacheIndex.insert({IndexEntry->hash, {this, IndexEntry->cache_db_file_offset, IndexEntry->size}});
    }
    // could truncate/delete index if we don't end up perfectly at end here
}

bool DiskCacheIndexedDB::ReadCacheBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    return CacheFOZ.ReadBlob(Offset, OutBlob);
}

bool DiskCacheIndexedDB::StoreCacheBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, DiskCacheIndex &CacheIndex) {
    if (ReadOnly) {
        // shouldn't happen
        return false;
    }
    uint64_t Hash = XXH3_64bits(Key.bytes, FOSSILIZE_BLOB_HASH_LENGTH);
    if (CacheIndex.contains(Hash)) {
        // shouldn't really happen.. assert or something?
        return true;
    }

    // write cache side first so we get offset for index
    uint64_t BlobOffset = 0;
    if (!CacheFOZ.WriteBlob(Key, BlobChunks, BlobOffset)) {
        return false;
    }

    uint64_t TotalBlobSize = 0;
    for (const std::span<const uint8_t> &Chunk : BlobChunks) {
        TotalBlobSize += Chunk.size();
    }

    mesa_index_db_file_entry IndexEntry {
        .hash = Hash,
        .size = (uint32_t)TotalBlobSize,
        .last_access_time = 0, // todo..
        .cache_db_file_offset = BlobOffset
    };

    std::span<const uint8_t> IndexBlobChunks[] = {
        {(const uint8_t*)&IndexEntry, sizeof(IndexEntry)}
    };
    uint64_t UnusedIndexBlobOffset = 0;
    if (!IndexFOZ.WriteBlob(Key, IndexBlobChunks, UnusedIndexBlobOffset)) {
        return false;
    }

    CacheIndex[Hash] = {this, BlobOffset, (uint32_t)TotalBlobSize};
    return true;
}

bool DiskCache::OpenCacheDB(const fextl::string &CacheDBName, bool ReadOnly) {
    fextl::unique_ptr<DiskCacheIndexedDB> CurDB;

    if (!ReadOnly && RWCacheDB) {
        // rw already opened, just support one
        return false;
    }
    
    CurDB = fextl::make_unique<DiskCacheIndexedDB>();
    if (!CurDB) {
        return false;
    }

    if (!CurDB->Open(CacheDBName, ReadOnly)) {
        CurDB.reset();
        return false;
    }

    CurDB->PopulateIndex(CacheIndex);

    if (ReadOnly) {
        ROCacheDBs.push_back(std::move(CurDB));
    } else {
        RWCacheDB = std::move(CurDB);
    }

    return true;
}

void DiskCache::Init(FEXCore::Context::ContextImpl *CTX) {
    this->CTX = CTX;

    // todo get those paths / enablement from options, etc
    fextl::string CacheBaseName = "fex_disk_cache";
    OpenCacheDB(CacheBaseName, false);

    // todo grab all CTX options that can change compilation here and bake somewhere
}

std::optional<DiskCacheCodeHitData> DiskCache::Lookup(Core::InternalThreadState* Thread, const ExecutableFileSectionInfo& Region, uint64_t GuestRIP) {
    uint64_t ModuleOffset = GuestRIP - Region.FileStartVA;

    // todo move key making to a helper once we have options and stuff (see Store)
    foz_payload_key Key = {};
    memcpy(Key.bytes, &ModuleOffset, sizeof(ModuleOffset));

    uint64_t Hash = XXH3_64bits(Key.bytes, FOSSILIZE_BLOB_HASH_LENGTH);
    auto CacheIndexIt = CacheIndex.find(Hash);
    if (CacheIndexIt == CacheIndex.end()) {
        // definite miss
        return std::nullopt;
    }
    const CacheIndexEntry& Entry = CacheIndexIt->second;
    // found a key hash match, could still be a miss, read the blob and verify more
    DiskCacheCodeHitData HitData;
    HitData.Blob.resize(Entry.Size);
    if (!Entry.DB->ReadCacheBlob(Entry.Offset, HitData.Blob)) {
        return std::nullopt;
    }

    uint32_t SizeNeeded = 0;

    uint32_t GuestSize;
    XXH128_hash_t GuestHash;
    uint32_t HostSize;
    uint32_t EntryPointCount;
    SizeNeeded += sizeof(GuestSize) + sizeof(GuestHash) + sizeof(HostSize) + sizeof(EntryPointCount);
    if (Entry.Size < SizeNeeded) {
        return std::nullopt;
    }
    // if we survived this, we know have enough to read the dynamic sizes, at least
    // we can't read them directly off of HitData because they might be unaligned?
    memcpy(&GuestSize, HitData.Blob.data(), sizeof(GuestSize));
    memcpy(&GuestHash, HitData.Blob.data() + sizeof(GuestSize), sizeof(GuestHash));
    memcpy(&HostSize, HitData.Blob.data() + sizeof(GuestSize) + sizeof(GuestHash), sizeof(HostSize));
    memcpy(&EntryPointCount, HitData.Blob.data() + sizeof(GuestSize) + sizeof(GuestHash) + sizeof(HostSize), sizeof(EntryPointCount));

    // do we have enough room in our live code to even hash GuestSize worth?
    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    if (RangeInfo.Size == 0 || RangeInfo.Base > GuestRIP) {
        return std::nullopt;
    }
    uint64_t Available = RangeInfo.Base + RangeInfo.Size - GuestRIP;
    if (Available < GuestSize) {
        return std::nullopt;
    }

    XXH128_hash_t LiveGuestHash = XXH3_128bits(reinterpret_cast<void *>(GuestRIP), GuestSize);
    if (std::memcmp(&LiveGuestHash, &GuestHash, sizeof(GuestHash)) != 0) {
        LogMan::Msg::IFmt("hash mismatch! length {:d}", GuestSize);
        return std::nullopt;
    }
    LogMan::Msg::IFmt("hash ok! length {:d}", GuestSize);

    HitData.HostCode = {HitData.Blob.data() + SizeNeeded, HostSize};
    HitData.EntryPoints = {reinterpret_cast<const DiskCacheBlobEntryPoint*>(HitData.Blob.data() + SizeNeeded + HostSize), EntryPointCount};

    // this seems to be a full hit, lastly, check the entry is big enough to have cached host code and other vital metadata
    SizeNeeded += HostSize + EntryPointCount * sizeof(DiskCacheBlobEntryPoint);
    if (Entry.Size < SizeNeeded) {
        return std::nullopt;
    }

    return HitData;
}

bool DiskCache::Store(const ExecutableFileSectionInfo& Region, uint64_t GuestRIP, std::span<const uint8_t> GuestCode,
                      std::span<const uint8_t> HostCode, std::span<const DiskCacheBlobEntryPoint> EntryPoints) {
    if (!RWCacheDB) {
        return false;
    }

    uint64_t ModuleOffset = GuestRIP - Region.FileStartVA;

    // todo also copy/hash options that affect codegen into the key
    // todo should try to keep the key ascii i think?
    foz_payload_key Key = {};
    memcpy(Key.bytes, &ModuleOffset, sizeof(ModuleOffset));

    uint32_t GuestSize = (uint32_t)GuestCode.size();
    XXH128_hash_t GuestHash = XXH3_128bits(GuestCode.data(), GuestCode.size());
    uint32_t HostSize = (uint32_t)HostCode.size();
    uint32_t EntryPointCount = (uint32_t)EntryPoints.size();

    std::span<const uint8_t> CacheBlobChunks[] = {
        {(const uint8_t*)&GuestSize, sizeof(GuestSize)},
        {(const uint8_t*)&GuestHash, sizeof(GuestHash)},
        {(const uint8_t*)&HostSize, sizeof(HostSize)},
        {(const uint8_t*)&EntryPointCount, sizeof(EntryPointCount)},
        HostCode,
        {(const uint8_t*)EntryPoints.data(), EntryPointCount * sizeof(DiskCacheBlobEntryPoint)},
        GuestCode,
    };

    return RWCacheDB->StoreCacheBlob(Key, CacheBlobChunks, CacheIndex);
}

}