#include "blockCirclebuf.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <util/base.h>
#include <util/bmem.h>
#include <util/c99defs.h>
#include <new>

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
	const SuperblockAllocation *const parentSuperblock, T *blockStart,
	size_t blockLength, BlockCirclebuf<T>::Block *next) noexcept
{
	this->parentSuperblock = parentSuperblock;
	this->blockStart = blockStart;
	this->blockLength = blockLength;
	this->tailPassedYet = true;
	this->tailDirection = false;
	if (next != this) {
		this->next = next;
		this->prev = next->prev;
		next->prev = this;
		if (next == this->prev->logicalNext) {
			this->logicalNext = this->prev->logicalNext;
			this->prev->logicalNext = this;
		} else {
			this->logicalNext = this->prev->logicalNext;
			this->prev->logicalNext = this;
			this->tailDirection = this->prev->tailDirection;
			this->prev->tailDirection = false;
		}
		this->tailPassedYet = next->tailPassedYet;

		this->prev->next = this;

	} else {
		this->next = this;
		this->prev = this;
		this->logicalNext = this;
		this->tailPassedYet = true;
		this->tailDirection = false;
	}
	this->willReconcileNext = false;
	this->willReconcilePrev = false;
	referencingPtrs = NULL;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(
	const SuperblockAllocation *const parentSuperblock, T *blockStart,
	size_t blockLength) noexcept
	: Block(parentSuperblock, blockStart, blockLength, this)
{
}

template<typename T> void BlockCirclebuf<T>::Block::split(T *splitPoint)
{

	if (splitPoint < blockStart || splitPoint > blockStart + blockLength)
		throw std::out_of_range(
			"Tried to split a BlockCirclebuf block at an out-of-range splitPoint");

	void *newBlockSpace = bmalloc(sizeof(Block));
	Block *newBlock = new (newBlockSpace)
		Block(parentSuperblock, splitPoint,
		      blockLength - (splitPoint - blockStart), this,
		      this->next);
	blockLength = blockLength - newBlock->blockLength;
	next->prev = newBlock;
	newBlock->next = this->next;
	this->next = newBlock;
	next->logicalNext = this->logicalNext;
	this->logicalNext = next;
	next->tailDirection = this->tailDirection;
	next->tailPassedYet = true;

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
		newBlock->referencingPtrs->prev = NULL;
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
size_t BlockCirclebuf<T>::ptrDifference(const BCPtr &a, const BCPtr &b) const noexcept
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

template<typename T> void BlockCirclebuf<T>::write(T *input, size_t count)
{
	size_t numRead = 0;
	while (numRead < count) {
		//Get available space in current block
		size_t numInCurrentBlock = std::min(
			count - numRead,
			head.block->getLength() -
				(head.ptr - head.block->getStartPtr()));

		//Handle cases where we overwrite the tail (advance tail past
		//it)
		if (tail.block == head.block && tail.ptr > head.ptr &&
		    tail.ptr <= head.ptr + numInCurrentBlock)
			advanceTail((head.ptr + numInCurrentBlock) - tail.ptr);

		memcpy(head.ptr, input + numRead,
		       numInCurrentBlock * sizeof(T));
		numRead += numInCurrentBlock;
		head.ptr += numInCurrentBlock;
		advanceHead(numInCurrentBlock);
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
		prev->referencingPtrs->prev = NULL;

	this->~Block();
	bfree(this);
	return true;
}

template<typename T>
bool BlockCirclebuf<T>::Block::getTailDirection() const noexcept
{
	return tailDirection;
}

template<typename T> void BlockCirclebuf<T>::Block::flipTailDirection() noexcept
{
	tailDirection = !tailDirection;
}

template<typename T>
bool BlockCirclebuf<T>::Block::getTailPassedYet() const noexcept
{
	return tailPassedYet;
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
	this->prev = NULL;
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

		this->prev = NULL;
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
void BlockCirclebuf<T>::BCPtr::move(Block *newBlock, T *newPos) noexcept
{
	UNUSED_PARAMETER(newBlock);
	UNUSED_PARAMETER(newPos);
	//TODO
}

template<typename T>
BlockCirclebuf<T>::SuperblockAllocation::SuperblockAllocation(
	T *const allocationStart) noexcept
{
	this->allocationStart = allocationStart;
}

template<typename T> void BlockCirclebuf<T>::advanceHead(size_t size)
{

	//TODO
	if (!bufferHealth() > size) {
		advanceTail(size);
	}
}

template<typename T> void BlockCirclebuf<T>::advanceTail(size_t size)
{
	size_t spaceInCurrentBlock;
	while (size > 0) {
		Block *currentBlock = tail.getBlock();
		spaceInCurrentBlock = (currentBlock->getStartPtr() +
				       currentBlock->getLength()) -
				      tail.getPtr();
		if (spaceInCurrentBlock < size) {
			if (head.getBlock() == currentBlock) {
				advanceHead((currentBlock->getStartPtr() +
					     currentBlock->getLength()) -
					    head->getPtr() + 1);
			}
			size -= spaceInCurrentBlock;
			if (currentBlock->getTailDirection()) {
				currentBlock->flipTailDirection();
				currentBlock = currentBlock->getNext();
			} else {
				currentBlock = currentBlock->getLogicalNext();
			}
			currentBlock->setTailPassedYet(true);
		} else {
			T *targetPtr;
			if (tail.getBlock() == currentBlock)
				targetPtr = tail.getPtr() + size;
			else
				targetPtr = currentBlock->getStartPtr() + size;

			if (head.getBlock() == currentBlock &&
			    head->getPtr() < targetPtr)
				advanceHead(targetPtr - head->getPtr() + 1);

			tail.move(currentBlock, targetPtr);
		}
	}
}

template<typename T> void BlockCirclebuf<T>::advanceTailToNextBlock()
{
	advanceTail((tail->getBlock() + tail.getBlock()->getLength()) -
		    tail->getPtr());
}

template<typename T> void BlockCirclebuf<T>::reserveNonContiguous(size_t n)
{
	size_t alreadyReserved = ptrDifference(head, tail);
	if (alreadyReserved < n)
		advanceTail(n - alreadyReserved);
}

template<typename T>
typename BlockCirclebuf<T>::BCPtr BlockCirclebuf<T>::reserveContiguous(size_t n)
{
	Block *block = head->getBlock();
	if ((block->getStartPtr() + block->getLength()) - head->getPtr() >= n) {
		if (tail->getBlock() == head->getBlock()) {
			size_t alreadyReserved = ptrDifference(head, tail);
			if (alreadyReserved < n)
				advanceTail(ptrDifference(head, tail) - n);
		}
		return head;
	} else {
		if (tail->getBlock() == block &&
		    tail->getPtr() > head->getPtr())
			advanceTailToNextBlock();

		block = block->getNext();
		while ((block->getLength() < n) && block != head->getBlock()) {
			if (tail->getBlock() == block)
				advanceTailToNextBlock();
			block = block->getNext();
		}
		if (block == head->getBlock()) {
			if (block->getLength() >= n) {
				advanceTail(ptrDifference(tail, head));
			} else {
				throw std::runtime_error(
					"contiguous write too large for any "
					"block in BlockCirclebuf");
			}
		} else {
			if (tail->getBlock() == block) {
				size_t alreadyReserved =
					tail.getPtr() - block->getStartPtr();
				advanceTail(n - alreadyReserved);
			}
		}
		return BCPtr(block, block->getStartPtr());
	}
}
}
