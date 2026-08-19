// SPDX-License-Identifier: MIT
#pragma once
#include "FEXCore/Core/CodeCache.h"
#include "FEXCore/Core/Context.h"
#include "Interface/Core/JIT/Relocations.h"
#include "Interface/Core/Frontend.h"
#include "Interface/Core/CPUBackend.h"
#include "FEXCore/Config/Config.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_set.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/vector.h>
#include <stdint.h>
#include <mutex>
#include <optional>
#include <span>
#include <xxhash.h>

namespace FEXCore {

#define FOSSILIZE_BLOB_HASH_LENGTH 40 /* SHA1 hexadecimal string length */

enum {
   FOSSILIZE_COMPRESSION_NONE = 1,
   FOSSILIZE_COMPRESSION_DEFLATE = 2
};

enum {
   FOSSILIZE_FORMAT_VERSION = 6,
   FOSSILIZE_FORMAT_MIN_COMPAT_VERSION = 5
};

struct __attribute__((packed)) foz_payload_key {
    uint8_t bytes[FOSSILIZE_BLOB_HASH_LENGTH];
};

struct __attribute__((packed)) foz_payload_header {
  uint32_t payload_size;
  uint32_t format;
  uint32_t crc;
  uint32_t uncompressed_size;
};

class DiskCacheIndexedDB;

struct CacheIndexEntry {
    DiskCacheIndexedDB *DB;
    uint64_t Offset;
    uint32_t Size;
};

struct __attribute__((packed)) DiskCacheBlobFixedHeader {
    uint32_t GuestSize;
    uint32_t HostSize;
    uint32_t EntryPointCount;
    uint32_t SmallRelocCount;
    uint32_t ThunkRelocCount;
    uint32_t TouchedGuestPagesCount;
    XXH128_hash_t GuestHash;
};

struct __attribute__((packed)) DiskCacheBlobEntryPoint {
    uint64_t GuestRIP; // todo those have been made relative since i wrote this, can we get away with less size here?
    uint32_t HostOffset;
};

// packed struct for types 0, 2 and 3. type 1 is bigger and separate below
struct __attribute__((packed)) DiskCacheBlobSmallRelocation {
    uint32_t Offset;
    uint8_t Type;
    union {
        struct __attribute__((packed)) {
            uint32_t Symbol;
        } Named;
        struct __attribute__((packed)) {
            uint64_t GuestRIP;
        } RIPLiteral;
        struct __attribute__((packed)) {
            uint8_t RegisterIndex;
            uint64_t GuestRIP;
        } RIPMove;
    };
};

// type 1, implicit
struct __attribute__((packed)) DiskCacheBlobThunkRelocation {
    uint32_t Offset;
    uint8_t RegisterIndex;
    uint8_t SymbolHash[32]; // sha256sum in the real RelocNamedThunkMove
};

struct DiskCacheCodeHitData {
    fextl::vector<uint8_t> Blob;
    std::span<uint8_t> HostCode;
    std::span<const DiskCacheBlobEntryPoint> EntryPoints;
    fextl::vector<FEXCore::CPU::Relocation> Relocations;
    fextl::vector<uint64_t> GuestPages;

    // the spans above point to memory owned by the Blob vec, so it's important this can't be copied
    DiskCacheCodeHitData() = default;
    DiskCacheCodeHitData(DiskCacheCodeHitData&&) = default;
    DiskCacheCodeHitData& operator=(DiskCacheCodeHitData&&) = default;
    DiskCacheCodeHitData(const DiskCacheCodeHitData&) = delete;
    DiskCacheCodeHitData& operator=(const DiskCacheCodeHitData&) = delete;
};

using DiskCacheIndex = fextl::robin_map<uint64_t, CacheIndexEntry>;

class DiskCacheFOZFile {
    fextl::string FileName;
    fextl::unique_ptr<File::File> FD;
    bool ReadOnly = false;
public:
    bool Open(const fextl::string &CacheFileName, bool ReadOnly);
    bool Lock(uint32_t TimeoutMS) {
        if (!FD) {
            return false;
        }
        return FD->Lock(TimeoutMS);
    }
    bool Unlock() {
        if (!FD) {
            return false;
        }
        return FD->Unlock();
    }
    bool ReadNextBlob(foz_payload_key &OutKey, foz_payload_header &OutHeader, fextl::vector<uint8_t> &OutBlob);
    bool ReadBlob(uint64_t Offset, std::span<uint8_t> OutBlob);
    bool WriteBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t &OutBlobOffset);
};

class DiskCacheIndexedDB {
    DiskCacheFOZFile CacheFOZ;
    DiskCacheFOZFile IndexFOZ;
    bool ReadOnly = false;
public:
    bool Open(const fextl::string &CacheDBName, bool ReadOnly);
    void PopulateIndex(DiskCacheIndex &CacheIndex);
    bool ReadCacheBlob(uint64_t Offset, std::span<uint8_t> OutBlob);
    bool StoreCacheBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, DiskCacheIndex &CacheIndex);
};

namespace Context {
    class ContextImpl;
}

class DiskCache {
    FEX_CONFIG_OPT(EnableDiskCache, DISKCACHE);
    FEX_CONFIG_OPT(RelocationFilter, DISKCACHERELOCATIONFILTER);
    FEX_CONFIG_OPT(BasePathOverride, DISKCACHEPATH);
    FEX_CONFIG_OPT(RODBNames, DISKCACHERODBNAMES);

    FEXCore::Context::ContextImpl *CTX;
    fextl::vector<fextl::unique_ptr<DiskCacheIndexedDB>> ROCacheDBs;
    fextl::unique_ptr<DiskCacheIndexedDB> RWCacheDB;
    DiskCacheIndex CacheIndex;
    std::mutex Lock;

    bool OpenCacheDB(const fextl::string &CacheDBName, bool ReadOnly);
public:
    void Init(FEXCore::Context::ContextImpl *CTX);

    std::optional<DiskCacheCodeHitData> Lookup(Core::InternalThreadState* Thread, const ExecutableFileSectionInfo& Region, uint64_t GuestRIP);
    bool Store(Core::InternalThreadState* Thread, const ExecutableFileSectionInfo& Region, uint64_t GuestRIP,
               std::span<const uint8_t> GuestCode, const CPU::CPUBackend::CompiledCode& CompiledCode,
               std::span<const FEXCore::CPU::Relocation> Relocations, const Frontend::Decoder::DecodedBlockInformation* DecodedBlockInfo);

    bool IsWritingDiskCache() const {
        return (bool)RWCacheDB;
    }
    bool IsReadingDiskCache() const {
        return !ROCacheDBs.empty() || RWCacheDB != nullptr;
    }
};

}