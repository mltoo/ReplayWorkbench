#pragma once
#include "blockCirclebuf.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <util/base.h>
#include <util/bmem.h>
using namespace ReplayWorkbench;

template<typename T>
void BlockCirclebuf<T>::allocateSuperblock(size_t size, Block *firstBlock)
{
	superblockAllocations.push_back(
		{.allocationStart = (T *)bmalloc(size * sizeof(T))});
	SuperblockAllocation &alloc = superblockAllocations.back();
	&firstBlock = Block(*alloc, alloc.allocationStart, size);
}

template<typename T>
void BlockCirclebuf<T>::allocateSuperblock(size_t size, Block *firstBlock,
					   Block *prev, Block *next)
{
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
	this->prev = prev;
}

template<typename T>
BlockCirclebuf<T>::Block::Block(SuperblockAllocation *parentSuperblock,
				T *blockStart, size_t blockLength)
	: Block(parentSuperblock, blockStart, blockLength, this, this)
{
}

template<typename T>
void BlockCirclebuf<T>::Block::split(T *splitPoint,
				     BlockCirclebuf<T>::Block *newBlock)
{
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
}

template<typename T>
BlockCirclebuf<T>::BlockCirclebuf(size_t size, Block *firstBlock)
{
	&firstBlock = allocateSuperblock(size);
	head = BCPtr{.block = firstBlock, .ptr = firstBlock->getStartPtr()};
	tail = BCPtr{.block = firstBlock, .ptr = firstBlock->getStartPtr()};
}

template<typename T> size_t BlockCirclebuf<T>::ptrDifference(BCPtr a, BCPtr b)
{
	size_t accumulator = 0;
	BCPtr currentPosn = a;
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
			} while (head.block->writeProtect);
		}
	}
}
