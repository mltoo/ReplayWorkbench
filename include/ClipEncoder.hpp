#pragma once

#include "blockCirclebuf.hpp"

namespace ReplayWorkbench {

class ClipEncoder {

public:
	virtual void encodeFrame(const void *frameStart, size_t size) = 0;
	virtual void encodeAudioPacket(const void* packetStart, size_t size) = 0;

};

}
