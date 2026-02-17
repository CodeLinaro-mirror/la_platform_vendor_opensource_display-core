/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __COMPRESSOR_H__
#define __COMPRESSOR_H__

#include <sys/fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils/fence.h>
#include <utils/utils.h>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>
#include <numeric>
#include <chrono>
#include <condition_variable>

namespace sdm {

struct CompressionConfig {
  uint32_t buf_size = 0;           // Buffer size in bytes.
  float target_time = 0.0f;        // Expected target time for compression in ms.
  bool dynamic_comp_ratio = true;  // Client can specify compression level / ratio.
};

struct QueueItem {
  uint32_t frame_number = 0;
  void *src = nullptr;
  void *dst = nullptr;
  uint32_t src_size = 0;
  uint32_t dst_size = 0;
  uint32_t compressed_size = 0;
  shared_ptr<Fence> acquire_fence = nullptr;
  uint32_t slot = 0;
  int32_t input_fd = -1;
};

struct CompStats {
  uint32_t frame_number = 0;
  float comp_achieved = 0;
  float comp_duration_in_ms = 0;
  float fwrite_duration_in_ms = 0;
  uint32_t compressed_size = 0;
  int32_t input_fd = -1;
  int32_t compression_level = 0;
};

class CompressorCallback {
 public:
  virtual ~CompressorCallback() = default;
  virtual void FillMetadata(struct CompStats &stats) = 0;
};

class Compressor {
 public:
  Compressor() {}
  ~Compressor();

  int Init(const CompressionConfig &config, FILE *fp, CompressorCallback *callback);
  int GetOutputBufferConfig(int *buffer_depth, int *buffer_size);
  void SetBufferPool(const std::vector<QueueItem> &buffer_pool);
  int QueueBuffer(const QueueItem &item);
  QueueItem DequeueBuffer();

 private:
  const int kReadThreadCount = 1;
  const int kMaxBufferCount = 20;
  const int kFlushFrequency = 5;  // Flush data for every these no of frames.

  void ProcessReadQueue();
  void ProcessWriteQueue();
  int PerformCompression(const QueueItem &item);
  void PerformFwrite(const QueueItem &item);
  int GetCompressionLevel();
  void UpdateModel();

  std::mutex mutex_;
  std::condition_variable cv_;
  CompressionConfig comp_config_ = {};
  std::vector<std::thread> read_threads_;
  std::thread write_thread_;
  uint32_t frames_to_flush_ = 0;

  std::queue<QueueItem> read_queue_;
  std::queue<QueueItem> write_queue_;
  std::queue<QueueItem> free_pool_;

  std::vector<CompStats> stats_;
  std::vector<CompStats> valid_stats_;
  uint32_t comp_acc_ = 32;

  bool exit_ = false;
  FILE *fp_ = nullptr;
  CompressorCallback *callback_ = nullptr;
};

}  // namespace sdm

#endif  // __COMPRESSOR_H__
