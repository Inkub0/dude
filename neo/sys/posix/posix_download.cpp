/*
===========================================================================
	Sys_DownloadURL for POSIX (Linux / macOS).

	Spawns the system curl (then wget as a fallback) in a child process with a
	sanitized environment, so an injected loader library - e.g. the libcurl the
	Steam runtime forces in through LD_LIBRARY_PATH/LD_PRELOAD - cannot affect
	the download tool. DUDE itself never links or dlopens libcurl, which is why
	the Steam launch issue (see the -DCURL=OFF note) cannot recur here.

	HTTPS only. Verifies nothing; the caller checks the file against a pinned
	hash. See docs/mod-auto-download.md.
===========================================================================
*/

#include "sys/platform.h"
#include "sys/sys_public.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

// Run argv[0] with the given argument vector in a child process whose
// environment has the dynamic-loader injection variables removed. Returns true
// only if the child ran and exited with status 0.
static bool Sys_RunDownloader( const char *const argv[] ) {
	pid_t pid = fork();
	if ( pid < 0 ) {
		return false;
	}
	if ( pid == 0 ) {
		// child: drop injected loader env so the tool uses the real system libs
		unsetenv( "LD_LIBRARY_PATH" );
		unsetenv( "LD_PRELOAD" );
		unsetenv( "DYLD_LIBRARY_PATH" );
		unsetenv( "DYLD_INSERT_LIBRARIES" );
		execvp( argv[0], (char * const *)argv );
		// only reached if exec failed (e.g. the tool is not installed)
		_exit( 127 );
	}

	int status = 0;
	while ( waitpid( pid, &status, 0 ) < 0 ) {
		if ( errno != EINTR ) {
			return false;
		}
	}
	return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
}

bool Sys_DownloadURL( const char *url, const char *destPath ) {
	if ( url == NULL || destPath == NULL ) {
		return false;
	}
	// HTTPS only
	if ( strncasecmp( url, "https://", 8 ) != 0 ) {
		return false;
	}

	// curl: fail on HTTP errors, follow redirects but stay on https, min TLS 1.2
	const char *curlArgv[] = {
		"curl", "-fsSL", "--proto", "=https", "--tlsv1.2",
		"--max-time", "120", "-o", destPath, url, (const char *)NULL
	};
	if ( Sys_RunDownloader( curlArgv ) ) {
		return true;
	}

	// wget fallback
	const char *wgetArgv[] = {
		"wget", "-q", "--https-only", "--timeout=120",
		"-O", destPath, url, (const char *)NULL
	};
	if ( Sys_RunDownloader( wgetArgv ) ) {
		return true;
	}

	// neither succeeded: remove any partial/empty output
	remove( destPath );
	return false;
}
