#ifndef ENGINE_HMASTERSERVER_H
#define ENGINE_HMASTERSERVER_H

#include "kernel.h"

class IHMasterServer : public IInterface
{
	MACRO_INTERFACE("hmasterserver", 0)
public:

	enum
	{
		MAX_MASTERSERVERS=4
	};

	virtual void Init(IEngine *pEngine, IStorage *pStorage) = 0;
	virtual void Update() = 0;

	virtual int Load() = 0;
	virtual int Save() = 0;
};

extern IHMasterServer *CreateHMasterServer();

#endif // ENGINE_HMASTERSERVER_H
