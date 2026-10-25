/*
	A decl's manager-side state for the offline tests in this directory, which link libidlib.a
	only: lets a test create a decl class of the engine (idDeclEffect ...) and call its Parse on a
	decl's text the way idDeclLocal::ParseLocal does - name, file and line come from here, and
	MakeDefault parses the decl's DefaultDefinition() as the engine does (and records that it
	happened). Include after common_stub.h and the decl's header.
*/

#ifndef DUDE_TOOLS_QUAKE4_DECL_STUB_H
#define DUDE_TOOLS_QUAKE4_DECL_STUB_H

#include <cstring>
#include <string>

class idDeclBaseStub : public idDeclBase {
public:
							idDeclBaseStub( idDecl *self, declType_t type, const char *name, const char *fileName, int lineNum ) :
								self( self ), type( type ), name( name ), fileName( fileName ), lineNum( lineNum ), defaulted( false ), depth( 0 ) {
								self->base = this;
							}

	bool					Defaulted( void ) const { return defaulted; }

							// the engine's idDeclLocal::ParseLocal: free, then parse the text
	bool					ParseText( const char *text, int textLength ) {
								self->FreeData();
								return self->Parse( text, textLength ) && !defaulted;
							}

	virtual const char *	GetName( void ) const { return name.c_str(); }
	virtual declType_t		GetType( void ) const { return type; }
	virtual declState_t		GetState( void ) const { return defaulted ? DS_DEFAULTED : DS_PARSED; }
	virtual bool			IsImplicit( void ) const { return false; }
	virtual bool			IsValid( void ) const { return true; }
	virtual void			Invalidate( void ) {}
	virtual void			Reload( void ) {}
	virtual void			EnsureNotPurged( void ) {}
	virtual int				Index( void ) const { return 0; }
	virtual int				GetLineNum( void ) const { return lineNum; }
	virtual const char *	GetFileName( void ) const { return fileName.c_str(); }
	virtual void			GetText( char *text ) const { text[0] = '\0'; }
	virtual int				GetTextLength( void ) const { return 0; }
	virtual void			SetText( const char *text ) {}
	virtual bool			ReplaceSourceFileText( void ) { return false; }
	virtual bool			SourceFileChanged( void ) const { return false; }
	virtual void			MakeDefault( void ) {
								// idDeclLocal::MakeDefault: parse the default definition instead
								defaulted = true;
								if ( ++depth > 2 ) {
									fprintf( stderr, "%s: the default definition does not parse\n", name.c_str() );
									exit( 3 );
								}
								self->FreeData();
								const char *defaultText = self->DefaultDefinition();
								self->Parse( defaultText, (int)strlen( defaultText ) );
								depth--;
							}
	virtual bool			EverReferenced( void ) const { return false; }
	virtual bool			SetDefaultText( void ) { return false; }
	virtual const char *	DefaultDefinition( void ) const { return self->DefaultDefinition(); }
	virtual bool			Parse( const char *text, const int textLength ) { return self->Parse( text, textLength ); }
	virtual void			FreeData( void ) { self->FreeData(); }
	virtual size_t			Size( void ) const { return self->Size(); }
	virtual void			List( void ) const {}
	virtual void			Print( void ) const {}

private:
	idDecl *				self;
	declType_t				type;
	std::string				name;
	std::string				fileName;
	int						lineNum;
	bool					defaulted;
	int						depth;
};

#endif
