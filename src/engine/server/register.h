/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SERVER_REGISTER_H
#define ENGINE_SERVER_REGISTER_H

#include <memory>

#include <engine/shared/http.h>
#include <engine/masterserver.h>

class CRegister
{
	enum
	{
		REGISTERSTATE_START=0,
		REGISTERSTATE_GETSTATUS,
		REGISTERSTATE_RUNNING,
		REGISTERSTATE_HEARTBEAT,
		REGISTERSTATE_REGISTERED,
		REGISTERSTATE_ERROR,

		MASTERSTATE_START=0,
		MASTERSTATE_LIVE,
		MASTERSTATE_ERROR,
		MASTERSTATE_INVALID,
	};

	struct CMasterserverInfo
	{
		NETADDR m_Addr;
		int64 m_LastSend;

		int m_State;
		std::shared_ptr<CGet> m_pTask;
	};

	class IEngine *m_pEngine;
	class IEngineMasterServer *m_pMasterServer;
	class IConsole *m_pConsole;

	int m_RegisterState;
	int64 m_RegisterStateStart;
	int m_RegisterFirst;
	int m_RegisterCount;

	CMasterserverInfo m_aMasterserverInfo[IMasterServer::MAX_MASTERSERVERS];
	int m_RegisterRegisteredServer;

	void RegisterNewState(int State);
	void RegisterSendFwcheckresponse(NETADDR *pAddr);
	void RegisterSendHeartbeat(NETADDR Addr);
	void RegisterSendCountRequest(NETADDR Addr);
	void RegisterGotCount(struct CNetChunk *pChunk);

public:
	CRegister();
	void Init(class IEngine *pEngine, class IEngineMasterServer *pMasterServer, class IConsole *pConsole);
	void RegisterUpdate(int Nettype);
};

#endif
