// doa4 - choice of renderer: the SDK's Xenos emulation (default) or the
// experimental native Plume renderer (--renderer=plume).

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace rex::ui {
class Window;
}

namespace doa4 {

// The native Plume renderer when --renderer=plume (and it is built in), else
// nullptr: the SDK then loads its GPU plugin as usual.
std::unique_ptr<rex::system::IGraphicsSystem> CreateSelectedGraphicsSystem();

// Hands the game window to the native renderer, if it is the one in use.
void AttachWindowToRenderer(rex::system::IGraphicsSystem* graphics, rex::ui::Window* window);

}  // namespace doa4
