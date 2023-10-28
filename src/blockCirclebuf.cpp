#include "blockCirclebuf.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <util/base.h>
#include <util/bmem.h>
#include <util/c99defs.h>
#include <new>
#include <assert.h>

namespace ReplayWorkbench {

template<typename T>
typename BlockCirclebuf<T>::Block *
BlockCirclebuf<T>::allocateSuperblock(const size_t size)
{
	Block *firstBlock = bmalloc(sizeof(Block));
	if (!firstBlock)
		throw std::bad_alloc();
	T *allocation = (T *)bmalloc(size * sizeof(T));
	if (!allocation)
		throw std::bad_alloc();
	superblockAllocations.push_back(SuperblockAllocation(allocation));
	SuperblockAllocation &alloc = superblockAllocations.back();
	*firstBlock = Block(*alloc, alloc.allocationStart, size);
	return firstBlock;
}

template<typename T>
void BlockCirclebuf<T>::allocateSuperblock(const size_t size, Block *prev,
					   Block *next)
{
	Block *firstBlock = bmalloc(sizeof(Block));
	if (!firstBlock)
		throw std::bad_alloc();
	T *allocation = (T *)bmalloc(size * sizeof(T));
	if (!allocation)
		throw std::bad_alloc();
	superblockAllocations.push_back(SuperblockAllocation(allocation));
	SuperblockAllocation &alloc = superblockAllocations.back();
	&firstBlock = Block(*alloc, alloc.allocationStart, size, prev, next);
	prev->next = firstBlock;
	next->prev = firstBlock;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(
	const BlockCirclebuf<T> &parentContainer,
	const SuperblockAllocation *const parentSuperblock, T *blockStart,
	size_t blockLength, BlockCirclebuf<T>::Block *next) noexcept
{
	this->parentSuperblock = parentSuperblock;
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
			this->tailPassedYet = next->tailPassedYet;
			next->tailPassedYet = true;
		} else {
			if (tail.getBlock() != head.getBlock() ||
			    tail.getPtr() > head.getPtr()) {
				this->tailPassedYet = false;
				next->tailPassedYet = false;
			} else {
				this->tailPassedYet = next->tailPassedYet;
				next->tailPassedYet = true;
			}
		}
	}

	this->willReconcileNext = next->prev->willReconcileNext;
	next->prev->willReconcileNext = false;
	this->willReconcilePrev = false;
	referencingPtrs = nullptr;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(
	const BlockCirclebuf<T> &parentContainer,
	const SuperblockAllocation *const parentSuperblock, T *blockStart,
	size_t blockLength) noexcept
	: Block(parentContainer, parentSuperblock, blockStart, blockLength,
		this)
{
}

template<typename T>
void BlockCirclebuf<T>::Block::split(T *splitPoint,
				     const BlockCirclebuf<T> &circlebuf)
{
	if (splitPoint < blockStart || splitPoint > blockStart + blockLength)
		throw std::out_of_range(
			"Tried to split a BlockCirclebuf block at an out-of-range splitPoint");

	void *newBlockSpace = bmalloc(sizeof(Block));
	if (!newBlockSpace)
		throw std::bad_alloc();
	Block *newBlock = new (newBlockSpace)
		Block(parentSuperblock, splitPoint,
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
			if (circlebuf.tail.getPtr() >= splitPoint) {
				if (circlebuf.head.getPtr() >= splitPoint) {
					newBlock->tailPassedYet =
						circlebuf.head.getPtr() >=
						circlebuf.tail.getPtr();
				} else {
					newBlock->tailPassedYet = true;
				}
			} else {
				if (circlebuf.head.getPtr() >= splitPoint) {
					newBlock->tailPassedYet = false;
				} else {
					newBlock->tailPassedYet =
						circlebuf.head.getPtr() >=
						circlebuf.tail.getPtr();
				}
			}
		} else {
			newBlock->tailPassedYet = circlebuf.tail.getPtr() <
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
			currentBCPtr->next->prev = currentBCPtr->prev;
		if (currentBCPtr->prev)
			currentBCPtr->prev->next = currentBCPtr->next;

		currentBCPtr->block = newBlock;
		currentBCPtr->next = newBlock->referencingPtrs;
		if (newBlock->referencingPtrs)
			newBlock->referencingPtrs->prev = newBlock;
		newBlock->referencingPtrs = newBlock;

		currentBCPtr = nextBCPtr;
	}
	if (newBlock->referencingPtrs)
		newBlock->referencingPtrs->prev = nullptr;
}

template<typename T>
void BlockCirclebuf<T>::Block::split(const BCPtr &splitPoint)
{
	if (splitPoint.block != this)
		throw std::runtime_error(
			"BCPtr provided to split a block referenced a different block");
	split(splitPoint.ptr);
}

template<typename T> BlockCirclebuf<T>::BlockCirclebuf(size_t size)
{
	Block *firstBlock;
	firstBlock = allocateSuperblock(size);
	head = BCPtr(firstBlock, firstBlock->getStartPtr());
	tail = BCPtr(firstBlock, firstBlock->getStartPtr());
}

template<typename T>
size_t BlockCirclebuf<T>::ptrDifference(const BCPtr &a,
					const BCPtr &b) const noexcept
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
		currentPosn.getBlock() = currentPosn.getBlock()->getNext();
	}

	accumulator += b.getPtr() - a.getPtr();
	return accumulator;
}

template<typename T> size_t BlockCirclebuf<T>::bufferHealth() const noexcept
{
	return ptrDifference(tail, head);
}

template<typename T> size_t BlockCirclebuf<T>::Block::getLength() const noexcept
{
	return blockLength;
}

template<typename T> T *BlockCirclebuf<T>::Block::getStartPtr() const noexcept
{
	return blockStart;
}

template<typename T>
typename BlockCirclebuf<T>::Block *
BlockCirclebuf<T>::Block::getNext() const noexcept
{
	return next;
}

template<typename T>
typename BlockCirclebuf<T>::Block *
BlockCirclebuf<T>::Block::getPrev() const noexcept
{
	return prev;
}

template<typename T>
void BlockCirclebuf<T>::write(const T *const input,
			      const size_t count) const noexcept
{
	assert(input != nullptr);
	size_t numRead = 0;
	while (numRead < count) {
		size_t numToRead = count - numRead;
		if (tail.getBlock() == head.getBlock() &&
		    tail.getPtr() - head.getPtr() < numToRead) {
			this->advanceTailToNextBlock();
		}
		size_t spaceLeftInBlock = head.getBlock()->getStartPtr() +
					  head.getBlock()->getLength() -
					  head.getPtr();
		if (numToRead < spaceLeftInBlock) {
			memcpy(input, head.getPtr(), numToRead);
			head.move(head.getBlock(), head.getPtr() + numToRead);
			return;
		} else {
			memcpy(input, head.getPtr(), spaceLeftInBlock);
			numRead += spaceLeftInBlock;
			this->advanceHeadToNextBlock();
		}
	}
}

template<typename T> bool BlockCirclebuf<T>::Block::attemptReconcileNext()
{
	return next->attemptReconcilePrev();
}

template<typename T> bool BlockCirclebuf<T>::Block::attemptReconcilePrev()
{
	//fail if reconciling with self
	if (prev == this)
		return false;

	//fail if not in same superblock
	if (prev->parentSuperblock != this->parentSuperblock)
		return false;

	//fail if not physically adjacent
	if (prev->blockStart + prev->blockLength != this->blockStart)
		return false;

	prev->blockLength = prev->blockLength + this->blockLength;
	prev->next = this->next;
	this->next->prev = prev;
	prev->willReconcileNext = false;

	//update all BCPtrs to point to newly reconciled block.
	BCPtr *currentBCPtr;
	while (this->referencingPtrs) {
		currentBCPtr = this->referencingPtrs;
		currentBCPtr->block = prev;

		if (prev->referencingPtrs)
			prev->referencingPtrs->prev = currentBCPtr;
		this->referencingPtrs = currentBCPtr->next;
		currentBCPtr->next = prev->referencingPtrs;

		prev->referencingPtrs = currentBCPtr;
	}

	if (prev->referencingPtrs)
		prev->referencingPtrs->prev = nullptr;

	this->~Block();
	bfree(this);
	return true;
}

template<typename T>
void BlockCirclebuf<T>::Block::setTailPassedYet(
	const bool tailPassedYet) noexcept
{
	this->tailPassedYet = tailPassedYet;
}

template<typename T>
BlockCirclebuf<T>::BCPtr::BCPtr(Block *block, T *ptr) noexcept
{
	if (ptr < block->getStartPtr() ||
	    ptr >= block->getStartPtr() + block->getLength())
		throw std::out_of_range(
			"Initialising a BCPtr out of range of the provided block");

	this->block = block;
	this->ptr = ptr;
	this->prev = nullptr;
	this->next = block->referencingPtrs;
	block->referencingPtrs->prev = this;
	block->referencingPtrs = this;
}

template<typename T>
BlockCirclebuf<T>::BCPtr::BCPtr(const BCPtr &copy) noexcept
	: BCPtr(copy.block, copy.ptr)
{
}

template<typename T> BlockCirclebuf<T>::BCPtr::~BCPtr() noexcept
{
	if (prev)
		prev->next = next;
	if (next)
		next->prev = prev;
	if (block->referencingPtrs == this)
		block->referencingPtrs = next;
}

template<typename T>
typename BlockCirclebuf<T>::BCPtr &
BlockCirclebuf<T>::BCPtr::operator=(const BCPtr &other) noexcept
{
	if (other.block != this->block) {
		if (prev)
			prev->next = next;
		if (next)
			next->prev = prev;
		if (block->referencingPtrs == this)
			block->referencingPtrs = next;

		this->prev = nullptr;
		this->next = block->referencingPtrs;
		if (other.block->referencingPtrs)
			other.block->referencingPtrs->prev = this;
		other.block->referencingPtrs = this;
		this->block = other.block;
	}

	this->ptr = other->ptr;

	return *this;
}

template<typename T>
typename BlockCirclebuf<T>::Block *
BlockCirclebuf<T>::BCPtr::getBlock() const noexcept
{
	return this->block;
}

template<typename T> T *BlockCirclebuf<T>::BCPtr::getPtr() const noexcept
{
	return this->ptr;
}

template<typename T>
void BlockCirclebuf<T>::BCPtr::move(Block *newBlock, T *newPos) noexcept(NDEBUG)
{
	assert(newPos >= newBlock->getStartPtr());
	assert(newPos < newBlock->getStartPtr() + newBlock->getLength());
	if (this->prev == nullptr) {
		this->getBlock()->referencingPtrs = this->next;
	} else {
		this->prev->next = this->next;
	}
	if (this->next != nullptr) {
		this->next->prev = this->prev;
	}

	this->prev = nullptr;
	this->next = newBlock->referencingPtrs;
	if (this->next != nullptr) {
		this->next->prev = this;
	}
	newBlock->referencingPtrs = this;
	this->ptr = newPos;
}

template<typename T>
BlockCirclebuf<T>::SuperblockAllocation::SuperblockAllocation(
	T *const allocationStart) noexcept
{
	this->allocationStart = allocationStart;
}

template<typename T>
size_t BlockCirclebuf<T>::read(T *buffer, const size_t count) const noexcept
{
	auto memcpyIfNotNull = [](T *src, T *target, size_t count) {
		if (target != nullptr) {
			memcpy(src, target, count);
		}
	};
	size_t numRead = 0;
	while (numRead < count) {
		size_t numToRead = count - numRead;
		if (head.getBlock() == tail.getBlock()) {
			if (head.getPtr() - tail.getPtr() < numToRead) {
				memcpyIfNotNull(tail.getPtr(), buffer + numRead,
						head.getPtr() - tail.getPtr());
				numRead += head.getPtr() - tail.getPtr();
				tail.move(tail.getBlock(), head.getPtr());
				return numRead;
			} else {
				memcpyIfNotNull(tail.getPtr(), buffer + numRead,
						numToRead);
				tail.move(tail.getBlock(),
					  tail.getPtr() + numToRead);
				return count;
			}
		}
		size_t spaceLeftInBlock = tail.getBlock().getStartPtr() +
					  tail.getBlock().getLength() -
					  tail.getPtr();
		if (numToRead < spaceLeftInBlock) {
			memcpyIfNotNull(tail.getPtr(), buffer + numRead,
					numToRead);
			tail.move(tail.getBlock(), tail.getPtr() + numToRead);
			return count;
		} else {
			memcpyIfNotNull(tail.getPtr(), buffer + numRead,
					spaceLeftInBlock);
			this->advanceTailToNextBlock();
		}
	}
	return numRead;
}

template<typename T> void BlockCirclebuf<T>::advanceTailToNextBlock()
{
	Block *nextBlock;
	if (tail.getBlock()->getLogicalNext()->getPrev() == tail.getBlock()) {
		nextBlock = tail.getBlock()->getLogicalNext();
	} else {
		nextBlock = tail.getBlock()->getNext();
	}
	nextBlock->setTailPassedYet(true);
}

template<typename T> void BlockCirclebuf<T>::advanceHeadToNextBlock()
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
}
