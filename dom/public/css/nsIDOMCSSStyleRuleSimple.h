/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */
/* AUTO-GENERATED. DO NOT EDIT!!! */

#ifndef nsIDOMCSSStyleRuleSimple_h__
#define nsIDOMCSSStyleRuleSimple_h__

#include "nsISupports.h"
#include "nsString.h"
#include "nsIScriptContext.h"
#include "nsIDOMCSSStyleRule.h"

class nsIDOMCSSStyleDeclaration;

#define NS_IDOMCSSSTYLERULESIMPLE_IID \
 { 0xa6cf90c1, 0x15b3, 0x11d2, \
  { 0x93, 0x2e, 0x00, 0x80, 0x5f, 0x8a, 0xdd, 0x32 } } 

class nsIDOMCSSStyleRuleSimple : public nsIDOMCSSStyleRule {
public:

  NS_IMETHOD    GetSelectorText(nsString& aSelectorText)=0;
  NS_IMETHOD    SetSelectorText(const nsString& aSelectorText)=0;

  NS_IMETHOD    GetStyle(nsIDOMCSSStyleDeclaration** aStyle)=0;
};


#define NS_DECL_IDOMCSSSTYLERULESIMPLE   \
  NS_IMETHOD    GetSelectorText(nsString& aSelectorText);  \
  NS_IMETHOD    SetSelectorText(const nsString& aSelectorText);  \
  NS_IMETHOD    GetStyle(nsIDOMCSSStyleDeclaration** aStyle);  \



#define NS_FORWARD_IDOMCSSSTYLERULESIMPLE(_to)  \
  NS_IMETHOD    GetSelectorText(nsString& aSelectorText) { return _to##GetSelectorText(aSelectorText); } \
  NS_IMETHOD    SetSelectorText(const nsString& aSelectorText) { return _to##SetSelectorText(aSelectorText); } \
  NS_IMETHOD    GetStyle(nsIDOMCSSStyleDeclaration** aStyle) { return _to##GetStyle(aStyle); } \


extern nsresult NS_InitCSSStyleRuleSimpleClass(nsIScriptContext *aContext, void **aPrototype);

extern "C" NS_DOM nsresult NS_NewScriptCSSStyleRuleSimple(nsIScriptContext *aContext, nsISupports *aSupports, nsISupports *aParent, void **aReturn);

#endif // nsIDOMCSSStyleRuleSimple_h__
