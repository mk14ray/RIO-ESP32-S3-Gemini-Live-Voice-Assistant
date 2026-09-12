#ifndef CAMERA_H
#define CAMERA_H

#include <Arduino.h>

// =============================================================================
// On-demand access to the XIAO ESP32-S3 Sense camera.
//
// The sensor is powered up only for the duration of one capture and torn down
// immediately afterwards, so it is genuinely off whenever RIO has not been
// granted permission to look. That is the whole point: a software flag saying
// "don't look" is not the same guarantee as an uninitialised sensor.
//
// Not thread-safe: only netTask may call these.
//
// -----------------------------------------------------------------------------
// The cost of that privacy guarantee is that the sensor's automatic loops get no
// history: auto-exposure, auto-gain and auto-white-balance start cold on every
// single capture, with one frame's worth of scene to converge on. A capture is
// therefore not a grab — it is a short, measured convergence:
//
//   1. init at the largest frame size the sensor supports (config.h asks for
//      UXGA; the driver's ceiling wins), then drop to VGA for the measuring
//   2. meter frames at 1/8 scale until brightness, colour and stillness all
//      agree on two consecutive frames — AWB settles later than AE, and a
//      moving scene predicts a motion-blurred shutter frame
//   3. bias the exposure toward a target a vision model can actually read; the
//      sensor's own AE aims darker than that indoors
//   4. classify the scene from the luma histogram — high dynamic range, dark,
//      flat — and reshape the sensor's gamma/contrast/gain curve to fit it
//   5. bracket the exposure when the scene is contrasty or dark, scoring
//      candidates on clipped highlights first
//   6. autofocus, if the attached sensor has a lens that can (neither stock
//      sensor does — OV2640 and OV3660 are both fixed-focus — but an OV5640
//      module does, and the driver is asked rather than assumed)
//   7. shoot CAM_SHOT_CANDIDATES full-resolution frames and keep the sharpest,
//      re-shooting at coarser quality if one will not fit
//
// Steps 2-6 are best-effort and bounded by CAM_TUNE_BUDGET_MS; step 7 has its
// own CAM_SHOT_BUDGET_MS. When either clock runs out the pipeline shoots with
// whatever it has: a slightly mis-exposed answer now beats a perfect one after
// RIO has gone quiet.
//
// The only per-frame CPU cost is decoding thumbnails — 1/8 scale for metering,
// 1/4 for sharpness scoring. Every actual image adjustment happens in the
// sensor's own DSP on the way to its hardware JPEG encoder, so netTask is never
// blocked doing arithmetic over a full-resolution image.
// =============================================================================

/**
 * Power up the sensor, capture one JPEG, and shut the sensor back down.
 *
 * Runs the full metering / auto-exposure / auto-HDR / autofocus pipeline
 * described above, then base64-encodes the frame straight into gImgTxBuf as a
 * complete clientContent turn, ready to hand to geminiSendRaw(). It must be sent
 * *after* the tool call that asked for it has been answered — see the note on
 * the message format in cameraCaptureToMessage().
 *
 * Blocks for roughly 1.5-2 s: the tuning phase is capped at CAM_TUNE_BUDGET_MS
 * and the shutter phase at CAM_SHOT_BUDGET_MS.
 *
 * @param outLen receives the message length on success.
 * @return false if init, capture, or encoding failed (nothing was sent).
 */
bool cameraCaptureToMessage(size_t* outLen);

#endif  // CAMERA_H
