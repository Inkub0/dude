/*
	Offline test for the engine's DDS source decoder (LoadDDS, neo/renderer/Image_files.cpp).

	test_dds_decode.sh copies the decoder - every line from "static void DDS_Color565" to the end
	of LoadDDS - and the DDS header definitions of neo/renderer/Image.h into dds_extract.inc,
	verbatim; this file supplies what they call (fileSystem->ReadFile / FreeFile from memory,
	R_StaticAlloc, common) and runs them. Files arrive on stdin as "name\nlength\n" plus the bytes;
	each decoded image is written to stdout as "name width height\n" plus width*height*4 RGBA bytes
	(width 0 = not decoded), for the script to compare with an independent decoder.

	No game, no window, no GPU.
*/

#include <cstring>
#include <iostream>
#include <string>

#include "sys/platform.h"
#include "framework/Common.h"

#include "common_stub.h"

static std::string fileData;

struct StubFileSystem {
	int ReadFile( const char *name, void **buffer, ID_TIME_T *timestamp ) {
		if ( timestamp ) {
			*timestamp = 0;
		}
		if ( buffer ) {
			*buffer = malloc( fileData.size() );
			memcpy( *buffer, fileData.data(), fileData.size() );
		}
		return (int)fileData.size();
	}
	void FreeFile( void *buffer ) { free( buffer ); }
};
static StubFileSystem stubFileSystem;
#define fileSystem ( &stubFileSystem )
#define R_StaticAlloc( size ) ( (void *)malloc( size ) )

#include "dds_extract.inc"

int main( int argc, char **argv ) {
	idLib::common = common;
	std::string name, length;
	while ( std::getline( std::cin, name ) && std::getline( std::cin, length ) ) {
		fileData.resize( std::stoul( length ) );
		std::cin.read( &fileData[0], fileData.size() );
		byte *pic = NULL;
		int width = 0, height = 0;
		LoadDDS( name.c_str(), &pic, &width, &height, NULL );
		if ( !pic ) {
			width = height = 0;
		}
		std::cout << name << " " << width << " " << height << "\n";
		if ( pic ) {
			std::cout.write( (const char *)pic, (std::streamsize)width * height * 4 );
			free( pic );
		}
	}
	std::cout.flush();
	return 0;
}
