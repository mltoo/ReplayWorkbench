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
		T *const allocationStart;

		/**
		 * Initialised a superblock around an allocated section of
		 * memory
		 *
		 * @param allocationStart The start of the allocated section of
		 *	memory.
		 */
		SuperblockAllocation(T *const allocationStart) noexcept;
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
		BCPtr(Block *block, T *ptr) noexcept;

		/**
		 * Create a `BCPtr`, copying from another `BCPtr`
		 *
		 * @param copy The `BCPtr` to copy from
		 */
		BCPtr(const BCPtr &copy) noexcept;

		/**
		 * Destroy a `BCPtr` and remove it from its linked list
		 */
		~BCPtr() noexcept;

		/**
		 * Copy one `BCPtr` into another, keeping linked lists up-to-
		 * date
		 *
		 * @param other The `BCPtr` to copy from
		 */
		BCPtr &operator=(const BCPtr &other) noexcept;

		/**
		 * Get the block the `BCPtr` is within
		 *
		 * @return The block the `BCPtr` is within
		 */
		Block *getBlock() const noexcept;

		/**
		 * Return the point within a block this `BCPtr` points to
		 *
		 * @return The specific pointer this `BCPtr` points to
		 */
		T *getPtr() const noexcept;

		/**
		 * Move this BCPtr to a new location
		 *
		 * @param newBlock The block this `BCPtr` should now point to
		 * @param newPoint The pointer to the specific element this
		 *	`BCPtr` should now point to
		 */
		void move(Block *newBlock, T *newPos) noexcept;
	};

	class Block {
	private:
		const SuperblockAllocation *const parentSuperblock;
		T *blockStart;
		size_t blockLength;
		Block *next;
		Block *prev;
		Block *logicalNext;
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
		Block(const BlockCirclebuf<T> &parentContainer,
		      const SuperblockAllocation *const parentSuperblock,
		      T *blockStart, size_t blockLength, Block *next) noexcept;

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
		void split(const BCPtr &splitPoint);

		/**
		 * Get the length of the block's memory region, as a number of 
		 * `T` objects.
		 *
		 * @return The length of the block's memory region, as a number
		 *	of `T` objects.
		 */
		size_t getLength() const noexcept;

		/**
		 * Get a pointer to the start of the block in memory.
		 *
		 * @return Pointer to the start of the block in memory.
		 */
		T *getStartPtr() const noexcept;

		/**
		 * Get the current block's next block.
		 *
		 * @return The current block's next block.
		 */
		Block *getNext() const noexcept;

		/**
		 * Get the block previous to the current block.
		 *
		 * @return The previous block to the current block
		 */
		Block *getPrev() const noexcept;

		/**
		 * Get the block which 'logically' follows this one at present,
		 * that is, the block which follows this one in the current
		 * 'active' BlockCirclebuf (i.e. skipping blocks being excluded
		 * temporarily from the active buffer, for example when sections
		 * of the buffer need to be preserved).
		 *
		 * @return The 'logical' next block (see above)
		 */
		Block *getLogicalNext() const noexcept;

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
		bool getTailPassedYet() const noexcept;

		/**
		 * Sets whether the tail has passed the block yet or not (see
		 * docs for `getTailPassedYet`)
		 *
		 * @param tailPassedYet The new value (see above)
		 */
		void setTailPassedYet(const bool tailPassedYet) noexcept;

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
		Block(const BlockCirclebuf& parentContainer, const SuperblockAllocation *const parentSuperblock,
		      T *blockStart, size_t blockLength) noexcept;
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
	Block *allocateSuperblock(const size_t size);

	/**
	 * Advances the tail pointer to the start of the next block
	 */
	virtual void advanceTailToNextBlock();

	/**
	 * Advances the head pointer to the start of the next block
	 */
	virtual void advanceHeadToNextBlock();

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
	virtual void write(const T * input, size_t count) const noexcept;

	/**
	 * Read objects from the circlebuf into a buffer.
	 * 
	 * @param buffer The buffer
	 * @param count The number of `T` objects to read
	 * @return The number of `T` objects successfully read
	 */
	virtual size_t read(T *buffer, size_t count) const noexcept;

	/**
	 * Get the distance between two `BCPtrs`, from `a` to `b`
	 *
	 * @param a The first `BCPtr`
	 * @param b The second `BCPtr`
	 * @return The distance between the two `BCPtrs`
	 */
	size_t ptrDifference(const BCPtr &a, const BCPtr &b) const noexcept;

	/**
	 * Get the amount of data in the circlebuf (i.e. distance between head
	 * and tail pointers)
	 *
	 * @return The amount of data in the circlebuf
	 */
	size_t bufferHealth() const noexcept;
};
}

//include implementation here to allow template expansion at compile time:
//#include "blockCirclebuf.cpp"
