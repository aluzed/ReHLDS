#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

namespace
{
	int Classify(const char *data, int size, uint32 &challenge)
	{
		return A2S_ClassifyQuery((const unsigned char *)data, size, challenge);
	}

	int Classify(const char *data, int size)
	{
		uint32 challenge;
		return Classify(data, size, challenge);
	}

	bool IsAnswer(int queryType, const char *data, int size)
	{
		return A2S_IsAnswer(queryType, (const unsigned char *)data, size);
	}
}

TEST(ClassifyQueries, A2SCache, 1000)
{
	uint32 challenge;

	// sizeof includes the terminating zero, which is part of the query
	const char info[] = "\xFF\xFF\xFF\xFFTSource Engine Query";
	LONGS_EQUAL("A2S_INFO", A2SQ_INFO, Classify(info, sizeof(info), challenge));
	CHECK("A2S_INFO has no challenge", challenge == A2S_NO_CHALLENGE);

	const char infoChallenge[] = "\xFF\xFF\xFF\xFFTSource Engine Query\0\x01\x02\x03\x04";
	LONGS_EQUAL("A2S_INFO with a challenge", A2SQ_INFO, Classify(infoChallenge, sizeof(infoChallenge) - 1, challenge));
	CHECK("A2S_INFO challenge", challenge == 0x04030201);

	LONGS_EQUAL("A2S_INFO without its terminating zero", A2SQ_NONE, Classify(info, sizeof(info) - 1));
	LONGS_EQUAL("Other 'T' query", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFFTxyz", 9));

	LONGS_EQUAL("A2S_PLAYER", A2SQ_PLAYER, Classify("\xFF\xFF\xFF\xFFU\xFF\xFF\xFF\xFF", 9, challenge));
	CHECK("A2S_PLAYER asking for a challenge", challenge == A2S_NO_CHALLENGE);
	LONGS_EQUAL("A2S_PLAYER with a challenge", A2SQ_PLAYER, Classify("\xFF\xFF\xFF\xFFU\x78\x56\x34\x12", 9, challenge));
	CHECK("A2S_PLAYER challenge", challenge == 0x12345678);
	LONGS_EQUAL("A2S_PLAYER without a challenge", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFFU", 5));
	LONGS_EQUAL("A2S_PLAYER, too long", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFFU\xFF\xFF\xFF\xFF\x00", 10));

	LONGS_EQUAL("A2S_RULES", A2SQ_RULES, Classify("\xFF\xFF\xFF\xFFV\x00\x00\x00\x00", 9, challenge));
	CHECK("A2S_RULES challenge", challenge == 0);
	LONGS_EQUAL("A2S_RULES without a challenge", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFFV", 5));

	LONGS_EQUAL("Not connectionless", A2SQ_NONE, Classify("\x01\x00\x00\x00U\xFF\xFF\xFF\xFF", 9));
	LONGS_EQUAL("getchallenge", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFFgetchallenge steam\n", 23));
	LONGS_EQUAL("Too short", A2SQ_NONE, Classify("\xFF\xFF\xFF\xFF", 4));
}

TEST(RecognizeAnswers, A2SCache, 1000)
{
	CHECK("Source info answer", IsAnswer(A2SQ_INFO, "\xFF\xFF\xFF\xFFI0name", 10));
	CHECK("GoldSrc detailed info answer", IsAnswer(A2SQ_INFO, "\xFF\xFF\xFF\xFFmaddr", 10));
	CHECK("Players answer", IsAnswer(A2SQ_PLAYER, "\xFF\xFF\xFF\xFF" "D\x00", 6));
	CHECK("Rules answer", IsAnswer(A2SQ_RULES, "\xFF\xFF\xFF\xFF" "E\x01\x00", 8));

	// Steam asking for a challenge
	CHECK("Challenge to an info query", !IsAnswer(A2SQ_INFO, "\xFF\xFF\xFF\xFF" "A\x01\x02\x03\x04", 9));
	CHECK("Challenge to a players query", !IsAnswer(A2SQ_PLAYER, "\xFF\xFF\xFF\xFF" "A\x01\x02\x03\x04", 9));
	CHECK("Challenge to a rules query", !IsAnswer(A2SQ_RULES, "\xFF\xFF\xFF\xFF" "A\x01\x02\x03\x04", 9));

	CHECK("Players answer to an info query", !IsAnswer(A2SQ_INFO, "\xFF\xFF\xFF\xFF" "D\x00", 6));
	CHECK("Split answer", !IsAnswer(A2SQ_RULES, "\xFE\xFF\xFF\xFF\x02\x00\x00\x00\x02\xFF\xFF\xFF\xFF" "E", 14));
}

TEST(Challenges, A2SCache, 1000)
{
	const uint64 key[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
	const uint64 otherKey[2] = { 0x0123456789ABCDEFull, 0xFEDCBA9876543211ull };

	uint32 challenge = A2S_MakeChallenge(key, 0x0100007F, 10);
	CHECK("Challenge is not stable", challenge == A2S_MakeChallenge(key, 0x0100007F, 10));
	CHECK("Same challenge for another address", challenge != A2S_MakeChallenge(key, 0x0200007F, 10));
	CHECK("Same challenge in the next period", challenge != A2S_MakeChallenge(key, 0x0100007F, 11));
	CHECK("Same challenge with another key", challenge != A2S_MakeChallenge(otherKey, 0x0100007F, 10));

	// The value that asks for a challenge is never handed out
	for (uint32 ip = 0; ip < 100000; ip++)
		CHECK("Challenge asks for a challenge", A2S_MakeChallenge(key, ip, 1) != A2S_NO_CHALLENGE);
}

TEST(SplitLikeNetSendLong, A2SCache, 1000)
{
	unsigned char answer[2545];
	for (int i = 0; i < (int)sizeof(answer); i++)
		answer[i] = (unsigned char)i;

	unsigned char packet[MAX_ROUTEABLE_PACKET];

	LONGS_EQUAL("Short answer split", 1, A2S_SplitCount(MAX_ROUTEABLE_PACKET));
	LONGS_EQUAL("Short answer changed size", 100, A2S_SplitPart(answer, 100, 0, 7, packet));
	CHECK("Short answer changed", !Q_memcmp(packet, answer, 100));

	// What NET_SendLong sends for Steam's 2545 bytes rules answer: 1391 + 1154 bytes
	LONGS_EQUAL("Long answer parts", 2, A2S_SplitCount(sizeof(answer)));

	LONGS_EQUAL("First part size", 1400, A2S_SplitPart(answer, sizeof(answer), 0, 0x40000001, packet));
	const unsigned char firstHeader[] = { 0xFE, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x40, 0x02 };
	CHECK("First part header", !Q_memcmp(packet, firstHeader, sizeof(firstHeader)));
	CHECK("First part data", !Q_memcmp(packet + 9, answer, 1391));

	LONGS_EQUAL("Second part size", 1163, A2S_SplitPart(answer, sizeof(answer), 1, 0x40000001, packet));
	const unsigned char secondHeader[] = { 0xFE, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x40, 0x12 };
	CHECK("Second part header", !Q_memcmp(packet, secondHeader, sizeof(secondHeader)));
	CHECK("Second part data", !Q_memcmp(packet + 9, answer + 1391, 1154));
}
