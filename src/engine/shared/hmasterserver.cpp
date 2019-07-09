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
        str_format(Info.m_aUrl, sizeof(Info.m_aUrl), "https://master%d.ddnet.tw", m_Count);
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
        str_copy(Info.m_aUrl, pLine, sizeof(Info.m_aUrl));
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
            pMaster->GetEndpoint(aUrl, sizeof(aUrl), "status");
            pMaster->m_pStatusTask = std::make_shared<CGet>(aUrl, true, true);
            m_pEngine->AddJob(pMaster->m_pStatusTask);

            pMaster->m_LastTry = time_get();
            pMaster->m_Tries++;

            pMaster->m_State = CMasterInfo::STATE_REFRESHING;
            break;

        case CMasterInfo::STATE_REFRESHING:
            dbg_assert((bool)pMaster->m_pStatusTask, "missing status task");
            if(pMaster->m_pStatusTask->State() == HTTP_DONE)
            {
                const json_value Status = *pMaster->m_pStatusTask->ResultJson();
                pMaster->m_State = Status.type == json_object && Status["status"].type == json_string && !str_comp(Status["status"], "ready") ?
                                        CMasterInfo::STATE_LIVE : CMasterInfo::STATE_ERROR;
                pMaster->m_pStatusTask = nullptr;
            }
            else if(pMaster->m_pStatusTask->State() != HTTP_RUNNING && pMaster->m_pStatusTask->State() != HTTP_QUEUED){
                dbg_msg("hmasterserver", "masterserver %d failed to respond or isn't ready", i);
                pMaster->m_pStatusTask = nullptr;
                pMaster->m_State = CMasterInfo::STATE_ERROR;
            }
            break;

        case CMasterInfo::STATE_ERROR:
            if(pMaster->m_Tries > 2)
            {
                dbg_msg("hmasterserver", "invalidating masterserver %d", i);
                pMaster->m_State = CMasterInfo::STATE_INVALID;
            }
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

        ReadServerList(m_pfnAddServer, m_pCbUser);
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
    m_pLastMaster->GetEndpoint(aUrl, sizeof(aUrl), "servers");
    m_pLastMaster->m_pListTask = std::make_shared<CGetFile>(m_pStorage, aUrl, SERVERLIST_TMP, IStorage::TYPE_SAVE, true);
    m_pEngine->AddJob(m_pLastMaster->m_pListTask);

    m_pfnAddServer = pfnCallback;
    m_pCbUser = pUser;
}

int CHMasterServer::ParseInfo(json_value Data, CServerInfo &Info)
{
    // Parse info
    if(Data["name"].type != json_string)
        return 1;

    str_copy(Info.m_aName, Data["name"], sizeof(Info.m_aName));

    if(Data["game_type"].type != json_string)
        return 1;

    str_copy(Info.m_aGameType, Data["game_type"], sizeof(Info.m_aGameType));

    if(Data["version"].type != json_string)
        return 1;

    str_copy(Info.m_aVersion, Data["version"], sizeof(Info.m_aVersion));

    if(Data["passworded"].type != json_boolean)
        return 1;

    Info.m_Flags = Data["passworded"] ? SERVER_FLAG_PASSWORD : 0;

    if(Data["max_players"].type != json_integer)
        return 1;

    Info.m_MaxPlayers = Data["max_players"];

    if(Data["max_clients"].type != json_integer)
        return 1;

    Info.m_MaxClients = Data["max_clients"];

    // Parse map
    json_value Map = Data["map"];
    if(Map.type != json_object)
        return 1;

    if(Map["name"].type != json_string)
        return 1;

    str_copy(Info.m_aMap, Map["name"], sizeof(Info.m_aMap));

    if(Map["size"].type != json_integer)
        return 1;

    Info.m_MapSize = Map["size"];

    if(Map["crc32"].type != json_string || !str_isallhex(Map["crc32"]))
        return 1;

    Info.m_MapCrc = str_toint_base(Map["crc32"], 16);

    if(Map["sha256"].type != json_string || sha256_from_str(&Info.m_MapSha, Map["sha256"]))
    {
        ; // Leave sha256 optional for now
    }

    // Parse clients
    if(Data["clients"].type != json_array)
        return 1;

    for(int i = 0; i < json_array_length(&Data["clients"]) && i < MAX_CLIENTS; i++)
    {
        json_value c = Data["clients"][i];
        if(c.type != json_object)
            return 1;

        CServerInfo::CClient *pClient = &Info.m_aClients[i];

        if(c["name"].type != json_string)
            return 1;

        str_copy(pClient->m_aName, c["name"], sizeof(pClient->m_aName));

        if(c["clan"].type != json_string)
            return 1;

        str_copy(pClient->m_aClan, c["clan"], sizeof(pClient->m_aClan));

        if(c["country"].type != json_integer)
            return 1;

        pClient->m_Country = c["country"];

        if(c["score"].type != json_integer)
            return 1;

        pClient->m_Score = c["score"];
    }

    return 0;
}

int CHMasterServer::ReadServerList(FServerListCallback pfnCallback, void *pUser)
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

    int Count = 0;
    for(int i = 0; i < json_array_length(pList); i++)
    {
        json_value pServer = *json_array_get(pList, i);
        NETADDR Addr;

        if(pServer["ip"].type != json_string)
            continue;
        net_addr_from_str(&Addr, pServer["ip"]);

        if(pServer["port"].type != json_integer)
            continue;
        Addr.port = (json_int_t)pServer["port"];

        CServerInfo Info = {0};
        if(pServer["info"].type != json_object || ParseInfo(pServer["info"], Info))
        {
            pfnCallback(Addr, nullptr, pUser);
            Count++;
            continue;
        }

        pfnCallback(Addr, &Info, pUser);
        Count++;
    }

    return Count;
}

IHMasterServer *CreateHMasterServer() { return new CHMasterServer; }
