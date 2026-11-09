#include "Cafe/OS/RPL/COSModule.h"

namespace H264
{
	COSModule* GetModule();
	// Drops the decoder sessions of a title that has stopped. See the comment
	// on the definition; called from CafeSystem::ShutdownTitle.
	void ResetToDefaultState();

	// The frontend's H.264 decoder (libretro's video decode interface,
	// version 4) the next decoder sessions use; null for rh264 in the core
	void SetFrontendDecoder(const struct retro_video_decode_interface* iface);
	// Once a frame: a decoder the GPU was behind on tries again
	void FrameTick();
	// Frontend decoders open now, which must be none once the title is down
	int OpenFrontendDecoders();
}
