#include <cstdio>
#include <gtest/gtest.h>
#include <iostream>
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

