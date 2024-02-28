#pragma once

#include <cstdio>
#include <iostream>
#include <new>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <assert.h>
#include <string.h>

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
	private:
		T *allocationStart;

	public:
		T *getAllocationStart() { return allocationStart; }

		/**
		 * Initialised a superblock around an allocated section of
		 * memory
		 *
		 * @param allocationStart The start of the allocated section of
		 *	memory.
		 */
		SuperblockAllocation(const size_t size) noexcept
			: allocationStart([size]() {
				  T *alloc = (T *)malloc(size * sizeof(T));
				  if (!alloc)
					  throw std::bad_alloc();
				  return alloc;
			  }())
		{
		}

		SuperblockAllocation(const SuperblockAllocation &other) = delete;

		SuperblockAllocation(SuperblockAllocation &&other) noexcept
			: allocationStart(other.allocationStart)
		{
			other.allocationStart = nullptr;
		}

		SuperblockAllocation &
		operator=(SuperblockAllocation &&other) noexcept
		{
			this->allocationStart = other.allocationStart;
			other.allocationStart = nullptr;
		}

		SuperblockAllocation &
		operator=(SuperblockAllocation &other) = delete;

		~SuperblockAllocation()
		{
			if (allocationStart) {
				free(allocationStart);
			}
		}
	};

	/**
	 * Pointer to a position in the `BlockCirclebuf`. Also forms a linked
	 * list of `BCPtrs` in the same block, so all referring pointers can be
	 * updated when the block is split or merged with another, to refer to
	 * the new block instead
	 */
	class BCPtr {
		friend class Block;

	private:
		Block *block;
		T *ptr;
		BCPtr *next;
		BCPtr *prev;

	public:
		/**
		 * Create a 'blank' `BCPtr`.
		 */
		BCPtr()
			: block(nullptr),
			  ptr(nullptr),
			  next(nullptr),
			  prev(nullptr)
		{
		}

		/**
		 * Create a `BCPtr` at a certain position in a block
		 *
		 * @param block The block this `BCPtr` points to
		 * @param ptr The position within the block this `BCPtr` points
		 *	to
		 */
		BCPtr(Block *block, T *ptr)
		{
			if (ptr < block->getStartPtr() ||
			    ptr >= block->getStartPtr() + block->getLength())
				throw std::out_of_range(
					"Initialising a BCPtr out of range of the provided block");

			this->block = block;
			this->ptr = ptr;
			this->prev = nullptr;
			this->next = block->referencingPtrs;
			if (block->referencingPtrs) {
				block->referencingPtrs->prev = this;
			}
			block->referencingPtrs = this;
		}

		/**
		 * Create a `BCPtr`, copying from another `BCPtr`
		 *
		 * @param copy The `BCPtr` to copy from
		 */
		BCPtr(const BCPtr &copy) noexcept : BCPtr(copy.block, copy.ptr)
		{
			this->next = this->block->referencingPtrs;
			this->prev = nullptr;
			if (this->block->referencingPtrs) {
				this->block->referencingPtrs->prev = this;
			}
			this->block->referencingPtrs->prev = this;
		}

		BCPtr(BCPtr &&other) noexcept
		{
			this->block = other.block;
			this->ptr = other.ptr;
			this->next = other.next;
			this->prev = other.prev;
			if (this->next) {
				this->next->prev = this;
				other.next = nullptr;
			}
			if (this->prev) {
				this->prev->next = this;
				other.prev = nullptr;
			}
			other.block = nullptr;
			other.ptr = nullptr;
		}

		/**
		 * Destroy a `BCPtr` and remove it from its linked list
		 */
		~BCPtr() noexcept
		{
			if (prev)
				prev->next = next;
			if (next)
				next->prev = prev;
			if (block && block->referencingPtrs == this)
				block->referencingPtrs = next;
		}

		/**
		 * Copy one `BCPtr` into another, keeping linked lists up-to-
		 * date
		 *
		 * @param other The `BCPtr` to copy from
		 */
		BCPtr &operator=(const BCPtr &other) noexcept
		{
			if (other.block != this->block) {
				if (prev)
					prev->next = next;
				if (next)
					next->prev = prev;
				if (block->referencingPtrs == this)
					block->referencingPtrs = next;

				this->prev = nullptr;
				this->next = other.block->referencingPtrs;
				if (other.block->referencingPtrs)
					other.block->referencingPtrs->prev =
						this;
				other.block->referencingPtrs = this;
				this->block = other.block;
			}

			this->ptr = other.ptr;

			return *this;
		}

		/**
		 * Assign to a BCPtr with move semantics.
		 *
		 * @param other Reference to an rvalue BCPtr to move from
		 */
		BCPtr &operator=(BCPtr &&other) noexcept
		{
			if (other.block != this->block) {
				if (this->next) {
					this->next->prev = this->prev;
				}
				if (this->prev) {
					this->prev->next = this->next;
				}
				if (this->block &&
				    this->block->referencingPtrs == this) {
					this->block->referencingPtrs =
						this->next;
				}
				this->next = other.next;
				this->prev = other.prev;
				this->block = other.block;
				if (this->block &&
				    this->block->referencingPtrs == &other) {
					this->block->referencingPtrs = this;
				}
				if (this->next) {
					this->next->prev = this;
				}
				if (this->prev) {
					this->prev->next = this;
				}
			} else {
				if (other.prev) {
					other.prev->next = other.next;
				} else {
					other.block->referencingPtrs =
						other.next;
				}
				if (other.next) {
					other.next->prev = other.prev;
				}
			}
			this->ptr = other.ptr;
			other.block = nullptr;
			other.ptr = nullptr;
			other.next = nullptr;
			other.prev = nullptr;
			return *this;
		}

		/**
		 * Get the block the `BCPtr` is within
		 *
		 * @return The block the `BCPtr` is within
		 */
		Block *getBlock() const noexcept { return block; }

		/**
		 * Return the point within a block this `BCPtr` points to
		 *
		 * @return The specific pointer this `BCPtr` points to
		 */
		T *getPtr() const noexcept { return ptr; }

		/**
		 * Move this BCPtr to a new location
		 *
		 * @param newBlock The block this `BCPtr` should now point to
		 * @param newPoint The pointer to the specific element this
		 *	`BCPtr` should now point to
		 */
		void move(Block *newBlock, T *newPos)
		{
			if (this->block) {
				if (this->prev == nullptr) {
					this->block->referencingPtrs =
						this->next;
				} else {
					this->prev->next = this->next;
				}
				if (this->next != nullptr) {
					this->next->prev = this->prev;
				}
				this->block = newBlock;
			}

			if (!newBlock) {
				assert(!newPos);
				this->block = nullptr;
				this->ptr = nullptr;
				this->next = nullptr;
				this->prev = nullptr;
			} else {
				assert(newPos >= newBlock->getStartPtr());
				assert(newPos < newBlock->getStartPtr() +
							newBlock->getLength());

				this->prev = nullptr;
				this->next = newBlock->referencingPtrs;
				if (this->next != nullptr) {
					this->next->prev = this;
				}
				newBlock->referencingPtrs = this;
				this->ptr = newPos;
			}
		}
	};

	class Block {
		friend class BCPtr;
		friend class BlockCirclebuf<T>;

	private:
		T *blockStart;
		size_t blockLength;
		Block *next;
		Block *prev;
		Block *logicalPrev;
		Block *protectionStartEndPtr;
		size_t protectionLength;
		size_t totalProtectionLength;
		BCPtr *referencingPtrs;

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
		Block(const BlockCirclebuf<T> &parentContainer, T *blockStart,
		      size_t blockLength, Block *next) noexcept
		{
			this->blockStart = blockStart;
			this->blockLength = blockLength;

			this->next = next;
			if (this == next)
				this->prev = next;
			else
				this->prev = next->prev;
			this->logicalPrev = nullptr;
			next->prev = this;
			this->prev->next = this;

			referencingPtrs = nullptr;
		}

		~Block()
		{
			if (prev) {
				prev->next = next;
			}
			if (next) {
				next->prev = prev;
			}
			while (referencingPtrs) {
				BCPtr *next = referencingPtrs->next;
				referencingPtrs->move(nullptr, nullptr);
				referencingPtrs = next;
			}
			std::flush(std::cout);
		}

	public:
		Block(const Block &other) = delete;
		Block(const Block &&other) = delete;
		Block *operator=(const Block &other) = delete;
		Block *operator=(const Block &&other) = delete;

		/**
		 * Split the block in two at a certain point. The new block
		 * will become this block's `next` block.
		 *
		 * @param splitPoint The point at which the block should be
		 *	split
		 * @param circlebuf The BlockCirclebuf containing this block
		 */
		void split(T *splitPoint, const BlockCirclebuf<T> &circlebuf)
		{
			// utility variables for conciseness:
			T *headPtr = circlebuf.head.getPtr();
			T *tailPtr = circlebuf.head.getPtr();
			Block *headBlock = circlebuf.head.getBlock();
			Block *tailBlock = circlebuf.tail.getBlock();

			if (splitPoint < blockStart ||
			    splitPoint > blockStart + blockLength)
				throw std::out_of_range(
					"Tried to split a BlockCirclebuf block at an out-of-range splitPoint");

			Block *newBlock = new Block(
				circlebuf, splitPoint,
				blockLength - (splitPoint - blockStart),
				this->next);
			this->blockLength = blockLength - newBlock->blockLength;

			bool asInCurrentBlock = true;
			if (headBlock == this) {
				if (tailBlock == this) {
					if (headPtr < splitPoint &&
					    tailPtr >= splitPoint) {
						newBlock->logicalPrev = nullptr;
						asInCurrentBlock = false;
					} else if (headPtr >= splitPoint &&
						   tailPtr < splitPoint) {
						newBlock->logicalPrev = this;
						asInCurrentBlock = false;
					}
				} else if (headPtr < splitPoint) {
					newBlock->logicalPrev = nullptr;
					asInCurrentBlock = false;
				}
			} else if (tailBlock == this && tailPtr < splitPoint) {
				newBlock->logicalPrev = this;
				asInCurrentBlock = false;
			}
			if (asInCurrentBlock) {
				newBlock->logicalPrev =
					this->logicalPrev ? nullptr : this;
			}

			//update all pointers after the split:
			BCPtr *currentBCPtr = this->referencingPtrs;
			BCPtr *nextBCPtr;
			while (currentBCPtr) {
				if (currentBCPtr->ptr < splitPoint) {
					currentBCPtr = currentBCPtr->next;
					continue;
				}

				nextBCPtr = currentBCPtr->next;

				if (currentBCPtr->next)
					currentBCPtr->next->prev =
						currentBCPtr->prev;
				if (currentBCPtr->prev)
					currentBCPtr->prev->next =
						currentBCPtr->next;

				currentBCPtr->block = newBlock;
				currentBCPtr->next = newBlock->referencingPtrs;
				if (newBlock->referencingPtrs)
					newBlock->referencingPtrs->prev =
						currentBCPtr;
				newBlock->referencingPtrs = currentBCPtr;

				currentBCPtr = nextBCPtr;
			}
			if (newBlock->referencingPtrs)
				newBlock->referencingPtrs->prev = nullptr;
		}

		/**
		 * Split the block in two at a certain point. The new block
		 * will become this block's `next` block.
		 *
		 * @param splitPoint The point at which the block should be
		 *	split
		 * @param circlebuf The BlockCirclebuf containing this block
		 */
		void split(const BCPtr &splitPoint,
			   const BlockCirclebuf<T> &circlebuf)
		{
			if (splitPoint.block != this)
				throw std::runtime_error(
					"BCPtr provided to split a block referenced a different block");
			split(splitPoint.ptr, circlebuf);
		}

		/**
		 * Get the length of the block's memory region, as a number of 
		 * `T` objects.
		 *
		 * @return The length of the block's memory region, as a number
		 *	of `T` objects.
		 */
		size_t getLength() const noexcept { return blockLength; }

		/**
		 * Get a pointer to the start of the block in memory.
		 *
		 * @return Pointer to the start of the block in memory.
		 */
		T *getStartPtr() const noexcept { return blockStart; }

		/**
		 * Get the current block's next block.
		 *
		 * @return The current block's next block.
		 */
		Block *getNext() const noexcept { return next; }

		/**
		 * Get the block previous to the current block.
		 *
		 * @return The previous block to the current block
		 */
		Block *getPrev() const noexcept { return prev; }

		/**
		 * Attempt to merge a block with the next block. Intended
		 * for use after removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcilePrev()
		{
			return next->attemptReconcilePrev();
		}

		/**
		 * Attempt to merge a block with the next block. Intended
		 * for use after removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcileNext()
		{
			//fail if reconciling with self
			if (prev == this)
				return false;

			//fail if not in same superblock
			if (prev->parentSuperblock != this->parentSuperblock)
				return false;

			//fail if not physically adjacent
			if (prev->blockStart + prev->blockLength !=
			    this->blockStart)
				return false;

			prev->blockLength =
				prev->blockLength + this->blockLength;
			prev->next = this->next;
			this->next->prev = prev;
			prev->willReconcileNext = false;

			//update all BCPtrs to point to newly reconciled block.
			BCPtr *currentBCPtr;
			while (this->referencingPtrs) {
				currentBCPtr = this->referencingPtrs;
				currentBCPtr->block = prev;

				if (prev->referencingPtrs)
					prev->referencingPtrs->prev =
						currentBCPtr;
				this->referencingPtrs = currentBCPtr->next;
				currentBCPtr->next = prev->referencingPtrs;

				prev->referencingPtrs = currentBCPtr;
			}

			if (prev->referencingPtrs)
				prev->referencingPtrs->prev = nullptr;

			this->~Block();
			delete this;
			return true;
		}

	private:
		/**
		 * Initialise a block with itself as its next and previous.
		 * Intended for starting a new BlockCirclebuf/superblock.
		 *
		 * @param blockStart Pointer to where the new block will start 
		 * @param blockLength The number of `T` objects the block 
		 *	will contain
		 */
		Block(const BlockCirclebuf &parentContainer, T *blockStart,
		      size_t blockLength) noexcept
			: Block(parentContainer, blockStart, blockLength, this)
		{
		}
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
	Block *allocateSuperblock(const size_t size)
	{
		superblockAllocations.emplace_back(size);
		SuperblockAllocation &alloc = superblockAllocations.back();
		return new Block(*this, &alloc, alloc.allocationStart, size);
	}

	/**
	 * Advances the tail pointer to the start of the next block
	 */
	virtual void advanceTailToNextBlock()
	{

		Block *nextBlock;
		if (tail.getBlock()->getLogicalNext()->getPrev() ==
		    tail.getBlock()) {
			nextBlock = tail.getBlock()->getLogicalNext();
		} else {
			nextBlock = tail.getBlock()->getNext();
		}
		tail.move(nextBlock, nextBlock->getStartPtr());
	}

	/**
	 * Advances the head pointer to the start of the next block
	 */
	virtual void advanceHeadToNextBlock()
	{

		Block *nextBlock = head.getBlock()->getNext();

		/*
		 * skip protected sections which begin after where the head
		 * already was
		 */
		while (nextBlock->protectionLength == 1) {
			/*
			 * If the end of the current section is 'underneath'
			 * another section, we can always skip the 'covering' 
			 * section as it is guaranteed to have come after. We
			 * keep skipping until the end block of a skipped
			 * section is not 'covered' by an earlier section, at
			 * which point the next block after this is guaranteed
			 * to be either of a protected section started before
			 * the entry to the above if-statement (job done), the
			 * start of a new protected section (handled by outer
			 * while-loop), or unprotected (job done)
			 */
			while (nextBlock->protectionStartEndPtr
				       ->protectionStartEndPtr != nextBlock) {
				nextBlock = nextBlock->protectionStartEndPtr
						    ->protectionStartEndPtr;
			}
			nextBlock = nextBlock->protectionStartEndPtr->next;
		}

		while (nextBlock->getPrev() != nullptr) {
			advanceTailToNextBlock();
		}
		while (nextBlock->protectionLength != 0) {
			if (tail.getBlock() == nextBlock) {
				while (nextBlock->protectionLength != 0) {
					nextBlock->logicalPrev = nullptr;
					nextBlock = nextBlock->getNext();
				}
				tail.move(nextBlock, nextBlock->getStartPtr());
			} else {
				if (nextBlock->protectionLength == 1 &&
				    nextBlock->protectionStartEndPtr
						    ->logicalPrev == nullptr) {
					nextBlock =
						nextBlock->protectionStartEndPtr;
				} else {
					nextBlock = nextBlock->getNext();
				}
			}
		}
		nextBlock->logicalPrev = head.getBlock();
		head.move(nextBlock, nextBlock->getStartPtr);
	}

public:
	/**
	 * Initialise a BlockCirclebuf with an initial superblock of a certain size
	 *
	 * @param firstBlock The new BlockCirclebuf's first block
	 */
	BlockCirclebuf(size_t size)
		: superblockAllocations(),
		  head([this, size]() {
			  superblockAllocations.emplace_back(size);
			  return BCPtr(
				  new Block(*this,
					    superblockAllocations[0]
						    .getAllocationStart(),
					    size),
				  superblockAllocations[0].getAllocationStart());
		  }()),
		  tail(BCPtr(head.getBlock(), head.getPtr()))
	{
	}

	~BlockCirclebuf()
	{
		Block *currentBlock = head.getBlock();
		while (currentBlock) {
			Block *nextBlock = currentBlock->getNext();
			delete currentBlock;
			currentBlock = nextBlock == currentBlock ? nullptr
								 : nextBlock;
		}
	}

	BlockCirclebuf &operator=(const BlockCirclebuf &other) = delete;
	BlockCirclebuf(const BlockCirclebuf &other) = delete;
	BlockCirclebuf &operator=(BlockCirclebuf &&other)
	{
		Block *currentBlock = head.getBlock();
		while (currentBlock) {
			Block *nextBlock = currentBlock->getNext();
			delete currentBlock;
			currentBlock = nextBlock == currentBlock ? nullptr
								 : nextBlock;
		}
		this->tail = std::move(other.tail);
		this->head = std::move(other.head);
		this->superblockAllocations =
			std::move(other.superblockAllocations);
		return *this;
	}

	/**
	 * Allocate a new superblock between two existing blocks.
	 *
	 * @param size The size of the superblock
	 * @param prev The block to appear before the first block of the new 
	 *	superblock
	 * @param next The block to appear after the first block of the new 
	 *	superblock
	 */
	void allocateSuperblock(size_t size, Block *prev, Block *next)
	{
		T *allocation = (T *)malloc(size * sizeof(T));
		if (!allocation)
			throw std::bad_alloc();
		superblockAllocations.push_back(
			SuperblockAllocation(allocation));
		SuperblockAllocation &alloc = superblockAllocations.back();
		Block *firstBlock = new Block(*alloc, alloc.allocationStart,
					      size, prev, next);
		prev->next = firstBlock;
		next->prev = firstBlock;
	}

	/**
	 * Write objects from a buffer into the circlebuf. Written data may
	 * cross block boundaries
	 *
	 * @param input The input buffer
	 * @param count The number of `T` objects to be read
	 */
	virtual void write(const T *input, size_t count) noexcept
	{
		assert(input != nullptr);
		size_t numRead = 0;
		while (numRead < count) {
			size_t numToRead = count - numRead;
			/* We only need to worry about the tail being in the 
			 * same block as the head if it entered before the head
			 * (and set logicalPrev=nullptr) and if there isn't
			 * enough space between them for the new data.
			 */
			if (tail.getBlock() == head.getBlock() &&
			    (head.getBlock()->logicalPrev != nullptr) &&
			    tail.getPtr() - head.getPtr() <
				    (ptrdiff_t)numToRead) {
				long numToSkip = numToRead - (tail.getPtr() -
							      head.getPtr());
				size_t readCount = read(nullptr, numToSkip);
				assert(readCount == numToSkip);
			}
			size_t spaceLeftInBlock =
				head.getBlock()->getStartPtr() +
				head.getBlock()->getLength() - head.getPtr();
			if (numToRead < spaceLeftInBlock) {
				memcpy(head.getPtr(), input + numRead,
				       numToRead);
				head.move(head.getBlock(),
					  head.getPtr() + numToRead);
				return;
			} else {
				memcpy(head.getPtr(), input + numRead,
				       spaceLeftInBlock);
				numRead += spaceLeftInBlock;
				this->advanceHeadToNextBlock();
			}
		}
	}

	/**
	 * Read objects from the circlebuf into a buffer.
	 * 
	 * @param buffer The buffer
	 * @param count The number of `T` objects to read
	 * @return The number of `T` objects successfully read
	 */
	virtual size_t read(T *buffer, size_t count) noexcept
	{
		auto memcpyIfNotNull = [](T *dest, const T *src, size_t count) {
			if (dest) {
				memcpy(dest, src, count);
			}
		};
		size_t numRead = 0;
		while (numRead < count) {
			size_t numToRead = count - numRead;
			/* If the tail is ahead of the head (implied by 
			 * block.logicalPrev not being nullptr), we can ignore 
			 * that the head and tail are in the same block.
			 */
			if (head.getBlock() == tail.getBlock() &&
			    tail.getBlock()->logicalPrev != nullptr) {
				if (head.getPtr() - tail.getPtr() <
				    (ptrdiff_t)numToRead) {
					memcpyIfNotNull(
						buffer + numRead, tail.getPtr(),
						head.getPtr() - tail.getPtr());
					numRead +=
						head.getPtr() - tail.getPtr();
					tail.move(tail.getBlock(),
						  head.getPtr());
					return numRead;
				} else {
					memcpyIfNotNull(buffer + numRead,
							tail.getPtr(),
							numToRead);
					tail.move(tail.getBlock(),
						  tail.getPtr() + numToRead);
					return count;
				}
			}
			size_t spaceLeftInBlock =
				tail.getBlock()->getStartPtr() +
				tail.getBlock()->getLength() - tail.getPtr();
			if (numToRead < spaceLeftInBlock) {
				memcpyIfNotNull(buffer + numRead, tail.getPtr(),
						numToRead);
				tail.move(tail.getBlock(),
					  tail.getPtr() + numToRead);
				return count;
			} else {
				memcpyIfNotNull(buffer + numRead, tail.getPtr(),
						spaceLeftInBlock);
				numRead += spaceLeftInBlock;
				this->advanceTailToNextBlock();
			}
		}
		return numRead;
	}

	/**
	 * Get the distance between two `BCPtrs`, from `a` to `b`
	 *
	 * @param a The first `BCPtr`
	 * @param b The second `BCPtr`
	 * @return The distance between the two `BCPtrs`
	 */
	size_t ptrDifference(const BCPtr &a, const BCPtr &b) const noexcept
	{

		size_t accumulator = 0;
		BCPtr currentPosn(a);
		while (currentPosn.getBlock() != b.getBlock() ||
		       currentPosn.getPtr() < b.getPtr()) {
			accumulator += (currentPosn.getBlock()->getLength()) -
				       (currentPosn.getPtr() -
					currentPosn.getBlock()->getStartPtr());
			currentPosn.getPtr() =
				currentPosn.getBlock()->getNext()->getStartPtr();
			currentPosn.getBlock() =
				currentPosn.getBlock()->getNext();
		}

		accumulator += b.getPtr() - a.getPtr();
		return accumulator;
	}

	/**
	 * Get the amount of data in the circlebuf (i.e. distance between head
	 * and tail pointers)
	 *
	 * @return The amount of data in the circlebuf
	 */
	size_t bufferHealth() const noexcept
	{
		return ptrDifference(tail, head);
	}

	const BCPtr &getHead() const { return head; }

	const BCPtr &getTail() const { return tail; }
};
}
