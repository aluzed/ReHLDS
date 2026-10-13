#pragma once

#include "archtypes.h"

#include <memory>
#include <vector>

// A2S query answers from a cached copy (MT-305).
//
// The Steam game server library answers A2S_INFO, A2S_PLAYER and A2S_RULES: the engine
// hands it every connectionless packet (Steam_HandleIncomingPacket, 20 to 40 us of main
// thread time each) and sends the answers it queues every 10 ms (CSteam3Server::RunFrame).
// Steam first answers queries with a challenge, except from the loopback address, so a
// player's server browser waits twice for the main thread and the next 10 ms send.
//
// With sv_a2s_cache 1 and the network thread running (-netthread), the network thread
// answers these queries itself, as soon as they arrive, with a copy of Steam's answers:
//
// * The main thread asks Steam the queries itself, from 127.0.0.1:1 (a privileged port
//   no client uses), and publishes the answers. It only asks for the queries clients
//   ask, again when an answer is older than a second (Steam's server details are updated
//   once per second) and when what Steam reports changes (cvar, player connect or
//   disconnect, map change). At most one query per frame.
// * The network thread hands out its own challenges, from a keyed hash of the address
//   that changes every 30 seconds, and answers the queries that carry one, like Steam
//   does. Loopback queries are answered at once, like Steam does.
// * Long answers are split like NET_SendLong does.
//
// Steam stays the source of the answers: plugins that change what Steam reports
// (Steam_GSBUpdateUserData hooks for instance) are still taken into account.
//
// The network thread answers only when it filters packets itself (net_thread_filter 1)
// and no plugin hooks PreprocessPacket or SV_CheckConnectionLessRateLimits: otherwise
// these hooks must see the queries, which then go to the main thread as before.

extern cvar_t sv_a2s_cache;

enum A2SQueryType
{
	A2SQ_NONE = -1,

	A2SQ_INFO,       // "TSource Engine Query", optionally followed by a challenge
	A2SQ_PLAYER,     // 'U' + challenge
	A2SQ_RULES,      // 'V' + challenge

	A2SQ_COUNT
};

// Challenge value of a query that has none, or asks for one
const uint32 A2S_NO_CHALLENGE = 0xFFFFFFFF;

// Answers captured from Steam, immutable once published
struct A2SReplies
{
	struct Reply
	{
		uint64 capturedNs = 0;	// Sys_MonotonicNs
		std::vector<unsigned char> packet;	// empty: not cached, the query goes to Steam
	};

	Reply replies[A2SQ_COUNT];
};

// Which cached query a connectionless packet is, A2SQ_NONE when it is none of them or
// not in a form Steam answers (the main thread then gets it as before). challenge is
// the value the query carries, A2S_NO_CHALLENGE when it has none.
int A2S_ClassifyQuery(const unsigned char *data, int size, uint32 &challenge);

// Whether a captured packet is an answer to the query (not a challenge)
bool A2S_IsAnswer(int queryType, const unsigned char *packet, int size);

// Challenge for an address, from a secret key and the current period. Never
// A2S_NO_CHALLENGE.
uint32 A2S_MakeChallenge(const uint64 key[2], uint32 ip, uint64 period);

// Number of packets NET_SendLong splits a packet of this size into (1 when it is not split)
int A2S_SplitCount(int size);

// Builds packet number part of the split of data, with the given sequence number, the
// same way NET_SendLong does. Returns its size. out holds MAX_ROUTEABLE_PACKET bytes.
int A2S_SplitPart(const unsigned char *data, int size, int part, uint32 sequence, unsigned char *out);

// Secret key of the challenges, from the system's random source
void A2S_RandomKey(uint64 key[2]);

void A2SCache_Init();

// Main thread: what Steam reports changed, capture the answers again
void A2SCache_Invalidate();

// Main thread, from CSteam3Server::RunFrame
void A2SCache_Frame();

void A2SCache_Stats();
