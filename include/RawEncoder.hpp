#pragma once

#include "ClipEncoder.hpp"
#include "blockCirclebuf.hpp"

namespace ReplayWorkbench {

class RawEncoder : public ClipEncoder {
private:
	struct RawEncodedVideoPacket {
		size_t length;
		void* video;
	};
	BlockCirclebuf<RawEncodedVideoPacket> videoClipBuffer;

	struct RawEncodedAudioPacket {
		size_t length;
		void* audio;
	};
	BlockCirclebuf<RawEncodedAudioPacket> audioClipBuffer;
public:
	RawEncoder();

	void encodeFrame(const void *frameStart, size_t size) override;
	void encodeAudioPacket(const void *packetStart, size_t) override;
};

}
