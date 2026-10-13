#include "precompiled.h"

#ifdef _WIN32
// Not in windows.h with WIN32_LEAN_AND_MEAN; advapi32 is linked by default
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

cvar_t sv_a2s_cache = { "sv_a2s_cache", "0", 0, 0.0f, nullptr };
cvar_t sv_a2s_challenges_sec = { "sv_a2s_challenges_sec", "0", 0, 0.0f, nullptr };

namespace
{
	// Address the main thread asks Steam from: loopback, which Steam answers without
	// a challenge, on a privileged port that no client uses. Host byte order, like
	// the Steam API.
	const uint32 CAPTURE_IP = 0x7F000001;
	const uint16 CAPTURE_PORT = 1;

	// Same buffer as CSteam3Server::RunFrame
	const int MAX_STEAM_PACKET = 4096;

	const uint64 NS_PER_SEC = 1000000000ull;

	// An answer asked for again is captured again once it is this old: Steam's server
	// details (scores, hostname, password) are updated once per second
	const uint64 REFRESH_AGE_NS = NS_PER_SEC;

	// Answer to A2S_INFO in the Source format, the one Steam sends (engine/net.h only
	// has the GoldSrc ones)
	const unsigned char S2A_INFO_SOURCE = 'I';

	const unsigned char QUERY_INFO[] = { 0xFF, 0xFF, 0xFF, 0xFF, 'T', 'S', 'o', 'u', 'r', 'c', 'e', ' ',
		'E', 'n', 'g', 'i', 'n', 'e', ' ', 'Q', 'u', 'e', 'r', 'y', 0 };
	const unsigned char QUERY_PLAYER[] = { 0xFF, 0xFF, 0xFF, 0xFF, 'U', 0xFF, 0xFF, 0xFF, 0xFF };
	const unsigned char QUERY_RULES[] = { 0xFF, 0xFF, 0xFF, 0xFF, 'V', 0xFF, 0xFF, 0xFF, 0xFF };

	struct QueryDesc
	{
		const char *name;
		const unsigned char *data;
		int size;
	};

	const QueryDesc s_Queries[A2SQ_COUNT] =
	{
		{ "A2S_INFO", QUERY_INFO, sizeof(QUERY_INFO) },
		{ "A2S_PLAYER", QUERY_PLAYER, sizeof(QUERY_PLAYER) },
		{ "A2S_RULES", QUERY_RULES, sizeof(QUERY_RULES) },
	};

	// Main thread state
	std::shared_ptr<const A2SReplies> s_Published;
	bool s_bDirty[A2SQ_COUNT];
	bool s_bDemanded[A2SQ_COUNT];
	uint32 s_Captures[A2SQ_COUNT];
	uint32 s_Failures[A2SQ_COUNT];
	uint64 s_TotalCostNs[A2SQ_COUNT];
	uint64 s_MaxCostNs[A2SQ_COUNT];

	// Sends a packet Steam queued for someone else, like CSteam3Server::RunFrame
	void SendSteamPacket(const unsigned char *data, int size, uint32 ip, uint16 port)
	{
		netadr_t adr;
		Q_memset(&adr, 0, sizeof(adr));
		*(uint32 *)&adr.ip[0] = htonl(ip);
		adr.port = htons(port);
		adr.type = NA_IP;

		NET_SendPacket(NS_SERVER, size, (void *)data, adr);
	}

	// Hands a packet to Steam and takes its answer, one packet (long answers are split
	// by NET_SendLong). Packets Steam had queued for other addresses before it are sent
	// on the way, a few milliseconds before RunFrame would have; those queued after it
	// are left to RunFrame: each GetNextOutgoingPacket call costs about 20 us, even
	// when the queue is empty.
	void AskSteam(ISteamGameServer *steam, const unsigned char *query, int size, A2SReplies::Reply &reply)
	{
		steam->HandleIncomingPacket(query, size, CAPTURE_IP, CAPTURE_PORT);

		unsigned char packet[MAX_STEAM_PACKET];
		uint32 ip;
		uint16 port;
		int packetSize;
		while ((packetSize = steam->GetNextOutgoingPacket(packet, sizeof(packet), &ip, &port)) > 0)
		{
			if (ip != CAPTURE_IP || port != CAPTURE_PORT)
			{
				SendSteamPacket(packet, packetSize, ip, port);
				continue;
			}

			reply.packet.assign(packet, packet + packetSize);
			Steam_OutgoingPacketsPending();
			break;
		}
	}

	bool IsChallenge(const A2SReplies::Reply &reply)
	{
		return reply.packet.size() == 9
			&& *(const int32 *)reply.packet.data() == -1 && reply.packet[4] == S2C_CHALLENGE;
	}

	bool Capture(ISteamGameServer *steam, int queryType, A2SReplies::Reply &reply)
	{
		const QueryDesc &query = s_Queries[queryType];
		AskSteam(steam, query.data, query.size, reply);

		// Should Steam ask the loopback address for a challenge as well, answer it
		if (IsChallenge(reply))
		{
			unsigned char retry[sizeof(QUERY_INFO) + 4];
			int size = query.size;
			Q_memcpy(retry, query.data, size);
			if (queryType == A2SQ_INFO)
			{
				Q_memcpy(&retry[size], &reply.packet[5], 4);
				size += 4;
			}
			else
				Q_memcpy(&retry[5], &reply.packet[5], 4);

			reply.packet.clear();
			AskSteam(steam, retry, size, reply);
		}

		bool valid = A2S_IsAnswer(queryType, reply.packet.data(), (int)reply.packet.size());
		if (!valid)
			reply.packet.clear();

		reply.capturedNs = Sys_MonotonicNs();
		return valid;
	}

	void Publish(int queryType, const A2SReplies::Reply *reply)
	{
		// Not std::make_shared: it needs a newer libstdc++ than the one shipped with HLDS
		std::shared_ptr<A2SReplies> replies(s_Published ? new A2SReplies(*s_Published) : new A2SReplies);
		A2SReplies::Reply &slot = replies->replies[queryType];
		if (reply)
			slot = *reply;
		else
			slot.packet.clear();

		s_Published = replies;
		g_NetReceiveThread.PublishQueryReplies(s_Published);
	}

	void Reset()
	{
		for (int i = 0; i < A2SQ_COUNT; i++)
		{
			s_bDirty[i] = false;
			s_bDemanded[i] = false;
		}
	}

	uint64 RotateLeft(uint64 x, int b)
	{
		return (x << b) | (x >> (64 - b));
	}

	void SipRound(uint64 &v0, uint64 &v1, uint64 &v2, uint64 &v3)
	{
		v0 += v1; v1 = RotateLeft(v1, 13); v1 ^= v0; v0 = RotateLeft(v0, 32);
		v2 += v3; v3 = RotateLeft(v3, 16); v3 ^= v2;
		v0 += v3; v3 = RotateLeft(v3, 21); v3 ^= v0;
		v2 += v1; v1 = RotateLeft(v1, 17); v1 ^= v2; v2 = RotateLeft(v2, 32);
	}

	// SipHash-2-4 of one 64-bit word: forging a challenge for another address
	// requires the key
	uint64 SipHash(const uint64 key[2], uint64 message)
	{
		uint64 v0 = key[0] ^ 0x736f6d6570736575ull;
		uint64 v1 = key[1] ^ 0x646f72616e646f6dull;
		uint64 v2 = key[0] ^ 0x6c7967656e657261ull;
		uint64 v3 = key[1] ^ 0x7465646279746573ull;

		v3 ^= message;
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		v0 ^= message;

		// Final block: the message length (8 bytes) in the top byte
		uint64 last = 8ull << 56;
		v3 ^= last;
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		v0 ^= last;

		v2 ^= 0xFF;
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		SipRound(v0, v1, v2, v3);
		return v0 ^ v1 ^ v2 ^ v3;
	}
}

int A2S_ClassifyQuery(const unsigned char *data, int size, uint32 &challenge)
{
	challenge = A2S_NO_CHALLENGE;

	if (size < 5 || *(const uint32 *)data != 0xFFFFFFFF)
		return A2SQ_NONE;

	switch (data[4])
	{
	case 'T':
		if (size < (int)sizeof(QUERY_INFO) || Q_memcmp(data, QUERY_INFO, sizeof(QUERY_INFO)))
			break;

		// Newer clients append the challenge Steam gave them
		if (size >= (int)sizeof(QUERY_INFO) + 4)
			Q_memcpy(&challenge, &data[sizeof(QUERY_INFO)], sizeof(challenge));

		return A2SQ_INFO;

	case 'U':
	case 'V':
		if (size != (int)sizeof(QUERY_PLAYER))
			break;

		Q_memcpy(&challenge, &data[5], sizeof(challenge));
		return data[4] == 'U' ? A2SQ_PLAYER : A2SQ_RULES;
	}

	return A2SQ_NONE;
}

bool A2S_IsAnswer(int queryType, const unsigned char *packet, int size)
{
	if (size < 5 || *(const int32 *)packet != -1)
		return false;

	unsigned char type = packet[4];
	switch (queryType)
	{
	case A2SQ_INFO:
		return type == S2A_INFO_SOURCE || type == S2A_INFO_DETAILED;

	case A2SQ_PLAYER:
		return type == S2A_PLAYERS;

	case A2SQ_RULES:
		return type == S2A_RULES;
	}

	return false;
}

uint32 A2S_MakeChallenge(const uint64 key[2], uint32 ip, uint64 period)
{
	uint32 challenge = (uint32)SipHash(key, ((uint64)ip << 32) ^ period);

	// A2S_NO_CHALLENGE asks for a challenge
	if (challenge == A2S_NO_CHALLENGE)
		challenge = 0;

	return challenge;
}

int A2S_SplitCount(int size)
{
	if (size <= MAX_ROUTEABLE_PACKET)
		return 1;

	return (size + (int)SPLIT_SIZE - 1) / (int)SPLIT_SIZE;
}

int A2S_SplitPart(const unsigned char *data, int size, int part, uint32 sequence, unsigned char *out)
{
	int count = A2S_SplitCount(size);
	if (count == 1)
	{
		Q_memcpy(out, data, size);
		return size;
	}

	int offset = part * (int)SPLIT_SIZE;
	int partSize = Q_min((int)SPLIT_SIZE, size - offset);

	SPLITPACKET *header = (SPLITPACKET *)out;
	header->netID = NET_HEADER_FLAG_SPLITPACKET;
	header->sequenceNumber = (int)sequence;
	header->packetID = (unsigned char)((part << 4) + count);
	Q_memcpy(out + sizeof(SPLITPACKET), data + offset, partSize);

	return partSize + (int)sizeof(SPLITPACKET);
}

void A2S_RandomKey(uint64 key[2])
{
	bool done = false;

#ifdef _WIN32
	HCRYPTPROV provider;
	if (CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
	{
		done = CryptGenRandom(provider, sizeof(uint64) * 2, (BYTE *)key) != FALSE;
		CryptReleaseContext(provider, 0);
	}
#else
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0)
	{
		done = read(fd, key, sizeof(uint64) * 2) == (ssize_t)(sizeof(uint64) * 2);
		close(fd);
	}
#endif

	if (!done)
	{
		// Weak, but challenges still differ between servers and restarts
		key[0] = Sys_MonotonicNs() ^ ((uint64)time(nullptr) << 32);
		key[1] = (uint64)(uintptr_t)key ^ 0x9E3779B97F4A7C15ull;
	}
}

A2SChallengeLimit::A2SChallengeLimit() :
	m_GlobalSecond(0),
	m_GlobalCount(0)
{
	m_Key[0] = m_Key[1] = 0;
}

void A2SChallengeLimit::SetKey(const uint64 key[2])
{
	// Allocated here rather than with the engine's globals: 1 MB that only servers
	// answering queries on the network thread need. Another key places every address
	// in another slot, so what the previous one placed is forgotten.
	m_Key[0] = key[0] ^ 0x736C6F7473736C6Full;	// "slotsslo"
	m_Key[1] = key[1];
	m_Slots.assign(NUM_SLOTS, Slot());
}

bool A2SChallengeLimit::Check(uint32 ip, uint64 nowNs, float perIpSec, float windowSec, float globalSec)
{
	if (m_Slots.empty())
		return false;

	if (windowSec < 1.0f)
		windowSec = 1.0f;

	uint64 windowNs = (uint64)(windowSec * (float)NS_PER_SEC);

	// Per address first, so that one address over its limit does not use up the
	// budget of the others (same order as CIPRateLimit)
	Slot &slot = m_Slots[SipHash(m_Key, ip) % NUM_SLOTS];
	bool windowOver = slot.count == 0 || nowNs - slot.windowStartNs > windowNs;
	if (slot.ip != ip && !windowOver && (float)slot.count / windowSec > perIpSec)
		return false;

	if (slot.ip != ip || windowOver)
	{
		slot.ip = ip;
		slot.count = 1;
		slot.windowStartNs = nowNs;
	}
	else
	{
		if (slot.count != 0xFFFFFFFF)
			slot.count++;

		if ((float)slot.count / windowSec > perIpSec)
			return false;
	}

	if (globalSec <= 0.0f)
		return true;

	uint64 second = nowNs / NS_PER_SEC;
	if (second != m_GlobalSecond)
	{
		m_GlobalSecond = second;
		m_GlobalCount = 0;
	}

	if ((float)m_GlobalCount >= globalSec)
		return false;

	m_GlobalCount++;
	return true;
}

void A2SCache_Init()
{
	Cvar_RegisterVariable(&sv_a2s_cache);
	Cvar_RegisterVariable(&sv_a2s_challenges_sec);
	Reset();
	Q_memset(s_Captures, 0, sizeof(s_Captures));
	Q_memset(s_Failures, 0, sizeof(s_Failures));
	Q_memset(s_TotalCostNs, 0, sizeof(s_TotalCostNs));
	Q_memset(s_MaxCostNs, 0, sizeof(s_MaxCostNs));
}

void A2SCache_Invalidate()
{
	for (int i = 0; i < A2SQ_COUNT; i++)
		s_bDirty[i] = true;
}

void A2SCache_Frame()
{
	if (sv_a2s_cache.value == 0.0f || !g_NetReceiveThread.IsRunning())
	{
		if (s_Published)
		{
			s_Published.reset();
			g_NetReceiveThread.PublishQueryReplies(nullptr);
		}

		Reset();
		return;
	}

	// From now on the network thread handles the queries, even before it has answers
	if (!s_Published)
	{
		s_Published.reset(new A2SReplies);
		g_NetReceiveThread.PublishQueryReplies(s_Published);
	}

	uint64 now = Sys_MonotonicNs();
	int toCapture = A2SQ_NONE;

	for (int i = 0; i < A2SQ_COUNT; i++)
	{
		if (g_NetReceiveThread.TakeQueryDemand(i))
			s_bDemanded[i] = true;

		const A2SReplies::Reply *cached = s_Published ? &s_Published->replies[i] : nullptr;
		bool isCached = cached && !cached->packet.empty();

		if (s_bDirty[i] && !s_bDemanded[i])
		{
			// Nobody asks: drop the answer instead of capturing it again
			if (isCached)
				Publish(i, nullptr);

			s_bDirty[i] = false;
			continue;
		}

		if (toCapture == A2SQ_NONE && s_bDemanded[i]
			&& (s_bDirty[i] || !isCached || now - cached->capturedNs >= REFRESH_AGE_NS))
			toCapture = i;
	}

	// One capture per frame: each costs 20 to 40 us
	if (toCapture == A2SQ_NONE)
		return;

	ISteamGameServer *steam = CRehldsPlatformHolder::get()->SteamGameServer();
	A2SReplies::Reply reply;
	if (Capture(steam, toCapture, reply))
		s_Captures[toCapture]++;
	else
		s_Failures[toCapture]++;

	Publish(toCapture, &reply);

	s_bDirty[toCapture] = false;
	s_bDemanded[toCapture] = false;

	uint64 cost = Sys_MonotonicNs() - now;
	s_TotalCostNs[toCapture] += cost;
	if (cost > s_MaxCostNs[toCapture])
		s_MaxCostNs[toCapture] = cost;
}

void A2SCache_Stats()
{
	if (!s_Published)
	{
		Con_Printf("  A2S cache     off\n");
		return;
	}

	uint64 now = Sys_MonotonicNs();
	for (int i = 0; i < A2SQ_COUNT; i++)
	{
		const A2SReplies::Reply &reply = s_Published->replies[i];
		uint32 attempts = s_Captures[i] + s_Failures[i];
		Con_Printf("  A2S cache     %-10s %u captures, %u failed, cost avg %.1f us, max %.1f us, %s\n",
			s_Queries[i].name, s_Captures[i], s_Failures[i],
			attempts ? s_TotalCostNs[i] / 1e3 / attempts : 0.0, s_MaxCostNs[i] / 1e3,
			reply.packet.empty() ? "not cached" : va("%.1f s old", (now - reply.capturedNs) / 1e9));
	}
}
