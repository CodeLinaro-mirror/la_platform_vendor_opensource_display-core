ImageAlgo Integration
=====================

Vendor-side plugin framework that connects the display pipeline to image algorithm
libraries (SmartSelection, future...).


Directory Layout
----------------

  smartselection/
  |-- include/
  |   |-- image_algo_interface.h              Public API: factory + AdapterTraits
  |   `-- image_algo_smartselection_intf.h    SmartSelection typed interface + payloads
  |-- image_algo_adapter_factory.cpp          PluginAdapterFactory (dlopen/dlsym/cache)
  |-- common/
  |   `-- image_algo_common.cpp               InitSnapMapper() shared helper
  |-- smartselection_adapter/
  |   |-- smartselection_adapter.h/.cpp       SmartSelectionAdapter implementation
  |   `-- QaiorSmartSelectionApi.h            C API declarations for libQaiorSmartSelection
  |-- config/
  |   |-- cpu/
  |   |   `-- smartselection_config_cpu.json  CPU (NCNN) backend config
  |   `-- npu/
  |       |-- smartselection_config_npu.json  NPU (QNN HTP) backend config
  |       |-- heat_config_vendor.json         QNN config for heatmap+featmap model
  |       `-- keypts_config_vendor.json       QNN config for keypoints model
  `-- Android.bp


Build Targets
-------------

  libImageAlgoIntegration
      Type: cc_library_shared
      Description: Factory entry point; exports GetImageAlgoAdapterFactory()

  libImageAlgoAdapter_smartselection
      Type: cc_library_shared
      Description: SmartSelection adapter plugin; loaded at runtime via dlopen

  libQaiorSmartSelection
      Type: cc_prebuilt_library_shared
      Description: SmartSelection algorithm library (prebuilt)


Android.bp dependency for clients:

  shared_libs: [
      "libImageAlgoIntegration",
  ],


Interface Overview
------------------

GetImageAlgoAdapterFactory()

  Returns the process-lifetime singleton ImageAlgoAdapterFactory*. Do NOT delete it.

    #include "image_algo_interface.h"
    imagealgo::ImageAlgoAdapterFactory* factory = GetImageAlgoAdapterFactory();


ImageAlgoAdapterFactory::CreateAdapter<IntfType>()

  dlopen's the plugin .so (path from AdapterTraits<IntfType>::kLibPath), calls
  CreateImageAlgoAdapter(), and returns a shared_ptr with a custom deleter
  (DestroyImageAlgoAdapter + dlclose). Subsequent calls return the same cached instance.

    std::shared_ptr<imagealgo::SmartSelectionIntf> adapter =
        factory->CreateAdapter<imagealgo::SmartSelectionIntf>();


SmartSelectionIntf (from GenericIntf)

    int Init();
        // load .so, resolve symbols, load config

    int Deinit();
        // unload .so, release resources

    int ProcessOps(SmartSelectionOp op,
                   const sdm::GenericPayload& input,
                   sdm::GenericPayload* output);


SmartSelection Operations
-------------------------

  kSSInit          Input: SmartSelectionInitInput*    Create pipeline + context; register emit callback
  kSSReconfigure   Input: std::string* (JSON)         Update pipeline config without reinit
  kSSEnqueue       Input: SmartSelectionEnqueueInput* Submit one CWB frame for processing
  kSSFlushAll      Input: (none)                      Emit final selected/rejected state via callback
  kSSFlushByConfig Input: std::string* (JSON)         Flush frames matching a specific app config
  kSSDeleteAll     Input: (none)                      Discard all buffered frames without emitting
  kSSDeleteByConfig Input: std::string* (JSON)        Discard frames matching a specific app config
  kSSDeinit        Input: (none)                      Destroy pipeline/context (adapter stays loaded)
  kSSWaitUntilIdle Input: SmartSelectionWaitInput*    Block until pipeline is idle (timeout_ms, default 5000)


Usage Example
-------------

Reference implementation: display-tests cwb-test CL#6723897
  (cwb_test_display.cpp / cwb_test_display.h)


ProcessOps Usage -- All Operations
-----------------------------------

1. kSSInit -- Create pipeline and register emit callback

    #include "image_algo_interface.h"
    #include "image_algo_smartselection_intf.h"

    // Get factory (process-lifetime singleton)
    imagealgo::ImageAlgoAdapterFactory* factory = GetImageAlgoAdapterFactory();

    // Create adapter (dlopen libImageAlgoAdapter_smartselection.so)
    auto adapter = factory->CreateAdapter<imagealgo::SmartSelectionIntf>();

    // Load library + resolve symbols + load /vendor/etc/smartselection_config.json
    adapter->Init();

    // Build input payload
    sdm::GenericPayload init_payload;
    imagealgo::SmartSelectionInitInput* init_input = nullptr;
    init_payload.CreatePayload(init_input);

    // Register emit callback -- called asynchronously when frames are selected/rejected.
    //
    // IMPORTANT: The adapter guarantees that fr.type in the emit result matches the
    // input_type used at enqueue time:
    //   kSSFrameNativeHandle    → fr.type == kSSFrameNativeHandle,
    //                             fr.buffer == native_handle_t* (original pointer; do NOT free)
    //   kSSFrameAHardwareBuffer → fr.type == kSSFrameAHardwareBuffer,
    //                             fr.buffer == AHardwareBuffer* (caller MUST AHardwareBuffer_release)
    //   kSSFrameParcelFd        → fr.type == kSSFrameParcelFd,
    //                             fr.parcel_fd == dup'd fd (caller MUST close)
    //   kSSFrameGraphicBuffer   → fr.type == kSSFrameGraphicBuffer,
    //                             fr.buffer == GraphicBuffer* (caller manages lifetime)
    init_input->callback = [](const imagealgo::SmartSelectionEmitResult* result, void* cookie) {
        for (const auto& fr : result->selected_frames) {
            // fr.metadata_json -- per-frame JSON built by adapter (see metadata_json format below)
            DLOGI("SELECTED: type=%d meta=%s", fr.type, fr.metadata_json.c_str());

            switch (fr.type) {
                case imagealgo::kSSFrameNativeHandle:
                    // fr.buffer is the original native_handle_t* passed at enqueue.
                    // Adapter retains ownership -- do NOT close/delete.
                    // Use it to identify which CWB buffer was selected.
                    break;
                case imagealgo::kSSFrameAHardwareBuffer:
                    if (fr.buffer) AHardwareBuffer_release(static_cast<AHardwareBuffer*>(fr.buffer));
                    break;
                case imagealgo::kSSFrameParcelFd:
                    if (fr.parcel_fd >= 0) close(fr.parcel_fd);
                    break;
                default:
                    break;
            }
        }
        for (const auto& fr : result->rejected_frames) {
            DLOGI("REJECTED: type=%d meta=%s", fr.type, fr.metadata_json.c_str());
            // Same ownership rules as selected_frames above.
            if (fr.type == imagealgo::kSSFrameParcelFd && fr.parcel_fd >= 0) close(fr.parcel_fd);
            if (fr.type == imagealgo::kSSFrameAHardwareBuffer && fr.buffer)
                AHardwareBuffer_release(static_cast<AHardwareBuffer*>(fr.buffer));
        }
    };
    init_input->cookie = this;

    // Optional: override the config loaded from /vendor/etc/smartselection_config.json
    // init_input->config_json = "<json string>";

    int ret = adapter->ProcessOps(imagealgo::kSSInit, init_payload, nullptr);
    // ret: 0 on success, -EINVAL if config missing, -ENOMEM if pipeline alloc fails


2. kSSReconfigure -- Update pipeline config without reinitializing

    std::string new_config = R"({"activeuserId":"display_service_user","smartSelection":[...]})";

    sdm::GenericPayload reconf_payload;
    std::string* reconf_input = nullptr;
    reconf_payload.CreatePayload(reconf_input);
    *reconf_input = new_config;

    int ret = adapter->ProcessOps(imagealgo::kSSReconfigure, reconf_payload, nullptr);
    // ret: 0 on success, -EINVAL on failure


3. kSSEnqueue -- Submit a CWB frame for processing

  SmartSelectionEnqueueInput directly mirrors QaiorSS_FrameHandle_t:
  the caller provides metadata_json as a pre-built JSON string, which the adapter
  passes as-is to QaiorSS_enqueue() as metadataJson.

  Results are delivered asynchronously via the emit callback registered in kSSInit.
  output is always nullptr for kSSEnqueue.

  kSSFrameNativeHandle (default) -- native_handle_t* from CWB/BufferAllocator
      Adapter converts to AHardwareBuffer via AHardwareBuffer_createFromHandle(CLONE).
      Emit callback returns the original native_handle_t* unchanged (adapter releases AHB clone).
      For AHardwareBuffer_Desc: adapter uses gralloc metadata (ISnapMapper) as primary source;
      falls back to parsing width/height/stride/format from metadata_json if gralloc fails.

    sdm::GenericPayload enq_payload;
    imagealgo::SmartSelectionEnqueueInput* enq_input = nullptr;
    enq_payload.CreatePayload(enq_input);

    // input_type defaults to kSSFrameNativeHandle -- no need to set explicitly
    enq_input->buffer = const_cast<native_handle_t*>(buffer);
    // Build metadata_json from BufferInfo (same fields as QaiorSS_FrameHandle_t.metadataJson)
    enq_input->metadata_json =
        std::string("{\"appName\":\"com.test.cwb.imagealgo\",")
        + "\"width\":"         + std::to_string(buffer_info.buffer_config.width)              + ","
        + "\"height\":"        + std::to_string(buffer_info.buffer_config.height)             + ","
        + "\"stride\":"        + std::to_string(buffer_info.alloc_buffer_info.aligned_width)  + ","
        + "\"alignedHeight\":" + std::to_string(buffer_info.alloc_buffer_info.aligned_height) + ","
        + "\"format\":\"RGBA8888_UBWC\"}";

    int ret = adapter->ProcessOps(imagealgo::kSSEnqueue, enq_payload, nullptr);
    // ret: 0 on success, -EINVAL on failure

  kSSFrameAHardwareBuffer -- AHardwareBuffer* (adapter acquires ref; released in emit callback)

    enq_input->input_type    = imagealgo::kSSFrameAHardwareBuffer;
    enq_input->buffer        = ahb;           // AHardwareBuffer*
    enq_input->metadata_json = "{\"appName\":\"com.test.cwb.imagealgo\","
                               "\"width\":1080,\"height\":2400,"
                               "\"stride\":1088,\"alignedHeight\":2432,"
                               "\"format\":\"RGBA8888_UBWC\"}";

  kSSFrameGraphicBuffer -- android::GraphicBuffer* (caller must keep alive until emit callback)

    enq_input->input_type    = imagealgo::kSSFrameGraphicBuffer;
    enq_input->buffer        = graphic_buffer.get();
    enq_input->metadata_json = "...";

  kSSFrameParcelFd -- file descriptor (library dups internally; caller retains original fd)

    enq_input->input_type    = imagealgo::kSSFrameParcelFd;
    enq_input->parcel_fd     = fd;
    enq_input->metadata_json = "...";


  metadata_json format
  --------------------

  JSON string passed directly to QaiorSS_enqueue() as metadataJson, and deep-copied
  into SmartSelectionFrame::metadata_json in the emit callback:

    {
      "appName":       "<string>",  // must match config appName
      "width":         <uint32>,    // true image width  (buffer_config.width)
      "height":        <uint32>,    // true image height (buffer_config.height)
      "stride":        <uint32>,    // aligned row width in pixels (aligned_width)
      "alignedHeight": <uint32>,    // aligned height in pixels (aligned_height)
      "format":        "<string>"   // pixel format (see values below)
    }

  format values:
    "RGBA8888"          -- kFormatRGBA8888, non-UBWC
    "RGBA8888_UBWC"     -- kFormatRGBA8888, UBWC compressed
    "RGBA1010102"       -- kFormatRGBA1010102, non-UBWC
    "RGBA1010102_UBWC"  -- kFormatRGBA1010102, UBWC compressed
    "RGB888"            -- kFormatRGB888

  Example (1080p RGBA8888 UBWC CWB buffer):
    {
      "appName":       "com.test.cwb.imagealgo",
      "width":         1080,
      "height":        2400,
      "stride":        1088,
      "alignedHeight": 2432,
      "format":        "RGBA8888_UBWC"
    }

  Parsing in emit callback (no JSON library needed for simple field extraction):
    const std::string& meta = fr.metadata_json;
    // meta == "{\"appName\":\"com.test.cwb.imagealgo\",\"width\":1080,...}"
    // Use std::string::find() + substr(), or a lightweight JSON parser.
    // The format field identifies the pixel format of the selected buffer.


4. kSSFlushAll -- Emit all buffered frames via callback

  Triggers the emit callback with all frames across all internal queues.
  No input payload needed.

    sdm::GenericPayload flush_payload;
    int ret = adapter->ProcessOps(imagealgo::kSSFlushAll, flush_payload, nullptr);
    // ret: 0 on success, -EINVAL if pipeline not initialized


5. kSSFlushByConfig -- Emit frames matching a specific app config

  Triggers the emit callback only for frames matching the given JSON filter.

    std::string flush_filter = R"({"appName":"com.example.myapp"})";

    sdm::GenericPayload flush_payload;
    std::string* flush_input = nullptr;
    flush_payload.CreatePayload(flush_input);
    *flush_input = flush_filter;

    int ret = adapter->ProcessOps(imagealgo::kSSFlushByConfig, flush_payload, nullptr);
    // ret: 0 on success, -EINVAL on failure


6. kSSDeleteAll -- Discard all buffered frames without emitting

  Deleted frames are consumed internally (fds closed). No emit callback is fired.
  No input payload needed.

    sdm::GenericPayload del_payload;
    int ret = adapter->ProcessOps(imagealgo::kSSDeleteAll, del_payload, nullptr);
    // ret: 0 on success, -EINVAL if pipeline not initialized


7. kSSDeleteByConfig -- Discard frames matching a specific app config

  Deleted frames are consumed internally (fds closed). No emit callback is fired.

    std::string delete_filter = R"({"appName":"com.example.myapp"})";

    sdm::GenericPayload del_payload;
    std::string* del_input = nullptr;
    del_payload.CreatePayload(del_input);
    *del_input = delete_filter;

    int ret = adapter->ProcessOps(imagealgo::kSSDeleteByConfig, del_payload, nullptr);
    // ret: 0 on success, -EINVAL on failure


8. kSSWaitUntilIdle -- Block until pipeline is idle

  Waits until all enqueued frames have been processed by the pipeline worker threads.
  Primarily used before kSSFlushAll to ensure deterministic teardown.

    sdm::GenericPayload wait_payload;
    imagealgo::SmartSelectionWaitInput* wait_input = nullptr;
    wait_payload.CreatePayload(wait_input);
    wait_input->timeout_ms = 5000;   // milliseconds; default is 5000

    int ret = adapter->ProcessOps(imagealgo::kSSWaitUntilIdle, wait_payload, nullptr);
    // ret:  0          -- pipeline became idle within timeout
    //       -ETIMEDOUT -- timeout expired before pipeline became idle
    //       -EINVAL    -- pipeline not initialized or timeout_ms < 0


9. kSSDeinit -- Destroy pipeline/context (adapter stays loaded)

  Tears down the pipeline and context. The adapter library remains loaded so
  kSSInit can be called again without reloading the .so.
  No input payload needed.

    sdm::GenericPayload deinit_payload;
    int ret = adapter->ProcessOps(imagealgo::kSSDeinit, deinit_payload, nullptr);
    // ret: 0 on success, -EINVAL if pipeline not initialized

    // To fully unload the library, reset the shared_ptr:

Configuration File
------------------

Deployed to /vendor/etc/smartselection_config.json.
Loaded automatically by adapter->Init().
Can be overridden per-session via SmartSelectionInitInput::config_json.

Two variants are provided under config/:

  CPU variant: config/cpu/smartselection_config_cpu.json   (NCNN CPU inference)
  NPU variant: config/npu/smartselection_config_npu.json   (QNN HTP / Hexagon inference)


CPU Config (config/cpu/smartselection_config_cpu.json):

  {
    "activeuserId": "display_service_user",
    "smartSelection": [
      {
        "appName": "com.test.cwb.imagealgo",
        "selector": {
          "accept_threshold": 0.05,
          "remove_threshold": 0.05,
          "max_size": 128
        },
        "extractor": {
          "top_k": 4096,
          "height": 640,
          "detection_threshold": 0.05,
          "is_path": true,
          "keypoint_param_path": "/vendor/etc/smartselection/models/SSModel_kpts.SSModelRuntime.param",
          "keypoint_bin_path":   "/vendor/etc/smartselection/models/SSModel_kpts.SSModelRuntime.bin",
          "features_param_path": "/vendor/etc/smartselection/models/SSModel_mean_norm.SSModelRuntime.param",
          "features_bin_path":   "/vendor/etc/smartselection/models/SSModel_mean_norm.SSModelRuntime.bin"
        },
        "matcher": {
          "min_cossim": 0.82
        }
      }
    ]
  }


NPU Config (config/npu/smartselection_config_npu.json):

  Uses QNN HTP backend. The featheat_qnn_config and kpts_qnn_config fields point to
  separate QNN config files.

  {
    "activeuserId": "display_service_user",
    "smartSelection": [
      {
        "appName": "com.test.cwb.imagealgo",
        "selector": {
          "accept_threshold": 0.05,
          "remove_threshold": 0.05,
          "max_size": 128
        },
        "extractor": {
          "top_k": 4096,
          "height": 640,
          "detection_threshold": 0.05,
          "is_path": true,
          "device": "npu",
          "featheat_qnn_config": "/vendor/etc/smartselection/models/npu/heat_config.json",
          "kpts_qnn_config": "/vendor/etc/smartselection/models/npu/keypts_config.json"
        },
        "matcher": {
          "min_cossim": 0.82
        }
      }
    ]
  }


QNN Model Configs (NPU only)

  config/npu/heat_config_vendor.json -- heatmap + feature map model:

    {
        "backend_lib_path": "/vendor/lib64/libQnnHtp.so",
        "model_path": "/vendor/etc/smartselection/models/npu/heatmap_featmap_640x288_w8a16_2.43.1_v81_87.serialized.bin",
        "performance_policy": "burst",
        "inputs": [{ "name": "image", "data_type": "FLOAT32", "data_layout": "NCHW", "shape": [1, 1, 640, 288] }]
    }

  config/npu/keypts_config_vendor.json -- keypoints model:

    {
        "backend_lib_path": "/vendor/lib64/libQnnHtp.so",
        "model_path": "/vendor/etc/smartselection/models/npu/kpts_640x288_w8a16_2.43.1_v81_87.serialized.bin",
        "performance_policy": "burst",
        "inputs": [{ "name": "image", "data_type": "FLOAT32", "data_layout": "NCHW", "shape": [1, 64, 80, 36] }]
    }


Config Field Reference

  activeuserId              User context identifier passed to the pipeline
  appName                   Package name to match against SmartSelectionEnqueueInput::app_name
  selector.accept_threshold Similarity score above which a frame is selected
  selector.remove_threshold Similarity score below which a frame is rejected
  selector.max_size         Maximum number of frames buffered in the pipeline
  extractor.top_k           Maximum keypoints extracted per frame
  extractor.height          Input image height for the model (frames are resized)
  extractor.detection_threshold  Keypoint detection confidence threshold
  extractor.device          "npu" to use QNN HTP backend; omit for CPU (NCNN)
  extractor.*_path (CPU)    Paths to NCNN .param/.bin model files
  extractor.*_qnn_config (NPU)  Paths to QNN config JSON files on the vendor partition
  matcher.min_cossim        Minimum cosine similarity for feature matching


Model Files on Device

  CPU:
    /vendor/etc/smartselection/models/
    |-- SSModel_kpts.SSModelRuntime.param
    |-- SSModel_kpts.SSModelRuntime.bin
    |-- SSModel_mean_norm.SSModelRuntime.param
    `-- SSModel_mean_norm.SSModelRuntime.bin

  NPU:
    /vendor/etc/smartselection/models/npu/
    |-- heat_config.json
    |-- keypts_config.json
    |-- heatmap_featmap_640x288_w8a16_2.43.1_v81_87.serialized.bin
    `-- kpts_640x288_w8a16_2.43.1_v81_87.serialized.bin


Runtime Library Loading
-----------------------

  libImageAlgoIntegration.so          (linked at build time by client)
    `-- dlopen --> libImageAlgoAdapter_smartselection.so   (loaded on first CreateAdapter call)
                     `-- dlopen --> libQaiorSmartSelection.so  (loaded on adapter->Init())

  libQaiorSmartSelection.so is intentionally NOT listed in shared_libs -- it is loaded at
  runtime by SmartSelectionAdapter::LoadLibrary() via
  dlopen("libQaiorSmartSelection.so", RTLD_NOW | RTLD_LOCAL).
