/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __TR_LOCAL_H__
#define __TR_LOCAL_H__

class idScreenRect; // yay for include recursion

#include "renderer/Image.h"
#include "renderer/Interaction.h"
#include "renderer/MegaTexture.h"
#include "renderer/ModelDecal.h"
#include "renderer/ModelOverlay.h"
#include "renderer/RenderSystem.h"
#include "renderer/RenderWorld.h"

class idRenderWorldLocal;

// everything that is needed by the backend needs
// to be double buffered to allow it to run in
// parallel on a dual cpu machine
const int SMP_FRAMES = 1;

const int FALLOFF_TEXTURE_SIZE =	64;

const float	DEFAULT_FOG_DISTANCE = 500.0f;

const int FOG_ENTER_SIZE = 64;
const float FOG_ENTER = (FOG_ENTER_SIZE+1.0f)/(FOG_ENTER_SIZE*2);
// picky to get the bilerp correct at terminator


// idScreenRect gets carried around with each drawSurf, so it makes sense
// to keep it compact, instead of just using the idBounds class
class idScreenRect {
public:
	short		x1, y1, x2, y2;							// inclusive pixel bounds inside viewport
	float       zmin, zmax;								// for depth bounds test

	void		Clear();								// clear to backwards values
	void		AddPoint( float x, float y );			// adds a point
	void		Expand();								// expand by one pixel each way to fix roundoffs
	void		Intersect( const idScreenRect &rect );
	void		Union( const idScreenRect &rect );
	bool		Equals( const idScreenRect &rect ) const;
	bool		IsEmpty() const;
};

idScreenRect R_ScreenRectFromViewFrustumBounds( const idBounds &bounds );
void R_ShowColoredScreenRect( const idScreenRect &rect, int colorIndex );

typedef enum {
	DC_BAD,
	DC_RENDERVIEW,
	DC_UPDATE_ENTITYDEF,
	DC_DELETE_ENTITYDEF,
	DC_UPDATE_LIGHTDEF,
	DC_DELETE_LIGHTDEF,
	DC_LOADMAP,
	DC_CROP_RENDER,
	DC_UNCROP_RENDER,
	DC_CAPTURE_RENDER,
	DC_END_FRAME,
	DC_DEFINE_MODEL,
	DC_SET_PORTAL_STATE,
	DC_UPDATE_SOUNDOCCLUSION,
	DC_GUI_MODEL
} demoCommand_t;

/*
==============================================================================

SURFACES

==============================================================================
*/

// drawSurf_t structures command the back end to render surfaces
// a given srfTriangles_t may be used with multiple viewEntity_t,
// as when viewed in a subview or multiple viewport render, or
// with multiple shaders when skinned, or, possibly with multiple
// lights, although currently each lighting interaction creates
// unique srfTriangles_t

// drawSurf_t are always allocated and freed every frame, they are never cached
static const int	DSF_VIEW_INSIDE_SHADOW	= 1;
static const int	DSF_SOFT_PARTICLE		= 2; // #3878 - soft particles

typedef struct drawSurf_s {
	const srfTriangles_t	*geo;
	const struct viewEntity_s *space;
	const idMaterial		*material;	// may be NULL for shadow volumes
	float					sort;		// material->sort, modified by gui / entity sort offsets
	const float				*shaderRegisters;	// evaluated and adjusted for referenceShaders
	const struct drawSurf_s	*nextOnLight;	// viewLight chains
	idScreenRect			scissorRect;	// for scissor clipping, local inside renderView viewport
	int						dsFlags;			// DSF_VIEW_INSIDE_SHADOW, etc
	struct vertCache_s		*dynamicTexCoords;	// float * in vertex cache memory
	// specular directions for non vertex program cards, skybox texcoords, etc
	float					particle_radius;	// The radius of individual quads for soft particles #3878
} drawSurf_t;


typedef struct {
	int		numPlanes;		// this is always 6 for now
	idPlane	planes[6];
	// positive sides facing inward
	// plane 5 is always the plane the projection is going to, the
	// other planes are just clip planes
	// all planes are in global coordinates

	bool	makeClippedPlanes;
	// a projected light with a single frustum needs to make sil planes
	// from triangles that clip against side planes, but a point light
	// that has adjacent frustums doesn't need to
} shadowFrustum_t;


// areas have references to hold all the lights and entities in them
typedef struct areaReference_s {
	struct areaReference_s *areaNext;				// chain in the area
	struct areaReference_s *areaPrev;
	struct areaReference_s *ownerNext;				// chain on either the entityDef or lightDef
	idRenderEntityLocal *	entity;					// only one of entity / light will be non-NULL
	idRenderLightLocal *	light;					// only one of entity / light will be non-NULL
	struct portalArea_s	*	area;					// so owners can find all the areas they are in
} areaReference_t;


// idRenderLight should become the new public interface replacing the qhandle_t to light defs in the idRenderWorld interface
class idRenderLight {
public:
	virtual					~idRenderLight() {}

	virtual void			FreeRenderLight() = 0;
	virtual void			UpdateRenderLight( const renderLight_t *re, bool forceUpdate = false ) = 0;
	virtual void			GetRenderLight( renderLight_t *re ) = 0;
	virtual void			ForceUpdate() = 0;
	virtual int				GetIndex() = 0;
};


// idRenderEntity should become the new public interface replacing the qhandle_t to entity defs in the idRenderWorld interface
class idRenderEntity {
public:
	virtual					~idRenderEntity() {}

	virtual void			FreeRenderEntity() = 0;
	virtual void			UpdateRenderEntity( const renderEntity_t *re, bool forceUpdate = false ) = 0;
	virtual void			GetRenderEntity( renderEntity_t *re ) = 0;
	virtual void			ForceUpdate() = 0;
	virtual int				GetIndex() = 0;

	// overlays are extra polygons that deform with animating models for blood and damage marks
	virtual void			ProjectOverlay( const idPlane localTextureAxis[2], const idMaterial *material ) = 0;
	virtual void			RemoveDecals() = 0;
};


class idRenderLightLocal : public idRenderLight {
public:
							idRenderLightLocal();

	virtual void			FreeRenderLight();
	virtual void			UpdateRenderLight( const renderLight_t *re, bool forceUpdate = false );
	virtual void			GetRenderLight( renderLight_t *re );
	virtual void			ForceUpdate();
	virtual int				GetIndex();

	renderLight_t			parms;					// specification

	bool					lightHasMoved;			// the light has changed its position since it was
													// first added, so the prelight model is not valid

	float					modelMatrix[16];		// this is just a rearrangement of parms.axis and parms.origin

	idRenderWorldLocal *	world;
	int						index;					// in world lightdefs

	int						areaNum;				// if not -1, we may be able to cull all the light's
													// interactions if !viewDef->connectedAreas[areaNum]

	int						lastModifiedFrameNum;	// to determine if it is constantly changing,
													// and should go in the dynamic frame memory, or kept
													// in the cached memory
	bool					archived;				// for demo writing


	// derived information
	idPlane					lightProject[4];

	const idMaterial *		lightShader;			// guaranteed to be valid, even if parms.shader isn't
	idImage *				falloffImage;
	bool					dudeFillLight;			// DUDE-generated fill light (r_emissiveSurfaces screens / r_itemGlow): its tint
													// follows an animated screen and the per-view budget churns it, so ReSTIR DI
													// never classes it as fast-changing (stamped each frame in tr_light.cpp)
	unsigned int			dudeSerial;				// DUDE: creation serial, unique for the life of the process (AddLightDef). The
													// lightDef INDEX is reused as soon as a def is freed (a weapon switch frees the old
													// weapon light and allocates the flashlight into the same slot), so the ReSTIR DI
													// gradient compares THIS to know it is re-shading the same light (Plan R1)

	idVec3					globalLightOrigin;		// accounting for lightCenter and parallel


	idPlane					frustum[6];				// in global space, positive side facing out, last two are front/back
	idWinding *				frustumWindings[6];		// used for culling
	srfTriangles_t *		frustumTris;			// triangulated frustumWindings[]

	int						numShadowFrustums;		// one for projected lights, usually six for point lights
	shadowFrustum_t			shadowFrustums[6];

	int						viewCount;				// if == tr.viewCount, the light is on the viewDef->viewLights list
	struct viewLight_s *	viewLight;

	areaReference_t *		references;				// each area the light is present in will have a lightRef
	idInteraction *			firstInteraction;		// doubly linked list
	idInteraction *			lastInteraction;

	struct doublePortal_s *	foggedPortals;
};


class idRenderEntityLocal : public idRenderEntity {
public:
							idRenderEntityLocal();

	virtual void			FreeRenderEntity();
	virtual void			UpdateRenderEntity( const renderEntity_t *re, bool forceUpdate = false );
	virtual void			GetRenderEntity( renderEntity_t *re );
	virtual void			ForceUpdate();
	virtual int				GetIndex();

	// overlays are extra polygons that deform with animating models for blood and damage marks
	virtual void			ProjectOverlay( const idPlane localTextureAxis[2], const idMaterial *material );
	virtual void			RemoveDecals();

	renderEntity_t			parms;

	float					modelMatrix[16];		// this is just a rearrangement of parms.axis and parms.origin

	idRenderWorldLocal *	world;
	int						index;					// in world entityDefs

	int						lastModifiedFrameNum;	// to determine if it is constantly changing,
													// and should go in the dynamic frame memory, or kept
													// in the cached memory
	bool					archived;				// for demo writing

	idRenderModel *			dynamicModel;			// if parms.model->IsDynamicModel(), this is the generated data
	int						dynamicModelFrameCount;	// continuously animating dynamic models will recreate
													// dynamicModel if this doesn't == tr.viewCount
	idRenderModel *			cachedDynamicModel;

	idBounds				referenceBounds;		// the local bounds used to place entityRefs, either from parms or a model

	// a viewEntity_t is created whenever a idRenderEntityLocal is considered for inclusion
	// in a given view, even if it turns out to not be visible
	int						viewCount;				// if tr.viewCount == viewCount, viewEntity is valid,
													// but the entity may still be off screen
	struct viewEntity_s *	viewEntity;				// in frame temporary memory

	int						visibleCount;
	// if tr.viewCount == visibleCount, at least one ambient
	// surface has actually been added by R_AddAmbientDrawsurfs
	// note that an entity could still be in the view frustum and not be visible due
	// to portal passing

	idRenderModelDecal *	decals;					// chain of decals that have been projected on this model
	idRenderModelOverlay *	overlay;				// blood overlays on animated models

	areaReference_t *		entityRefs;				// chain of all references
	idInteraction *			firstInteraction;		// doubly linked list
	idInteraction *			lastInteraction;

	bool					needsPortalSky;
};


// viewLights are allocated on the frame temporary stack memory
// a viewLight contains everything that the back end needs out of an idRenderLightLocal,
// which the front end may be modifying simultaniously if running in SMP mode.
// a viewLight may exist even without any surfaces, and may be relevent for fogging,
// but should never exist if its volume does not intersect the view frustum
typedef struct viewLight_s {
	struct viewLight_s *	next;

	// back end should NOT reference the lightDef, because it can change when running SMP
	idRenderLightLocal *	lightDef;

	// for scissor clipping, local inside renderView viewport
	// scissorRect.Empty() is true if the viewEntity_t was never actually
	// seen through any portals
	idScreenRect			scissorRect;

	// if the view isn't inside the light, we can use the non-reversed
	// shadow drawing, avoiding the draws of the front and rear caps
	bool					viewInsideLight;

	// true if globalLightOrigin is inside the view frustum, even if it may
	// be obscured by geometry.  This allows us to skip shadows from non-visible objects
	bool					viewSeesGlobalLightOrigin;

	// if !viewInsideLight, the corresponding bit for each of the shadowFrustum
	// projection planes that the view is on the negative side of will be set,
	// allowing us to skip drawing the projected caps of shadows if we can't see the face
	int						viewSeesShadowPlaneBits;

	idVec3					globalLightOrigin;			// global light origin used by backend
	idPlane					lightProject[4];			// light project used by backend
	idPlane					fogPlane;					// fog plane for backend fog volume rendering
	const srfTriangles_t *	frustumTris;				// light frustum for backend fog volume rendering
	const idMaterial *		lightShader;				// light shader used by backend
	const float	*			shaderRegisters;			// shader registers used by backend
	idImage *				falloffImage;				// falloff image used by backend

	const struct drawSurf_s	*globalShadows;				// shadow everything
	const struct drawSurf_s	*localInteractions;			// don't get local shadows
	const struct drawSurf_s	*localShadows;				// don't shadow local Surfaces
	const struct drawSurf_s	*globalInteractions;		// get shadows from everything
	const struct drawSurf_s	*translucentInteractions;	// get shadows from everything

	// DUDE Phase 3.5: shadow-casting occluders that are NOT view-visible this frame
	// (behind/beside the camera) and therefore never made it into globalInteractions/
	// localInteractions. Shadow maps must still render them so the shadow they throw
	// onto a receiver that IS in view doesn't blink out when you turn away from the
	// caster. Populated only while r_shadowMapping is on; unused by the stencil path.
	const struct drawSurf_s	*shadowMapCasters;
} viewLight_t;


// a viewEntity is created whenever a idRenderEntityLocal is considered for inclusion
// in the current view, but it may still turn out to be culled.
// viewEntity are allocated on the frame temporary stack memory
// a viewEntity contains everything that the back end needs out of a idRenderEntityLocal,
// which the front end may be modifying simultaniously if running in SMP mode.
// A single entityDef can generate multiple viewEntity_t in a single frame, as when seen in a mirror
typedef struct viewEntity_s {
	struct viewEntity_s	*next;

	// back end should NOT reference the entityDef, because it can change when running SMP
	idRenderEntityLocal	*entityDef;

	// for scissor clipping, local inside renderView viewport
	// scissorRect.Empty() is true if the viewEntity_t was never actually
	// seen through any portals, but was created for shadow casting.
	// a viewEntity can have a non-empty scissorRect, meaning that an area
	// that it is in is visible, and still not be visible.
	idScreenRect		scissorRect;

	bool				weaponDepthHack;
	float				modelDepthHack;

	float				modelMatrix[16];		// local coords to global coords
	float				modelViewMatrix[16];	// local coords to eye coords
} viewEntity_t;


const int	MAX_CLIP_PLANES	= 1;				// we may expand this to six for some subview issues

// viewDefs are allocated on the frame temporary stack memory
typedef struct viewDef_s {
	// specified in the call to DrawScene()
	renderView_t		renderView;

	float				projectionMatrix[16];
	// projectionMatrix with any sub-pixel jitter removed (bit-identical to projectionMatrix
	// when r_jitter is off). Temporal consumers (SSAO/SSR reprojection, motion vectors, FSR2)
	// must reproject against the un-jittered world->clip or the frame jitter smears the
	// history. See docs/fsr-temporal-pipeline.md (increments A1/B).
	float				unjitteredProjectionMatrix[16];
	// sub-pixel projection jitter applied THIS frame, in pixels ([-0.5,0.5], +Y-up), 0 when off
	// (docs/fsr-temporal-pipeline.md B). Halton(2,3) per rendered frame; handed to FSR2's
	// jitterOffset in C2 so it can undo the jitter it accumulates. Main fullscreen view only.
	float				jitter[2];
	viewEntity_t		worldSpace;

	idRenderWorldLocal *renderWorld;

	float				floatTime;

	idVec3				initialViewAreaOrigin;
	// Used to find the portalArea that view flooding will take place from.
	// for a normal view, the initialViewOrigin will be renderView.viewOrg,
	// but a mirror may put the projection origin outside
	// of any valid area, or in an unconnected area of the map, so the view
	// area must be based on a point just off the surface of the mirror / subview.
	// It may be possible to get a failed portal pass if the plane of the
	// mirror intersects a portal, and the initialViewAreaOrigin is on
	// a different side than the renderView.viewOrg is.

	bool				isSubview;				// true if this view is not the main view
	bool				isMirror;				// the portal is a mirror, invert the face culling
	bool				isXraySubview;
	// DUDE: this world view covers the whole window -- THE primary scene view. Set once by
	// RenderScene, against the WINDOW and before any FSR2 sub-native scaling; it is the one view
	// RenderScene scales to the FSR2 render size and the only one the full-screen scene passes
	// (HDR routing, SSAO / RT / NRD, FSR2, post) may treat as primary. Testing the viewport
	// against the scaled world size instead misclassified any cropped render at least that
	// large: the level-change wipe capture is 2048x1024 on a 1440p window, which passes a
	// 1715x965 (r_fsrScale 0.67) test. Subviews inherit it; every test also excludes isSubview.
	bool				isWindowView;

	bool				isEditor;

	int					numClipPlanes;			// mirrors will often use a single clip plane
	idPlane				clipPlanes[MAX_CLIP_PLANES];		// in world space, the positive side
												// of the plane is the visible side
	idScreenRect		viewport;				// in real pixels and proper Y flip

	idScreenRect		scissor;
	// for scissor clipping, local inside renderView viewport
	// subviews may only be rendering part of the main view
	// these are real physical pixel values, possibly scaled and offset from the
	// renderView x/y/width/height

	struct viewDef_s *	superView;				// never go into an infinite subview loop
	struct drawSurf_s *	subviewSurface;

	// drawSurfs are the visible surfaces of the viewEntities, sorted
	// by the material sort parameter
	drawSurf_t **		drawSurfs;				// we don't use an idList for this, because
	int					numDrawSurfs;			// it is allocated in frame temporary memory
	int					maxDrawSurfs;			// may be resized

	struct viewLight_s	*viewLights;			// chain of all viewLights effecting view
	struct viewEntity_s	*viewEntitys;			// chain of all viewEntities effecting view, including off screen ones casting shadows
	// we use viewEntities as a check to see if a given view consists solely
	// of 2D rendering, which we can optimize in certain ways.  A 2D view will
	// not have any viewEntities

	idPlane				frustum[5];				// positive sides face outward, [4] is the front clip plane
	idFrustum			viewFrustum;

	int					areaNum;				// -1 = not in a valid area

	bool *				connectedAreas;
	// An array in frame temporary memory that lists if an area can be reached without
	// crossing a closed door.  This is used to avoid drawing interactions
	// when the light is behind a closed door.

} viewDef_t;


// complex light / surface interactions are broken up into multiple passes of a
// simple interaction shader
typedef struct {
	const drawSurf_t *	surf;

	idImage *			lightImage;
	idImage *			lightFalloffImage;
	idImage *			bumpImage;
	idImage *			diffuseImage;
	idImage *			specularImage;

	idVec4				diffuseColor;	// may have a light color baked into it, will be < tr.backEndRendererMaxLight
	idVec4				specularColor;	// may have a light color baked into it, will be < tr.backEndRendererMaxLight
	stageVertexColor_t	vertexColor;	// applies to both diffuse and specular

	int					ambientLight;	// use tr.ambientNormalMap instead of normalization cube map
	// (not a bool just to avoid an uninitialized memory check of the pad region by valgrind)

	// these are loaded into the vertex program
	idVec4				localLightOrigin;
	idVec4				localViewOrigin;
	idVec4				lightProjection[4];	// in local coordinates, possibly with a texture matrix baked in
	idVec4				bumpMatrix[2];
	idVec4				diffuseMatrix[2];
	idVec4				specularMatrix[2];
} drawInteraction_t;


/*
=============================================================

RENDERER BACK END COMMAND QUEUE

TR_CMDS

=============================================================
*/

typedef enum {
	RC_NOP,
	RC_DRAW_VIEW,
	RC_SET_BUFFER,
	RC_COPY_RENDER,
	RC_SWAP_BUFFERS		// can't just assume swap at end of list because
						// of forced list submission before syncs
} renderCommand_t;

typedef struct {
	renderCommand_t		commandId, *next;
} emptyCommand_t;

typedef struct {
	renderCommand_t		commandId, *next;
	GLenum	buffer;
	int		frameCount;
} setBufferCommand_t;

typedef struct {
	renderCommand_t		commandId, *next;
	viewDef_t	*viewDef;
} drawSurfsCommand_t;

typedef struct {
	renderCommand_t		commandId, *next;
	int		x, y, imageWidth, imageHeight;
	idImage	*image;
	int		cubeFace;					// when copying to a cubeMap
} copyRenderCommand_t;


//=======================================================================

// this is the inital allocation for max number of drawsurfs
// in a given view, but it will automatically grow if needed
const int	INITIAL_DRAWSURFS =			0x4000;

// a request for frame memory will never fail
// (until malloc fails), but it may force the
// allocation of a new memory block that will
// be discontinuous with the existing memory
typedef struct frameMemoryBlock_s {
	struct frameMemoryBlock_s *next;
	int		size;
	int		used;
	int		poop;			// so that base is 16 byte aligned
	byte	base[4];	// dynamically allocated as [size]
} frameMemoryBlock_t;

// all of the information needed by the back end must be
// contained in a frameData_t.  This entire structure is
// duplicated so the front and back end can run in parallel
// on an SMP machine (OBSOLETE: this capability has been removed)
typedef struct {
	// one or more blocks of memory for all frame
	// temporary allocations
	frameMemoryBlock_t	*memory;

	// alloc will point somewhere into the memory chain
	frameMemoryBlock_t	*alloc;

	srfTriangles_t *	firstDeferredFreeTriSurf;
	srfTriangles_t *	lastDeferredFreeTriSurf;

	int					memoryHighwater;	// max used on any frame

	// the currently building command list
	// commands can be inserted at the front if needed, as for required
	// dynamically generated textures
	emptyCommand_t	*cmdHead, *cmdTail;		// may be of other command type based on commandId
} frameData_t;

extern	frameData_t	*frameData;

//=======================================================================

void R_ClearCommandChain( void );
void R_AddDrawViewCmd( viewDef_t *parms );

void R_ReloadGuis_f( const idCmdArgs &args );
void R_ListGuis_f( const idCmdArgs &args );

void *R_GetCommandBuffer( int bytes );

// this allows a global override of all materials
bool R_GlobalShaderOverride( const idMaterial **shader );

// this does various checks before calling the idDeclSkin
const idMaterial *R_RemapShaderBySkin( const idMaterial *shader, const idDeclSkin *customSkin, const idMaterial *customShader );


//====================================================


/*
** performanceCounters_t
*/
typedef struct {
	int		c_sphere_cull_in, c_sphere_cull_clip, c_sphere_cull_out;
	int		c_box_cull_in, c_box_cull_out;
	int		c_createInteractions;	// number of calls to idInteraction::CreateInteraction
	int		c_createLightTris;
	int		c_createShadowVolumes;
	int		c_shadowVolumesSkipped;	// Phase 0: builds skipped because the light will be shadow-mapped (r_shadowMapSkipStencilBuild)
	int		c_generateMd5;
	int		c_entityDefCallbacks;
	int		c_alloc, c_free;	// counts for R_StaticAllc/R_StaticFree
	int		c_visibleViewEntities;
	int		c_shadowViewEntities;
	int		c_viewLights;
	int		c_numViews;			// number of total views rendered
	int		c_deformedSurfaces;	// idMD5Mesh::GenerateSurface
	int		c_deformedVerts;	// idMD5Mesh::GenerateSurface
	int		c_deformedIndexes;	// idMD5Mesh::GenerateSurface
	int		c_tangentIndexes;	// R_DeriveTangents()
	int		c_entityUpdates, c_lightUpdates, c_entityReferences, c_lightReferences;
	int		c_guiSurfs;
	int		frontEndMsec;		// sum of time in all RE_RenderScene's in a frame
	double	frontEndMs;			// same, sub-millisecond (DUDE benchmark)
} performanceCounters_t;


typedef struct {
	int		current2DMap;
	int		current3DMap;
	int		currentCubeMap;
	int		texEnv;
	textureType_t	textureType;
} tmu_t;

// Sizes the tmu[] bind cache. Historically 8 (the fixed-function multitexture limit).
// DUDE: raised to 16 so the enhancement backends can cache-bind sampler units past 8
// (unit 9 = SSAO, unit 10 = the baked occlusion map). The legacy cap on reported
// fixed-function units (glConfig.maxTextureUnits, from GL_MAX_TEXTURE_UNITS_ARB) is
// clamped to this too, but that value is <= 8 on real hardware, so legacy is unaffected.
const int MAX_MULTITEXTURE_UNITS =	16;
typedef struct {
	tmu_t		tmu[MAX_MULTITEXTURE_UNITS];
	int			currenttmu;

	int			faceCulling;
	int			glStateBits;
	bool		forceGlState;		// the next GL_State will ignore glStateBits and set everything
} glstate_t;


typedef struct {
	int		c_surfaces;
	int		c_shaders;
	int		c_vertexes;
	int		c_indexes;		// one set per pass
	int		c_totalIndexes;	// counting all passes

	int		c_drawElements;
	int		c_drawIndexes;
	int		c_drawVertexes;
	int		c_drawRefIndexes;
	int		c_drawRefVertexes;

	int		c_shadowElements;
	int		c_shadowIndexes;
	int		c_shadowVertexes;

	int		c_vboIndexes;
	float	c_overDraw;

	float	maxLightValue;	// for light scale
	int		msec;			// total msec for backend run
	double	ms;				// same, sub-millisecond (DUDE benchmark)
} backEndCounters_t;

// all state modified by the back end is separated
// from the front end state
typedef struct {
	int					frameCount;		// used to track all images used in a frame
	const viewDef_t	*	viewDef;
	backEndCounters_t	pc;

	const viewEntity_t *currentSpace;		// for detecting when a matrix must change
	idScreenRect		currentScissor;
	// for scissor clipping, local inside renderView viewport

	viewLight_t *		vLight;
	int					depthFunc;			// GLS_DEPTHFUNC_EQUAL, or GLS_DEPTHFUNC_LESS for translucent
	float				lightTextureMatrix[16];	// only if lightStage->texture.hasMatrix
	float				lightColor[4];		// evaluation of current light's color stage

	float				lightScale;			// Every light color calaculation will be multiplied by this,
											// which will guarantee that the result is < tr.backEndRendererMaxLight
											// A card with high dynamic range will have this set to 1.0
	float				overBright;			// The amount that all light interactions must be multiplied by
											// with post processing to get the desired total light level.
											// A high dynamic range card will have this set to 1.0.

	bool				currentRenderCopied;	// true if any material has already referenced _currentRender
	bool				smokeBackgroundCaptured;	// DUDE: _currentRender snapshotted this view for smoke-darkness blend

	// our OpenGL state deltas
	glstate_t			glState;

	int					c_copyFrameBuffer;
} backEndState_t;


const int MAX_GUI_SURFACES	= 1024;		// default size of the drawSurfs list for guis, will
										// be automatically expanded as needed

typedef enum {
	BE_ARB2,
	BE_BAD
} backEndName_t;

typedef struct {
	int		x, y, width, height;	// these are in physical, OpenGL Y-at-bottom pixels
} renderCrop_t;
static const int	MAX_RENDER_CROPS = 8;

/*
** Most renderer globals are defined here.
** backend functions should never modify any of these fields,
** but may read fields that aren't dynamically modified
** by the frontend.
*/
class idRenderSystemLocal : public idRenderSystem {
public:
	// external functions
	virtual void			Init( void );
	virtual void			Shutdown( void );
	virtual void			InitOpenGL( void );
	virtual void			ShutdownOpenGL( void );
	virtual bool			IsOpenGLRunning( void ) const;
	virtual bool			IsFullScreen( void ) const;
	virtual int				GetScreenWidth( void ) const;
	virtual int				GetScreenHeight( void ) const;
	virtual idRenderWorld *	AllocRenderWorld( void );
	virtual void			FreeRenderWorld( idRenderWorld *rw );
	virtual void			BeginLevelLoad( void );
	virtual void			EndLevelLoad( void );
	virtual bool			RegisterFont( const char *fontName, fontInfoEx_t &font );
	virtual void			SetColor( const idVec4 &rgba );
	virtual void			SetColor4( float r, float g, float b, float a );
	virtual void			DrawStretchPic ( const idDrawVert *verts, const glIndex_t *indexes, int vertCount, int indexCount, const idMaterial *material,
											bool clip = true, float x = 0.0f, float y = 0.0f, float w = 640.0f, float h = 0.0f );
	virtual void			DrawStretchPic ( float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial *material );

	virtual void			DrawStretchTri ( idVec2 p1, idVec2 p2, idVec2 p3, idVec2 t1, idVec2 t2, idVec2 t3, const idMaterial *material );
	virtual void			GlobalToNormalizedDeviceCoordinates( const idVec3 &global, idVec3 &ndc );
	virtual void			GetGLSettings( int& width, int& height );
	virtual void			PrintMemInfo( MemInfo_t *mi );

	virtual void			DrawSmallChar( int x, int y, int ch, const idMaterial *material );
	virtual void			DrawSmallStringExt( int x, int y, const char *string, const idVec4 &setColor, bool forceColor, const idMaterial *material );
	virtual void			DrawBigChar( int x, int y, int ch, const idMaterial *material );
	virtual void			DrawBigStringExt( int x, int y, const char *string, const idVec4 &setColor, bool forceColor, const idMaterial *material );
	virtual void			WriteDemoPics();
	virtual void			DrawDemoPics();
	virtual void			BeginFrame( int windowWidth, int windowHeight );
	virtual void			EndFrame( int *frontEndMsec, int *backEndMsec );
	virtual void			GetFrameStats( renderFrameStats_t &stats );
	virtual void			SetGpuTimerRequested( bool on );
	virtual void			TakeScreenshot( int width, int height, const char *fileName, int downSample, renderView_t *ref );
	virtual void			CropRenderSize( int width, int height, bool makePowerOfTwo = false, bool forceDimensions = false );
	virtual void			CaptureRenderToImage( const char *imageName );
	virtual void			CaptureRenderToFile( const char *fileName, bool fixAlpha );
	virtual void			UnCrop();
	virtual bool			UploadImage( const char *imageName, const byte *data, int width, int height );

public:
	// internal functions
							idRenderSystemLocal( void );
							~idRenderSystemLocal( void );

	void					Clear( void );
	void					SetBackEndRenderer();			// sets tr.backEndRenderer based on cvars
	void					RenderViewToViewport( const renderView_t *renderView, idScreenRect *viewport );

public:
	// renderer globals
	bool					registered;		// cleared at shutdown, set at InitOpenGL

	bool					takingScreenshot;
	bool					takingEnvProbe;	// DUDE glass probes (docs/ssr.md): baking — keep the view weapon out of the capture

	int						frameCount;		// incremented every frame
	int						viewCount;		// incremented every view (twice a scene if subviewed)
											// and every R_MarkFragments call

	int						staticAllocCount;	// running total of bytes allocated

	float					frameShaderTime;	// shader time for all non-world 2D rendering

	int						viewportOffset[2];	// for doing larger-than-window tiled renderings
	int						tiledViewport[2];

	// determines which back end to use, and if vertex programs are in use
	backEndName_t			backEndRenderer;
	bool					backEndRendererHasVertexPrograms;
	float					backEndRendererMaxLight;	// 1.0 for standard, unlimited for floats
														// determines how much overbrighting needs
														// to be done post-process

	idVec4					ambientLightVector;	// used for "ambient bump mapping"

	float					sortOffset;				// for determinist sorting of equal sort materials

	idList<idRenderWorldLocal*>worlds;

	idRenderWorldLocal *	primaryWorld;
	renderView_t			primaryRenderView;
	viewDef_t *				primaryView;
	// many console commands need to know which world they should operate on

	const idMaterial *		defaultMaterial;
	idImage *				testImage;
	idCinematic *			testVideo;
	float					testVideoStartTime;

	idImage *				ambientCubeImage;	// hack for testing dependent ambient lighting

	viewDef_t *				viewDef;

	performanceCounters_t	pc;					// performance counters
	renderFrameStats_t		lastFrameStats;		// DUDE benchmark: EndFrame snapshot (GetFrameStats)

	drawSurfsCommand_t		lockSurfacesCmd;	// use this when r_lockSurfaces = 1
	//renderView_t			lockSurfacesRenderView;
	viewDef_t				lockSurfacesViewDef; // of locked position/view
	viewDef_t				lockSurfacesRealViewDef; // of actual player position

	viewEntity_t			identitySpace;		// can use if we don't know viewDef->worldSpace is valid
	int						stencilIncr, stencilDecr;	// GL_INCR / INCR_WRAP_EXT, GL_DECR / GL_DECR_EXT

	renderCrop_t			renderCrops[MAX_RENDER_CROPS];
	int						currentRenderCrop;

	// GUI drawing variables for surface creation
	int						guiRecursionLevel;		// to prevent infinite overruns
	class idGuiModel *		guiModel;
	class idGuiModel *		demoGuiModel;

	// DG: remember the original glConfig.vidWidth/Height values that get overwritten in BeginFrame()
	//     so they can be reset in EndFrame() (Editors tend to mess up the viewport by using BeginFrame())
	int						origWidth;
	int						origHeight;

	// DG: taken from the current map's worldspawn ("allow_nospecular")
	//     true if (unlike in Vanilla Doom3) the "nospecular" parm of a light should be respected
	bool					allowNoSpecular;
};

extern backEndState_t		backEnd;
extern idRenderSystemLocal	tr;
extern glconfig_t			glConfig;		// outside of TR since it shouldn't be cleared during ref re-init


//
// cvars
//
extern idCVar r_graphicsAPI;			// rendering backend: opengl, vulkan, vulkan-rt (vulkan* in development)
extern idCVar r_mode;					// video mode number
extern idCVar r_displayRefresh;			// optional display refresh rate option for vid mode
extern idCVar r_fullscreen;				// 0 = windowed, 1 = full screen
extern idCVar r_fullscreenDesktop;		// 0: 'real' fullscreen mode 1: keep resolution 'desktop' fullscreen mode
extern idCVar r_fullscreenDisplay;		// which monitor to open on; -1 = auto (mouse cursor's display)
extern idCVar r_multiSamples;			// number of antialiasing samples
extern idCVar r_windowResizable;		// DG: allow resizing and maximizing the window

extern idCVar r_ignore;					// used for random debugging without defining new vars
extern idCVar r_ignore2;				// used for random debugging without defining new vars
extern idCVar r_znear;					// near Z clip plane

extern idCVar r_finish;					// force a call to glFinish() every frame
extern idCVar r_frontBuffer;			// draw to front buffer for debugging
extern idCVar r_swapInterval;			// changes the GL swap interval
extern idCVar r_vrr;					// variable refresh rate (G-SYNC/FreeSync/Adaptive-Sync) present+pacing mode
extern idCVar dude_vsync;				// native-menu VSync bridge: 0 off, 1 on, 2 adaptive, 3 VRR (mirrors r_swapInterval+r_vrr)
extern idCVar r_offsetFactor;			// polygon offset parameter
extern idCVar r_offsetUnits;			// polygon offset parameter
extern idCVar r_singleTriangle;			// only draw a single triangle per primitive
extern idCVar r_clear;					// force screen clear every frame
extern idCVar r_shadows;				// enable shadows
extern idCVar r_subviewOnly;			// 1 = don't render main view, allowing subviews to be debugged
extern idCVar r_lightScale;				// all light intensities are multiplied by this, which is normally 2
extern idCVar r_flareSize;				// scale the flare deforms from the material def

extern idCVar r_gamma;					// changes gamma tables
extern idCVar r_brightness;				// changes gamma tables
extern idCVar r_gammaInShader;			// set gamma+brightness in shader instead of modifying system gamma tables

// DUDE non-vanilla post-process effects — GL3/Vulkan backends only, default off
extern idCVar r_postFilmGrain;			// film grain intensity (0 = off)
extern idCVar r_postFilmGrainSize;		// grain cell size in pixels (1 = per-pixel)
extern idCVar r_postChromaticAberration;	// chromatic aberration strength (0 = off)
extern idCVar r_rhiAA;					// post-resolve antialiasing (0 = off, 1 = FXAA)
extern idCVar r_hdr;					// HDR float scene buffer (removes banding); opengl3/Vulkan only
extern idCVar r_fsr;					// AMD FSR2 temporal AA, Native-AA mode (Vulkan only, R1/C2)
extern idCVar r_fsrSharpness;			// FSR2 RCAS sharpening amount (0 = off)
extern idCVar r_fsrReactive;			// FSR2 auto-reactive mask: deghost additive/translucent content (R1/D)
extern idCVar r_fsrReactiveScale;		// FSR2 auto-reactive mask strength
extern idCVar r_fxaaStrength;			// FXAA subpixel smoothing amount (0 = edge-only .. 1)

// DUDE Phase 3.5 specular tuning — GL3/Vulkan interaction shader only, vanilla at default
extern idCVar r_shading;				// 0 = vanilla LUT, 1 = Blinn-Phong
extern idCVar r_specularScale;			// scales specular contribution (1 = vanilla)
extern idCVar r_specularExp;			// analytic specular exponent (r_shading 1)

// DUDE PBR materials (docs/pbr-materials.md) — GL3/Vulkan interaction shader only
void R_PbrTableInvalidate( void );		// drop the cached per-material table (reloadPbrTable)
int  R_PbrTableReloadApply( void );		// invalidate + re-apply to all materials, returns count
// in-game material editor (Com_DrawPbrMaterialEditor): pick the crosshair material
// and persist an override line to pbr/pbr_overrides.cfg (fs_basepath), then reload
const idMaterial *R_PbrPickCrosshairMaterial( void );
bool R_PbrWriteOverrideLine( const idMaterial *mat, float metal, float rough,
                             float wet, float env, int category );
extern idCVar r_pbr;					// GGX interaction path; supersedes r_shading while on
extern idCVar r_pbrRoughness;			// Phase A global fallback roughness (per-material in Phase B)
extern idCVar r_pbrSpecScale;			// artistic energy scale on the GGX lobe (1.66 = calibrated look)
extern idCVar r_pbrMetalDiffuse;		// how much albedo colour metals keep (1 = all, 0 = physical kill)
extern idCVar r_pbrToksvigBase;			// Toksvig variance baseline (anti-firefly vs highlight tightness)
extern idCVar r_pbrFireflyClamp;		// GGX lobe ceiling (spike suppression / highlight-core cap)
// Per-category defaults (metal/rough/wet/env) live in a table now (Material.cpp,
// R_PbrCategoryDefaults), persisted as @cat rows in the pbr config — not cvars.
extern idCVar r_pbrEnvScale;			// Phase C.1 metal env floor GLOBAL base (per-category env rides on top)

// DUDE skin / flesh subsurface scattering (docs/skin-shading.md) — wrapped diffuse on the
// classifier's skin/flesh categories; diffuse-only, works on every shading model (all tiers)
extern idCVar r_skinShading;			// SSS strength on skin/flesh materials (0 = off .. 1 = full)
extern idCVar r_skinScatter;			// terminator wrap reach (red channel; g/b derive shorter)

// DUDE PBR Phase C.2 screen-space reflections (docs/ssr.md) — GL3 backend only
extern idCVar r_ssr;					// enable; reflectivity follows the PBR material table
extern idCVar r_ssrIntensity;			// reflection strength multiplier
extern idCVar r_ssrMaxRoughness;		// gloss cutoff (v1 is sharp-only, keep low)
extern idCVar r_ssrRoughnessFade;		// fade start as a fraction of the cutoff (lower = gentler transition)
extern idCVar r_ssrFireflyClamp;		// cap on reflected HDR radiance (kills bright-speckle fireflies)
extern idCVar r_ssrSteps;				// ray-march samples per pixel
extern idCVar r_ssrMaxDistance;			// ray reach, world units
extern idCVar r_ssrThickness;			// assumed surface thickness for hit tests
extern idCVar r_ssrResScale;			// march buffer resolution as a fraction of the view
extern idCVar r_ssrHiZ;					// min-Z depth pyramid to leap empty march span (dev A/B)
extern idCVar r_ssrHiZLevel;			// coarse mip level the Hi-Z march leaps at
extern idCVar r_ssrTemporal;			// accumulate across frames (resolves the march grain)
extern idCVar r_ssrTemporalFeedback;	// history fraction kept per frame
extern idCVar r_ssrGlossy;				// roughness-blurred reflections (reflection mip pyramid)
extern idCVar r_ssrGlossyScale;			// blur-amount multiplier on the roughness-driven LOD
extern idCVar r_ssrGlassProbes;			// glass reflects baked per-area room cubemaps
extern idCVar r_ssrGlassProbeBake;		// auto-capture missing probes (one-time hitch per area)
extern idCVar r_ssrGlassProbeSize;		// probe face resolution
extern idCVar r_ssrGlassProbeScale;		// probe brightness on glass (glass-only intensity)
extern idCVar r_specOcclusion;			// RR14: roughness-aware AO dimming of SSR + RT reflections

// DUDE screen-space contact shadows (docs/contact-shadows.md): per-light depth-buffer march grounding
extern idCVar r_contactShadows;			// master toggle (opt-in, raster tiers; UN uses r_rtAllLights)
extern idCVar r_contactShadowStrength;	// darkening on a found occluder (0..1)
extern idCVar r_contactShadowLength;	// march length toward the light (world units)
extern idCVar r_contactShadowThickness;	// occluder thickness band (world units)
extern idCVar r_contactShadowSteps;		// march steps per lit pixel (2..32)

// DUDE glass probes (docs/ssr.md): fs_savepath-relative extensionless base path
// for a map area's probe faces (envprobes/<map>/area<N>; + _px.tga etc), and the
// backend cache invalidation the bake command calls after writing new faces
void R_GlassProbeBasePath( const char *mapName, int area, idStr &out );
void RB_RHI_InvalidateGlassProbe( int area );

// DUDE Phase 3.5 shadow mapping — GL3/Vulkan only, stencil stays the default
extern idCVar r_shadowMapping;			// 0 = stencil volumes, 1 = shadow maps where supported
extern idCVar r_shadowMapSize;			// shadow map resolution per light
extern idCVar r_shadowMapBias;			// depth-compare bias, world/perforated receivers
extern idCVar r_shadowMapModelBias;		// depth-compare bias, model (non-world) receivers
extern idCVar r_shadowMapFlashlightBias;	// depth-compare bias, player flashlight (small)
extern idCVar r_shadowMapSlopeBias;		// slope-scaled bias multiplier (grazing-angle acne)
extern idCVar r_shadowMapDebug;			// print per-view light classification
extern idCVar r_shadowMapCacheDebug;	// once/sec cube-cache breakdown (hit rate + re-render causes)
extern idCVar r_shadowMapCachePerFace;	// warm miss re-renders only the faces an occluder moved across
extern idCVar r_shadowMapCacheSplit;	// split a moving-caster light into a cached static cube + a scratch dynamic cube
extern idCVar r_shadowMapSplitDynDrop;	// render the split dynamic cube N resolution tiers below the static cube (0 = same)
extern idCVar r_shadowMapCull;			// caster face culling (0 front / 1 back / 2 two-sided)
extern idCVar r_shadowMapPerforated;	// let noShadows perforated surfaces cast punched-out shadow maps
extern idCVar r_shadowMapViewWeapon;	// view weapon casts shadow-map shadows (default 0 = kept out of the map, no floor blob)
extern idCVar r_shadowMapPerforatedStrength;	// perforated shadow darkness (1 full, <1 dithered lighter)
extern idCVar r_shadowMapPointSize;		// point-light cube shadow map resolution per face
extern idCVar r_shadowMapCubePcf;		// point-light cube shadow PCF tap count (1 = single hardware tap)
extern idCVar r_shadowMapPointLimit;	// max point lights cube-shadowed per view (importance budget)
extern idCVar r_shadowMapPointRangeScale;	// scale point-light cube shadow range (far plane + depth normalizer)
extern idCVar r_shadowMapSizeScale;			// scale per-light shadow resolution with light radius
extern idCVar r_shadowMapSizeScaleRadius;	// light radius mapped to the base shadow resolution
extern idCVar r_shadowMapFaceCull;			// skip cube faces outside the view frustum
extern idCVar r_shadowMapCache;				// cache static point-light cube maps across frames
extern idCVar r_shadowMapCacheMB;			// VRAM budget for the shadow-map cache (MB; -1 auto, 0 unlimited)
extern idCVar r_shadowMapBudgetHysteresis;	// point-light budget stickiness (percent score bonus for recent incumbents)
extern idCVar r_shadowMapMaxUpdates;		// max cached cube maps re-rendered per view (stagger update bursts; 0 = unlimited)
extern idCVar r_shadowMapStencilRadius;		// lights bigger than this (max radius axis) fall back to stencil shadows
extern idCVar r_shadowMapSkipStencilBuild;	// skip building CPU stencil volumes for animated casters under shadow-mapped lights
extern idCVar r_shadowMapSun;				// oversize/parallel "sun" lights: per-view fitted 2D map instead of stencil
extern idCVar r_shadowMapSunBias;			// sun map depth-compare bias
extern idCVar r_shadowMapSunRange;			// how far ahead of the camera the sun map covers (world units)
extern idCVar r_shadowMapNormalOffset;		// normal-offset shadow bias in texels (cube + sun lookups; 0 = off)
extern idCVar r_rtSunShadows;				// RT sun shadows: trace sun visibility per fragment (VK + RT hardware)
extern idCVar r_rtReflections;				// RT reflections on monsters: trace + shade the dynamic hit (VK + RT; rides SSR)
extern idCVar r_rtao;						// H4 RTAO: ray-traced AO producer replacing GTAO at the RT tier
extern idCVar r_rtaoRadius;					// RTAO ray length (world units)
extern idCVar r_rtaoResScale;			// RTAO render/denoise resolution fraction (bilinear upscale in the resolve)
extern idCVar r_rtaoIntensity;				// RTAO strength (1.0 neutral; r_ssaoIntensity stays GTAO-only)
extern idCVar r_rtaoHistory;				// RTAO denoiser (ReBLUR) max accumulated history frames; 0 = NRD default (~30)
extern idCVar r_rtaoHistoryMs;				// R0: RTAO history in ms (0 = frames)
extern idCVar r_rtaoFastClamp;				// R0: RTAO NRD fastHistoryClampingSigmaScale (2.0 = NRD default; 1.1 = NRD's occlusion value)
extern idCVar r_rtaoHistoryFix;				// R0: RTAO NRD historyFixFrameNum (3 = NRD default)
extern idCVar r_skinnedMotionVectors;		// GPU-skinned characters write per-bone motion into the velocity buffer
extern idCVar r_rtaoDebug;					// RTAO debug view (1 raw rays, 2 denoised)
extern idCVar r_rtReStirDI;					// H7 ReSTIR DI: reservoir resampling of the scene lights (docs/rtx-restir-di.md); Stage 1 = unshadowed RIS pass
extern idCVar r_rtLightTextures;			// RT scene-light buffer carries + samples each light's projection cookie / falloff ramp (bindless); 0 = analytic frustum attenuation
extern idCVar r_rtReStirDIApply;			// ReSTIR DI lights the scene: deferred G-buffer shade + additive apply + forward per-light suppression (Stage D)
extern idCVar r_rtReStirDIDynamicForward;	// ReSTIR DI: keep first-person weapon lights forward (R6: bool; the gradient confidence handles every other light in DI)
extern idCVar r_rtReStirDICombineGI;		// ReSTIR DI gathers the RTGI bounce into its own buffer: one NRD denoise + one apply for direct + indirect
extern idCVar r_rtReStirDISH;				// ReSTIR DI: SH denoise (direction + full-res cosine resolve) so bump detail survives
extern idCVar r_rtReStirDISpecReshade;		// ReSTIR DI: diffuse-only buffer + full-res GGX specular rebuilt in the apply (sharp glint)
extern idCVar r_rtReStirDICandidates;		// ReSTIR DI: light candidates per pixel before the reservoir pick (0 = every light, exhaustive RIS)
extern idCVar r_rtReStirDICheapTarget;		// ReSTIR DI: cheap RIS target (irradiance, no GGX) for the candidate loop; full-shade only the winner
extern idCVar r_rtReStirDIDebug;			// ReSTIR DI debug view (1 = blit the DI irradiance - denoised when it ran, else raw - over the scene)
extern idCVar r_rtReStirDIHistoryFix;		// ReSTIR DI NRD historyFixFrameNum: spatial reconstruction frames where history is thin (moving parts)
extern idCVar r_rtReStirDIEdgeStop;			// ReSTIR DI NRD ReLAX edge-stopping scale (lobeAngle); >1 blurs across detail edges (grates), costs detail
extern idCVar r_rtReStirDIHistory;			// ReSTIR DI NRD history length (maxAccumFrames); DI's own, mirrors RTGI's r_rtgiHistory (2..63)
extern idCVar r_rtReStirDIHistoryMs;		// R0: DI history in ms (0 = frames)
extern idCVar r_rtReStirDIFastClamp;		// R0: DI NRD fastHistoryClampingSigmaScale (2.0 = NRD default)
extern idCVar r_rtReStirDIAntilag;			// R0: DI NRD built-in antilag on/off (A/B baseline)
extern idCVar r_rtReStirDIDisocclusion;	// R0: DI NRD disocclusionThreshold (0.01 = NRD default)
extern idCVar r_rtReStirDIGradient;		// R1: temporal gradient confidence (re-shade last frame's light) -> NRD IN_DIFF_CONFIDENCE
extern idCVar r_rtReStirDIGradientPower;	// R1: confidence sharpening power
extern idCVar r_rtReStirDIGradientHistory;	// R1: rise-only temporal smoothing length (frames)
extern idCVar r_rtReStirDIGradientFloor;	// R1: minimum confidence (ReBLUR raises it to historyFix / history)
extern idCVar r_rtReStirDIGradientBias;		// R1: darkness bias in the change denominator
extern idCVar r_rtReStirDIGradientDeadzone;	// R1: relative per-light change below which nothing counts (authored flicker)
extern idCVar r_rtReStirDIGradientStatic;	// R1: static threshold (full-res px) for the exact previous-position re-shade
extern idCVar r_rtReStirDIGradientStratum;	// R1: NxN stratum size (3 = RTXDI, 5 = NRD's 1/5 res)
extern idCVar r_rtReStirDIGradientFilterPasses;	// R1: A-trous passes over the gradient (4 = RTXDI)
extern idCVar r_rtReStirDIRelaxFast;		// R2: ReLAX fast history (Remix direct 1)
extern idCVar r_rtReStirDIRelaxPrepass;		// R2: ReLAX prepass blur px (Remix direct 0)
extern idCVar r_rtReStirDIRelaxPhi;			// R2: ReLAX diffusePhiLuminance (RTXDI 1.0)
extern idCVar r_rtReStirDIRelaxAtrous;		// R2: ReLAX A-trous iterations
extern idCVar r_rtReStirDIRelaxVarianceHist;	// R2: ReLAX spatial variance history threshold (RTXDI 1)
extern idCVar r_rtReStirDIRelaxConfRelax;	// R2: ReLAX confidence-driven relaxation multiplier (Remix 0.7)
extern idCVar r_rtReStirDIRelaxFirefly;		// R2: ReLAX anti-firefly
extern idCVar r_rtReStirDIBoost;			// R3a: K picks + rays on young pixels (0/1 = off)
extern idCVar r_rtReStirDIBoostHistory;	// R3a: history length (frames) below which a pixel is boosted (stays until 2x)
extern idCVar r_nrdSigma;					// R0: keep SIGMA_SHADOW in the NRD instance (never dispatched since H3)
extern idCVar r_rtReStirDICheckerboard;		// ReSTIR DI full-res checkerboard: trace half the pixels (packed left), NRD reconstructs; ~half the rays
extern idCVar r_rtReStirDIResScale;			// ReSTIR DI gather/denoise resolution fraction (0.25..1); upscaled for display. Composes with checkerboard
extern idCVar r_rtReStirDIBilateralUpscale;	// ReSTIR DI: depth+normal-aware upscale of a reduced-res gather (vs plain bilinear); inert at 100%
extern idCVar r_rtgi;						// H6 RT GI: one-bounce ray-traced global illumination (gathers radiance at the hemisphere hit)
extern idCVar r_rtgiRadius;					// RT GI gather ray length (world units)
extern idCVar r_rtgiResScale;				// RT GI gather/denoise resolution fraction (biggest cost lever)
extern idCVar r_rtgiCheckerboard;			// RT GI: full-res horizontal checkerboard gather + NRD reconstruct (sharp edges vs resScale blur)
extern idCVar r_rtgiHistory;				// RT GI denoiser (ReBLUR) max accumulated history frames; lower = less ghosting, more noise
extern idCVar r_rtgiHistoryMs;				// R0: RTGI history in ms (0 = frames)
extern idCVar r_rtgiFastClamp;				// R0: RTGI NRD fastHistoryClampingSigmaScale (2.0 = NRD default)
extern idCVar r_rtgiConfidence;				// RT GI denoiser history-confidence sensitivity (IN_DIFF_CONFIDENCE); kills edge ghosting without flat dimming; 0 = off
extern idCVar r_rtgiLightScale;				// RT GI: brightness of the diffuse lighting at a bounce hit
extern idCVar r_rtgiEmissive;				// RT GI: self-illuminated surface contribution at a bounce hit
extern idCVar r_rtgiEmissiveClamp;			// RT GI: peak clamp on a bounce ray's emissive (pre-NRD variance cut)
extern idCVar r_rtgiBilateralUpscale;		// RT GI: depth+normal-aware upscale of the reduced-res GI (vs plain bilinear)
extern idCVar r_rtgiShadow;					// RT GI (H6d): shadow ray at the bounce hit toward the dominant light
extern idCVar r_rtgiDebug;					// RT GI debug view (H6a: 1 = raw radiance-gather buffer)
extern idCVar r_rtgiIntensity;				// RT GI apply strength (bounced radiance multiplier, H6c)
extern idCVar r_rtgiFogAttenuate;			// RT GI reconcile: fade bounce light out through the view's fog so GI stops washing out distance fog
extern idCVar r_rtgiCache;					// RT irradiance cache (docs/rtx-irradiance-cache.md): the bounce hit reads a hashed world-space cache of indirect light (multi-bounce)
extern idCVar r_rtgiCacheScale;				// RT irradiance cache: weight of the cached term at the bounce hit
extern idCVar r_rtgiCacheTail;				// RT irradiance cache: C6 tail mode (cell = L_out, serves the bounce hit) vs C2 indirect-only; DI-apply only
extern idCVar r_rtgiCacheSlotsLog2;			// RT irradiance cache: log2 slot count (reconfiguring: joins ComputeGfxCfgSignature)
extern idCVar r_rtgiCacheHistory;			// RT irradiance cache: Nmax frames of memory per cell (when the ms knob is 0)
extern idCVar r_rtgiCacheHistoryMs;			// RT irradiance cache: the history in milliseconds through the low-passed frame rate (default 500; 0 = frames)
extern idCVar r_rtgiCacheCellAngle;			// RT irradiance cache: tan(alpha/2), cell width per unit eye distance
extern idCVar r_rtgiCacheDebug;				// RT irradiance cache debug overlay: 1 cell colour, 2 cached value, 3 age, 4 N, 5 activity (reset / fresh), 6 servable (C6 tail)
extern idCVar r_rtgiCacheReset;				// RT irradiance cache: the DI gradient restarts a cell whose direct light changed (diagnostic A/B)
extern idCVar r_rtgiCacheServeError;		// RT irradiance cache: C6 tail serve gate - default 0.33 = by the mean's relative standard error; 0 = by age (N >= SEED_N), the original C6 A/B
extern idCVar r_rtLightGrid;				// RT light grid (docs/rtx-light-grid.md): per-cell importance-sampled light candidates replace the DI/bounce exhaustive RIS (perf)
extern idCVar r_rtLightGridSlots;			// RT light grid: candidates stored per cell (reconfiguring: joins ComputeGfxCfgSignature)
extern idCVar r_rtLightGridJitter;
extern idCVar r_rtLightGridRankLinear;		// RT light grid: rank candidates by the light's own linear falloff, not the inverse-square proxy			// RT light grid: per-pixel half-cell jitter of the cell lookup (cell-border steps -> noise)
extern idCVar r_rtLightGridRes;				// RT light grid: cells per cascade axis (reconfiguring)
extern idCVar r_rtLightGridCascades;		// RT light grid: camera-centred cascades (reconfiguring)
extern idCVar r_rtLightGridCellBase;		// RT light grid: cascade-0 cell size in units (per-frame, no realloc)
extern idCVar r_rtLightGridDebug;			// RT light grid debug overlay: 1 cascade, 2 candidate count, 3 dominant light, 4 importance
extern idCVar r_rtEmissiveTable;			// RT emissive triangle table (docs/rtx-emissive-triangles.md E1): every emissive triangle + flux CDF in a GPU table
extern idCVar r_rtEmissiveTableDebug;		// RT emissive table debug overlay: 1 per-texture tint, 2 flux, 3 emitted colour
extern idCVar r_rtEmissiveScale;
extern idCVar r_rtEmissiveLighting;		// EMISSIVE MODE master switch (D6): emitters light the scene (E2); implies the table, hides flares
extern idCVar r_rtEmissiveIntensity;		// emissive mode: radiance multiplier of every emitter
extern idCVar r_rtEmissiveMaxRows;		// emissive mode: emitter rows (surfaces) a view's scene-light buffer may carry
extern idCVar r_rtEmissivePowerMatch;
extern idCVar r_rtEmissiveFixtureMinArea;	// emissive mode: the EFFECTIVE emitting area (lit area x brightness) below which a surface is an indicator (LED), never a fixture
extern idCVar r_rtEmissiveFixtureFullArea;	// emissive mode: the effective emitting area at which an emitter inherits its light's FULL power (ramps from the min)
extern idCVar r_rtEmissiveMaxRadiance;	// emissive mode: cap on the radiance the power match may give one emitter	// emissive mode: an emitter inherits the POWER of the placed light it replaces (radiance from its radius / colour / area)
extern idCVar r_rtEmissiveBounce;			// emitter rows ride the GI bounce in normal mode too (bounce-only rows)
extern idCVar r_rtEmissiveReach;			// emissive mode: reach as a multiple of the replaced light's authored radius (0 = physical)
extern idCVar r_rtEmissiveReachDefault;	// emissive mode: reach (units) of an emitter no placed light covers
extern idCVar r_rtEmissiveHideFlares;		// emitter mode: hide the flare sprites of fixtures that are table emitters			// RT emissive table: global flux / emitted-colour multiplier (rebuilds the table)
extern idCVar r_rtEmissiveFlareInfer;		// RT emissive table: promote the diffuse patch under a flare on a glow-less fixture to an emitter
extern idCVar r_rtReflTemporal;				// RR6: accumulate RT reflections through the SSR temporal pipeline (+ ray jitter) to kill the firefly grid
extern idCVar r_rtReflJitter;				// RR6: per-frame RT-reflection ray jitter cone scale (needs r_rtReflTemporal)
extern idCVar r_rtReflBlur;					// RR6b: box-blur the RT reflection before compositing (dissolves the static firefly grid)
extern idCVar r_rtReflRealLights;			// RR16: shade an RT reflection hit with the scene's real lights (not a fixed key light)
extern idCVar r_volumetrics;				// volumetric lighting master (docs/volumetric-lighting.md); VK + RT, opt-in
extern idCVar r_volShadows;					// one visibility ray per reaching light per march sample (needs the RT scene)
extern idCVar r_volIntensity;
extern idCVar r_volDensity;
extern idCVar r_volAnisotropy;
extern idCVar r_volLights;					// 1 = projected/spot + parallel lights, 2 = also point lights
extern idCVar r_volSteps;
extern idCVar r_volResScale;
extern idCVar r_volMaxDist;
extern idCVar r_volExtinction;
extern idCVar r_volPointLightScale;
extern idCVar r_volFlashlightScale;
extern idCVar r_volLargeLightRadius;
extern idCVar r_volAoIntensity;
extern idCVar r_volFogPointLightScale;
extern idCVar r_volPointShadows;
extern idCVar r_volBlur;
extern idCVar r_volTemporal;
extern idCVar r_volTemporalFeedback;
extern idCVar r_rtReflLightFrustum;		// RR16c: confine each real light to its projection frustum (spot pyramid / point-light box) at an RT reflection hit
extern idCVar r_rtReflLightScale;			// RR16: overall brightness of the real-light diffuse at an RT reflection hit (compensates the point-light approximation)
extern idCVar r_rtReflEmissive;				// RR16f: brightness of self-lit light-source surfaces (neon/ceiling tiles) in RT reflections
extern idCVar r_rtReflResScale;				// RT reflection trace resolution (fraction of screen; unsnapped, temporally upscaled)
extern idCVar r_rtReflBump;					// RR10: how much the RT reflection follows the normal map (0 = flat mirror, 1 = full bump like SSR)
extern idCVar r_rtReflGlassScale;			// RR5c: taste knob on the RT glass (goggles/visor/cockpit) reflection, on top of the replaced cube's energy normalization
extern idCVar r_rtMovingLights;				// RT shadows for moving point lights: skip the un-cacheable cube re-render, trace instead
int  R_RtGatherPerforatedBoxes( const idBounds &b, idBounds *out, int maxOut );	// tr_main.cpp: the perforated RT casters inside a box; returns the total found
// RT emissive triangle table (tr_main.cpp, docs/rtx-emissive-triangles.md E1): device address of the table built for
// the current map (0 = none / off), with its triangle count and total flux; and the lazily built debug geometry
// (one idDrawVert per table vertex + an index list) for r_rtEmissiveTableDebug.
unsigned long long R_RtEmissiveTableAddress( int *numTris, float *totalFlux, float *meanFlux );
// E2 v3: one emitter ROW per emissive surface run - what RB_RHI_GatherSceneLights turns into a DUDE_LIGHT_EMISSIVE row
// of the scene-light buffer (centroid, bounding radius, flux, flux-weighted mean linear radiance x material scale, total
// area, the inherited reach of the placed light it replaces (0 = none), its triangle range in the table). dark = every
// triangle at zero flux (switched off / gone): never a row.
struct rtEmissiveRow_t { idVec3 centroid; float boundRadius; float flux; float area; idVec3 meanRadiance; float reach; float lightLum; idVec3 lightOrigin; bool hasLight; int firstTri, numTris; bool dark; };
int R_RtEmissiveRows( const rtEmissiveRow_t **rows );		// the rows (0 = no table)
bool R_RtEmissiveDebugGeometry( unsigned int *vb, unsigned int *ib, int *numIndexes );
bool R_RtEmissiveLightDebugGeometry( unsigned int *vb, unsigned int *ib, int *numIndexes );	// overlay mode 5: the placed lights as census-coloured spheres
bool R_RtEmissiveLightDropped( int lightIndex, unsigned int serial );	// E5: a fixtured placed light is dropped while emissive mode is on
// E1d labels: the frontend projects one label per in-sight emissive SURFACE into the 2D VIRTUAL 640x480 space
// (R_RtEmissiveProjectLabels, from R_RtEmissiveUpdate); the console's debug-text pass (Console.cpp
// SCR_DrawEmissiveLabels) draws them every frame while the overlay is on - the FPS counter's 2D path, so screenshots
// include them. Always on top: no occlusion test, only facing + the view's portal-reached areas.
struct rtEmissiveScreenLabel_t { float x, y; char text[80]; };
void R_RtEmissiveProjectLabels( const struct viewDef_s *viewDef );
bool R_RtEmissiveLabelsWanted( void );
int  R_RtEmissiveGetScreenLabels( const rtEmissiveScreenLabel_t **out );
void R_RtEmissiveProbe_f( const idCmdArgs &args );		// console: name the surface under the crosshair + the table's verdict on it
bool R_RtEmissiveHideEntity( int entityIndex );	// emitter mode (r_rtEmissiveHideFlares): this entity is a fixture's flare sprite - skip its draw surfaces
bool R_RtEmissiveIsFlareMaterial( const idMaterial *mat );	// a flare / sprite-deformed additive material (the surface the hide skips)
extern idCVar r_rtPerforatedShadows;		// alpha-tested grate/fence casters in the RT shadow rays
extern idCVar r_rtAllLights;				// RT shadows for EVERY shadow-casting light (mode 4 inline ray), no shadow maps at all
extern idCVar r_rtShadowBlur;				// screen-space blur on top of the hard RT shadows (sun / moving lights), docs/rtx-shadow-blur.md
extern idCVar r_rtShadowBlurStagger;		// far lights refresh their blurred mask every 2nd / 3rd frame (reprojected reuse)
extern idCVar r_rtShadowBlurStaggerNear;	// nearer than this (viewer -> light volume): every frame
extern idCVar r_rtShadowBlurStaggerFar;		// farther than this: every 3rd frame; between: every 2nd
extern idCVar r_rtShadowBlurIntensity;		// blur width multiplier (0 = hard, 1 = default)
extern idCVar r_rtShadowBlurFastTiles;		// tile pass reads the compact RG8 summary instead of the RGBA16F ray target
extern idCVar r_rtShadowBlurCheckerboard;	// r_rtShadowBlur perf: trace the shadow ray at a static checkerboard, reconstruct skipped pixels in the horizontal blur
extern idCVar r_volFastResolve;				// volumetric perf: low-res temporal + single depth-aware upsample at composite (vs upsample-then-full-res-temporal)
extern idCVar r_rtShadowBlurCurve;			// exponent on the gap ratio: how the blur grows with distance from the caster (1 = geometric)
extern idCVar r_rtShadowBlurMinPenumbra;	// skip the per-light blur when its estimated screen penumbra is below this many pixels (distant/small lights)
extern idCVar r_rtTlasDirty;				// skip the per-frame TLAS rebuild when no mover/monster changed this frame
extern idCVar r_rtSunShadowOffset;			// RT sun shadows: ray-origin normal offset (world units)
extern idCVar r_rtMonsterShadows;			// RT sun shadows: include animated characters as per-frame casters

// DUDE: emissive fill lights for interactive GUI screens (enhancement backends only)
extern idCVar r_emissiveSurfaces;		// master toggle (emissive surfaces cast fill light)
extern idCVar r_emissiveLightScale;		// fill-light brightness
extern idCVar r_emissiveLightRadius;	// reach as a multiple of the screen's size
extern idCVar r_emissiveLightMaxReach;	// soft cap: reach rolls off toward this for very large screens
extern idCVar r_emissiveLightFalloff;	// projected-cone fade softness (0=sharp edge, 1=long soft fade)
extern idCVar r_emissiveLightSaturation;	// desaturate tint toward white (0=white, 1=full hue)
extern idCVar r_emissiveLightLimit;		// per-view budget cap on fill lights (0=unlimited)
extern idCVar r_emissiveLightSpread;	// projected cone width (low=beam, high=near-hemisphere)
extern idCVar r_emissiveLightSpecular;	// fill lights cast specular (1) or diffuse-only (0)
extern idCVar r_itemGlow;				// self-lit pickup items cast a faint glow (0 = vanilla .. 1 = faint)

// DUDE: GTAO screen-space ambient occlusion (enhancement backends only; see docs/ssao-gtao.md)
extern idCVar r_ssao;					// master toggle (AO on the ambient term)
extern idCVar r_ssaoIntensity;			// occlusion darkening strength
extern idCVar r_ssaoFloor;				// min ambient visibility (anti-crush floor)
extern idCVar r_ssaoDirectLight;		// AO strength on direct-light diffuse (0..1)
extern idCVar r_ssaoRadius;				// world-space sampling radius
extern idCVar r_ssaoSlices;				// horizon-search directions per pixel (1..8)
extern idCVar r_ssaoSteps;				// samples marched per direction (1..12)
extern idCVar r_ssaoResScale;			// AO buffer resolution fraction (0.5 half .. 1.0 full)
extern idCVar r_ssaoBentNormal;			// shade ambient along the bent normal
extern idCVar r_ssaoBentStrength;		// blend toward the bent normal for the ambient lookup (C.2)
extern idCVar r_ssaoNormalBuffer;		// SSAO reads a bump-mapped normal G-buffer vs depth reconstruct
extern idCVar r_ssaoMergeNormal;		// VK: emit the SSAO normal inside the depth prepass (one pass) vs the standalone normal pass
extern idCVar r_ssaoSpecular;			// also attenuate specular in occluded areas
extern idCVar r_ssaoDebug;				// 1=show AO buffer, 2=show bent normals
extern idCVar r_ssaoTemporal;			// accumulate AO across frames via camera reprojection
extern idCVar r_ssaoTemporalFeedback;	// temporal history weight (0..0.97)
extern idCVar r_ssaoTemporalTrade;		// A/B: temporal-funded 4x6 budget vs the current cvars
extern idCVar r_temporalResetDist;		// view-origin jump (world units) that resets temporal history (cut/teleport); 0 = never
extern idCVar r_motionVectors;			// VK: per-object screen-velocity MRT in the normal prepass (R1/A2, feeds FSR2 / temporal SSAO+SSR)
extern idCVar r_weaponMotionVectors;	// give the view weapon real motion vectors (fix FSR2 garbling its displays); 0 = old zero-velocity weapon
extern idCVar r_mvDebug;				// visualize the velocity buffer: 0=off, 1=direction, 2=magnitude
extern idCVar r_mvDebugScale;			// live gain multiplier for the r_mvDebug overlay (crank up to see slow motion)
extern idCVar r_ssaoDepthMip;			// march the horizon search over a prefiltered linear-depth mip chain (Phase 1)
extern idCVar r_ssaoDepthMipBias;		// depth-mip LOD aggressiveness (log2(stepPix * bias))
extern idCVar r_ssaoDepthMipMaxLod;		// depth-mip coarseness cap (kills far-tap halos)

// DUDE: GPU tessellation of enemy/prop meshes (Vulkan only; docs/tessellation.md)
extern idCVar r_tessellation;			// master toggle: PN-triangle smooth enemy/prop meshes
extern idCVar r_tessEyeOffset;			// DUDE: lift the iris decal clear of a tessellated face
extern idCVar r_tessLevel;				// subdivision level (1 = flat .. capped by device / 32)
extern idCVar r_tessMaxDist;			// distance the subdivision starts rolling back toward flat
extern idCVar r_tessMinEdge;			// min triangle edge length (world units) to tessellate (anti eye-bulge)
extern idCVar r_tessDisplace;			// normal-map displacement strength (world units; +out/-in, 0=off)
extern idCVar r_tessDebug;				// log each material name accepted for tessellation once (diagnostic)

// DUDE: Roadmap B compute "deform once" tessellation (Vulkan only; docs/tessellation.md roadmap)
extern idCVar r_tessDeform;				// deform tessellated meshes once/frame in a compute pass (vs per-pass .tesc/.tese)

// DUDE: GPU MD5 skinning via the compute lane (Vulkan only; docs/gpu-offload-plan.md Phase 2)
extern idCVar r_gpuSkinning;			// skin animated meshes on the GPU (on by default on Vulkan, off = faithful CPU skin)
extern idCVar r_gpuSkinDerive;			// GPU skin TBN: 1 = re-derive from the posed mesh (default), 0 = LBS-rotate the baked bind TBN (option B)
extern idCVar r_gpuSkinWeld;			// weld GPU-derived seam normals under tessellation (0 = off/faithful default, 1 = weld/no-tear but hard edges round)
extern idCVar r_gpuSkinProfile;			// dev: size the Milestone-C prize (skinned-surface derive + ambient upload)
void R_GpuSkinProfileAddDerive( double ms );	// accumulate one skinned-surface tangent-derive time (impl tr_light.cpp)

// Shared stencil-build routing (Interaction.cpp CreateInteraction + the view-flag loop): true when the
// light will be shadow-MAPPED, so its CPU stencil volume is never drawn and the build can be skipped.
bool R_ShadowMapSkipStencilBuild( const idRenderLightLocal *lightDef, const idMaterial *lightShader );

// DUDE: baked ambient-occlusion (occlusion) maps (enhancement backends only; docs/occlusion-maps.md)
extern idCVar r_occlusionMaps;			// master toggle: use per-material baked AO maps
extern idCVar r_occlusionMapScale;		// AO-map strength on the ambient term (0..1)
extern idCVar r_occlusionMapDirect;		// AO-map strength on direct-light diffuse (0..1 of scale)
extern idCVar r_occlusionMapsAutoBake;	// DEV: lazily bake missing model AO maps on first sight

extern idCVar r_dof;					// DUDE: weapon-reload depth-of-field master toggle (RHI)
extern idCVar r_dofBlurRadius;			// DUDE: DoF max world blur radius (pixels)
extern idCVar r_dofFocusStart;			// DUDE: DoF scene raw-depth where the blur begins
extern idCVar r_dofFocusEnd;			// DUDE: DoF scene raw-depth where the blur maxes
extern idCVar r_weaponReloadFocus;		// DUDE: DoF strength bridge (game-written 0..1 envelope)
extern idCVar r_parallax;				// DUDE: master toggle for parallax occlusion mapping (Vulkan)
extern idCVar r_parallaxScale;			// DUDE: global multiplier on per-material POM height scale
extern idCVar r_parallaxMinSteps;		// DUDE: POM march steps head-on
extern idCVar r_parallaxMaxSteps;		// DUDE: POM march steps at grazing angle (loop cap 32)
extern idCVar r_parallaxShadow;			// DUDE: POM self-shadow strength (0 = off)

// clears the per-material generated-AO lookup cache (RhiWorld.cpp) so freshly baked maps
// are picked up without a vid_restart; called by the bakeAO* commands
void R_ResetOcclusionMapCache( void );

extern idCVar r_decalBloodSheen;			// sheen on blood decals (0=matte, 1=as authored)
extern idCVar r_decalMetalMask;			// overlay decal coverage strips the PBR metalness under it (0=off)
extern idCVar r_decalRoughen;			// overlay decal coverage roughens the PBR surface under it (0=off)
extern idCVar r_decalWetBlood;			// wet blood film: blood decals glint as wet surfaces of their own (strength, 0=off)
extern idCVar r_decalWetBloodRelief;	// wet blood film: slope multiplier on the blood's generated relief
extern idCVar r_decalWetBloodThreshold;	// wet blood film: minimum decal coverage that shows any film
extern idCVar r_decalWetBloodRough;		// wet blood film: roughness of the film
bool R_DecalIsBlood( const idMaterial *material );	// "blood" content flag or a blood-like name
extern idCVar r_decalMatte;				// translucent non-blood decals lose specular + cube reflection
// sheen multiplier for a material's specular interaction and texgen-reflect stage (1 = not a decal)
float R_DecalSheenScale( const idMaterial *material );

extern idCVar r_gl3ReflectionScale;		// GL3: cube-map glass reflection brightness (1=untouched, 0.7=-30%)

extern idCVar r_renderer;				// arb2, etc

extern idCVar r_checkBounds;			// compare all surface bounds with precalculated ones

extern idCVar r_useLightPortalFlow;		// 1 = do a more precise area reference determination
extern idCVar r_useShadowSurfaceScissor;// 1 = scissor shadows by the scissor rect of the interaction surfaces
extern idCVar r_useConstantMaterials;	// 1 = use pre-calculated material registers if possible
extern idCVar r_useInteractionTable;	// create a full entityDefs * lightDefs table to make finding interactions faster
extern idCVar r_useNodeCommonChildren;	// stop pushing reference bounds early when possible
extern idCVar r_useSilRemap;			// 1 = consider verts with the same XYZ, but different ST the same for shadows
extern idCVar r_useCulling;				// 0 = none, 1 = sphere, 2 = sphere + box
extern idCVar r_useLightCulling;		// 0 = none, 1 = box, 2 = exact clip of polyhedron faces
extern idCVar r_useLightScissors;		// 1 = use custom scissor rectangle for each light
extern idCVar r_useClippedLightScissors;// 0 = full screen when near clipped, 1 = exact when near clipped, 2 = exact always
extern idCVar r_useEntityCulling;		// 0 = none, 1 = box
extern idCVar r_useEntityScissors;		// 1 = use custom scissor rectangle for each entity
extern idCVar r_useInteractionCulling;	// 1 = cull interactions
extern idCVar r_useInteractionScissors;	// 1 = use a custom scissor rectangle for each interaction
extern idCVar r_useFrustumFarDistance;	// if != 0 force the view frustum far distance to this distance
extern idCVar r_useShadowCulling;		// try to cull shadows from partially visible lights
extern idCVar r_usePreciseTriangleInteractions;	// 1 = do winding clipping to determine if each ambiguous tri should be lit
extern idCVar r_useTurboShadow;			// 1 = use the infinite projection with W technique for dynamic shadows
extern idCVar r_useExternalShadows;		// 1 = skip drawing caps when outside the light volume
extern idCVar r_useOptimizedShadows;	// 1 = use the dmap generated static shadow volumes
extern idCVar r_useShadowVertexProgram;	// 1 = do the shadow projection in the vertex program on capable cards
extern idCVar r_useShadowProjectedCull;	// 1 = discard triangles outside light volume before shadowing
extern idCVar r_useDeferredTangents;	// 1 = don't always calc tangents after deform
extern idCVar r_useCachedDynamicModels;	// 1 = cache snapshots of dynamic models
extern idCVar r_useTwoSidedStencil;		// 1 = do stencil shadows in one pass with different ops on each side
extern idCVar r_useInfiniteFarZ;		// 1 = use the no-far-clip-plane trick
extern idCVar r_useScissor;				// 1 = scissor clip as portals and lights are processed
extern idCVar r_usePortals;				// 1 = use portals to perform area culling, otherwise draw everything
extern idCVar r_useStateCaching;		// avoid redundant state changes in GL_*() calls
extern idCVar r_useCombinerDisplayLists;// if 1, put all nvidia register combiner programming in display lists
extern idCVar r_useVertexBuffers;		// if 0, don't use ARB_vertex_buffer_object for vertexes
extern idCVar r_useIndexBuffers;		// if 0, don't use ARB_vertex_buffer_object for indexes
extern idCVar r_useEntityCallbacks;		// if 0, issue the callback immediately at update time, rather than defering
extern idCVar r_lightAllBackFaces;		// light all the back faces, even when they would be shadowed
extern idCVar r_useDepthBoundsTest;     // use depth bounds test to reduce shadow fill

extern idCVar r_supportNoSpecular;		// support nospecular parm of lights

extern idCVar r_skipPostProcess;		// skip all post-process renderings
extern idCVar r_skipSuppress;			// ignore the per-view suppressions
extern idCVar r_skipInteractions;		// skip all light/surface interaction drawing
extern idCVar r_skipFrontEnd;			// bypasses all front end work, but 2D gui rendering still draws
extern idCVar r_skipBackEnd;			// don't draw anything
extern idCVar r_skipCopyTexture;		// do all rendering, but don't actually copyTexSubImage2D
extern idCVar r_skipRender;				// skip 3D rendering, but pass 2D
extern idCVar r_skipRenderContext;		// NULL the rendering context during backend 3D rendering
extern idCVar r_skipTranslucent;		// skip the translucent interaction rendering
extern idCVar r_skipAmbient;			// bypasses all non-interaction drawing
extern idCVar r_skipNewAmbient;			// bypasses all vertex/fragment program ambients
extern idCVar r_skipBlendLights;		// skip all blend lights
extern idCVar r_skipFogLights;			// skip all fog lights
extern idCVar r_skipSubviews;			// 1 = don't render any mirrors / cameras / etc
extern idCVar r_skipGuiShaders;			// 1 = don't render any gui elements on surfaces
extern idCVar r_skipParticles;			// 1 = don't render any particles
extern idCVar r_skipUpdates;			// 1 = don't accept any entity or light updates, making everything static
extern idCVar r_skipDeforms;			// leave all deform materials in their original state
extern idCVar r_skipDynamicTextures;	// don't dynamically create textures
extern idCVar r_skipLightScale;			// don't do any post-interaction light scaling, makes things dim on low-dynamic range cards
extern idCVar r_skipBump;				// uses a flat surface instead of the bump map
extern idCVar r_skipSpecular;			// use black for specular
extern idCVar r_skipDiffuse;			// use black for diffuse
extern idCVar r_whiteWorld;				// DUDE: white diffuse everywhere, lighting-only view
extern idCVar r_skipOverlays;			// skip overlay surfaces
extern idCVar r_skipROQ;

extern idCVar r_ignoreGLErrors;

extern idCVar r_forceLoadImages;		// draw all images to screen after registration
extern idCVar r_demonstrateBug;			// used during development to show IHV's their problems
extern idCVar r_screenFraction;			// for testing fill rate, the resolution of the entire screen can be changed

extern idCVar r_showUnsmoothedTangents;	// highlight geometry rendered with unsmoothed tangents
extern idCVar r_showSilhouette;			// highlight edges that are casting shadow planes
extern idCVar r_showVertexColor;		// draws all triangles with the solid vertex color
extern idCVar r_showUpdates;			// report entity and light updates and ref counts
extern idCVar r_showDemo;				// report reads and writes to the demo file
extern idCVar r_showDynamic;			// report stats on dynamic surface generation
extern idCVar r_showLightScale;			// report the scale factor applied to drawing for overbrights
extern idCVar r_showIntensity;			// draw the screen colors based on intensity, red = 0, green = 128, blue = 255
extern idCVar r_showDefs;				// report the number of modeDefs and lightDefs in view
extern idCVar r_showTrace;				// show the intersection of an eye trace with the world
extern idCVar r_showSmp;				// show which end (front or back) is blocking
extern idCVar r_showDepth;				// display the contents of the depth buffer and the depth range
extern idCVar r_showImages;				// draw all images to screen instead of rendering
extern idCVar r_showTris;				// enables wireframe rendering of the world
extern idCVar r_showSurfaceInfo;		// show surface material name under crosshair
extern idCVar r_showNormals;			// draws wireframe normals
extern idCVar r_showEdges;				// draw the sil edges
extern idCVar r_showViewEntitys;		// displays the bounding boxes of all view models and optionally the index
extern idCVar r_showTexturePolarity;	// shade triangles by texture area polarity
extern idCVar r_showTangentSpace;		// shade triangles by tangent space
extern idCVar r_showDominantTri;		// draw lines from vertexes to center of dominant triangles
extern idCVar r_showTextureVectors;		// draw each triangles texture (tangent) vectors
extern idCVar r_showLights;				// 1 = print light info, 2 = also draw volumes
extern idCVar r_showLightCount;			// colors surfaces based on light count
extern idCVar r_showShadows;			// visualize the stencil shadow volumes
extern idCVar r_showShadowCount;		// colors screen based on shadow volume depth complexity
extern idCVar r_showLightScissors;		// show light scissor rectangles
extern idCVar r_showEntityScissors;		// show entity scissor rectangles
extern idCVar r_showInteractionFrustums;// show a frustum for each interaction
extern idCVar r_showInteractionScissors;// show screen rectangle which contains the interaction frustum
extern idCVar r_showMemory;				// print frame memory utilization
extern idCVar r_showCull;				// report sphere and box culling stats
extern idCVar r_showInteractions;		// report interaction generation activity
extern idCVar r_showSurfaces;			// report surface/light/shadow counts
extern idCVar r_showPrimitives;			// report vertex/index/draw counts
extern idCVar r_showPortals;			// draw portal outlines in color based on passed / not passed
extern idCVar r_showAlloc;				// report alloc/free counts
extern idCVar r_showSkel;				// draw the skeleton when model animates
extern idCVar r_showOverDraw;			// show overdraw
extern idCVar r_jointNameScale;			// size of joint names when r_showskel is set to 1
extern idCVar r_jointNameOffset;		// offset of joint names when r_showskel is set to 1

extern idCVar r_testGamma;				// draw a grid pattern to test gamma levels
extern idCVar r_testStepGamma;			// draw a grid pattern to test gamma levels
extern idCVar r_testGammaBias;			// draw a grid pattern to test gamma levels

extern idCVar r_testARBProgram;			// experiment with vertex/fragment programs

extern idCVar r_singleLight;			// suppress all but one light
extern idCVar r_singleEntity;			// suppress all but one entity
extern idCVar r_singleArea;				// only draw the portal area the view is actually in
extern idCVar r_singleSurface;			// suppress all but one surface on each entity
extern idCVar r_shadowPolygonOffset;	// bias value added to depth test for stencil shadow drawing
extern idCVar r_shadowPolygonFactor;	// scale value for stencil shadow drawing

extern idCVar r_jitter;					// randomly subpixel jitter the projection matrix
extern idCVar r_lightSourceRadius;		// for soft-shadow sampling
extern idCVar r_lockSurfaces;
extern idCVar r_orderIndexes;			// perform index reorganization to optimize vertex use

extern idCVar r_debugLineDepthTest;		// perform depth test on debug lines
extern idCVar r_debugLineWidth;			// width of debug lines
extern idCVar r_debugArrowStep;			// step size of arrow cone line rotation in degrees
extern idCVar r_debugPolygonFilled;

extern idCVar r_materialOverride;		// override all materials

extern idCVar r_debugRenderToTexture;

extern idCVar r_glDebugContext; // DG: use debug context to call logging callbacks on GL errors
extern idCVar r_enableDepthCapture; // DG: disable capturing depth buffer, used for soft particles
extern idCVar r_useSoftParticles;
extern idCVar r_smokeDarkBlend;			// DUDE: fade alpha-blended smoke into scene darkness (opengl3/Vulkan)
extern idCVar r_smokeDarkBlendFloor;	// smoke opacity over a black background (0..1)
extern idCVar r_smokeDarkBlendKnee;		// background luminance at which smoke reaches full opacity

/*
====================================================================

GL wrapper/helper functions

====================================================================
*/

void	GL_SelectTexture( int unit );
void	GL_CheckErrors( void );
void	GL_ClearStateDelta( void );
void	GL_State( int stateVector );
void	GL_TexEnv( int env );
void	GL_Cull( int cullType );

const int GLS_SRCBLEND_ZERO						= 0x00000001;
const int GLS_SRCBLEND_ONE						= 0x0;
const int GLS_SRCBLEND_DST_COLOR				= 0x00000003;
const int GLS_SRCBLEND_ONE_MINUS_DST_COLOR		= 0x00000004;
const int GLS_SRCBLEND_SRC_ALPHA				= 0x00000005;
const int GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA		= 0x00000006;
const int GLS_SRCBLEND_DST_ALPHA				= 0x00000007;
const int GLS_SRCBLEND_ONE_MINUS_DST_ALPHA		= 0x00000008;
const int GLS_SRCBLEND_ALPHA_SATURATE			= 0x00000009;
const int GLS_SRCBLEND_BITS						= 0x0000000f;

const int GLS_DSTBLEND_ZERO						= 0x0;
const int GLS_DSTBLEND_ONE						= 0x00000020;
const int GLS_DSTBLEND_SRC_COLOR				= 0x00000030;
const int GLS_DSTBLEND_ONE_MINUS_SRC_COLOR		= 0x00000040;
const int GLS_DSTBLEND_SRC_ALPHA				= 0x00000050;
const int GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA		= 0x00000060;
const int GLS_DSTBLEND_DST_ALPHA				= 0x00000070;
const int GLS_DSTBLEND_ONE_MINUS_DST_ALPHA		= 0x00000080;
const int GLS_DSTBLEND_BITS						= 0x000000f0;


// these masks are the inverse, meaning when set the glColorMask value will be 0,
// preventing that channel from being written
const int GLS_DEPTHMASK							= 0x00000100;
const int GLS_REDMASK							= 0x00000200;
const int GLS_GREENMASK							= 0x00000400;
const int GLS_BLUEMASK							= 0x00000800;
const int GLS_ALPHAMASK							= 0x00001000;
const int GLS_COLORMASK							= (GLS_REDMASK|GLS_GREENMASK|GLS_BLUEMASK);

const int GLS_POLYMODE_LINE						= 0x00002000;

const int GLS_DEPTHFUNC_ALWAYS					= 0x00010000;
const int GLS_DEPTHFUNC_EQUAL					= 0x00020000;
const int GLS_DEPTHFUNC_LESS					= 0x0;

const int GLS_ATEST_EQ_255						= 0x10000000;
const int GLS_ATEST_LT_128						= 0x20000000;
const int GLS_ATEST_GE_128						= 0x40000000;
const int GLS_ATEST_BITS						= 0x70000000;

const int GLS_DEFAULT							= GLS_DEPTHFUNC_ALWAYS;

void R_Init( void );
void R_InitOpenGL( void );

void R_DoneFreeType( void );

void R_SetColorMappings( void );

void R_ScreenShot_f( const idCmdArgs &args );
void R_StencilShot( void );

bool R_CheckExtension( const char *name );


/*
====================================================================

IMPLEMENTATION SPECIFIC FUNCTIONS

====================================================================
*/

typedef struct {
	int			width;
	int			height;
	bool		fullScreen;
	bool		fullScreenDesktop;
	bool		stereo;
	int			displayHz; // TODO: SDL3 uses float
	int			multiSamples;
	bool		coreProfile;	// DUDE: request a GL 3.3 core context (r_graphicsAPI opengl3)
	bool		vulkan;			// DUDE Phase 4: SDL_WINDOW_VULKAN window, no GL context/attributes
} glimpParms_t;

// DUDE RHI executor (renderer/rhi/RhiBackend.cpp) — idTech4 command list →
// rhi::GetRHI() calls; shared by the opengl3 and (Phase 4) vulkan backends
void		RB_RHI_ExecuteBackEndCommands( const emptyCommand_t *cmds );

// DUDE: true when the active backend supports the non-vanilla "Enhancements"
// (film grain, chromatic aberration, soft particles, shadow mapping, SSAO...).
// True for the RHI backends (opengl3, later vulkan). The legacy ARB2 path must
// stay faithful to vanilla Doom 3, so these effects are suppressed whenever
// this returns false.
bool		R_BackendSupportsEnhancements();
bool		R_SupportsRayTracing();		// active backend is Vulkan with ray-query hardware (RT-shadow gate)
bool		R_HdrOverbrightEnabled();	// HDR tonemap curve on AND r_hdrOverbright > 1 (impl RenderSystem_init.cpp)

bool		GLimp_Init( glimpParms_t parms );
#ifdef DHEWM3_VULKAN
// Phase 4 (docs/vulkan-backend.md): prove SDL can create a Vulkan-capable
// window and list instance extensions, without touching the GL path. Logs
// findings; returns false if the system can't do SDL+Vulkan. Used as the
// pre-gate before selecting the Vulkan backend, so unsupported systems fall
// back to GL instead of wedging on the archived cvar.
bool		GLimp_VulkanProbe( void );
// the live SDL window (as void* to keep SDL types out of this header) — the
// Vulkan backend creates its VkSurfaceKHR from it
void *		GLimp_GetSDLWindow( void );
#endif
// If the desired mode can't be set satisfactorily, false will be returned.
// The renderer will then reset the glimpParms to "safe mode" of 640x480
// fullscreen and try again.  If that also fails, the error will be fatal.

bool		GLimp_SetScreenParms( glimpParms_t parms );
// will set up gl up with the new parms

void		GLimp_Shutdown( void );
// Destroys the rendering context, closes the window, resets the resolution,
// and resets the gamma ramps.

void		GLimp_SwapBuffers( void );
// Calls the system specific swapbuffers routine, and may also perform
// other system specific cvar checks that happen every frame.
// This will not be called if 'r_drawBuffer GL_FRONT'

void		GLimp_SetGamma( unsigned short red[256],
							unsigned short green[256],
							unsigned short blue[256] );
// Sets the hardware gamma ramps for gamma and brightness adjustment.
// These are now taken as 16 bit values, so we can take full advantage
// of dacs with >8 bits of precision

void		GLimp_ResetGamma();
// resets the gamma to what it was at startup

// Returns false if the system only has a single processor

void		GLimp_ActivateContext( void );
void		GLimp_DeactivateContext( void );
// These are used for managing SMP handoffs of the OpenGL context
// between threads, and as a performance tunining aid.  Setting
// 'r_skipRenderContext 1' will call GLimp_DeactivateContext() before
// the 3D rendering code, and GLimp_ActivateContext() afterwards.  On
// most OpenGL implementations, this will result in all OpenGL calls
// being immediate returns, which lets us guage how much time is
// being spent inside OpenGL.

const int GRAB_GRABMOUSE	= (1 << 0);
const int GRAB_HIDECURSOR	= (1 << 1);
const int GRAB_RELATIVEMOUSE = (1 << 2);
const int GRAB_ENABLETEXTINPUT = (1 << 3); // to explicitly enable/disable textinput in SDL2/3

void GLimp_GrabInput(int flags);

bool GLimp_SetSwapInterval( int swapInterval );
int GLimp_GetSwapInterval();
bool GLimp_SetWindowResizable( bool enableResizable );
void GLimp_UpdateWindowSize();

glimpParms_t GLimp_GetCurState();
float GLimp_GetDisplayRefresh();

/*
====================================================================

MAIN

====================================================================
*/

void R_RenderView( viewDef_t *parms );

// performs radius cull first, then corner cull
bool R_CullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes );
bool R_RadiusCullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes );
bool R_CornerCullLocalBox( const idBounds &bounds, const float modelMatrix[16], int numPlanes, const idPlane *planes );

// DUDE GPU-offload Phase 3.2 (live GPU frustum cull validation, cvar r_gpuCullLive):
// the front-end records every ambient-cull candidate (real local bounds + modelMatrix +
// numIndexes + the CPU R_CullLocalBox decision) so R_GpuCullLive() can prove the GPU cull
// reproduces R_CullLocalBox on the LIVE per-frame surface set (dynamic/animated bounds,
// parented transforms, live frustum) — the real-data half of wiring the cull in. All are
// cheap no-ops when the cvar is off (R_GpuCullLiveActive() returns false, so no recording).
void R_GpuCull_ResetLive( void );					// per-view: arm at most once/sec, reset the collector
bool R_GpuCullLiveActive( void );					// true only for the armed view this second
void R_GpuCull_RecordCandidate( const idBounds &bounds, const float modelMatrix[16], int numIndexes, bool culled );

void R_AxisToModelMatrix( const idMat3 &axis, const idVec3 &origin, float modelMatrix[16] );

// note that many of these assume a normalized matrix, and will not work with scaled axis
void R_GlobalPointToLocal( const float modelMatrix[16], const idVec3 &in, idVec3 &out );
void R_GlobalVectorToLocal( const float modelMatrix[16], const idVec3 &in, idVec3 &out );
void R_GlobalPlaneToLocal( const float modelMatrix[16], const idPlane &in, idPlane &out );
void R_PointTimesMatrix( const float modelMatrix[16], const idVec4 &in, idVec4 &out );
void R_LocalPointToGlobal( const float modelMatrix[16], const idVec3 &in, idVec3 &out );
void R_LocalVectorToGlobal( const float modelMatrix[16], const idVec3 &in, idVec3 &out );
void R_LocalPlaneToGlobal( const float modelMatrix[16], const idPlane &in, idPlane &out );
void R_TransformEyeZToWin( float src_z, const float *projectionMatrix, float &dst_z );

void R_GlobalToNormalizedDeviceCoordinates( const idVec3 &global, idVec3 &ndc );

void R_TransformModelToClip( const idVec3 &src, const float *modelMatrix, const float *projectionMatrix, idPlane &eye, idPlane &dst );

void R_TransformClipToDevice( const idPlane &clip, const viewDef_t *view, idVec3 &normalized );

void R_TransposeGLMatrix( const float in[16], float out[16] );

void R_SetViewMatrix( viewDef_t *viewDef );

void myGlMultMatrix( const float *a, const float *b, float *out );

/*
============================================================

LIGHT

============================================================
*/

void R_ListRenderLightDefs_f( const idCmdArgs &args );
void R_ListRenderEntityDefs_f( const idCmdArgs &args );

bool R_IssueEntityDefCallback( idRenderEntityLocal *def );
idRenderModel *R_EntityDefDynamicModel( idRenderEntityLocal *def );

viewEntity_t *R_SetEntityDefViewEntity( idRenderEntityLocal *def );
viewLight_t *R_SetLightDefViewLight( idRenderLightLocal *def );

void R_AddDrawSurf( const srfTriangles_t *tri, const viewEntity_t *space, const renderEntity_t *renderEntity,
					const idMaterial *shader, const idScreenRect &scissor, const float soft_particle_radius = -1.0f ); // soft particles in #3878

void R_LinkLightSurf( const drawSurf_t **link, const srfTriangles_t *tri, const viewEntity_t *space,
				   const idRenderLightLocal *light, const idMaterial *shader, const idScreenRect &scissor, bool viewInsideShadow );

bool R_CreateAmbientCache( srfTriangles_t *tri, bool needsLighting );
bool R_CreateLightingCache( const idRenderEntityLocal *ent, const idRenderLightLocal *light, srfTriangles_t *tri );
void R_CreatePrivateShadowCache( srfTriangles_t *tri );
void R_CreateVertexProgramShadowCache( srfTriangles_t *tri );

/*
============================================================

LIGHTRUN

============================================================
*/

void R_RegenerateWorld_f( const idCmdArgs &args );

void R_ModulateLights_f( const idCmdArgs &args );

void R_SetLightProject( idPlane lightProject[4], const idVec3 origin, const idVec3 targetPoint,
	   const idVec3 rightVector, const idVec3 upVector, const idVec3 start, const idVec3 stop );

void R_AddLightSurfaces( void );
void R_AddModelSurfaces( void );
void R_RemoveUnecessaryViewLights( void );

void R_FreeDerivedData( void );
void R_ReCreateWorldReferences( void );

// RHI/GL3 shadow-map cube cache reset (defined in rhi/RhiWorld.cpp); called on
// world teardown so cached cubes can't outlive the light defs they were keyed to.
void RB_RHI_FreeShadowCubeCache( void );

// RHI/GL3 strong backend teardown (defined in rhi/RhiBackend.cpp); called from the
// renderer while the GL context is still current (vid_restart before GLimp_Shutdown,
// and full renderer shutdown) so switching backends leaves no residue — deletes all
// GPU objects and forgets every cached render-target handle. No-op on a legacy session.
void RB_RHI_Shutdown( void );

// HDR (r_hdr) capture signals for the shared idImage::CopyFramebuffer (defined in
// rhi/RhiBackend.cpp). HdrCaptureActive: the float scene FBO is the bound read source
// right now (view pass) → read GL_COLOR_ATTACHMENT0 vs GL_BACK. HdrFrameActive: this is
// an HDR frame for its whole duration → capture _currentRender as RGBA16F (so the format
// doesn't thrash-realloc between the view and gamma passes). Both false on the legacy
// backend / when r_hdr is off.
bool RB_RHI_HdrCaptureActive( void );

extern idCVar r_fsrScale;				// FSR2 render scale: the world view's size as a fraction of the display (1.0 = Native-AA)

// DUDE FSR2 sub-native upscaling (tr_main.cpp): the latched per-frame render scale, the world
// render size, and the backend's "primary fullscreen view" reference size
float	R_FsrRenderScale( void );
void	R_FsrWorldSize( int &w, int &h );
bool RB_RHI_HdrFrameActive( void );

void R_CreateEntityRefs( idRenderEntityLocal *def );
void R_CreateLightRefs( idRenderLightLocal *light );

void R_DeriveLightData( idRenderLightLocal *light );
void R_FreeLightDefDerivedData( idRenderLightLocal *light );
void R_CheckForEntityDefsUsingModel( idRenderModel *model );

void R_ClearEntityDefDynamicModel( idRenderEntityLocal *def );
void R_FreeEntityDefDerivedData( idRenderEntityLocal *def, bool keepDecals, bool keepCachedDynamicModel );
void R_FreeEntityDefCachedDynamicModel( idRenderEntityLocal *def );
void R_FreeEntityDefDecals( idRenderEntityLocal *def );
void R_FreeEntityDefOverlay( idRenderEntityLocal *def );
void R_FreeEntityDefFadedDecals( idRenderEntityLocal *def, int time );

void R_CreateLightDefFogPortals( idRenderLightLocal *ldef );

/*
============================================================

POLYTOPE

============================================================
*/

srfTriangles_t *R_PolytopeSurface( int numPlanes, const idPlane *planes, idWinding **windings );

/*
============================================================

RENDER

============================================================
*/

void RB_EnterWeaponDepthHack();
void RB_EnterModelDepthHack( float depth );
void RB_LeaveDepthHack();
void RB_DrawElementsImmediate( const srfTriangles_t *tri );
void RB_RenderTriangleSurface( const srfTriangles_t *tri );
void RB_T_RenderTriangleSurface( const drawSurf_t *surf );
void RB_RenderDrawSurfListWithFunction( drawSurf_t **drawSurfs, int numDrawSurfs,
					  void (*triFunc_)( const drawSurf_t *) );
void RB_RenderDrawSurfChainWithFunction( const drawSurf_t *drawSurfs,
										void (*triFunc_)( const drawSurf_t *) );
void RB_DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs );
void RB_LoadShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture );
void RB_GetShaderTextureMatrix( const float *shaderRegisters, const textureStage_t *texture, float matrix[16] );
void RB_CreateSingleDrawInteractions( const drawSurf_t *surf, void (*DrawInteraction)(const drawInteraction_t *) );
// DUDE wet blood film: a synthesized specular-only interaction for a blood decal (RHI backends)
void RB_CreateFilmDrawInteractions( const drawSurf_t *surf, const shaderStage_t *coverStage,
									idImage *reliefImage, const shaderStage_t *reliefStage, float strength,
									void (*DrawInteraction)(const drawInteraction_t *) );

const shaderStage_t *RB_SetLightTexture( const idRenderLightLocal *light );

void RB_DrawView( const void *data );

void RB_DetermineLightScale( void );
void RB_STD_LightScale( void );
void RB_BeginDrawingView (void);

/*
============================================================

DRAW_STANDARD

============================================================
*/

void RB_DrawElementsWithCounters( const srfTriangles_t *tri );
void RB_DrawShadowElementsWithCounters( const srfTriangles_t *tri, int numIndexes );
void RB_STD_FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs );
void RB_BindVariableStageImage( const textureStage_t *texture, const float *shaderRegisters );
void RB_BindStageTexture( const float *shaderRegisters, const textureStage_t *texture, const drawSurf_t *surf );
void RB_FinishStageTexture( const textureStage_t *texture, const drawSurf_t *surf );
void RB_StencilShadowPass( const drawSurf_t *drawSurfs );
void RB_STD_DrawView( void );
void RB_STD_FogAllLights( void );
void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float textureMatrix[16] );

/*
============================================================

DRAW_*

============================================================
*/

void	R_ARB2_Init( void );
void	RB_ARB2_DrawInteractions( void );
void	R_ReloadARBPrograms_f( const idCmdArgs &args );
int		R_FindARBProgram( GLenum target, const char *program );
const char *R_ARBProgramName( int ident, unsigned int target );	// DUDE: ident -> glprogs file name (Material IR transpile)

typedef enum {
	PROG_INVALID,
	VPROG_INTERACTION,
	VPROG_ENVIRONMENT,
	VPROG_BUMPY_ENVIRONMENT,
	VPROG_STENCIL_SHADOW,
	VPROG_TEST,
	FPROG_INTERACTION,
	FPROG_ENVIRONMENT,
	FPROG_BUMPY_ENVIRONMENT,
	FPROG_TEST,
	VPROG_AMBIENT,
	FPROG_AMBIENT,
	VPROG_GLASSWARP,
	FPROG_GLASSWARP,
	// SteveL #3878: soft particles
	VPROG_SOFT_PARTICLE,
	FPROG_SOFT_PARTICLE,
	//
	PROG_USER
} program_t;

/*

  All vertex programs use the same constant register layout:

c[4]	localLightOrigin
c[5]	localViewOrigin
c[6]	lightProjection S
c[7]	lightProjection T
c[8]	lightProjection Q
c[9]	lightFalloff	S
c[10]	bumpMatrix S
c[11]	bumpMatrix T
c[12]	diffuseMatrix S
c[13]	diffuseMatrix T
c[14]	specularMatrix S
c[15]	specularMatrix T


c[20]	light falloff tq constant

// texture 0 was cube map
// texture 1 will be the per-surface bump map
// texture 2 will be the light falloff texture
// texture 3 will be the light projection texture
// texture 4 is the per-surface diffuse map
// texture 5 is the per-surface specular map
// texture 6 is the specular half angle cube map

*/

typedef enum {
	PP_LIGHT_ORIGIN = 4,
	PP_VIEW_ORIGIN,
	PP_LIGHT_PROJECT_S,
	PP_LIGHT_PROJECT_T,
	PP_LIGHT_PROJECT_Q,
	PP_LIGHT_FALLOFF_S,
	PP_BUMP_MATRIX_S,
	PP_BUMP_MATRIX_T,
	PP_DIFFUSE_MATRIX_S,
	PP_DIFFUSE_MATRIX_T,
	PP_SPECULAR_MATRIX_S,
	PP_SPECULAR_MATRIX_T,
	PP_COLOR_MODULATE,
	PP_COLOR_ADD, // 17

	PP_LIGHT_FALLOFF_TQ = 20,	// only for NV programs - DG: unused
	PP_GAMMA_BRIGHTNESS = 21, // DG: for gamma in shader: { r_brightness, r_brightness, r_brightness, 1/r_gamma }
	// DG: for soft particles from TDM: reciprocal of _currentDepth size.
	//     Lets us convert a screen position to a texcoord in _currentDepth
	PP_CURDEPTH_RECIPR  = 22,
	// DG: for soft particles from TDM: particle radius, given as { radius, 1/(fadeRange), 1/radius }
	//     fadeRange is the particle diameter for alpha blends (like smoke), but the particle radius for additive
	//     blends (light glares), because additive effects work differently. Fog is half as apparent when a wall
	//     is in the middle of it. Light glares lose no visibility when they have something to reflect off.
	PP_PARTICLE_RADIUS  = 23,
	// DG: for soft particles from TDM: color channel mask.
	//     Particles with additive blend need their RGB channels modifying to blend them out
	//     Particles with an alpha blend need their alpha channel modifying.
	PP_PARTICLE_COLCHAN_MASK = 24,
} programParameter_t;


/*
============================================================

TR_STENCILSHADOWS

"facing" should have one more element than tri->numIndexes / 3, which should be set to 1

============================================================
*/

void R_MakeShadowFrustums( idRenderLightLocal *def );

typedef enum {
	SG_DYNAMIC,		// use infinite projections
	SG_STATIC,		// clip to bounds
	SG_OFFLINE		// perform very time consuming optimizations
} shadowGen_t;

srfTriangles_t *R_CreateShadowVolume( const idRenderEntityLocal *ent,
									 const srfTriangles_t *tri, const idRenderLightLocal *light,
									 shadowGen_t optimize, srfCullInfo_t &cullInfo );

/*
============================================================

TR_TURBOSHADOW

Fast, non-clipped overshoot shadow volumes

"facing" should have one more element than tri->numIndexes / 3, which should be set to 1
calling this function may modify "facing" based on culling

============================================================
*/

srfTriangles_t *R_CreateVertexProgramTurboShadowVolume( const idRenderEntityLocal *ent,
									 const srfTriangles_t *tri, const idRenderLightLocal *light,
									 srfCullInfo_t &cullInfo );

srfTriangles_t *R_CreateTurboShadowVolume( const idRenderEntityLocal *ent,
									 const srfTriangles_t *tri, const idRenderLightLocal *light,
									 srfCullInfo_t &cullInfo );

/*
============================================================

util/shadowopt3

dmap time optimization of shadow volumes, called from R_CreateShadowVolume

============================================================
*/


typedef struct {
	idVec3	*verts;			// includes both front and back projections, caller should free
	int		numVerts;
	glIndex_t	*indexes;	// caller should free

	// indexes must be sorted frontCap, rearCap, silPlanes so the caps can be removed
	// when the viewer is in a position that they don't need to see them
	int		numFrontCapIndexes;
	int		numRearCapIndexes;
	int		numSilPlaneIndexes;
	int		totalIndexes;
} optimizedShadow_t;

optimizedShadow_t SuperOptimizeOccluders( idVec4 *verts, glIndex_t *indexes, int numIndexes,
										 idPlane projectionPlane, idVec3 projectionOrigin );

void CleanupOptimizedShadowTris( srfTriangles_t *tri );

/*
============================================================

TRISURF

============================================================
*/

#define USE_TRI_DATA_ALLOCATOR

void				R_InitTriSurfData( void );
void				R_ShutdownTriSurfData( void );
void				R_PurgeTriSurfData( frameData_t *frame );
void				R_ShowTriSurfMemory_f( const idCmdArgs &args );

srfTriangles_t *	R_AllocStaticTriSurf( void );
srfTriangles_t *	R_CopyStaticTriSurf( const srfTriangles_t *tri );
void				R_AllocStaticTriSurfVerts( srfTriangles_t *tri, int numVerts );
void				R_AllocStaticTriSurfIndexes( srfTriangles_t *tri, int numIndexes );
void				R_AllocStaticTriSurfShadowVerts( srfTriangles_t *tri, int numVerts );
void				R_AllocStaticTriSurfPlanes( srfTriangles_t *tri, int numIndexes );
// DUDE binary model cache (Model.cpp LoadBinaryModel): the derived arrays must come from the
// very pools R_ReallyFreeStaticTriSurf frees them into
void				R_AllocStaticTriSurfSilIndexes( srfTriangles_t *tri, int numIndexes );
void				R_AllocStaticTriSurfSilEdges( srfTriangles_t *tri, int numSilEdges );
void				R_AllocStaticTriSurfDominantTris( srfTriangles_t *tri, int numVerts );
void				R_AllocStaticTriSurfMirroredVerts( srfTriangles_t *tri, int numMirroredVerts );
void				R_AllocStaticTriSurfDupVerts( srfTriangles_t *tri, int numDupVerts );
void				R_ResizeStaticTriSurfVerts( srfTriangles_t *tri, int numVerts );
void				R_ResizeStaticTriSurfIndexes( srfTriangles_t *tri, int numIndexes );
void				R_ResizeStaticTriSurfShadowVerts( srfTriangles_t *tri, int numVerts );
void				R_ReferenceStaticTriSurfVerts( srfTriangles_t *tri, const srfTriangles_t *reference );
void				R_ReferenceStaticTriSurfIndexes( srfTriangles_t *tri, const srfTriangles_t *reference );
void				R_FreeStaticTriSurfSilIndexes( srfTriangles_t *tri );
void				R_FreeStaticTriSurfSilEdges( srfTriangles_t *tri );
void				R_FreeStaticTriSurfIndexes( srfTriangles_t *tri );
void				R_FreeStaticTriSurf( srfTriangles_t *tri );
void				R_FreeStaticTriSurfVertexCaches( srfTriangles_t *tri );
void				R_ReallyFreeStaticTriSurf( srfTriangles_t *tri );
void				R_FreeDeferredTriSurfs( frameData_t *frame );
int					R_TriSurfMemory( const srfTriangles_t *tri );

void				R_BoundTriSurf( srfTriangles_t *tri );
void				R_RemoveDuplicatedTriangles( srfTriangles_t *tri );
void				R_CreateSilIndexes( srfTriangles_t *tri );
void				R_IdentifySilEdges( srfTriangles_t *tri, bool omitCoplanarEdges );
void				R_RemoveDegenerateTriangles( srfTriangles_t *tri );
void				R_RemoveUnusedVerts( srfTriangles_t *tri );
void				R_RangeCheckIndexes( const srfTriangles_t *tri );
void				R_CreateVertexNormals( srfTriangles_t *tri );	// also called by dmap
void				R_DeriveFacePlanes( srfTriangles_t *tri );		// also called by renderbump
void				R_CleanupTriangles( srfTriangles_t *tri, bool createNormals, bool identifySilEdges, bool useUnsmoothedTangents );
void				R_ReverseTriangles( srfTriangles_t *tri );

// Only deals with vertexes and indexes, not silhouettes, planes, etc.
// Does NOT perform a cleanup triangles, so there may be duplicated verts in the result.
srfTriangles_t *	R_MergeSurfaceList( const srfTriangles_t **surfaces, int numSurfaces );
srfTriangles_t *	R_MergeTriangles( const srfTriangles_t *tri1, const srfTriangles_t *tri2 );

// if the deformed verts have significant enough texture coordinate changes to reverse the texture
// polarity of a triangle, the tangents will be incorrect
void				R_DeriveTangents( srfTriangles_t *tri, bool allocFacePlanes = true );

// deformable meshes precalculate as much as possible from a base frame, then generate
// complete srfTriangles_t from just a new set of vertexes
typedef struct deformInfo_s {
	int				numSourceVerts;

	// numOutputVerts may be smaller if the input had duplicated or degenerate triangles
	// it will often be larger if the input had mirrored texture seams that needed
	// to be busted for proper tangent spaces
	int				numOutputVerts;

	int				numMirroredVerts;
	int *			mirroredVerts;

	int				numIndexes;
	glIndex_t *		indexes;

	glIndex_t *		silIndexes;

	int				numDupVerts;
	int *			dupVerts;

	int				numSilEdges;
	silEdge_t *		silEdges;

	dominantTri_t *	dominantTris;
} deformInfo_t;


deformInfo_t *		R_BuildDeformInfo( int numVerts, const idDrawVert *verts, int numIndexes, const int *indexes, bool useUnsmoothedTangents );
void				R_FreeDeformInfo( deformInfo_t *deformInfo );
int					R_DeformInfoMemoryUsed( deformInfo_t *deformInfo );

/*
============================================================

SUBVIEW

============================================================
*/

bool	R_PreciseCullSurface( const drawSurf_t *drawSurf, idBounds &ndcBounds );
bool	R_GenerateSubViews( void );

/*
============================================================

SCENE GENERATION

============================================================
*/

void R_InitFrameData( void );
void R_ShutdownFrameData( void );
int R_CountFrameData( void );
void R_ToggleSmpFrame( void );
void *R_FrameAlloc( int bytes );
void *R_ClearedFrameAlloc( int bytes );
void R_FrameFree( void *data );

void *R_StaticAlloc( int bytes );		// just malloc with error checking
void *R_ClearedStaticAlloc( int bytes );	// with memset
void R_StaticFree( void *data );

// Phase 2 GPU MD5 skinning (docs/gpu-offload-plan.md): the front end (idMD5Mesh::UpdateSurface)
// records a skin job per visible skinned surface; the RHI backend flushes them as compute
// dispatches in the pre-scene window (RB_RHI_ExecuteBackEndCommands, before the first pass).
// All handles are rhi::BufferHandle (unsigned int); jointData is an R_FrameAlloc snapshot of
// this frame's joint palette. Vulkan only; no-op on GL3. Impl in rhi/RhiBackend.cpp.
void RB_RHI_AddSkinJob( unsigned int shader, unsigned int outVB, int numOutVerts,
                        unsigned int weightsBuf, unsigned int wdescBuf, unsigned int wstartBuf, unsigned int localTbnBuf,
                        const void *jointData, int numJoints, float skinScale,
                        unsigned int prevPosBuf = 0, bool prevPoseInVB = false,	// prevPosBuf: skinned motion vectors (kernel variant PREVPOS)
                        // re-derive + weld second pass (r_gpuSkinDerive): 0 deriveShader = keep the position kernel's LBS bind TBN
                        unsigned int deriveShader = 0, unsigned int domIdxBuf = 0, unsigned int domScaleBuf = 0,
                        unsigned int weldInfoBuf = 0, unsigned int weldMembersBuf = 0 );
void RB_RHI_FlushSkinJobs( void );		// dispatch + clear the recorded jobs (backend, pre-scene)

// Roadmap B compute deform-once tessellation (docs/tessellation.md; Vulkan only, no-op on GL3). Records
// a deform job per visible tessellated surface; flushed as compute dispatches in the pre-scene window
// AFTER the skin jobs (so the skin-write->deform-read COMPUTE->COMPUTE barrier orders them). srcVB is
// the GPU source (gpuSkinVB) OR 0 with srcCpu an R_FrameAlloc'd copy the flush uploads. The three
// static topology SSBOs are the mesh's; outVB is the per-surface expanded output. Impl rhi/RhiBackend.cpp.
void RB_RHI_AddTessJob( unsigned int shader, unsigned int srcVB, const void *srcCpu, int numSrcVerts,
                        unsigned int outVB, unsigned int barySeam, unsigned int srcTri, unsigned int height,
                        int numOutVerts, float dispStrength );
void RB_RHI_FlushTessJobs( void );		// dispatch + clear the recorded deform jobs (backend, pre-scene)


/*
=============================================================

RENDERER DEBUG TOOLS

=============================================================
*/

float RB_DrawTextLength( const char *text, float scale, int len );
void RB_AddDebugText( const char *text, const idVec3 &origin, float scale, const idVec4 &color, const idMat3 &viewAxis, const int align, const int lifetime, const bool depthTest );
void RB_ClearDebugText( int time );
void RB_AddDebugLine( const idVec4 &color, const idVec3 &start, const idVec3 &end, const int lifeTime, const bool depthTest );
void RB_ClearDebugLines( int time );
void RB_AddDebugPolygon( const idVec4 &color, const idWinding &winding, const int lifeTime, const bool depthTest );
void RB_ClearDebugPolygons( int time );
void RB_DrawBounds( const idBounds &bounds );
void RB_ShowLights( drawSurf_t **drawSurfs, int numDrawSurfs );
void RB_ShowLightCount( drawSurf_t **drawSurfs, int numDrawSurfs );
void RB_PolygonClear( void );
void RB_ScanStencilBuffer( void );
void RB_ShowDestinationAlpha( void );
void RB_ShowOverdraw( void );
void RB_RenderDebugTools( drawSurf_t **drawSurfs, int numDrawSurfs );
void RB_ShutdownDebugTools( void );

/*
=============================================================

TR_BACKEND

=============================================================
*/

void RB_SetDefaultGLState( void );
void RB_SetGL2D( void );

void RB_ShowImages( void );

void RB_ExecuteBackEndCommands( const emptyCommand_t *cmds );


/*
=============================================================

TR_GUISURF

=============================================================
*/

void R_SurfaceToTextureAxis( const srfTriangles_t *tri, idVec3 &origin, idVec3 axis[3] );
void R_RenderGuiSurf( idUserInterface *gui, drawSurf_t *drawSurf );

// DUDE: dominant colour of the GUI surface most recently rendered by R_RenderGuiSurf,
// handed to the emissive fill-light queue in R_AddDrawSurf. Only meaningful when valid.
extern idVec3 tr_guiEmissiveColor;
extern bool   tr_guiEmissiveColorValid;

/*
=============================================================

TR_ORDERINDEXES

=============================================================
*/

void R_OrderIndexes( int numIndexes, glIndex_t *indexes );

/*
=============================================================

TR_DEFORM

=============================================================
*/

void R_DeformDrawSurf( drawSurf_t *drawSurf );

/*
=============================================================

TR_TRACE

=============================================================
*/

typedef struct {
	float		fraction;
	// only valid if fraction < 1.0
	idVec3		point;
	idVec3		normal;
	int			indexes[3];
} localTrace_t;

localTrace_t R_LocalTrace( const idVec3 &start, const idVec3 &end, const float radius, const srfTriangles_t *tri );
void RB_ShowTrace( drawSurf_t **drawSurfs, int numDrawSurfs );

/*
=============================================================

TR_SHADOWBOUNDS

=============================================================
*/
idScreenRect R_CalcIntersectionScissor( const idRenderLightLocal * lightDef,
										const idRenderEntityLocal * entityDef,
										const viewDef_t * viewDef );

//=============================================

#endif /* !__TR_LOCAL_H__ */
