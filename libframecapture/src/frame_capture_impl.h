/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __FRAME_CAPTURE_IMPL_H__
#define __FRAME_CAPTURE_IMPL_H__

#include <debug_handler.h>
#include <core/layer_stack.h>
#include <core/display_interface.h>
#include <utils/SystemClock.h>

#include <vector>
#include <mutex>
#include <future>
#include <map>
#include <list>
#include <condition_variable>
#include <sstream>
#include <fstream>

#include "frame_capture_intf.h"
#include "Compressor.h"

/* This is platform specific code, so make sure that we are compiling
   for correct architecture */
#if defined(__aarch64__)
#define TARGET_ARM64
#elif defined(__arm__)
#define TARGET_ARM
#endif

namespace sdm {
enum CWBStatus {
  kCWBAvailable,     // Available to accept new CWB request
  kCWBConfigure,     // CWB is Configured in the current frame
  kCWBTeardown,      // CWB tear down in the current frame. Frame's Retire fence would be cached.
                     // New CWB requests coming in are rejected until this retire fence signals.
  kCWBPostTeardown,  // CWB teardown done in previous frame.
};

struct CwbState {
  CWBClient cwb_client = kCWBClientNone;                    // the client actively performing cwb.
  CWBStatus cwb_status = CWBStatus::kCWBAvailable;          // current cwb statuss
  shared_ptr<Fence> teardown_frame_retire_fence = nullptr;  // cache cwb disable frame retire fence
                                                            // to reject requests until it signals.
};

struct Metadata {
  uint64_t qtimer_ticks = 0;
  uint64_t fence_time = 0;
};

class FrameCaptureImpl : public FrameCaptureIntf, public CompressorCallback {
 public:
  FrameCaptureImpl(DisplayInterface *display_intf, BufferAllocator *buffer_allocator);
  int Init();
  int DeInit();
  void NotifyCwbDone(int32_t status, const LayerBuffer &buffer);
  int ConfigureFCM(CWBPacketData &data);
  void FillMetadata(struct CompStats &stats);

 private:
  int UpdateCWBRoi(sdm::LayerRect cwb_roi, int32_t eye_index, int32_t field_index,
                   sdm::LayerRect *out_roi);
  int AllocateOutputBuffers(CwbConfig &cwb_config);
  void DestroyOutputBuffers();
  int SetupCWBFile(BufferInfo buffer_info);
  int TeardownConcurrentWriteback();
  void DumpOutputBufferToFile();
  void DumpOutputBuffer(const BufferInfo &buffer_info, void *base);
  void SubmitCWBConfig();
  int SetReadbackBuffer(const BufferInfo &info, shared_ptr<Fence> acquire_fence);
  uint64_t GetQtimerTicks();

  DisplayInterface *display_intf_ = nullptr;
  BufferAllocator *buffer_allocator_ = nullptr;
  DisplayConfigVariableInfo display_config_ = {};
  CwbConfig cwb_config_ = {};
  CwbState cwb_state_;
  std::mutex cwb_state_lock_;  // cwb state lock. Set before accesing or updating cwb_state_
  uint32_t dump_frame_count_ = 0;
  uint32_t dump_frame_index_ = 0;

  // buffer management
  static const int kCWBBufferDepth = 20;
  std::mutex buffer_manage_lock_;
  std::list<BufferInfo> available_out_buffer_ = {};
  std::list<BufferInfo> dump_out_buffer_ = {};
  std::list<BufferInfo> currently_used_buffers_ = {};
  std::map<int, void *> fd_buffer_base_map_;
  std::map<int, Metadata> metadata_map_;
  LayerBuffer output_buffer_ = {};
  int32_t pending_notify_ = 0;

  std::ofstream metadata_file_;
  std::ostringstream metadata_;
  std::mutex metadata_lock_;
  FILE *cwb_fp_ = nullptr;
  std::future<void> cwb_dump_future_;

  std::future<void> cwb_trigger_future_;
  std::condition_variable buf_cv_;
  bool trigger_cwb_ = false;
  uint32_t notify_count_ = 0;
  int InitCompressionCtx();
  bool enable_compression_ = true;
  Compressor *compressor_ = nullptr;
  std::vector<BufferInfo> lz4_input_buffer_info_;
  std::vector<BufferInfo> lz4_output_buffer_info_;
  uint32_t dumped_frames_ = 0;
  uint32_t requested_frames_ = 0;
  std::vector<QueueItem> buffer_pool_;
  float fwrite_time_ = 0;
};

}  // namespace sdm
#endif  // __FRAME_CAPTURE_IMPL_H__
