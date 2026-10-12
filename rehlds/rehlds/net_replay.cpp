#include "precompiled.h"

CNetReplay g_NetReplay;

namespace
{
	const char INPUTS_MAGIC[8] = { 'R', 'H', 'R', 'E', 'P', 'L', 'Y', '2' };
	const char SENDS_MAGIC[8] = { 'R', 'H', 'S', 'E', 'N', 'D', 'S', '1' };

	const char *FindArg(int argc, char **argv, const char *name)
	{
		for (int i = 1; i + 1 < argc; i++)
		{
			if (!Q_stricmp(argv[i], name))
				return argv[i + 1];
		}

		return nullptr;
	}

	FILE *OpenFile(const char *path, const char *mode, const char *magic)
	{
		FILE *f = fopen(path, mode);
		if (!f)
		{
			fprintf(stderr, "Net replay: cannot open %s\n", path);
			return nullptr;
		}

		setvbuf(f, nullptr, _IOFBF, 1 << 20);
		if (mode[0] == 'w')
			fwrite(magic, 1, 8, f);

		return f;
	}

	void CloseAtExit()
	{
		g_NetReplay.Shutdown();
	}
}

CNetReplay::CNetReplay() :
	m_Mode(MODE_OFF),
	m_Inputs(nullptr),
	m_Sends(nullptr),
	m_NextEvent(0),
	m_bFinished(false),
	m_Frame(0),
	m_Events(0),
	m_Sent(0),
	m_bStateDiverged(false)
{
}

const char *CNetReplay::EventName(int type)
{
	switch (type)
	{
	case EV_FRAME: return "frame";
	case EV_PACKET: return "packet";
	case EV_NO_PACKET: return "no packet";
	case EV_CONSOLE: return "console text";
	case EV_VALUE: return "value";
	case EV_VALUE_FLOAT: return "float value";
	case -1: return "end of recording";
	default: return "unknown event";
	}
}

void CNetReplay::Init(int argc, char **argv)
{
	const char *recordPath = FindArg(argc, argv, "-netrecord");
	const char *replayPath = FindArg(argc, argv, "-netreplay");
	const char *sendsPath = FindArg(argc, argv, "-netsends");

	if (replayPath)
	{
		m_Inputs = OpenFile(replayPath, "rb", INPUTS_MAGIC);
		char magic[8];
		if (m_Inputs && (fread(magic, 1, 8, m_Inputs) != 8 || Q_memcmp(magic, INPUTS_MAGIC, 8)))
		{
			fprintf(stderr, "Net replay: %s is not a recording\n", replayPath);
			fclose(m_Inputs);
			m_Inputs = nullptr;
		}

		if (!m_Inputs)
		{
			// Running the server normally instead would never end
			fprintf(stderr, "Net replay: cannot replay %s, quitting\n", replayPath);
			exit(1);
		}

		m_Mode = MODE_REPLAY;
	}
	else if (recordPath)
	{
		m_Inputs = OpenFile(recordPath, "wb", INPUTS_MAGIC);
		if (m_Inputs)
			m_Mode = MODE_RECORD;
	}

	if (sendsPath && m_Mode != MODE_OFF)
		m_Sends = OpenFile(sendsPath, "wb", SENDS_MAGIC);

	if (m_Mode != MODE_OFF)
	{
		fprintf(stderr, "Net replay: %s %s\n", IsRecording() ? "recording to" : "replaying", IsRecording() ? recordPath : replayPath);
		atexit(CloseAtExit);
	}
}

void CNetReplay::Shutdown()
{
	if (m_Mode == MODE_OFF)
		return;

	if (IsReplaying() && !m_bFinished)
		fprintf(stderr, "Net replay: stopped at frame %u, event %u\n", m_Frame, m_Events);

	if (m_Inputs)
		fclose(m_Inputs);
	if (m_Sends)
		fclose(m_Sends);

	m_Inputs = nullptr;
	m_Sends = nullptr;
	m_Mode = MODE_OFF;
}

void CNetReplay::Write(const void *data, size_t size)
{
	fwrite(data, 1, size, m_Inputs);
}

void CNetReplay::WriteEvent(int type)
{
	uint8 b = (uint8)type;
	Write(&b, 1);
	m_Events++;
}

bool CNetReplay::Read(void *data, size_t size)
{
	return fread(data, 1, size, m_Inputs) == size;
}

int CNetReplay::PeekEvent()
{
	if (!m_NextEvent)
	{
		uint8 b;
		m_NextEvent = Read(&b, 1) ? b : -1;
	}

	return m_NextEvent;
}

bool CNetReplay::ExpectEvent(int type)
{
	if (m_bFinished)
		return false;

	int found = PeekEvent();
	if (found != type)
	{
		if (found == -1)
			Finish("end of recording");
		else
			Diverged(type, found);

		return false;
	}

	m_NextEvent = 0;
	m_Events++;
	return true;
}

void CNetReplay::Diverged(int expected, int found)
{
	fprintf(stderr, "Net replay: diverged at frame %u, event %u: the engine asked for a %s, the recording has a %s\n",
		m_Frame, m_Events, EventName(expected), EventName(found));
	Finish("diverged");
}

void CNetReplay::Finish(const char *reason)
{
	if (m_bFinished)
		return;

	m_bFinished = true;
	fprintf(stderr, "Net replay: %s after %u frames, %u events, %u packets sent\n", reason, m_Frame, m_Events, m_Sent);
	Cbuf_AddText("quit\n");
}

CNetReplay::FrameState CNetReplay::CurrentState()
{
	FrameState state;
	state.random = idum;
	state.entities = 0;

	if (g_psv.active && g_psv.edicts)
	{
		for (int i = 0; i < g_psv.num_edicts; i++)
		{
			const edict_t *ed = &g_psv.edicts[i];
			if (ed->free)
				continue;

			const uint32 *origin = (const uint32 *)ed->v.origin;
			state.entities = state.entities * 31 + i;
			state.entities = state.entities * 31 + origin[0];
			state.entities = state.entities * 31 + origin[1];
			state.entities = state.entities * 31 + origin[2];
		}
	}

	return state;
}

void CNetReplay::RecordFrame(double realtime)
{
	FrameState state = CurrentState();

	WriteEvent(EV_FRAME);
	Write(&realtime, sizeof(realtime));
	Write(&state, sizeof(state));
	m_Frame++;

	// Keep the files usable if the server is killed
	if (!(m_Frame % 1000))
	{
		fflush(m_Inputs);
		if (m_Sends)
			fflush(m_Sends);
	}
}

double CNetReplay::ReplayFrame(double realtime)
{
	ReplayConsoleText();

	if (!ExpectEvent(EV_FRAME))
	{
		// Let the server run its last frames (quit) with a moving clock
		return realtime + 0.001;
	}

	double recorded;
	FrameState recordedState;
	if (!Read(&recorded, sizeof(recorded)) || !Read(&recordedState, sizeof(recordedState)))
	{
		Finish("truncated recording");
		return realtime + 0.001;
	}

	if (!m_bStateDiverged)
	{
		FrameState state = CurrentState();
		if (state.random != recordedState.random || state.entities != recordedState.entities)
		{
			m_bStateDiverged = true;
			fprintf(stderr, "Net replay: simulation differs from the recording from frame %u (%s)\n", m_Frame,
				state.random != recordedState.random ? "random generator" : "entity positions");
		}
	}

	m_Frame++;
	return recorded;
}

void CNetReplay::RecordPacket(bool received, const sizebuf_t *msg, const netadr_t &from)
{
	if (!received)
	{
		WriteEvent(EV_NO_PACKET);
		return;
	}

	int32 size = msg->cursize;
	WriteEvent(EV_PACKET);
	Write(&from, sizeof(from));
	Write(&size, sizeof(size));
	Write(msg->data, size);
}

bool CNetReplay::ReplayPacket(sizebuf_t *msg, netadr_t *from)
{
	ReplayConsoleText();

	int type = PeekEvent();
	if (type == EV_NO_PACKET)
	{
		ExpectEvent(EV_NO_PACKET);
		return false;
	}

	if (!ExpectEvent(EV_PACKET))
		return false;

	int32 size;
	if (!Read(from, sizeof(*from)) || !Read(&size, sizeof(size)) || size < 0 || size > msg->maxsize || !Read(msg->data, size))
	{
		Finish("truncated recording");
		return false;
	}

	msg->cursize = size;
	return true;
}

void CNetReplay::RecordConsoleText(const char *text)
{
	int32 len = Q_strlen(text);
	WriteEvent(EV_CONSOLE);
	Write(&len, sizeof(len));
	Write(text, len);
}

void CNetReplay::ReplayConsoleText()
{
	// Console text arrives between frames: replay it where the engine next asks for input
	while (!m_bFinished && PeekEvent() == EV_CONSOLE)
	{
		ExpectEvent(EV_CONSOLE);

		int32 len;
		char text[4096];
		if (!Read(&len, sizeof(len)) || len < 0 || len >= (int32)sizeof(text) || !Read(text, len))
		{
			Finish("truncated recording");
			return;
		}

		text[len] = '\0';
		Cbuf_AddText(text);
	}
}

int64 CNetReplay::Value(int64 live)
{
	if (IsRecording())
	{
		WriteEvent(EV_VALUE);
		Write(&live, sizeof(live));
	}
	else if (IsReplaying())
	{
		ReplayConsoleText();

		int64 recorded;
		if (ExpectEvent(EV_VALUE) && Read(&recorded, sizeof(recorded)))
			return recorded;
	}

	return live;
}

double CNetReplay::ValueF(double live)
{
	if (IsRecording())
	{
		WriteEvent(EV_VALUE_FLOAT);
		Write(&live, sizeof(live));
	}
	else if (IsReplaying())
	{
		ReplayConsoleText();

		double recorded;
		if (ExpectEvent(EV_VALUE_FLOAT) && Read(&recorded, sizeof(recorded)))
			return recorded;
	}

	return live;
}

void CNetReplay::RecordSend(int length, const void *data, const netadr_t &to)
{
	if (!m_Sends)
		return;

	// frame, address (IPv4 and port, network order), size, payload
	uint32 header[4];
	header[0] = m_Frame;
	header[1] = *(const uint32 *)to.ip;
	header[2] = to.port;
	header[3] = (uint32)length;
	fwrite(header, 1, sizeof(header), m_Sends);
	fwrite(data, 1, length, m_Sends);
	m_Sent++;
}
