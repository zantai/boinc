// $Id$
//

/*////////////////////////////////////////////////////////////////////////////
 *  Project:
 *    Memory_and_Exception_Trace
 *
 * ///////////////////////////////////////////////////////////////////////////
 *	File:
 *		Stackwalker.cpp
 *
 *	Remarks:
 *    Dumps the stack of an thread if an exepction occurs
 *
 *	Author:
 *		Jochen Kalmbach, Germany
 *    (c) 2002-2003 (Freeware)
 *    http://www.codeproject.com/tools/leakfinder.asp
 *
 * License (The zlib/libpng License, http://www.opensource.org/licenses/zlib-license.php):
 *
 * Copyright (c) 2003 Jochen Kalmbach
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from the
 * use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, including
 * commercial applications, and to alter it and redistribute it freely, subject to
 * the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not claim
 *    that you wrote the original software. If you use this software in a product,
 *    an acknowledgment in the product documentation would be appreciated but is
 *    not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 *//////////////////////////////////////////////////////////////////////////////


#if defined(_WIN32) && !defined(__STDWX_H__) && !defined(_BOINC_WIN_) && !defined(_AFX_STDAFX_H_)
#include "boinc_win.h"
#endif

#include "stackwalker_win.h"
#include "stackwalker_imports.h"


// Link to dbghelp.dll and version.dll dynamically at runtime so we
//   can be specific about which version we are getting and where
//   we are getting it from
static tIAV pIAV = NULL;                    // ImagehlpApiVersion()
static tSC pSC = NULL;                      // SymCleanup()
static tSEM pSEM = NULL;                    // SymEnumerateModules64()
static tSFTA pSFTA = NULL;                  // SymFunctionTableAccess64()
static tSGLFA pSGLFA = NULL;                // SymGetLineFromAddr64()
static tSGMB pSGMB = NULL;                  // SymGetModuleBase64()
static tSGMI pSGMI = NULL;                  // SymGetModuleInfo64()
static tSGO pSGO = NULL;                    // SymGetOptions()
static tSGSP pSGSP = NULL;                  // SymGetSearchPath()
static tSFA pSFA = NULL;                    // SymFromAddr()
static tSI pSI = NULL;                      // SymInitialize()
static tSLM pSLM = NULL;                    // SymLoadModuleEx()
static tSRC pSRC = NULL;                    // SymRegisterCallback64()
static tSSO pSSO = NULL;                    // SymSetOptions()
static tSW pSW = NULL;                      // StackWalk()
static tUDSN pUDSN = NULL;                  // UnDecorateSymbolName()
static tSSSO pSSSO = NULL;                  // SymbolServerSetOptions
static tSDD pSDD = NULL;                    // SetDllDirectory
static tGFVIS pGFVIS = NULL;                // GetFileVersionInfoSize 
static tGFVI pGFVI = NULL;                  // GetFileVersionInfo
static tVQV pVQV = NULL;                    // VerQueryValue


// Forward definitions of functions:
static void ShowStackRM(HANDLE hThread, CONTEXT& c);

// Global data:
static BOOL g_bInitialized = FALSE;
static HANDLE g_hProcess = NULL;
static DWORD g_dwShowCount = 0;  // increase at every ShowStack-Call
static HMODULE g_hDbgHelpDll = NULL;
static HMODULE g_hSymSrvDll = NULL;
static HMODULE g_hSrcSrvDll = NULL;
static HMODULE g_hVersionDll = NULL;
static CRITICAL_SECTION g_csFileOpenClose = {0};


// ##########################################################################################
// ##########################################################################################
// ##########################################################################################
// ##########################################################################################


bool DebuggerLoadLibrary( 
    HINSTANCE* lphInstance, const std::string strBOINCLocation, const std::string strLibrary
)
{
    std::string strTargetLibrary;

    if (strBOINCLocation.empty()) {
        strTargetLibrary = strLibrary;
    } else {
        strTargetLibrary = strBOINCLocation + "\\" + strLibrary;
    }

    *lphInstance = LoadLibraryA( strTargetLibrary.c_str() );
    if ( *lphInstance == NULL )
    {
        strTargetLibrary = strLibrary;
        *lphInstance = LoadLibraryA( strTargetLibrary.c_str() );
        if ( *lphInstance == NULL )
        {
            _ftprintf( stderr, "LoadLibraryA( %s ): GetLastError = %lu\n", strLibrary.c_str(), gle );
            return false;
        }
    }
    return true;
}

BOOL CALLBACK SymbolServerCallbackProc(UINT_PTR ActionCode, ULONG64 CallbackData, ULONG64 UserContext)
{
    BOOL bRetVal = FALSE;
    PIMAGEHLP_CBA_EVENT pEvent = NULL;

    switch(ActionCode) {
        case SSRVACTION_EVENT:
            pEvent = (PIMAGEHLP_CBA_EVENT)CallbackData;
            switch(pEvent->severity) {
                case sevInfo:
                    _ftprintf(stderr, _T("SSRVINFO: %s\n"), pEvent->desc);
                    break;
                case sevProblem:
                    _ftprintf(stderr, _T("SSRVPROB: %s\n"), pEvent->desc);
                    break;
                case sevAttn:
                    _ftprintf(stderr, _T("SSRVATTN: %s\n"), pEvent->desc);
                    break;
                case sevFatal:
                    _ftprintf(stderr, _T("SSRVFATAL: %s\n"), pEvent->desc);
                    break;
            }
            bRetVal = TRUE;
            break;
        case SSRVACTION_TRACE:
            _ftprintf(stderr, _T("SSRVDEBUG: %s\n"), (PCTSTR)CallbackData);
            bRetVal = TRUE;
            break;
    }

    return bRetVal;
}

BOOL CALLBACK SymRegisterCallbackProc(HANDLE hProcess, ULONG ActionCode, ULONG64 CallbackData, ULONG64 UserContext)
{
    BOOL bRetVal = FALSE;
    PIMAGEHLP_CBA_EVENT pEvent = NULL;

    switch(ActionCode) {
        case CBA_EVENT:
            pEvent = (PIMAGEHLP_CBA_EVENT)CallbackData;
            switch(pEvent->severity) {
                case sevInfo:
                    _ftprintf(stderr, _T("INFO: %s\n"), pEvent->desc);
                    break;
                case sevProblem:
                    _ftprintf(stderr, _T("PROB: %s\n"), pEvent->desc);
                    break;
                case sevAttn:
                    _ftprintf(stderr, _T("ATTN: %s\n"), pEvent->desc);
                    break;
                case sevFatal:
                    _ftprintf(stderr, _T("FATAL: %s\n"), pEvent->desc);
                    break;
            }
            bRetVal = TRUE;
            break;
        case CBA_DEBUG_INFO:
            _ftprintf(stderr, _T("DEBUG: %s\n"), (PCTSTR)CallbackData);
            bRetVal = TRUE;
            break;
    }

    return bRetVal;
}

BOOL CALLBACK SymEnumerateModulesProc(LPSTR ModuleName, DWORD64 BaseOfDll, PVOID UserContext)
{
    IMAGEHLP_MODULE64   Module;
    char                szSymbolType[32];
    DWORD               dwHandle;
    LPVOID              lpData;
    DWORD               dwSize;
    char                szQuery[256];
    LPVOID              lpVar;
    UINT                uiVarSize;
    VS_FIXEDFILEINFO*   pFileInfo;
    char                szVersionInfo[24];
    char                szCompanyName[256];
    char                szProductName[256];
    char                szFileVersion[256];
    char                szProductVersion[256];
    bool                bFileVersionSupported = false;
    bool                bFileVersionRetrieved = false;

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate;


    memset( &Module, '\0', sizeof(IMAGEHLP_MODULE64) );
    Module.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

    memset( &szSymbolType, '\0', sizeof(szSymbolType) );
    memset( &szQuery, '\0', sizeof(szQuery) );
    memset( &szVersionInfo, '\0', sizeof(szVersionInfo) );
    memset( &szCompanyName, '\0', sizeof(szCompanyName) );
    memset( &szProductName, '\0', sizeof(szProductName) );
    memset( &szFileVersion, '\0', sizeof(szFileVersion) );
    memset( &szProductVersion, '\0', sizeof(szProductVersion) );

    if ( !pSGMI( g_hProcess, BaseOfDll, &Module ) )
    {
        _ftprintf(stderr, _T("SymGetModuleInfo(): GetLastError = %lu\n"), gle );
    }
    else
    { 
        switch ( Module.SymType )
        {
            case SymNone:
                strcpy( szSymbolType, "-nosymbols-" );
                break;
            case SymCoff:
                strcpy( szSymbolType, "COFF" );
                break;
            case SymCv:
                strcpy( szSymbolType, "CV" );
                break;
            case SymPdb:
                strcpy( szSymbolType, "PDB" );
                break;
            case SymExport:
                strcpy( szSymbolType, "-exported-" );
                break;
            case SymDeferred:
                strcpy( szSymbolType, "-deferred-" );
                break;
            case SymSym:
                strcpy( szSymbolType, "SYM" );
                break;
            default:
                _snprintf( szSymbolType, sizeof(szSymbolType), "symtype=%ld", (long) Module.SymType );
                break;
        }
    }

    // Get File Version Information
    //
    bFileVersionSupported = (NULL != pGFVIS) && (NULL != pGFVI) && (NULL != pVQV);
    if (bFileVersionSupported) {

        dwSize = pGFVIS(Module.LoadedImageName, &dwHandle);
        if (dwSize) {
            lpData = (LPVOID)malloc(dwSize);
            if(pGFVI(Module.LoadedImageName, dwHandle, dwSize, lpData)) {
                bFileVersionRetrieved = true;

                // Which language should be used to lookup the structure?
                strcpy(szQuery, "\\VarFileInfo\\Translation");
                pVQV(lpData, szQuery, (LPVOID*)&lpTranslate, &uiVarSize);


                // Version specified as part of the root record.
                if (pVQV(lpData, "\\", (LPVOID*)&pFileInfo, &uiVarSize)) {
                    snprintf(szVersionInfo, sizeof(szVersionInfo), "%d.%d.%d.%d", 
                        HIWORD(pFileInfo->dwFileVersionMS),
                        LOWORD(pFileInfo->dwFileVersionMS),
                        HIWORD(pFileInfo->dwFileVersionLS),
                        LOWORD(pFileInfo->dwFileVersionLS)
                    );                }

                // Company Name.
                sprintf(szQuery, "\\StringFileInfo\\%04x%04x\\CompanyName",
                    lpTranslate[0].wLanguage,
                    lpTranslate[0].wCodePage
                );
                if (pVQV(lpData, szQuery, &lpVar, &uiVarSize)) {
                    uiVarSize = snprintf(szCompanyName, sizeof(szCompanyName), "%s", lpVar);
                    if ((sizeof(szCompanyName) == uiVarSize) || (-1 == uiVarSize)) {
                        szCompanyName[255] = '\0';
                    }
                } else {
                    _ftprintf(stderr, _T("Get Company Name Failed.\n"));
                }

                // Product Name.
                sprintf(szQuery, "\\StringFileInfo\\%04x%04x\\ProductName",
                    lpTranslate[0].wLanguage,
                    lpTranslate[0].wCodePage
                );
                if (pVQV(lpData, szQuery, &lpVar, &uiVarSize)) {
                    uiVarSize = snprintf(szProductName, sizeof(szProductName), "%s", lpVar);
                    if ((sizeof(szProductName) == uiVarSize) || (-1 == uiVarSize)) {
                        szProductName[255] = '\0';
                    }
                } else {
                    _ftprintf(stderr, _T("Get Product Name Failed.\n"));
                }

                // File Version.
                sprintf(szQuery, "\\StringFileInfo\\%04x%04x\\FileVersion",
                    lpTranslate[0].wLanguage,
                    lpTranslate[0].wCodePage
                );
                if (pVQV(lpData, szQuery, &lpVar, &uiVarSize)) {
                    uiVarSize = snprintf(szFileVersion, sizeof(szFileVersion), "%s", lpVar);
                    if ((sizeof(szFileVersion) == uiVarSize) || (-1 == uiVarSize)) {
                        szFileVersion[255] = '\0';
                    }
                }

                // Product Version.
                sprintf(szQuery, "\\StringFileInfo\\%04x%04x\\ProductVersion",
                    lpTranslate[0].wLanguage,
                    lpTranslate[0].wCodePage
                );
                if (pVQV(lpData, szQuery, &lpVar, &uiVarSize)) {
                    uiVarSize = snprintf(szProductVersion, sizeof(szProductVersion), "%s", lpVar);
                    if ((sizeof(szProductVersion) == uiVarSize) || (-1 == uiVarSize)) {
                        szProductVersion[255] = '\0';
                    }
                }

                free(lpData);
            }
        }
    }

    _ftprintf(stderr, _T("ModLoad: "));
    _ftprintf(stderr, _T("%.8x ")                          , Module.BaseOfImage);
    _ftprintf(stderr, _T("%.8x ")                          , Module.ImageSize);
    _ftprintf(stderr, _T("%s ")                            , Module.LoadedImageName);
    if (bFileVersionSupported && bFileVersionRetrieved) {
        _ftprintf(stderr, _T("(%s) ")                      , szVersionInfo);
    }
    _ftprintf(stderr, _T("(%s Symbols Loaded)")            , szSymbolType);
    _ftprintf(stderr, _T("\n"));
    if (bFileVersionSupported && bFileVersionRetrieved) {
        _ftprintf(stderr, _T("    File Version   : %s\n")  , szFileVersion);
        _ftprintf(stderr, _T("    Company Name   : %s\n")  , szCompanyName);
        _ftprintf(stderr, _T("    Product Name   : %s\n")  , szProductName);
        _ftprintf(stderr, _T("    Product Version: %s\n")  , szProductVersion);
        _ftprintf(stderr, _T("\n"));
    }

    return TRUE;
}



int DebuggerInitialize( LPCSTR pszBOINCLocation, LPCSTR pszSymbolStore, BOOL bProxyEnabled, LPCSTR pszProxyServer )
{
    if (g_bInitialized != FALSE)
        return 0;

    // Detect which version of Windows we are running on.
    OSVERSIONINFO osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx((OSVERSIONINFO*)&osvi);

    // Get a real handle to the current process and store it for future use.
    DuplicateHandle(
        GetCurrentProcess(),
        GetCurrentProcess(),
        GetCurrentProcess(), 
        &g_hProcess, 
        0,
        false,
        DUPLICATE_SAME_ACCESS
    );

    // For the most part the dbghelp.dll does the right stuff, but there are
    // conditions where things go off into never never land.  Most of the
    // time the error comes back ERROR_MOD_NOT_FOUND.  Most of the info
    // out on the net describes conditions in which dbghelp.dll is trying
    // to dynamically load another module such as symsrv.dll and fails to
    // find it.  Preloading the module only seems to work on some machines.
    // On Windows XP or better a new API has been introduced called
    // SetDllDirectory which will inject a path into the search order
    // that is before the System and Windows directories which is what we
    // want.
    if ((VER_PLATFORM_WIN32_NT == osvi.dwPlatformId) &&
        (5 == osvi.dwMajorVersion) && (1 == osvi.dwMinorVersion))
    {
        HMODULE hKernel32 = LoadLibraryA("kernel32.dll");
        if (hKernel32) {
            pSDD = (tSDD)GetProcAddress( hKernel32, "SetDllDirectoryA" );
            if (!pSDD(pszBOINCLocation)) {
                _ftprintf(stderr, _T("SetDllDirectory(): GetLastError = %lu\n"), gle );
            }
            FreeLibrary(hKernel32);
            hKernel32 = NULL;
            pSDD = NULL;
        }
    }


    // If Win9x use the old dbghelp.dll (5.0.2195.1)
    if (VER_PLATFORM_WIN32_WINDOWS == osvi.dwPlatformId) {
        if (!DebuggerLoadLibrary(&g_hDbgHelpDll, pszBOINCLocation, "dbghelp95.dll")) {
            if (!DebuggerLoadLibrary(&g_hDbgHelpDll, pszBOINCLocation, "dbghelp.dll")) {
                g_bInitialized = FALSE;
                return 1;
            }
        }
    } else {
        if (!DebuggerLoadLibrary(&g_hDbgHelpDll, pszBOINCLocation, "dbghelp.dll")) {
            g_bInitialized = FALSE;
            return 1;
        }

        DebuggerLoadLibrary(&g_hSymSrvDll, pszBOINCLocation, "symsrv.dll");
        if (g_hSymSrvDll) {
            pSSSO = (tSSSO)GetProcAddress(g_hSymSrvDll, "SymbolServerSetOptions");
            if (pSSSO) {
                if (!pSSSO(SSRVOPT_TRACE, (ULONG64)TRUE)) {
                    _ftprintf(stderr, _T("SymbolServerSetOptions(): Register Trace Failed, GetLastError = %lu\n"), gle);
                }
                if (!pSSSO(SSRVOPT_CALLBACK, (ULONG64)SymbolServerCallbackProc)) {
                    _ftprintf(stderr, _T("SymbolServerSetOptions(): Register Callback Failed, GetLastError = %lu\n"), gle);
                }
                if (!pSSSO(SSRVOPT_UNATTENDED, (ULONG64)TRUE)) {
                    _ftprintf(stderr, _T("SymbolServerSetOptions(): Register Unattended Failed, GetLastError = %lu\n"), gle);
                }
                if (bProxyEnabled) {
                    if (!pSSSO(SSRVOPT_PROXY, (ULONG64)pszProxyServer)) {
                        _ftprintf(stderr, _T("SymbolServerSetOptions(): Register Proxy Failed, GetLastError = %lu\n"), gle);
                    }
                } else {
                    if (!pSSSO(SSRVOPT_PROXY, (ULONG64)NULL)) {
                        _ftprintf(stderr, _T("SymbolServerSetOptions(): Register Proxy Failed, GetLastError = %lu\n"), gle);
                    }
                }
            }
        }

        DebuggerLoadLibrary(&g_hSrcSrvDll, pszBOINCLocation, "srcsrv.dll");

        DebuggerLoadLibrary(&g_hVersionDll, pszBOINCLocation, "version.dll");
        if (g_hVersionDll) {
            pGFVIS = (tGFVIS)GetProcAddress(g_hVersionDll, "GetFileVersionInfoSizeA");
            pGFVI = (tGFVI)GetProcAddress(g_hVersionDll, "GetFileVersionInfoA");
            pVQV = (tVQV)GetProcAddress(g_hVersionDll, "VerQueryValueA");
        }
    }

    pIAV = (tIAV) GetProcAddress( g_hDbgHelpDll, "ImagehlpApiVersion" );
    pSC = (tSC) GetProcAddress( g_hDbgHelpDll, "SymCleanup" );
    pSEM = (tSEM) GetProcAddress( g_hDbgHelpDll, "SymEnumerateModules64" );
    pSFTA = (tSFTA) GetProcAddress( g_hDbgHelpDll, "SymFunctionTableAccess64" );
    pSGLFA = (tSGLFA) GetProcAddress( g_hDbgHelpDll, "SymGetLineFromAddr64" );
    pSGMB = (tSGMB) GetProcAddress( g_hDbgHelpDll, "SymGetModuleBase64" );
    pSGMI = (tSGMI) GetProcAddress( g_hDbgHelpDll, "SymGetModuleInfo64" );
    pSGO = (tSGO) GetProcAddress( g_hDbgHelpDll, "SymGetOptions" );
    pSGSP = (tSGSP) GetProcAddress( g_hDbgHelpDll, "SymGetSearchPath" );
    pSFA = (tSFA) GetProcAddress( g_hDbgHelpDll, "SymFromAddr" );
    pSI = (tSI) GetProcAddress( g_hDbgHelpDll, "SymInitialize" );
    pSRC = (tSRC) GetProcAddress( g_hDbgHelpDll, "SymRegisterCallback64" );
    pSSO = (tSSO) GetProcAddress( g_hDbgHelpDll, "SymSetOptions" );
    pSW = (tSW) GetProcAddress( g_hDbgHelpDll, "StackWalk64" );
    pUDSN = (tUDSN) GetProcAddress( g_hDbgHelpDll, "UnDecorateSymbolName" );
    pSLM = (tSLM) GetProcAddress( g_hDbgHelpDll, "SymLoadModuleEx" );

    if ( pIAV == NULL || pSC == NULL || pSEM == NULL || pSFTA == NULL ||
         pSGMB == NULL || pSGMI == NULL || pSGO == NULL || pSFA == NULL ||
         pSI == NULL || pSRC == NULL || pSSO == NULL || pSW == NULL ||
         pUDSN == NULL || pSLM == NULL )
    {
        _ftprintf( stderr, "GetProcAddress(): some required function not found.\n" );
        FreeLibrary( g_hDbgHelpDll );
        g_bInitialized = FALSE;
        return 1;
    }

    g_bInitialized = TRUE;

    InitializeCriticalSection(&g_csFileOpenClose);
    EnterCriticalSection(&g_csFileOpenClose);

    CHAR* tt;
    CHAR* p;
    DWORD symOptions; // symbol handler settings
    std::string strLocalSymbolStore;
    std::string strSymbolSearchPath;

    static const std::basic_string<char>::size_type npos = -1;

    tt = (CHAR*) malloc(sizeof(CHAR) * TTBUFLEN); // Get the temporary buffer
    if (!tt) return 1;  // not enough memory...

    // build symbol search path from:
    strLocalSymbolStore = "";
    strSymbolSearchPath = "";

    // current directory
    if ( GetCurrentDirectoryA( TTBUFLEN, tt ) )
        strSymbolSearchPath += tt + std::string( ";" );

    // dir with executable
    if ( GetModuleFileNameA( 0, tt, TTBUFLEN ) )
    {
        for ( p = tt + strlen( tt ) - 1; p >= tt; -- p )
        {
            // locate the rightmost path separator
            if ( *p == '\\' || *p == '/' || *p == ':' )
                break;
        }
        // if we found one, p is pointing at it; if not, tt only contains
        // an exe name (no path), and p points before its first byte
        if ( p != tt ) // path sep found?
        {
            if ( *p == ':' ) // we leave colons in place
                ++p;
            *p = '\0'; // eliminate the exe name and last path sep
            strSymbolSearchPath += tt + std::string( ";" );
        }
    }

    // environment variable _NT_SYMBOL_PATH
    if ( GetEnvironmentVariableA( "_NT_SYMBOL_PATH", tt, TTBUFLEN ) )
        strSymbolSearchPath += tt + std::string( ";" );
    // environment variable _NT_ALTERNATE_SYMBOL_PATH
    if ( GetEnvironmentVariableA( "_NT_ALT_SYMBOL_PATH", tt, TTBUFLEN ) )
        strSymbolSearchPath += tt + std::string( ";" );

    if ( GetTempPathA( TTBUFLEN, tt ) )
        strLocalSymbolStore += tt + std::string("symbols");

    // microsoft public symbol server
    if (npos == strSymbolSearchPath.find("http://msdl.microsoft.com/download/symbols")) {
        strSymbolSearchPath += 
            std::string( "srv*" ) + strLocalSymbolStore + 
            std::string( "*http://msdl.microsoft.com/download/symbols;" );
    }

    // project symbol server
    if ((npos == strSymbolSearchPath.find(pszSymbolStore)) && (0 < strlen(pszSymbolStore))) {
        strSymbolSearchPath += 
            std::string( "srv*" ) + strLocalSymbolStore + std::string( "*" ) +
            std::string( pszSymbolStore );
    }

    // boinc symbol server
    if (npos == strSymbolSearchPath.find("http://boinc.berkeley.edu/symstore")) {
        strSymbolSearchPath += 
            std::string( "srv*" ) + strLocalSymbolStore + 
            std::string( "*http://boinc.berkeley.edu/symstore;" );
    }

    if ( strSymbolSearchPath.size() > 0 ) // if we added anything, we have a trailing semicolon
        strSymbolSearchPath = strSymbolSearchPath.substr( 0, strSymbolSearchPath.size() - 1 );

    if (tt) 
        free( tt );

    // Setting symbol options to the WinDbg defaults.
    symOptions = NULL;
    symOptions |= SYMOPT_CASE_INSENSITIVE;
    symOptions |= SYMOPT_LOAD_LINES;
    symOptions |= SYMOPT_OMAP_FIND_NEAREST;
    symOptions |= SYMOPT_FAIL_CRITICAL_ERRORS;
    symOptions |= SYMOPT_AUTO_PUBLICS;
    symOptions |= SYMOPT_NO_IMAGE_SEARCH;
    symOptions |= SYMOPT_DEBUG;
    symOptions |= SYMOPT_NO_PROMPTS;
    pSSO( symOptions ); // SymSetOptions()

    // init symbol handler stuff (SymInitialize())
    if (!pSI(g_hProcess, strSymbolSearchPath.c_str(), TRUE))
    {
        _ftprintf(stderr, _T("SymInitialize(): GetLastError = %lu\n"), gle);
        return 1;
    }

    if (!pSRC(g_hProcess, SymRegisterCallbackProc, (ULONG64)g_hProcess))
    {
        _ftprintf(stderr, _T("SymRegisterCallback64(): GetLastError = %lu\n"), gle);
    }

    LeaveCriticalSection(&g_csFileOpenClose);
    return 0;
}

int DebuggerDisplayDiagnostics()
{
    EnterCriticalSection(&g_csFileOpenClose);

    LPAPI_VERSION lpDV = NULL;
    TCHAR buf[TTBUFLEN];

    lpDV = pIAV();
    pSGSP(g_hProcess, buf, TTBUFLEN);

    _ftprintf( stderr, _T("Debugger Engine   : %d.%d.%d.%d\n"), lpDV->MajorVersion, lpDV->MinorVersion, lpDV->Revision, lpDV->Reserved);
    _ftprintf( stderr, _T("Symbol Search Path: %s\n"), buf);
    _ftprintf( stderr, _T("\n\n"));

    if (!pSEM(g_hProcess, SymEnumerateModulesProc, NULL))
    {
        _ftprintf(stderr, _T("SymEnumerateModules64(): GetLastError = %lu\n"), gle );
    }

    _ftprintf( stderr, _T("\n\n"));

    LeaveCriticalSection(&g_csFileOpenClose);

    return 0;
}


// #################################################################################
// #################################################################################
// Here the Stackwalk-Part begins.
//   Some of the code is from an example from a book 
//   But I couldn�t find the reference anymore... sorry...
//   If someone knowns, please let me know...
// #################################################################################
// #################################################################################


// if you use C++ exception handling: install a translator function
// with set_se_translator(). In the context of that function (but *not*
// afterwards), you can either do your stack dump, or save the CONTEXT
// record as a local copy. Note that you must do the stack sump at the
// earliest opportunity, to avoid the interesting stackframes being gone
// by the time you do the dump.

// status: 
// - EXCEPTION_CONTINUE_SEARCH: exception wird weitergereicht
// - EXCEPTION_CONTINUE_EXECUTION: 
// - EXCEPTION_EXECUTE_HANDLER:
DWORD StackwalkFilter(EXCEPTION_POINTERS *ep, DWORD status)
{
  HANDLE hThread;

  DuplicateHandle( GetCurrentProcess(), GetCurrentThread(),
    GetCurrentProcess(), &hThread, 0, false, DUPLICATE_SAME_ACCESS );

  StackwalkThread( hThread, ep->ContextRecord );

  CloseHandle( hThread );

  return status;
}  // StackwalkFilter

void StackwalkThread(HANDLE hThread, CONTEXT* c)
{
  ShowStackRM(hThread, *c);
}

static void ShowStackRM(HANDLE hThread, CONTEXT& Context)
{
    BOOL bRetVal = FALSE;

    int frameNum; // counts walked frames
    DWORD64 offsetFromSymbol; // tells us how far from the symbol we were
    DWORD offsetFromLine; // tells us how far from the line we were

    char undName[MAX_SYM_NAME]; // undecorated name
    IMAGEHLP_MODULE64 Module;
    IMAGEHLP_LINE64 Line;
    STACKFRAME64 StackFrame;

    ULONG64 SymbolBuffer[
        (sizeof(SYMBOL_INFO) +
        MAX_SYM_NAME*sizeof(TCHAR) +
        sizeof(ULONG64) - 1) /
        sizeof(ULONG64)
    ];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)SymbolBuffer;


    if (g_bInitialized == FALSE)
    {
        _ftprintf(stderr, _T("Stackwalker not initialized (or was not able to initialize)!\n"));
        return;
    }


    // Critical section begin...
    EnterCriticalSection(&g_csFileOpenClose);
    InterlockedIncrement((long*) &g_dwShowCount);  // erh�he counter

    _ftprintf(stderr, _T("- Registers -\n"));

    // Dump the Context data
    _ftprintf(stderr, 
        _T("eax=%.8x ebx=%.8x ecx=%.8x edx=%.8x esi=%.8x edi=%.8x\n"),
        Context.Eax, Context.Ebx, Context.Ecx, Context.Edx, Context.Esi, Context.Edi
    );
    _ftprintf(stderr, 
        _T("eip=%.8x esp=%.8x ebp=%.8x\n"),
        Context.Eip, Context.Esp, Context.Ebp
    );
    _ftprintf(stderr, 
        _T("cs=%.4x  ss=%.4x  ds=%.4x  es=%.4x  fs=%.4x  gs=%.4x             efl=%.8x\n\n"),
        Context.SegCs, Context.SegSs, Context.SegDs,  Context.SegEs,  Context.SegFs,  Context.SegGs, Context.EFlags
    );

    // Stack Header
    _ftprintf(stderr, _T("- Callstack -\n"));
    _ftprintf(stderr, _T("ChildEBP RetAddr  Args to Child\n"));
    fflush( stderr );


    // init STACKFRAME for first call
    // Notes: AddrModeFlat is just an assumption. I hate VDM debugging.
    // Notes: will have to be #ifdef-ed for Alphas; MIPSes are dead anyway,
    // and good riddance.
    memset( &StackFrame, '\0', sizeof(STACKFRAME64) );
    StackFrame.AddrPC.Offset = Context.Eip;
    StackFrame.AddrPC.Mode = AddrModeFlat;
    StackFrame.AddrFrame.Offset = Context.Ebp;
    StackFrame.AddrFrame.Mode = AddrModeFlat;

    memset( pSymbol, '\0', sizeof(SymbolBuffer) );
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    memset( &Line, '\0', sizeof(IMAGEHLP_LINE64) );
    Line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    memset( &Module, '\0', sizeof(IMAGEHLP_MODULE64) );
    Module.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
  
    for ( frameNum = 0; ; ++frameNum )
    {
        // get next stack frame (StackWalk(), SymFunctionTableAccess(), SymGetModuleBase())
        // if this returns ERROR_INVALID_ADDRESS (487) or ERROR_NOACCESS (998), you can
        // assume that either you are done, or that the stack is so hosed that the next
        // deeper frame could not be found.
        bRetVal = pSW(
            IMAGE_FILE_MACHINE_I386,
            g_hProcess,
            hThread,
            &StackFrame,
            &Context,
            NULL,
            (PFUNCTION_TABLE_ACCESS_ROUTINE64)pSFTA,
            (PGET_MODULE_BASE_ROUTINE64)pSGMB,
            NULL
        );

        if (!bRetVal)
            break;

        if ( StackFrame.AddrPC.Offset == 0 )
        {
            // Special case: If we are here, we have no valid callstack entry!
            _ftprintf(stderr, _T("(-nosymbols- PC == 0)\n"), g_dwShowCount);
        }
        else
        {
            // show procedure info (SymFromAddr())
            undName[0] = 0;
            offsetFromSymbol = 0;
            if ( !pSFA( g_hProcess, StackFrame.AddrPC.Offset, &offsetFromSymbol, pSymbol ) )
            {
                if ( gle != 487 )
                {
                    _ftprintf(stderr, _T("SymFromAddr(): GetLastError = '%lu' Address = '%.8x'\n"), gle, StackFrame.AddrPC.Offset);
                }
            }
            else
            {
                // UnDecorateSymbolName()
                pUDSN( pSymbol->Name, undName, MAX_SYM_NAME, UNDNAME_NAME_ONLY );
            }

            // show line number info (SymGetLineFromAddr())
            offsetFromLine = 0;
            if ( !pSGLFA( g_hProcess, StackFrame.AddrPC.Offset, &offsetFromLine, &Line ) )
            {
                if ( (gle != 487) && (frameNum > 0) )  // ignore error for first frame
                {
                    _ftprintf(stderr, _T("SymGetLineFromAddr(): GetLastError = '%lu' Address = '%.8x'\n"), gle, StackFrame.AddrPC.Offset);
                }
            }

            // show module info (SymGetModuleInfo())
            if ( !pSGMI( g_hProcess, StackFrame.AddrPC.Offset, &Module ) )
            {
                _ftprintf(stderr, _T("SymGetModuleInfo(): GetLastError = '%lu' Address = '%.8x'\n"), gle, StackFrame.AddrPC.Offset);
            }
        } // we seem to have a valid PC


        _ftprintf(stderr, "%.8x ", StackFrame.AddrFrame.Offset);
        _ftprintf(stderr, "%.8x ", StackFrame.AddrReturn.Offset);
        _ftprintf(stderr, "%.8x ", StackFrame.Params[0]);
        _ftprintf(stderr, "%.8x ", StackFrame.Params[1]);
        _ftprintf(stderr, "%.8x ", StackFrame.Params[2]);
        _ftprintf(stderr, "%.8x ", StackFrame.Params[3]);
        _ftprintf(stderr, "%s",    Module.ModuleName);
        _ftprintf(stderr, "!%s+",  undName);
        _ftprintf(stderr, "0x%x ", offsetFromLine);

        if (Line.LineNumber)
            _ftprintf(stderr, "(%s:%lu) ", Line.FileName, Line.LineNumber);

        if (StackFrame.FuncTableEntry) {
            // FPO Data
            PFPO_DATA pFPO = (PFPO_DATA)StackFrame.FuncTableEntry;
            switch(pFPO->cbFrame) {
                case FRAME_FPO:
                    _ftprintf(stderr, "FPO: [%d,%d,%d] ", pFPO->cdwParams, pFPO->cdwLocals, pFPO->cbRegs);
                    break;
                case FRAME_TRAP:
                    _ftprintf(stderr, "FPO: [%d,%d] TrapFrame @ 0x%.8x ", pFPO->cdwParams, pFPO->cdwLocals, pFPO->ulOffStart);
                    break;
                case FRAME_TSS:
                    _ftprintf(stderr, "FPO: TaskGate Segment: 0 ");
                    break;
            }
        }

        _ftprintf(stderr, "\n");


        // Zero out params so we have fresh parameters through the next interation
        StackFrame.Params[0] = NULL;
        StackFrame.Params[1] = NULL;
        StackFrame.Params[2] = NULL;
        StackFrame.Params[3] = NULL;


        // no return address means no deeper stackframe
        if ( StackFrame.AddrReturn.Offset == 0 )
        {
            // avoid misunderstandings in the printf() following the loop
            SetLastError( 0 );
            break;
        }
    } // for ( frameNum )

	switch(gle){
    case ERROR_SUCCESS:
        break;
    case ERROR_INVALID_ADDRESS:
        _ftprintf(stderr, _T("\nStackWalk(): ERROR_INVALID_ADDRESS (%lu) - Possible stack corruption.\n"), gle );
        break;
    case ERROR_NOACCESS:
        _ftprintf(stderr, _T("\nStackWalk(): ERROR_NOACCESS (%lu) - Possible stack corruption.\n"), gle );
    	break;
    default:
        _ftprintf(stderr, _T("\nStackWalk(): GetLastError = %lu\n"), gle );
        break;
	}

    fflush(stderr);

    LeaveCriticalSection(&g_csFileOpenClose);
}

const char *BOINC_RCSID_e8b4633192 = "$Id$";
