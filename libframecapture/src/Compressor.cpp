/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include "Compressor.h"

#include <lz4.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include "utils/debug.h"
#include "utils/utils.h"

#define __CLASS__ "Compressor"

namespace sdm {

int Compressor::Init(const CompressionConfig &config, FILE *fp, CompressorCallback *callback) {
  std::lock_guard<std::mutex> obj(mutex_);
  if (!config.buf_size || (config.target_time <= 0) || (fp == nullptr) || (callback == nullptr)) {
    return -1;
  }

  // Create 3 threads for compression and 1 for file write.
  for (int i = 0; i < kReadThreadCount; i++) {
    read_threads_.push_back(std::thread(&Compressor::ProcessReadQueue, this));
  }

  std::thread write_thread(&Compressor::ProcessWriteQueue, this);
  write_thread_.swap(write_thread);

  comp_config_ = config;
  fp_ = fp;
  callback_ = callback;

  return 0;
}

int Compressor::GetOutputBufferConfig(int *buffer_depth, int *buffer_size) {
  std::lock_guard<std::mutex> obj(mutex_);
  *buffer_depth = kMaxBufferCount;
  *buffer_size = LZ4_compressBound(comp_config_.buf_size);
  return 0;
}

int Compressor::QueueBuffer(const QueueItem &item) {
  if (item.src == nullptr || item.dst == nullptr || !item.src_size || !item.dst_size) {
    return -1;
  }

  {
    std::lock_guard<std::mutex> obj(mutex_);
    read_queue_.push(item);
    cv_.notify_all();
  }

  return 0;
}

QueueItem Compressor::DequeueBuffer() {
  DTRACE_SCOPED();
  std::unique_lock<std::mutex> obj(mutex_);
  while (free_pool_.empty()) {
    cv_.wait(obj);
  }
  QueueItem item = free_pool_.front();
  free_pool_.pop();

  DLOGI("Slot: %d", item.slot);
  return item;
}

void Compressor::UpdateModel() {
  std::unique_lock<std::mutex> obj(mutex_);
  if (!comp_config_.dynamic_comp_ratio) {
    // clear stats. No change in comp acc.
    valid_stats_.clear();
  }

  // Model operates on last 3 frames.
  if (valid_stats_.size() < kFlushFrequency) {
    return;
  }

  // Derive compression factor from fwrite duration.
  // float fwrite_duration = 0.0f;
  float compression_duration = 0.0f;
  uint32_t num_frames = valid_stats_.size();
  for (auto &stat : valid_stats_) {
    compression_duration += stat.comp_duration_in_ms;
    DLOGI("fwrite_duration: %f compression_duration: %f", stat.fwrite_duration_in_ms,
          stat.comp_duration_in_ms);
  }

  DLOGI("Compression Achieved: %f", valid_stats_.at(0).comp_achieved);
  compression_duration /= num_frames;

  // If fwrite misses by 20% then data need to be compressed more by 20%.
  float compression_req_boost = compression_duration / comp_config_.target_time;

  comp_acc_ *= compression_req_boost;
  // compression level should be within range from 1 to 32000
  const uint32_t kMinAceleration = 1;
  const uint32_t kMAxAceleration = 32000;
  comp_acc_ = std::max(comp_acc_, kMinAceleration);
  comp_acc_ = std::min(comp_acc_, kMAxAceleration);

  valid_stats_.clear();
  DLOGI("compression acceleration %d", comp_acc_);
}

void Compressor::ProcessReadQueue() {
  SetRealTimePriority();
  while (1) {
    QueueItem item = {};
    {
      std::unique_lock<std::mutex> obj(mutex_);
      while (read_queue_.empty() && !exit_) {
        // Wait for buffer to be queued.
        cv_.wait(obj);
      }
      if (exit_) {
        break;
      }
      if (read_queue_.empty()) {
        continue;
      }
      item = read_queue_.front();
      read_queue_.pop();
      cv_.notify_all();
    }

    // Track fwrite duration and update compression level.
    UpdateModel();
    // Wait for buffer to be filled.
    Fence::Wait(item.acquire_fence);
    // Compress Item with LZ4.
    item.compressed_size = PerformCompression(item);

    // Push the item to write queue.
    {
      std::unique_lock<std::mutex> obj(mutex_);
      write_queue_.push(item);
      cv_.notify_all();
    }
  }
}

int Compressor::PerformCompression(const QueueItem &item) {
  DTRACE_SCOPED();
  uint64_t begin_time = GetSystemTimeInNs();
  int compression_level = GetCompressionLevel();
  int compressed_size =
      LZ4_compress_fast(static_cast<const char *>(item.src), static_cast<char *>(item.dst),
                        item.src_size, item.dst_size, compression_level);

  CompStats stats = {};
  stats.comp_achieved = (static_cast<float>(item.src_size) / compressed_size);
  stats.comp_duration_in_ms = static_cast<float>(GetSystemTimeInNs() - begin_time) / (1000 * 1000);
  stats.frame_number = item.frame_number;
  stats.compression_level = compression_level;
  DLOGI("stats.comp_duration_in_ms: %f stats.comp_achieved: %f", stats.comp_duration_in_ms,
        stats.comp_achieved);

  {
    std::unique_lock<std::mutex> obj(mutex_);
    stats_.push_back(stats);
  }

  return compressed_size;
}

int Compressor::GetCompressionLevel() {
  std::unique_lock<std::mutex> obj(mutex_);
  return comp_acc_;
}

void Compressor::PerformFwrite(const QueueItem &item) {
  DTRACE_SCOPED();
  uint64_t begin_time = GetSystemTimeInNs();

  // Write metadata.
  fwrite(item.dst, item.compressed_size, 1, fp_);
  // Flush for every 3 frames.
  frames_to_flush_++;
  if (frames_to_flush_ == kFlushFrequency) {
    fflush(fp_);
    frames_to_flush_ = 0;
  }

  // Update stats.
  float fwrite_duration_in_ms =
      static_cast<float>(GetSystemTimeInNs() - begin_time) / (1000 * 1000);
  DLOGI("fwrite_duration_in_ms: %f", fwrite_duration_in_ms);
  {
    std::unique_lock<std::mutex> obj(mutex_);
    for (auto it = stats_.begin(); it != stats_.end(); it++) {
      if (it->frame_number != item.frame_number) {
        continue;
      }
      it->fwrite_duration_in_ms = fwrite_duration_in_ms;
      it->input_fd = item.input_fd;
      it->compressed_size = item.compressed_size;
      valid_stats_.push_back(*it);
      callback_->FillMetadata(*it);
      stats_.erase(it);
      break;
    }
  }
}

void Compressor::ProcessWriteQueue() {
  SetRealTimePriority();
  while (1) {
    QueueItem item = {};
    {
      std::unique_lock<std::mutex> obj(mutex_);
      while (write_queue_.empty() && !exit_) {
        // Wait for buffer to be queued.
        cv_.wait(obj);
      }
      if (exit_) {
        break;
      }
      if (write_queue_.empty()) {
        continue;
      }
      item = write_queue_.front();
      write_queue_.pop();
      cv_.notify_all();
    }

    PerformFwrite(item);

    // Push the item to free queue.
    {
      std::unique_lock<std::mutex> obj(mutex_);
      free_pool_.push(item);
      cv_.notify_all();
    }
  }
}

void Compressor::SetBufferPool(const std::vector<QueueItem> &buffer_pool) {
  std::unique_lock<std::mutex> obj(mutex_);
  // Push all buffers to free_pool.
  for (auto &buffer : buffer_pool) {
    free_pool_.push(buffer);
  }
}

Compressor::~Compressor() {
  DTRACE_SCOPED();
  exit_ = true;
  cv_.notify_all();
  DLOGI("Waiting for threads to join");
  for (int i = 0; i < kReadThreadCount; i++) {
    read_threads_[i].join();
  }
  write_thread_.join();
  fflush(fp_);
  DLOGI("Clean exit");
}

}  // namespace sdm
