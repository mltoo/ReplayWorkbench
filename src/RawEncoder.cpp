#include "RawEncoder.hpp"
#include <stdexcept>
#include <util/c99defs.h>

namespace ReplayWorkbench {

RawEncoder::RawEncoder() : videoClipBuffer(128), audioClipBuffer(128){}

void RawEncoder::encodeFrame(const void *frameStart, size_t size)
{
	throw std::runtime_error("NOT IMPLEMENTED");
	UNUSED_PARAMETER(frameStart);
	UNUSED_PARAMETER(size);
}

void RawEncoder::encodeAudioPacket(const void* frameStart, size_t size) {
	throw std::runtime_error("NOT IMPLEMENTED");
	UNUSED_PARAMETER(frameStart);
	UNUSED_PARAMETER(size);
}

}
