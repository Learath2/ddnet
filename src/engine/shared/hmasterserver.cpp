#include <engine/engine.h>
#include <engine/storage.h>
#include <engine/hmasterserver.h>

#include <engine/shared/http.h>
#include <engine/shared/linereader.h>

#include <engine/external/json-parser/json.h>

#include "hmasterserver.h"

CHMasterServer::CHMasterServer()
{
    mem_zero(m_aMasterServers, sizeof(m_aMasterServers));
    m_Count = 0;
    m_pStorage = 0;
    m_pEngine = 0;
}

void CHMasterServer::Init(IEngine *pEngine, IStorage *pStorage)
{
    m_pStorage = pStorage;
    m_pEngine = pEngine;
}

int CHMasterServer::LoadDefaults()
{
    for(m_Count = 0; m_Count < MAX_MASTERSERVERS; m_Count++)
    {
        CMasterInfo Info;
        str_format(Info.m_aUrl, sizeof(Info.m_aUrl), "https://master%d.ddnet.tw/" HTTP_MASTER_VERSION, m_Count);
        m_aMasterServers[m_Count] = Info;
    }

    return 0;
}

int CHMasterServer::Load()
{
    if(!m_pStorage)
        return -1;

    IOHANDLE File = m_pStorage->OpenFile("hmasters.cfg", IOFLAG_READ, IStorage::TYPE_SAVE);
    if(!File)
        return LoadDefaults();

    CLineReader Reader;
	Reader.Init(File);

    char *pLine = nullptr;
    for(m_Count = 0; m_Count < MAX_MASTERSERVERS && (pLine = Reader.Get()); m_Count++)
    {
        CMasterInfo Info;
        str_format(Info.m_aUrl, sizeof(Info.m_aUrl), "%s/" HTTP_MASTER_VERSION, pLine);
        m_aMasterServers[m_Count] = Info;
    }

    io_close(File);
    return 0;
}

int CHMasterServer::Save()
{
    if(!m_pStorage)
        return -1;

    IOHANDLE File = m_pStorage->OpenFile("hmasters.cfg", IOFLAG_WRITE, IStorage::TYPE_SAVE);
    if(!File)
        return -1;

    for(int i = 0; i < m_Count; i++)
    {
        if(m_aMasterServers[i].m_State == CMasterInfo::STATE_INVALID)
            continue;

        io_write(File, m_aMasterServers[i].m_aUrl, str_length(m_aMasterServers[i].m_aUrl));
        io_write_newline(File);
    }

    io_write_newline(File);
    io_close(File);
    return 0;
}

void CHMasterServer::Refresh(bool Force)
{
    for(int i = 0; i < m_Count; i++)
    {
        if(m_aMasterServers[i].m_State != CMasterInfo::STATE_INVALID || Force)
            m_aMasterServers[i].m_State = CMasterInfo::STATE_STALE;
    }
}

void CHMasterServer::Update()
{
    for(int i = 0; i < m_Count; i++)
    {
        CMasterInfo *pMaster = &m_aMasterServers[i];
        switch(pMaster->m_State)
        {
        case CMasterInfo::STATE_STALE:
            char aUrl[256];
            str_format(aUrl, sizeof(aUrl), "%s/status", pMaster->m_aUrl);
            pMaster->m_pStatusTask = std::make_shared<CGet>(aUrl, true);
            m_pEngine->AddJob(pMaster->m_pStatusTask);

            pMaster->m_LastTry = time_get();
            pMaster->m_Tries++;

            pMaster->m_State = CMasterInfo::STATE_REFRESHING;
            break;

        case CMasterInfo::STATE_REFRESHING:
            dbg_assert((bool)pMaster->m_pStatusTask, "missing status task");
            if(pMaster->m_pStatusTask->State() == HTTP_DONE)
            {
                const json_value pStatus = *pMaster->m_pStatusTask->ResultJson();
                pMaster->m_State = pStatus["status"] && !str_comp(pStatus["status"], "ready") ?
                                        CMasterInfo::STATE_LIVE : CMasterInfo::STATE_ERROR;
                pMaster->m_pStatusTask = nullptr;
            }
            else if(pMaster->m_pStatusTask->State() != HTTP_RUNNING)
                dbg_msg("hmasterserver", "masterserver %d failed to respond or isn't ready", i);
            break;

        case CMasterInfo::STATE_ERROR:
            if(pMaster->m_Tries > 5)
                pMaster->m_State = CMasterInfo::STATE_INVALID;
            else if(time_get() > pMaster->m_LastTry + time_freq() * 15)
            {
                pMaster->m_State = CMasterInfo::STATE_STALE;
            }
            break;
        }
    }
}

IHMasterServer *CreateHMasterServer() { return new CHMasterServer; }
