#pragma once

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
#include <assert.h>

namespace ReplayWorkbench {

/**
 * Circular-buffer-like datastructure, but where the buffer is split across 
 * several `blocks' (which may be non-contiguous) which can be further logically
 * subdivided in O(1) at any time. Blocks may be temporarily excluded from the
 * chain, to be merged back in at a later time.
 *
 * @tparam T The type of object the `BlockCirclebuf<T>` will contain.
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
		 * Initialised a superblock around an allocated section of memory.
		 *
		 * @param allocationStart The start of the allocated section of memory.
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

		SuperblockAllocation &operator=(SuperblockAllocation &&other) noexcept
		{
			this->allocationStart = other.allocationStart;
			other.allocationStart = nullptr;
		}

		SuperblockAllocation &operator=(SuperblockAllocation &other) = delete;

		~SuperblockAllocation()
		{
			if (allocationStart) {
				free(allocationStart);
			}
		}
	};

	/**
	 * Pointer to a position in the `BlockCirclebuf`. Also forms a linked list
     * of `BCPtrs` in the same block, so all referring pointers can be updated
     * when the block is split or merged with another, to refer to the new
     * block instead.
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
		BCPtr() : block(nullptr), ptr(nullptr), next(nullptr), prev(nullptr) {}

		/**
		 * Create a `BCPtr` at a certain position in a block.
		 *
		 * @param block The block this `BCPtr` points to.
		 * @param ptr The position within the block this `BCPtr` points to.
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
		 * Create a `BCPtr`, copying from another `BCPtr`.
		 *
		 * @param copy The `BCPtr` to copy from.
		 */
		BCPtr(const BCPtr &copy) noexcept : BCPtr(copy.block, copy.ptr) {}

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
		 * Destroy a `BCPtr` and remove it from its linked list.
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
		 * Copy one `BCPtr` into another, keeping linked lists up-to-date.
		 *
		 * @param other The `BCPtr` to copy from.
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
					other.block->referencingPtrs->prev = this;
				other.block->referencingPtrs = this;
				this->block = other.block;
			}

			this->ptr = other.ptr;

			return *this;
		}

		/**
		 * Assign to a BCPtr with move semantics.
		 *
		 * @param other Reference to an rvalue BCPtr to move from.
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
				if (this->block && this->block->referencingPtrs == this) {
					this->block->referencingPtrs = this->next;
				}
				this->next = other.next;
				this->prev = other.prev;
				this->block = other.block;
				if (this->block && this->block->referencingPtrs == &other) {
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
					other.block->referencingPtrs = other.next;
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
		 * Return the point within a block this `BCPtr` points to.
		 *
		 * @return The specific pointer this `BCPtr` points to.
		 */
		T *getPtr() const noexcept { return ptr; }

		/**
		 * Move this BCPtr to a new location
		 *
		 * @param newBlock The block this `BCPtr` should now point to.
		 * @param newPoint The pointer to the specific element this `BCPtr`
         * should now point to.
		 */
		void move(Block *newBlock, T *newPos)
		{
			// only allow null block if pos is also null
			assert(!newBlock == !newPos);

			// assert pointer is in range for new block
			assert(!newBlock || newPos >= newBlock->getStartPtr());
			assert(!newBlock ||
				   newPos < newBlock->getStartPtr() + newBlock->getLength());

			this->ptr = newPos;

			//update block and ref list if necessary
			if (this->block != newBlock) {
				if (this->prev == nullptr) {
					this->block->referencingPtrs = this->next;
				} else {
					this->prev->next = this->next;
				}
				if (this->next != nullptr) {
					this->next->prev = this->prev;
				}
				this->block = newBlock;
				if (newBlock) {
					this->next = newBlock->referencingPtrs;
					if (this->next) {
						this->next->prev = this;
					}
					newBlock->referencingPtrs = this;
					this->prev = nullptr;
				}
			}
		}
	};

	// The idea with this is that reservations should be contiguous, so we'd do
	// several reservations in an LL to be able to do non-contiguous
	// protections.
	struct ReservationLL {
		Block *startBlock;
		std::unique_ptr<ReservationLL> next = nullptr;
	};

	class Block {
		friend class BCPtr;
		friend class BlockCirclebuf<T>;

	private:
		/**
		 * Pointer to a section of memory in which this `Block`'s data are
         * stored.
		 */
		T *blockStart;

		/**
		 * Length of the section of memory for which this `Block` is
		 * responsible, starting from `blockStart`.
		 */
		size_t blockLength;

		/**
		 * The `Block` which 'physically' follows this one in the data
		 * structure. If there is only one memory allocation in use, `next`'s
         * pointer literally follows this `Block`'s memory (or wraps round to
         * the start of the allocation to complete the circular structure), but
         * this may point to some other memory. The physical arrangement of
         * the memory is abstracted from the user; this member is to help with
         * freeing structures on deconstruction and determining when two
         * logical `Block`s can be merged.
		 */
		Block *next;

		/**
		 * The `Block` which 'physically' precedes this one in the data
		 * structure. See `next` for details.
		 */
		Block *prev;

		/**
		 * The block which comes before the current one in the 'flow' of
         * written blocks. Set to {@code nullptr} if this block has not yet
         * begun to be written.
		 */
		Block *logicalPrev;
		/**
		 * The block which comes after the current one in the 'flow' of written
         * blocks. Set to {@code nullptr} if this block is not yet fully
         * written.
		 */
		Block *logicalNext;

		/**
		 * Pointer to the `Block` at the end of the highest-priority protected
         * section containing this `Block` if this `Block` is the first `Block`
         * of that P.S., otherwise points to the `Block` at the start of that
         * P.S.
		 */
		Block *protectionStartEndPtr;

		/**
		 * The number of blocks which precede this one in the containing
		 * protected section. Starts at 1 for the beginning of a protected
         * section. Set to 0 for unprotected blocks. If the block is within
         * several overlapping protected sections, set to the lowest value for
         * any of those sections.
         *
		 * Note that this is only for a *contiguous* protected section.
		 */
		size_t protectionLength;

		/**
		 * The total length of the protected section. Only valid on the first
         * block of the P.S.
		 */
		size_t totalProtectionLength;

		/**
		 * If this block is at the end of a protected section, which is not at
         * the end of its containing super-protected-section (non-contiguous
         * P.S.), this points to the block at the start of the next P.S. in the
         * super-P.S.
		 */
		Block *reservationContinuation;

		/**
		 * Pointer to the first `BCPtr` in the LL of `BCPtr`s which reference
         * this `Block`. Allows all referencing `BCPtr`s to be updated if this
         * `Block` gets merged/deleted/split/etc.
		 */
		BCPtr *referencingPtrs;

		/**
		 * Construct a block before an existing block. Preferably blocks should
         * only be made before data is written or any blocks are split,
         * otherwise this probably needs re-writing
		 *
		 * @param parentContainer The `BlockCirclebuf` containing this `Block`.
		 * @param blockStart Pointer to where this block should start.
		 * @param blockLength The length of the block, as a number of `T`
         * objects it can contain.
		 * @param next The block which should follow the new block.
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

			this->protectionLength = 0;
			this->protectionStartEndPtr = nullptr;
		}

		~Block()
		{
			if (prev) {
				prev->next = next;
			}
			if (next) {
				next->prev = prev;
			}
			while (referencingPtrs != nullptr) {
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
		 * Split the block in two at a certain point. The new block will become
         * this block's `next` block, which will have `splitPoint` as its start
         * pointer.
		 *
		 * @param splitPoint The point at which the block should be split
		 * @param circlebuf The BlockCirclebuf containing this block
		 */
		void split(T *splitPoint, const BlockCirclebuf<T> &circlebuf)
		{
			assert(splitPoint >= this->blockStart);
			std::size_t wholeLength{this->blockLength};
			std::size_t newThisLength{
				static_cast<std::size_t>(splitPoint - this->blockStart)};
			std::size_t newBlockLength{wholeLength - newThisLength};

			auto *newBlock{new Block{circlebuf, splitPoint, newBlockLength}};
			newBlock->next = this->next;
			newBlock->prev = this;
			this->blockLength = newThisLength;
			newBlock->blockLength = newBlockLength;
			if (this->next->logicalPrev == this) {
				this->next->logicalPrev = newBlock;
			}
			this->next->prev = newBlock;
			this->next = newBlock;
			if (this->protectionLength == 0) {
				this->next->prev = newBlock;
				this->next = newBlock;
			} else {
				//TODO(AT): Write and run more tests for this
				Block *currentPS{this->protectionLength == 1U
									 ? this
									 : this->protectionStartEndPtr};
				Block *startingPS{currentPS};
				std::size_t currentDist{this->protectionLength + 1U};
				std::size_t distToSplit{currentDist};
				Block *currentBlock{this->next};

				do {
					currentPS->totalProtectionLength += 1U;

					// iterate from the current block to the end of the current
					// protected section, bumping the running protection length
					// counter to represent the new protection length
					while (currentDist <= currentPS->totalProtectionLength) {
						if (currentBlock->protectionStartEndPtr == currentPS) {
							currentBlock->protectionLength++;
						} else {
							// skip to the end of this higher-priority PS
							Block *higherPS =
								currentBlock->protectionLength == 1
									? currentBlock
									: currentBlock->protectionStartEndPtr;
							currentDist += (higherPS->totalProtectionLength -
											currentBlock->protectionLength);
							currentDist++; //1 more for the ->next below

							currentBlock =
								higherPS->protectionStartEndPtr->next;
						}
						currentBlock = currentBlock->next;
						currentDist++;
					}

					// move to the next surrounding PS
					for (;;) {
						Block *candidatePS{currentPS->prev};
						currentDist++;
						distToSplit++;

						if (candidatePS->protectionLength != 1) {
							currentDist += (candidatePS->protectionLength - 1);
							distToSplit += (candidatePS->protectionLength - 1);
							candidatePS = currentPS->protectionStartEndPtr;
						}

						if (currentPS->totalProtectionLength > currentDist) {
							// if the PS extends over the current end pointer,
                            // process it and increment its blocks'
							// protectionLengths
							currentPS = candidatePS;
							break;
						} else if (candidatePS->totalProtectionLength >=
								   distToSplit) {
                            // if the PS is overlapped from the split to the
                            // current end pointer, but contains the split
                            // itself, extend its total length, but continue
                            // iterating back through the PSs
							candidatePS->totalProtectionLength++;
                            if (candidatePS->protectionStartEndPtr == this) {
                                candidatePS->protectionStartEndPtr = this->next;
                            }
						} else if (candidatePS == currentPS) {
                            // if we've wrapped the loop, stop.
							currentPS = nullptr;
							break;
						}
					}

				} while (currentPS != nullptr);
			}

			if (this->logicalPrev != nullptr) {
				// if logical prev exists to left, but head is still in first
				// half, the two halves aren't logically linked UNLESS tail is
				// also in the first half, and after head, in which case the
				// previous write still crosses the split point
				if (circlebuf.head.getBlock() == this &&
					circlebuf.head.getPtr() < splitPoint &&
					(circlebuf.tail.getBlock() != this ||
					 circlebuf.tail.getPtr() >= splitPoint)) {
					newBlock->logicalPrev = nullptr;
				} else {
					newBlock->logicalPrev = this;
				}
			} else {
				newBlock->logicalPrev = nullptr;
			}

			newBlock->logicalNext = this->logicalNext;
			if (this->logicalNext != nullptr) {
				// if logical next exists to right, but tail has passed halfway,
				// the blocks aren't logically linked UNLESS head is also in the
				// second half, in which case it has just written over the split
				// point
				if (circlebuf.tail.getBlock() == this &&
					circlebuf.tail.getPtr() >= splitPoint &&
					(circlebuf.head.getBlock() != this ||
					 circlebuf.head.getPtr() < splitPoint)) {
					this->logicalNext = nullptr;
				} else {
					this->logicalNext = newBlock;
				}
			} else {
				this->logicalNext = nullptr;
			}

			BCPtr *oldPtrs{this->referencingPtrs};
			this->referencingPtrs = nullptr;
			while (oldPtrs != nullptr) {
				BCPtr *next = oldPtrs->next;
				oldPtrs->prev = nullptr;
				if (oldPtrs->getPtr() < splitPoint) {
					oldPtrs->next = this->referencingPtrs;
					if (this->referencingPtrs != nullptr) {
						this->referencingPtrs->prev = oldPtrs;
					}
					this->referencingPtrs = oldPtrs;
				} else {
					oldPtrs->block = newBlock;
					oldPtrs->next = newBlock->referencingPtrs;
					if (newBlock->referencingPtrs != nullptr) {
						newBlock->referencingPtrs->prev = oldPtrs;
					}
					newBlock->referencingPtrs = oldPtrs;
				}
				oldPtrs = next;
			}
		}

		/**
		 * Split the block in two at a certain point. The new block will become
         * this block's `next` block, which will have `splitPoint` as its start
         * pointer.
		 *
		 * @param splitPoint The point at which the block should be split
		 * @param circlebuf The BlockCirclebuf containing this block
		 */
		void split(const BCPtr &splitPoint, const BlockCirclebuf<T> &circlebuf)
		{
			if (splitPoint.block != this)
				throw std::runtime_error(
					"BCPtr provided to split a block referenced a different block");
			split(splitPoint.ptr, circlebuf);
		}

		/**
		 * Get the length of the block's memory region, as a number of `T`
         * objects.
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
		 * Get a pointer to the last {@code T} in this block.
		 *
		 * @return A pointer to the last {@code T} object in this block
		 */
		T *getEndPtr() const noexcept { return getStartPtr() + getLength(); }

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
		 * Attempt to merge a block with the next block. Intended for use after
         * removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcilePrev()
		{
			throw std::runtime_error{"Not implemented"};
			//TODO(AT)
		}

		/**
		 * Attempt to merge a block with the next block. Intended for use after
         * removing read/write protection from a block.
		 *
		 * @return Whether or not the merge was able to be completed
		 */
		bool attemptReconcileNext()
		{
			throw std::runtime_error{"Not implemented"};
			// TODO(AT)
		}

	private:
		/**
		 * Initialise a block with itself as its next and previous. Intended
         * for starting a new BlockCirclebuf/superblock.
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
	 * Allocate a superblock, disconnected from any other existing superblocks.
     * The first block will have itself as next and previous.
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

		Block *nextBlock{tail.getBlock()};
		do {
			nextBlock = nextBlock->next;
			// TODO: can maybe do some optimisation with skipping
			// reserved blocks here. O(num_of_blocks) is probably
			// pretty fast though unless something insane is
			// happening though, so not a major issue.
		} while (nextBlock->logicalPrev != tail.getBlock());
		nextBlock->logicalPrev = nullptr;

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
			while (nextBlock->protectionStartEndPtr->protectionStartEndPtr !=
				   nextBlock) {
				nextBlock =
					nextBlock->protectionStartEndPtr->protectionStartEndPtr;
			}
			nextBlock = nextBlock->protectionStartEndPtr->next;
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
					nextBlock->protectionStartEndPtr->logicalPrev == nullptr) {
					nextBlock = nextBlock->protectionStartEndPtr;
				} else {
					nextBlock = nextBlock->getNext();
				}
			}
		}
		while (nextBlock->logicalPrev != nullptr) {
			advanceTailToNextBlock();
		}
		nextBlock->logicalPrev = head.getBlock();
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
							superblockAllocations[0].getAllocationStart(),
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
			currentBlock = nextBlock == currentBlock ? nullptr : nextBlock;
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
			currentBlock = nextBlock == currentBlock ? nullptr : nextBlock;
		}
		this->tail = std::move(other.tail);
		this->head = std::move(other.head);
		this->superblockAllocations = std::move(other.superblockAllocations);
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
		superblockAllocations.push_back(SuperblockAllocation(allocation));
		SuperblockAllocation &alloc = superblockAllocations.back();
		Block *firstBlock =
			new Block(*alloc, alloc.allocationStart, size, prev, next);
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
				tail.getPtr() - head.getPtr() < (ptrdiff_t)numToRead) {
				auto numToSkip{
					std::min(head.getBlock()->blockLength -
								 (tail.getPtr() - head.getPtr()),
							 numToRead - (tail.getPtr() - head.getPtr()))};
				size_t readCount = read(nullptr, numToSkip);
				assert(readCount == numToSkip);
			}
			size_t spaceLeftInBlock = head.getBlock()->getStartPtr() +
									  head.getBlock()->getLength() -
									  head.getPtr();
			if (numToRead < spaceLeftInBlock) {
				memcpy(head.getPtr(), input + numRead, numToRead);
				head.move(head.getBlock(), head.getPtr() + numToRead);
				return;
			} else {
				memcpy(head.getPtr(), input + numRead, spaceLeftInBlock);
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
				tail.getBlock()->logicalPrev == nullptr) {
				if (head.getPtr() - tail.getPtr() < (ptrdiff_t)numToRead) {
					memcpyIfNotNull(buffer + numRead, tail.getPtr(),
									head.getPtr() - tail.getPtr());
					numRead += head.getPtr() - tail.getPtr();
					tail.move(tail.getBlock(), head.getPtr());
					return numRead;
				} else {
					memcpyIfNotNull(buffer + numRead, tail.getPtr(), numToRead);
					tail.move(tail.getBlock(), tail.getPtr() + numToRead);
					return count;
				}
			}
			size_t spaceLeftInBlock = tail.getBlock()->getStartPtr() +
									  tail.getBlock()->getLength() -
									  tail.getPtr();
			if (numToRead < spaceLeftInBlock) {
				memcpyIfNotNull(buffer + numRead, tail.getPtr(), numToRead);
				tail.move(tail.getBlock(), tail.getPtr() + numToRead);
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
			accumulator +=
				(currentPosn.getBlock()->getLength()) -
				(currentPosn.getPtr() - currentPosn.getBlock()->getStartPtr());
			currentPosn.getPtr() =
				currentPosn.getBlock()->getNext()->getStartPtr();
			currentPosn.getBlock() = currentPosn.getBlock()->getNext();
		}

		accumulator += b.getPtr() - a.getPtr();
		return accumulator;
	}

	/**
	 * Write-protect the data between {@code startPtr} and
	 * {@code startPtr + length}
	 * @param startPtr The position at which the new protected section
	 *	should start
	 * @param length The length of the new protected section
	 */
	std::unique_ptr<ReservationLL> protect(BCPtr const &startPtr, size_t length)
	{
		if (startPtr.getBlock()->logicalPrev == nullptr) {
			throw std::runtime_error(
				"Tried to reserve section of BlockCirclebuf outside written section!");
		}
		Block *startBlock;
		if (startPtr.getPtr() != startPtr.getBlock()->getStartPtr()) {
			// start ptr not at start of existing block, so we have
			// to split the block
			startPtr.getBlock()->split(startPtr.getPtr(), *this);
			// BCPtr should have been updated with the new block it
			// sits in
			assert(startPtr.getPtr() == startPtr.getBlock()->getStartPtr());
			startBlock = startPtr.getBlock();
		} else if (startPtr.getBlock()->protectionLength == 1) {
			// block is the start of an existing protected section;
			// we have to insert a 'shim' block to hold the data of
			// the new P.S.
			Block *shimBlock =
				new Block(*this, startPtr.getPtr(), 0, startPtr.getBlock());
			shimBlock->logicalPrev = startPtr.getBlock()->logicalPrev;
			startPtr.getBlock()->logicalPrev = shimBlock;
			startBlock = shimBlock;
		} else {
			//start ptr is already the start of an unprotected block
			startBlock = startPtr.getBlock();
		}

		std::unique_ptr<ReservationLL> protectedSections{
			new ReservationLL{startBlock, nullptr}};
		ReservationLL *currentPS = protectedSections.get();

		size_t lengthLeftToProtect = length;
		// iterates for each contiguous PS which needs to be created to
		// protect the whole area
		while (lengthLeftToProtect > 0) {
			Block *currentBlock = currentPS->startBlock;
			size_t protectionLength = 0;
			while (lengthLeftToProtect > 0) {
				// Following hackery depends on protection
				// length being unsigned, so 0-1 wraps around
				static_assert(
					!std::is_signed_v<decltype(currentBlock->protectionLength)>);
				currentBlock->protectionLength =
					std::min(protectionLength++,
							 currentBlock->protectionLength - 1) +
					1;

				if (currentBlock->protectionLength == protectionLength) {
					currentBlock->protectionStartEndPtr = currentPS->startBlock;
				}

				// Block is strictly greater than we need -
				// split the bit we need off the front
				if (currentBlock->blockLength > lengthLeftToProtect) {
					currentBlock->split(currentBlock->getStartPtr() +
											lengthLeftToProtect,
										*this);
				}

				// Block is precisely the right size (possibly
				// after being split). Return as we're now done
				if (currentBlock->blockLength == lengthLeftToProtect) {
					lengthLeftToProtect = 0;
					break;
				}
				// Current block isn't enough, need to continue.
				lengthLeftToProtect -= currentBlock->blockLength;

				if (currentBlock->next == currentBlock->logicalNext ||
					currentBlock->protectionLength != 1) {
					// We're allowed into the next
					// contiguous block
					currentBlock->logicalNext = currentBlock->next;
					currentBlock = currentBlock->next;
				} else {
					// need to add a new non-contiguous P.S.
					break;
				}
			}

			//Close of previous string of protected blocks
			currentBlock->protectionStartEndPtr = currentPS->startBlock;
			currentPS->startBlock->protectionStartEndPtr = currentBlock;

			// At this point, either we're completely done, or we
			// need to find a start point for a new PS to continue
			// in (i.e. we follow the head's path)
			if (lengthLeftToProtect == 0) {
				break;
			}

			// if a previous P.S. has already marked out a next
			// block, we have to use the same one
			if (currentBlock->reservationContinuation != nullptr) {
				currentPS->next = std::unique_ptr<ReservationLL>(
					new ReservationLL{currentBlock->reservationContinuation,
									  nullptr});
			} else { // find the next unprotected block
				Block *nextBlock{currentBlock};
				while (nextBlock->protectionLength != 0) {
					nextBlock = nextBlock->next;
					if (nextBlock->protectionLength == 1)
						// Skip the whole P.S.
						nextBlock = nextBlock->protectionStartEndPtr;
					else if (nextBlock->protectionLength > 1) {
						//go back to start, skip whole P.S.
						nextBlock = nextBlock->protectionStartEndPtr
										->protectionStartEndPtr;
					}
				}
				currentBlock->reservationContinuation = nextBlock;
				currentBlock = nextBlock;
			}
		}
		return protectedSections;
	}

	void release(std::unique_ptr<ReservationLL> reservation)
	{
		while (reservation != nullptr) {
			Block *startBlock{reservation->startBlock};
			Block *containingSection{startBlock->prev};
			size_t precedeAmount{1};
			if (containingSection->protectionLength > 1) {
				precedeAmount = containingSection->protectionLength;
				containingSection = containingSection->protectionStartEndPtr;
			}
		}
	}

	void release(BCPtr &startPtr)
	{
		Block *firstBlock{startPtr.getBlock()};
		Block *lastBlock{firstBlock->protectionStartEndPtr};
		size_t originalLength{firstBlock->totalProtectionLength};
		// the newest PS which may overlap the current block
		Block *overridePS;
		// distance between override and current block
		size_t overridePSOffset;
		bool firstMergeEligbility{true};
		bool lastMergeEligibility{false};
		if (firstBlock->prev->protectionLength == 0 ||
			firstBlock->prev->logicalNext != firstBlock) {
			overridePS = nullptr;
		} else {
			if (firstBlock->prev->protectionLength == 1) {
				overridePSOffset = 1;
				overridePS = firstBlock->prev;
			} else {
				overridePSOffset = firstBlock->prev->protectionLength;
				overridePS = firstBlock->prev->protectionStartEndPtr;
			}
			if (overridePS->protectionStartEndPtr == firstBlock->prev) {
				firstMergeEligbility = false;
			}
			if (overridePS->protectionStartEndPtr == lastBlock) {
				lastMergeEligibility = false;
			}
		}
		Block *firstOverridePS{overridePS};
		Block *currentBlock{firstBlock};
		do {
			overridePSOffset += 1;
			// if block is already written with a higher-priority PS
			// we don't need to do anything
			if (currentBlock->protectionStartEndPtr != firstBlock &&
				currentBlock != firstBlock) {
				currentBlock = currentBlock->next;
				continue;
			}

			// backtrack until either we're in 'free space' or we're
			// in a PS long enough to contain currentBlock
			while (overridePS != nullptr &&
				   overridePS->totalProtectionLength < overridePSOffset) {
				//may be able to break out here if overridePS->prev->logicalNext != overridePS
				overridePS = overridePS->prev;
				overridePSOffset += 1;

				// Go back to beginning of PS if we're not
				// already there
				if (overridePS != nullptr &&
					overridePS->protectionLength != 1) {
					overridePSOffset += overridePS->protectionLength - 1;
					overridePS = overridePS->protectionStartEndPtr;
				}
				if (overridePS->protectionStartEndPtr == firstBlock->prev) {
					firstMergeEligbility = false;
				}

				if (overridePS->protectionStartEndPtr == lastBlock) {
					lastMergeEligibility = false;
				}
			}

			if (overridePS == nullptr) {
				currentBlock->protectionStartEndPtr = nullptr;
				currentBlock->protectionLength = 0;
				currentBlock->totalProtectionLength = 0;
				currentBlock = currentBlock->next;
				continue;
			}

			// overridePS->totalProtectionLength must be >= overridePSOffset
			// therefore block is inside overridePS
			currentBlock->protectionLength = overridePSOffset;
			currentBlock->protectionStartEndPtr = overridePS;
			currentBlock->totalProtectionLength = 0;
			currentBlock = currentBlock->next;
			continue;

		} while (currentBlock != lastBlock);

		// Continue going back through preceding PSs until we're in free
		// space or back at the start (may never be possible)
		while (overridePS != nullptr && overridePS != firstOverridePS) {
			// may be able  to break out here if overridePS->prev->logicalNext != overridePS
			overridePS = overridePS->prev;
			overridePSOffset += 1;
			if (overridePS != nullptr) {
				if (overridePS->protectionLength != 1) {
					overridePSOffset += overridePS->protectionLength - 1;
					overridePS = overridePS->protectionStartEndPtr;
				}

				if (overridePS->protectionStartEndPtr == firstBlock->prev) {
					firstMergeEligbility = false;
				}

				if (overridePS->protectionStartEndPtr == lastBlock) {
					lastMergeEligibility = false;
				}
			}
		}

		if (lastBlock->protectionLength > 0) {
			// check that the last block is not the first or last
			// block of another PS
			lastMergeEligibility =
				lastMergeEligibility &&
				lastBlock->protectionStartEndPtr->protectionStartEndPtr !=
					lastBlock;
		}

		Block *firstBlockNewPS{firstBlock->protectionStartEndPtr};
		Block *lastBlockNewPS{lastBlock->protectionLength == 1
								  ? lastBlock
								  : lastBlock->protectionStartEndPtr};
		currentBlock = firstMergeEligbility ? firstBlock : lastBlock;
	}

	/**
	 * Get the amount of data in the circlebuf (i.e. distance between head
	 * and tail pointers)
	 *
	 * @return The amount of data in the circlebuf
	 */
	size_t bufferHealth() const noexcept { return ptrDifference(tail, head); }

	const BCPtr &getHead() const { return head; }

	const BCPtr &getTail() const { return tail; }
};
}
