// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#define MEM_IDEV

#include <atomic>
#include <cstdint>
#include <string>
#include <libaio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MEM_IDEV
#include <stdexcept>
#endif

#ifdef FASTER_URING
#include <liburing.h>
#endif

#include "async.h"
#include "status.h"
#include "file_common.h"

namespace FASTER {
namespace environment {

constexpr const char* kPathSeparator = "/";

/// The File class encapsulates the OS file handle.
class File {
 protected:
  File()
    : fd_{ -1 }
    , device_alignment_{ 0 }
    , filename_{}
    , owner_{ false }
#ifdef IO_STATISTICS
    , bytes_written_ { 0 }
    , read_count_{ 0 }
    , bytes_read_{ 0 }
#endif
  {
  }

  File(const std::string& filename)
    : fd_{ -1 }
    , device_alignment_{ 0 }
    , filename_{ filename }
    , owner_{ false }
#ifdef IO_STATISTICS
    , bytes_written_ { 0 }
    , read_count_{ 0 }
    , bytes_read_{ 0 }
#endif
  {
  }

  ~File() {
    if(owner_) {
      core::Status s = Close();
    }
  }

  File(const File&) = delete;
  File &operator=(const File&) = delete;

  /// Move constructor.
  File(File&& other)
    : fd_{ other.fd_ }
    , device_alignment_{ other.device_alignment_ }
    , filename_{ std::move(other.filename_) }
    , owner_{ other.owner_ }
#ifdef IO_STATISTICS
    , bytes_written_ { other.bytes_written_ }
    , read_count_{ other.read_count_ }
    , bytes_read_{ other.bytes_read_ }
#endif
  {
    other.owner_ = false;
  }

  /// Move assignment operator.
  File& operator=(File&& other) {
    fd_ = other.fd_;
    device_alignment_ = other.device_alignment_;
    filename_ = std::move(other.filename_);
    owner_ = other.owner_;
#ifdef IO_STATISTICS
    bytes_written_ = other.bytes_written_;
    read_count_ = other.read_count_;
    bytes_read_ = other.bytes_read_;
#endif
    other.owner_ = false;
    return *this;
  }

 protected:
  core::Status Open(int flags, FileCreateDisposition create_disposition, 
                  uint64_t kSegmentSize, bool* exists = nullptr);

 public:
  core::Status Close();
  core::Status Delete();

  uint64_t size() const {
    struct stat stat_buffer;
    int result = ::fstat(fd_, &stat_buffer);
    return (result == 0) ? stat_buffer.st_size : 0;
  }

  size_t device_alignment() const {
    return device_alignment_;
  }

  const std::string& filename() const {
    return filename_;
  }

#ifdef IO_STATISTICS
  uint64_t bytes_written() const {
    return bytes_written_.load();
  }
  uint64_t read_count() const {
    return read_count_.load();
  }
  uint64_t bytes_read() const {
    return bytes_read_.load();
  }
#endif

 private:
  core::Status GetDeviceAlignment();
  static int GetCreateDisposition(FileCreateDisposition create_disposition);

 protected:
  int fd_;

 private:
  size_t device_alignment_;
  std::string filename_;
  bool owner_;

#ifdef IO_STATISTICS
 protected:
  std::atomic<uint64_t> bytes_written_;
  std::atomic<uint64_t> read_count_;
  std::atomic<uint64_t> bytes_read_;
#endif
};

class QueueFile;

/// The QueueIoHandler class encapsulates completions for async file I/O, where the completions
/// are put on the AIO completion queue.
class QueueIoHandler {
 public:
  typedef QueueFile async_file_t;

 private:
  constexpr static int kMaxEvents = 128;

 public:
  QueueIoHandler()
    : io_object_{ 0 } {
  }
  QueueIoHandler(size_t max_threads)
    : io_object_{ 0 } {
    int result = ::io_setup(kMaxEvents, &io_object_);
    assert(result >= 0);
  }

  /// Move constructor
  QueueIoHandler(QueueIoHandler&& other) {
    io_object_ = other.io_object_;
    other.io_object_ = 0;
  }

  ~QueueIoHandler() {
    io_context_t io_object = io_object_;
    io_object_ = 0;
    if(io_object != 0)
      ::io_destroy(io_object);
  }

  /// Invoked whenever a Linux AIO completes.
  static void IoCompletionCallback(io_context_t ctx, struct iocb* iocb, long res, long res2);

  struct IoCallbackContext {
    IoCallbackContext(FileOperationType operation, int fd, size_t offset, uint32_t length,
                      uint8_t* buffer, core::IAsyncContext* context_, core::AsyncIOCallback callback_)
      : caller_context{ context_ }
      , callback{ callback_ } {
      if(FileOperationType::Read == operation) {
        ::io_prep_pread(&this->parent_iocb, fd, buffer, length, offset);
      } else {
        ::io_prep_pwrite(&this->parent_iocb, fd, buffer, length, offset);
      }
      ::io_set_callback(&this->parent_iocb, IoCompletionCallback);
    }

    // WARNING: "parent_iocb" must be the first field in AioCallbackContext. This class is a C-style
    // subclass of "struct iocb".

    /// The iocb structure for Linux AIO.
    struct iocb parent_iocb;

    /// Caller callback context.
    core::IAsyncContext* caller_context;

    /// The caller's asynchronous callback function
    core::AsyncIOCallback callback;
  };

  inline io_context_t io_object() const {
    return io_object_;
  }

  /// Try to execute the next IO completion on the queue, if any.
  bool TryComplete();

  // Process IO completions on queue with timeout
  int QueueRun(int timeout_secs);

 private:
  /// The Linux AIO context used for IO completions.
  io_context_t io_object_;
};

/// The QueueFile class encapsulates asynchronous reads and writes, using the specified AIO
/// context.
class QueueFile : public File {
 public:
  QueueFile()
    : File()
    , io_object_{ nullptr } {
  }
  QueueFile(const std::string& filename)
    : File(filename)
    , io_object_{ nullptr } {
  }
  /// Move constructor
  QueueFile(QueueFile&& other)
    : File(std::move(other))
    , io_object_{ other.io_object_ } {
  }
  /// Move assignment operator.
  QueueFile& operator=(QueueFile&& other) {
    File::operator=(std::move(other));
    io_object_ = other.io_object_;
    return *this;
  }

  core::Status Open(FileCreateDisposition create_disposition, const FileOptions& options,
              QueueIoHandler* handler, uint64_t kSegmentSize, bool* exists = nullptr);

  core::Status Read(size_t offset, uint32_t length, uint8_t* buffer,
                    core::IAsyncContext& context, core::AsyncIOCallback callback) const;
  core::Status Write(size_t offset, uint32_t length, const uint8_t* buffer,
                     core::IAsyncContext& context, core::AsyncIOCallback callback);
  core::Status ResizeSegment(uint64_t _segment_size);

 private:
  core::Status ScheduleOperation(FileOperationType operationType, uint8_t* buffer, size_t offset,
                           uint32_t length, core::IAsyncContext& context, core::AsyncIOCallback callback);

  io_context_t io_object_;
};

#ifdef MEM_IDEV

// #define DEFAULT_MEMORY_SIZE_MB 1024

class LocalMemory;

class LocalMemoryIoHandler {
  public:
  typedef LocalMemory async_file_t;

 public:
  LocalMemoryIoHandler() {}
  LocalMemoryIoHandler(size_t max_threads) {}
  LocalMemoryIoHandler(LocalMemoryIoHandler&& other) {}
  ~LocalMemoryIoHandler() {}

  /// Try to execute the next IO completion on the queue, if any.
  bool TryComplete();

  // Process IO completions on queue with timeout
  int QueueRun(int timeout_secs);
};

class LocalMemory {
  public:
  LocalMemory()
    : virtfilename{ (std::string)"default_virtual_file" }
    // , capacity{ (uint64_t)(-1) }
    , segment_size{ 0 }
    , sector_size{ 1 }
    , num_r { 0 }
    , num_w { 0 }
    , segment_ptr{ nullptr } {
  }
  LocalMemory(const std::string& virtfilename_)
    : virtfilename{ virtfilename_ }
    // , capacity{ (uint64_t)(-1) }
    , segment_size{ 0 }
    , sector_size{ 1 }
    , num_r { 0 }
    , num_w { 0 }
    , segment_ptr{ nullptr } {
      // segment_ptr = (uint8_t*)std::malloc(sizeof(uint8_t) * segment_size);
      // if(!segment_ptr) throw std::runtime_error("local memory exhausted.");
  }
  // LocalMemory(const std::string& virtfilename_, uint64_t segment_size_mb_)
  //   : virtfilename{ virtfilename_ }
  //    , capacity{ (uint64_t)(-1) }
  //    , segment_size{ 1024 * 1024 * segment_size_mb_ }
  //   , sector_size{ 1 }
  //   , segment_ptr{ nullptr } {
  //     segment_ptr = (uint8_t*)std::malloc(sizeof(uint8_t) * segment_size);
  //     if(!segment_ptr) throw std::runtime_error("local memory exhausted.");
  // }
  ~LocalMemory() {
    if(segment_ptr){
      std::free(segment_ptr);
      segment_ptr = nullptr;
    }
  }
  LocalMemory(LocalMemory&& other)
    : virtfilename{ other.virtfilename }
    // , capacity{ other.capacity }
    , segment_size{ other.segment_size }
    , sector_size{ other.sector_size }
    , num_r { other.num_r }
    , num_w { other.num_w }
    , segment_ptr{ other.segment_ptr } {
  }
  LocalMemory& operator=(LocalMemory&& other) {
    virtfilename = other.virtfilename;
    // capacity = other.capacity;
    segment_size = other.segment_size;
    sector_size = other.sector_size;
    segment_ptr = other.segment_ptr;
    num_r = other.num_r;
    num_w = other.num_w;
    return *this;
  }

  uint64_t size() const {
    return segment_size;
  }

  size_t device_alignment() const {
    return 1;
  }

  const std::string& filename() const {
    return virtfilename;
  }

  core::Status Open(FileCreateDisposition create_disposition, const FileOptions& options,
              LocalMemoryIoHandler* handler, uint64_t kSegmentSize, bool* exists = nullptr);
  core::Status Read(size_t offset, uint32_t length, uint8_t* buffer,
                    core::IAsyncContext& context, core::AsyncIOCallback callback);
  core::Status Write(size_t offset, uint32_t length, const uint8_t* buffer,
                     core::IAsyncContext& context, core::AsyncIOCallback callback);
  core::Status Delete();
  core::Status Close();
  core::Status ResizeSegment(uint64_t _segment_size);

 private:
  uint8_t* segment_ptr;

 protected:
  // uint64_t capacity;
  uint64_t segment_size, sector_size, num_r, num_w;
  std::string virtfilename;
};

#endif

#ifdef FASTER_URING

class alignas(64) SpinLock {
public:
    SpinLock(): locked_(false) {}

    void Acquire() noexcept {
        for (;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) {
                return;
            }

            while (locked_.load(std::memory_order_relaxed)) {
                __builtin_ia32_pause();
            }
        }
    }

    void Release() noexcept {
        locked_.store(false, std::memory_order_release);
    }
private:
    std::atomic_bool locked_;
};

class UringFile;

/// The QueueIoHandler class encapsulates completions for async file I/O, where the completions
/// are put on the AIO completion queue.
class UringIoHandler {
 public:
  typedef UringFile async_file_t;

 private:
  constexpr static int kMaxEvents = 128;

 public:
  UringIoHandler() {
    ring_ = new struct io_uring();
    int ret = io_uring_queue_init(kMaxEvents, ring_, 0);
    assert(ret == 0);
  }

  UringIoHandler(size_t max_threads) {
    ring_ = new struct io_uring();
    int ret = io_uring_queue_init(kMaxEvents, ring_, 0);
    assert(ret == 0);
  }

  /// Move constructor
  UringIoHandler(UringIoHandler&& other) {
    ring_ = other.ring_;
    other.ring_ = 0;
  }

  ~UringIoHandler() {
    if (ring_ != 0) {
      io_uring_queue_exit(ring_);
      delete ring_;
    }
  }

  /*
  /// Invoked whenever a Linux AIO completes.
  static void IoCompletionCallback(io_context_t ctx, struct iocb* iocb, long res, long res2);
  */
  struct IoCallbackContext {
    IoCallbackContext(bool is_read, int fd, uint8_t* buffer, size_t length, size_t offset, core::IAsyncContext* context_, core::AsyncIOCallback callback_)
      : is_read_(is_read)
      , fd_(fd)
      , vec_{buffer, length}
      , offset_(offset)
      , caller_context{ context_ }
      , callback{ callback_ } {}

    bool is_read_;

    int fd_;
    struct iovec vec_;
    size_t offset_;

    /// Caller callback context.
    core::IAsyncContext* caller_context;

    /// The caller's asynchronous callback function
    core::AsyncIOCallback callback;
  };

  inline struct io_uring* io_uring() const {
    return ring_;
  }

  inline SpinLock* sq_lock() {
    return &sq_lock_;
  }

  /// Try to execute the next IO completion on the queue, if any.
  bool TryComplete();
  int QueueRun(int timeout_secs);

private:
  /// The io_uring for all the I/Os
  struct io_uring* ring_;
  SpinLock sq_lock_, cq_lock_;
};

/// The UringFile class encapsulates asynchronous reads and writes, using the specified
/// io_uring
class UringFile : public File {
 public:
  UringFile()
    : File()
    , ring_{ nullptr } {
  }
  UringFile(const std::string& filename)
    : File(filename)
    , ring_{ nullptr } {
  }
  /// Move constructor
  UringFile(UringFile&& other)
    : File(std::move(other))
    , ring_{ other.ring_ }
    , sq_lock_{ other.sq_lock_ } {
  }
  /// Move assignment operator.
  UringFile& operator=(UringFile&& other) {
    File::operator=(std::move(other));
    ring_ = other.ring_;
    sq_lock_ = other.sq_lock_;
    return *this;
  }

  core::Status Open(FileCreateDisposition create_disposition, const FileOptions& options,
              UringIoHandler* handler, uint64_t kSegmentSize, bool* exists = nullptr);

  core::Status Read(size_t offset, uint32_t length, uint8_t* buffer,
              core::IAsyncContext& context, core::AsyncIOCallback callback) const;
  core::Status Write(size_t offset, uint32_t length, const uint8_t* buffer,
               core::IAsyncContext& context, core::AsyncIOCallback callback);

 private:
  core::Status ScheduleOperation(FileOperationType operationType, uint8_t* buffer, size_t offset,
                                 uint32_t length, core::IAsyncContext& context, core::AsyncIOCallback callback);

  struct io_uring* ring_;
  SpinLock* sq_lock_;
};

#endif

}
} // namespace FASTER::environment
