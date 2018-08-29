#include <stdio.h>
#include <base/system.h>
#include <base/math.h>
#include <mastersrv/mastersrv.h>
#include <engine/kernel.h>
#include <engine/config.h>
#include <engine/shared/config.h>
#include <engine/shared/network.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/packer.h>
#include <engine/client.h>

#include <game/generated/protocol.h>
#include <game/version.h>

static CNetClient g_NetClient;
static int g_State;
static bool g_Running = true;
static char g_Password[32];

static CNetObjHandler g_NetObjHandler;

int SendMsgEx(CMsgPacker *pMsg, int Flags, bool System = true)
{
	CNetChunk Packet;

	if(g_State == IClient::STATE_OFFLINE)
		return 0;

	mem_zero(&Packet, sizeof(CNetChunk));

	Packet.m_ClientID = 0;
	Packet.m_pData = pMsg->Data();
	Packet.m_DataSize = pMsg->Size();

	// HACK: modify the message id in the packet and store the system flag
	if(*((unsigned char*)Packet.m_pData) == 1 && System && Packet.m_DataSize == 1)
	{
		dbg_break();
	}

	*((unsigned char*)Packet.m_pData) <<= 1;
	if(System)
		*((unsigned char*)Packet.m_pData) |= 1;

	if(Flags&MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags&MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;

	if(!(Flags&MSGFLAG_NOSEND))
	{
		g_NetClient.Send(&Packet);
	}

	return 0;
}

void SetState(int s)
{
	if(s == IClient::STATE_ONLINE)
		dbg_msg("client", "connected");

	g_State = s;
}

void Connect(NETADDR *Addr)
{
	g_NetClient.Connect(Addr);
	SetState(IClient::STATE_CONNECTING);
}

void Disconnect()
{
	g_NetClient.Disconnect("Leaving");
	SetState(IClient::STATE_OFFLINE);
}

void SendInfo()
{
	CMsgPacker Msg(NETMSG_INFO);
	Msg.AddString(GAME_NETVERSION, 128);
	Msg.AddString(g_Password, 128);
	SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH);
}

void SendReady()
{
	CMsgPacker Msg(NETMSG_READY);
	SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH);
}

void SendStartInfo()
{
	CNetMsg_Cl_StartInfo Msg;
	Msg.m_pName = g_Config.m_PlayerName;
	Msg.m_pClan = g_Config.m_PlayerClan;
	Msg.m_Country = g_Config.m_PlayerCountry;
	Msg.m_pSkin = g_Config.m_ClPlayerSkin;
	Msg.m_UseCustomColor = g_Config.m_ClPlayerUseCustomColor;
	Msg.m_ColorBody = g_Config.m_ClPlayerColorBody;
	Msg.m_ColorFeet = g_Config.m_ClPlayerColorFeet;
	CMsgPacker Packer(Msg.MsgID());
	Msg.Pack(&Packer);

	SendMsgEx(&Packer, MSGFLAG_VITAL, false);
}

void Say(int Team, const char *pLine)
{
	CNetMsg_Cl_Say Msg;
	Msg.m_Team = Team;
	Msg.m_pMessage = pLine;
	CMsgPacker Packer(Msg.MsgID());
	Msg.Pack(&Packer);

	SendMsgEx(&Packer, MSGFLAG_VITAL, false);
}

void SendEnterGame()
{
	CMsgPacker Msg(NETMSG_ENTERGAME);
	SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH);
}

void Tick()
{
	g_NetClient.Update();

	if(g_State != IClient::STATE_OFFLINE && g_State != IClient::STATE_QUITING && g_NetClient.State() == NETSTATE_OFFLINE)
	{
		SetState(IClient::STATE_OFFLINE);
		Disconnect();
		dbg_msg("client", "offline %d error='%s'", g_NetClient.State(), g_NetClient.ErrorString());
	}

	//
	if(g_State == IClient::STATE_CONNECTING && g_NetClient.State() == NETSTATE_ONLINE)
	{
		// we switched to online
		dbg_msg("client", "connected, sending info");
		SetState(IClient::STATE_LOADING);
		SendInfo();
	}

	CNetChunk Packet;
	while(g_NetClient.Recv(&Packet))
	{
		CUnpacker Unpacker;
		Unpacker.Reset(Packet.m_pData, Packet.m_DataSize);
		CMsgPacker Packer(NETMSG_EX);

		int Msg;
		bool Sys;
		CUuid Uuid;

		int Result = UnpackMessageID(&Msg, &Sys, &Uuid, &Unpacker, &Packer);
		if(Result == UNPACKMESSAGE_ERROR)
		{
			return;
		}
		else if(Result == UNPACKMESSAGE_ANSWER)
		{
			SendMsgEx(&Packer, MSGFLAG_VITAL, true);
		}

		char aBuf[UUID_MAXSTRSIZE];
		FormatUuid(Uuid, aBuf, sizeof aBuf);
		//dbg_msg("client", "Packet %d %s %s", Msg, Sys?"true":"false", aBuf);
		if(Sys)
		{
			if(Msg == NETMSG_MAP_CHANGE)
			{
				SendReady();
			}
			if(Msg == NETMSG_CON_READY)
			{
				SendStartInfo();
				SendEnterGame();
			}
			else if(Msg == NETMSG_SNAP || Msg == NETMSG_SNAPSINGLE || Msg == NETMSG_SNAPEMPTY)
			{
				if(g_State != IClient::STATE_ONLINE)
					SetState(IClient::STATE_ONLINE);
			}
		}
		else
		{
			if(Msg != NETMSGTYPE_SV_TUNEPARAMS || Msg != NETMSGTYPE_SV_EXTRAPROJECTILE)
			{
				void *pRawMsg = g_NetObjHandler.SecureUnpackMsg(Msg, &Unpacker);
				if(!pRawMsg)
				{
					dbg_msg("client", "dropped weird message '%s' (%d), failed on '%s'", g_NetObjHandler.GetMsgName(Msg), Msg, g_NetObjHandler.FailedMsgOn());
					return;
				}

				if(Msg == NETMSGTYPE_SV_CHAT)
				{
					CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
					dbg_msg("chat", "%d: %s", pMsg->m_ClientID, pMsg->m_pMessage);
				}
			}
		}
	}
}

int main(int argc, char **argv)
{
	dbg_logger_stdout();

	if(argc < 2 || argc > 3)
	{
		fprintf(stderr, "usage: %s server[:port] [password](default port: 8303)\n", argv[0]);
		return 1;
	}

	net_init();
	CNetBase::Init();

	NETADDR Addr;
	if (net_host_lookup(argv[1], &Addr, NETTYPE_ALL))
	{
		fprintf(stderr, "host lookup failed\n");
		return 1;
	}

	if(Addr.port == 0)
		Addr.port = 8303;

	if(secure_random_init() != 0)
	{
		fprintf(stderr, "couldn't initialize secure_random\n");
		return 1;
	}

	IKernel *pKernel = IKernel::Create();
	IConfig *pConfig = CreateConfig();

	pKernel->RegisterInterface(pConfig);

	pConfig->Init();

	CNetBase::OpenLog(io_open("sent", IOFLAG_WRITE), io_open("recieveth", IOFLAG_WRITE));

	NETADDR BindAddr;
	mem_zero(&BindAddr, sizeof(BindAddr));
	BindAddr.type = NETTYPE_ALL;
	BindAddr.port = (secure_rand() % 64511) + 1024;

	g_NetClient.Open(BindAddr, 0);

	if(argc == 3)
		str_copy(g_Password, argv[2], sizeof g_Password);

	Connect(&Addr);

	while(g_Running)
	{
		Tick();
	}
}
