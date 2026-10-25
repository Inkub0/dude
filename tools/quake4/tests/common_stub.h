/*
	idCommon for the offline tests in this directory, which link libidlib.a only: Printf goes
	to stdout, Warning to stderr (counted in stubWarnings), and Error / FatalError hand their
	message to stubErrorHandler - by default print it and exit(2); a test that must go on after
	a bad input (test_cm_parse.sh) installs one that throws. Defines the engine's `common`.
*/

#ifndef DUDE_TOOLS_QUAKE4_COMMON_STUB_H
#define DUDE_TOOLS_QUAKE4_COMMON_STUB_H

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

static int stubWarnings = 0;

static void StubErrorExit( const char *message ) {
	fprintf( stderr, "%s\n", message );
	exit( 2 );
}
static void ( *stubErrorHandler )( const char *message ) = StubErrorExit;

class idCommonStub : public idCommon {
public:
	virtual void			Init( int argc, char **argv ) {}
	virtual void			Shutdown( void ) {}
	virtual void			Quit( void ) {}
	virtual bool			IsInitialized( void ) const { return true; }
	virtual void			Frame( void ) {}
	virtual void			GUIFrame( bool execCmd, bool network ) {}
	virtual void			Async( void ) {}
	virtual void			StartupVariable( const char *match, bool once ) {}
	virtual void			InitTool( const toolFlag_t tool, const idDict *dict ) {}
	virtual void			ActivateTool( bool active ) {}
	virtual void			WriteConfigToFile( const char *filename ) {}
	virtual void			WriteFlaggedCVarsToFile( const char *filename, int flags, const char *setCmd ) {}
	virtual void			BeginRedirect( char *buffer, int buffersize, void (*flush)( const char * ) ) {}
	virtual void			EndRedirect( void ) {}
	virtual void			SetRefreshOnPrint( bool set ) {}
	virtual void			Printf( const char *fmt, ... ) { va_list a; va_start( a, fmt ); vprintf( fmt, a ); va_end( a ); }
	virtual void			VPrintf( const char *fmt, va_list arg ) { vprintf( fmt, arg ); }
	virtual void			DPrintf( const char *fmt, ... ) {}
	virtual void			Warning( const char *fmt, ... ) {
		va_list a; va_start( a, fmt ); fprintf( stderr, "WARNING: " ); vfprintf( stderr, fmt, a ); fprintf( stderr, "\n" ); va_end( a );
		stubWarnings++;
	}
	virtual void			DWarning( const char *fmt, ... ) {}
	virtual void			PrintWarnings( void ) {}
	virtual void			ClearWarnings( const char *reason ) {}
	virtual void			Error( const char *fmt, ... ) { va_list a; va_start( a, fmt ); Fail( "", fmt, a ); va_end( a ); }
	virtual void			FatalError( const char *fmt, ... ) { va_list a; va_start( a, fmt ); Fail( "FATAL: ", fmt, a ); va_end( a ); }
	virtual const idLangDict *GetLanguageDict( void ) { return NULL; }
	virtual const char *	KeysFromBinding( const char *bind ) { return ""; }
	virtual const char *	BindingFromKey( const char *key ) { return ""; }
	virtual int				ButtonState( int key ) { return 0; }
	virtual int				KeyState( int key ) { return 0; }
	virtual bool			SetCallback( CallbackType cbt, FunctionPointer cb, void *userArg ) { return false; }
	virtual bool			GetAdditionalFunction( FunctionType ft, FunctionPointer *out_fnptr, void **out_userArg ) { return false; }
	virtual float			GetTicInterpolation( void ) { return 0.0f; }
	virtual void			GetPendingViewAngleDelta( float &yaw, float &pitch ) { yaw = pitch = 0.0f; }
	// the Quake 4 game library's additions (framework/Common.h)
	virtual int				GetUserCmdHz( void ) const { return 60; }
	virtual int				GetUserCmdMSec( void ) const { return 16; }
	virtual bool			DoingDeclValidation( void ) { return false; }
	virtual int				GetRModeForMachineSpec( int machineSpec ) const { return 0; }
	virtual void			SetDesiredMachineSpec( int machineSpec ) {}
	virtual void			RunAlwaysThinkGUIs( int time ) {}
	virtual void			LoadToolsDLL( void ) {}
	virtual void			ModViewThink( void ) {}

private:
	static void				Fail( const char *prefix, const char *fmt, va_list arg ) {
		char message[4096];
		const int length = idStr::snPrintf( message, sizeof( message ), "%s", prefix );
		idStr::vsnPrintf( message + length, sizeof( message ) - length, fmt, arg );
		stubErrorHandler( message );
	}
};

static idCommonStub		commonStub;
idCommon *				common = &commonStub;

#endif
