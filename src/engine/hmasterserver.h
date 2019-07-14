#ifndef ENGINE_HMASTERSERVER_H
#define ENGINE_HMASTERSERVER_H

#include <engine/serverbrowser.h>

#include "kernel.h"

typedef void (*FServerListCallback)(NETADDR Addr, const CServerInfo *pInfo, void *pUser);

class IHMasterServer : public IInterface
{
	MACRO_INTERFACE("hmasterserver", 0)
public:

	enum
	{
		MAX_MASTERSERVERS=4
	};

	virtual void Init(class IEngine *pEngine, class IStorage *pStorage) = 0;
	virtual void Update() = 0;

	virtual void GetServerList(FServerListCallback pfnCallback, void *pUser) = 0;
	virtual int ReadServerList(FServerListCallback pfnCallback, void *pUser) = 0;

	virtual void RegisterUpdate(int Port, const char *pSecret, CServerInfo *pInfo) = 0;

	virtual int Load() = 0;
	virtual int Save() = 0;
};

extern IHMasterServer *CreateHMasterServer();

#endif // ENGINE_HMASTERSERVER_H
