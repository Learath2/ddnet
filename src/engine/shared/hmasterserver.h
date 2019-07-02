#ifndef ENGINE_SHARED_HMASTERSERVER_H
#define ENGINE_SHARED_HMASTERSERVER_H

#include <memory>

#include <engine/shared/http.h>
#include <engine/hmasterserver.h>

#define HTTP_MASTER_VERSION "v1"

class CHMasterServer : public IHMasterServer
{
    struct CMasterInfo
    {
        char m_aUrl[128];

        enum
        {
            STATE_STALE=0,
            STATE_REFRESHING,
            STATE_LIVE,
            STATE_ERROR,
            STATE_INVALID,
        };
        int m_State;

        int m_Tries;
        int64 m_LastTry;

        std::shared_ptr<CGet> m_pStatusTask;

        CMasterInfo() : m_aUrl(""), m_State(STATE_STALE), m_Tries(0), m_LastTry(0), m_pStatusTask(nullptr) {}
    };
    CMasterInfo m_aMasterServers[IHMasterServer::MAX_MASTERSERVERS];
    int m_Count;

    class IEngine *m_pEngine;
    class IStorage *m_pStorage;

public:
    CHMasterServer();

    void Init(class IEngine *pEngine, class IStorage *pStorage);
    void Refresh(bool Force);
    void Update();

    int LoadDefaults();

    int Save();
    int Load();
};

#endif // ENGINE_SHARED_HMASTERSERVER_H
