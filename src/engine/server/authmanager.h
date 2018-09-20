#ifndef ENGINE_SERVER_AUTHMANAGER_H
#define ENGINE_SERVER_AUTHMANAGER_H

#include <base/tl/array.h>

#include <engine/shared/protocol.h>

#define MD5_BYTES 16
#define SALT_BYTES 8

class CAuthManager
{
private:
	enum
	{
		AUTHED_NO = 0,
		AUTHED_HELPER,
		AUTHED_MOD,
		AUTHED_ADMIN
	};
	struct CKey
	{
		char m_aIdent[64];
		unsigned char m_aPw[MD5_BYTES];
		unsigned char m_aSalt[SALT_BYTES];
		int m_Level;
		char m_aNick[MAX_NAME_LENGTH];
	};
	array<CKey> m_aKeys;

	int m_aDefault[3];
	bool m_Generated;
public:
	typedef void (*FListCallback)(const char *pIdent, int Level, const char *pNick, void *pUser);

	CAuthManager();

	void Init();
	int AddKeyHash(const char *pIdent, const unsigned char *pHash, const unsigned char *pSalt, int AuthLevel, const char *pNick = 0);
	int AddKey(const char *pIdent, const char *pPw, int AuthLevel, const char *pNick = 0);
	int RemoveKey(int Slot); // Returns the old key slot that is now in the named one.
	int FindKey(const char *pIdent);
	bool CheckKey(int Slot, const char *pPw);
	int DefaultKey(int AuthLevel);
	int KeyLevel(int Slot);
	const char *KeyIdent(int Slot);
	void UpdateKeyHash(int Slot, const unsigned char *pHash, const unsigned char *pSalt, int AuthLevel, const char *pNick = 0);
	void UpdateKey(int Slot, const char *pPw, int AuthLevel, const char *pNick = 0);
	void ListKeys(FListCallback pfnListCallbac, void *pUser);
	void AddDefaultKey(int Level, const char *pPw);
	bool IsGenerated();
	int NumNonDefaultKeys();
	bool CanUseName(const char *pName, int AuthKey);
};

#endif //ENGINE_SERVER_AUTHMANAGER_H
