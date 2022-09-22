#include "http.h"

#include <base/log.h>
#include <base/math.h>
#include <base/system.h>
#include <engine/external/json-parser/json.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <game/version.h>

#if !defined(CONF_FAMILY_WINDOWS)
#include <csignal>
#endif

#define WIN32_LEAN_AND_MEAN
#include <curl/curl.h>

char g_WakeUp = 'w';

CHttpRunner gs_Runner;

bool CHttpRunner::Init()
{
#if !defined(CONF_FAMILY_WINDOWS)
	// As a multithreaded application we have to tell curl to not install signal
	// handlers and instead ignore SIGPIPE from OpenSSL ourselves.
	signal(SIGPIPE, SIG_IGN);
#endif

	// Create wakeup pair
#if defined(CONF_FAMILY_UNIX)
	if(io_pipe(m_WakeUpPair))
		return true;
#elif defined(CONF_FAMILY_WINDOWS)
	if((m_WakeUpPair[0] = m_WakeUpPair[1] = net_loop_create()) == -1)
		return true;
#endif

	m_pThread = thread_init(CHttpRunner::ThreadMain, this, "http_runner");

	std::unique_lock l(m_Lock);
	m_Cv.wait(l, [this]() { return m_State != UNINITIALIZED; });
	if(m_State != RUNNING)
	{
#if defined(CONF_FAMILY_UNIX)
		io_pipe_close(m_WakeUpPair);
#elif defined(CONF_FAMILY_WINDOWS)
		net_loop_close(m_WakeUpPair[0]);
#endif

		return true;
	}

	return false;
}

void CHttpRunner::WakeUp()
{
//TODO: Check with TSA to make sure we hold m_Lock
#if defined(CONF_FAMILY_UNIX)
	(void)io_pipe_write(m_WakeUpPair[1], &g_WakeUp, sizeof(g_WakeUp));
#elif defined(CONF_FAMILY_WINDOWS)
	(void)net_loop_send(m_WakeUpPair[1], &g_WakeUp, sizeof(g_WakeUp));
#endif
}

void CHttpRunner::Run(std::shared_ptr<IEngineRunnable> pRunnable)
{
	std::unique_lock l(m_Lock);
	auto pHttpRunnable = std::static_pointer_cast<CHttpRunnable>(pRunnable);
	if(auto pRequest = std::dynamic_pointer_cast<CHttpRequest>(pHttpRunnable))
	{
		if(m_State != RUNNING)
		{
			dbg_msg("http", "Discarding request due to %s", m_State == SHUTDOWN ? "shutdown" : "irrecoverable error");
			pRequest->m_State = HTTP_ABORTED;
			pRequest->SetStatus(IEngineRunnable::DONE);
			return;
		}
		pRequest->SetStatus(IEngineRunnable::RUNNING);
		m_PendingRequests.emplace(std::move(pRequest));
		WakeUp();
	}
	else
	{
		dbg_assert(false, "Unknown http runnable type");
	}
}

void CHttpRunner::ThreadMain(void *pUser)
{
	CHttpRunner *pSelf = static_cast<CHttpRunner *>(pUser);
	pSelf->RunLoop();
}

void Discard(int fd)
{
	char aBuf[32];
	while(
#if defined(CONF_FAMILY_UNIX)
		io_pipe_read(fd, aBuf, sizeof(aBuf)) >= 0
#elif defined(CONF_FAMILY_WINDOWS)
		net_loop_recv(fd, aBuf, sizeof(aBuf))
#endif
	)
		;
}

void CHttpRunner::RunLoop()
{
	std::unique_lock InitL(m_Lock);
	if(curl_global_init(CURL_GLOBAL_DEFAULT))
	{
		dbg_msg("http", "curl_global_init failed");
		m_State = ERROR;
		m_Cv.notify_one();
		return;
	}

	CURLM *pMultiH = curl_multi_init();
	if(!pMultiH)
	{
		dbg_msg("http", "curl_multi_init failed");
		m_State = ERROR;
		m_Cv.notify_one();
		return;
	}

	// print curl version
	{
		curl_version_info_data *pVersion = curl_version_info(CURLVERSION_NOW);
		dbg_msg("http", "libcurl version %s (compiled = " LIBCURL_VERSION ")", pVersion->version);
	}

	m_State = RUNNING;
	m_Cv.notify_one();
	InitL.unlock();
	InitL.release();

	curl_waitfd ExtraFds[] = {{static_cast<curl_socket_t>(m_WakeUpPair[0]), CURL_POLL_IN, 0}};
	while(m_State == RUNNING)
	{
		int Events = 0;
		CURLMcode mc = curl_multi_wait(pMultiH, ExtraFds, sizeof(ExtraFds) / sizeof(ExtraFds[0]), 1000000, &Events);

		std::unique_lock LoopL(m_Lock);
		if(m_State != RUNNING)
			break;

		if(mc != CURLM_OK)
		{
			dbg_msg("http", "Failed multi wait: %s", curl_multi_strerror(mc));
			m_State = ERROR;
			break;
		}
		LoopL.unlock();

		// Discard data on the wakeup pair
		Discard(m_WakeUpPair[0]);

		mc = curl_multi_perform(pMultiH, &Events);
		if(mc != CURLM_OK)
		{
			dbg_msg("http", "Failed multi perform: %s", curl_multi_strerror(mc));
			LoopL.lock();
			m_State = ERROR;
			break;
		}

		struct CURLMsg *m;
		while((m = curl_multi_info_read(pMultiH, &Events)))
		{
			if(m->msg == CURLMSG_DONE)
			{
				auto RequestIt = m_RunningRequests.find(m->easy_handle);
				dbg_assert(RequestIt != m_RunningRequests.end(), "Running handle not added to map");
				auto pRequest = RequestIt->second;

				pRequest->OnCompletionInternal(m->data.result);
				curl_multi_remove_handle(pMultiH, m->easy_handle);
				curl_easy_cleanup(m->easy_handle);

				m_RunningRequests.erase(RequestIt);
			}
		}

		decltype(m_PendingRequests) NewRequests = {};
		LoopL.lock();
		std::swap(m_PendingRequests, NewRequests);
		LoopL.unlock();

		while(!NewRequests.empty())
		{
			auto pRequest = std::move(NewRequests.front());
			NewRequests.pop();

			dbg_msg("http", "task: %s", pRequest->m_aUrl);

			CURL *pEH = curl_easy_init();
			if(!pEH)
				goto error_init;

			if(!pRequest->ConfigureHandle(pEH))
				goto error_configure;

			mc = curl_multi_add_handle(pMultiH, pEH);
			if(mc != CURLM_OK)
				goto error_configure;

			m_RunningRequests.emplace(std::make_pair(pEH, std::move(pRequest)));
			continue;

		error_configure:
			curl_easy_cleanup(pEH);
		error_init:
			dbg_msg("http", "failed to start new request");
			NewRequests.emplace(std::move(pRequest));
			LoopL.lock();
			m_State = ERROR;
			LoopL.unlock();
			break;
		}

		// Only possible if m_State == ERROR
		while(!NewRequests.empty())
		{
			auto pRequest = std::move(NewRequests.front());
			NewRequests.pop();

			pRequest->KillRequest(CURLE_ABORTED_BY_CALLBACK, "Shutting down");
		}
	}

	bool Cleanup = true;
	std::lock_guard FinalL(m_Lock);
	if(m_State == ERROR)
	{
		Cleanup = false;
	}

	while(!m_PendingRequests.empty())
	{
		auto pRequest = std::move(m_PendingRequests.front());
		m_PendingRequests.pop();

		pRequest->KillRequest(CURLE_ABORTED_BY_CALLBACK, "Shutting down");
	}

	for(auto &ReqPair : m_RunningRequests)
	{
		auto &[pHandle, pRequest] = ReqPair;
		if(Cleanup)
		{
			curl_multi_remove_handle(pMultiH, pHandle);
			curl_easy_cleanup(pHandle);
		}

		pRequest->KillRequest(CURLE_ABORTED_BY_CALLBACK, "Shutting down");
	}

	if(Cleanup)
	{
		curl_multi_cleanup(pMultiH);
		curl_global_cleanup();
	}
}

void CHttpRunner::Shutdown()
{
	if(m_pThread)
	{
		std::lock_guard l(m_Lock);
		m_State = SHUTDOWN;
		WakeUp();
	}
}

CHttpRunner::~CHttpRunner()
{
	if(m_State != SHUTDOWN)
		Shutdown();

	if(m_pThread)
		thread_wait(m_pThread);

#if defined(CONF_FAMILY_UNIX)
	io_pipe_close(m_WakeUpPair);
#elif defined(CONF_FAMILY_WINDOWS)
	net_loop_close(m_WakeUpPair[0]);
#endif
}

int CurlDebug(CURL *pHandle, curl_infotype Type, char *pData, size_t DataSize, void *pUser)
{
	char TypeChar;
	switch(Type)
	{
	case CURLINFO_TEXT:
		TypeChar = '*';
		break;
	case CURLINFO_HEADER_OUT:
		TypeChar = '<';
		break;
	case CURLINFO_HEADER_IN:
		TypeChar = '>';
		break;
	default:
		return 0;
	}
	while(const char *pLineEnd = (const char *)memchr(pData, '\n', DataSize))
	{
		int LineLength = pLineEnd - pData;
		log_debug("curl", "%c %.*s", TypeChar, LineLength, pData);
		pData += LineLength + 1;
		DataSize -= LineLength + 1;
	}
	return 0;
}

void EscapeUrl(char *pBuf, int Size, const char *pStr)
{
	char *pEsc = curl_easy_escape(0, pStr, 0);
	str_copy(pBuf, pEsc, Size);
	curl_free(pEsc);
}

bool HttpHasIpresolveBug()
{
	// curl < 7.77.0 doesn't use CURLOPT_IPRESOLVE correctly wrt.
	// connection caches.
	return curl_version_info(CURLVERSION_NOW)->version_num < 0x074d00;
}

CHttpRequest::CHttpRequest(const char *pUrl)
{
	str_copy(m_aUrl, pUrl);
}

CHttpRequest::~CHttpRequest()
{
	m_ResponseLength = 0;
	if(!m_WriteToFile)
	{
		m_BufferSize = 0;
		free(m_pBuffer);
		m_pBuffer = nullptr;
	}
	curl_slist_free_all((curl_slist *)m_pHeaders);
	m_pHeaders = nullptr;
	if(m_pBody)
	{
		m_BodyLength = 0;
		free(m_pBody);
		m_pBody = nullptr;
	}
}

bool CHttpRequest::BeforeInit()
{
	if(m_WriteToFile)
	{
		if(fs_makedir_rec_for(m_aDestAbsolute) < 0)
		{
			dbg_msg("http", "i/o error, cannot create folder for: %s", m_aDest);
			return false;
		}

		m_File = io_open(m_aDestAbsolute, IOFLAG_WRITE);
		if(!m_File)
		{
			dbg_msg("http", "i/o error, cannot open file: %s", m_aDest);
			return false;
		}
	}
	return true;
}

bool CHttpRequest::ConfigureHandle(void *pUser)
{
	if(!BeforeInit())
		return false;

	CURL *pHandle = m_pHandle = static_cast<CURL *>(pUser);
	if(g_Config.m_DbgCurl)
	{
		curl_easy_setopt(pHandle, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(pHandle, CURLOPT_DEBUGFUNCTION, CurlDebug);
	}

	long Protocols = CURLPROTO_HTTPS;
	if(g_Config.m_HttpAllowInsecure)
	{
		Protocols |= CURLPROTO_HTTP;
	}

	curl_easy_setopt(pHandle, CURLOPT_ERRORBUFFER, m_aErr);

	curl_easy_setopt(pHandle, CURLOPT_CONNECTTIMEOUT_MS, m_Timeout.ConnectTimeoutMs);
	curl_easy_setopt(pHandle, CURLOPT_TIMEOUT_MS, m_Timeout.TimeoutMs);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_LIMIT, m_Timeout.LowSpeedLimit);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_TIME, m_Timeout.LowSpeedTime);
	if(m_MaxResponseSize >= 0)
	{
		curl_easy_setopt(pHandle, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)m_MaxResponseSize);
	}

	curl_easy_setopt(pHandle, CURLOPT_PROTOCOLS, Protocols);
	curl_easy_setopt(pHandle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(pHandle, CURLOPT_MAXREDIRS, 4L);
	curl_easy_setopt(pHandle, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(pHandle, CURLOPT_URL, m_aUrl);
	curl_easy_setopt(pHandle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pHandle, CURLOPT_USERAGENT, GAME_NAME " " GAME_RELEASE_VERSION " (" CONF_PLATFORM_STRING "; " CONF_ARCH_STRING ")");
	curl_easy_setopt(pHandle, CURLOPT_ACCEPT_ENCODING, ""); // Use any compression algorithm supported by libcurl.

	curl_easy_setopt(pHandle, CURLOPT_WRITEDATA, this);
	curl_easy_setopt(pHandle, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(pHandle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(pHandle, CURLOPT_PROGRESSDATA, this);
	curl_easy_setopt(pHandle, CURLOPT_PROGRESSFUNCTION, ProgressCallback);
	curl_easy_setopt(pHandle, CURLOPT_IPRESOLVE, m_IpResolve == IPRESOLVE::V4 ? CURL_IPRESOLVE_V4 : m_IpResolve == IPRESOLVE::V6 ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_WHATEVER);
	if(g_Config.m_Bindaddr[0] != '\0')
	{
		curl_easy_setopt(pHandle, CURLOPT_INTERFACE, g_Config.m_Bindaddr);
	}

	if(curl_version_info(CURLVERSION_NOW)->version_num < 0x074400)
	{
		// Causes crashes, see https://github.com/ddnet/ddnet/issues/4342.
		// No longer a problem in curl 7.68 and above, and 0x44 = 68.
		curl_easy_setopt(pHandle, CURLOPT_FORBID_REUSE, 1L);
	}

#ifdef CONF_PLATFORM_ANDROID
	curl_easy_setopt(pHandle, CURLOPT_CAINFO, "data/cacert.pem");
#endif

	switch(m_Type)
	{
	case REQUEST::GET:
		break;
	case REQUEST::HEAD:
		curl_easy_setopt(pHandle, CURLOPT_NOBODY, 1L);
		break;
	case REQUEST::POST:
	case REQUEST::POST_JSON:
		if(m_Type == REQUEST::POST_JSON)
		{
			Header("Content-Type: application/json");
		}
		else
		{
			Header("Content-Type:");
		}
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDS, m_pBody);
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDSIZE, m_BodyLength);
		break;
	}

	curl_easy_setopt(pHandle, CURLOPT_HTTPHEADER, m_pHeaders);
	return true;
}

void CHttpRequest::OnCompletionInternal(unsigned int Result)
{
	CURLcode Code = static_cast<CURLcode>(Result);
	int State;
	if(Code != CURLE_OK)
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::FAILURE)
			dbg_msg("http", "%s failed. libcurl error (%u): %s", m_aUrl, Result, m_aErr);
		State = (Code == CURLE_ABORTED_BY_CALLBACK) ? HTTP_ABORTED : HTTP_ERROR;
	}
	else
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::ALL)
			dbg_msg("http", "task done: %s", m_aUrl);
		State = HTTP_DONE;
	}

	if(m_WriteToFile)
	{
		if(m_File && io_close(m_File) != 0)
		{
			dbg_msg("http", "i/o error, cannot close file: %s", m_aDest);
			State = HTTP_ERROR;
		}

		if(State == HTTP_ERROR || State == HTTP_ABORTED)
		{
			fs_remove(m_aDestAbsolute);
		}
	}

	m_State = State;
	OnCompletion();
	SetStatus(IEngineRunnable::DONE);
}

size_t CHttpRequest::OnData(char *pData, size_t DataSize)
{
	// Need to check for the maximum response size here as curl can only
	// guarantee it if the server sets a Content-Length header.
	if(m_MaxResponseSize >= 0 && m_ResponseLength + DataSize > (uint64_t)m_MaxResponseSize)
	{
		return 0;
	}
	if(!m_WriteToFile)
	{
		if(DataSize == 0)
		{
			return DataSize;
		}
		size_t NewBufferSize = maximum((size_t)1024, m_BufferSize);
		while(m_ResponseLength + DataSize > NewBufferSize)
		{
			NewBufferSize *= 2;
		}
		if(NewBufferSize != m_BufferSize)
		{
			m_pBuffer = (unsigned char *)realloc(m_pBuffer, NewBufferSize);
			m_BufferSize = NewBufferSize;
		}
		mem_copy(m_pBuffer + m_ResponseLength, pData, DataSize);
		m_ResponseLength += DataSize;
		return DataSize;
	}
	else
	{
		m_ResponseLength += DataSize;
		return io_write(m_File, pData, DataSize);
	}
}

void CHttpRequest::KillRequest(unsigned int Code, const char *pReason)
{
	str_copy(m_aErr, pReason, sizeof(m_aErr));
	OnCompletionInternal(Code);
}

size_t CHttpRequest::WriteCallback(char *pData, size_t Size, size_t Number, void *pUser)
{
	return ((CHttpRequest *)pUser)->OnData(pData, Size * Number);
}

int CHttpRequest::ProgressCallback(void *pUser, double DlTotal, double DlCurr, double UlTotal, double UlCurr)
{
	CHttpRequest *pTask = (CHttpRequest *)pUser;
	pTask->m_Current.store(DlCurr, std::memory_order_relaxed);
	pTask->m_Size.store(DlTotal, std::memory_order_relaxed);
	pTask->m_Progress.store((100 * DlCurr) / (DlTotal ? DlTotal : 1), std::memory_order_relaxed);
	pTask->OnProgress();
	return pTask->m_Abort ? -1 : 0;
}

void CHttpRequest::WriteToFile(IStorage *pStorage, const char *pDest, int StorageType)
{
	m_WriteToFile = true;
	str_copy(m_aDest, pDest);
	if(StorageType == -2)
	{
		pStorage->GetBinaryPath(m_aDest, m_aDestAbsolute, sizeof(m_aDestAbsolute));
	}
	else
	{
		pStorage->GetCompletePath(StorageType, m_aDest, m_aDestAbsolute, sizeof(m_aDestAbsolute));
	}
}

void CHttpRequest::Header(const char *pNameColonValue)
{
	m_pHeaders = curl_slist_append((curl_slist *)m_pHeaders, pNameColonValue);
}

void CHttpRequest::Result(unsigned char **ppResult, size_t *pResultLength) const
{
	if(m_WriteToFile || State() != HTTP_DONE)
	{
		*ppResult = nullptr;
		*pResultLength = 0;
		return;
	}
	*ppResult = m_pBuffer;
	*pResultLength = m_ResponseLength;
}

json_value *CHttpRequest::ResultJson() const
{
	unsigned char *pResult;
	size_t ResultLength;
	Result(&pResult, &ResultLength);
	if(!pResult)
	{
		return nullptr;
	}
	return json_parse((char *)pResult, ResultLength);
}

bool HttpInit(IEngine *pEngine, IStorage *pStorage)
{
	static_assert(CURL_ERROR_SIZE <= 256); // CHttpRequest::m_aErr

	// CHttpRequest::OnCompletionInternal(unsigned int == CURLcode)
	static_assert(std::numeric_limits<std::underlying_type_t<CURLcode>>::min() >= std::numeric_limits<unsigned int>::min() && // NOLINT(misc-redundant-expression)
		      std::numeric_limits<std::underlying_type_t<CURLcode>>::max() <= std::numeric_limits<unsigned int>::max()); // NOLINT(misc-redundant-expression)

	dbg_assert(CHttpRunnable::m_sRunner == -1, "http module initialized twice");

	bool Result = gs_Runner.Init();
	CHttpRunnable::m_sRunner = pEngine->RegisterRunner(&gs_Runner);

	return Result;
}
