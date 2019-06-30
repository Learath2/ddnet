/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>
#include <engine/shared/network.h>
#include <engine/shared/config.h>
#include <engine/engine.h>
#include <engine/console.h>
#include <engine/masterserver.h>

#include <engine/external/json-parser/json.h>
#include <engine/external/json-builder/json-builder.h>

#include <mastersrv/mastersrv.h>

#include "register.h"

CRegister::CRegister()
{
	m_pMasterServer = 0;
	m_pConsole = 0;

	m_RegisterState = REGISTERSTATE_START;
	m_RegisterStateStart = 0;
	m_RegisterFirst = 1;
	m_RegisterCount = 0;

	mem_zero(m_aMasterserverInfo, sizeof(m_aMasterserverInfo));
	m_RegisterRegisteredServer = -1;
}

void CRegister::RegisterNewState(int State)
{
	m_RegisterState = State;
	m_RegisterStateStart = time_get();
}

void CRegister::Init(IEngine *pEngine, IEngineMasterServer *pMasterServer, IConsole *pConsole)
{
	m_pEngine = pEngine;
	m_pMasterServer = pMasterServer;
	m_pConsole = pConsole;
}

void CRegister::RegisterUpdate(int Nettype)
{
	int64 Now = time_get();
	int64 Freq = time_freq();

	if(!g_Config.m_SvRegister)
		return;

	m_pMasterServer->Update();

	if(m_RegisterState == REGISTERSTATE_START)
	{
		m_RegisterCount = 0;
		m_RegisterFirst = 1;
		RegisterNewState(REGISTERSTATE_GETSTATUS);
		m_pMasterServer->RefreshAddresses(Nettype);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "refreshing ip addresses");
	}
	else if(m_RegisterState == REGISTERSTATE_GETSTATUS)
	{
		if(!m_pMasterServer->IsRefreshing())
		{
			for(int i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
			{
				CMasterserverInfo *pMaster = &m_aMasterserverInfo[i];
				if(!m_pMasterServer->IsValid(i))
				{
					pMaster->m_State = MASTERSTATE_INVALID;
					continue;
				}

				NETADDR Addr = m_pMasterServer->GetAddr(i);
				pMaster->m_Addr = Addr;
				pMaster->m_State = MASTERSTATE_START;
				pMaster->m_LastSend = 0;

				char aUrl[256];
				m_pMasterServer->GetURL(i, aUrl, sizeof(aUrl));
				str_append(aUrl, "/" HTTP_MASTER_VERSION "/status", sizeof(aUrl));

				pMaster->m_pTask = std::make_shared<CGet>(aUrl, true);
				m_pEngine->AddJob(pMaster->m_pTask);
			}

			m_pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "fetching server status");
			RegisterNewState(REGISTERSTATE_RUNNING);
		}
	}
	else if(m_RegisterState == REGISTERSTATE_RUNNING)
	{
		for(int i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
		{
			CMasterserverInfo *pMaster = &m_aMasterserverInfo[i];
			switch(pMaster->m_State)
			{
			case MASTERSTATE_START:
				if(pMaster->m_pTask)
				{
					switch(pMaster->m_pTask->State())
					{
					case HTTP_DONE:
						const json_value *s = pMaster->m_pTask->ResultJson();
						const char *Status = json_string_get(json_object_get(s, "status"));
						if(!str_comp(Status, "ready"))
						{
							pMaster->m_pTask = nullptr;
							pMaster->m_State = MASTERSTATE_LIVE;
						}
						else
						{
							pMaster->m_State = MASTERSTATE_ERROR;
							dbg_msg("register", "server %d is %s, trying again in %d", i, Status, 15);
						}
						goto next;
					case HTTP_ERROR:
						pMaster->m_State = MASTERSTATE_ERROR;
						dbg_msg("register", "server %d is dead, trying again in %d", i, 15);
						goto next;
					}
				}
				break;
			case MASTERSTATE_LIVE:
				if(!pMaster->m_pTask && Now > pMaster->m_LastSend + Freq * 15)
				{

				}
			}
			next:;
		}

	}
	else if(m_RegisterState == REGISTERSTATE_ERROR)
	{
		// check for restart
		if(Now > m_RegisterStateStart+Freq*60)
			RegisterNewState(REGISTERSTATE_START);
	}
}
