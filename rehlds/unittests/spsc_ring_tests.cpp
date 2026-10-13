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

TEST(PeekAndPopBatches, SpscRing, 1000)
{
	SpscRing<int> ring;
	CHECK("Allocation failed", ring.Allocate(8));

	// Several rounds, so that the batches wrap around the end of the slots
	int next = 0;
	int expected = 0;
	for (int round = 0; round < 10; round++)
	{
		for (int i = 0; i < 5; i++)
		{
			int *slot = ring.BeginPush();
			CHECK("Ring full too early", slot != nullptr);
			*slot = next++;
			ring.CommitPush();
		}

		CHECK("Peek past the queued elements", ring.Peek(5) == nullptr);

		int seen = 0;
		while (int *slot = ring.Peek(seen))
		{
			LONGS_EQUAL("Peek out of order", expected + seen, *slot);
			seen++;
		}
		LONGS_EQUAL("Peek did not see every element", 5, seen);

		ring.Pop(3);
		LONGS_EQUAL("Wrong front after a batch pop", expected + 3, *ring.Front());
		ring.Pop(2);
		expected += 5;
		CHECK("Ring not empty after the batch pops", ring.Front() == nullptr);
	}
}

TEST(TwoThreadsBatchesKeepOrder, SpscRing, 10000)
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
		int seen = 0;
		while (seen < 16)
		{
			int *slot = ring.Peek(seen);
			if (!slot)
				break;

			if (*slot != expected + seen)
				inOrder = false;
			seen++;
		}

		if (!seen)
		{
			std::this_thread::yield();
			continue;
		}

		ring.Pop(seen);
		expected += seen;
	}

	producer.join();

	CHECK("Elements lost, duplicated or out of order", inOrder);
	CHECK("Ring not empty at the end", ring.Front() == nullptr);
}
