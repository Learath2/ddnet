#include <base/system.h>
#include "vote.h"

CVote::CVote(const char *pDesc, const char *pCommand, const char *pReason, bool Majority, int RequiredPercentage, int64 CloseTime)
{
	str_copy(m_aDescription, pDesc, sizeof(m_aDescription));
	str_copy(m_aCommand, pCommand, sizeof(m_aCommand));
	str_copy(m_aReason, pReason, sizeof(m_aReason));
	m_Majority = Majority;
	m_RequiredPercentage = RequiredPercentage;
	m_CloseTime = CloseTime;
}

void CVote::Clear()
{
	m_Yes = 0;
	m_No = 0;
	m_Total = m_Majority ? m_Total : 0;
	m_Result = VOTE_RESULT_NA;
}

void CVote::Force(int Result, int Actor)
{
	m_Done = true;
	m_Result = Result > 0 ? VOTE_RESULT_FORCE_PASS : VOTE_RESULT_FORCE_FAIL;
	m_Actor = Actor;
}

void CVote::Hold()
{
	m_Held = true;
}

void CVote::Abort()
{
	m_Done = true;
	m_Result = VOTE_RESULT_ABORT;
}

void CVote::Veto()
{
	m_Done = true;
	m_Result = VOTE_RESULT_VETO;
}

void CVote::UpdatePlayerCount(int NewCount)
{
	if(!m_Majority)
		return;

	m_Total = NewCount;
}

void CVote::Tick()
{
	if(time_get() > m_CloseTime)
	{
		m_Done = true;
		m_Result = Passed() && !m_Held ? VOTE_RESULT_PASS : VOTE_RESULT_FAIL;
	}
}

void CVote::Vote(int ActVote)
{
	if(ActVote > 0)
		m_Yes++;
	else if(ActVote < 0)
		m_No++;

	if(!m_Majority)
		m_Total++;
}

void CVote::Tally()
{
	if(!m_Majority || m_Held)
		return;

	if(Passed())
	{
		m_Done = true;
		m_Result = VOTE_RESULT_PASS;
	}
	else if(Failed())
	{
		m_Done = false;
		m_Result = VOTE_RESULT_FAIL;
	}
}

void CVote::SetCreator(int ClientID)
{
	m_Creator = ClientID;
}

void CVote::SetType(int Type)
{
	m_Type = Type;
}

const char *CVote::Message()
{
	switch(m_Result)
	{
	case VOTE_RESULT_PASS:
		return "Vote Passed";
	case VOTE_RESULT_FAIL:
		return "Vote failed";
	case VOTE_RESULT_FORCE_PASS:
		return "Vote passed enforced by server moderator";
	case VOTE_RESULT_FORCE_FAIL:
		return "Vote failed enforced by server moderator";
	case VOTE_RESULT_VETO:
		return "Vote failed because of veto. Find an empty server instead";
	case VOTE_RESULT_ABORT:
		return "Vote aborted";
	case VOTE_RESULT_NA:
	default:
		return "An error occured";
	}
}
