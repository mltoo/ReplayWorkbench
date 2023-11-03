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
		      T *blockStart, size_t blockLength, Block *next) noexcept
			: parentSuperblock(parentSuperblock)
		{
			this->blockStart = blockStart;
			this->blockLength = blockLength;
			this->tailPassedYet = true;

			this->next = next;
			if (this == next)
				this->prev = next;
			else
				this->prev = next->prev;
			next->prev = this;
			this->logicalNext = next;
			this->prev->next = this;
			if (this->prev->logicalNext == next)
				this->prev->logicalNext = this;

			if (this == next)
				this->tailPassedYet = true;
			this->tailPassedYet = next->tailPassedYet;
			const BCPtr &head = parentContainer.head;
			const BCPtr &tail = parentContainer.tail;

			/*
			 * Dealing with inserting block with next as start of excluded section
			 * Ideally should never happen; 'correct' behaviour is not necessarily
			 * obvious here, this should hopefully be valid in all situations
			 */
			if (this->prev->logicalNext != this) {
				if (head.getBlock() != next) {
					this->tailPassedYet =
						next->tailPassedYet;
					next->tailPassedYet = true;
				} else {
					if (tail.getBlock() !=
						    head.getBlock() ||
					    tail.getPtr() > head.getPtr()) {
						this->tailPassedYet = false;
						next->tailPassedYet = false;
					} else {
						this->tailPassedYet =
							next->tailPassedYet;
						next->tailPassedYet = true;
					}
				}
			}

			this->willReconcileNext = next->prev->willReconcileNext;
			next->prev->willReconcileNext = false;
			this->willReconcilePrev = false;
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
			if (splitPoint < blockStart ||
			    splitPoint > blockStart + blockLength)
				throw std::out_of_range(
					"Tried to split a BlockCirclebuf block at an out-of-range splitPoint");

			Block *newBlock = new Block(
				parentSuperblock, splitPoint,
				blockLength - (splitPoint - blockStart), this,
				this->next);
			blockLength = blockLength - newBlock->blockLength;
			if (next->prev == this) {
				next->prev = newBlock;
			}
			if (logicalNext->prev == this) {
				logicalNext->prev = newBlock;
			}
			newBlock->prev = this;
			newBlock->next = this->next;
			newBlock->logicalNext = this->logicalNext;
			this->logicalNext = newBlock;
			this->next = newBlock;

			if (circlebuf.tail.getBlock() == this) {
				if (circlebuf.head.getBlock() == this) {
					if (circlebuf.tail.getPtr() >=
					    splitPoint) {
						if (circlebuf.head.getPtr() >=
						    splitPoint) {
							newBlock->tailPassedYet =
								circlebuf.head
									.getPtr() >=
								circlebuf.tail
									.getPtr();
						} else {
							newBlock->tailPassedYet =
								true;
						}
					} else {
						if (circlebuf.head.getPtr() >=
						    splitPoint) {
							newBlock->tailPassedYet =
								false;
						} else {
							newBlock->tailPassedYet =
								circlebuf.head
									.getPtr() >=
								circlebuf.tail
									.getPtr();
						}
					}
				} else {
					newBlock->tailPassedYet =
						circlebuf.tail.getPtr() <
						splitPoint;
				}
			} else {
				newBlock->tailPassedYet = this->tailPassedYet;
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
						newBlock;
				newBlock->referencingPtrs = newBlock;

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
		 */
		void split(const BCPtr &splitPoint)
		{
			if (splitPoint.block != this)
				throw std::runtime_error(
					"BCPtr provided to split a block referenced a different block");
			split(splitPoint.ptr);
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
		 * Get the block which 'logically' follows this one at present,
		 * that is, the block which follows this one in the current
		 * 'active' BlockCirclebuf (i.e. skipping blocks being excluded
		 * temporarily from the active buffer, for example when sections
		 * of the buffer need to be preserved).
		 *
		 * @return The 'logical' next block (see above)
		 */
		Block *getLogicalNext() const noexcept { return logicalNext; }

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
		bool getTailPassedYet() const noexcept
		{
			return this->tailPassedYet == tailPassedYet;
		}

		/**
		 * Sets whether the tail has passed the block yet or not (see
		 * docs for `getTailPassedYet`)
		 *
		 * @param tailPassedYet The new value (see above)
		 */
		void setTailPassedYet(const bool tailPassedYet) noexcept
		{
			this->tailPassedYet = tailPassedYet;
		}

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
		Block(const BlockCirclebuf &parentContainer,
		      const SuperblockAllocation *const parentSuperblock,
		      T *blockStart, size_t blockLength) noexcept
			: Block(parentContainer, parentSuperblock, blockStart,
				blockLength, this)
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
		nextBlock->setTailPassedYet(true);
	}

	/**
	 * Advances the head pointer to the start of the next block
	 */
	virtual void advanceHeadToNextBlock()
	{
		Block *nextBlock;
		if (!head.getBlock()->getNext()->getTailPassedYet()) {
			nextBlock = head.getBlock()->getNext();
		} else {
			nextBlock = head.getBlock()->getLogicalNext();
			nextBlock->prev = head.getBlock();
			while (!nextBlock->getTailPassedYet()) {
				advanceTailToNextBlock();
			}
		}
		nextBlock->setTailPassedYet(false);
		head.move(nextBlock, nextBlock->getStartPtr());
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
					    &superblockAllocations.front(),
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
			if (tail.getBlock() == head.getBlock() &&
			    tail.getPtr() - head.getPtr() <
				    (ptrdiff_t)numToRead) {
				this->advanceTailToNextBlock();
			}
			size_t spaceLeftInBlock =
				head.getBlock()->getStartPtr() +
				head.getBlock()->getLength() - head.getPtr();
			if (numToRead < spaceLeftInBlock) {
				memcpy(head.getPtr(), input, numToRead);
				head.move(head.getBlock(),
					  head.getPtr() + numToRead);
				return;
			} else {
				memcpy(head.getPtr(), input, spaceLeftInBlock);
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
			if (src != nullptr) {
				memcpy(dest, src, count);
			}
		};
		size_t numRead = 0;
		while (numRead < count) {
			size_t numToRead = count - numRead;
			if (head.getBlock() == tail.getBlock()) {
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
};
}
