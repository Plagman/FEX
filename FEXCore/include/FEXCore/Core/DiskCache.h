// SPDX-License-Identifier: MIT
#pragma once
#include "FEXCore/Utils/File.h"
#include "FEXCore/fextl/memory.h"
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_set.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/vector.h>
#include <stdint.h>
#include <xxhash.h>

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

    bool operator==(const foz_payload_key& other) const {
        return std::memcmp(bytes, other.bytes, FOSSILIZE_BLOB_HASH_LENGTH) == 0;
    }
};

template<>
struct std::hash<foz_payload_key> {
    std::size_t operator()(const foz_payload_key& k) const noexcept {
        return XXH3_64bits(k.bytes, FOSSILIZE_BLOB_HASH_LENGTH);
    }
};

struct __attribute__((packed)) foz_payload_header {
  uint32_t payload_size;
  uint32_t format;
  uint32_t crc;
  uint32_t uncompressed_size;
};

namespace FEXCore {

class DiskCacheFOZFile {
    bool OpenExisting();
    bool CreateNew();

    fextl::string FileName;
    fextl::unique_ptr<File::File> FD;
    bool ReadOnly = false;
    bool ReadyForAppend = false;
public:
    bool Open(const fextl::string &CacheFileName, bool ReadOnly);
};

class DiskCacheIndexedDB {
    DiskCacheFOZFile CacheFOZ;
    DiskCacheFOZFile IndexFOZ;
    bool ReadOnly = false;
public:
    bool Open(const fextl::string &CacheDBName, bool ReadOnly);
};

struct CacheEntry {
    DiskCacheIndexedDB *DB;
    uint64_t Offset;
    uint64_t Size;
};

using DiskCacheIndex = fextl::robin_map<foz_payload_key, CacheEntry>;

class DiskCache {
    fextl::vector<fextl::unique_ptr<DiskCacheIndexedDB>> ROCacheDBs;
    fextl::unique_ptr<DiskCacheIndexedDB> RWCacheDB;
    DiskCacheIndex CacheIndex;
    bool OpenCacheDB(const fextl::string &CacheDBName, bool ReadOnly);
public:
    void Init();
};

}