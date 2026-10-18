// doa4 - on-demand capture of the game image (diagnostics)
//
// If the DOA4_CAPTURE_DIR environment variable is set, a thread checks every
// half second whether the file "capture.req" exists in that directory (it may
// contain the number of frames to capture in a burst). When it does, the last
// frame produced by the game is read from the emulated GPU
// (Presenter::CaptureGuestOutput), saved as capture_N.raw, and the request is
// deleted. tools/raw2png.py converts the .raw files to PNG.

#pragma once

namespace rex {
class Runtime;
}

namespace doa4 {

void StartFrameCapture(rex::Runtime* runtime);

}  // namespace doa4
