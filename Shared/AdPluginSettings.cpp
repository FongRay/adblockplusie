#include "AdPluginStdAfx.h"

#include <Wbemidl.h>

#include "AdPluginIniFile.h"
#include "AdPluginSettings.h"
#include "AdPluginDictionary.h"
#include "AdPluginClient.h"
#include "AdPluginChecksum.h"
#ifdef SUPPORT_FILTER
#include "AdPluginFilterClass.h"
#endif
#include "AdPluginMutex.h"


class TSettings
{
    DWORD processorId;

    char sPluginId[44];
};


class CAdPluginSettingsLock : public CAdPluginMutex
{
public:
    CAdPluginSettingsLock() : CAdPluginMutex("SettingsFile", PLUGIN_ERROR_MUTEX_SETTINGS_FILE) {}
    ~CAdPluginSettingsLock() {}

};


class CAdPluginSettingsTabLock : public CAdPluginMutex
{
public:
    CAdPluginSettingsTabLock() : CAdPluginMutex("SettingsFileTab", PLUGIN_ERROR_MUTEX_SETTINGS_FILE_TAB) {}
    ~CAdPluginSettingsTabLock() {}
};

#ifdef SUPPORT_WHITELIST

class CAdPluginSettingsWhitelistLock : public CAdPluginMutex
{
public:
    CAdPluginSettingsWhitelistLock() : CAdPluginMutex("SettingsFileWhitelist", PLUGIN_ERROR_MUTEX_SETTINGS_FILE_WHITELIST) {}
    ~CAdPluginSettingsWhitelistLock() {}
};

#endif

char* CAdPluginSettings::s_dataPath = NULL;
CAdPluginSettings* CAdPluginSettings::s_instance = NULL;

CComAutoCriticalSection CAdPluginSettings::s_criticalSectionLocal;
#ifdef SUPPORT_FILTER
CComAutoCriticalSection CAdPluginSettings::s_criticalSectionFilters;
#endif
#ifdef SUPPORT_WHITELIST
CComAutoCriticalSection CAdPluginSettings::s_criticalSectionDomainHistory;
#endif


CAdPluginSettings::CAdPluginSettings() : 
    m_settingsVersion("1"), m_isDirty(false), m_isPluginSelftestEnabled(true), m_isFirstRun(false), m_isFirstRunUpdate(false), m_dwMainProcessId(0), m_dwMainThreadId(0), m_dwWorkingThreadId(0), 
    m_isDirtyTab(false), m_isPluginEnabledTab(true), m_tabNumber("1")
{
#ifdef SUPPORT_WHITELIST
    m_isDirtyWhitelist = false;
#endif

    m_settingsFile = std::auto_ptr<CAdPluginIniFile>(new CAdPluginIniFile(GetDataPath(SETTINGS_INI_FILE), true));
    m_settingsFileTab = std::auto_ptr<CAdPluginIniFile>(new CAdPluginIniFile(GetDataPath(SETTINGS_INI_FILE_TAB), true));
#ifdef SUPPORT_WHITELIST
    m_settingsFileWhitelist = std::auto_ptr<CAdPluginIniFile>(new CAdPluginIniFile(GetDataPath(SETTINGS_INI_FILE_WHITELIST), true));
#endif

    Clear();
    ClearTab();
#ifdef SUPPORT_WHITELIST
    ClearWhitelist();
#endif

    // Check existence of settings file
    bool isFileExisting = false;
    {
        CAdPluginSettingsLock lock;
        if (lock.IsLocked())
        {
            std::ifstream is;
	        is.open(GetDataPath(SETTINGS_INI_FILE), std::ios_base::in);
	        if (!is.is_open())
	        {
                m_isDirty = true;
	        }
	        else
	        {
		        is.close();

	            isFileExisting = true;
	        }
        }
    }

    // Read or convert file
    if (isFileExisting)
    {
        Read(false);
    }
    else
    {
        m_isDirty = true;
    }

    Write();
}


CAdPluginSettings* CAdPluginSettings::GetInstance() 
{
	CAdPluginSettings* instance = NULL;

	s_criticalSectionLocal.Lock();
	{
		if (!s_instance)
		{
			s_instance = new CAdPluginSettings();
		}

		instance = s_instance;
	}
	s_criticalSectionLocal.Unlock();

	return instance;
}


bool CAdPluginSettings::HasInstance() 
{
	bool hasInstance = true;

	s_criticalSectionLocal.Lock();
	{
        hasInstance = s_instance != NULL;
	}
	s_criticalSectionLocal.Unlock();

	return hasInstance;
}


bool CAdPluginSettings::Read(bool bDebug)
{
    bool isRead = true;

    DEBUG_SETTINGS("Settings::Read")
    {
        if (bDebug)
        {
            DEBUG_GENERAL("*** Loading settings:" + m_settingsFile->GetFilePath());
        }

        CAdPluginSettingsLock lock;
        if (lock.IsLocked())
        {
            isRead = m_settingsFile->Read();        
            if (isRead)
            {
                if (m_settingsFile->IsValidChecksum())
                {
	                s_criticalSectionLocal.Lock();
		            {
			            m_properties = m_settingsFile->GetSectionData("Settings");

			            // Delete obsolete properties
			            TProperties::iterator it = m_properties.find("pluginupdate");
			            if (it != m_properties.end())
			            {
				            m_properties.erase(it);
				            m_isDirty = true;
			            }

			            it = m_properties.find("pluginerrors");
			            if (it != m_properties.end())
			            {
				            m_properties.erase(it);
				            m_isDirty = true;
			            }

			            it = m_properties.find("pluginerrorcodes");
			            if (it != m_properties.end())
			            {
				            m_properties.erase(it);
				            m_isDirty = true;
			            }

			            it = m_properties.find("pluginenabled");
			            if (it != m_properties.end())
			            {
				            m_properties.erase(it);
				            m_isDirty = true;
			            }

			            // Convert property 'pluginid' to 'userid'
			            if (m_properties.find(SETTING_USER_ID) == m_properties.end())
			            {
				            it = m_properties.find("pluginid");
				            if (it != m_properties.end())
				            {
					            m_properties[SETTING_USER_ID] = it->second;

					            m_properties.erase(it);
					            m_isDirty = true;
				            }
			            }
		            }
		            s_criticalSectionLocal.Unlock();

#ifdef SUPPORT_FILTER            	    
                    // Unpack filter URLs
                    CAdPluginIniFile::TSectionData filters = m_settingsFile->GetSectionData("Filters");
                    int filterCount = 0;
                    bool bContinue = true;

    	            s_criticalSectionFilters.Lock();
		            {
			            m_filterUrlList.clear();

			            do
			            {
				            CStringA filterCountStr;
				            filterCountStr.Format("%d", ++filterCount);
            	            
				            CAdPluginIniFile::TSectionData::iterator filterIt = filters.find("filter" + filterCountStr);
				            CAdPluginIniFile::TSectionData::iterator versionIt = filters.find("filter" + filterCountStr + "v");

				            if (bContinue = (filterIt != filters.end() && versionIt != filters.end()))
				            {
					            m_filterUrlList[filterIt->second] = atoi(versionIt->second);
				            }

			            } while (bContinue);
		            }
                    s_criticalSectionFilters.Unlock();

#endif // SUPPORT_FILTER
	            }
	            else
	            {
                    DEBUG_SETTINGS("Settings:Invalid checksum - Deleting file")

                    Clear();

                    DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ_CHECKSUM, "Settings::Read - Checksum")
                    isRead = false;
                    m_isDirty = true;
	            }
            }
            else if (m_settingsFile->GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ, "Settings::Read")
                m_isDirty = true;
            }
            else
            {
                DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_READ, "Settings::Read")
            }
        }
        else
        {
            isRead = false;
        }
    }

	// Write file in case it is dirty
    if (isRead)
    {
        isRead = Write();
    }

    return isRead;
}


void CAdPluginSettings::Clear()
{
	// Default settings
	s_criticalSectionLocal.Lock();
	{
		m_properties.clear();

		m_properties[SETTING_PLUGIN_ACTIVATED] = "false";
		m_properties[SETTING_PLUGIN_EXPIRED] = "false";
		m_properties[SETTING_PLUGIN_VERSION] = IEPLUGIN_VERSION;
		m_properties[SETTING_PLUGIN_SELFTEST] = "true";
		m_properties[SETTING_LANGUAGE] = "en";
		m_properties[SETTING_DICTIONARY_VERSION] = "1";
		m_properties[SETTING_PLUGIN_INFO_PANEL] = "1"; // Welcome screen
	}
	s_criticalSectionLocal.Unlock();

	// Default filters
#ifdef SUPPORT_FILTER

	s_criticalSectionFilters.Lock();
	{
	    m_filterUrlList.clear();
		m_filterUrlList[CStringA(FILTERS_PROTOCOL) + CStringA(FILTERS_HOST) + "/easylist.txt"] = 1;
	}
	s_criticalSectionFilters.Unlock();

#endif // SUPPORT_FILTER
}


CStringA CAdPluginSettings::GetDataPathParent()
{
	char* lpData = new char[1024];

    lpData[0] = 0;

	OSVERSIONINFO osVersionInfo;
	::ZeroMemory(&osVersionInfo, sizeof(OSVERSIONINFO));

	osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

	::GetVersionEx(&osVersionInfo);

	//Windows Vista				- 6.0 
	//Windows Server 2003 R2	- 5.2 
	//Windows Server 2003		- 5.2 
	//Windows XP				- 5.1 
	if (osVersionInfo.dwMajorVersion >= 6)
	{
		if (::SHGetSpecialFolderPathA(NULL, lpData, CSIDL_LOCAL_APPDATA, TRUE))
		{
			strcat(lpData, "Low");
		}
	}
	else
	{
		::SHGetSpecialFolderPathA(NULL, lpData, CSIDL_APPDATA, TRUE);
	}

    ::PathAddBackslashA(lpData);

    return lpData;
}


CStringA CAdPluginSettings::GetDataPath(const CStringA& filename)
{
	if (s_dataPath == NULL) 
	{
		char* lpData = new char[1024];

		OSVERSIONINFO osVersionInfo;
		::ZeroMemory(&osVersionInfo, sizeof(OSVERSIONINFO));

		osVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

		::GetVersionEx(&osVersionInfo);

		//Windows Vista				- 6.0 
		//Windows Server 2003 R2	- 5.2 
		//Windows Server 2003		- 5.2 
		//Windows XP				- 5.1 
		if (osVersionInfo.dwMajorVersion >= 6)
		{
			if (::SHGetSpecialFolderPathA(NULL, lpData, CSIDL_LOCAL_APPDATA, TRUE))
			{
				strcat(lpData, "Low");
			}
			else
			{
				DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER_LOCAL, "Settings::GetDataPath failed");
			}
		}
		else
		{
			if (!SHGetSpecialFolderPathA(NULL, lpData, CSIDL_APPDATA, TRUE))
			{
				DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_GET_SPECIAL_FOLDER, "Settings::GetDataPath failed");
			}
		}

	    ::PathAddBackslashA(lpData);

	    s_dataPath = lpData;

    	if (!::CreateDirectoryA(s_dataPath + CStringA(USER_DIR), NULL))
		{
			DWORD errorCode = ::GetLastError();
			if (errorCode != ERROR_ALREADY_EXISTS)
			{
				DEBUG_ERROR_LOG(errorCode, PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_CREATE_FOLDER, "Settings::CreateDirectory failed");
			}
		}
	}
	
	return s_dataPath + CStringA(USER_DIR) + filename;
}


CStringA CAdPluginSettings::GetTempPath(const CStringA& filename)
{
    char lpPathBuffer[512] = "";

    DWORD dwRetVal = ::GetTempPathA(512, lpPathBuffer);
    if (dwRetVal == 0)
    {
	    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_TEMP_PATH, "Settings::GetTempPath failed");
    }

	return lpPathBuffer + filename;
}

CStringA CAdPluginSettings::GetTempFile(const CStringA& prefix)
{
    char lpNameBuffer[512] = "";
    CStringA tempPath;
 
    DWORD dwRetVal = ::GetTempFileNameA(GetTempPath(), prefix, 0, lpNameBuffer);
    if (dwRetVal == 0)
    {
	    DEBUG_ERROR_LOG(::GetLastError(), PLUGIN_ERROR_SYSINFO, PLUGIN_ERROR_SYSINFO_TEMP_FILE, "Settings::GetTempFileName failed");

        tempPath = GetDataPath();
    }
    else
    {
        tempPath = lpNameBuffer;
    }

    return tempPath;
}


bool CAdPluginSettings::Has(const CStringA& key) const
{
	bool hasKey;

    s_criticalSectionLocal.Lock();
	{
		hasKey = m_properties.find(key) != m_properties.end();
	}
    s_criticalSectionLocal.Unlock();
    
    return hasKey;
}


void CAdPluginSettings::Remove(const CStringA& key)
{
    s_criticalSectionLocal.Lock();
	{    
		TProperties::iterator it = m_properties.find(key);
		if (it != m_properties.end())
		{
			m_properties.erase(it);
			m_isDirty = true;
		}
	}
    s_criticalSectionLocal.Unlock();
}


CStringA CAdPluginSettings::GetString(const CStringA& key, const CStringA& defaultValue) const
{
	CStringA val = defaultValue;

    s_criticalSectionLocal.Lock();
	{
		TProperties::const_iterator it = m_properties.find(key);
		if (it != m_properties.end())
		{
			val = it->second;
		}
	}
    s_criticalSectionLocal.Unlock();

    DEBUG_SETTINGS("Settings::GetString key:" + key + " value:" + val)

	return val;
}


void CAdPluginSettings::SetString(const CStringA& key, const CStringA& value)
{
    if (value.IsEmpty()) return;

    DEBUG_SETTINGS("Settings::SetString key:" + key + " value:" + value)

    s_criticalSectionLocal.Lock();
	{
		TProperties::iterator it = m_properties.find(key);
		if (it != m_properties.end() && it->second != value)
		{
			it->second = value;
			m_isDirty = true;
		}
		else if (it == m_properties.end())
		{
			m_properties[key] = value; 
			m_isDirty = true;
		}
	}
    s_criticalSectionLocal.Unlock();
}




int CAdPluginSettings::GetValue(const CStringA& key, int defaultValue) const
{
	int val = defaultValue;

    CStringA sValue;
    sValue.Format("%d", defaultValue);

    s_criticalSectionLocal.Lock();
	{
		TProperties::const_iterator it = m_properties.find(key);
		if (it != m_properties.end())
		{
		    sValue = it->second;
			val = atoi(it->second);
		}
	}
    s_criticalSectionLocal.Unlock();

    DEBUG_SETTINGS("Settings::GetValue key:" + key + " value:" + sValue)

	return val;
}


void CAdPluginSettings::SetValue(const CStringA& key, int value)
{
    CStringA sValue;
    sValue.Format("%d", value);

    DEBUG_SETTINGS("Settings::SetValue key:" + key + " value:" + sValue)

    s_criticalSectionLocal.Lock();
	{
		TProperties::iterator it = m_properties.find(key);
		if (it != m_properties.end() && it->second != sValue)
		{
			it->second = sValue;
			m_isDirty = true;
		}
		else if (it == m_properties.end())
		{
			m_properties[key] = sValue; 
			m_isDirty = true;
		}
	}
    s_criticalSectionLocal.Unlock();
}


bool CAdPluginSettings::GetBool(const CStringA& key, bool defaultValue) const
{
	bool value = defaultValue;

    s_criticalSectionLocal.Lock();
    {
		TProperties::const_iterator it = m_properties.find(key);
		if (it != m_properties.end())
		{
			if (it->second == "true") value = true;
			if (it->second == "false") value = false;
		}
	}
    s_criticalSectionLocal.Unlock();

	DEBUG_SETTINGS("Settings::GetBool key:" + key + " value:" + (value ? "true":"false"))

 	return value;
}


void CAdPluginSettings::SetBool(const CStringA& key, bool value)
{
    SetString(key, value ? "true":"false");
}


bool CAdPluginSettings::IsPluginEnabled() const
{
    return m_isPluginEnabledTab && !GetBool(SETTING_PLUGIN_EXPIRED, false);
}

bool CAdPluginSettings::IsPluginSelftestEnabled()
{
    if (m_isPluginSelftestEnabled)
    {
        m_isPluginSelftestEnabled = GetBool(SETTING_PLUGIN_SELFTEST, true);
    }
    
    return m_isPluginSelftestEnabled;
}


#ifdef SUPPORT_FILTER

void CAdPluginSettings::SetFilterUrlList(const TFilterUrlList& filters) 
{
    DEBUG_SETTINGS("Settings::SetFilterUrlList")

	s_criticalSectionFilters.Lock();
	{
		if (m_filterUrlList != filters)
		{
    		m_filterUrlList = filters;
    		m_isDirty = true;
		}
	}
	s_criticalSectionFilters.Unlock();
}


TFilterUrlList CAdPluginSettings::GetFilterUrlList() const
{
	TFilterUrlList filterUrlList;

	s_criticalSectionFilters.Lock();
	{
		filterUrlList = m_filterUrlList;
	}
	s_criticalSectionFilters.Unlock();

	return filterUrlList;
}


void CAdPluginSettings::AddFilterUrl(const CStringA& url, int version) 
{
	s_criticalSectionFilters.Lock();
	{
		TFilterUrlList::iterator it = m_filterUrlList.find(url);
		if (it == m_filterUrlList.end() || it->second != version)
		{
            m_filterUrlList[url] = version;
		    m_isDirty = true;
	    }
    }
	s_criticalSectionFilters.Unlock();
}

#endif // SUPPORT_FILTER

bool CAdPluginSettings::Write(bool isDebug)
{
	bool isWritten = true;

    if (!m_isDirty)
    {
        return isWritten;
    }

    if (isDebug)
    {
		DEBUG_GENERAL("*** Writing changed settings")
	}

    CAdPluginSettingsLock lock;
    if (lock.IsLocked())
    {
        m_settingsFile->Clear();

        // Properties
        CAdPluginIniFile::TSectionData settings;        

        s_criticalSectionLocal.Lock();
        {
		    for (TProperties::iterator it = m_properties.begin(); it != m_properties.end(); ++it)
		    {
			    settings[it->first] = it->second;
		    }
	    }
        s_criticalSectionLocal.Unlock();

        m_settingsFile->UpdateSection("Settings", settings);

        // Filter URL's
#ifdef SUPPORT_FILTER

        int filterCount = 0;
        CAdPluginIniFile::TSectionData filters;        

        s_criticalSectionFilters.Lock();
	    {
		    for (TFilterUrlList::iterator it = m_filterUrlList.begin(); it != m_filterUrlList.end(); ++it)
		    {
			    CStringA filterCountStr;
			    filterCountStr.Format("%d", ++filterCount);

			    CStringA filterVersion;
			    filterVersion.Format("%d", it->second);

			    filters["filter" + filterCountStr] = it->first;
			    filters["filter" + filterCountStr + "v"] = filterVersion;
		    }
	    }
        s_criticalSectionFilters.Unlock();

        m_settingsFile->UpdateSection("Filters", filters);

#endif // SUPPORT_FILTER

        // Write file
        isWritten = m_settingsFile->Write();
        if (!isWritten)
        {
            DEBUG_ERROR_LOG(m_settingsFile->GetLastError(), PLUGIN_ERROR_SETTINGS, PLUGIN_ERROR_SETTINGS_FILE_WRITE, "Settings::Write")
        }
        
        m_isDirty = false;

        IncrementTabVersion(SETTING_TAB_SETTINGS_VERSION);
    }
    else
    {
        isWritten = false;
    }

    return isWritten;
}

#ifdef SUPPORT_WHITELIST

void CAdPluginSettings::AddDomainToHistory(const CStringA& domain)
{
	if (!LocalClient::IsValidDomain(domain))
    {
	    return;
    }

    // Delete domain
	s_criticalSectionDomainHistory.Lock();
	{
		for (TDomainHistory::iterator it = m_domainHistory.begin(); it != m_domainHistory.end(); ++it)
		{
			if (it->first == domain)
			{
				m_domainHistory.erase(it);
				break;
			}
		}

		// Get whitelist reason
		int reason = 0;

		s_criticalSectionLocal.Lock();
		{
			TDomainList::iterator it = m_whitelist.find(domain);
			if (it != m_whitelist.end())
			{
				reason = it->second;
			}
			else
			{
				reason = 3;
			}
		}
		s_criticalSectionLocal.Unlock();

		// Delete domain, if history is too long
		if (m_domainHistory.size() >= DOMAIN_HISTORY_MAX_COUNT)
		{
			m_domainHistory.erase(m_domainHistory.begin());
		}

		m_domainHistory.push_back(std::make_pair(domain, reason));
	}
	s_criticalSectionDomainHistory.Unlock();
}


TDomainHistory CAdPluginSettings::GetDomainHistory() const
{
	TDomainHistory domainHistory;

	s_criticalSectionDomainHistory.Lock();
	{
		domainHistory = m_domainHistory;
	}
	s_criticalSectionDomainHistory.Unlock();

    return domainHistory;
}

#endif // SUPPORT_WHITELIST


bool CAdPluginSettings::IsPluginUpdateAvailable() const
{
	bool isAvailable = Has(SETTING_PLUGIN_UPDATE_VERSION);
	if (isAvailable)
	{
		CStringA newVersion = GetString(SETTING_PLUGIN_UPDATE_VERSION);
	    CStringA curVersion = IEPLUGIN_VERSION;

		isAvailable = newVersion != curVersion;
		if (isAvailable)
		{
			int curPos = 0;
			int curMajor = atoi(curVersion.Tokenize(".", curPos));
			int curMinor = atoi(curVersion.Tokenize(".", curPos));
			int curDev   = atoi(curVersion.Tokenize(".", curPos));

			int newPos = 0;
			int newMajor = atoi(newVersion.Tokenize(".", newPos));
			int newMinor = newPos > 0 ? atoi(newVersion.Tokenize(".", newPos)) : 0;
			int newDev   = newPos > 0 ? atoi(newVersion.Tokenize(".", newPos)) : 0;

			isAvailable = newMajor > curMajor || newMajor == curMajor && newMinor > curMinor || newMajor == curMajor && newMinor == curMinor && newDev > curDev;
		}
	}

	return isAvailable;
}

bool CAdPluginSettings::IsMainProcess(DWORD dwProcessId) const
{
    if (dwProcessId == 0)
    {
        dwProcessId = ::GetCurrentProcessId();
    }
    return m_dwMainProcessId == dwProcessId;
}

void CAdPluginSettings::SetMainProcessId()
{
    m_dwMainProcessId = ::GetCurrentProcessId();
}

bool CAdPluginSettings::IsMainThread(DWORD dwThreadId) const
{
    if (dwThreadId == 0)
    {
        dwThreadId = ::GetCurrentThreadId();
    }
    return m_dwMainThreadId == dwThreadId;
}

void CAdPluginSettings::SetMainThreadId()
{
    m_dwMainThreadId = ::GetCurrentThreadId();
}

bool CAdPluginSettings::IsWorkingThread(DWORD dwThreadId) const
{
    if (dwThreadId == 0)
    {
        dwThreadId = ::GetCurrentThreadId();
    }
    return m_dwWorkingThreadId == dwThreadId;
}

void CAdPluginSettings::SetWorkingThreadId()
{
    m_dwWorkingThreadId = ::GetCurrentThreadId();
}

void CAdPluginSettings::SetFirstRun()
{
    m_isFirstRun = true;
}

bool CAdPluginSettings::IsFirstRun() const
{
    return m_isFirstRun;
}

void CAdPluginSettings::SetFirstRunUpdate()
{
    m_isFirstRunUpdate = true;
}

bool CAdPluginSettings::IsFirstRunUpdate() const
{
    return m_isFirstRunUpdate;
}

bool CAdPluginSettings::IsFirstRunAny() const
{
    return m_isFirstRun || m_isFirstRunUpdate;
}

// ============================================================================
// Tab settings
// ============================================================================

void CAdPluginSettings::ClearTab()
{
    s_criticalSectionLocal.Lock();
	{
	    m_isPluginEnabledTab = true;

	    m_errorsTab.clear();

	    m_propertiesTab.clear();

	    m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = "true";
    }
    s_criticalSectionLocal.Unlock();
}


bool CAdPluginSettings::ReadTab(bool bDebug)
{
    bool isRead = true;

    DEBUG_SETTINGS("SettingsTab::Read tab")

    if (bDebug)
    {
        DEBUG_GENERAL("*** Loading tab settings:" + m_settingsFileTab->GetFilePath());
    }

    isRead = m_settingsFileTab->Read();        
    if (isRead)
    {
        ClearTab();

        if (m_settingsFileTab->IsValidChecksum())
        {
            s_criticalSectionLocal.Lock();
            {
                m_propertiesTab = m_settingsFileTab->GetSectionData("Settings");

                m_errorsTab = m_settingsFileTab->GetSectionData("Errors");

                TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_PLUGIN_ENABLED);
                if (it != m_propertiesTab.end())
                {
                    m_isPluginEnabledTab = it->second != "false";
                }
            }
            s_criticalSectionLocal.Unlock();
        }
        else
        {
            DEBUG_SETTINGS("SettingsTab:Invalid checksum - Deleting file")

            DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_READ_CHECKSUM, "SettingsTab::Read - Checksum")
            isRead = false;
            m_isDirtyTab = true;
        }
    }
    else if (m_settingsFileTab->GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        m_isDirtyTab = true;
    }
    else
    {
        DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_READ, "SettingsTab::Read")
    }


	// Write file in case it is dirty or does not exist
    WriteTab();

    return isRead;
}

bool CAdPluginSettings::WriteTab(bool isDebug)
{
	bool isWritten = true;

    if (!m_isDirtyTab)
    {
        return isWritten;
    }

    if (isDebug)
    {
		DEBUG_GENERAL("*** Writing changed tab settings")
	}

    m_settingsFileTab->Clear();

    // Properties & errors
    CAdPluginIniFile::TSectionData settings;        
    CAdPluginIniFile::TSectionData errors;        

    s_criticalSectionLocal.Lock();
    {
        for (TProperties::iterator it = m_propertiesTab.begin(); it != m_propertiesTab.end(); ++it)
        {
	        settings[it->first] = it->second;
        }

        for (TProperties::iterator it = m_errorsTab.begin(); it != m_errorsTab.end(); ++it)
        {
	        errors[it->first] = it->second;
        }
    }
    s_criticalSectionLocal.Unlock();

    m_settingsFileTab->UpdateSection("Settings", settings);
    m_settingsFileTab->UpdateSection("Errors", errors);

    // Write file
    isWritten = m_settingsFileTab->Write();
    if (!isWritten)
    {
        DEBUG_ERROR_LOG(m_settingsFileTab->GetLastError(), PLUGIN_ERROR_SETTINGS_TAB, PLUGIN_ERROR_SETTINGS_FILE_WRITE, "SettingsTab::Write")
    }
    
    m_isDirtyTab = !isWritten;

    return isWritten;
}


void CAdPluginSettings::EraseTab()
{
    ClearTab();
    
    m_isDirtyTab = true;

    WriteTab();
}

bool CAdPluginSettings::IncrementTabCount()
{
    int tabCount = 1;

    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        SYSTEMTIME systemTime;
        ::GetSystemTime(&systemTime);

        CStringA today;
        today.Format("%d-%d-%d", systemTime.wYear, systemTime.wMonth, systemTime.wDay);

        ReadTab(false);
        
        s_criticalSectionLocal.Lock();
        {
            TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_COUNT);
            if (it != m_propertiesTab.end())
            {        
                tabCount = atoi(it->second) + 1;
            }

            it = m_propertiesTab.find(SETTING_TAB_START_TIME);
            if (it != m_propertiesTab.end() && it->second != today)
            {
                tabCount = 1;        
            }
            m_tabNumber.Format("%d", tabCount);
        
            m_propertiesTab[SETTING_TAB_COUNT] = m_tabNumber;
            m_propertiesTab[SETTING_TAB_START_TIME] = today;
            
            // Main tab?
            if (tabCount == 1)
            {
                m_propertiesTab[SETTING_TAB_DICTIONARY_VERSION] = "1";
                m_propertiesTab[SETTING_TAB_SETTINGS_VERSION] = "1";
#ifdef SUPPORT_WHITELIST
                m_propertiesTab[SETTING_TAB_WHITELIST_VERSION] = "1";
#endif
#ifdef SUPPORT_FILTER
                m_propertiesTab[SETTING_TAB_FILTER_VERSION] = "1";
#endif
#ifdef SUPPORT_CONFIG
                m_propertiesTab[SETTING_TAB_CONFIG_VERSION] = "1";
#endif
            }
        }
        s_criticalSectionLocal.Unlock();

        m_isDirtyTab = true;

        WriteTab(false);        
    }

    return tabCount == 1;
}


CStringA CAdPluginSettings::GetTabNumber() const
{
    CStringA tabNumber;
    
    s_criticalSectionLocal.Lock();
    {
        tabNumber = m_tabNumber;
    }
    s_criticalSectionLocal.Unlock();
    
    return tabNumber;
}


bool CAdPluginSettings::DecrementTabCount()
{
    int tabCount = 0;

    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);
        
        s_criticalSectionLocal.Lock();
        {
            TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_COUNT);
            if (it != m_propertiesTab.end())
            {
                tabCount = max(atoi(it->second) - 1, 0);

                if (tabCount > 0)
                {
                    m_tabNumber.Format("%d", tabCount);
                
                    m_propertiesTab[SETTING_TAB_COUNT] = m_tabNumber;
                }
                else
                {
                    it = m_propertiesTab.find(SETTING_TAB_START_TIME);
                    if (it != m_propertiesTab.end())
                    {
                        m_propertiesTab.erase(it);
                    }

                    it = m_propertiesTab.find(SETTING_TAB_COUNT);
                    if (it != m_propertiesTab.end())
                    {
                        m_propertiesTab.erase(it);
                    }
                }

                m_isDirtyTab = true;               
            }
        }
        s_criticalSectionLocal.Unlock();

        WriteTab(false);
    }

    return tabCount == 0;
}


void CAdPluginSettings::TogglePluginEnabled()
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
            m_isPluginEnabledTab = m_isPluginEnabledTab ? false : true;
            m_propertiesTab[SETTING_TAB_PLUGIN_ENABLED] = m_isPluginEnabledTab ? "true" : "false";
            m_isDirtyTab = true;
        }
        s_criticalSectionLocal.Unlock();
        
        WriteTab(false);
    }
}


bool CAdPluginSettings::GetPluginEnabled() const
{
    return m_isPluginEnabledTab;
}


void CAdPluginSettings::AddError(const CStringA& error, const CStringA& errorCode)
{
    DEBUG_SETTINGS("SettingsTab::AddError error:" + error + " code:" + errorCode)

    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
		    if (m_errorsTab.find(error) == m_errorsTab.end())
		    {
			    m_errorsTab[error] = errorCode; 
			    m_isDirtyTab = true;
		    }
		}
        s_criticalSectionLocal.Unlock();

		WriteTab(false);
	}
}


CStringA CAdPluginSettings::GetErrorList() const
{
    CStringA errors;

    s_criticalSectionLocal.Lock();
    {
        for (TProperties::const_iterator it = m_errorsTab.begin(); it != m_errorsTab.end(); ++it)
        {
            if (!errors.IsEmpty())
            {
                errors += ',';
            }

            errors += it->first + '.' + it->second;
        }
	}
    s_criticalSectionLocal.Unlock();

    return errors;
}


void CAdPluginSettings::RemoveErrors()
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
	        if (m_errorsTab.size() > 0)
	        {
	            m_isDirtyTab = true;
	        }
            m_errorsTab.clear();
        }
        s_criticalSectionLocal.Unlock();

        WriteTab(false);
	}
}


bool CAdPluginSettings::GetForceConfigurationUpdateOnStart() const
{
    bool isUpdating = false;

    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        s_criticalSectionLocal.Lock();
        {
            isUpdating = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START) != m_propertiesTab.end();
        }
        s_criticalSectionLocal.Unlock();
    }

    return isUpdating;
}


void CAdPluginSettings::ForceConfigurationUpdateOnStart(bool isUpdating)
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
            TProperties::iterator it = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START);
            
            if (isUpdating && it == m_propertiesTab.end())
            {
                m_propertiesTab[SETTING_TAB_UPDATE_ON_START] = "true";
                m_propertiesTab[SETTING_TAB_UPDATE_ON_START_REMOVE] = "false";
                
                m_isDirtyTab = true;
            }
            else if (!isUpdating)
            {
                // OK to remove?
                TProperties::iterator itRemove = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START_REMOVE);

                if (itRemove == m_propertiesTab.end() || itRemove->second == "true")
                {
                    if (it != m_propertiesTab.end())
                    {
                        m_propertiesTab.erase(it);
                    }

                    if (itRemove != m_propertiesTab.end())
                    {
                        m_propertiesTab.erase(itRemove);
                    }

                    m_isDirtyTab = true;
                }
            }
        }
        s_criticalSectionLocal.Unlock();

        WriteTab(false);
    }
}

void CAdPluginSettings::RemoveForceConfigurationUpdateOnStart()
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
            // OK to remove?
            TProperties::iterator itRemove = m_propertiesTab.find(SETTING_TAB_UPDATE_ON_START_REMOVE);

            if (itRemove != m_propertiesTab.end())
            {
                m_propertiesTab.erase(itRemove);
                m_isDirtyTab = true;
            }
        }
        s_criticalSectionLocal.Unlock();

        WriteTab(false);
    }
}

void CAdPluginSettings::RefreshTab()
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab();
    }
}


int CAdPluginSettings::GetTabVersion(const CStringA& key) const
{
    int version = 0;

    s_criticalSectionLocal.Lock();
    {
        TProperties::const_iterator it = m_propertiesTab.find(key);
        if (it != m_propertiesTab.end())
        {
            version = atoi(it->second);
        }
    }
    s_criticalSectionLocal.Unlock();

    return version;
}

void CAdPluginSettings::IncrementTabVersion(const CStringA& key)
{
    CAdPluginSettingsTabLock lock;
    if (lock.IsLocked())
    {
        ReadTab(false);

        s_criticalSectionLocal.Lock();
        {
            int version = 1;

            TProperties::iterator it = m_propertiesTab.find(key);
            if (it != m_propertiesTab.end())
            {
                version = atoi(it->second) + 1;
            }

            CStringA versionString;
            versionString.Format("%d", version);
        
            m_propertiesTab[key] = versionString;
        }
        s_criticalSectionLocal.Unlock();

        m_isDirtyTab = true;

        WriteTab(false);        
    }
}


// ============================================================================
// Whitelist settings
// ============================================================================

#ifdef SUPPORT_WHITELIST

void CAdPluginSettings::ClearWhitelist()
{
    s_criticalSectionLocal.Lock();
	{
	    m_whitelist.clear();
	    m_whitelistToGo.clear();
    }
    s_criticalSectionLocal.Unlock();
}


bool CAdPluginSettings::ReadWhitelist(bool isDebug)
{
    bool isRead = true;

    DEBUG_SETTINGS("SettingsWhitelist::Read")

    if (isDebug)
    {
        DEBUG_GENERAL("*** Loading whitelist settings:" + m_settingsFileWhitelist->GetFilePath());
    }

    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        isRead = m_settingsFileWhitelist->Read();        
        if (isRead)
        {
            if (m_settingsFileWhitelist->IsValidChecksum())
            {
                ClearWhitelist();

                s_criticalSectionLocal.Lock();
	            {
                    // Unpack white list
                    CAdPluginIniFile::TSectionData whitelist = m_settingsFileWhitelist->GetSectionData("Whitelist");
                    int domainCount = 0;
                    bool bContinue = true;

		            do
		            {
			            CStringA domainCountStr;
			            domainCountStr.Format("%d", ++domainCount);
        	            
			            CAdPluginIniFile::TSectionData::iterator domainIt = whitelist.find("domain" + domainCountStr);
			            CAdPluginIniFile::TSectionData::iterator reasonIt = whitelist.find("domain" + domainCountStr + "r");

			            if (bContinue = (domainIt != whitelist.end() && reasonIt != whitelist.end()))
			            {
				            m_whitelist[domainIt->second] = atoi(reasonIt->second);
			            }

		            } while (bContinue);

                    // Unpack white list
                    whitelist = m_settingsFileWhitelist->GetSectionData("Whitelist togo");
                    domainCount = 0;
                    bContinue = true;

		            do
		            {
			            CStringA domainCountStr;
			            domainCountStr.Format("%d", ++domainCount);
        	            
			            CAdPluginIniFile::TSectionData::iterator domainIt = whitelist.find("domain" + domainCountStr);
			            CAdPluginIniFile::TSectionData::iterator reasonIt = whitelist.find("domain" + domainCountStr + "r");

			            if (bContinue = (domainIt != whitelist.end() && reasonIt != whitelist.end()))
			            {
				            m_whitelistToGo[domainIt->second] = atoi(reasonIt->second);
			            }

		            } while (bContinue);
	            }
	            s_criticalSectionLocal.Unlock();
            }
            else
            {
                DEBUG_SETTINGS("SettingsWhitelist:Invalid checksum - Deleting file")

                DEBUG_ERROR_LOG(m_settingsFileWhitelist->GetLastError(), PLUGIN_ERROR_SETTINGS_WHITELIST, PLUGIN_ERROR_SETTINGS_FILE_READ_CHECKSUM, "SettingsWhitelist::Read - Checksum")
                isRead = false;
                m_isDirtyWhitelist = true;
            }
        }
        else if (m_settingsFileWhitelist->GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            m_isDirtyWhitelist = true;
        }
        else
        {
            DEBUG_ERROR_LOG(m_settingsFileWhitelist->GetLastError(), PLUGIN_ERROR_SETTINGS_WHITELIST, PLUGIN_ERROR_SETTINGS_FILE_READ, "SettingsWhitelist::Read")
        }
    }
    else
    {
        isRead = false;
    }

	// Write file in case it is dirty
    WriteWhitelist(isDebug);

    return isRead;
}


bool CAdPluginSettings::WriteWhitelist(bool isDebug)
{
	bool isWritten = true;

    if (!m_isDirtyWhitelist)
    {
        return isWritten;
    }

    if (isDebug)
    {
		DEBUG_GENERAL("*** Writing changed whitelist settings")
	}

    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        m_settingsFileWhitelist->Clear();

        s_criticalSectionLocal.Lock();
	    {
            // White list
            int whitelistCount = 0;
            CAdPluginIniFile::TSectionData whitelist;

		    for (TDomainList::iterator it = m_whitelist.begin(); it != m_whitelist.end(); ++it)
		    {
			    CStringA whitelistCountStr;
			    whitelistCountStr.Format("%d", ++whitelistCount);

			    CStringA reason;
			    reason.Format("%d", it->second);

			    whitelist["domain" + whitelistCountStr] = it->first;
			    whitelist["domain" + whitelistCountStr + "r"] = reason;
		    }

            m_settingsFileWhitelist->UpdateSection("Whitelist", whitelist);

            // White list (not yet committed)
            whitelistCount = 0;
            whitelist.clear();

            for (TDomainList::iterator it = m_whitelistToGo.begin(); it != m_whitelistToGo.end(); ++it)
            {
	            CStringA whitelistCountStr;
	            whitelistCountStr.Format("%d", ++whitelistCount);

	            CStringA reason;
	            reason.Format("%d", it->second);

	            whitelist["domain" + whitelistCountStr] = it->first;
	            whitelist["domain" + whitelistCountStr + "r"] = reason;
            }

            m_settingsFileWhitelist->UpdateSection("Whitelist togo", whitelist);
	    }
        s_criticalSectionLocal.Unlock();

        // Write file
        isWritten = m_settingsFileWhitelist->Write();
        if (!isWritten)
        {
            DEBUG_ERROR_LOG(m_settingsFileWhitelist->GetLastError(), PLUGIN_ERROR_SETTINGS_WHITELIST, PLUGIN_ERROR_SETTINGS_FILE_WRITE, "SettingsWhitelist::Write")
        }
        
        m_isDirty = false;
    }
    else
    {
        isWritten = false;
    }

    if (isWritten)
    {
        DEBUG_WHITELIST("Whitelist::Icrement version")

        IncrementTabVersion(SETTING_TAB_WHITELIST_VERSION);
    }

    return isWritten;
}


void CAdPluginSettings::AddWhiteListedDomain(const CStringA& domain, int reason, bool isToGo)
{
    DEBUG_SETTINGS("SettingsWhitelist::AddWhiteListedDomain domain:" + domain)

    bool isNewVersion = false;
    bool isForcingUpdateOnStart = false;

    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        ReadWhitelist(false);

        s_criticalSectionLocal.Lock();
        {
            bool isToGoMatcingReason = false;
            bool isToGoMatcingDomain = false;

	        TDomainList::iterator itToGo = m_whitelistToGo.find(domain);
	        TDomainList::iterator it = m_whitelist.find(domain);
	        if (isToGo)
	        {
		        if (itToGo != m_whitelistToGo.end())  
		        {
    		        isToGoMatcingDomain = true;
    		        isToGoMatcingReason = itToGo->second == reason;

                    if (reason == 3)
                    {
                        m_whitelistToGo.erase(itToGo);
				        m_isDirtyWhitelist = true;                        
                    }
                    else if (!isToGoMatcingReason)
			        {
				        itToGo->second = reason;
				        m_isDirtyWhitelist = true;
			        }
		        }
		        else 
		        {
			        m_whitelistToGo[domain] = reason;
			        m_isDirtyWhitelist = true;

                    // Delete new togo item from saved white list
                    if (it != m_whitelist.end())
                    {
                        m_whitelist.erase(it);
                    }
		        }
	        }
	        else
	        {
	            if (isToGoMatcingDomain)
	            {
                    m_whitelistToGo.erase(itToGo);
			        m_isDirtyWhitelist = true;
	            }

		        if (it != m_whitelist.end())  
		        {
			        if (it->second != reason)
			        {
				        it->second = reason;
				        m_isDirtyWhitelist = true;
			        }
		        }
		        else 
		        {
			        m_whitelist[domain] = reason; 
			        m_isDirtyWhitelist = true;
		        }
	        }

            isForcingUpdateOnStart = m_whitelistToGo.size() > 0;
        }
	    s_criticalSectionLocal.Unlock();

	    WriteWhitelist(false);
	}

    if (isForcingUpdateOnStart)
    {
        ForceConfigurationUpdateOnStart();
    }
}


bool CAdPluginSettings::IsWhiteListedDomain(const CStringA& domain) const
{
	bool bIsWhiteListed;

	s_criticalSectionLocal.Lock();
	{
		bIsWhiteListed = m_whitelist.find(domain) != m_whitelist.end();
		if (!bIsWhiteListed)
		{
		    TDomainList::const_iterator it = m_whitelistToGo.find(domain);
		    bIsWhiteListed = it != m_whitelistToGo.end() && it->second != 3;
		}
	}
	s_criticalSectionLocal.Unlock();

    return bIsWhiteListed;
}

int CAdPluginSettings::GetWhiteListedDomainCount() const
{
	int count = 0;

	s_criticalSectionLocal.Lock();
	{
		count = m_whitelist.size();
	}
	s_criticalSectionLocal.Unlock();

    return count;
}


TDomainList CAdPluginSettings::GetWhiteListedDomainList(bool isToGo) const
{
	TDomainList domainList;

	s_criticalSectionLocal.Lock();
	{
	    if (isToGo)
	    {
	        domainList = m_whitelistToGo;
	    }
	    else
	    {
	        domainList = m_whitelist;
	    }
	}
	s_criticalSectionLocal.Unlock();

    return domainList;
}


void CAdPluginSettings::ReplaceWhiteListedDomains(const TDomainList& domains)
{
    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        ReadWhitelist(false);

        s_criticalSectionLocal.Lock();
        {
            if (m_whitelist != domains)
            {
                m_whitelist = domains;
                m_isDirtyWhitelist = true;
            }

            // Delete entries in togo list
            bool isDeleted = true;

            while (isDeleted)
            {
                isDeleted = false;

                for (TDomainList::iterator it = m_whitelistToGo.begin(); it != m_whitelistToGo.end(); ++it)
                {
	                if (m_whitelist.find(it->first) != m_whitelist.end() || it->second == 3)
	                {
    	                m_whitelistToGo.erase(it);

                        // Force another round...
    	                isDeleted = true;
    	                break;
	                }
                }
            }
        }
        s_criticalSectionLocal.Unlock();

        WriteWhitelist(false);
    }
}


void CAdPluginSettings::RemoveWhiteListedDomainsToGo(const TDomainList& domains)
{
    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        ReadWhitelist(false);

        s_criticalSectionLocal.Lock();
        {
            for (TDomainList::const_iterator it = domains.begin(); it != domains.end(); ++it)
            {
                for (TDomainList::iterator itToGo = m_whitelistToGo.begin(); itToGo != m_whitelistToGo.end(); ++ itToGo)
                {
                    if (it->first == itToGo->first)
                    {
                        m_whitelistToGo.erase(itToGo);
                        m_isDirtyWhitelist = true;
                        break;
                    }
                }
            }
        }
        s_criticalSectionLocal.Unlock();

        WriteWhitelist(false);
    }
}


bool CAdPluginSettings::RefreshWhitelist()
{
    CAdPluginSettingsWhitelistLock lock;
    if (lock.IsLocked())
    {
        ReadWhitelist(true);
    }

    return true;
}

#endif // SUPPORT_WHITELIST