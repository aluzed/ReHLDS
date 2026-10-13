#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#include <atomic>
#include <thread>

// delta_tests.cpp
void _InitDeltaField(delta_description_t* fieldDesc, int expectedOffset, int type, const char* name, int off, short sz, int bits, float preMult, float postMult);

namespace
{
	const int NUM_WORKERS = 3;

	engine_thread_state_t s_TestWorkerStates[NUM_WORKERS];

	void InitTestWorker(int worker)
	{
		ThreadState_Init(&s_TestWorkerStates[worker], false);
		ThreadState_SetCurrent(&s_TestWorkerStates[worker]);
	}

	struct count_items_t
	{
		std::atomic<int> runs[64];
		std::atomic<int> onWorkers;
	};

	void CountItem(void *context, int item)
	{
		count_items_t *counts = (count_items_t *)context;
		counts->runs[item].fetch_add(1);
		if (!IsMainThread())
			counts->onWorkers.fetch_add(1);
	}
}

TEST(EveryItemRunsOnce, WorkerPool, 10000)
{
	CWorkerPool pool;
	CHECK("Cannot start the workers", pool.Start(NUM_WORKERS, InitTestWorker, "test"));
	LONGS_EQUAL("Wrong number of workers", NUM_WORKERS, pool.NumWorkers());

	count_items_t counts;
	counts.onWorkers = 0;
	int total = 0;

	for (int batch = 0; batch < 2000; batch++)
	{
		int numItems = batch % 64;
		for (int i = 0; i < 64; i++)
			counts.runs[i] = 0;

		// Items published one by one, as SV_SendClientMessages does
		pool.BeginBatch(CountItem, &counts);
		for (int i = 1; i <= numItems; i++)
			pool.Publish(i);
		pool.Finish();

		for (int i = 0; i < 64; i++)
			LONGS_EQUAL("Item not run exactly once", i < numItems ? 1 : 0, counts.runs[i].load());

		total += numItems;
	}

	CHECK("The workers never ran an item", counts.onWorkers.load() > 0);
	CHECK("More items on the workers than run", counts.onWorkers.load() <= total);

	pool.Stop();
	CHECK("Pool still running after Stop", !pool.IsRunning());
}

#ifdef REHLDS_JIT

namespace
{
#pragma pack(push, 1)
	struct encode_test_struct_t
	{
		uint8 b_00;
		uint16 s_01;
		uint32 i_03;
		float f_07;
		uint8 b_0B;
	};
#pragma pack(pop)

	const int NUM_PAIRS = 200;
	const int MSG_SIZE = 64;

	delta_t *s_pEncodeDelta;
	encode_test_struct_t s_From[NUM_PAIRS], s_To[NUM_PAIRS];
	byte s_Expected[NUM_PAIRS][MSG_SIZE];
	int s_ExpectedSize[NUM_PAIRS];
	std::atomic<int> s_Mismatches;
	std::atomic<int> s_EncoderCallsOffMainThread;

	// Encoder records of the work items, made by the main thread
	const int NUM_ITEMS = 8;
	encoder_record_t s_Records[NUM_ITEMS];

	// Like the game DLLs' encoders: changes the marks through the engine
	void TestConditionalEncoder(delta_s *pFields, const unsigned char *from, const unsigned char *to)
	{
		if (!IsMainThread())
			s_EncoderCallsOffMainThread.fetch_add(1);

		const encode_test_struct_t *t = (const encode_test_struct_t *)to;
		if (t->b_0B & 1)
			DELTA_UnsetFieldByIndex(pFields, 0);
		else
			DELTA_SetFieldByIndex(pFields, 2);
	}

	int EncodePair(int pair, byte *buf)
	{
		sizebuf_t msg;
		msg.buffername = "test";
		msg.data = buf;
		msg.maxsize = MSG_SIZE;
		msg.cursize = 0;
		msg.flags = SIZEBUF_ALLOW_OVERFLOW;

		MSG_StartBitWriting(&msg);
		DELTA_WriteDelta((byte *)&s_From[pair], (byte *)&s_To[pair], TRUE, s_pEncodeDelta, NULL);
		MSG_EndBitWriting(&msg);
		return msg.cursize;
	}

	// The main thread records the encoders of an item's pairs, in encoding order
	void RecordItem(int item)
	{
		encoder_record_t *record = &s_Records[item];
		record->numCalls = 0;
		record->numOps = 0;
		record->complete = TRUE;
		for (int pair = item; pair < NUM_PAIRS; pair += NUM_ITEMS)
		{
			if (!DELTAJit_RecordConditionalEncoder(record, s_pEncodeDelta, (byte *)&s_From[pair], (byte *)&s_To[pair]))
				record->complete = FALSE;
		}
	}

	void EncodeAndCompare(void *context, int item)
	{
		engine_thread_state_t *state = ThreadState();
		state->encoderReplay = &s_Records[item];
		state->encoderReplayCursor = 0;
		state->encoderReplayMiss = false;

		for (int pair = item; pair < NUM_PAIRS; pair += NUM_ITEMS)
		{
			byte buf[MSG_SIZE];
			int size = EncodePair(pair, buf);
			if (size != s_ExpectedSize[pair] || Q_memcmp(buf, s_Expected[pair], size))
				s_Mismatches.fetch_add(1);
		}

		if (state->encoderReplayMiss || state->encoderReplayCursor != s_Records[item].numCalls)
			s_Mismatches.fetch_add(1);

		state->encoderReplay = nullptr;
	}

	void InitEncodeDelta(delta_description_t *fields)
	{
		_InitDeltaField(&fields[0], 0x00, DT_BYTE, "b_00", offsetof(encode_test_struct_t, b_00), 1, 8, 1.0f, 1.0f);
		_InitDeltaField(&fields[1], 0x01, DT_SHORT, "s_01", offsetof(encode_test_struct_t, s_01), 2, 16, 1.0f, 1.0f);
		_InitDeltaField(&fields[2], 0x03, DT_INTEGER, "i_03", offsetof(encode_test_struct_t, i_03), 4, 32, 1.0f, 1.0f);
		_InitDeltaField(&fields[3], 0x07, DT_FLOAT, "f_07", offsetof(encode_test_struct_t, f_07), 4, 32, 1.0f, 1.0f);
		_InitDeltaField(&fields[4], 0x0B, DT_BYTE, "b_0B", offsetof(encode_test_struct_t, b_0B), 1, 8, 1.0f, 1.0f);

		s_pEncodeDelta = (delta_t *)Mem_ZeroMalloc(sizeof(delta_t));
		s_pEncodeDelta->fieldCount = 5;
		s_pEncodeDelta->pdd = fields;
		s_pEncodeDelta->conditionalencode = TestConditionalEncoder;

		delta_info_t *dinfo = (delta_info_t *)Mem_ZeroMalloc(sizeof(delta_info_t));
		dinfo->delta = s_pEncodeDelta;
		dinfo->loadfile = Mem_Strdup("__fake_encode_test_struct_t");
		dinfo->name = Mem_Strdup("encode_test_struct_t");
		dinfo->next = g_sv_delta;
		g_sv_delta = dinfo;

		g_DeltaJitRegistry.CreateAndRegisterDeltaJIT(s_pEncodeDelta);
	}
}

// Several threads encoding with the same delta description, replaying the conditional
// encoder that the main thread recorded, produce the bits of a serial encoding
// (MT-402, MT-404) and never call the encoder themselves
TEST(ConcurrentDeltaEncoding, WorkerPool, 10000)
{
	EngineInitializer engInitGuard;

	static delta_description_t fields[8];
	InitEncodeDelta(fields);

	for (int pair = 0; pair < NUM_PAIRS; pair++)
	{
		encode_test_struct_t *from = &s_From[pair], *to = &s_To[pair];
		Q_memset(from, 0, sizeof(*from));
		from->b_00 = pair & 0xFF;
		from->s_01 = pair * 3;
		from->i_03 = pair * 7;
		from->f_07 = pair * 0.5f;
		from->b_0B = pair & 3;

		*to = *from;
		if (pair & 1) to->b_00++;
		if (pair & 2) to->s_01 += 5;
		if (pair & 4) to->i_03 ^= 0x5555;
		if (pair & 8) to->f_07 += 1.0f;
		to->b_0B = (pair >> 4) & 0xFF;

		s_ExpectedSize[pair] = EncodePair(pair, s_Expected[pair]);
	}

	CWorkerPool pool;
	CHECK("Cannot start the workers", pool.Start(NUM_WORKERS, InitTestWorker, "test"));

	s_Mismatches = 0;
	s_EncoderCallsOffMainThread = 0;
	for (int item = 0; item < NUM_ITEMS; item++)
	{
		RecordItem(item);
		CHECK("Record incomplete", s_Records[item].complete);
	}

	for (int round = 0; round < 500; round++)
	{
		pool.BeginBatch(EncodeAndCompare, nullptr);
		pool.Publish(NUM_ITEMS);
		pool.Finish();
	}

	pool.Stop();
	LONGS_EQUAL("Concurrent encoding differs from the serial one", 0, s_Mismatches.load());
	LONGS_EQUAL("Encoder called off the main thread", 0, s_EncoderCallsOffMainThread.load());
}

// A replay that does not match the record changes nothing and reports a miss; an
// encoding thread without a record never calls the encoder
TEST(EncoderReplayMiss, WorkerPool, 1000)
{
	EngineInitializer engInitGuard;

	static delta_description_t fields[8];
	InitEncodeDelta(fields);

	for (int pair = 0; pair < 2; pair++)
	{
		Q_memset(&s_From[pair], 0, sizeof(s_From[pair]));
		s_To[pair] = s_From[pair];
		s_To[pair].b_0B = (uint8)pair;
	}

	encoder_record_t *record = &s_Records[0];
	record->numCalls = 0;
	record->numOps = 0;
	CHECK("Recording failed", DELTAJit_RecordConditionalEncoder(record, s_pEncodeDelta, (byte *)&s_From[0], (byte *)&s_To[0]));
	LONGS_EQUAL("One call recorded", 1, record->numCalls);
	LONGS_EQUAL("One op recorded", 1, record->numOps);
	LONGS_EQUAL("Wrong op recorded", ENCODER_OP_SET | 2, record->ops[0]);

	// Replayed for another pair: miss, the cursor stays
	engine_thread_state_t *state = ThreadState();
	state->encoderReplay = record;
	state->encoderReplayCursor = 0;
	state->encoderReplayMiss = false;
	byte buf[MSG_SIZE];
	EncodePair(1, buf);
	CHECK("Miss not reported", state->encoderReplayMiss);
	LONGS_EQUAL("Cursor moved on a miss", 0, state->encoderReplayCursor);
	state->encoderReplay = nullptr;
	state->encoderReplayMiss = false;

	// A worker thread without a record
	s_EncoderCallsOffMainThread = 0;
	bool missed = false;
	std::thread worker([&missed]()
	{
		engine_thread_state_t workerState;
		ThreadState_Init(&workerState, false);
		ThreadState_SetCurrent(&workerState);

		byte workerBuf[MSG_SIZE];
		EncodePair(0, workerBuf);
		missed = workerState.encoderReplayMiss;
	});
	worker.join();

	CHECK("Miss not reported on a worker without a record", missed);
	LONGS_EQUAL("Encoder called on a worker", 0, s_EncoderCallsOffMainThread.load());
}

#endif // REHLDS_JIT
