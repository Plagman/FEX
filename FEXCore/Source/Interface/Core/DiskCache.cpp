// SPDX-License-Identifier: MIT

#include "FEXCore/Core/DiskCache.h"
#include "FEXCore/Utils/LogManager.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <cstdint>
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

bool DiskCacheFOZFile::WriteBlob(const foz_payload_key &Key, std::span<const uint8_t> Blob, uint64_t &OutBlobOffset) {
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

    foz_payload_header ScratchHeader {
        .payload_size = (uint32_t)Blob.size(),
        .format = FOSSILIZE_COMPRESSION_NONE,
        .crc = 0, // todo? maybe
        .uncompressed_size = (uint32_t)Blob.size()
    };

    if (FD->Write(&ScratchHeader, sizeof(ScratchHeader)) != sizeof(ScratchHeader)) {
        return false;
    }
    AppendCursor += sizeof(ScratchHeader);

    OutBlobOffset = AppendCursor;

    if (FD->Write(Blob.data(), Blob.size()) != (ssize_t)Blob.size()) {
        return false;
    }
    AppendCursor += Blob.size();

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

bool DiskCacheIndexedDB::StoreCacheBlob(const foz_payload_key &Key, std::span<const uint8_t> Blob, DiskCacheIndex &CacheIndex) {
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
    if (!CacheFOZ.WriteBlob(Key, Blob, BlobOffset)) {
        return false;
    }

    mesa_index_db_file_entry IndexEntry {
        .hash = Hash,
        .size = (uint32_t)Blob.size(),
        .last_access_time = 0, // todo..
        .cache_db_file_offset = BlobOffset
    };

    std::span<const uint8_t> IndexBlob((const uint8_t*)&IndexEntry, sizeof(IndexEntry));
    uint64_t UnusedIndexBlobOffset = 0;
    if (!IndexFOZ.WriteBlob(Key, IndexBlob, UnusedIndexBlobOffset)) {
        return false;
    }

    CacheIndex[Hash] = {this, BlobOffset, (uint32_t)Blob.size()};
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

    if (ReadOnly) {
        ROCacheDBs.push_back(std::move(CurDB));
    } else {
        RWCacheDB = std::move(CurDB);
    }

    return true;
}

void DiskCache::Init() {
    LogMan::Msg::IFmt("DiskCache::Init");

    fextl::string CacheBaseName = "fex_disk_cache";
    OpenCacheDB(CacheBaseName, false);
}

}