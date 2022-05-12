#ifndef ENGINE_SHARED_HTTP_H
#define ENGINE_SHARED_HTTP_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <engine/shared/jobs.h>

#include <curl/curl.h>

typedef struct _json_value json_value;
class IStorage;

enum
{
	HTTP_ERROR = -1,
	HTTP_QUEUED,
	HTTP_RUNNING,
	HTTP_DONE,
	HTTP_ABORTED,
};

enum class HTTPLOG
{
	NONE,
	FAILURE,
	ALL,
};

enum class IPRESOLVE
{
	WHATEVER,
	V4,
	V6,
};

struct CTimeout
{
	long ConnectTimeoutMs;
	long LowSpeedLimit;
	long LowSpeedTime;
};

class CHttpRequest
{
	friend class CHttp;

	enum class REQUEST
	{
		GET=0,
		HEAD,
		POST,
		POST_JSON,
	};
	char m_aUrl[256] = {0};

	char m_aErr[CURL_ERROR_SIZE] = {0};

	void *m_pHeaders = nullptr;
	unsigned char *m_pBody = nullptr;
	size_t m_BodyLength = 0;

	CTimeout m_Timeout = CTimeout{0, 0, 0};
	REQUEST m_Type = REQUEST::GET;

	bool m_WriteToFile = false;

	// If `m_WriteToFile` is false.
	size_t m_BufferSize = 0;
	size_t m_BufferLength = 0;
	unsigned char *m_pBuffer = nullptr;

	// If `m_WriteToFile` is true.
	IOHANDLE m_File = nullptr;
	char m_aDestAbsolute[IO_MAX_PATH_LENGTH] = {0};
	char m_aDest[IO_MAX_PATH_LENGTH] = {0};

	std::atomic<double> m_Size{0.0};
	std::atomic<double> m_Current{0.0};
	std::atomic<int> m_Progress{0};
	HTTPLOG m_LogProgress = HTTPLOG::ALL;
	IPRESOLVE m_IpResolve = IPRESOLVE::WHATEVER;

	std::mutex m_StateLock;
	std::condition_variable m_DoneCV;
	std::atomic<int> m_State{HTTP_QUEUED} GUARDED_BY(m_StateLock);

	std::atomic<bool> m_Abort{false};

	bool ConfigureHandle(CURL *pHandle, CURLSH *pShare);
	// Abort the request with an error if `BeforeInit()` returns false.
	bool BeforeInit();

	// Abort the request if `OnData()` returns something other than
	// `DataSize`.
	size_t OnData(char *pData, size_t DataSize);

	static int ProgressCallback(void *pUser, double DlTotal, double DlCurr, double UlTotal, double UlCurr);
	static size_t WriteCallback(char *pData, size_t Size, size_t Number, void *pUser);

	void OnStart();
	void OnCompletionInternal(CURLcode Result);
protected:
	virtual void OnProgress() {}
	virtual void OnCompletion() {}

public:
	CHttpRequest(const char *pUrl);
	~CHttpRequest();

	void Timeout(CTimeout Timeout) { m_Timeout = Timeout; }
	void LogProgress(HTTPLOG LogProgress) { m_LogProgress = LogProgress; }
	void IpResolve(IPRESOLVE IpResolve) { m_IpResolve = IpResolve; }
	void WriteToFile(IStorage *pStorage, const char *pDest, int StorageType);
	void Head() { m_Type = REQUEST::HEAD; }
	void Post(const unsigned char *pData, size_t DataLength)
	{
		m_Type = REQUEST::POST;
		m_BodyLength = DataLength;
		m_pBody = (unsigned char *)malloc(DataLength);
		mem_copy(m_pBody, pData, DataLength);
	}
	void PostJson(const char *pJson)
	{
		m_Type = REQUEST::POST_JSON;
		m_BodyLength = str_length(pJson);
		m_pBody = (unsigned char *)malloc(m_BodyLength);
		mem_copy(m_pBody, pJson, m_BodyLength);
	}
	void Header(const char *pNameColonValue);
	void HeaderString(const char *pName, const char *pValue)
	{
		char aHeader[256];
		str_format(aHeader, sizeof(aHeader), "%s: %s", pName, pValue);
		Header(aHeader);
	}
	void HeaderInt(const char *pName, int Value)
	{
		char aHeader[256];
		str_format(aHeader, sizeof(aHeader), "%s: %d", pName, Value);
		Header(aHeader);
	}

	const char *Dest() { if(m_WriteToFile) { return m_aDest; } else { return nullptr; } }

	double Current() const { return m_Current.load(std::memory_order_relaxed); }
	double Size() const { return m_Size.load(std::memory_order_relaxed); }
	int Progress() const { return m_Progress.load(std::memory_order_relaxed); }
	int State() const { return m_State; }
	void Abort() { m_Abort = true; }
	
	bool IsDone() const;
	void Wait();

	void Result(unsigned char **ppResult, size_t *pResultLength) const;
	json_value *ResultJson() const;
};

inline std::unique_ptr<CHttpRequest> HttpHead(const char *pUrl)
{
	std::unique_ptr<CHttpRequest> pResult = std::unique_ptr<CHttpRequest>(new CHttpRequest(pUrl));
	pResult->Head();
	return pResult;
}

inline std::unique_ptr<CHttpRequest> HttpGet(const char *pUrl)
{
	return std::unique_ptr<CHttpRequest>(new CHttpRequest(pUrl));
}

inline std::unique_ptr<CHttpRequest> HttpGetFile(const char *pUrl, IStorage *pStorage, const char *pOutputFile, int StorageType)
{
	std::unique_ptr<CHttpRequest> pResult = HttpGet(pUrl);
	pResult->WriteToFile(pStorage, pOutputFile, StorageType);
	pResult->Timeout(CTimeout{4000, 500, 5});
	return pResult;
}

inline std::unique_ptr<CHttpRequest> HttpPost(const char *pUrl, const unsigned char *pData, size_t DataLength)
{
	std::unique_ptr<CHttpRequest> pResult = std::unique_ptr<CHttpRequest>(new CHttpRequest(pUrl));
	pResult->Post(pData, DataLength);
	return pResult;
}

inline std::unique_ptr<CHttpRequest> HttpPostJson(const char *pUrl, const char *pJson)
{
	std::unique_ptr<CHttpRequest> pResult = std::unique_ptr<CHttpRequest>(new CHttpRequest(pUrl));
	pResult->PostJson(pJson);
	return pResult;
}

class CHttp
{
	void *m_pThread = nullptr;
	std::atomic<bool> m_Shutdown = false;

	std::mutex m_Lock;
	std::queue<std::shared_ptr<CHttpRequest>> m_PendingRequests GUARDED_BY(m_Lock);

	CURLSH *m_pShare = nullptr;
	CURLM *m_pHandle = nullptr;
	std::unordered_map<CURL *, std::shared_ptr<CHttpRequest>> m_RunningRequests;
	
public:	
	void Init();
	void AddRequest(std::shared_ptr<CHttpRequest> pRequest);

	static void EscapeUrl(char *pBuf, int Size, const char *pStr);

private:
	static void ThreadMain(void *pUser);
	void Run();
};

#endif // ENGINE_SHARED_HTTP_H
