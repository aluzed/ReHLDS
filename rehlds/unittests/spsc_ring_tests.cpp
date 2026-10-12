#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#include <thread>

TEST(FillAndDrain, SpscRing, 1000)
{
	SpscRing<int> ring;
	CHECK("Allocation failed", ring.Allocate(5));
	LONGS_EQUAL("Capacity is not rounded up to a power of two", 8, (int)ring.Capacity());
	CHECK("New ring is not empty", ring.Front() == nullptr);

	for (int i = 0; i < 8; i++)
	{
		int *slot = ring.BeginPush();
		CHECK("Ring full before its capacity", slot != nullptr);
		*slot = i;
		ring.CommitPush();
	}

	CHECK("Ring accepted more than its capacity", ring.BeginPush() == nullptr);
	LONGS_EQUAL("Wrong size", 8, (int)ring.Size());

	for (int i = 0; i < 8; i++)
	{
		int *slot = ring.Front();
		CHECK("Ring empty too early", slot != nullptr);
		LONGS_EQUAL("Elements out of order", i, *slot);
		ring.Pop();
	}

	CHECK("Ring not empty after draining", ring.Front() == nullptr);
	CHECK("Ring still full after draining", ring.BeginPush() != nullptr);
}

TEST(TwoThreadsKeepOrder, SpscRing, 10000)
{
	const int count = 1000000;

	SpscRing<int> ring;
	CHECK("Allocation failed", ring.Allocate(64));

	std::thread producer([&ring]()
	{
		for (int i = 0; i < count; i++)
		{
			int *slot;
			while ((slot = ring.BeginPush()) == nullptr)
				std::this_thread::yield();

			*slot = i;
			ring.CommitPush();
		}
	});

	int expected = 0;
	bool inOrder = true;
	while (expected < count)
	{
		int *slot = ring.Front();
		if (!slot)
		{
			std::this_thread::yield();
			continue;
		}

		if (*slot != expected)
			inOrder = false;

		expected++;
		ring.Pop();
	}

	producer.join();

	CHECK("Elements lost, duplicated or out of order", inOrder);
	CHECK("Ring not empty at the end", ring.Front() == nullptr);
}
