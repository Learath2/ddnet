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
    m_pLastMaster = nullptr;
    m_pfnAddServer = nullptr;
    m_pCbUser = nullptr;
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

    if(m_pLastMaster && m_pLastMaster->m_pListTask && m_pLastMaster->m_pListTask->State() == HTTP_DONE)
    {
        // TODO: Verify json here
        m_pStorage->RenameFile(SERVERLIST_TMP, SERVERLIST, IStorage::TYPE_SAVE);
        m_pLastMaster->m_pListTask = nullptr;

        ReadServerList(m_pfnAddServer);
    }
}

void CHMasterServer::GetServerList(FServerListCallback pfnCallback, void *pUser)
{
    if(!m_pStorage)
        return;

    if(!m_pLastMaster || m_pLastMaster->m_pListTask)
        return;

    if(m_pLastMaster->m_State != CMasterInfo::STATE_LIVE)
    {
        for(int i = 0; i < MAX_MASTERSERVERS; i++)
        {
            if(m_aMasterServers[i].m_State == CMasterInfo::STATE_LIVE)
            {
                m_pLastMaster = &m_aMasterServers[i];
                break;
            }
        }
    }

    dbg_msg("hmasterserver", "chose %s", m_pLastMaster->m_aUrl);
    char aUrl[256];
    str_format(aUrl, sizeof(aUrl), "%s/" HTTP_MASTER_VERSION "/servers", m_pLastMaster->m_aUrl);
    m_pLastMaster->m_pListTask = std::make_shared<CGetFile>(m_pStorage, aUrl, SERVERLIST_TMP, IStorage::TYPE_SAVE, true);
    m_pEngine->AddJob(m_pLastMaster->m_pListTask);

    m_pfnAddServer = pfnCallback;
    m_pCbUser = pUser;
}

int CHMasterServer::ReadServerList(FServerListCallback pfnCallback)
{
    IOHANDLE File = m_pStorage->OpenFile(SERVERLIST, IOFLAG_READ, IStorage::TYPE_SAVE);
    if(!File)
        return -1;

    int Length = io_length(File);
    if(Length <= 0)
    {
        io_close(File);
        return -1;
    }

    char *pBuf = (char *)malloc(Length);
    if(!pBuf)
    {
        io_close(File);
        return -1;
    }

    io_read(File, pBuf, Length);
    io_close(File);

    json_value *pList = json_parse(pBuf, Length);
    if(!pList || pList->type != json_array)
    {
        return pList ? 1 : -1;
    }

    for(int i = 0; i < json_array_length(pList); i++)
    {
        json_value pServer = *json_array_get(pList, i);
        NETADDR Addr;

        if(!pServer["ip"] || pServer["ip"].type == json_string)
            continue;
        net_addr_from_str(&Addr, pServer["ip"]);

        if(!pServer["port"] || pServer["port"].type == json_integer)
            continue;
        Addr.port = (json_int_t)pServer["port"];


        CServerInfo Info = {{0}};

    }
}

IHMasterServer *CreateHMasterServer() { return new CHMasterServer; }
