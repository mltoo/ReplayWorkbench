#pragma once
#include "blockCirclebuf.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <util/base.h>
#include <util/bmem.h>
using namespace ReplayWorkbench;

template<typename T>
typename BlockCirclebuf<T>::Block *
BlockCirclebuf<T>::allocateSuperblock(size_t size)
{
	Block *firstBlock = bmalloc(sizeof(Block));
	superblockAllocations.push_back({(T *)bmalloc(size * sizeof(T))});
	SuperblockAllocation &alloc = superblockAllocations.back();
	*firstBlock = Block(*alloc, alloc.allocationStart, size);
	return firstBlock;
}

template<typename T>
void BlockCirclebuf<T>::allocateSuperblock(size_t size, Block *prev,
					   Block *next)
{
	Block *firstBlock = bmalloc(sizeof(Block));
	superblockAllocations.push_back(
		{.allocationStart = (T *)bmalloc(size * sizeof(T))});
	SuperblockAllocation &alloc = superblockAllocations.back();
	&firstBlock = Block(*alloc, alloc.allocationStart, size, prev, next);
	prev->next = firstBlock;
	next->prev = firstBlock;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(SuperblockAllocation *parentSuperblock,
				T *blockStart, size_t blockLength,
				BlockCirclebuf<T>::Block *prev,
				BlockCirclebuf<T>::Block *next)
{
	this->parentSuperblock = parentSuperblock;
	this->blockStart = blockStart;
	this->blockLength = blockLength;
	this->next = next;
	next->prev = this;
	this->prev = prev;
	prev->next = this;
	this->writeProtect = false;
	this->readProtect = false;
	this->willReconcileNext = false;
	this->willReconcilePrev = false;
	referencingPtrs = NULL;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(SuperblockAllocation *parentSuperblock,
				T *blockStart, size_t blockLength)
	: Block(parentSuperblock, blockStart, blockLength, this, this)
{
}

template<typename T> void BlockCirclebuf<T>::Block::split(T *splitPoint)
{
	Block *newBlock = (Block *)bmalloc(sizeof(Block));
	if (splitPoint < blockStart || splitPoint > blockStart + blockLength)
		throw std::out_of_range(
			"Tried to split a BlockCirclebuf block at an out-of-range splitPoint");
	//if ((splitPoint - blockStart) % sizeof(T) != 0)
	//	throw std::runtime_error(
	//		"Tried to split a BlockCirclebuf at a misaligned location");

	&newBlock = (parentSuperblock, splitPoint,
		     blockLength - (splitPoint - blockStart), this, next);
	newBlock->writeProtect = writeProtect;
	newBlock->readProtect = readProtect;
	blockLength = blockLength - newBlock->blockLength;
	next->prev = newBlock;
	next = newBlock;

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

template<typename T> void BlockCirclebuf<T>::Block::split(BCPtr &splitPoint)
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

template<typename T> size_t BlockCirclebuf<T>::ptrDifference(BCPtr &a, BCPtr &b)
{
	size_t accumulator = 0;
	BCPtr currentPosn(a);
	while (currentPosn.block != b.block || currentPosn.ptr < b.ptr) {
		accumulator +=
			(currentPosn.block->getLength()) -
			(currentPosn.ptr - currentPosn.block.getStartPtr());
		currentPosn.ptr = currentPosn.block->getNext().getStartPtr();
		currentPosn.block = currentPosn.block->getNext();
	}

	accumulator += b.ptr - a.ptr;
	return accumulator;
}

template<typename T> size_t BlockCirclebuf<T>::bufferHealth()
{
	return ptrDifference(tail, head);
}

template<typename T> size_t BlockCirclebuf<T>::Block::getLength()
{
	return blockLength;
}

template<typename T> T *BlockCirclebuf<T>::Block::getStartPtr()
{
	return blockStart;
}

template<typename T>
typename BlockCirclebuf<T>::Block *BlockCirclebuf<T>::Block::getNext()
{
	return next;
}

template<typename T>
typename BlockCirclebuf<T>::Block *BlockCirclebuf<T>::Block::getPrev()
{
	return prev;
}

template<typename T> void BlockCirclebuf<T>::write(T *input, size_t count)
{
	size_t numRead = 0;
	while (numRead < count) {
		size_t numInCurrentBlock = std::min(
			count - numRead,
			head.block->getLength() -
				(head.ptr - head.block->getStartPtr()));

		//TODO: handle read protection
		if (tail.block == head.block &&
		    tail.ptr <= head.ptr + numInCurrentBlock) {
			tail.ptr = head.ptr + numInCurrentBlock;
			if (tail.ptr >= tail.block->getStartPtr() +
						tail.block->getLength()) {
				do {
					tail.block = tail.block->getNext();
					tail.ptr = tail.block->getStartPtr();
				} while (tail.block->readProtect);
			}
		}

		memcpy(head.ptr, input + numRead, numInCurrentBlock);
		numRead += numInCurrentBlock;
		head.ptr += numInCurrentBlock;
		if (head.ptr >=
		    head.block->getStartPtr() + head.block->getLength()) {
			do {
				head.block = head.block->getNext();
				head.ptr = head.block->getStartPtr();
				head.block.readProtect =
					head.block.writeProtect;
				if (!head.block.readProtect) {
					if (head.block->willReconcilePrev)
						head.block
							->attemptReconcilePrev();

					if (head.block->willReconcileNext)
						head.block
							->attemptReconcileNext();
				}

			} while (head.block->writeProtect);
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

	//defer until read protection ends if either is read protected.
	//(avoid merging in corrupted data/excluding good data)
	if (this->readProtect || prev->readProtect) {
		this->willReconcilePrev = true;
		prev->willReconcileNext = true;
		return false;
	}

	//perform reconciliation:
	prev->writeProtect = this->writeProtect || prev->writeProtect;
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

	bfree(this);
	return true;
}

template<typename T> BlockCirclebuf<T>::BCPtr::BCPtr(Block *block, T *ptr)
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
BlockCirclebuf<T>::BCPtr::BCPtr(BCPtr &copy) : BCPtr(copy.block, copy.ptr)
{
}

template<typename T> BlockCirclebuf<T>::BCPtr::~BCPtr()
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
BlockCirclebuf<T>::BCPtr::operator=(const BCPtr &other)
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
