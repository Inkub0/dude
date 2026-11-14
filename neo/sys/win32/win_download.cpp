/*
===========================================================================
	Sys_DownloadURL for Windows, using WinHTTP (a system component, no extra
	runtime DLL and nothing to do with libcurl).

	HTTPS only. Follows https->https redirects (GitHub release assets redirect
	to their CDN). Verifies nothing; the caller checks the file against a pinned
	hash. See docs/mod-auto-download.md.
===========================================================================
*/

#include "sys/platform.h"
#include "sys/sys_public.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <wchar.h>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1	0x00000200
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2	0x00000800
#endif
#ifndef WINHTTP_OPTION_SECURE_PROTOCOLS
#define WINHTTP_OPTION_SECURE_PROTOCOLS		84
#endif

static bool Win_Widen( const char *utf8, wchar_t *out, int outCount ) {
	return MultiByteToWideChar( CP_UTF8, 0, utf8, -1, out, outCount ) > 0;
}

bool Sys_DownloadURL( const char *url, const char *destPath ) {
	if ( url == NULL || destPath == NULL ) {
		return false;
	}
	if ( _strnicmp( url, "https://", 8 ) != 0 ) {
		return false;
	}

	wchar_t wurl[2048];
	wchar_t wdest[1024];
	if ( !Win_Widen( url, wurl, 2048 ) || !Win_Widen( destPath, wdest, 1024 ) ) {
		return false;
	}

	URL_COMPONENTS uc;
	wchar_t host[256];
	wchar_t path[2048];
	memset( &uc, 0, sizeof( uc ) );
	uc.dwStructSize = sizeof( uc );
	uc.lpszHostName = host;
	uc.dwHostNameLength = sizeof( host ) / sizeof( host[0] );
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = sizeof( path ) / sizeof( path[0] );
	if ( !WinHttpCrackUrl( wurl, 0, 0, &uc ) || uc.nScheme != INTERNET_SCHEME_HTTPS ) {
		return false;
	}

	bool ok = false;
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
	FILE *f = NULL;

	hSession = WinHttpOpen( L"DUDE/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( hSession == NULL ) {
		goto done;
	}
	WinHttpSetTimeouts( hSession, 30000, 30000, 60000, 120000 );

	// enable TLS 1.1/1.2 explicitly; older Windows (the build targets 0x0501)
	// does not negotiate them by default, and GitHub requires TLS 1.2
	{
		DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption( hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof( secureProtocols ) );
	}

	hConnect = WinHttpConnect( hSession, host, uc.nPort, 0 );
	if ( hConnect == NULL ) {
		goto done;
	}

	hRequest = WinHttpOpenRequest( hConnect, L"GET", path, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
	if ( hRequest == NULL ) {
		goto done;
	}

	if ( !WinHttpSendRequest( hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) ) {
		goto done;
	}
	if ( !WinHttpReceiveResponse( hRequest, NULL ) ) {
		goto done;
	}

	// require HTTP 200
	{
		DWORD status = 0;
		DWORD statusLen = sizeof( status );
		if ( !WinHttpQueryHeaders( hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX ) ) {
			goto done;
		}
		if ( status != 200 ) {
			goto done;
		}
	}

	f = _wfopen( wdest, L"wb" );
	if ( f == NULL ) {
		goto done;
	}

	for ( ;; ) {
		DWORD avail = 0;
		if ( !WinHttpQueryDataAvailable( hRequest, &avail ) ) {
			goto done;
		}
		if ( avail == 0 ) {
			break;	// end of response
		}

		char buf[16384];
		DWORD want = ( avail < sizeof( buf ) ) ? avail : (DWORD)sizeof( buf );
		DWORD read = 0;
		if ( !WinHttpReadData( hRequest, buf, want, &read ) ) {
			goto done;
		}
		if ( read == 0 ) {
			break;
		}
		if ( fwrite( buf, 1, read, f ) != read ) {
			goto done;
		}
	}

	ok = true;

done:
	if ( f != NULL ) {
		fclose( f );
	}
	if ( hRequest != NULL ) {
		WinHttpCloseHandle( hRequest );
	}
	if ( hConnect != NULL ) {
		WinHttpCloseHandle( hConnect );
	}
	if ( hSession != NULL ) {
		WinHttpCloseHandle( hSession );
	}
	if ( !ok ) {
		_wremove( wdest );	// drop any partial file
	}
	return ok;
}
