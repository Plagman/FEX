// SPDX-License-Identifier: MIT

#include "FEXCore/Core/DiskCache.h"
#include "FEXCore/Utils/LogManager.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"

namespace FEXCore {

#define FOZ_REF_MAGIC_SIZE 16

static const uint8_t stream_reference_magic_and_version[FOZ_REF_MAGIC_SIZE] = {
   0x81, 'F', 'O', 'S',
   'S', 'I', 'L', 'I',
   'Z', 'E', 'D', 'B',
   0, 0, 0, FOSSILIZE_FORMAT_VERSION, /* 4 bytes to use for versioning. */
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

    ReadyForAppend = true;

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