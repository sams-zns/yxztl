//  Copyright (c) 2019, Samsung Electronics.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
//  Written by Ivan L. Picoli <i.picoli@samsung.com>

#include <unistd.h>
#include <libzrocks.h>
#include <string>
#include <iostream>
#include <atomic>
#include <map>
#include <vector>
#include <memory>

#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "port/port.h"

#define ZNS_DEBUG     0
#define ZNS_DEBUG_R   (ZNS_DEBUG && 0) /* Read */
#define ZNS_DEBUG_W   (ZNS_DEBUG && 1) /* Write and Sync */
#define ZNS_DEBUG_AF  (ZNS_DEBUG && 0) /* Append and Flush */

#define ZNS_OBJ_STORE 0
#define ZNS_PREFETCH  1
#define ZNS_PREFETCH_BUF_SZ (1024 * 1024 * 1) /* 1MB */
#define ZROCKS_MAX_READ_SZ    (1024 * ZNS_ALIGMENT)

#define ZNS_MAX_MAP_ENTS 64

#define METADATA_MAGIC  0x3D
#define FILE_METADATA_MAGIC 0x3E
#define FILE_NAME_LEN  128

namespace rocksdb {

/* ### ZNS Environment ### */

struct MetadataHead {
    union {
        struct {
            std::uint8_t magic;
            std::uint32_t fileNum;
            std::uint64_t time;
            std::uint64_t dataLength;
        } meta;
        char addr[ZNS_ALIGMENT];
    };

    MetadataHead() {
        meta.magic = 0;
        meta.fileNum = 0;
        meta.time = 0;
        meta.dataLength = 0;
    }
};

struct ZrocksFileMetaHead {
    std::uint8_t magic;
    int8_t level;
    std::uint32_t zrocksMapNum;
    std::uint32_t filesize;
    char filename[FILE_NAME_LEN];
    std::uint32_t misalignsize;
    char misalign[ZNS_ALIGMENT];

    ZrocksFileMetaHead() {
        magic = 0;
        zrocksMapNum = 0;
        filesize = 0;
        level = -1;
        misalignsize = 0;
        memset(filename, 0, sizeof(filename));
        memset(misalign, 0, sizeof(misalign));
    }
};

class ZNSFile {
 public:
    const std::string name;
    size_t size;
    std::uint64_t uuididx;
    int level;
    std::vector<struct zrocks_map> map;
    char misalign[ZNS_ALIGMENT];
    std::uint32_t misalignsize;

    ZNSFile(const std::string& fname, int lvl) :
    name(fname),
    uuididx(0),
    level(lvl)  {
        misalignsize = 0;
        memset(misalign, 0, sizeof(misalign));
        size = 0;
    }

    ~ZNSFile() {}

    std::uint32_t GetFileMetaLen();

    std::uint32_t WriteMetaToBuf(unsigned char* buf);

    void PrintMetaData();
};

class ZNSEnv : public Env {

 public:
  port::Mutex filesMutex;
  std::map<std::string, ZNSFile*> files;
  port::Mutex envStartMutex;
  bool isEnvStart;
  std::uint64_t uuididx;
  explicit ZNSEnv(const std::string& dname) : dev_name(dname) {
    posixEnv = Env::Default();
    writable_file_leveled = true;
    isFlushRuning = false;
    isEnvStart = false;
    uuididx = 0;
	flushThread = 0;
    std::cout << "Initializing ZNS Environment" << std::endl;

    if (zrocks_init(dev_name.data())) {
	std::cout << "ZRocks failed to initialize." <<  std::endl;
	exit(1);
    }
  }

  virtual ~ZNSEnv() {
#if !ZNS_OBJ_STORE
    FlushMetaData();
#endif
    zrocks_exit();
    std::cout << "Destroying ZNS Environment" <<  std::endl;
  }

  Status FlushMetaData();

  void RecoverFileFromBuf(unsigned char* buf, std::uint32_t& praseLen);

  Status LoadMetaData();

  static void* WorkThread(void* arg);

  Status StartFlushThread();

  void StopFlushThread();

  /* ### Implemented at env_zns.cc ### */

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result,
                           const EnvOptions& options) override;

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result,
                             const EnvOptions& options) override;

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result,
                         const EnvOptions& options) override;

  Status NewWritableLeveledFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result,
                         const EnvOptions& options,
			 int level) override;

  Status DeleteFile(const std::string& fname) override;

  Status GetFileSize(const std::string& fname, std::uint64_t* size) override;

  Status GetFileModificationTime(const std::string& fname,
                                 std::uint64_t* file_mtime) override;


  /* ### Implemented here ### */

  Status LinkFile(const std::string& /*src*/,
                  const std::string& /*target*/) override {
    return Status::NotSupported(); // not supported
  }

  static std::uint64_t gettid() {
    assert(sizeof(pthread_t) <= sizeof(std::uint64_t));
    return (std::uint64_t)pthread_self();
  }

  std::uint64_t GetThreadID() const override {
    return ZNSEnv::gettid();
  }


  /* ### Posix inherited functions ### */

  Status NewDirectory(const std::string& name,
                      std::unique_ptr<Directory>* result) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << name << std::endl;
    return posixEnv->NewDirectory (name, result);
  }

  Status FileExists(const std::string& fname) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << fname << std::endl;
    return posixEnv->FileExists(fname);
  }

  Status GetChildren(const std::string& path,
                     std::vector<std::string>* result) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << path << std::endl;
    return posixEnv->GetChildren(path, result);
  }

  Status CreateDir(const std::string& name) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << name << std::endl;
    return posixEnv->CreateDir(name);
  }

  Status CreateDirIfMissing(const std::string& name) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << name << std::endl;
    return posixEnv->CreateDirIfMissing(name);
  }

  Status DeleteDir(const std::string& name) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << name << std::endl;
    return posixEnv->DeleteDir(name);
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << src << ":" << target << std::endl;
    return posixEnv->RenameFile(src, target);
  };

  Status LockFile(const std::string& fname, FileLock** lock) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << fname << std::endl;
    return posixEnv->LockFile(fname, lock);
  }

  Status UnlockFile(FileLock* lock) override {
    if (ZNS_DEBUG) std::cout << __func__ << std::endl;
    return posixEnv->UnlockFile(lock);
  }

  Status NewLogger(const std::string& fname,
                   std::shared_ptr<Logger>* result) override {
    if (ZNS_DEBUG) std::cout << __func__ << ":" << fname << std::endl;
    return posixEnv->NewLogger (fname, result);
  }

  void Schedule(void (*function)(void* arg), void* arg, Priority pri = LOW,
                void* tag = nullptr,
                void (*unschedFunction)(void* arg) = 0) override {
    posixEnv->Schedule(function, arg, pri, tag, unschedFunction);
  }

  int UnSchedule(void* tag, Priority pri) override {
    return posixEnv->UnSchedule(tag, pri);
  }

  void StartThread(void (*function)(void* arg), void* arg) override {
    posixEnv->StartThread(function, arg);
  }

  void WaitForJoin() override {
    posixEnv->WaitForJoin();
  }

  unsigned int GetThreadPoolQueueLen(Priority pri = LOW) const override {
    return posixEnv->GetThreadPoolQueueLen(pri);
  }

  Status GetTestDirectory(std::string* path) override {
    return posixEnv->GetTestDirectory(path);
  }

  std::uint64_t NowMicros() override {
    return posixEnv->NowMicros();
  }

  void SleepForMicroseconds(int micros) override {
    posixEnv->SleepForMicroseconds(micros);
  }

  Status GetHostName(char* name, std::uint64_t len) override {
    return posixEnv->GetHostName(name, len);
  }

  Status GetCurrentTime(int64_t* unix_time) override {
    return posixEnv->GetCurrentTime(unix_time);
  }

  Status GetAbsolutePath(const std::string& db_path,
                         std::string* output_path) override {
    return posixEnv->GetAbsolutePath(db_path, output_path);
  }

  void SetBackgroundThreads(int number, Priority pri = LOW) override {
    posixEnv->SetBackgroundThreads(number, pri);
  }

  int GetBackgroundThreads(Priority pri = LOW) override {
    return posixEnv->GetBackgroundThreads(pri);
  }

  void IncBackgroundThreadsIfNeeded(int number, Priority pri) override {
    posixEnv->IncBackgroundThreadsIfNeeded(number, pri);
  }

  std::string TimeToString(std::uint64_t number) override {
    return posixEnv->TimeToString(number);
  }

  Status GetThreadList(std::vector<ThreadStatus>* thread_list) {
    return posixEnv->GetThreadList(thread_list);
  }

  ThreadStatusUpdater* GetThreadStatusUpdater() const override {
    return posixEnv->GetThreadStatusUpdater();
  }

 private:
  Env*  posixEnv;       // This object is derived from Env, but not from
                        // posixEnv. We have posixnv as an encapsulated
                        // object here so that we can use posix timers,
                        // posix threads, etc.
  const std::string dev_name;
  pthread_t flushThread;
  bool isFlushRuning;
  bool IsFilePosix(const std::string& fname) {
    return (fname.find("uuid") != std::string::npos ||
	fname.find("CURRENT") != std::string::npos ||
	fname.find("IDENTITY") != std::string::npos ||
	fname.find("MANIFEST") != std::string::npos ||
	fname.find("OPTIONS") != std::string::npos ||
	fname.find("LOG") != std::string::npos ||
	fname.find("LOCK") != std::string::npos ||
	fname.find(".dbtmp") != std::string::npos ||
	fname.find(".log") != std::string::npos||
	fname.find(".trace") != std::string::npos);
  }
};


/* ### SequentialFile, RandAccessFile, and Writable File ### */

class ZNSSequentialFile : public SequentialFile {

 private:
  std::string filename_;
  bool use_direct_io_;
  size_t logical_sector_size_;
  std::uint64_t ztl_id;

  ZNSEnv *env_zns;
  uint64_t read_off;

 public:
  ZNSSequentialFile(const std::string& fname, ZNSEnv* zns, const EnvOptions& options)
    : filename_(fname),
      use_direct_io_(options.use_direct_reads),
      logical_sector_size_(ZNS_ALIGMENT) {
      env_zns = zns;
      ztl_id = 1UL * stoi(fname.substr(fname.length() - 10, 6));
      read_off = 0;
  }

  virtual ~ZNSSequentialFile() {
  }


  /* ### Implemented at env_zns_io.cc ### */

  Status ReadOffset(uint64_t offset, size_t n, Slice* result,
                                    char* scratch, size_t* readLen) const;

  Status Read(size_t n, Slice* result, char* scratch) override;

  Status PositionedRead(std::uint64_t offset, size_t n, Slice* result,
					       char* scratch) override;

  Status Skip(std::uint64_t n) override;

  Status InvalidateCache(size_t offset, size_t length) override;


  /* ### Implemented here ### */

  bool use_direct_io() const override {
    return use_direct_io_;
  }

  size_t GetRequiredBufferAlignment() const override {
    return logical_sector_size_;
  }
};


class ZNSRandomAccessFile : public RandomAccessFile {
 private:
  std::string filename_;
  bool use_direct_io_;
  size_t logical_sector_size_;
  std::uint64_t ztl_id;
  std::uint64_t uuididx;

  ZNSEnv *env_zns;

#if ZNS_PREFETCH
  char  *prefetch;
  size_t prefetch_sz;
  std::uint64_t prefetch_off;
  std::atomic_flag prefetch_lock = ATOMIC_FLAG_INIT;
#endif

 public:
  ZNSRandomAccessFile(const std::string& fname, ZNSEnv* zns,
					    const EnvOptions& options)
    : filename_(fname),
      use_direct_io_(options.use_direct_reads),
      logical_sector_size_(ZNS_ALIGMENT),
      uuididx(0),
      env_zns(zns) {

    ztl_id = 1UL * stoi(fname.substr(fname.length() - 10, 6));
    #if ZNS_PREFETCH
    prefetch_off = 0;
#endif
    env_zns->filesMutex.Lock();
    // uuididx = env_zns->uuididx++;
    // uuididx = env_zns->files[filename_]->uuididx;
    env_zns->filesMutex.Unlock();

#if ZNS_PREFETCH
    prefetch = reinterpret_cast<char*> (zrocks_alloc(ZNS_PREFETCH_BUF_SZ));
    if (!prefetch) {
	std::cout << " ZRocks (alloc prefetch) error." << std::endl;
	prefetch = nullptr;
    }
    prefetch_sz = 0;
#endif
  }

  virtual ~ZNSRandomAccessFile() {
#if ZNS_PREFETCH
    zrocks_free(prefetch);
#endif
  }


  /* ### Implemented at env_zns_io.cc ### */

  Status Read(std::uint64_t offset, size_t n, Slice* result,
					char* scratch) const override;

  Status Prefetch(std::uint64_t offset, size_t n) override;

  size_t GetUniqueId(char* id, size_t max_size) const override;

  Status InvalidateCache(size_t offset, size_t length) override;

  virtual Status ReadObj(std::uint64_t offset, size_t n, Slice* result,
						char* scratch) const;

  virtual Status ReadOffset(std::uint64_t offset, size_t n, Slice* result,
						char* scratch) const;


  /* ### Implemented here ### */

  bool use_direct_io() const override {
    return use_direct_io_;
  }

  size_t GetRequiredBufferAlignment() const override {
    return logical_sector_size_;
  }
};

class ZNSWritableFile : public WritableFile {
 private:
  const std::string filename_;
  const bool use_direct_io_;
  int fd_;
  std::uint64_t filesize_;
  std::uint64_t ztl_id;
  size_t logical_sector_size_;
#ifdef ROCKSDB_FALLOCATE_PRESENT
  bool allow_fallocate_;
  bool fallocate_with_keep_size_;
#endif
  char* wcache;
  char* cache_off;
  int level;

  ZNSEnv *env_zns;
  std::uint64_t map_off;

 public:
  explicit ZNSWritableFile(const std::string& fname, ZNSEnv* zns,
				const EnvOptions& options, int lvl)
    : WritableFile(options),
      filename_(fname),
      use_direct_io_(options.use_direct_writes),
      fd_(0),
      filesize_(0),
      logical_sector_size_(ZNS_ALIGMENT),
      level(lvl),
      env_zns(zns) {

#ifdef ROCKSDB_FALLOCATE_PRESENT
    allow_fallocate_ = options.allow_fallocate;
    fallocate_with_keep_size_ = options.fallocate_with_keep_size;
#endif

    ztl_id = 1UL * stoi(fname.substr(fname.length() - 10, 6));

    wcache = reinterpret_cast<char*> (zrocks_alloc(ZNS_MAX_BUF));
    if (!wcache) {
	std::cout << " ZRocks (alloc) error." << std::endl;
	cache_off = nullptr;
    }

    cache_off = wcache;
    map_off = 0;
  }

  virtual ~ZNSWritableFile() {
    zrocks_free(wcache);
  }


  /* ### Implemented at env_zns_io.cc ### */

  Status Append(const Slice& data) override;

  Status PositionedAppend(const Slice& data, std::uint64_t offset) override;

  Status Truncate(std::uint64_t size) override;

  Status Close() override;

  Status Flush() override;

  Status Sync() override;

  Status Fsync() override;

  Status InvalidateCache(size_t offset, size_t length) override;

#ifdef ROCKSDB_FALLOCATE_PRESENT
  Status Allocate(std::uint64_t offset, std::uint64_t len) override;
#endif

  Status RangeSync(std::uint64_t offset, std::uint64_t nbytes) override;

  size_t GetUniqueId(char* id, size_t max_size) const override;


  /* ### Implemented here ### */

  bool IsSyncThreadSafe() const override {
    return true;
  }

  bool use_direct_io() const override {
    return use_direct_io_;
  }

  void SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) override {
     if (ZNS_DEBUG) std::cout << __func__ << " : " << hint << std::endl;
  }

  size_t GetRequiredBufferAlignment() const override {
    return logical_sector_size_;
  }
};

} // namespace rocksdb
