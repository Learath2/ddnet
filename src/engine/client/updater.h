#ifndef ENGINE_CLIENT_UPDATER_H
#define ENGINE_CLIENT_UPDATER_H

#include <engine/client/http.h>
#include <engine/updater.h>
#include <map>
#include <string>
#include <vector>

#define CLIENT_EXEC "DDNet"
#define SERVER_EXEC "DDNet-Server"

#if defined(CONF_FAMILY_WINDOWS)
#define PLAT_EXT ".exe"
#define PLAT_NAME CONF_PLATFORM_STRING
#elif defined(CONF_FAMILY_UNIX)
#define PLAT_EXT ""
#if defined(CONF_ARCH_IA32)
#define PLAT_NAME CONF_PLATFORM_STRING "-x86"
#elif defined(CONF_ARCH_AMD64)
#define PLAT_NAME CONF_PLATFORM_STRING "-x86_64"
#else
#define PLAT_NAME CONF_PLATFORM_STRING "-unsupported"
#endif
#else
#define PLAT_EXT ""
#define PLAT_NAME "unsupported-unsupported"
#endif

#define PLAT_CLIENT_DOWN CLIENT_EXEC "-" PLAT_NAME PLAT_EXT
#define PLAT_SERVER_DOWN SERVER_EXEC "-" PLAT_NAME PLAT_EXT

#define PLAT_CLIENT_EXEC CLIENT_EXEC PLAT_EXT
#define PLAT_SERVER_EXEC SERVER_EXEC PLAT_EXT

class CUpdaterFetchTask;

class CUpdater : public IUpdater
{
	friend class CUpdaterFetchTask;

	class IClient *m_pClient;
	class IStorage *m_pStorage;
	class IEngine *m_pEngine;

	bool m_IsWinXP;

	LOCK m_Lock;

	std::atomic<int> m_State;
	char m_aClientExecTmp[64];
	char m_aServerExecTmp[64];

	bool m_ClientUpdate;
	bool m_ServerUpdate;

	std::map<std::string, bool> m_FileJobs;
	std::vector<std::shared_ptr<CUpdaterFetchTask>> m_FetchJobs;
	int64 m_DownloadStart;
	std::atomic<int> m_TotalDownloaded;
	std::atomic<unsigned> m_CompletedFetchJobs;
	bool m_PreventRestart;

	std::shared_ptr<CUpdaterFetchTask> m_ManifestJob;

	std::shared_ptr<CUpdaterFetchTask> FetchFile(const char *pFile, const char *pDestPath = 0);
	void OnProgress();
	bool MoveFile(const char *pFile);

	std::map<std::string, bool> ParseUpdate();
	void CommitUpdate();

	bool ReplaceClient();
	bool ReplaceServer();

public:
	CUpdater();

	int State() const { return m_State; }
	float Progress() const;
	char *Speed(char *pBuf, int BufSize) const;

	virtual void InitiateUpdate();
	void PerformUpdate(const std::map<std::string, bool> &Jobs, bool PreventRestart = false);
	void Init();
	virtual void Update();
	void WinXpRestart();
};

#endif
