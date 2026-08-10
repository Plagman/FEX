// SPDX-License-Identifier: MIT
#pragma once
#include "FEXCore/Core/CodeCache.h"
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_set.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/vector.h>
#include <stdint.h>
#include <span>

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

struct __attribute__((packed)) DiskCacheBlobEntryPoint {
    uint64_t GuestRIP;
    uint32_t HostOffset;
};

using DiskCacheIndex = fextl::robin_map<uint64_t, CacheIndexEntry>;

class DiskCacheFOZFile {
    bool OpenExisting();
    bool CreateNew();

    fextl::string FileName;
    fextl::unique_ptr<File::File> FD;
    bool ReadOnly = false;
    bool ReadyForAppend = false;
    uint64_t AppendCursor = 0;
public:
    bool Open(const fextl::string &CacheFileName, bool ReadOnly);
    bool ReadNextBlob(foz_payload_key &OutKey, foz_payload_header &OutHeader, fextl::vector<uint8_t> &OutBlob);
    bool WriteBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t &OutBlobOffset);
};

class DiskCacheIndexedDB {
    DiskCacheFOZFile CacheFOZ;
    DiskCacheFOZFile IndexFOZ;
    bool ReadOnly = false;
public:
    bool Open(const fextl::string &CacheDBName, bool ReadOnly);
    void PopulateIndex(DiskCacheIndex &CacheIndex);
    bool StoreCacheBlob(const foz_payload_key &Key, std::span<const std::span<const uint8_t>> BlobChunks, DiskCacheIndex &CacheIndex);
};

class DiskCache {
    fextl::vector<fextl::unique_ptr<DiskCacheIndexedDB>> ROCacheDBs;
    fextl::unique_ptr<DiskCacheIndexedDB> RWCacheDB;
    DiskCacheIndex CacheIndex;
    bool OpenCacheDB(const fextl::string &CacheDBName, bool ReadOnly);
public:
    void Init();
    bool Store(const ExecutableFileSectionInfo& Region, uint64_t GuestRIP, std::span<const uint8_t> GuestCode,
               std::span<const uint8_t> HostCode, std::span<const DiskCacheBlobEntryPoint> EntryPoints);
};

}