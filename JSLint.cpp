//This file is part of JSLint Plugin for Notepad++
//Copyright (C) 2010 Martin Vladic <martin.vladic@gmail.com>
//
//This program is free software; you can redistribute it and/or
//modify it under the terms of the GNU General Public License
//as published by the Free Software Foundation; either
//version 2 of the License, or (at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program; if not, write to the Free Software
//Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#include "StdHeaders.h"
#include "JSLint.h"
#include "Settings.h"
#include "DownloadJSLint.h"
#include "resource.h"
#include <v8.h>

using namespace v8;

extern HANDLE g_hDllModule;

////////////////////////////////////////////////////////////////////////////////

bool JSLintReportItem::IsReasonUndefVar() const
{
    return !GetUndefVar().empty();
}

tstring JSLintReportItem::GetUndefVar() const
{
    tstring var;

    if (m_type == LINT_TYPE_ERROR) {
        ScriptSourceDef& scriptSource = Settings::GetInstance().GetScriptSource(JSLintOptions::GetInstance().GetSelectedLinter());

        tstring errMsg = scriptSource.m_scriptSource == SCRIPT_SOURCE_DOWNLOADED && scriptSource.m_bSpecUndefVarErrMsg
            ? scriptSource.m_undefVarErrMsg : scriptSource.GetDefaultUndefVarErrMsg();

        tstring::size_type i = errMsg.find(TEXT("%s"));
        if (i != tstring::npos) {
            int nAfter = errMsg.size() - (i + 2);
            if (m_strReason.substr(0, i) == errMsg.substr(0, i) &&
                m_strReason.substr(m_strReason.size() - nAfter) == errMsg.substr(i + 2)) 
            {
                var = m_strReason.substr(i, m_strReason.size() - nAfter - i);
            }
        }
    }

    return var;
}

////////////////////////////////////////////////////////////////////////////////

/*
void fatalErrorHandler(const char* location, const char* message)
{
}

void messageListener(Handle<Message> message, Handle<Value> data)
{
    Local<String> msg = message->Get();
    String::AsciiValue str(msg);
    const char* sz = *str;
    const char* a = sz;
    int line = message->GetLineNumber();
    int sc = message->GetStartColumn();
    int ec = message->GetEndColumn();
}
*/

void JSLint::CheckScript(const string& strOptions, const string& strScript, 
	int nppTabWidth, int jsLintTabWidth, list<JSLintReportItem>& items)
{
    //V8::SetFatalErrorHandler(fatalErrorHandler);
    //V8::AddMessageListener(messageListener);

    // bug fix:
    // In presence of WebEdit plugin (https://sourceforge.net/projects/notepad-plus/forums/forum/482781/topic/4875421)
    // JSLint crashes below when executing Context::New(). V8 expects that fp divide by zero doesn't throw exception,
    // but in presence of WebEdit it does. So, here we are setting fp control word to the default (no fp exceptions are thrown).
    unsigned int cw = _controlfp(0, 0); // Get the default fp control word
    unsigned int cwOriginal = _controlfp(cw, MCW_EM);

	Isolate::CreateParams create_params;
	create_params.array_buffer_allocator =
		v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	Isolate* isolate = Isolate::New(create_params);

    HandleScope handle_scope(isolate);
	Local<Context> context = Context::New(isolate);
   


    ScriptSourceDef& scriptSource = Settings::GetInstance().GetScriptSource(JSLintOptions::GetInstance().GetSelectedLinter());

    string strJSLintScript;
    if (scriptSource.m_scriptSource == SCRIPT_SOURCE_BUILTIN) {
        strJSLintScript = LoadCustomDataResource((HMODULE)g_hDllModule, MAKEINTRESOURCE(scriptSource.GetScriptResourceID()), TEXT("JS"));
    } else {
        strJSLintScript = DownloadJSLint::GetInstance().GetVersion(scriptSource.m_linter, scriptSource.m_scriptVersion).GetContent();
    }
    if (strJSLintScript.empty()) {
        throw JSLintException("Invalid JSLint script!");
    }

	Local<String> source = String::NewFromUtf8(isolate, strJSLintScript.c_str(), NewStringType::kNormal).ToLocalChecked();

	Local<Script> script = Script::Compile(context , source).ToLocalChecked();
    if (script.IsEmpty()) {
        throw JSLintException("Invalid JSLint script!");
    }
    script->Run();

    // init script variable
    context->Global()->Set(String::NewFromUtf8(isolate,"script"), String::NewFromUtf8(isolate,strScript.c_str()));

    // init options variable
    script = Script::Compile(String::NewFromUtf8(isolate, ("options = " + strOptions).c_str()));
    if (script.IsEmpty()) {
        throw JSLintException("Invalid JSLint options (probably error in additional options)!");
    }
    script->Run();

    // call JSLINT
    script = Script::Compile(String::NewFromUtf8(isolate,(std::string(scriptSource.GetNamespace()) + "(script, options);").c_str()));
    if (script.IsEmpty()) {
        throw JSLintUnexpectedException();
    }
    script->Run();

    // get JSLINT data
    script = Script::Compile(String::NewFromUtf8(isolate,(std::string(scriptSource.GetNamespace()) + ".data();").c_str()));
    if (script.IsEmpty()) {
        throw JSLintUnexpectedException();
    }
    Handle<Object> data = script->Run()->ToObject();

    // read errors
    Handle<Object> errors = data->Get(String::NewFromUtf8(isolate,"errors"))->ToObject();
    if (!errors.IsEmpty()) {
        int32_t length = errors->Get(String::NewFromUtf8(isolate,"length"))->Int32Value();
        for (int32_t i = 0; i < length; ++i) {
            Handle<Value> eVal = errors->Get(Int32::New(isolate,i));
            if (eVal->IsObject()) {
                Handle<Object> e = eVal->ToObject();

                int line = e->Get(String::NewFromUtf8(isolate,"line"))->Int32Value();
                int character = e->Get(String::NewFromUtf8(isolate,"character"))->Int32Value();
                String::Utf8Value reason(e->Get(String::NewFromUtf8(isolate,"reason")));
                String::Utf8Value evidence(e->Get(String::NewFromUtf8(isolate,"evidence")));

                // adjust character position if there is a difference 
                // in tab width between Notepad++ and JSLint
                if (nppTabWidth != jsLintTabWidth) {
	                character += GetNumTabs(strScript, line, character, jsLintTabWidth) * (nppTabWidth - jsLintTabWidth);
                }

                items.push_back(JSLintReportItem(JSLintReportItem::LINT_TYPE_ERROR,
                    line - 1, character - 1, 
                    TextConversion::UTF8_To_T(*reason), 
	                TextConversion::UTF8_To_T(*evidence)));
            }
        }
    }

    // read unused
    Handle<Object> unused = data->Get(String::NewFromUtf8(isolate,"unused"))->ToObject();
    if (!unused.IsEmpty()) {
        int32_t length = unused->Get(String::NewFromUtf8(isolate,"length"))->Int32Value();
        for (int32_t i = 0; i < length; ++i) {
            Handle<Value> eVal = unused->Get(Int32::New(isolate,i));
            if (eVal->IsObject()) {
                Handle<Object> e = eVal->ToObject();

                int line = e->Get(String::NewFromUtf8(isolate,"line"))->Int32Value();
                String::Utf8Value name(e->Get(String::NewFromUtf8(isolate,"name")));
                String::Utf8Value function(e->Get(String::NewFromUtf8(isolate,"function")));

                tstring reason = TEXT("'") + TextConversion::UTF8_To_T(*name) + 
                    TEXT("' in '") + TextConversion::UTF8_To_T(*function) + TEXT("'");

                items.push_back(JSLintReportItem(JSLintReportItem::LINT_TYPE_UNUSED,
                    line - 1, 0, 
                    TextConversion::UTF8_To_T(*name),
	                TextConversion::UTF8_To_T(*function)));
            }
        }
    }

	isolate->Dispose();

    _controlfp(cwOriginal, MCW_EM);  // Restore the original gp control world
}

string JSLint::LoadCustomDataResource(HMODULE hModule, LPCTSTR lpName, LPCTSTR lpType)
{
	HRSRC hRes = FindResource(hModule, lpName, lpType);
	if (hRes == NULL) {
		throw JSLintResourceException();
	}

	DWORD dwSize = SizeofResource(hModule, hRes);
	if (dwSize == 0) {
		throw JSLintResourceException();
	}

	HGLOBAL hResLoad = LoadResource(hModule, hRes);
	if (hResLoad == NULL) {
		throw JSLintResourceException();
	}

	LPVOID pData = LockResource(hResLoad);
	if (pData == NULL) {
		throw JSLintResourceException();
	}

    return string((const char*)pData, dwSize);
}

int JSLint::GetNumTabs(const string& strScript, int line, int character, int tabWidth)
{
	int numTabs = 0;

	size_t i = 0;

	while (line-- > 0) {
		i = strScript.find('\n', i) + 1;
	}

	while (character > 0) {
        if (i < strScript.length() && strScript[i++] == '\t') {
			++numTabs;
			character -= tabWidth;
		} else {
			character--;
		}

	}

	return numTabs;
}
