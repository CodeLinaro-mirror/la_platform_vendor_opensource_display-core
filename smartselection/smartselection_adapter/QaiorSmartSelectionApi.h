/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ================================================================
 *  Smart Selection C API
 *  ABI-stable interface for SmartSelectionPipeline
 * ================================================================
 *
 *  This header exposes a pure C interface that wraps the internal
 *  C++ SmartSelectionPipeline implementation. It is safe to use
 *  across toolchains (NDK vs. platform) and via JNI.
 *
 *  All types are POD and ABI-stable.
 *  All objects are opaque handles.
 *
 * ================================================================
 */

#include <stdint.h>

/* ---------------------------------------------------------------
 *  Status Codes
 * --------------------------------------------------------------- */
/**
 * @brief Status returned by all pipeline operations.
 *
 * QAIOR_SS_STATUS_OK:
 *      Operation succeeded.
 *
 * QAIOR_SS_STATUS_ERROR:
 *      Generic failure (invalid arguments, parsing error, no matching
 *      queues, internal failure, etc.).
 *
 * Additional error codes may be added in future versions.
 */
typedef enum {
    QAIOR_SS_STATUS_OK = 0,
    QAIOR_SS_STATUS_ERROR = 1,
    QAIOR_SS_STATUS_ERROR_INVALID_ARGUMENT = 2,
    QAIOR_SS_STATUS_ERROR_TIMEOUT = 3
} QaiorSS_Status_t;

/* ---------------------------------------------------------------
 *  Opaque Handles
 * --------------------------------------------------------------- */

/**
 * @brief Opaque handle representing a Smart Selection Pipeline instance.
 *
 * This corresponds to the C++ SmartSelectionPipeline class.
 *
 * Lifetime:
 *   - Create with QaiorSS_createPipeline()
 *   - Destroy with QaiorSS_destroyPipeline()
 */
typedef struct QaiorSS_Pipeline QaiorSS_Pipeline_t;

/**
 * @brief Opaque representing a single Smart Selection pipeline instance.
 *
 *
 * Lifetime rules:
 *   - Created internally by QaiorSS_init()
 *   - Destroyed only by QaiorSS_deinit()
 *
 * The caller must treat this as an opaque handle and must NOT access
 * or assume anything about its internal layout.
 */
typedef struct QaiorSS_Context QaiorSS_Context_t;

/* ---------------------------------------------------------------
 * FrameHandle
 * --------------------------------------------------------------- */

/**
 * @brief Frame type enumeration (C ABI-safe).
 */
typedef enum {
    QAIOR_SS_FRAME_AHARDWAREBUFFER = 0,  ///< Backed by AHardwareBuffer (NDK)
    QAIOR_SS_FRAME_GRAPHICBUFFER = 1,    ///< Backed by android::GraphicBuffer (platform)
    QAIOR_SS_FRAME_PARCELFD = 2,         ///< Backed by a file descriptor
    QAIOR_SS_FRAME_OTHER = 3             ///< OEM/custom frame type
} QaiorSS_FrameType_t;

/**
 * @brief C representation of a captured frame.
 *
 *
 * Ownership rules:
 *   - For ParcelFd:
 *       * The pipeline duplicates (dup) the FD internally.
 *       * Caller retains ownership of the original FD.
 *       * When returned from DeleteByConfig/DeleteAll, caller MUST close().
 *
 *   - For GraphicBuffer/AHardwareBuffer:
 *       * Pointer is passed through as void*.
 *       * Reference counting or ownership is handled internally by the pipeline.
 *
 *   - For OtherHandle:
 *       * OEM-defined ownership semantics.
 */
typedef struct {
    QaiorSS_FrameType_t type;  ///< Underlying representation type

    /**
     * @brief Pointer to the underlying buffer.
     *
     * - AHardwareBuffer* when type == QAIOR_SS_FRAME_AHARDWAREBUFFER
     * - android::GraphicBuffer* when type == QAIOR_SS_FRAME_GRAPHICBUFFER
     * - NULL otherwise
     */
    void* buffer;

    /**
     * @brief File descriptor (valid only when type == QAIOR_SS_FRAME_PARCELFD).
     *
     * Caller must close() this FD when frames are returned from
     * QaiorSS_deleteByConfig/QaiorSS_deleteAll.
     */
    int parcelFd;

    /**
     * @brief OEM-defined handle (valid only when type == QAIOR_SS_FRAME_OTHER).
     */
    void* otherHandle;

    /**
     * @brief JSON metadata describing the frame.
     *
     * Includes fields such as:
     *   - appName
     *   - userId
     *   - timestamp
     *   - width/height/format
     *
     * The string is owned by the pipeline and must remain valid
     * for the duration of the callback.
     */
    const char* metadataJson;
} QaiorSS_FrameHandle_t;

/* ---------------------------------------------------------------
 *  EmitResult
 * --------------------------------------------------------------- */
/**
 * @brief C representation of EmitResult.
 *
 * Delivered asynchronously via the callback registered in ss_init().
 * Contains arrays of selected and rejected frames.
 *
 * Memory ownership:
 *   - The arrays (selectedFrames, rejectedFrames) are allocated by the pipeline.
 *   - The callback MUST NOT free them.
 *   - The pipeline frees them after the callback returns.
 */
typedef struct {
    QaiorSS_FrameHandle_t* selectedFrames;  ///< Array of selected frames
    int32_t selectedCount;                  ///< Number of selected frames

    QaiorSS_FrameHandle_t* rejectedFrames;  ///< Array of rejected frames
    int32_t rejectedCount;                  ///< Number of rejected frames
} QaiorSS_EmitResult_t;

/**
 * @brief Callback invoked when frames are emitted.
 *
 * @param result Pointer to emitted result (valid only during callback).
 * @param cookie Opaque pointer passed through from QaiorSS_init().
 */
typedef void (*QaiorSS_emitCallback)(const QaiorSS_EmitResult_t* result, void* cookie);

/* ---------------------------------------------------------------
 *  Pipeline Lifecycle
 * --------------------------------------------------------------- */
/**
 * @brief Create a new Smart Selection Pipeline instance.
 *
 * @return A non-null pipeline handle on success, NULL on failure.
 */
QaiorSS_Pipeline_t* QaiorSS_createPipeline(void);

/**
 * @brief Destroy a Smart Selection Pipeline instance.
 *
 * @param pipeline The pipeline handle returned by QaiorSS_createPipeline().
 */
void QaiorSS_destroyPipeline(QaiorSS_Pipeline_t* pipeline);

/* ---------------------------------------------------------------
 *  Context Lifecycle
 * --------------------------------------------------------------- */
/**
 * @brief Initialize a new Smart Selection Pipeline context.
 *
 * Equivalent to C++ SmartSelectionPipeline::Init().
 *
 * @param pipeline     The pipeline handle.
 * @param jsonConfig   CaptureConfig JSON string.
 * @param callback     Callback invoked when frames are emitted.
 * @param cookie       Opaque pointer returned unchanged to callback.
 *
 * @return A non-null SS_Context* on success, NULL on failure.
 */
QaiorSS_Context_t* QaiorSS_init(QaiorSS_Pipeline_t* pipeline, const char* jsonConfig,
                                QaiorSS_emitCallback callback, void* cookie);

/**
 * @brief Deinitialize the context and release all resources.
 *
 * @param pipeline   The pipeline handle.
 * @param ctx        The context to destroy.
 */
void QaiorSS_deinit(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx);

/* ---------------------------------------------------------------
 *  Pipeline Operations
 * --------------------------------------------------------------- */
/**
 * @brief Apply a new configuration to an existing context.
 *
 * @param pipeline     The pipeline handle.
 * @param ctx          The active context.
 * @param jsonConfig   New CaptureConfig JSON.
 *
 * @return QAIOR_SS_STATUS_OK on success, QAIOR_SS_STATUS_ERROR on failure.
 */
QaiorSS_Status_t QaiorSS_reconfigure(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx,
                                     const char* jsonConfig);

/**
 * @brief Submit a frame for deduplication and potential selection.
 *
 * @param pipeline   The pipeline handle.
 * @param ctx        Active context.
 * @param frame      FrameHandle (C version).
 * @param cookie     Opaque pointer returned in callback.
 *
 * @return QAIOR_SS_STATUS_OK if accepted, QAIOR_SS_STATUS_ERROR otherwise.
 */
QaiorSS_Status_t QaiorSS_enqueue(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx,
                                 const QaiorSS_FrameHandle_t* frame, void* cookie);

/**
 * @brief Emit all frames matching the given JSON filter.
 *
 * @param pipeline        The pipeline handle.
 * @param ctx             Active context.
 * @param flushConfigJson JSON filter.
 *
 * @return QAIOR_SS_STATUS_OK on success, QAIOR_SS_STATUS_ERROR on failure.
 */
QaiorSS_Status_t QaiorSS_flushByConfig(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx,
                                       const char* flushConfigJson);

/**
 * @brief Emit all frames across all internal queues.
 *
 * @param pipeline   The pipeline handle.
 * @param ctx        Active context.
 *
 * @return QAIOR_SS_STATUS_OK on success, QAIOR_SS_STATUS_ERROR on failure.
 */
QaiorSS_Status_t QaiorSS_flushAll(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx);

/* ---------------------------------------------------------------
 *  Deletion Operations
 * --------------------------------------------------------------- */
/**
 * @brief Delete frames matching the given JSON filter.
 *
 * @param pipeline        The pipeline handle.
 * @param ctx             Active context.
 * @param deleteJson      JSON filter.
 * @param outFrames       Output array of deleted frames (allocated by library).
 * @param outCount        Number of deleted frames.
 *
 * Caller must free outFrames using ss_free_frames().
 *
 * @return QAIOR_SS_STATUS_OK on success, QAIOR_SS_STATUS_ERROR on failure.
 */
QaiorSS_Status_t QaiorSS_deleteByConfig(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx,
                                        const char* deleteJson, QaiorSS_FrameHandle_t** outFrames,
                                        int32_t* outCount);

/**
 * @brief Delete all frames across all internal queues.
 *
 * @param pipeline    The pipeline handle.
 * @param ctx         Active context.
 * @param outFrames   Output array of deleted frames.
 * @param outCount    Number of deleted frames.
 *
 * Caller must free outFrames using ss_free_frames().
 *
 * @return QAIOR_SS_STATUS_OK on success, QAIOR_SS_STATUS_ERROR on failure.
 */
QaiorSS_Status_t QaiorSS_deleteAll(QaiorSS_Pipeline_t* pipeline, QaiorSS_Context_t* ctx,
                                   QaiorSS_FrameHandle_t** outFrames, int32_t* outCount);

/**
 * @brief Free an array of SS_FrameHandle returned by delete operations.
 *
 */
void QaiorSS_freeFrames(QaiorSS_FrameHandle_t* frames);

/**
 * @brief Blocks until the SmartSelection pipeline becomes idle.
 *
 * This function waits until all pending tasks in the SmartSelection pipeline
 * have completed and no worker threads are actively processing frames.
 * It is primarily intended for testing and diagnostic scenarios where the
 * caller needs deterministic synchronization with the internal pipeline state.
 *
 * @param p            Pointer to an initialized SS_Pipeline instance.
 * @param ctx          Pointer to an initialized SS_Context associated with @p p.
 * @param timeout_ms   Maximum time to wait, in milliseconds. Must be >= 0.
 *
 * @return QAIOR_SS_STATUS_OK on success (pipeline became idle within the timeout),
 *         QAIOR_SS_STATUS_ERROR_INVALID_ARGUMENT if @p p or @p ctx is null or
 *         @p timeout_ms is negative,
 *         QAIOR_SS_STATUS_ERROR_TIMEOUT if the pipeline did not become idle before
 *         the timeout expired,
 *         QAIOR_SS_STATUS_ERROR for any other internal failure.
 *
 * @note This function is not part of the production runtime API. It is exposed
 *       only for test harnesses and controlled environments that require
 *       deterministic pipeline flushing behavior.
 */
QaiorSS_Status_t QaiorSS_waitUntilIdle(QaiorSS_Pipeline_t* p, QaiorSS_Context_t* ctx,
                                       int32_t timeout_ms);

#ifdef __cplusplus
}
#endif
