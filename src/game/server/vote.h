#ifndef GAME_SERVER_VOTE_H
#define GAME_SERVER_VOTE_H

#include <game/voting.h>

class CVote {
private:
	enum {
		VOTE_RESULT_NA = 0,
		VOTE_RESULT_PASS,
		VOTE_RESULT_FAIL,
		VOTE_RESULT_FORCE_PASS,
		VOTE_RESULT_FORCE_FAIL,
		VOTE_RESULT_VETO,
		VOTE_RESULT_ABORT
	};

	char m_aDescription[VOTE_DESC_LENGTH];
	char m_aCommand[VOTE_CMD_LENGTH];
	char m_aReason[VOTE_REASON_LENGTH];
	bool m_Majority;
	int m_RequiredPercentage;
	int64 m_CloseTime;
	int m_Type;
	int m_Creator;

	int m_Total;
	int m_Yes;
	int m_No;

	bool m_Done;
	bool m_Held;
	int m_Actor;

	int m_Result;

public:
	enum {
		VOTE_TYPE_OPT = 0,
		VOTE_TYPE_KICK,
		VOTE_TYPE_SPEC,

		VOTE_FAIL = -1,
		VOTE_PASS = 1
	};

	CVote(const char *pDesc, const char *pCommand, const char *pReason, bool Majority, int RequiredPercentage, int64 CloseTime);

	void Clear();
	void Force(int Result, int Actor);
	void Veto();
	void Hold();
	void Abort();

	void UpdatePlayerCount(int NewCount);
	void Tick();
	void Vote(int ActVote);
	void Tally();

	void SetCreator(int ClientID);
	void SetType(int Type);

	const char *Message();
	int Type() const { return m_Type; };
	int Creator() const { return m_Creator; };
	const char *Description() const { return m_aDescription; };
	const char *Command() const { return m_aCommand; };
	const char *Reason() const { return m_aReason; };
	int64 CloseTime() const { return m_CloseTime; };
	bool Passed() const { return m_Yes > m_Total / (100.0 / m_RequiredPercentage); };
	bool Failed() const { return m_No > m_Total - (m_Total / (100.0 / m_RequiredPercentage)); };
	bool IsDone() const { return m_Done; };
	int Actor() const { return m_Actor; };
	int Total() const { return m_Total; };
	int Yes() const { return m_Yes; };
	int No() const { return m_No; };
};
#endif
