#pragma once

#include "archtypes.h"
#include "netadr.h"

#include <stdio.h>

// Records the inputs of a dedicated server session and replays them, so that two
// engine builds can be checked for byte-identical network output (MT-004).
//
//	-netrecord <file>   records the session's inputs to <file>
//	-netreplay <file>   replays them instead of reading the clock, the sockets and
//	                    the console, as fast as possible, then quits
//	-netsends <file>    in both modes, writes every packet sent by the server
//	                    socket to <file> (tools/bench/replay_compare.py compares them)
//
// Inputs are, in the order the engine consumed them: the time of each accepted
// frame, the packets returned by NET_GetPacket(NS_SERVER) (and its "no packet"
// answers), console text, and the few values that come from the system clock or
// from hardware (seeds, challenge salts, PF_Time). The rest of the server is
// deterministic, so a replay reproduces the recorded session, and an engine change
// that alters what is sent shows up as a difference in the sends file.
//
// The command line of the replay must be the recorded one, plus -netreplay.
class CNetReplay
{
public:
	CNetReplay();

	// Called first thing in Host_Init, before anything reads the clock
	void Init(int argc, char **argv);
	void Shutdown();

	bool IsRecording() const { return m_Mode == MODE_RECORD; }
	bool IsReplaying() const { return m_Mode == MODE_REPLAY; }

	// Host_FilterTime: an accepted frame's realtime
	void RecordFrame(double realtime);
	double ReplayFrame(double realtime);

	// NET_GetPacket(NS_SERVER)
	void RecordPacket(bool received, const sizebuf_t *msg, const netadr_t &from);
	bool ReplayPacket(sizebuf_t *msg, netadr_t *from);

	// Console text from the launcher
	void RecordConsoleText(const char *text);

	// Values from the clock or the hardware: recorded, or replaced by the recorded one
	int64 Value(int64 live);
	double ValueF(double live);

	// NET_SendPacket(NS_SERVER)
	void RecordSend(int length, const void *data, const netadr_t &to);

private:
	enum Mode
	{
		MODE_OFF,
		MODE_RECORD,
		MODE_REPLAY,
	};

	enum EventType
	{
		EV_FRAME = 1,
		EV_PACKET,
		EV_NO_PACKET,
		EV_CONSOLE,
		EV_VALUE,
		EV_VALUE_FLOAT,
	};

	static const char *EventName(int type);

	void Write(const void *data, size_t size);
	void WriteEvent(int type);
	bool Read(void *data, size_t size);

	// Replay: reads the next event's type, -1 at the end of the file
	int PeekEvent();
	bool ExpectEvent(int type);
	void Diverged(int expected, int found);
	void ReplayConsoleText();
	void Finish(const char *reason);

	// Fingerprint of the simulation at the start of a frame: the engine's random
	// generator and the entities' positions. A replay reports the first frame where
	// it differs from the recording, which locates a missing input.
	struct FrameState
	{
		int32 random;
		uint32 entities;
	};

	static FrameState CurrentState();

	Mode m_Mode;
	FILE *m_Inputs;
	FILE *m_Sends;
	int m_NextEvent;		// replay: type of the event read ahead, 0 when none
	bool m_bFinished;
	uint32 m_Frame;			// accepted frames so far
	uint32 m_Events;
	uint32 m_Sent;
	bool m_bStateDiverged;
};

extern CNetReplay g_NetReplay;
