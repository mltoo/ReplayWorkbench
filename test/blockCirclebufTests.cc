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
	BlockCirclebuf<char>::BCPtr protectStart{
		cb.getHead().getBlock(), cb.getHead().getBlock()->getStartPtr() + 1};
	auto protectedSection{cb.protect(protectStart, 2)};
	cb.write("5678", 4);
	EXPECT_EQ(*(protectStart.getPtr()), '2');
	EXPECT_EQ(*(protectStart.getPtr() + 1), '3');
	EXPECT_NE(cb.getTail().getPtr(), protectStart.getPtr());
}

TEST(BlockCirclebufTests, ProtectOverTip)
{
	BlockCirclebuf<char> cb{10};
	cb.write("12", 2);
	BlockCirclebuf<char>::BCPtr protectStart{cb.getHead().getBlock(),
											 cb.getHead().getPtr()};
	cb.write("34", 2);
	auto protectedSection{cb.protect(protectStart, 4)};
	cb.write("AB34567890", 10);
	EXPECT_EQ(*(protectStart.getPtr()), '3');
	EXPECT_EQ(*(protectStart.getPtr() + 1), '4');
	EXPECT_EQ(*(protectStart.getPtr() + 2), 'A');
	EXPECT_EQ(*(protectStart.getPtr() + 3), 'B');
	std::array<char, 10U> readResult{};
	EXPECT_EQ(cb.read(readResult.data(), 10U), 6U);
	EXPECT_STREQ(readResult.data(), "567890");
	EXPECT_EQ(cb.read(readResult.data(), 10U), 0U);
	EXPECT_STREQ(readResult.data(), "567890");
	cb.write("CDEFGHIJKL", 10U);
	EXPECT_EQ(cb.read(readResult.data(), 10U), 6U);
	EXPECT_STREQ(readResult.data(), "GHIJKL");
	EXPECT_EQ(cb.read(readResult.data(), 10U), 0U);
	EXPECT_STREQ(readResult.data(), "GHIJKL");
}

TEST(BlockCirclebufTests, ReadProtectedOnce)
{
	BlockCirclebuf<char> cb{10U};
	cb.write("12", 2U);
	BlockCirclebuf<char>::BCPtr protectStart{cb.getHead().getBlock(),
											 cb.getHead().getPtr()};
	cb.write("34", 2U);
	auto protectedSection{cb.protect(protectStart, 4U)};
	cb.write("AB", 2U);
	std::array<char, 10U> readResult{};
	EXPECT_EQ(cb.read(readResult.data(), 10U), 6U);
	EXPECT_STREQ(readResult.data(), "1234AB");
	EXPECT_EQ(cb.read(readResult.data(), 10U), 0U);
	EXPECT_STREQ(readResult.data(), "1234AB");
	cb.write("34567890", 8U);
	EXPECT_EQ(*(protectStart.getPtr()), '3');
	EXPECT_EQ(*(protectStart.getPtr() + 1), '4');
	EXPECT_EQ(*(protectStart.getPtr() + 2), 'A');
	EXPECT_EQ(*(protectStart.getPtr() + 3), 'B');
	EXPECT_EQ(cb.read(readResult.data(), 10U), 6U);
	EXPECT_STREQ(readResult.data(), "567890");
	EXPECT_EQ(cb.read(readResult.data(), 10U), 0U);
	EXPECT_STREQ(readResult.data(), "567890");
}

TEST(BlockCirclebufTests, OverlappingPS)
{
	BlockCirclebuf<char> cb{20U};
	cb.write("ABCD EFGH IJKL MNOP ", 20U);
	BlockCirclebuf<char>::BCPtr firstProtectStart{cb.getTail().getBlock(),
												  cb.getTail().getPtr() + 5U};
	BlockCirclebuf<char>::BCPtr secondProtectStart{cb.getTail().getBlock(),
												   cb.getTail().getPtr() + 7U};
	cb.protect(firstProtectStart, 10U);
	cb.protect(secondProtectStart, 10U);

	std::array<char, 21U> readResult{};
	EXPECT_EQ(cb.read(readResult.data(), 20U), 20U);
	EXPECT_STREQ(readResult.data(), "ABCD EFGH IJKL MNOP ");

	cb.write("12345678901234567890", 20U);

	std::string_view firstExpected{"EFGH IJKL "};
	std::size_t numRead{0U};
	auto currentBlock{firstProtectStart.getBlock()};
	while (numRead < 10U) {
		for (std::size_t i = 0U; i < currentBlock->getLength(); ++i) {
			EXPECT_EQ((*(currentBlock->getStartPtr() + i)),
					  (firstExpected.at(numRead++)));
		}
		currentBlock = currentBlock->getLogicalNext();
	}

	std::string_view secondExpected{"GH IJKL MN"};
	currentBlock = secondProtectStart.getBlock();
	numRead = 0U;
	while (numRead < 10U) {
		for (std::size_t i = 0U; i < currentBlock->getLength(); ++i) {
			EXPECT_EQ((*(currentBlock->getStartPtr() + i)),
					  (secondExpected.at(numRead++)));
		}
		currentBlock = currentBlock->getLogicalNext();
	}

	std::fill(readResult.begin(), readResult.end(), 0U);
	EXPECT_EQ(cb.read(readResult.data(), 20U), 8U);
	EXPECT_STREQ(readResult.data(), "34567890");
}

TEST(BlockCirclebufTests, ReverseOverlappingPS)
{
	BlockCirclebuf<char> cb{20U};
	cb.write("ABCD EFGH IJKL MNOP ", 20U);
	BlockCirclebuf<char>::BCPtr firstProtectStart{cb.getTail().getBlock(),
												  cb.getTail().getPtr() + 7U};
	BlockCirclebuf<char>::BCPtr secondProtectStart{cb.getTail().getBlock(),
												   cb.getTail().getPtr() + 5U};
	cb.protect(firstProtectStart, 10U);
	cb.protect(secondProtectStart, 10U);

	std::array<char, 21U> readResult{};
	EXPECT_EQ(cb.read(readResult.data(), 20U), 20U);
	EXPECT_STREQ(readResult.data(), "ABCD EFGH IJKL MNOP ");

	cb.write("12345678901234567890", 20U);

	std::size_t numRead{0U};
	std::string_view firstExpected{"GH IJKL MN"};
	auto currentBlock{firstProtectStart.getBlock()};
	while (numRead < 10U) {
		for (std::size_t i = 0U; i < currentBlock->getLength(); ++i) {
			EXPECT_EQ((*(currentBlock->getStartPtr() + i)),
					  (firstExpected.at(numRead++)));
		}
		currentBlock = currentBlock->getLogicalNext();
	}

	std::string_view secondExpected{"EFGH IJKL "};
	currentBlock = secondProtectStart.getBlock();
	numRead = 0U;
	while (numRead < 10U) {
		for (std::size_t i = 0U; i < currentBlock->getLength(); ++i) {
			EXPECT_EQ((*(currentBlock->getStartPtr() + i)),
					  (secondExpected.at(numRead++)));
		}
		currentBlock = currentBlock->getLogicalNext();
	}

	std::fill(readResult.begin(), readResult.end(), 0U);
	EXPECT_EQ(cb.read(readResult.data(), 20U), 8U);
	EXPECT_STREQ(readResult.data(), "34567890");
}

