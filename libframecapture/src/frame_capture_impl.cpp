/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <utils/fence.h>
#include <sys/mman.h>
#include <utils/formats.h>
#include <utils/rect.h>
#include <utils/utils.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <time.h>
#include <list>
#include <vector>
#include <string>
#include "frame_capture_impl.h"
#include <debug_handler.h>

#define __CLASS__ "FrameCaptureImpl"

namespace sdm {

FrameCaptureImpl::FrameCaptureImpl(DisplayInterface *display_intf,
                                   BufferAllocator *buffer_allocator)
    : display_intf_(display_intf), buffer_allocator_(buffer_allocator) {}

int FrameCaptureImpl::Init() {
  uint32_t active_index = 0;
  DisplayError error = display_intf_->GetActiveConfig(&active_index);
  if (error != kErrorNone) {
    DLOGE("GetActiveConfig failed. Error = %d", error);
    return -1;
  }

  error = display_intf_->GetConfig(active_index, &display_config_);
  if (error != kErrorNone) {
    DLOGE("GetConfig failed. Error = %d", error);
    return -1;
  }

  return 0;
}

int FrameCaptureImpl::DeInit() {
  TeardownConcurrentWriteback();
  return 0;
}

int FrameCaptureImpl::ConfigureFCM(CWBPacketData &data) {
  if (data.cwb_config.num_parallel_buffers == 0) {
    DLOGE("Invalid num of side by side buffer");
    return -1;
  }
  if (data.stop_cwb) {
    trigger_cwb_ = false;
    return 0;
  }

  {
    std::lock_guard<std::mutex> lock(cwb_state_lock_);
    if (cwb_state_.cwb_client != kCWBClientNone) {
      DLOGE("CWB is in use with client = %d", cwb_state_.cwb_client);
      return -1;
    }
  }

  LayerRect cwb_roi = data.cwb_config.cwb_roi;
  {
    std::lock_guard<std::mutex> lock(metadata_lock_);
    metadata_ << "# GLOBAL_METADATA:" << std::endl;
    metadata_ << "EYE_INDEX = " << data.eye_index << std::endl;
    metadata_ << "FIELD_INDEX = " << data.field_index << std::endl;
    metadata_ << "FCM_ROI = " << cwb_roi.left << ", " << cwb_roi.top << ", " << cwb_roi.right
              << ", " << cwb_roi.bottom << std::endl;
    metadata_ << "SIDE_BY_SIDE_BUFFERS = " << data.cwb_config.num_parallel_buffers << std::endl;
    // NUM_FRAMES = 0 will indicate that Cwb dump will be taken until separate
    // stop will be triggered
    metadata_ << "NUM_FRAMES = " << data.frame_dump_count << std::endl;
  }

  int err = UpdateCWBRoi(cwb_roi, data.eye_index, data.field_index, &data.cwb_config.cwb_roi);
  if (err) {
    DLOGE("Invalid CWB ROI on Fsc panel");
    TeardownConcurrentWriteback();
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(cwb_state_lock_);
    cwb_state_.cwb_client = kCWBClientFrameDump;
  }

  DisplayState display_state = {};
  display_intf_->GetDisplayState(&display_state);
  if (display_state == kStateOff) {
    DLOGE("CWB requested on Powered-Off display.");
    TeardownConcurrentWriteback();
    return -1;
  }

  sdm::LayerRect disp_rect = {0, 0, static_cast<float>(display_config_.x_pixels),
                              static_cast<float>(display_config_.y_pixels)};
  sdm::LayerRect cwb_rect = data.cwb_config.cwb_roi;
  if (!Contains(disp_rect, cwb_rect)) {
    DLOGE("cwb roi is not within display roi");
    TeardownConcurrentWriteback();
    return -1;
  }

  sdm::LayerRect actual_roi = data.cwb_config.cwb_roi;
  DLOGI("Display cwb_roi: [l t r b] : [%f %f %f %f]", actual_roi.left, actual_roi.top,
        actual_roi.right, actual_roi.bottom);

  cwb_buffer_pool_size_ = std::max(1, static_cast<int>(data.buffer_depth));
  enable_compression_ = data.enable_compression;

  if (AllocateOutputBuffers(data.cwb_config)) {
    DLOGE("Failed to allocate CWB buffers");
    TeardownConcurrentWriteback();
    return -1;
  }

  cwb_config_ = data.cwb_config;

  if (SetupCWBFile(available_out_buffer_.front())) {
    DLOGE("Failed to setup CWB file");
    TeardownConcurrentWriteback();
    return -1;
  }

  if (enable_compression_ && InitCompressionCtx()) {
    DLOGE("Failed to initialize comp. ctx");
    TeardownConcurrentWriteback();
    return -1;
  }

  dump_frame_count_ = data.frame_dump_count;
  dump_frame_index_ = 0;
  teardown_done_ = false;
  trigger_cwb_ = true;

  // initialise Async task to sumbit CWB buffer to SDM
  cwb_trigger_future_ = std::async(std::launch::async, [&]() { SubmitCWBConfig(); });

  cwb_dump_future_ = std::async(std::launch::async, [&]() { DumpOutputBufferToFile(); });

  return 0;
}

void FrameCaptureImpl::DumpOutputBuffer(const BufferInfo &buffer_info, void *base) {
  size_t result = 0;
  if (!base) {
    DLOGE("Buffer base ptr is Null");
    return;
  }
  if (!cwb_fp_) {
    DLOGE("File ptr is null");
    return;
  }
  uint64_t begin_time = GetSystemTimeInNs();
  result = fwrite(base, buffer_info.alloc_buffer_info.size, 1, cwb_fp_);
  fwrite_time_ = (static_cast<float>(GetSystemTimeInNs() - begin_time)) / (1000 * 1000);
}

int FrameCaptureImpl::InitCompressionCtx() {
  compressor_ = new Compressor();
  CompressionConfig config = {};
  BufferInfo info = available_out_buffer_.front();
  config.buf_size = info.alloc_buffer_info.size;
  config.target_time = 1000 / static_cast<float>(display_config_.fps);
  config.dynamic_comp_ratio = true;
  config.buffer_count = static_cast<uint32_t>(cwb_buffer_pool_size_);

  if (compressor_->Init(config, cwb_fp_, this) != 0) {
    DLOGE("Failed to init compressor buf size %d time: %f fp: %p", config.buf_size,
          config.target_time, cwb_fp_);
    return -1;
  }

  int buffer_depth = 0;
  int buffer_size = 0;
  compressor_->GetOutputBufferConfig(&buffer_depth, &buffer_size);
  buffer_pool_.resize(buffer_depth);

  // Pick input buffers from available_out_buffer_.
  for (uint32_t i = 0; i < buffer_depth; i++) {
    info = available_out_buffer_.front();
    lz4_input_buffer_info_.push_back(info);
    available_out_buffer_.pop_front();

    void *buffer = mmap(NULL, info.alloc_buffer_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        info.alloc_buffer_info.fd, 0);
    QueueItem &item = buffer_pool_.at(i);
    item.src = buffer;
    item.src_size = info.alloc_buffer_info.size;
    item.slot = i;
    DLOGV("Input buffers fd: %d", info.alloc_buffer_info.fd);
  }

  // Allocate output buffers for compression.
  float metadata_factor = FLOAT(buffer_size) / info.alloc_buffer_info.size;
  info.buffer_config.width = INT(info.alloc_buffer_info.aligned_width * metadata_factor) + 1;

  for (uint32_t i = 0; i < buffer_depth; i++) {
    buffer_allocator_->AllocateBuffer(&info);
    void *buffer = mmap(NULL, info.alloc_buffer_info.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        info.alloc_buffer_info.fd, 0);
    QueueItem &item = buffer_pool_.at(i);
    item.dst = buffer;
    item.dst_size = info.alloc_buffer_info.size;
    lz4_output_buffer_info_.push_back(info);
  }

  DLOGV("buf size: input %d output: %d required: %d", buffer_pool_.front().src_size,
        buffer_pool_.front().dst_size, buffer_size);
  // Set buffer pool.
  compressor_->SetBufferPool(buffer_pool_);

  return 0;
}

void FrameCaptureImpl::DumpOutputBufferToFile() {
  SetRealTimePriority();

  bool exit = false;
  while (!exit) {
    BufferInfo buffer_info = {};
    {
      std::unique_lock<std::mutex> lock(buffer_manage_lock_);
      if (dump_out_buffer_.empty()) {
        if (currently_used_buffers_.empty() && !trigger_cwb_) {
          exit = true;
          continue;
        }
        buf_cv_.wait(lock);
      }
      if (dump_out_buffer_.empty()) {  // spurious wakeup.
        continue;
      }
      buffer_info = dump_out_buffer_.front();
      dump_out_buffer_.pop_front();
      if (enable_compression_) {
        uint32_t slot = 0;
        for (uint32_t i = 0; i < lz4_input_buffer_info_.size(); i++) {
          if (buffer_info.alloc_buffer_info.fd ==
              lz4_input_buffer_info_.at(i).alloc_buffer_info.fd) {
            DLOGV("found slot: %d for fd: %d", i, buffer_info.alloc_buffer_info.fd);
            slot = i;
            break;
          }
        }
        QueueItem item = buffer_pool_.at(slot);
        item.frame_number = dumped_frames_;
        item.input_fd = buffer_info.alloc_buffer_info.fd;
        compressor_->QueueBuffer(item);
        dumped_frames_++;
        if (dumped_frames_ == dump_frame_count_) {
          // Dumped all frames.
          exit = true;
        }
        buf_cv_.notify_all();
        continue;
      }
    }

    DumpOutputBuffer(buffer_info, fd_buffer_base_map_[buffer_info.alloc_buffer_info.fd]);

    {
      std::lock_guard<std::mutex> lock(metadata_lock_);
      metadata_ << dumped_frames_ << ", "
                << metadata_map_[buffer_info.alloc_buffer_info.fd].qtimer_ticks << ", "
                << metadata_map_[buffer_info.alloc_buffer_info.fd].fence_time << ", "
                << fwrite_time_ << ", 0, 0, 0" << std::endl;
    }

    {
      std::lock_guard<std::mutex> lock(buffer_manage_lock_);
      available_out_buffer_.push_back(buffer_info);
      buf_cv_.notify_all();
      dumped_frames_++;
      if (dumped_frames_ == dump_frame_count_) {
        // Dumped all frames.
        exit = true;
      }
    }
  }
}

void FrameCaptureImpl::FillMetadata(struct CompStats &stats) {
  {
    std::lock_guard<std::mutex> lock(metadata_lock_);
    metadata_ << stats.frame_number << ", " << metadata_map_[stats.input_fd].qtimer_ticks << ", "
              << metadata_map_[stats.input_fd].fence_time << ", " << stats.fwrite_duration_in_ms
              << ", " << stats.compression_level << ", " << stats.comp_duration_in_ms << ", "
              << stats.compressed_size << std::endl;
  }
}

int FrameCaptureImpl::SetReadbackBuffer(const BufferInfo &info, shared_ptr<Fence> acquire_fence) {
  if ((cwb_state_.cwb_status == CWBStatus::kCWBTeardown) ||
      (Fence::GetStatus(cwb_state_.teardown_frame_retire_fence) != Fence::Status::kSignaled)) {
    DLOGE("CWB teardown is currently undergoing on display");
    return -1;
  }

  if (info.alloc_buffer_info.fd < 0) {
    DLOGE("Bad parameter: fd is null");
    return -1;
  }

  // Configure the output buffer as Readback buffer
  output_buffer_.width = info.alloc_buffer_info.aligned_width;
  output_buffer_.height = info.alloc_buffer_info.aligned_height;
  output_buffer_.unaligned_width = info.buffer_config.width;
  output_buffer_.unaligned_height = info.buffer_config.height;
  output_buffer_.format = info.alloc_buffer_info.format;
  output_buffer_.planes[0].fd = info.alloc_buffer_info.fd;
  output_buffer_.planes[0].stride = info.alloc_buffer_info.stride;
  output_buffer_.acquire_fence = acquire_fence;
  output_buffer_.handle_id = info.alloc_buffer_info.fd;
  output_buffer_.size = info.alloc_buffer_info.size;
  output_buffer_.usage = info.alloc_buffer_info.usage;

  if (output_buffer_.format == kFormatInvalid) {
    DLOGE("Format is not supported by SDM");
    return -1;
  }

  LayerRect &roi = cwb_config_.cwb_roi;
  LayerRect &full_rect = cwb_config_.cwb_full_rect;
  CwbTapPoint &tap_point = cwb_config_.tap_point;

  DLOGI(
      "CWB config from client: tap_point %d, CWB ROI Rect(%f %f %f %f), "
      "PU_as_CWB_ROI %d, Cwb full rect : (%f %f %f %f)",
      tap_point, roi.left, roi.top, roi.right, roi.bottom, cwb_config_.pu_as_cwb_roi,
      full_rect.left, full_rect.top, full_rect.right, full_rect.bottom);

  return 0;
}

void FrameCaptureImpl::NotifyCwbDone(int32_t status, const LayerBuffer &buffer) {
  {
    std::lock_guard<std::mutex> lock(cwb_state_lock_);
    if (cwb_state_.cwb_client == kCWBClientNone) {
      // teardown is done. Do not take any action on notifycwbdone
      return;
    }
  }
  {
    std::unique_lock<std::mutex> lock(buffer_manage_lock_);
    notify_count_++;
    char trace_str[64];
    snprintf(trace_str, sizeof(trace_str), "CWBDone_%d", notify_count_);
    DTRACE_BEGIN(trace_str);
    DTRACE_END();
    DLOGI("status=%d fd=%d notify_count=%d pending_notify=%d release_fence=%s", status,
          buffer.planes[0].fd, notify_count_, pending_notify_ - 1,
          Fence::GetStr(buffer.release_fence).c_str());

    Metadata data;
    data.qtimer_ticks = GetQtimerTicks();
    data.fence_time = Fence::GetSignalTime(buffer.release_fence);

    for (auto it = currently_used_buffers_.begin(); it != currently_used_buffers_.end(); ++it) {
      if (it->alloc_buffer_info.fd == buffer.planes[0].fd) {
        {
          std::lock_guard<std::mutex> lock(metadata_lock_);
          metadata_map_[buffer.planes[0].fd] = data;
        }
        dump_out_buffer_.push_back(*it);
        currently_used_buffers_.erase(it);
        break;
      }
    }
    pending_notify_--;
    buf_cv_.notify_all();
  }
}

void FrameCaptureImpl::SubmitCWBConfig() {
  SetRealTimePriority();

  clock_t begin_time = clock();
  bool exit = false;
  while (!exit) {
    if (enable_compression_) {
      QueueItem item = compressor_->DequeueBuffer();
      BufferInfo info = lz4_input_buffer_info_.at(item.slot);
      std::unique_lock<std::mutex> lock(buffer_manage_lock_);
      DLOGV("Deque: Slot: %d fd: %d", item.slot, info.alloc_buffer_info.fd);
      // Push this buffer to available queue.
      available_out_buffer_.push_back(info);
    }

    {
      const uint32_t max_in_flight = (cwb_config_.num_parallel_buffers > 1) ? 2u : 4u;
      std::unique_lock<std::mutex> lock(buffer_manage_lock_);
      while (available_out_buffer_.empty() || currently_used_buffers_.size() >= max_in_flight) {
        buf_cv_.wait(lock);
      }
    }

    // submit buffer to CWB manager
    {
      std::lock_guard<std::mutex> lock(buffer_manage_lock_);
      BufferInfo info = available_out_buffer_.front();
      int ret = SetReadbackBuffer(info, nullptr);
      if (ret) {
        DLOGE("Failed to set readback buffer");
        TeardownConcurrentWriteback();
        exit = true;
        return;
      }
      currently_used_buffers_.push_back(info);
      available_out_buffer_.pop_front();
      buf_cv_.notify_all();
    }

    clock_t cap_start = clock();
    DLOGI("frame=%d num_parallel_buffers=%u available=%zu currently_used=%zu buffer_fd %d",
          requested_frames_ + 1, cwb_config_.num_parallel_buffers, available_out_buffer_.size(),
          currently_used_buffers_.size(), output_buffer_.planes[0].fd);

    {
      char trace_str[64];
      snprintf(trace_str, sizeof(trace_str), "CWBTrigger_%d", requested_frames_ + 1);
      DTRACE_BEGIN(trace_str);
      DTRACE_END();
    }
    DisplayError error =
        display_intf_->CaptureCwb(output_buffer_, cwb_config_, kCWBClientFrameDump);
    if (error != kErrorNone) {
      DLOGE("Capture CWB failed. error: %d", error);
      TeardownConcurrentWriteback();
      exit = true;
      return;
    }

    // Wait for this CWB request to be picked up by a display commit (CWBProgram fired)
    // before submitting the next trigger. This prevents queue buildup in the CWB manager
    // and ensures 1 trigger per display commit, eliminating frame misses.
    while (display_intf_->HasPendingCwbRequest() && trigger_cwb_) {
      usleep(500);
    }

    dump_frame_index_++;
    pending_notify_++;
    requested_frames_++;
    DLOGV("Requested dump for frame: %d capture time: %f ms", requested_frames_,
          static_cast<float>(clock() - cap_start) / 1000.0);
    if ((dump_frame_count_ == requested_frames_) || !trigger_cwb_) {
      exit = true;
    }
  }
  float time_taken = (static_cast<float>(clock() - begin_time) / 1000.f);
  float dump_eff = ((1000.0 / display_config_.fps) * dump_frame_count_) / time_taken;
  DLOGV("Waiting for dump thread to exit. Time taken for %d frames: %f ms eff: %f",
        dump_frame_count_, time_taken, dump_eff);
  cwb_dump_future_.get();

  if (enable_compression_) {
    // Grab all buffers.
    std::vector<QueueItem> buffers;
    uint32_t buffer_depth = lz4_input_buffer_info_.size();
    for (uint32_t i = 0; i < buffer_depth; i++) {
      QueueItem item = compressor_->DequeueBuffer();
      DLOGV("Dequeued slot: %d", item.slot);
    }
    delete compressor_;
  }

  TeardownConcurrentWriteback();
}

int FrameCaptureImpl::TeardownConcurrentWriteback() {
  if (teardown_done_.exchange(true)) {
    DLOGW("Teardown already done, skipping duplicate call");
    return 0;
  }

  dump_frame_count_ = 0;
  dumped_frames_ = 0;
  requested_frames_ = 0;
  notify_count_ = 0;
  cwb_buffer_pool_size_ = 5;
  enable_compression_ = true;
  // Unmap and Free buffer
  DestroyOutputBuffers();
  cwb_config_ = {};
  output_buffer_ = {};
  available_out_buffer_.clear();
  dump_out_buffer_.clear();
  currently_used_buffers_.clear();
  lz4_input_buffer_info_.clear();
  lz4_output_buffer_info_.clear();
  trigger_cwb_ = false;
  if (cwb_fp_) {
    fclose(cwb_fp_);
    cwb_fp_ = nullptr;
  }

  // dumping the metadata
  {
    std::lock_guard<std::mutex> lock(metadata_lock_);
    metadata_map_.clear();
    if (metadata_file_.is_open()) {
      std::string content = metadata_.str();
      metadata_file_ << content;
      if (metadata_file_.fail() || metadata_file_.bad()) {
        DLOGE("metadata teardown write failed errno=%d (%s) bad=%d fail=%d", errno, strerror(errno),
              (int)metadata_file_.bad(), (int)metadata_file_.fail());
      }
      metadata_file_.close();
      metadata_.str(std::string());
    }
  }

  {
    std::lock_guard<std::mutex> lock(cwb_state_lock_);
    cwb_state_.cwb_client = kCWBClientNone;
    cwb_state_.cwb_status = CWBStatus::kCWBAvailable;
  }

  return 0;
}

int FrameCaptureImpl::SetupCWBFile(BufferInfo buffer_info) {
  char dir_path[PATH_MAX] = "/data/vendor/display/frame_dump";
  int status;

  status = mkdir(dir_path, 777);
  if ((status != 0) && errno != EEXIST) {
    DLOGE("Failed to create %s directory errno = %d, desc = %s", dir_path, errno, strerror(errno));
    return -1;
  }

  // Even if directory exists already, need to explicitly change the permission.
  if (chmod(dir_path, 0777) != 0) {
    DLOGE("Failed to change permissions on %s directory", dir_path);
    return -1;
  }

  char dump_file_name[PATH_MAX];
  snprintf(dump_file_name, sizeof(dump_file_name), "%s/fcm.raw", dir_path);
  cwb_fp_ = fopen(dump_file_name, "w+");
  if (!cwb_fp_) {
    DLOGE("Failed to open output dump file");
    return -1;
  }

  // Setup metadata file
  snprintf(dump_file_name, sizeof(dump_file_name), "%s/fcm_metadata.txt", dir_path);
  {
    std::lock_guard<std::mutex> lock(metadata_lock_);
    metadata_file_.open(dump_file_name, std::ios::out | std::ios::trunc);
    if (!metadata_file_.is_open()) {
      DLOGE("Failed to open metadata_fp_ file");
      return -1;
    }
  }

  return 0;
}

int FrameCaptureImpl::UpdateCWBRoi(sdm::LayerRect cwb_roi, int32_t eye_index, int32_t field_index,
                                   sdm::LayerRect *out_roi) {
  uint32_t disp_width = display_config_.x_pixels;
  uint32_t disp_height = display_config_.y_pixels;

  SDMDisplayViewMode view_mode = kViewModeNone;
  display_intf_->GetDisplayViewMode(&view_mode);

  DLOGI("view_mode=%d eye_index=%d field_index=%d cwb_roi=[%f %f %f %f]", view_mode, eye_index,
        field_index, cwb_roi.left, cwb_roi.top, cwb_roi.right, cwb_roi.bottom);

  if (view_mode == kViewModeBinocular) {
    // Binocular: display is split left/right per eye.
    // For single-eye capture (kLeftEye/kRightEye): client ROI is in per-eye space
    //   {0..half_width, 0..height}. The right-eye offset is applied internally.
    // For both-eyes capture (kBothEyes): client ROI is in full-display space
    //   {0..disp_width, 0..height} and can span across both eyes.
    uint32_t half_width = disp_width / 2;
    uint32_t eye_width = half_width;
    uint32_t eye_height = disp_height;

    if (display_config_.fsc_panel) {
      // for FSC if the actual display resolution is 1200X1200 dal will convert it 400X3600
      // converting it back to client space
      eye_width = half_width * display_config_.num_fsc_fields;
      eye_height = disp_height / display_config_.num_fsc_fields;

      if ((INT(cwb_roi.left) % display_config_.num_fsc_fields != 0) ||
          (INT(cwb_roi.top) % display_config_.num_fsc_fields != 0) ||
          (INT(cwb_roi.right) % display_config_.num_fsc_fields != 0) ||
          (INT(cwb_roi.bottom) % display_config_.num_fsc_fields != 0)) {
        DLOGE("ROI should be multiple of %d", display_config_.num_fsc_fields);
        return -1;
      }
    }

    if (eye_index == kBothEyes) {
      // Both eyes: capture full display width.
      // Client ROI is in hardware display space {0..hw_width, 0..hw_height}.
      if (cwb_roi == (sdm::LayerRect){0, 0, 0, 0}) {
        out_roi->left = 0;
        out_roi->top = 0;
        out_roi->right = static_cast<float>(disp_width);
        out_roi->bottom = static_cast<float>(disp_height);
      } else if (display_config_.fsc_panel) {
        // Client ROI is in hardware space (e.g., {0, 0, 2400, 1200}).
        // Validate against hardware full-display bounds.
        uint32_t hw_full_width = eye_width * 2;  // = disp_width * num_fsc_fields
        sdm::LayerRect hw_full_rect = {0, 0, static_cast<float>(hw_full_width),
                                       static_cast<float>(eye_height)};
        if (!Contains(hw_full_rect, cwb_roi)) {
          DLOGE("cwb roi is not within hardware full display rect for kBothEyes FSC");
          return -1;
        }
        // Convert from hardware space to FSC-transformed space.
        // top is not multiplied; bottom = top + roi_height * num_fsc_fields.
        uint32_t roi_height = UINT32(cwb_roi.bottom - cwb_roi.top);
        out_roi->left = cwb_roi.left / display_config_.num_fsc_fields;
        out_roi->top = cwb_roi.top;
        out_roi->right = cwb_roi.right / display_config_.num_fsc_fields;
        out_roi->bottom = cwb_roi.top + roi_height * display_config_.num_fsc_fields;
      } else {
        // Client ROI in full-display space; validate and use directly.
        sdm::LayerRect full_rect = {0, 0, static_cast<float>(disp_width),
                                    static_cast<float>(disp_height)};
        if (!Contains(full_rect, cwb_roi)) {
          DLOGE("cwb roi is not within full display rect for kBothEyes");
          return -1;
        }
        *out_roi = cwb_roi;
      }
      // No eye offset needed for both-eyes capture.
    } else {
      // Single eye (left or right): client ROI is in per-eye space.
      if (cwb_roi == (sdm::LayerRect){0, 0, 0, 0}) {
        out_roi->left = 0;
        out_roi->top = 0;
        out_roi->right = static_cast<float>(half_width);
        out_roi->bottom = static_cast<float>(disp_height);
      } else if (display_config_.fsc_panel) {
        // Client ROI is in hardware display space.
        // eye_width = half_width * num_fsc_fields = hardware half-width.
        // eye_height = disp_height / num_fsc_fields = hardware full height.
        // Validate against hardware-space eye bounds.
        if (cwb_roi.right > eye_width) {
          DLOGE("Invalid ROI for FSC binocular [l t r b] : [%f %f %f %f]", cwb_roi.left,
                cwb_roi.top, cwb_roi.right, cwb_roi.bottom);
          return -1;
        }
        // Convert from hardware space to FSC-transformed space used internally by SDM.
        // top is not multiplied; bottom = top + roi_height * num_fsc_fields.
        uint32_t roi_height = UINT32(cwb_roi.bottom - cwb_roi.top);
        out_roi->left = cwb_roi.left / display_config_.num_fsc_fields;
        out_roi->top = cwb_roi.top;
        out_roi->right = cwb_roi.right / display_config_.num_fsc_fields;
        out_roi->bottom = cwb_roi.top + roi_height * display_config_.num_fsc_fields;
      } else {
        *out_roi = cwb_roi;
      }

      // Validate ROI is within single-eye bounds.
      sdm::LayerRect eye_rect = {0, 0, static_cast<float>(half_width),
                                 static_cast<float>(disp_height)};
      if (!Contains(eye_rect, *out_roi)) {
        DLOGE("cwb roi is not within Eye roi [l t r b] : [%f %f %f %f]", out_roi->left,
              out_roi->top, out_roi->right, out_roi->bottom);
        return -1;
      }

      // Apply right-eye offset. Use half_width (hardware space) since out_roi
      // has already been transformed to hardware coordinates by the FSC transform above.
      if (eye_index == kRightEye) {
        out_roi->left += half_width;
        out_roi->right += half_width;
      }
    }

    // Apply FSC field offset.
    if (!display_config_.fsc_panel && field_index != kAllFSCField) {
      DLOGE("RGB display does not support Field dump");
      return -1;
    }
    if (display_config_.fsc_panel && field_index != kAllFSCField) {
      // Field selection is only supported when the ROI covers the full display height.
      uint32_t roi_height_hw =
          UINT32(out_roi->bottom - out_roi->top) / display_config_.num_fsc_fields;
      if (roi_height_hw != eye_height) {
        DLOGE("Field dump only supported for full-height ROI (roi_height=%u eye_height=%u)",
              roi_height_hw, eye_height);
        return -1;
      }
      if (field_index == kRedFscField) {
        out_roi->bottom /= display_config_.num_fsc_fields;
      } else if (field_index == kGreenFscField) {
        out_roi->top += eye_height;
        out_roi->bottom -= eye_height;
      } else if (field_index == kBlueFscField) {
        out_roi->top += eye_height * 2;
      }
    }

  } else {
    // kViewModeNone (mobile) or kViewModeMonocular: eye_index is ignored.
    // Client ROI is in hardware display space.
    // For FSC panels: hardware space = (x_pixels*num_fsc_fields) x (y_pixels/num_fsc_fields).
    // For non-FSC panels: hardware space = x_pixels x y_pixels (= disp_width x disp_height).
    if (cwb_roi == (sdm::LayerRect){0, 0, 0, 0}) {
      out_roi->left = 0;
      out_roi->top = 0;
      out_roi->right = static_cast<float>(disp_width);
      out_roi->bottom = static_cast<float>(disp_height);
    } else if (display_config_.fsc_panel) {
      // Client ROI is in hardware space (e.g., 2400x1200).
      // Validate against hardware full-display bounds.
      uint32_t hw_width = disp_width * display_config_.num_fsc_fields;
      uint32_t hw_height = disp_height / display_config_.num_fsc_fields;
      sdm::LayerRect hw_full_rect = {0, 0, static_cast<float>(hw_width),
                                     static_cast<float>(hw_height)};
      if (!Contains(hw_full_rect, cwb_roi)) {
        DLOGE("cwb roi is not within hardware display rect [l t r b] : [%f %f %f %f]", cwb_roi.left,
              cwb_roi.top, cwb_roi.right, cwb_roi.bottom);
        return -1;
      }
      // Convert from hardware space to FSC-transformed space used internally by SDM.
      // top is not multiplied; bottom = top + roi_height * num_fsc_fields.
      uint32_t roi_height = UINT32(cwb_roi.bottom - cwb_roi.top);
      out_roi->left = cwb_roi.left / display_config_.num_fsc_fields;
      out_roi->top = cwb_roi.top;
      out_roi->right = cwb_roi.right / display_config_.num_fsc_fields;
      out_roi->bottom = cwb_roi.top + roi_height * display_config_.num_fsc_fields;
    } else {
      sdm::LayerRect full_rect = {0, 0, static_cast<float>(disp_width),
                                  static_cast<float>(disp_height)};
      if (!Contains(full_rect, cwb_roi)) {
        DLOGE("cwb roi is not within display rect [l t r b] : [%f %f %f %f]", cwb_roi.left,
              cwb_roi.top, cwb_roi.right, cwb_roi.bottom);
        return -1;
      }
      *out_roi = cwb_roi;
    }

    if (!display_config_.fsc_panel && field_index != kAllFSCField) {
      DLOGE("Field dump not supported on non-FSC non-binocular display");
      return -1;
    }
    if (display_config_.fsc_panel && field_index != kAllFSCField) {
      // Field selection is only supported when the ROI covers the full display height.
      uint32_t hw_height = disp_height / display_config_.num_fsc_fields;
      uint32_t roi_height_hw =
          UINT32(out_roi->bottom - out_roi->top) / display_config_.num_fsc_fields;
      if (roi_height_hw != hw_height) {
        DLOGE("Field dump only supported for full-height ROI (roi_height=%u hw_height=%u)",
              roi_height_hw, hw_height);
        return -1;
      }
      uint32_t field_height = hw_height;
      if (field_index == kRedFscField) {
        out_roi->bottom = out_roi->top + field_height;
      } else if (field_index == kGreenFscField) {
        out_roi->top += field_height;
        out_roi->bottom = out_roi->top + field_height;
      } else if (field_index == kBlueFscField) {
        out_roi->top += field_height * 2;
        out_roi->bottom = out_roi->top + field_height;
      }
    }
  }

  DLOGI("Final CWB ROI: [l t r b] : [%f %f %f %f]", out_roi->left, out_roi->top, out_roi->right,
        out_roi->bottom);
  return 0;
}

void FrameCaptureImpl::DestroyOutputBuffers() {
  std::unique_lock<std::mutex> lock(buffer_manage_lock_);
  for (auto it : fd_buffer_base_map_) {
    if (munmap(it.second, output_buffer_.size) != 0) {
      DLOGE("unmap failed with err %d", errno);
    }
  }

  for (auto it : available_out_buffer_) {
    if (buffer_allocator_->FreeBuffer(&it) != 0) {
      DLOGE("FreeBuffer failed");
    }
  }

  for (auto it : dump_out_buffer_) {
    if (buffer_allocator_->FreeBuffer(&it) != 0) {
      DLOGE("FreeBuffer failed");
    }
  }

  for (auto it : currently_used_buffers_) {
    if (buffer_allocator_->FreeBuffer(&it) != 0) {
      DLOGE("FreeBuffer failed");
    }
  }

  for (auto it : lz4_output_buffer_info_) {
    if (buffer_allocator_->FreeBuffer(&it) != 0) {
      DLOGE("FreeBuffer failed for LZ4 o/p buffers");
    }
  }
}

int FrameCaptureImpl::AllocateOutputBuffers(CwbConfig &cwb_config) {
  BufferInfo output_buffer_info = {};

  // Allocate and map output buffer
  const CwbTapPoint &tap_point = cwb_config.tap_point;
  if (display_intf_->GetCwbBufferResolution(&cwb_config, &output_buffer_info.buffer_config.width,
                                            &output_buffer_info.buffer_config.height)) {
    DLOGE("Buffer Resolution setting failed.");
    return -1;
  }
  output_buffer_info.buffer_config.width *= cwb_config.num_parallel_buffers;

  DLOGI("CWB output buffer resolution: width:%d height:%d tap point:%s",
        output_buffer_info.buffer_config.width, output_buffer_info.buffer_config.height,
        UINT32(tap_point) == 0 ? "LM" : "DSPP");

  output_buffer_info.buffer_config.format = kFormatBGR888;
  output_buffer_info.buffer_config.buffer_count = 1;
  for (int i = 0; i < cwb_buffer_pool_size_; i++) {
    if (buffer_allocator_->AllocateBuffer(&output_buffer_info) != 0) {
      DLOGE("Buffer allocation failed");
      output_buffer_info = {};
      return -1;
    }
    available_out_buffer_.push_back(output_buffer_info);

    void *buffer = mmap(NULL, output_buffer_info.alloc_buffer_info.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, output_buffer_info.alloc_buffer_info.fd, 0);

    if (buffer == MAP_FAILED) {
      DLOGE("mmap failed with err %d", errno);
      DestroyOutputBuffers();
      output_buffer_info = {};
      return -1;
    }

    fd_buffer_base_map_.insert({output_buffer_info.alloc_buffer_info.fd, buffer});
    output_buffer_info.alloc_buffer_info.fd = -1;
  }

  uint32_t disp_width = display_config_.fsc_panel
                            ? (display_config_.x_pixels * display_config_.num_fsc_fields)
                            : display_config_.x_pixels;
  uint32_t disp_height = display_config_.fsc_panel
                             ? display_config_.y_pixels / display_config_.num_fsc_fields
                             : display_config_.y_pixels;
  {
    std::lock_guard<std::mutex> lock(metadata_lock_);
    auto &allocated_info = output_buffer_info.alloc_buffer_info;
    metadata_ << "UNCOMPRESSED_SIZE = " << allocated_info.size << std::endl;
    metadata_ << "COMPRESSION = " << enable_compression_ << std::endl;
    metadata_ << std::endl << "# DISPLAY_INFO:" << std::endl;
    metadata_ << "DISPLAY_FPS = " << display_config_.fps << std::endl;
    metadata_ << "DISPLAY_WIDTH = " << disp_width << std::endl;
    metadata_ << "DISPLAY_HEIGHT = " << disp_height << std::endl;
    metadata_ << "BUFFER_WIDTH = " << output_buffer_info.buffer_config.width << std::endl;
    metadata_ << "BUFFER_HEIGHT = " << output_buffer_info.buffer_config.height << std::endl;
    metadata_ << "BUFFER_ALIGNED_WIDTH = " << allocated_info.aligned_width << std::endl;
    metadata_ << "BUFFER_ALIGNED_HEIGHT = " << allocated_info.aligned_height << std::endl;
    metadata_ << "BUFFER_FORMAT = " << GetFormatString(output_buffer_info.buffer_config.format)
              << std::endl;
    metadata_ << "FINAL ROI = " << cwb_config.cwb_roi.left << ", " << cwb_config.cwb_roi.top << ", "
              << cwb_config.cwb_roi.right << ", " << cwb_config.cwb_roi.bottom << std::endl;
    metadata_ << std::endl << "# FRAME_METADATA:" << std::endl;
    metadata_ << "FRAME_FORMAT = FRAME_NO, QTIMER_TIME, FENCE_TIME, WRITE_TIME, COMP_LEVEL, "
              << "COMP_TIME, COMP_SIZE" << std::endl;
  }

  return 0;
}

uint64_t FrameCaptureImpl::GetQtimerTicks() {
  uint64_t ticks = 0;
#if defined(TARGET_ARM64)
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
#else
  uint64_t lsb = 0, msb = 0;
  asm volatile("mrrc p15, 1, %[lsb], %[msb], c14" : [lsb] "=r"(lsb), [msb] "=r"(msb));
  ticks = ((uint64_t)msb << 32) | lsb;
#endif
  return ticks;
}

}  // namespace sdm
