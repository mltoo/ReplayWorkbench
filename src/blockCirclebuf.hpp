#pragma once

#include <vector>

namespace ReplayWorkbench {

/*
 * Circular-buffer-like datastructure, but where the buffer is split across 
 * several `blocks' (which may be non-contiguous) which can be further logically
 * subdivided in O(1) at any time. These blocks can be `write-protected' at any
 * time, which prevents the currently stored data from being overwritten; the 
 * write-head just moves to the next unprotected block to continue.
 */
template<typename T> class BlockCirclebuf {
public:
	class Block;

	/*
	 * `Superblock' of blocks declared within a single memory allocation.
	 * Essentially only necessary to keep track of root of free list node 
	 * despite splitting blocks, etc.
	 */
	struct SuperblockAllocation {
		T *allocationStart;

		SuperblockAllocation(T *allocationStart);
	};

	/*
	 * Pointer to a position in the BlockCirclebuf
	 */
	class BCPtr {
	private:
		Block *block;
		T *ptr;
		BCPtr *next;
		BCPtr *prev;

	public:
		BCPtr(Block *block, T *ptr);
		BCPtr(BCPtr &copy);
		~BCPtr();
		BCPtr &operator=(const BCPtr &other);
	};

	class Block {
	private:
		SuperblockAllocation *parentSuperblock;
		bool writeProtect;
		bool readProtect;
		T *blockStart;
		size_t blockLength;
		Block *next;
		Block *prev;
		bool willReconcilePrev;
		bool willReconcileNext;
		BCPtr *referencingPtrs;

	public:
		Block(SuperblockAllocation *parentSuperblock, T *blockStart,
		      size_t blockLength, Block *prev, Block *next);
		void split(T *splitPoint);
		void split(BCPtr &splitPoint);
		void protect();
		void unprotect();
		bool isProtected();
		size_t getLength();
		T *getStartPtr();
		Block *getNext();
		Block *getPrev();
		bool attemptReconcilePrev();
		bool attemptReconcileNext();

	private:
		/*
		 * Initialises a block with itself as its next and previous.
		 * Intended for starting a new BlockCirclebuf
		 */
		Block(SuperblockAllocation *parentSuperblock, T *blockStart,
		      size_t blockLength);
	};

private:
	std::vector<SuperblockAllocation> superblockAllocations;
	BCPtr head;
	BCPtr tail;

	Block *allocateSuperblock(size_t size);

public:
	BlockCirclebuf(size_t size);
	void allocateSuperblock(size_t size, Block *prev, Block *next);
	void write(T *input, size_t count);
	size_t read(T *buffer, size_t count);
	size_t ptrDifference(BCPtr &a, BCPtr &b);
	size_t bufferHealth();
};
}

//include implementation here to allow template expansion at compile time:
#include "blockCirclebuf.cpp"
