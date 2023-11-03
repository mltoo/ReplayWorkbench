#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <ostream>
#include "blockCirclebuf.hpp"
using namespace ReplayWorkbench;

TEST(BlockCirclebufTests, SimpleWriteRead) {
	BlockCirclebuf<char> cb = BlockCirclebuf<char>(128);
	const char *input = "test123";	
	cb.write(input, 8);
	char output[128] = {'A'};
	cb.read(&output[0], 8);
	EXPECT_STREQ(output, input) << "input data and output data differ";
}

TEST(BlockCirclebufTests, BlockedRead) {
	BlockCirclebuf<char> cb = BlockCirclebuf<char>(128);
	char output[128] = {'A'};
	const char *testStr = "DEADBEEF";
	strcpy(&output[0], testStr);
	size_t readCount = cb.read(&output[0], 1);
	EXPECT_EQ(readCount, 0);
	EXPECT_STREQ(output, testStr);
	cb = BlockCirclebuf<char>(128);
	for (int i = 0; i < sizeof(output)/sizeof(char); i++) {
		output[i] = 'A';
	}
	cb.write(testStr, 1);
	readCount = cb.read(&output[0], 2);
	EXPECT_EQ(readCount, 1);
	EXPECT_EQ(output[0], 'D');
	EXPECT_EQ(output[1], 'A');
}
