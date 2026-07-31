// Stand-in for the vendor's LGFX_GC9B72.hpp panel definition.
//
// The real file declares `class LGFX : public lgfx::LGFX_Device` and fills
// in the bus config, pin assignments and the GC9B72 init sequence. None of
// that is meaningful without hardware, so here LGFX is just the stubbed
// device that paints into a framebuffer. The point of this file is to keep
// the include line in the sketch identical between host and target.
#pragma once
#include "LovyanGFX.hpp"

class LGFX : public lgfx::LGFX_Device {};
