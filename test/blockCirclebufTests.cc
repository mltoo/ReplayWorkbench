#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include "blockCirclebuf.hpp"
using namespace ReplayWorkbench;

TEST(BlockCirclebufTests, SimpleWriteRead)
{
	BlockCirclebuf<char> cb{128};
	const char *input{"test123"};
	cb.write(input, 8);
	char output[128]{'A'};
	cb.read(&output[0], 8);
	EXPECT_STREQ(output, input) << "input data and output data differ";
}

TEST(BlockCirclebufTests, BlockedRead)
{
	BlockCirclebuf<char> cb{128};
	char output[128]{'A'};
	const char *testStr{"DEADBEEF"};
	strcpy(&output[0], testStr);
	size_t readCount{cb.read(&output[0], 1)};
	EXPECT_EQ(readCount, 0);
	EXPECT_STREQ(output, testStr);
	cb = BlockCirclebuf<char>(128);
	for (size_t i{0U}; i < sizeof(output) / sizeof(char); i++) {
		output[i] = 'A';
	}
	cb.write(testStr, 1);
	readCount = cb.read(&output[0], 2);
	EXPECT_EQ(readCount, 1);
	EXPECT_EQ(output[0], 'D');
	EXPECT_EQ(output[1], 'A');
}

TEST(BlockCirclebufTests, WriteWraparound)
{
	BlockCirclebuf<char> cb{2};
	char output[128]{'A'};
	const char *testStr{"1234"};
	cb.write(testStr, 3);
	cb.read(&output[0], 1);
	EXPECT_EQ(output[0], '2');
}

TEST(BlockCirclebufTests, BlockSplit)
{
	BlockCirclebuf<char> cb{4};
	BlockCirclebuf<char>::BCPtr splitPtr{cb.getHead()};
	char output[128]{'A'};
	splitPtr.move(splitPtr.getBlock(), splitPtr.getPtr() + 2);
	splitPtr.getBlock()->split(splitPtr, cb);
	const char *testStr{"TEST"};
	cb.write(testStr, 4);
	size_t readCount{cb.read(&output[0], 4)};
	EXPECT_EQ(readCount, 4);
	EXPECT_STREQ(&output[0], testStr);
}

TEST(BlockCirclebufTests, Protect)
{
	BlockCirclebuf<char> cb{4};
	cb.write("1234", 4);
	BlockCirclebuf<char>::BCPtr protectStart{cb.getHead().getBlock(), cb.getHead().getBlock()->getStartPtr() + 1};
	cb.protect(protectStart, 2);
	cb.write("5678", 4);
	EXPECT_EQ(*(protectStart.getPtr()), '2');
	EXPECT_EQ(*(protectStart.getPtr() + 1), '3');
	EXPECT_NE(cb.getTail().getPtr(), protectStart.getPtr());

}
