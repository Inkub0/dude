/*
DUDE: accumulating load-phase sub-timers.

Session.cpp prints a per-map "load phases (msec)" breakdown (proc/game/media/settle), but the
"game" phase lumps together exactly the work a binary asset cache would attack. These
accumulators split it out: every render-model parse, every ASE/LWO -> collision-model
conversion, and every .aas text parse adds its wall time here, reset once per map load.
Engine-side only -- never referenced from the game DLL.
*/

#ifndef __LOADTIMING_H__
#define __LOADTIMING_H__

#include "sys/sys_public.h"

struct loadTiming_t {
	double	modelParseMs;		// render-model parse + FinishSurfaces, whatever triggered it
	int		modelParseCount;
	// per-type split of the above: static = ase/lwo/flt/ma (InitFromFile), md5 = .md5mesh,
	// other = md3/prt/liquid/default -- tells the binary model cache work what remains
	double	modelStaticMs;
	int		modelStaticCount;
	double	modelMd5Ms;
	int		modelMd5Count;
	double	modelOtherMs;
	int		modelOtherCount;
	// md5 sub-split (Model_md5.cpp ParseMesh): decides what an MD5 cache has to store
	double	md5DeformInfoMs;	// R_BuildDeformInfo: sil edges / mirrored + dup verts on the bind pose
	int		md5DeformInfoCount;
	double	md5GpuSkinMs;		// BuildGpuSkinData: the Vulkan GPU-skin bake (runs at load on VK)
	int		md5GpuSkinCount;
	double	md5TessMs;			// BuildTessTopology: the deform-once tess bake (only if enabled)
	int		md5TessCount;
	// binary model cache (r_binaryModelCache, Model.cpp): a hit skips the text parse + FinishSurfaces
	int		bmodelHits;
	int		bmodelMisses;
	double	bmodelWriteMs;		// spent WRITING caches on misses -- the first-load population cost
	int		bmodelWrites;
	// decl first-parse (DeclManager.cpp idDeclLocal::ParseLocal): the material/skin/etc TEXT parse a
	// model load merely TRIGGERS on first reference. It nests inside the model scopes, which are made
	// EXCLUSIVE of it -- so modelParse is pure geometry and this is the material cost, separately.
	double	declParseMs;		// every decl type
	int		declParseCount;
	double	declParseMaterialMs;	// of which DECL_MATERIAL
	int		declParseMaterialCount;
	double	declParseOtherMs;		// every other decl type (tables, skins, entityDefs, ...)
	int		declParseOtherCount;
	double	cmConvertMs;		// EXCLUSIVE: ASE/LWO -> cm_model_t build for props that ship no .cm
	int		cmConvertCount;		// (the render-model parse it triggers is counted above, not here)
	double	aasLoadMs;			// .aas text-lexer parse
	int		aasLoadCount;
};

extern loadTiming_t loadTiming;

void LoadTiming_Reset( void );

/*
idLoadTimingScope

RAII accumulator: adds the scope's wall time (and one count) to the given accumulator on exit,
so early returns inside a timed function are still accounted for. Pass `nested` to make the
scope EXCLUSIVE of time that accrues in another accumulator while it is open -- the
collision-model conversion nests a render-model parse it must not double-count.
*/
class idLoadTimingScope {
public:
	idLoadTimingScope( double &accumMs, int &count, const double *nested = NULL ) :
		accumMs( accumMs ), count( count ), nested( nested ),
		t0( Sys_MillisecondsPrecise() ), nested0( nested ? *nested : 0.0 ) {}
	~idLoadTimingScope() {
		double dt = Sys_MillisecondsPrecise() - t0;
		if ( nested ) {
			dt -= ( *nested - nested0 );
		}
		accumMs += dt;
		count++;
	}
private:
	double			&accumMs;
	int				&count;
	const double	*nested;
	double			t0;
	double			nested0;
};

#endif /* !__LOADTIMING_H__ */
