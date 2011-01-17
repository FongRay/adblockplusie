#include "PluginStdAfx.h"

#include "PluginSettings.h"
#include "PluginSystem.h"
#include "PluginClass.h"
#include "PluginConfiguration.h"

#include "SimpleAdblockTab.h"


CPluginTab::CPluginTab(CPluginClass* plugin) : CPluginTabBase(plugin)
{
}

CPluginTab::~CPluginTab()
{
}


void CPluginTab::OnNavigate(const CString& url)
{
	CPluginTabBase::OnNavigate(url);

	int r = url.Find(L".simple-adblock.com");
	if ((r > 0) && (r < 15))
	{
		if (url.Find(L"?update") > 0)
		{
			CPluginConfiguration pluginConfig;
			pluginConfig.Download();
			DWORD id;
			HANDLE handle = ::CreateThread(NULL, 0, CPluginClass::MainThreadProc, (LPVOID)this, NULL, &id);
			CPluginSettings* settings = CPluginSettings::GetInstance();
			settings->SetPluginEnabled();
			settings->SetBool(SETTING_PLUGIN_REGISTRATION, pluginConfig.IsPluginRegistered());
			settings->SetValue(SETTING_PLUGIN_ADBLOCKLIMIT, pluginConfig.GetAdBlockLimit());
			settings->Write();
			this->OnUpdateConfig();
			this->OnUpdateSettings();
		}
	}
}