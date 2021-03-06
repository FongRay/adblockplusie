/*
 * This file is part of Adblock Plus <https://adblockplus.org/>,
 * Copyright (C) 2006-2015 Eyeo GmbH
 *
 * Adblock Plus is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Adblock Plus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Adblock Plus.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ADBLOCK_PLUS_DOM_TRAVERSER_H_
#define _ADBLOCK_PLUS_DOM_TRAVERSER_H_


#include "PluginDomTraverserBase.h"


class CPluginTab;


class CPluginDomTraverserCache : public CPluginDomTraverserCacheBase
{
public:

  bool m_isHidden;

  CPluginDomTraverserCache() : CPluginDomTraverserCacheBase(), m_isHidden(false) {}

  void Init() { CPluginDomTraverserCacheBase::Init(); m_isHidden = false; }
};


class CPluginDomTraverser : public CPluginDomTraverserBase<CPluginDomTraverserCache>
{

public:

  CPluginDomTraverser(CPluginTab* tab);

protected:

  bool OnIFrame(IHTMLElement* pEl, const std::wstring& url, CString& indent);
  bool OnElement(IHTMLElement* pEl, const CString& tag, CPluginDomTraverserCache* cache, bool isDebug, CString& indent);

  bool IsEnabled();

  void HideElement(IHTMLElement* pEl, const CString& type, const std::wstring& url, bool isDebug, CString& indent);

};


#endif // _ADBLOCK_PLUS_DOM_TRAVERSER_H_
