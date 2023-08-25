#pragma once

#include <vector>

namespace ReplayWorkbench {

/**
 * Circular-buffer-like datastructure, but where the buffer is split across 
 * several `blocks' (which may be non-contiguous) which can be further logically
 * subdivided in O(1) at any time. Blocks may be temporarily excluded from the
 * chain, to be merged back in at a later time
 *
 * @tparam T The type of object the `BlockCirclebuf<T>` will contain
 */
template<typename T> class BlockCirclebuf {
public:
	class Block;

	/**
	 * `Superblock' of blocks declared within a single memory allocation.
	 * Essentially only necessary to keep track of root of free list node 
	 * despite splitting blocks, etc.
	 */
	struct SuperblockAllocation {
		T *allocationStart;

		/**
		 * Initialised a superblock around an allocated section of
		 * memory
		 *
		 * @param allocationStart The start of the allocated section of
		 *	memory.
		 */
		SuperblockAllocation(T *allocationStart);
	};

	/**
	 * Pointer to a position in the `BlockCirclebuf`. Also forms a linked
	 * list of `BCPtrs` in the same block, so all referring pointers can be
	 * updated when the block is split or merged with another, to refer to
	 * the new block instead
	 */
	class BCPtr {
	private:
		Block *block;
		T *ptr;
		BCPtr *next;
		BCPtr *prev;

	public:
		/**
		 * Create a `BCPtr` at a certain position in a block
		 *
		 * @param block The block this `BCPtr` points to
		 * @param ptr The position within the block this `BCPtr` points
		 *	to
		 */
		BCPtr(Block *block, T *ptr);

		/**
		 * Create a `BCPtr`, copying from another `BCPtr`
		 *
		 * @param copy The `BCPtr` to copy from
		 */
		BCPtr(BCPtr &copy);

		/**
		 * Destroy a `BCPtr` and remove it from its linked list
		 */
		~BCPtr();

		/**
		 * Copy one `BCPtr` into another, keeping linked lists up-to-
		 * date
		 *
		 * @param other The `BCPtr` to copy from
		 */
		BCPtr &operator=(const BCPtr &other);

		/**
		 * Get the block the `BCPtr` is within
		 *
		 * @return The block the `BCPtr` is within
		 */
		Block *getBlock();
		T *getPtr();

		void move(Block *newBlock, T *newPos);
	};

	class Block {
	private:
		SuperblockAllocation *parentSuperblock;
		T *blockStart;
		size_t blockLength;
		Block *next;
		Block *prev;
		Block *logicalNext;
		bool tailDirection;
		bool tailPassedYet;
		bool willReconcilePrev;
		bool willReconcileNext;
		BCPtr *referencingPtrs;

	public:
		/**
		 * Construct a block before an existing block. Preferably
		 * blocks should only be made before data is written or any
		 * blocks are split, otherwise this probably needs re-writing
		 *
		 * @param parentSuperblock The superblock containing this block
		 * @param blockStart Pointer to where this block should start
		 * @param blockLength The length of the block, as a number of
		 *	`T` objects it can contain
		 * @param next The block which should follow the new block
		 */
		Block(SuperblockAllocation *parentSuperblock, T *blockStart,
		      size_t blockLength, Block *next);

		/**
		 * Split the block in two at a certain point. The new block
		 * will become this block's `next` block.
		 *
		 * @param splitPoint The point at which the block should be
		 *	split
		 */
		void split(T *splitPoint);

		/**
		 * Split the block in two at a certain point. The new block
		 * will become this block's `next` block.
		 *
		 * @param splitPoint The point at which the block should be
		 *	split
		 */
		void split(BCPtr &splitPoint);

		/**
		 * Get the length of the block's memory region, as a number of 
		 * `T` objects.
		 *
		 * @return The length of the block's memory region, as a number
		 *	of `T` objects.
		 */
		size_t getLength();

		/**
		 * Get a pointer to the start of the block in memory.
		 *
		 * @return Pointer to the start of the block in memory.
		 */
		T *getStartPtr();

		/**
		 * Get the current block's next block.
		 *
		 * @return The current block's next block.
		 */
		Block *getNext();

		/**
		 * Get the block previous to the current block.
		 *
		 * @return The previous block to the current block
		 */
		Block *getPrev();
		
		/**
		 * Get the block which 'logically' follows this one at present,
		 * that is, the block which follows this one in the current
		 * 'active' BlockCirclebuf (i.e. skipping blocks being excluded
		 * temporarily from the active buffer, for example when sections
		 * of the buffer need to be preserved).
		 *
		 * @return The 'logical' next block (see above)
		 */
		const Block *getLogicalNext();

		/**
		 * Attempt to merge a block with the next block. Intended
		 * for use after removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcilePrev();

		/**
		 * Attempt to merge a block with the next block. Intended
		 * for use after removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcileNext();

		/**
		 * Returns a value which decides which 'next block' pointer the
		 * tail `BCPtr` should follow out of this block. If true, the
		 * tail should follow the 'canonical next' pointer (via 
		 * `getNext()`) then flip the tail direction value on the block
		 * it is leaving (via `flipTailDirection()`). If false it should
		 * always follow the 'logical next'.
		 *
		 * @return The 'tail direction' boolean. (see above)
		 */
		const bool& getTailDirection() const;
		
		/**
		 * Flip the value of the 'tail direction' boolean, which decides
		 * whether the tail should follow the 'logical' or 'canonical'
		 * next block pointer (see docs for `getTailDirection()`).
		 */
		void flipTailDirection();

		/**
		 * Get whether or not the tail has passed this block yet. When
		 * a section has been excluded from the 'active' buffer, the
		 * tail must scan that excluded section once, following the old
		 * continuuity of the buffer, before it re-enters the active
		 * buffer section. The head has to allow the tail to re-enter
		 * the active buffer before the head can proceed past the point
		 * at which the 'seam' at which the excluded section split away.
		 * This variable ensures any block the head enters meets that
		 * requirement
		 *
		 * @return Whether the tail has passed the current block yet
		 */
		const bool& getTailPassedYet() const;
		
		/**
		 * Sets whether the tail has passed the block yet or not (see
		 * docs for `getTailPassedYet`)
		 *
		 * @param tailPassedYet The new value (see above)
		 */
		void setTailPassedYet(const bool tailPassedYet);

	private:
		/**
		 * Initialise a block with itself as its next and previous.
		 * Intended for starting a new BlockCirclebuf/superblock.
		 *
		 * @param parentSuperblock The parent superblock of the new 
		 *	block
		 * @param blockStart Pointer to where the new block will start 
		 * @param blockLength The number of `T` objects the block 
		 *	will contain
		 */
		Block(SuperblockAllocation *parentSuperblock, T *blockStart,
		      size_t blockLength);
	};

private:
	std::vector<SuperblockAllocation> superblockAllocations;
	BCPtr head;
	BCPtr tail;

	/**
	 * Allocate a superblock, disconnected from any other existing 
	 * superblocks. The first block will have itself as next and previous.
	 *
	 * @param size The size of the new superblock
	 * @return The first block of the new superblock
	 */
	Block *allocateSuperblock(size_t size);

	/**
	 * Advances the tail pointer to the start of the next block
	 */
	virtual void advanceTailToNextBlock();

protected:
	/**
	 * Move the head pointer onwards.
	 *
	 * @param jump The number of `T` objects by which the head should be 
	 *	advanced
	 */
	virtual void advanceHead(size_t jump);

	/**
	 * Move the tail pointer onwards.
	 *
	 * @param jump The max number of `T` objects by which the tail should 
	 *	be advanced
	 */
	virtual void advanceTail(size_t jump);

	/**
	 * Moves the `tail` pointer onwards sufficiently to allow the head to 
	 * advance by `n` non-contiguous bytes.
	 * 
	 * @param n The number of bytes to be reserved between the head and
	 *	tail pointers
	 */
	virtual void reserveNonContiguous(size_t n);

	/**
	 * Moves the `tail` pointer onwards sufficiently to allow the head to
	 * advance over a section containing a minimum of `n` contiguous bytes.
	 *
	 * @param n The number of contiguous bytes to be reserved between the
	 *	head and tail pointers
	 * @return The start of the reserved contiguous block
	 */
	virtual BCPtr reserveContiguous(size_t n);

public:
	/**
	 * Construct a `BlockCirclebuf`.
	 *
	 * @param size The size of the first superblock in the circlebuf
	 */
	BlockCirclebuf(size_t size);

	/**
	 * Allocate a new superblock between two existing blocks.
	 *
	 * @param size The size of the superblock
	 * @param prev The block to appear before the first block of the new 
	 *	superblock
	 * @param next The block to appear after the first block of the new 
	 *	superblock
	 */
	void allocateSuperblock(size_t size, Block *prev, Block *next);

	/**
	 * Write objects from a buffer into the circlebuf. Written data may
	 * cross block boundaries
	 *
	 * @param input The input buffer
	 * @param count The number of `T` objects to be read
	 */
	virtual void write(T *input, size_t count);

	/**
	 * Read objects from the circlebuf into a buffer.
	 * 
	 * @param buffer The buffer
	 * @param count The number of `T` objects to read
	 * @return The number of `T` objects successfully read
	 */
	virtual size_t read(T *buffer, size_t count);

	/**
	 * Get the distance between two `BCPtrs`, from `a` to `b`
	 *
	 * @param a The first `BCPtr`
	 * @param b The second `BCPtr`
	 * @return The distance between the two `BCPtrs`
	 */
	size_t ptrDifference(BCPtr &a, BCPtr &b);

	/**
	 * Get the amount of data in the circlebuf (i.e. distance between head
	 * and tail pointers)
	 *
	 * @return The amount of data in the circlebuf
	 */
	size_t bufferHealth();
};
}

//include implementation here to allow template expansion at compile time:
//#include "blockCirclebuf.cpp"
