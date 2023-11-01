#pragma once

#include "blockCirclebuf.hpp"


namespace ReplayWorkbench {
	class ClipBuffer : BlockCirclebuf<uint8_t> {
		protected:
			typedef BlockCirclebuf<uint8_t> InternalCBuf_t;

			void advanceTail(size_t minJump) override;

		public:
			ClipBuffer(size_t size);
		
	};
}
