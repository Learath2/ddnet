#include "http.h"

#include <base/system.h>
#include <engine/engine.h>
#include <engine/external/json-parser/json.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <game/version.h>

#if !defined(CONF_FAMILY_WINDOWS)
#include <csignal>
#endif

#define WIN32_LEAN_AND_MEAN
#include <curl/easy.h>

void CHttp::EscapeUrl(char *pBuf, int Size, const char *pStr)
{
	char *pEsc = curl_easy_escape(0, pStr, 0);
	str_copy(pBuf, pEsc, Size);
	curl_free(pEsc);
}

CHttpRequest::CHttpRequest(const char *pUrl)
{
	str_copy(m_aUrl, pUrl, sizeof(m_aUrl));
}

CHttpRequest::~CHttpRequest()
{
	if(!m_WriteToFile)
	{
		m_BufferSize = 0;
		m_BufferLength = 0;
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

bool CHttpRequest::ConfigureHandle(CURL *pHandle, CURLSH *pShare)
{
	if(!BeforeInit())
		return false;
	
	m_pHandle = pHandle;

	if(g_Config.m_DbgCurl)
	{
		curl_easy_setopt(pHandle, CURLOPT_VERBOSE, 1L);
	}

	curl_easy_setopt(pHandle, CURLOPT_ERRORBUFFER, m_aErr);

	curl_easy_setopt(pHandle, CURLOPT_CONNECTTIMEOUT_MS, m_Timeout.ConnectTimeoutMs);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_LIMIT, m_Timeout.LowSpeedLimit);
	curl_easy_setopt(pHandle, CURLOPT_LOW_SPEED_TIME, m_Timeout.LowSpeedTime);

	curl_easy_setopt(pHandle, CURLOPT_SHARE, pShare);
	curl_easy_setopt(pHandle, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
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
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDS, m_pBody);
		curl_easy_setopt(pHandle, CURLOPT_POSTFIELDSIZE, m_BodyLength);
		break;
	}

	curl_easy_setopt(pHandle, CURLOPT_HTTPHEADER, m_pHeaders);

	return true;
}

size_t CHttpRequest::OnData(char *pData, size_t DataSize)
{
	if(!m_WriteToFile)
	{
		if(DataSize == 0)
		{
			return DataSize;
		}
		bool Reallocate = false;
		if(m_BufferSize == 0)
		{
			m_BufferSize = 1024;
			Reallocate = true;
		}
		while(m_BufferLength + DataSize > m_BufferSize)
		{
			m_BufferSize *= 2;
			Reallocate = true;
		}
		if(Reallocate)
		{
			m_pBuffer = (unsigned char *)realloc(m_pBuffer, m_BufferSize);
		}
		mem_copy(m_pBuffer + m_BufferLength, pData, DataSize);
		m_BufferLength += DataSize;
		return DataSize;
	}
	else
	{
		return io_write(m_File, pData, DataSize);
	}
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

void CHttpRequest::OnStart()
{
	if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::ALL)
		dbg_msg("http", "fetching %s", m_aUrl);
}

void CHttpRequest::OnCompletionInternal(CURLcode Result)
{
	int State = m_State;
	int FinalState = State;
	if(Result != CURLE_OK)
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::FAILURE)
			dbg_msg("http", "%s failed. libcurl error: %s", m_aUrl, m_aErr);
		FinalState = (Result == CURLE_ABORTED_BY_CALLBACK) ? HTTP_ABORTED : HTTP_ERROR;
	}
	else
	{
		if(g_Config.m_DbgCurl || m_LogProgress >= HTTPLOG::ALL)
			dbg_msg("http", "task done %s", m_aUrl);
		FinalState = HTTP_DONE;
	}

	if(m_WriteToFile)
	{
		if(m_File && io_close(m_File) != 0)
		{
			dbg_msg("http", "i/o error, cannot close file: %s", m_aDest);
			FinalState = HTTP_ERROR;
		}

		if(State == HTTP_ERROR || State == HTTP_ABORTED)
		{
			fs_remove(m_aDestAbsolute);
		}
	}

	m_State = FinalState;
	OnCompletion();
	m_DoneCV.notify_one();
}

void CHttpRequest::WriteToFile(IStorage *pStorage, const char *pDest, int StorageType)
{
	m_WriteToFile = true;
	str_copy(m_aDest, pDest, sizeof(m_aDest));
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

bool CHttpRequest::IsDone() const
{
	switch(m_State) {
		case HTTP_ERROR:
		case HTTP_ABORTED:
		case HTTP_DONE:
			return true;
		default:
			return false;
	}
}

void CHttpRequest::Wait()
{
	std::unique_lock l(m_StateLock);
	m_DoneCV.wait(l, [this]{return IsDone(); });
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
	*pResultLength = m_BufferLength;
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

void CHttp::Init()
{
#if !defined(CONF_FAMILY_WINDOWS)
	// As a multithreaded application we have to tell curl to not install signal
	// handlers and instead ignore SIGPIPE from OpenSSL ourselves.
	signal(SIGPIPE, SIG_IGN);
#endif

	m_pThread = thread_init(CHttp::ThreadMain, this, "http");
}

void CHttp::AddRequest(std::shared_ptr<CHttpRequest> pRequest)
{
	std::lock_guard l(m_Lock);
	m_PendingRequests.emplace(std::move(pRequest));
	curl_multi_wakeup(m_pHandle); // Is it appropriate to use this as a CV?
}

void CHttp::ThreadMain(void *pUser)
{
	CHttp *pSelf = static_cast<CHttp *>(pUser);
	pSelf->Run();
}

void CHttp::Run()
{
	if(curl_global_init(CURL_GLOBAL_DEFAULT))
	{
		return; //TODO: Report the error somehow
	}

	// print curl version
	{
		curl_version_info_data *pVersion = curl_version_info(CURLVERSION_NOW);
		dbg_msg("http", "libcurl version %s (compiled = " LIBCURL_VERSION ")", pVersion->version);
	}

	m_pShare = curl_share_init();
	if(!m_pShare)
	{
		return; //TODO: Report the error somehow
	}
	curl_share_setopt(m_pShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

	if(!(m_pHandle = curl_multi_init()))
	{
		return; //TODO: Report the error somehow
	}

	while(!m_Shutdown.load(std::memory_order_relaxed))
	{
		int Running = 0;
		CURLMcode mc = curl_multi_poll(m_pHandle, NULL, 0, 100, NULL);
		if(m_Shutdown.load(std::memory_order_relaxed))
			break;

		if(mc != CURLM_OK)
			break; //TOOD: Report the error somehow
		
		mc = curl_multi_perform(m_pHandle, &Running);
		if(mc != CURLM_OK)
			break; //TOOD: Report the error somehow

		{
			struct CURLMsg *m;
			int Remaining = 0;
			while((m = curl_multi_info_read(m_pHandle, &Remaining)))
			{
				if(m->msg == CURLMSG_DONE)
				{
					SRequestWrapper *pWrapper = nullptr;
					curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &pWrapper);

					pWrapper->m_pRequest->OnCompletionInternal(m->data.result);

					curl_multi_remove_handle(m_pHandle, m->easy_handle);
					curl_easy_cleanup(m->easy_handle);

					if(m_pFirstRunning.get() == pWrapper)
						m_pFirstRunning = pWrapper->m_pNext;
					if(pWrapper->m_pPrev)
						pWrapper->m_pPrev->m_pNext = pWrapper->m_pNext;
					if(pWrapper->m_pNext)
						pWrapper->m_pNext->m_pPrev = pWrapper->m_pPrev;
				}
			}
		}

		std::unique_lock l(m_Lock);
		decltype(m_PendingRequests) NewRequests;
		std::swap(m_PendingRequests, NewRequests);
		l.unlock();

		while(!NewRequests.empty())
		{
			auto pRequest = std::move(NewRequests.front());
			NewRequests.pop();

			CURL *pHandle = curl_easy_init();
			if(!pHandle)
			{
				m_Shutdown = true;
				break;
			}

			if(!pRequest->ConfigureHandle(pHandle, m_pShare))
			{
				pRequest->m_State = HTTP_ERROR;
				dbg_msg("http", "Invalid request"); //TODO: More details?
				continue;
			}

			pRequest->m_State = HTTP_RUNNING;
			pRequest->OnStart();

			auto pWrapper = std::make_shared<SRequestWrapper>(std::move(pRequest));
			pWrapper->m_pNext = m_pFirstRunning;
			pWrapper->m_pPrev = nullptr;
			if(m_pFirstRunning)
				m_pFirstRunning->m_pPrev = pWrapper;
				
			m_pFirstRunning = std::move(pWrapper);

			curl_easy_setopt(pHandle, CURLOPT_PRIVATE, m_pFirstRunning.get());
			curl_multi_add_handle(m_pHandle, pHandle);
		}
	}

	for(auto pWrapper = m_pFirstRunning; pWrapper; )
	{
		auto pNext = pWrapper->m_pNext;
		pWrapper->m_pPrev = nullptr;
		pWrapper->m_pNext = nullptr;
		pWrapper = pNext;
	}

	curl_share_cleanup(m_pShare);
	curl_multi_cleanup(m_pHandle);
	curl_global_cleanup();
}
