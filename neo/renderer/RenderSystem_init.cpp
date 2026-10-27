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

#include "sys/platform.h"
#include "idlib/LangDict.h"
#include "framework/Licensee.h"
#include "framework/Console.h"
#include "framework/Session.h"
#include "renderer/VertexCache.h"
#include "renderer/ModelManager.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/GuiModel.h"
#include "sound/sound.h"
#include "ui/UserInterface.h"

#include "renderer/tr_local.h"
#include "renderer/bse/BseEffect.h"
#include "renderer/rhi/RHI.h"
#include "renderer/rhi/GL3Local.h"
#include "sys/sys_imgui.h"		// DUDE Phase 4 M6: ImGui init on the Vulkan backend

#include "framework/GameCallbacks_local.h"
#include "framework/Game.h"

// Vista OpenGL wrapper check
#ifdef _WIN32
#include "sys/win32/win_local.h"
#endif

#include "stb_image_write.h"

// functions that are not called every frame

glconfig_t	glConfig;

const char *r_rendererArgs[] = { "best", "arb2", NULL };

idCVar r_inhibitFragmentProgram( "r_inhibitFragmentProgram", "0", CVAR_RENDERER | CVAR_BOOL, "ignore the fragment program extension" );
idCVar r_useLightPortalFlow( "r_useLightPortalFlow", "1", CVAR_RENDERER | CVAR_BOOL, "use a more precise area reference determination" );
// Selects the rendering backend. Only "opengl" is implemented today; "vulkan"
// (1.1 baseline) and "vulkan-rt" (modern profile + ray tracing) are in development
// (see docs/vulkan-port.md) and require a DHEWM3_VULKAN build. Switching backends
// needs a vid_restart (window recreate); the settings-menu selector comes later.
idCVar r_graphicsAPI( "r_graphicsAPI", "opengl", CVAR_RENDERER | CVAR_ARCHIVE, "rendering backend: opengl (legacy, default), opengl3 (GL 3.3 core), vulkan, vulkan-rt (Vulkan + ray tracing)" );
idCVar r_rhiActive( "r_rhiActive", "0", CVAR_RENDERER | CVAR_BOOL, "1 when the active backend routes through the RHI executor (opengl3 / vulkan); set by the renderer at init, don't change" );
idCVar r_multiSamples( "r_multiSamples", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "number of antialiasing samples" );
idCVar r_mode( "r_mode", "5", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER, "video mode number" );
idCVar r_displayRefresh( "r_displayRefresh", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "optional display refresh rate option for vid mode", 0.0f, 200.0f );
idCVar r_fullscreen( "r_fullscreen", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "0 = windowed, 1 = full screen" );
idCVar r_fullscreenDesktop( "r_fullscreenDesktop", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "0: 'real' fullscreen mode 1: keep resolution 'desktop' fullscreen mode" );
idCVar r_fullscreenDisplay( "r_fullscreenDisplay", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "which monitor to open the (fullscreen) window on. -1 = auto (the display the mouse cursor is on at launch). On a multi-monitor setup with mixed refresh rates, set this to your high-refresh display's index (see the 'SDL detected N displays' list printed at startup). Needs a full vid_restart", -1, 16 );
idCVar r_customWidth( "r_customWidth", "720", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen width. set r_mode to -1 to activate" );
idCVar r_customHeight( "r_customHeight", "486", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen height. set r_mode to -1 to activate" );
idCVar r_singleTriangle( "r_singleTriangle", "0", CVAR_RENDERER | CVAR_BOOL, "only draw a single triangle per primitive" );
idCVar r_checkBounds( "r_checkBounds", "0", CVAR_RENDERER | CVAR_BOOL, "compare all surface bounds with precalculated ones" );

idCVar r_useConstantMaterials( "r_useConstantMaterials", "1", CVAR_RENDERER | CVAR_BOOL, "use pre-calculated material registers if possible" );
idCVar r_useSilRemap( "r_useSilRemap", "1", CVAR_RENDERER | CVAR_BOOL, "consider verts with the same XYZ, but different ST the same for shadows" );
idCVar r_useNodeCommonChildren( "r_useNodeCommonChildren", "1", CVAR_RENDERER | CVAR_BOOL, "stop pushing reference bounds early when possible" );
idCVar r_useShadowProjectedCull( "r_useShadowProjectedCull", "1", CVAR_RENDERER | CVAR_BOOL, "discard triangles outside light volume before shadowing" );
idCVar r_useShadowVertexProgram( "r_useShadowVertexProgram", "1", CVAR_RENDERER | CVAR_BOOL, "do the shadow projection in the vertex program on capable cards" );
idCVar r_useShadowSurfaceScissor( "r_useShadowSurfaceScissor", "1", CVAR_RENDERER | CVAR_BOOL, "scissor shadows by the scissor rect of the interaction surfaces" );
idCVar r_useInteractionTable( "r_useInteractionTable", "1", CVAR_RENDERER | CVAR_BOOL, "create a full entityDefs * lightDefs table to make finding interactions faster" );
idCVar r_useTurboShadow( "r_useTurboShadow", "1", CVAR_RENDERER | CVAR_BOOL, "use the infinite projection with W technique for dynamic shadows" );
idCVar r_useTwoSidedStencil( "r_useTwoSidedStencil", "1", CVAR_RENDERER | CVAR_BOOL, "do stencil shadows in one pass with different ops on each side" );
idCVar r_useDeferredTangents( "r_useDeferredTangents", "1", CVAR_RENDERER | CVAR_BOOL, "defer tangents calculations after deform" );
idCVar r_useCachedDynamicModels( "r_useCachedDynamicModels", "1", CVAR_RENDERER | CVAR_BOOL, "cache snapshots of dynamic models" );

idCVar r_useVertexBuffers( "r_useVertexBuffers", "1", CVAR_RENDERER | CVAR_INTEGER, "use ARB_vertex_buffer_object for vertexes", 0, 1, idCmdSystem::ArgCompletion_Integer<0,1>  );
idCVar r_useIndexBuffers( "r_useIndexBuffers", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "use ARB_vertex_buffer_object for indexes", 0, 1, idCmdSystem::ArgCompletion_Integer<0,1>  );

idCVar r_useStateCaching( "r_useStateCaching", "1", CVAR_RENDERER | CVAR_BOOL, "avoid redundant state changes in GL_*() calls" );
idCVar r_useInfiniteFarZ( "r_useInfiniteFarZ", "1", CVAR_RENDERER | CVAR_BOOL, "use the no-far-clip-plane trick" );

idCVar r_znear( "r_znear", "3", CVAR_RENDERER | CVAR_FLOAT, "near Z clip plane distance", 0.001f, 200.0f );

idCVar r_ignoreGLErrors( "r_ignoreGLErrors", "1", CVAR_RENDERER | CVAR_BOOL, "ignore GL errors" );
idCVar r_finish( "r_finish", "0", CVAR_RENDERER | CVAR_BOOL, "force a call to glFinish() every frame" );
idCVar r_swapInterval( "r_swapInterval", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "changes the GL swap interval" );
idCVar r_vrr( "r_vrr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "variable refresh rate mode (G-SYNC / FreeSync / VESA Adaptive-Sync): keeps VSync on (FIFO) and auto-paces the render rate just below the display's refresh so frames stay inside the VRR window. Needs VRR enabled in the GPU driver/compositor and a VRR-capable display." );
// Bridge cvar for the native System menu's VSync cycler (a choiceDef can only drive one cvar, but VRR
// needs r_swapInterval + r_vrr). The menu writes 0..3; R_CheckCvars translates that into the real cvars
// and mirrors their live state back here. Not archived -- it's derived from r_swapInterval/r_vrr each
// frame (default -1 = uninitialized, so it can never clobber saved settings on first boot).
idCVar dude_vsync( "dude_vsync", "-1", CVAR_RENDERER | CVAR_INTEGER, "VSync mode for the in-game menu: 0 = off, 1 = on, 2 = adaptive, 3 = VRR", -1, 3 );

idCVar r_gamma( "r_gamma", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 3.0f );
idCVar r_brightness( "r_brightness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 2.0f );
idCVar r_gammaInShader( "r_gammaInShader", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Set gamma and brightness in shaders instead using hardware gamma" );

idCVar r_renderer( "r_renderer", "best", CVAR_RENDERER | CVAR_ARCHIVE, "hardware specific renderer path to use", r_rendererArgs, idCmdSystem::ArgCompletion_String<r_rendererArgs> );

idCVar r_jitter( "r_jitter", "0", CVAR_RENDERER | CVAR_BOOL, "randomly subpixel jitter the projection matrix" );
idCVar r_skipSuppress( "r_skipSuppress", "0", CVAR_RENDERER | CVAR_BOOL, "ignore the per-view suppressions" );
idCVar r_skipPostProcess( "r_skipPostProcess", "0", CVAR_RENDERER | CVAR_BOOL, "skip all post-process renderings" );
idCVar r_skipLightScale( "r_skipLightScale", "0", CVAR_RENDERER | CVAR_BOOL, "don't do any post-interaction light scaling, makes things dim on low-dynamic range cards" );
idCVar r_skipInteractions( "r_skipInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "skip all light/surface interaction drawing" );
idCVar r_skipDynamicTextures( "r_skipDynamicTextures", "0", CVAR_RENDERER | CVAR_BOOL, "don't dynamically create textures" );
idCVar r_skipCopyTexture( "r_skipCopyTexture", "0", CVAR_RENDERER | CVAR_BOOL, "do all rendering, but don't actually copyTexSubImage2D" );
idCVar r_skipBackEnd( "r_skipBackEnd", "0", CVAR_RENDERER | CVAR_BOOL, "don't draw anything" );
idCVar r_skipRender( "r_skipRender", "0", CVAR_RENDERER | CVAR_BOOL, "skip 3D rendering, but pass 2D" );
idCVar r_skipRenderContext( "r_skipRenderContext", "0", CVAR_RENDERER | CVAR_BOOL, "NULL the rendering context during backend 3D rendering" );
idCVar r_skipTranslucent( "r_skipTranslucent", "0", CVAR_RENDERER | CVAR_BOOL, "skip the translucent interaction rendering" );
idCVar r_skipAmbient( "r_skipAmbient", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all non-interaction drawing" );
idCVar r_skipNewAmbient( "r_skipNewAmbient", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "bypasses all vertex/fragment program ambient drawing" );
idCVar r_skipBlendLights( "r_skipBlendLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all blend lights" );
idCVar r_skipFogLights( "r_skipFogLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all fog lights" );
idCVar r_skipDeforms( "r_skipDeforms", "0", CVAR_RENDERER | CVAR_BOOL, "leave all deform materials in their original state" );
idCVar r_skipFrontEnd( "r_skipFrontEnd", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all front end work, but 2D gui rendering still draws" );
idCVar r_skipUpdates( "r_skipUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't accept any entity or light updates, making everything static" );
idCVar r_skipOverlays( "r_skipOverlays", "0", CVAR_RENDERER | CVAR_BOOL, "skip overlay surfaces" );
idCVar r_skipSpecular( "r_skipSpecular", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_CHEAT | CVAR_ARCHIVE, "use black for specular1" );
idCVar r_skipBump( "r_skipBump", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "uses a flat surface instead of the bump map" );
idCVar r_skipDiffuse( "r_skipDiffuse", "0", CVAR_RENDERER | CVAR_BOOL, "use black for diffuse" );
idCVar r_whiteWorld( "r_whiteWorld", "0", CVAR_RENDERER | CVAR_INTEGER, "white-world debug: 0 = off, 1 = diffuse=white (lighting incl. light/material colour), 2 = clay (also neutralises light/material colour and forces metalness 0, so only occlusion/relief remains — SSAO + POM in grey)", 0, 2 );
idCVar r_skipROQ( "r_skipROQ", "0", CVAR_RENDERER | CVAR_BOOL, "skip ROQ decoding" );

idCVar r_ignore( "r_ignore", "0", CVAR_RENDERER, "used for random debugging without defining new vars" );
idCVar r_ignore2( "r_ignore2", "0", CVAR_RENDERER, "used for random debugging without defining new vars" );
idCVar r_usePreciseTriangleInteractions( "r_usePreciseTriangleInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "1 = do winding clipping to determine if each ambiguous tri should be lit" );
idCVar r_useCulling( "r_useCulling", "2", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = sphere, 2 = sphere + box", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_useLightCulling( "r_useLightCulling", "3", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = box, 2 = exact clip of polyhedron faces, 3 = also areas", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_useLightScissors( "r_useLightScissors", "1", CVAR_RENDERER | CVAR_BOOL, "1 = use custom scissor rectangle for each light" );
idCVar r_useClippedLightScissors( "r_useClippedLightScissors", "1", CVAR_RENDERER | CVAR_INTEGER, "0 = full screen when near clipped, 1 = exact when near clipped, 2 = exact always", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_useEntityCulling( "r_useEntityCulling", "1", CVAR_RENDERER | CVAR_BOOL, "0 = none, 1 = box" );
idCVar r_useEntityScissors( "r_useEntityScissors", "0", CVAR_RENDERER | CVAR_BOOL, "1 = use custom scissor rectangle for each entity" );
idCVar r_useInteractionCulling( "r_useInteractionCulling", "1", CVAR_RENDERER | CVAR_BOOL, "1 = cull interactions" );
idCVar r_useInteractionScissors( "r_useInteractionScissors", "2", CVAR_RENDERER | CVAR_INTEGER, "1 = use a custom scissor rectangle for each shadow interaction, 2 = also crop using portal scissors", -2, 2, idCmdSystem::ArgCompletion_Integer<-2,2> );
idCVar r_useShadowCulling( "r_useShadowCulling", "1", CVAR_RENDERER | CVAR_BOOL, "try to cull shadows from partially visible lights" );
idCVar r_useFrustumFarDistance( "r_useFrustumFarDistance", "0", CVAR_RENDERER | CVAR_FLOAT, "if != 0 force the view frustum far distance to this distance" );
idCVar r_clear( "r_clear", "2", CVAR_RENDERER, "force screen clear every frame, 1 = purple, 2 = black, 'r g b' = custom" );
idCVar r_offsetFactor( "r_offsetfactor", "0", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
idCVar r_offsetUnits( "r_offsetunits", "-600", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
idCVar r_shadowPolygonOffset( "r_shadowPolygonOffset", "-1", CVAR_RENDERER | CVAR_FLOAT, "bias value added to depth test for stencil shadow drawing" );
idCVar r_shadowPolygonFactor( "r_shadowPolygonFactor", "0", CVAR_RENDERER | CVAR_FLOAT, "scale value for stencil shadow drawing" );
idCVar r_frontBuffer( "r_frontBuffer", "0", CVAR_RENDERER | CVAR_BOOL, "draw to front buffer for debugging" );
idCVar r_skipSubviews( "r_skipSubviews", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = don't render any gui elements on surfaces" );
idCVar r_skipGuiShaders( "r_skipGuiShaders", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all gui elements on surfaces, 2 = skip drawing but still handle events, 3 = draw but skip events", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_skipParticles( "r_skipParticles", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all particle systems", 0, 1, idCmdSystem::ArgCompletion_Integer<0,1> );
idCVar r_subviewOnly( "r_subviewOnly", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't render main view, allowing subviews to be debugged" );
idCVar r_shadows( "r_shadows", "1", CVAR_RENDERER | CVAR_BOOL  | CVAR_ARCHIVE, "enable shadows" );
idCVar r_testARBProgram( "r_testARBProgram", "0", CVAR_RENDERER | CVAR_BOOL, "experiment with vertex/fragment programs" );
idCVar r_testGamma( "r_testGamma", "0", CVAR_RENDERER | CVAR_FLOAT, "if > 0 draw a grid pattern to test gamma levels", 0, 195 );
idCVar r_testGammaBias( "r_testGammaBias", "0", CVAR_RENDERER | CVAR_FLOAT, "if > 0 draw a grid pattern to test gamma levels" );
idCVar r_testStepGamma( "r_testStepGamma", "0", CVAR_RENDERER | CVAR_FLOAT, "if > 0 draw a grid pattern to test gamma levels" );
idCVar r_lightScale( "r_lightScale", "2", CVAR_RENDERER | CVAR_FLOAT, "all light intensities are multiplied by this" );
idCVar r_lightSourceRadius( "r_lightSourceRadius", "0", CVAR_RENDERER | CVAR_FLOAT, "for soft-shadow sampling" );
idCVar r_flareSize( "r_flareSize", "0.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "size of light flares/glares from the material deform (1 = vanilla Doom 3, lower to dampen, 0 = off)" );

idCVar r_useExternalShadows( "r_useExternalShadows", "1", CVAR_RENDERER | CVAR_INTEGER, "1 = skip drawing caps when outside the light volume, 2 = force to no caps for testing", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_useOptimizedShadows( "r_useOptimizedShadows", "1", CVAR_RENDERER | CVAR_BOOL, "use the dmap generated static shadow volumes" );
idCVar r_useScissor( "r_useScissor", "1", CVAR_RENDERER | CVAR_BOOL, "scissor clip as portals and lights are processed" );
idCVar r_useCombinerDisplayLists( "r_useCombinerDisplayLists", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT, "put all nvidia register combiner programming in display lists" );
idCVar r_useDepthBoundsTest( "r_useDepthBoundsTest", "1", CVAR_RENDERER | CVAR_BOOL, "use depth bounds test to reduce shadow fill" );

idCVar r_screenFraction( "r_screenFraction", "100", CVAR_RENDERER | CVAR_INTEGER, "for testing fill rate, the resolution of the entire screen can be changed" );
idCVar r_demonstrateBug( "r_demonstrateBug", "0", CVAR_RENDERER | CVAR_BOOL, "used during development to show IHV's their problems" );
idCVar r_usePortals( "r_usePortals", "1", CVAR_RENDERER | CVAR_BOOL, " 1 = use portals to perform area culling, otherwise draw everything" );
idCVar r_singleLight( "r_singleLight", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one light" );
idCVar r_singleEntity( "r_singleEntity", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one entity" );
idCVar r_singleSurface( "r_singleSurface", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one surface on each entity" );
idCVar r_singleArea( "r_singleArea", "0", CVAR_RENDERER | CVAR_BOOL, "only draw the portal area the view is actually in" );
idCVar r_forceLoadImages( "r_forceLoadImages", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "draw all images to screen after registration" );
idCVar r_orderIndexes( "r_orderIndexes", "1", CVAR_RENDERER | CVAR_BOOL, "perform index reorganization to optimize vertex use" );
idCVar r_lightAllBackFaces( "r_lightAllBackFaces", "0", CVAR_RENDERER | CVAR_BOOL, "light all the back faces, even when they would be shadowed" );

// DG: added this to support "nospecular" param of lights
// NOTE: if you're developing a standalone game, you'll probably want to use "1" as default value
idCVar r_supportNoSpecular( "r_supportNoSpecular", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
		"Support 'nospecular' parm on lights. Vanilla Doom3 didn't, so the original maps are probably "
		"expecting it to not do anything. -1: Only support in maps that have have \"allow_nospecular\" \"1\" set in worldspawn (default), "
		"0: never respect 'nospecular' parm 1: support 'nospecular' in all maps", -1, 1 );

// visual debugging info
idCVar r_showPortals( "r_showPortals", "0", CVAR_RENDERER | CVAR_BOOL, "draw portal outlines in color based on passed / not passed" );
idCVar r_showUnsmoothedTangents( "r_showUnsmoothedTangents", "0", CVAR_RENDERER | CVAR_BOOL, "if 1, put all nvidia register combiner programming in display lists" );
idCVar r_showSilhouette( "r_showSilhouette", "0", CVAR_RENDERER | CVAR_BOOL, "highlight edges that are casting shadow planes" );
idCVar r_showVertexColor( "r_showVertexColor", "0", CVAR_RENDERER | CVAR_BOOL, "draws all triangles with the solid vertex color" );
idCVar r_showUpdates( "r_showUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "report entity and light updates and ref counts" );
idCVar r_showDemo( "r_showDemo", "0", CVAR_RENDERER | CVAR_BOOL, "report reads and writes to the demo file" );
idCVar r_showDynamic( "r_showDynamic", "0", CVAR_RENDERER | CVAR_BOOL, "report stats on dynamic surface generation" );
idCVar r_showLightScale( "r_showLightScale", "0", CVAR_RENDERER | CVAR_BOOL, "report the scale factor applied to drawing for overbrights" );
idCVar r_showDefs( "r_showDefs", "0", CVAR_RENDERER | CVAR_BOOL, "report the number of modeDefs and lightDefs in view" );
idCVar r_showTrace( "r_showTrace", "0", CVAR_RENDERER | CVAR_INTEGER, "show the intersection of an eye trace with the world", idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_showIntensity( "r_showIntensity", "0", CVAR_RENDERER | CVAR_BOOL, "draw the screen colors based on intensity, red = 0, green = 128, blue = 255" );
idCVar r_showImages( "r_showImages", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = show all images instead of rendering, 2 = show in proportional size", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_showSmp( "r_showSmp", "0", CVAR_RENDERER | CVAR_BOOL, "show which end (front or back) is blocking" );
idCVar r_showLights( "r_showLights", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = just print volumes numbers, highlighting ones covering the view, 2 = also draw planes of each volume, 3 = also draw edges of each volume", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showShadows( "r_showShadows", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = visualize the stencil shadow volumes, 2 = draw filled in", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showShadowCount( "r_showShadowCount", "0", CVAR_RENDERER | CVAR_INTEGER, "colors screen based on shadow volume depth complexity, >= 2 = print overdraw count based on stencil index values, 3 = only show turboshadows, 4 = only show static shadows", 0, 4, idCmdSystem::ArgCompletion_Integer<0,4> );
idCVar r_showLightScissors( "r_showLightScissors", "0", CVAR_RENDERER | CVAR_BOOL, "show light scissor rectangles" );
idCVar r_showEntityScissors( "r_showEntityScissors", "0", CVAR_RENDERER | CVAR_BOOL, "show entity scissor rectangles" );
idCVar r_showInteractionFrustums( "r_showInteractionFrustums", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = show a frustum for each interaction, 2 = also draw lines to light origin, 3 = also draw entity bbox", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showInteractionScissors( "r_showInteractionScissors", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = show screen rectangle which contains the interaction frustum, 2 = also draw construction lines", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_showLightCount( "r_showLightCount", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = colors surfaces based on light count, 2 = also count everything through walls, 3 = also print overdraw", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showViewEntitys( "r_showViewEntitys", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = displays the bounding boxes of all view models, 2 = print index numbers" );
idCVar r_showTris( "r_showTris", "0", CVAR_RENDERER | CVAR_INTEGER, "enables wireframe rendering of the world, 1 = only draw visible ones, 2 = draw all front facing, 3 = draw all", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showSurfaceInfo( "r_showSurfaceInfo", "0", CVAR_RENDERER | CVAR_BOOL, "show surface material name under crosshair" );
idCVar r_showNormals( "r_showNormals", "0", CVAR_RENDERER | CVAR_FLOAT, "draws wireframe normals" );
idCVar r_showMemory( "r_showMemory", "0", CVAR_RENDERER | CVAR_BOOL, "print frame memory utilization" );
idCVar r_showCull( "r_showCull", "0", CVAR_RENDERER | CVAR_BOOL, "report sphere and box culling stats" );
idCVar r_showInteractions( "r_showInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "report interaction generation activity" );
idCVar r_showDepth( "r_showDepth", "0", CVAR_RENDERER | CVAR_BOOL, "display the contents of the depth buffer and the depth range" );
idCVar r_showSurfaces( "r_showSurfaces", "0", CVAR_RENDERER | CVAR_BOOL, "report surface/light/shadow counts" );
idCVar r_showPrimitives( "r_showPrimitives", "0", CVAR_RENDERER | CVAR_INTEGER, "report drawsurf/index/vertex counts" );
idCVar r_showEdges( "r_showEdges", "0", CVAR_RENDERER | CVAR_BOOL, "draw the sil edges" );
idCVar r_showTexturePolarity( "r_showTexturePolarity", "0", CVAR_RENDERER | CVAR_BOOL, "shade triangles by texture area polarity" );
idCVar r_showTangentSpace( "r_showTangentSpace", "0", CVAR_RENDERER | CVAR_INTEGER, "shade triangles by tangent space, 1 = use 1st tangent vector, 2 = use 2nd tangent vector, 3 = use normal vector", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );
idCVar r_showDominantTri( "r_showDominantTri", "0", CVAR_RENDERER | CVAR_BOOL, "draw lines from vertexes to center of dominant triangles" );
idCVar r_showAlloc( "r_showAlloc", "0", CVAR_RENDERER | CVAR_BOOL, "report alloc/free counts" );
idCVar r_showTextureVectors( "r_showTextureVectors", "0", CVAR_RENDERER | CVAR_FLOAT, " if > 0 draw each triangles texture (tangent) vectors" );
idCVar r_showOverDraw( "r_showOverDraw", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = geometry overdraw, 2 = light interaction overdraw, 3 = geometry and light interaction overdraw", 0, 3, idCmdSystem::ArgCompletion_Integer<0,3> );

idCVar r_lockSurfaces( "r_lockSurfaces", "0", CVAR_RENDERER | CVAR_BOOL, "allow moving the view point without changing the composition of the scene, including culling" );
idCVar r_useEntityCallbacks( "r_useEntityCallbacks", "1", CVAR_RENDERER | CVAR_BOOL, "if 0, issue the callback immediately at update time, rather than defering" );

idCVar r_showSkel( "r_showSkel", "0", CVAR_RENDERER | CVAR_INTEGER, "draw the skeleton when model animates, 1 = draw model with skeleton, 2 = draw skeleton only", 0, 2, idCmdSystem::ArgCompletion_Integer<0,2> );
idCVar r_jointNameScale( "r_jointNameScale", "0.02", CVAR_RENDERER | CVAR_FLOAT, "size of joint names when r_showskel is set to 1" );
idCVar r_jointNameOffset( "r_jointNameOffset", "0.5", CVAR_RENDERER | CVAR_FLOAT, "offset of joint names when r_showskel is set to 1" );

idCVar r_debugLineDepthTest( "r_debugLineDepthTest", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "perform depth test on debug lines" );
idCVar r_debugLineWidth( "r_debugLineWidth", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "width of debug lines" );
idCVar r_debugArrowStep( "r_debugArrowStep", "120", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "step size of arrow cone line rotation in degrees", 0, 120 );
idCVar r_debugPolygonFilled( "r_debugPolygonFilled", "1", CVAR_RENDERER | CVAR_BOOL, "draw a filled polygon" );

idCVar r_materialOverride( "r_materialOverride", "", CVAR_RENDERER, "overrides all materials", idCmdSystem::ArgCompletion_Decl<DECL_MATERIAL> );

idCVar r_debugRenderToTexture( "r_debugRenderToTexture", "0", CVAR_RENDERER | CVAR_INTEGER, "" );

// DG: let users disable the "scale menus to 4:3" hack
idCVar r_scaleMenusTo43( "r_scaleMenusTo43", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Scale menus, fullscreen videos and PDA to 4:3 aspect ratio" );
// DG: the fscking patent has finally expired
idCVar r_useCarmacksReverse( "r_useCarmacksReverse", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Use Z-Fail (Carmack's Reverse) when rendering shadows" );
idCVar r_useStencilOpSeparate( "r_useStencilOpSeparate", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Use glStencilOpSeparate() (if available) when rendering shadows" );
idCVar r_screenshotFormat("r_screenshotFormat", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Screenshot format. 0 = TGA (default), 1 = BMP, 2 = PNG, 3 = JPG");
idCVar r_screenshotJpgQuality("r_screenshotJpgQuality", "75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Screenshot quality for JPG images (1-100). Lower value means smaller file but worse quality");
idCVar r_screenshotPngCompression("r_screenshotPngCompression", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Compression level when using PNG screenshots (0-9). Higher levels generate smaller files, but take noticeably longer");
// DG: allow freely resizing the window
idCVar r_windowResizable("r_windowResizable", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Allow resizing (and maximizing) the window (needs SDL2; with 2.0.5 or newer it's applied immediately)" );
idCVar r_vidRestartAlwaysFull( "r_vidRestartAlwaysFull", 0, CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Always do a full vid_restart (ignore 'partial' argument), e.g. when changing window size" );

// DG: for soft particles (ported from TDM)
idCVar r_enableDepthCapture( "r_enableDepthCapture", "-1", CVAR_RENDERER | CVAR_INTEGER,
		"enable capturing depth buffer to texture. -1: enable automatically (if soft particles are enabled), 0: disable, 1: enable", -1, 1 ); // #3877
idCVar r_useSoftParticles( "r_useSoftParticles", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Remove the hard line where particles cut into walls and floors: drawn as without, except that they fade out over the last few units before the surface they meet. Needs r_enableDepthCapture. GL3/Vulkan only. Can slow down rendering!" ); // #3878

// DUDE: dim smoke/steam/dust particles where the scene behind them is dark, so
// smoke fades into shadow instead of glowing as grey blobs. Alpha-blended smoke is
// always dimmed; additive smoke (e.g. textures/particles/smokepuff steam) is dimmed
// when the material name reads as smoke, so additive fire/sparks/glares stay bright.
// Builds on soft particles (reuses the captured-scene render path); opengl3/Vulkan only.
idCVar r_smokeDarkBlend( "r_smokeDarkBlend", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "GL3: fade smoke/steam/dust particles into darkness. On a black background they show at only r_smokeDarkBlendFloor opacity, ramping to full by r_smokeDarkBlendKnee background brightness. Alpha smoke always; additive smoke matched by material name (fire/sparks/glares stay bright). Needs Soft Particles. Non-vanilla, opengl3/Vulkan only." );
idCVar r_smokeDarkBlendFloor( "r_smokeDarkBlendFloor", "0.55", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: smoke opacity over a fully black background, as a fraction (0.55 = 55%). 1 = no dimming. See r_smokeDarkBlend.", 0.0f, 1.0f );
idCVar r_smokeDarkBlendKnee( "r_smokeDarkBlendKnee", "0.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: background luminance (0..1) at which smoke returns to full opacity (0.2 = 20%). Lower = smoke recovers full opacity in dimmer light. See r_smokeDarkBlend.", 0.05f, 1.0f );

idCVar r_glDebugContext( "r_glDebugContext", "0", CVAR_RENDERER | CVAR_BOOL, "Enable OpenGL Debug context - requires vid_restart, needs SDL2" );

// DUDE "improvements over the classic engine": post-process effects run as one
// fullscreen pass over _currentRender after the 3D view, before 2D/GUI (HUD
// unaffected). Both default off (0 = exact passthrough); GL3/Vulkan backends only.
idCVar r_postFilmGrain( "r_postFilmGrain", "0.05", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "film grain intensity (0 = off, ~0.05..0.15, max 0.25)", 0.0f, 0.25f );
// spatial size of one grain cell: 1 = per-pixel noise (sensor-static look at high
// resolutions), ~1.5-2 clumps the noise like scanned film stock
idCVar r_postFilmGrainSize( "r_postFilmGrainSize", "1.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "film grain cell size in pixels (1 = per-pixel, 1.5-2 = coarser filmic clumps)", 1.0f, 4.0f );
idCVar r_postChromaticAberration( "r_postChromaticAberration", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "chromatic aberration strength (0 = off, ~0.1..0.35, max 0.5); off by default — opt-in taste effect", 0.0f, 0.5f );

// DUDE post-resolve antialiasing over the finished 3D view (before 2D/GUI, HUD
// unaffected). Separate from the hardware MSAA in r_multiSamples. GL3/Vulkan only.
// 1 = FXAA: one cheap pass; its subpixel low-pass (r_fxaaStrength) also damps the
//     specular/normal-map shimmer MSAA can't touch, at some texture softening.
// 2 = SMAA 1x (vendored reference implementation, docs/antialiasing.md): sharper
//     pattern-classified edge reconstruction that leaves texture interiors alone,
//     but no shimmer damping (that's TAA's job; PBR's Toksvig covers it there).
idCVar r_rhiAA( "r_rhiAA", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "post-resolve antialiasing on the opengl3/Vulkan backend (0 = off, 1 = FXAA, 2 = SMAA)", 0, 2 );
// FXAA subpixel smoothing amount: 0 = edge-only (sharpest), 1 = max subpixel blur (most
// shimmer reduction, softens textures). Only used when r_rhiAA selects FXAA.
idCVar r_fxaaStrength( "r_fxaaStrength", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "FXAA subpixel smoothing amount (0 = edge-only, 1 = strongest)", 0.0f, 1.0f );
// HDR render pipeline (docs/hdr-pipeline.md): accumulate the scene into an RGBA16F float
// buffer instead of the 8-bit backbuffer, then resolve back. Phase A is look-neutral —
// it removes fog/gradient banding without changing the image. GL3/Vulkan backends only.
idCVar r_hdr( "r_hdr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "render the scene into a float (RGBA16F) buffer to remove banding (non-vanilla; opengl3/Vulkan only)" );
// HDR tonemap (needs r_hdr): static-exposure curve folded into the resolve. 0 = off/faithful
// (bit-identical to the straight resolve), 1 = Reinhard, 2 = ACES, 3 = AgX, 4 = Khronos PBR Neutral.
idCVar r_hdrTonemap( "r_hdrTonemap", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "HDR tonemap curve (needs r_hdr): 0=off/faithful, 1=Reinhard, 2=ACES, 3=AgX, 4=Khronos PBR Neutral, 5=DUDE", 0, 5 );
idCVar r_hdrExposure( "r_hdrExposure", "1.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR static exposure multiplier applied before the tonemap curve (needs r_hdr + r_hdrTonemap>=1). Default calibrated for the DUDE curve; other curves may want it higher", 0.1f, 8.0f );
idCVar r_hdrGamma( "r_hdrGamma", "1.18", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR display gamma applied on TOP of the tonemap curve (the normal r_gamma is bypassed while a curve is active). >1 lifts shadows/midtones for a softer, less 'punchy' image (barely touches the faithful highlights); <1 deepens contrast; 1 = off. Both backends, any tonemap curve", 0.5f, 2.5f );
idCVar r_hdrDudeKnee( "r_hdrDudeKnee", "0.33", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "DUDE tonemap (r_hdrTonemap 5): highlight-rolloff knee. Below it the scene is untouched (faithful blacks/midtones); above it highlights roll off. Lower = start compressing sooner / preserve more highlight range", 0.05f, 0.95f );
idCVar r_hdrDudeDesat( "r_hdrDudeDesat", "0.69", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "DUDE tonemap (r_hdrTonemap 5): highlight white-hot desaturation. Bright cores (fireballs, lava) blend toward white as they compress so they read as detail, not a saturated blob. 0 = pure hue-preserving", 0.0f, 4.0f );
idCVar r_hdrDudeTint( "r_hdrDudeTint", "0.4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "DUDE tonemap (r_hdrTonemap 5): where the white-hot desaturation points. 0 = neutral white (a saturated lava/fire core can read pink en route to white); 1 = keep the highlight's own hue. ~0.65 keeps molten cores warm yellow-white instead of pink", 0.0f, 1.0f );
// HDR C-lite overbright (needs r_hdr + a tonemap curve): multiply additive self-illum stages
// (blend add — lamps, screens, fire, glares) so they exceed 1.0 in the float scene buffer,
// giving the tonemap curve real highlight range and eye-adaptation bright anchors. Only active
// with r_hdrTonemap>=1 (like r_hdrExposure), so it never touches the faithful mode-0 look. 1=off.
idCVar r_hdrOverbright( "r_hdrOverbright", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR overbright multiplier for additive self-illum stages (needs r_hdr + r_hdrTonemap>=1): pushes lamps/screens/fire above 1.0 for tonemap+adaptation range; 1=off. When active it auto-disables the legacy r_flareSize light haze", 1.0f, 8.0f );
// DUDE: the common "emissive overbright is live" sub-gate (tonemap curve on AND overbright > 1),
// shared by the backend additive pass and the tr_deform flare-haze suppression so they can't drift.
// Callers still AND their own HDR-active test (r_hdr cvar vs the per-frame capture state).
bool R_HdrOverbrightEnabled( void ) {
	return r_hdrTonemap.GetInteger() >= 1 && r_hdrOverbright.GetFloat() > 1.0f;
}
// Overbright saturation: the tonemap desaturates the very bright colours overbright pushes into
// (coloured fire/lava go white while white lights are fine). Pre-saturate the boosted additive
// stages so the hue survives the curve. 1 = off; white is unaffected (no saturation to boost).
idCVar r_hdrOverbrightSat( "r_hdrOverbrightSat", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR overbright saturation compensation for coloured emissive (fire/lava) that the tonemap washes out; 1 = off", 1.0f, 3.0f );
// HDR bloom (needs r_hdr): threshold the bright HDR pixels, blur them through a downsample/upsample
// chain, and add the glow back before the tonemap — the "punch" the curve otherwise rolls off.
// Threshold sets the brightness that starts to bloom (lower it to catch fire/lava directly, which
// can replace the emissive overbright). 0 = off.
idCVar r_hdrBloom( "r_hdrBloom", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR bloom intensity: glow bled from bright pixels, added before the tonemap (needs r_hdr); 0 = off", 0.0f, 2.0f );
idCVar r_hdrBloomThreshold( "r_hdrBloomThreshold", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "HDR bloom brightness threshold: pixels above this bloom (lower to catch fire/lava directly)", 0.0f, 8.0f );
// HDR eye adaptation / auto-exposure (Phase B1, needs r_hdr + r_hdrTonemap>=1): the scene's
// average (log) luminance drives the tonemap exposure over time. r_hdrExposure becomes the
// exposure at a mid-gray (~0.18) scene; brighter scenes expose down, darker expose up, clamped
// to [min,max] and eased with a time constant ~1/speed seconds. Off = the static r_hdrExposure.
idCVar r_hdrEyeAdaptation( "r_hdrEyeAdaptation", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "HDR auto-exposure / eye adaptation (needs r_hdr + r_hdrTonemap>=1): average scene luminance drives the tonemap exposure over time; off = static r_hdrExposure Always inactive while a cinematic camera is the view (cutscenes keep their authored exposure); it resumes by itself afterwards - this cvar is never changed for that" );
idCVar r_hdrAdaptSpeed( "r_hdrAdaptSpeed", "2.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation speed (higher = faster); the exposure lags with a time constant ~1/speed seconds", 0.1f, 10.0f );
// Relative adaptation (no hard clamp): r_hdrExposure is the neutral "mid" exposure at a reference
// scene luminance (r_hdrAdaptKey). As the scene darkens the exposure rises by up to +Brighten; as
// it brightens it falls by up to -Darken. A smooth tanh rolloff self-limits both, so it eases to
// the bounds instead of pinning. Darken is kept small for the "mildly less bright" look.
idCVar r_hdrAdaptBrighten( "r_hdrAdaptBrighten", "1.22", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation: max exposure ADDED to r_hdrExposure as the scene darkens (dark areas brighten)", 0.0f, 8.0f );
idCVar r_hdrAdaptDarken( "r_hdrAdaptDarken", "0.85", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation: max exposure REMOVED from r_hdrExposure as the scene brightens (bright scenes dim, keep mild)", 0.0f, 8.0f );
// Reference luminance: the geometric-mean scene luminance mapped to r_hdrExposure (neutral). Doom 3
// scenes are dark, so this is low. Raise it to treat brighter scenes as neutral, lower for darker.
idCVar r_hdrAdaptKey( "r_hdrAdaptKey", "0.03", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation reference luminance mapped to r_hdrExposure (the neutral scene brightness)", 0.005f, 1.0f );
// Metering region: fraction of the view width/height averaged for luminance, centred on the screen.
// 1 = whole screen (dark periphery washes out what you look at); lower = center-weighted, so aiming
// at a light or a dark corner actually moves the exposure — the "look at" behaviour.
idCVar r_hdrAdaptCenter( "r_hdrAdaptCenter", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation metering: central fraction of the view measured (1 = whole screen, lower = center-weighted)", 0.15f, 1.0f );
// Low-light grain boost: as the dark-scene brightening ramps up, scale film grain (r_postFilmGrain)
// with it — high-gain sensor/eye noise. 1 = grain (almost) doubles at full brightening; 0 = off.
idCVar r_hdrAdaptGrain( "r_hdrAdaptGrain", "3.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation: extra film grain at full low-light brightening (1 = ~double the grain, 0 = off; needs r_postFilmGrain)", 0.0f, 5.0f );
// Low-light desaturation: as the dark-scene brightening ramps up, wash colours toward gray (scotopic
// vision — rods take over from cones in the dark). 0.25 = colours drop to ~75% at full brightening.
idCVar r_hdrAdaptDesat( "r_hdrAdaptDesat", "0.33", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "eye-adaptation: colour desaturation at full low-light brightening (0.25 = ~75% saturation, 0 = off)", 0.0f, 1.0f );
idCVar r_hdrAdaptWhitePoint( "r_hdrAdaptWhitePoint", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "EXPERIMENTAL (needs r_hdrEyeAdaptation + DUDE tonemap): the DUDE shoulder's white point tracks the scene's (smoothed) brightest metered spot, so a bright lamp/fire core keeps its internal gradient instead of flattening to a white disc when auto-exposure ramps up in the dark. 0 = off (static shoulder); 1 = full tracking. Extend-only (never rolls off harder than static)", 0.0f, 1.0f );
// Debug: draw the adapted exposure as a flat grayscale (exposure * 0.2, so mid-gray ~= 2.5) so you
// can see whether it is pinned or actually tracking the scene. Needs r_hdrEyeAdaptation.
idCVar r_hdrEyeAdaptDebug( "r_hdrEyeAdaptDebug", "0", CVAR_RENDERER | CVAR_BOOL, "debug: show the adapted exposure as a flat grayscale (needs r_hdrEyeAdaptation)" );
idCVar r_fsr( "r_fsr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "AMD FSR2 temporal anti-aliasing (Native-AA; docs/fsr-temporal-pipeline.md R1; Vulkan only). Forces the RGBA16F scene buffer on (FSR2's HDR input), auto-enables motion vectors + sub-pixel jitter, resolves the scene before the HUD, and replaces FXAA/SMAA with FSR2's RCAS. Off = the frame is unchanged" );
idCVar r_fsrSharpness( "r_fsrSharpness", "0.8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "FSR2 RCAS sharpening amount (needs r_fsr): 0 = off, 1 = maximum", 0.0f, 1.0f );
// DUDE FSR2 sub-native upscaling (docs/fsr-temporal-pipeline.md): the primary world view renders
// at this fraction of the display size and FSR2 upscales it. Every screen-space and ray-traced
// pass scales with it; the HUD, film grain and chromatic aberration stay at display resolution.
idCVar r_fsrScale( "r_fsrScale", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "FSR2 render scale (Vulkan, needs r_fsr): the 3D scene renders at this fraction of the display size and FSR2 upscales it to the display. 1.0 = Native-AA (render == display, the shipped path); 0.67 = 'Quality', 0.59 = 'Balanced', 0.5 = 'Performance'. Every screen-space and ray-traced pass scales with it; the HUD, film grain and chromatic aberration stay at display resolution", 0.5f, 1.0f );

// FSR2 auto-reactive mask (R1/D): snapshot the opaque scene at the translucent split, and let
// FSR2 compare it against the final frame — where they diverge (additive particles, muzzle
// flashes, GUI screens, fog) history is trusted less, killing temporal ghost trails.
idCVar r_fsrReactive( "r_fsrReactive", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "FSR2 auto-reactive mask (needs r_fsr): deghosts additive/translucent content (particles, flashes, screens) by comparing an opaque-only snapshot against the final frame; off = plain accumulation everywhere" );
idCVar r_fsrReactiveScale( "r_fsrReactiveScale", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "FSR2 auto-reactive mask strength (needs r_fsr + r_fsrReactive); higher = less ghosting on particles but more shimmer on them", 0.0f, 2.0f );

// DUDE Phase 3.5 "specular tuning" enhancement (GL3/Vulkan interaction shader
// only; inert on the legacy ARB2 path). Defaults reproduce vanilla exactly:
// r_shading 0 keeps the original N.H specular lookup table, r_specularScale 1
// leaves the specular contribution untouched. r_specularExp only applies to the
// analytic Blinn-Phong model. fhDOOM reference. (A third mode, classic Phong
// R.V, was removed 2026-08-01: redundant — Blinn-Phong is both the closer match
// to the vanilla LUT and the better-looking analytic model.)
idCVar r_shading( "r_shading", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "specular shading model: 0 = vanilla lookup table (faithful), 1 = Blinn-Phong", 0, 1 );
idCVar r_specularScale( "r_specularScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "scales the specular contribution (1 = vanilla)", 0.0f, 8.0f );
idCVar r_specularExp( "r_specularExp", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "specular exponent for the analytic shading model (r_shading 1)", 1.0f, 128.0f );

// DUDE PBR materials (docs/pbr-materials.md; GL3/Vulkan interaction shader only,
// inert on the legacy ARB2 path). r_pbr switches the per-light interaction pass to
// an energy-conserving GGX specular with a metalness workflow; while on it
// supersedes r_shading / r_specularScale / r_specularExp (their values are
// preserved for toggling back). Works with r_hdr on or off (HDR recommended for
// the >1 highlight energy). Phase A: global fallback parameters only; the Phase B
// classifier table supplies per-material values.
idCVar r_pbr( "r_pbr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "physically based (GGX/Cook-Torrance) specular + energy-conserving diffuse in the per-light interaction pass. Supersedes r_shading and the specular scale/exponent while on. Non-vanilla; opengl3/Vulkan only" );
idCVar r_pbrRoughness( "r_pbrRoughness", "0.58", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "global fallback roughness for r_pbr until the per-material classifier table exists (docs/pbr-materials.md Phase B). 0.58 matches the Blinn-Phong exponent-16 highlight width via alpha = sqrt(2/(n+2)); in-game A/B put the perceptual match between 0.5 and 0.58", 0.03f, 1.0f );
idCVar r_pbrSpecScale( "r_pbrSpecScale", "1.66", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "artistic energy scale on the r_pbr specular lobe (the PBR path's counterpart to r_specularScale, which it deliberately doesn't read). Dielectric-weighted: fades to 1 as metalness rises. 1.66 is the confirmed in-game perceptual match to the calibrated Blinn-Phong look now that the Toksvig baseline keeps lobes tight (the earlier 3 was calibrated against flattened lobes)", 0.0f, 8.0f );
idCVar r_pbrMetalDiffuse( "r_pbrMetalDiffuse", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how much of a metal's albedo colour survives in the diffuse term. Physical PBR kills diffuse entirely on metals (0), pushing the colour into reflections Doom 3 has no environment to supply, so metals read dark and off-colour; raising this keeps the asset's painted colour while the metallic specular still rides on top (1 = keep all, 0 = physical). A stylized, energy-relaxed metal for the enhancement path (docs/pbr-materials.md sec. 5)", 0.0f, 1.0f );
idCVar r_pbrToksvigBase( "r_pbrToksvigBase", "0.08", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "normal-variance baseline subtracted before the Toksvig specular widening. Low = more anti-firefly widening but softer highlights; high = tighter highlights but white pixel spikes on seams return. 0.08 = calibrated split alongside the 2026-08-01 SSR/reflection retune (docs/pbr-materials.md sec. 4)", 0.0f, 0.6f );
idCVar r_pbrFireflyClamp( "r_pbrFireflyClamp", "12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "upper bound on the GGX specular lobe. Prevents isolated normal-map texels from spiking to clipped white; also the ceiling that skin/tight-roughness highlight cores ride at. Raise for hotter highlight cores (pairs well with r_hdr), lower to flatten", 1.0f, 16.0f );

// DUDE skin / flesh subsurface scattering (docs/skin-shading.md). A per-channel WRAPPED diffuse
// on materials the PBR classifier already tags skin or flesh (no new assets): the lighting
// terminator softens and warms to red, the way light that entered the lit side and scattered a
// short way under the skin leaves in shadow. Kills the "plastic" read faces and Hell flesh get
// under the tight GGX lobe. Diffuse-only, so it works on every shading model (all tiers, not just
// r_pbr) and never touches the specular. Cheap (a few ALU per lit skin pixel); the whole risk is
// taste, so both the strength and the reach are live knobs.
idCVar r_skinShading( "r_skinShading", "0.6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "subsurface-scattering strength on skin/flesh-classified materials: a wrapped diffuse that warms and softens the lighting terminator so faces and Hell flesh stop reading as plastic. Defaults to 0.6 (on); 0 = off (vanilla), 1 = full. Non-vanilla; opengl3/Vulkan. Direct light only; keyed off the PBR classifier categories, so it needs the pbr table (no r_pbr required). Every preset writes it (Potato 0 = faithful floor, Low+ 0.6)", 0.0f, 1.0f );
idCVar r_skinScatter( "r_skinScatter", "0.32", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far light wraps around the terminator on r_skinShading surfaces (the red channel's scatter reach; green/blue derive shorter for the warm falloff). Higher = a softer, redder, wider terminator glow; lower = tighter. Flesh scatters ~30% deeper than face skin. Only active when r_skinShading > 0", 0.0f, 1.0f );

// Per-category PBR defaults (metalness/roughness/wetness/env) moved OUT of cvars into a
// table persisted in the pbr config (docs/pbr-materials.md) — edited via the 8x4 grid in
// the Developer tab / material editor, not individual cvars. r_pbrEnvScale (below) stays
// as the GLOBAL env-glow base the per-category env multiplier rides on top of.
idCVar r_pbrEnvScale( "r_pbrEnvScale", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Phase C.1 metal environment floor: stock Doom 3 has no environment probes, so metals reflect an F0-tinted share of each light's own energy instead of going black where the specular lobe misses. Scales with the light and its shadow, so metals still go dark in darkness. 0 = off (docs/pbr-materials.md sec. 5)", 0.0f, 2.0f );

// DUDE PBR Phase C.2 screen-space reflections (docs/ssr.md). Opt-in; independent of
// r_pbr (the classifier table drives per-pixel reflectivity either way) and of r_hdr.
idCVar r_ssr( "r_ssr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space reflections on glossy/metallic surfaces (GL3 backend). Reflection strength follows the PBR material table: polished floors and bare metal mirror the on-screen scene" );
idCVar r_ssrIntensity( "r_ssrIntensity", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "reflection strength multiplier", 0.0f, 4.0f );
idCVar r_ssrMaxRoughness( "r_ssrMaxRoughness", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness cutoff: surfaces rougher than this reflect nothing (fade starts at r_ssrRoughnessFade of it). Default 0.5 still includes the ceramic floors (roughness 0.45); lower to exclude rougher surfaces. v1 reflections are sharp-only, so keep this low — rough surfaces would show implausible mirror images", 0.02f, 1.0f );
idCVar r_ssrRoughnessFade( "r_ssrRoughnessFade", "0.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "where the reflection starts fading, as a fraction of r_ssrMaxRoughness: full strength up to fade*cutoff, tapering to zero at the cutoff. Default 0.25 gives a wide, gentle fade so a few percent of roughness only makes a small change in reflection strength (easy material authoring); RAISE it (e.g. 0.7) for a sharper edge nearer the cutoff. Applies to SSR and RT reflections", 0.05f, 0.95f );
idCVar r_ssrSteps( "r_ssrSteps", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ray-march samples per pixel; more = longer/cleaner reflections, higher GPU cost (r_gl3GpuTime)", 4, 64 );
idCVar r_ssrFireflyClamp( "r_ssrFireflyClamp", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "cap on the reflected HDR radiance (luminance), to stop a bright reflected light / GUI / specular highlight from showing as a hot speckle 'firefly' on a reflective floor. Lower = fewer/dimmer fireflies (but dimmer bright reflections); higher = brighter reflections but more fireflies; 0 = no clamp", 0.0f, 64.0f );
idCVar r_ssrMaxDistance( "r_ssrMaxDistance", "1024", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "reflection ray reach in world units", 64.0f, 8192.0f );
idCVar r_ssrThickness( "r_ssrThickness", "26", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "assumed surface thickness (world units) when testing ray hits; too low = gaps in reflections, too high = smearing behind edges", 1.0f, 256.0f );
idCVar r_ssrResScale( "r_ssrResScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "resolution the reflection (SSR march + RT trace) runs at, as a fraction of the screen. SNAPPED to an integer divisor - Full, Half (1/2), or Quarter (1/4): fractional ratios (2/3, 3/4) make the low-res reflection upscale beat into strobing/stripes over the sharp RT reflection (docs/rtx-reflections.md RR7). The Fresnel/material weighting stays full-res (ssr_composite), so lowering this only softens the reflected image - Half is ~4x cheaper", 0.25f, 1.0f );
idCVar r_ssrHiZ( "r_ssrHiZ", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space reflections: use a min-Z depth pyramid so the ray leaps provably-empty march span instead of stepping it (dev A/B; exact off = today's full-res march). Modest, scene-dependent win, near-zero on grazing reflective floors" );
idCVar r_ssrHiZLevel( "r_ssrHiZLevel", "4", CVAR_RENDERER | CVAR_INTEGER, "with r_ssrHiZ: coarse mip level the march leaps at (higher = bigger blocks / longer leaps but a coarser nearest-surface bound). Dev tuning knob; 4 measured best on the dev box", 1, 5 );
idCVar r_ssrTemporal( "r_ssrTemporal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "accumulate reflections across frames (reprojected by camera motion) so the march's jitter grain resolves into a clean image. Neighbourhood-clamped to limit ghosting" );
idCVar r_ssrTemporalFeedback( "r_ssrTemporalFeedback", "0.96", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "fraction of reflection history kept per frame with r_ssrTemporal: higher = smoother but slower to react, lower = noisier but snappier. Variance clipping + hit-aware blending keep ghosting bounded even this high", 0.0f, 0.97f );
idCVar r_ssrGlossy( "r_ssrGlossy", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "glossy (roughness-blurred) reflections: build a mip pyramid of the reflection buffer and sample it at a roughness-proportional LOD in the composite, so rougher surfaces blur instead of mirroring sharply. Sharp cutoff still at r_ssrMaxRoughness. Off = the exact sharp path" );
idCVar r_ssrGlossyScale( "r_ssrGlossyScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "with r_ssrGlossy: multiplier on the blur amount (the roughness-driven LOD). 1 = full pyramid reach at the roughness cutoff; lower keeps rough reflections tighter, higher blurs sooner", 0.0f, 1.0f );
idCVar r_ssrGlassProbes( "r_ssrGlassProbes", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "with r_ssr: glass (cube-reflection) surfaces reflect a baked cubemap of the actual room (envprobes/<map>/ under fs_savepath, captured by bakeGlassProbe) instead of Doom 3's generic env/gen* cubemap, so panes mirror the real room at any viewing angle" );
idCVar r_ssrGlassProbeScale( "r_ssrGlassProbeScale", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "brightness of the baked room probe on glass, on top of the stage colour, r_gl3ReflectionScale and the energy normalization to the replaced env/gen* cube. Only applies while a probe is bound (bump-mapped glass ignores it — that shader takes no stage colour, matching vanilla). Default 0.5 from in-game calibration", 0.0f, 4.0f );
idCVar r_ssrGlassProbeBake( "r_ssrGlassProbeBake", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "auto-capture a missing glass probe for the area the player stands in (6 offscreen renders = a one-time hitch per area, cached to disk forever). 0 = only the manual bakeGlassProbe command writes probes" );
idCVar r_ssrGlassProbeSize( "r_ssrGlassProbeSize", "256", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "face resolution of baked glass probes; re-bake with 'bakeGlassProbe force' after changing", 64, 1024 );
idCVar r_specOcclusion( "r_specOcclusion", "0.7", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "specular occlusion on reflections (SSR + RT, docs/rtx-reflections.md RR14): dim a surface's reflection by the ambient-occlusion buffer, roughness-aware (Lagarde) so a near-mirror is barely touched while a rough-glossy surface loses the reflected 'glow' in creases and under props/at contact. 0 = off; 0.7 = the tuned default (user-approved 2026-09-19). Only does anything where reflections (r_ssr) AND ambient occlusion (r_ssao / RTAO) both run this view - inert otherwise, so it follows the presets that enable them (like r_ssrIntensity, it's a global reflection knob, not preset-wired). Reflections only - direct specular from lights is never occluded", 0.0f, 1.0f );

// DUDE screen-space contact shadows (docs/contact-shadows.md; opengl3/Vulkan only). A short depth-buffer
// march from each lit pixel toward the light grounds the small geometry shadow maps are too coarse for -
// pickups, thin props, weapon detail, feet. Opt-in, not preset-wired (a look change, gated by eye). Meant
// for Medium..Nightmare: at Path Tracing r_rtAllLights already traces a real per-light ray, so leave
// this off there. Runs per interaction (cost scales with lights x lit pixels), so keep the step count low.
idCVar r_contactShadows( "r_contactShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space contact shadows: march the depth buffer from each lit pixel toward the light and darken where a nearer occluder is found. Grounds small props/pickups/weapon detail/feet that shadow maps miss. 0 = off. Non-vanilla; opengl3/Vulkan only; automatically disabled while r_rtAllLights is on (Path Tracing's per-light RT rays supersede it)" );
idCVar r_contactShadowStrength( "r_contactShadowStrength", "0.8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "contact-shadow darkening: how much a found occluder dims the light, 0..1 (0 = none, 1 = fully black at the contact). Faded by march distance so a nearer occluder darkens more. Needs r_contactShadows", 0.0f, 1.0f );
idCVar r_contactShadowLength( "r_contactShadowLength", "12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "contact-shadow march length in world units: how far toward the light the depth march reaches (short = tight contact grounding, long = broader but coarser and dearer). Clamped to the distance to the light. Needs r_contactShadows", 1.0f, 128.0f );
idCVar r_contactShadowThickness( "r_contactShadowThickness", "8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "contact-shadow occluder thickness in world units: a depth-buffer sample nearer than the ray by more than this is treated as background, not an occluder (too small = shadows drop out behind thin geometry, too large = haloing/over-occlusion). Needs r_contactShadows", 0.5f, 128.0f );
idCVar r_contactShadowSteps( "r_contactShadowSteps", "8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "contact-shadow march steps per lit pixel (more = smoother/further-reaching, dearer; runs per light so keep it low). Capped at 32 by the shader loop. Needs r_contactShadows", 2, 32 );

// DUDE Phase 3.5 shadow mapping (GL3/Vulkan only; stencil stays the faithful
// default). Global mode for now: 0 = stencil shadow volumes (vanilla), 1 =
// shadow maps for projected/spot lights (point + parallel lights fall back to
// stencil until implemented). See docs/port-phases.md Phase 8.
idCVar r_shadowMapping( "r_shadowMapping", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "shadow technique: 0 = stencil volumes (faithful), 1 = shadow maps where supported" );
idCVar r_shadowMapSize( "r_shadowMapSize", "1024", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "shadow map resolution (per light), power of two", 256, 4096 );
idCVar r_shadowMapBias( "r_shadowMapBias", "0.0002", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for world/perforated receivers (acne suppression). Tuned tight against r_shadowMapNormalOffset, which now carries most of the anti-acne load geometrically", 0.0f, 0.5f );
idCVar r_shadowMapModelBias( "r_shadowMapModelBias", "0.0012", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for model (non-world) receivers; models usually need more. Tuned tight against r_shadowMapNormalOffset", 0.0f, 0.5f );
idCVar r_shadowMapFlashlightBias( "r_shadowMapFlashlightBias", "0.001", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for the player flashlight; its grazing narrow cone needs a much smaller bias than other lights (0.0001-0.005) to avoid peter-panning", 0.0f, 0.5f );
idCVar r_shadowMapSlopeBias( "r_shadowMapSlopeBias", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map slope-scaled bias: grows the depth bias by this * tan(surface-to-light angle) to kill banded acne where light grazes a surface. 0 = flat constant bias (old behaviour)", 0.0f, 8.0f );
idCVar r_shadowMapDebug( "r_shadowMapDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "shadow-map debug: 1 = per-view light classification summary, 2 = also per-light readout (technique, occluder counts, dist/radius)", 0, 2 );
idCVar r_shadowMapCacheDebug( "r_shadowMapCacheDebug", "0", CVAR_RENDERER | CVAR_BOOL, "print a once/sec cube-shadow cache breakdown: hit rate + cube re-renders split by cause (cold = new/evicted light, warm-caster = an occluder moved, warm-light = the light moved), plus scratch/dynamic/deferred/evictions/faces. Dev diagnostic for shadow-cache work" );
idCVar r_shadowMapCachePerFace( "r_shadowMapCachePerFace", "1", CVAR_RENDERER | CVAR_BOOL, "cube-shadow cache: on a warm miss, re-render only the cube faces an occluder actually moved across, instead of all six (fidelity-identical). 0 = re-render the whole cube (old behaviour, for A/B)" );
idCVar r_shadowMapCacheSplit( "r_shadowMapCacheSplit", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "cube-shadow cache: when a moving/animated caster (monster) is near a static point light, cache the world (static) cube and re-render only the movers into a small dynamic cube each frame, sampling min(static, dynamic). Keeps such lights cached instead of bypassing the cache entirely. Fidelity-identical. 0 = old behaviour (whole cube on the scratch path every frame, for A/B)" );
idCVar r_shadowMapSplitDynDrop( "r_shadowMapSplitDynDrop", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "cube-shadow split cache: render the dynamic (movers-only) cube this many resolution tiers below the static cube (0 = same size, 1 = half, 2 = quarter). The movers' cube re-renders every frame, so a smaller one cuts that per-frame fill cost; moving shadows get proportionally softer (PCF kernel and depth bias scale to the coarser texels) while the cached static cube stays full-res. Floors at the smallest pool tier. User-verified at 1 (2026-08-20): mover softness reads as penumbra, static shadows untouched (docs/shadow-research.md, 2026-08 recency check)", 0, 2 );
idCVar r_shadowMapCull( "r_shadowMapCull", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "shadow caster faces: 0 = front, 1 = back (second-depth, less acne), 2 = two-sided", 0, 2 );
idCVar r_shadowMapPerforated( "r_shadowMapPerforated", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "let perforated (alpha-tested) grates/fences cast real punched-out shadow maps even when flagged noShadows (that flag exists only because stencil couldn't perforate)" );
idCVar r_shadowMapViewWeapon( "r_shadowMapViewWeapon", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "whether the first-person view weapon (and arms) casts shadow-map shadows. The view model sits at the player's world position, so under shadow mapping it throws a gun-shaped shadow onto nearby floors/walls (vanilla avoided this with per-material noShadows/noSelfShadow flags, but the chainsaw chain and plasmagun canister aren't flagged and cast anyway). A single shadow map can't self-shadow the weapon without also casting it on the world, so default 0 keeps the whole view model out of the map (no floor blob, no cast self-shadow; the gun still gets normal bump-mapped shading). 1 lets every view-weapon surface cast, even ones flagged noShadows; translucent invisibility skins never cast either way" );
idCVar r_shadowMapPerforatedStrength( "r_shadowMapPerforatedStrength", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how dark perforated-caster (grate/fence) shadows get: 1 = fully dark like opaque casters, 0 = no shadow. Partial values dither the caster out of the shadow map so PCF averages the area toward lit -- softens the huge black regions a fence throws over a room (vanilla faked those with faded light textures)", 0.0f, 1.0f );
idCVar r_shadowMapPointSize( "r_shadowMapPointSize", "1200", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "point-light cube shadow map resolution per face (6 faces); lower than the 2D map since faces cover more", 128, 4096 );
idCVar r_shadowMapCubePcf( "r_shadowMapCubePcf", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "point-light cube shadow PCF taps: 1 = single hardware 2x2 tap (hardest/blockiest edge, cheapest), higher = softer disc-filtered edge at more cost", 1, 16 );
idCVar r_shadowMapPointLimit( "r_shadowMapPointLimit", "64", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "max point lights that get a cube shadow map per view (by on-screen importance); out-of-budget point lights are left unshadowed while r_shadowMapping is on. 0 = all point lights", 0, 128 );
idCVar r_shadowMapSizeScale( "r_shadowMapSizeScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "scale each light's shadow-map resolution with its radius so texel-to-world size (shadow-edge sharpness) stays roughly constant; large lights get more resolution, small lights less" );
idCVar r_shadowMapSizeScaleRadius( "r_shadowMapSizeScaleRadius", "192", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "light radius that maps to the base shadow resolution (r_shadowMapSize / r_shadowMapPointSize); lights larger than this get proportionally more resolution, smaller ones less", 16.0f, 8192.0f );
idCVar r_shadowMapCache( "r_shadowMapCache", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "cache static point-light cube shadow maps across frames; a light is only regenerated when it or one of its shadow casters moves. Huge win in static scenes" );
idCVar r_shadowMapCacheMB( "r_shadowMapCacheMB", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "VRAM budget for the shadow-map (cube) cache, in MB. -1 = auto: the VRAM actually free right now (measured) minus a safety reserve for other growth, so the cache is as large as safely possible and static cubes rarely evict (eviction + re-render is the frame-time cost). Scales to the card instead of a blind fraction. 0 = unlimited. A positive value caps it explicitly", -1, 32768 );
idCVar r_shadowMapBudgetHysteresis( "r_shadowMapBudgetHysteresis", "30", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "point-light shadow budget stickiness (percent). A light that was cube-shadowed in the last few frames keeps this on-screen-size score bonus, so it isn't kicked out of the r_shadowMapPointLimit set by a marginally bigger newcomer. Stops shadows flickering on/off at the budget boundary as the camera turns. 0 = off (rank purely by on-screen size)", 0, 500 );
idCVar r_shadowMapMaxUpdates( "r_shadowMapMaxUpdates", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "max cached cube shadow maps re-rendered per view when a light or a moving rigid caster (door, lift, fan) changes. Excess lights reuse their previous (1-frame-stale) cube this view and refresh on a later one, spreading a burst of updates across frames. Only defers lights that already hold a cached cube; cold and dynamic (animated-caster) lights always render. 0 = unlimited (no staggering)", 0, 128 );
idCVar r_shadowMapBudgetDownscale( "r_shadowMapBudgetDownscale", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "when the point lights in view don't fit the shadow-cache VRAM budget (r_shadowMapCacheMB), halve the cube resolution of the farthest ones (each down to a quarter of its size, never below 256) until they fit: softer distant shadows instead of re-rendering the overflow lights into the scratch cube every frame. A downscaled light only grows back once the budget has clear headroom, so lights don't flip size as you move. Lowers fidelity (distant point-light shadows get softer); 0 = off, every light keeps its full cube resolution" );
idCVar r_shadowMapStencilRadius( "r_shadowMapStencilRadius", "255", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "lights whose largest light_radius axis exceeds this (world units) fall back to Carmack stencil shadows instead of a shadow map. Large 'sun replacement' lights look better as stencil (no cube-map pixelation on distant shadows) and cost no shadow-map VRAM. 0 = every light uses shadow maps", 0.0f, 16384.0f );
idCVar r_shadowMapSkipStencilBuild( "r_shadowMapSkipStencilBuild", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "when a light will be shadow-MAPPED rather than stencil-shadowed, skip building its CPU stencil shadow volume for animated casters (it would never be drawn). The shadow-map caster path still shadows them. A pure CPU front-end win in shadow-mapped scenes; self-heals on r_shadowMapping toggle. 0 = always build volumes (old behaviour, for A/B)" );
// DUDE sun shadow maps (docs/shadow-research.md item 1): oversize-omni / parallel "sun"
// lights get a per-view fitted virtual 2D shadow map instead of Carmack stencil volumes.
idCVar r_shadowMapSun( "r_shadowMapSun", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "oversize 'sun replacement' omni lights and parallel lights render a per-view fitted 2D shadow map instead of falling back to stencil volumes (needs r_shadowMapping). 0 = old stencil fallback" );
idCVar r_shadowMapSunBias( "r_shadowMapSunBias", "0.0008", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "depth-compare bias for sun shadow maps; the sun map's depth unit spans the whole fitted view region, so this is smaller than the per-light biases. Tuned tight against r_shadowMapNormalOffset", 0.0f, 0.1f );
idCVar r_shadowMapSunRange( "r_shadowMapSunRange", "3000", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far ahead of the camera (world units) the sun shadow map covers; bigger = longer shadow reach but coarser texels", 512.0f, 16384.0f );
idCVar r_shadowMapNormalOffset( "r_shadowMapNormalOffset", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "normal-offset shadow bias in shadow texels: the cube and sun shadow lookups sample from a point pushed along the surface normal by about this many texels' world size, killing grazing-angle acne geometrically instead of with a large depth bias. 0 = off", 0.0f, 8.0f );

// DUDE RT sun shadows (docs/rtx-shadow-roadmap.md R3): ray-traced replacement for the sun
// map's LOOKUP - the interaction shader traces one ray per fragment against the persistent
// world acceleration structure (r_rtWorld auto-builds while this is on).
idCVar r_rtSunShadows( "r_rtSunShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced sun shadows (Vulkan + RT hardware): the interaction shader traces one ray at the sun against the persistent world scene instead of sampling the fitted sun map - exact at any distance, no r_shadowMapSunRange cap or map aliasing. Static world casters only until the dynamic BLAS lane lands (monsters/movers cast no sun shadow yet). Rides the sun shadow-map path, so it needs r_shadowMapSun" );
idCVar r_rtSunShadowOffset( "r_rtSunShadowOffset", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT sun shadows: ray-origin offset along the surface normal in world units - the self-intersection guard (the receiving triangle is itself in the acceleration structure)", 0.0f, 16.0f );
idCVar r_rtMonsterShadows( "r_rtMonsterShadows", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT sun shadows: include animated characters (monsters) as casters. Each frame their current-pose CPU-skinned geometry is gathered into a per-frame dynamic acceleration structure. Needs r_rtSunShadows; only this-frame-visible characters cast" );
idCVar r_rtReflections( "r_rtReflections", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Ray-traced reflections (docs/rtx-reflections.md, Vulkan + RT hardware): trace a reflection ray into the scene TLAS for every reflective pixel and shade the hit at the hit point (real per-light diffuse + per-pixel diffuse/emissive material). Since 2026-09-23 RT OWNS all reflective pixels: it runs decoupled from r_ssr (does NOT need it) and, when on, the screen-space SSR march is skipped entirely - one consistent reflection model, no screen-space seam. Needs the animated BLAS for monster reflections (r_rtAnimBlas); additive with the same Fresnel weighting as SSR" );
// H4 RTAO (docs/rtx-rtao.md): ray-traced AO replacing the screen-space GTAO producer at the
// RT tier. The 2026-09-17 profile measured full-res GTAO at ~4.3ms / 40% of the UN frame;
// hemisphere rays + the NRD REBLUR denoiser see off-screen occluders and cost less.
idCVar r_rtao( "r_rtao", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced ambient occlusion (docs/rtx-rtao.md, Vulkan + RT hardware): one cosine hemisphere ray per pixel against the scene TLAS, denoised by NRD ReBLUR, replacing the screen-space GTAO producer. Sees occluders GTAO can't (off-screen, behind camera). Needs r_ssao (the master AO switch); falls back to GTAO without RT support" );
idCVar r_rtaoRadius( "r_rtaoRadius", "260", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RTAO ray length in world units. Longer rays gather broader, more grounded occlusion than GTAO's screen-space radius can. Default 260 (user-tuned)", 8.0f, 512.0f );
idCVar r_rtaoResScale( "r_rtaoResScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "resolution RTAO runs at (rays + the NRD denoiser + the guides), as a fraction of the screen: the denoised occlusion is bilinearly upscaled to full res in the resolve. The NRD denoiser is the bulk of RTAO's cost, so this is what actually saves time (unlike halving the rays alone). AO is low-frequency, so fractional scales work here without the strobing that forces reflections to integer divisors - 0.75 or 0.5 read nearly identical. 1 = full res (unchanged)", 0.25f, 1.0f );
idCVar r_rtaoIntensity( "r_rtaoIntensity", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RTAO strength (power curve on the traced visibility). RTAO's occlusion is already physically grounded, so the neutral 1.0 is the default — unlike r_ssaoIntensity's 1.2, which compensates for the screen-space march and applies only to GTAO tiers", 0.0f, 4.0f );
idCVar r_rtaoHistory( "r_rtaoHistory", "32", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RTAO denoiser history length: max frames NRD's ReBLUR accumulates for the 1-ray/px occlusion. 32 is the tuned default (clean with the least blur; ~0.1 ms at full res). 0 = NRD's own default (~30). Raise for smoother/less noise at the cost of more lag/trailing on moving occluders; lower (e.g. 12-20) for snappier-but-noisier. Mirrors r_rtgiHistory for the RTAO path", 0, 63 );
idCVar r_rtaoHistoryMs( "r_rtaoHistoryMs", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RTAO denoiser history in MILLISECONDS instead of frames (R0, docs/rtx-nrd.md N0): >0 converts to frames every frame from a 0.2 s low-passed frame rate (NRD GetMaxAccumulatedFrameNum), so the accumulation window stays the same TIME at 60 or 144 Hz (45 frames = 0.75 s vs 0.31 s); the fast history scales with it. 0 = r_rtaoHistory in frames, as before", 0.0f, 2000.0f );
idCVar r_rtaoFastClamp( "r_rtaoFastClamp", "1.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RTAO denoiser: NRD ReBLUR fastHistoryClampingSigmaScale [1;3] - how tightly the slow main history is clamped to the responsive fast history. 1.2 = the tuned default (near NRD's '1.1 is a safe value for occlusion denoising', snappier on moving occluders - NPCs, doors); 2.0 = NRD's own default. Lower = snappier, slightly noisier", 1.0f, 3.0f );
idCVar r_rtaoHistoryFix( "r_rtaoHistoryFix", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RTAO denoiser: NRD ReBLUR historyFixFrameNum - frames of strong spatial reconstruction after a history reset (disocclusion). 3 = NRD default (today); the NRD sample uses 2 for occlusion. Must stay below the fast history (6)", 0, 5 );
idCVar r_rtaoDebug( "r_rtaoDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "RTAO debug view: 0 = off, 1 = raw noisy ray output, 2 = denoised (pre-composite). The final composited AO shows in r_ssaoDebug 1 as always", 0, 2 );
// H6 RT GI (docs/rtx-rtgi.md): one-bounce ray-traced global illumination on the Path Tracing tier. Extends
// the RTAO hemisphere ray from "am I occluded" to "gather the incoming radiance at the hit" (bindless albedo *
// scene lights + emissive). Replaces RTAO when on (AO = this gather with radiance 0). H6a is a debug bring-up:
// nothing consumes it in play yet (NRD denoise = H6b, ambient apply + fake-fill reconcile = H6c), so r_rtgi 0
// leaves normal play + the RTAO path byte-identical. VK + RT hardware only; implies the ray-tracing scene.
idCVar r_rtgi( "r_rtgi", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced one-bounce global illumination (docs/rtx-rtgi.md, Vulkan + RT hardware): one cosine hemisphere ray per pixel gathers the incoming radiance at its hit (the hit shaded by the scene's real lights + emissive), denoised and added as indirect diffuse. The H6 bridge toward Full RT; replaces RTAO at the RT tier (AO is this gather with radiance 0). NON-VANILLA LOOK: adds bounce light stock never had - off by default, turned on by the top tiers. Implies the RT scene; needs r_hdr" );
idCVar r_rtgiRadius( "r_rtgiRadius", "1440", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI gather ray length in world units: how far a bounce ray looks for the surface whose light it gathers. Longer = broader colour bleed and more distant bounce (room-scale, so larger than r_rtaoRadius's contact-occlusion range); shorter concentrates GI on nearby surfaces and is cheaper on traversal", 8.0f, 4096.0f );
idCVar r_rtgiResScale( "r_rtgiResScale", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "resolution the RT GI gather (and, from H6b, its NRD denoise) runs at, as a fraction of the screen - the result is bilinearly upscaled. Bounce light is very low-frequency, so this is nearly free of visible quality but the single biggest cost lever: a full-res single-sample gather costs ~1/scale^2 this. 0.75 = default (3/4 res; checkerboard then traces half of those), 0.5 = quarter the rays, 1 = full res", 0.25f, 1.0f );
idCVar r_rtgiCheckerboard( "r_rtgiCheckerboard", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI: trace a horizontal CHECKERBOARD (half the pixels, alternating each frame) at FULL resolution and let NRD reconstruct the rest, instead of the r_rtgiResScale uniform downscale. Half-res scaling blurs silhouette edges (coarse guides + bilinear upscale); checkerboard keeps sharper edges at ~half the rays. Composes with r_rtgiResScale (which still sets the RTGI resolution; checkerboard traces half of those pixels), so the sharp-edge win is greatest at resScale 1. Measured ~1.3 ms/frame saved at 1440p on a 3080 Ti (the ray pass ~halves) for no visible quality change - on by default. The history-confidence pass still runs in this mode (it evaluates only the traced cells). VK + RT only" );
idCVar r_rtgiHistory( "r_rtgiHistory", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT GI denoiser history length: max frames the NRD ReBLUR pass accumulates for the GI radiance (GI path only; RTAO is untouched). The 1-spp half-res gather needs temporal accumulation to look smooth: too short and single pixels 'fry' (noise/flicker), too long and a switched-off light or a stale bounce lingers longer before it fades. ~12 is the balance; raise for smoother-but-laggier GI, lower for snappier-but-noisier. NRD's own default is 30", 2, 63 );
idCVar r_rtgiHistoryMs( "r_rtgiHistoryMs", "192", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI denoiser history in MILLISECONDS instead of frames (R0): >0 converts to frames from a 0.2 s low-passed frame rate so the window is the same TIME at any frame rate; the fast history scales with it. 0 = r_rtgiHistory in frames, as before", 0.0f, 2000.0f );
idCVar r_rtgiFastClamp( "r_rtgiFastClamp", "1.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI denoiser: NRD ReBLUR fastHistoryClampingSigmaScale [1;3]. 2.0 = NRD default (today); the NRD sample ships 1.5 ('works well even for dirty signals'). Lower = less ghosting, more flicker on the 1-spp bounce", 1.0f, 3.0f );
idCVar r_rtgiConfidence( "r_rtgiConfidence", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI denoiser history-confidence sensitivity (NRD IN_DIFF_CONFIDENCE): a per-pixel pass compares this frame's bounce luminance against last frame's denoised result reprojected by the motion vectors, and where they disagree (a lit silhouette moving under the camera, a light toggling) it tells NRD to drop history THERE only - killing edge ghosting/trailing without the flat-area dimming a global antilag causes. Higher = more aggressive rejection (sharper edges, more noise on them); 0 = off (NRD's built-in antilag only). GI path only", 0.0f, 16.0f );
idCVar r_rtgiLightScale( "r_rtgiLightScale", "1.6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI: overall brightness of the diffuse lighting evaluated at a bounce hit (the same per-light point/spot model RT reflections use). Raise for stronger colour bleed, lower if bounced light overpowers the direct lighting", 0.0f, 8.0f );
idCVar r_rtgiEmissive( "r_rtgiEmissive", "1.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI: how brightly a self-illuminated surface (neon tube, ceiling light tile - materials with an additive glow stage) contributes when a bounce ray hits it, so glowing sources bleed their colour into the room. 0 = self-lit surfaces bounce nothing", 0.0f, 16.0f );
idCVar r_rtgiBilateralUpscale( "r_rtgiBilateralUpscale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI: upscale the reduced-resolution GI to full res with a DEPTH+NORMAL-aware (joint bilateral) filter instead of a plain bilinear stretch. Reweights the 2x2 GI footprint by how well each low-res texel's surface matches the full-res pixel, so the bounce stops bleeding across silhouettes and creases - sharper GI at edges for a handful of extra taps in the apply. 0 = plain bilinear (the old look). GI path only" );
idCVar r_rtgiEmissiveClamp( "r_rtgiEmissiveClamp", "0.15", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI: ceiling on a single bounce ray's emissive contribution (peak channel, hue preserved) BEFORE the NRD denoise. Small bright emitters are the worst input for the 1-spp gather - a ray either hits one (big spike) or misses (zero), and that hit/miss variance denoises into grain/colour blotches. Clamping the peak cuts the variance so NRD resolves it cleanly, trading a little emissive brightness for a lot less noise. Lowered 0.38 -> 0.15 (user, 2026-10-18): a bounce ray hitting a STROBING light fixture's bright emissive is the worst case (bright + flickering + 1-spp = a sparse 'raindrop' scatter the denoiser can't merge, worst at low FSR resolution); 0.15 tames it without over-dimming neon / screens / glow-feature bounces. Lower = cleaner/dimmer glow bleed, higher = brighter/noisier; set high (e.g. 16) to effectively disable the clamp", 0.0f, 16.0f );
idCVar r_rtgiShadow( "r_rtgiShadow", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI (docs/rtx-rtgi.md H6d): trace one shadow ray at each bounce hit toward the DOMINANT (brightest) reaching light, so a light behind a thin wall stops leaking indirect light through it. One extra ray per gather ray regardless of how many lights reach the hit (only the brightest is tested, not full next-event estimation); if it is occluded its contribution is dropped. 0 = unshadowed bounce (lights leak through walls, a little cheaper)" );
idCVar r_rtgiDebug( "r_rtgiDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "RT GI debug view: 0 = off (GI composited into the scene), 1 = the raw noisy radiance gather, 2 = the NRD-denoised GI, blitted over the scene (YCoCg-decoded). Needs r_rtgi 1; the composite into the scene is suppressed while this is on", 0, 2 );
idCVar r_rtgiIntensity( "r_rtgiIntensity", "1.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI apply strength (docs/rtx-rtgi.md H6c): multiplier on the bounced radiance (albedo * gathered irradiance) added into the scene. 1 = the one-bounce irradiance as-is; raise for punchier colour bleed, lower for a subtle fill", 0.0f, 8.0f );
idCVar r_rtgiFogAttenuate( "r_rtgiFogAttenuate", "3.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI reconcile: fade the added bounce light out with distance through the view's fog so GI stops washing out Doom 3's distance fog. The GI apply is ADDITIVE and runs BEFORE the fog blend, so without this a far surface buried in fog keeps its full bounce and the fog veil reads thin. Fades GI by the fog's own distance ramp (transmittance T, opaque at the fog light's distance), raised to this power: 0 = off (GI ignores fog, the old look); 1 = physical (one transmittance); >1 overdrives the falloff so GI dies sooner than the fog thickens - useful when a map's fog distance is huge and strength 1 is too subtle. Only affects the GI apply, and only in views that have a fog light", 0.0f, 4.0f );
// RT irradiance cache (docs/rtx-irradiance-cache.md): multi-bounce GI for the RT GI bounce. VK + RT hardware; default OFF
// (off = bit-identical). Needs r_rtgi (the bounce it feeds and reads). Reconfiguring: r_rtgiCacheSlotsLog2 (buffer size).
idCVar r_rtgiCache( "r_rtgiCache", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI irradiance cache (docs/rtx-irradiance-cache.md, Vulkan + RT hardware, needs r_rtgi): a world-space, camera-adaptive hashed cache of the INDIRECT light every surface receives, learned from the GI bounce at each pixel and read back where a bounce ray lands - so a bounce hit is lit by the light bouncing onto IT as well as by the lights shining on it directly. Closes the recursion: over a few frames the one-bounce GI becomes all orders of diffuse bounce (corridor side walls, undersides, ceilings lit from below stop being black), and the bounce signal the denoiser sees is smoother. NON-VANILLA LOOK (as r_rtgi); 0 = the one-bounce GI as before (bit-identical)" );
idCVar r_rtgiCacheScale( "r_rtgiCacheScale", "1.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI irradiance cache: weight of the cached (second and later bounce) light added at a bounce hit. 1 = physical; lower for a subtler multi-bounce fill, higher to exaggerate", 0.0f, 4.0f );
idCVar r_rtgiCacheTail( "r_rtgiCacheTail", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI irradiance cache mode, only while ReSTIR DI lights the scene (dude_rtx 2): 1 = TAIL - a cell stores the surface's whole OUTGOING radiance and a converged cell IS the bounce's path tail, so a bounce hit reads the cached value (plus its own emissive) and skips its light loop + shadow ray - the gather is cheaper and the bounce the denoiser sees is cell + frame averaged (far less 1-spp noise) rather than a single direct shade; 0 = INDIRECT-ONLY (the C2 A/B) - the cell stores only the second-and-later-bounce light added on top of the hit's full live shade. A change clears and re-learns the cache" );
idCVar r_rtgiCacheSlotsLog2( "r_rtgiCacheSlotsLog2", "21", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT GI irradiance cache: log2 of the cache cell count - 20 = 1M cells = 2 x 20 MB of VRAM (cells + accumulation, each with a 4 B variance sidecar), 19 = half, 22 = 4x. More cells = fewer collisions / dropped updates in large open areas; a change re-allocates and clears the cache", 19, 22 );
idCVar r_rtgiCacheHistory( "r_rtgiCacheHistory", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT GI irradiance cache: frames of memory per cell (Nmax) when r_rtgiCacheHistoryMs is 0. A cell blends each frame's samples in with weight max( 1/N, 0.02 ), so this caps how long a switched or swinging light's OLD indirect contribution can linger (about this many frames) against how smooth the cached term is. SHARC-style caches use 128-256; Doom's switched / swinging lights want less", 1, 255 );
idCVar r_rtgiCacheHistoryMs( "r_rtgiCacheHistoryMs", "420", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI irradiance cache: the cell history in MILLISECONDS - converted to frames from a 0.2 s low-passed frame rate, so a cell remembers the same TIME at 60 or 144 Hz (the eviction age follows it). Doom's light tables and swings are authored in time, so this is the knob to tune: lower = a switched / swinging light's old indirect light fades sooner, higher = a smoother cached term. 0 = use r_rtgiCacheHistory in frames", 0.0f, 5000.0f );
idCVar r_rtgiCacheCellAngle( "r_rtgiCacheCellAngle", "0.0027", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI irradiance cache: tan( alpha / 2 ) - the cache cell width per unit of distance from the eye (cell width = 2 x this x distance; 0.004 is ~10 screen pixels at 1440p). Smaller cells = sharper indirect light and fewer thin-wall leaks but slower convergence and more cells; larger = smoother and faster but blurrier, and same-facing surfaces on both sides of a thin wall share a cell sooner", 0.0005f, 0.05f );
idCVar r_rtgiCacheDebug( "r_rtgiCacheDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "RT GI irradiance cache debug overlay over the finished view: 0 = off, 1 = CELL colouring (every cache cell its own tint; dim = not in the cache yet) - proves the keying, the distance levels and the normal bins; 2 = the CACHED indirect value per cell (tonemapped); 3 = cell AGE (frames since its last update, white = evictable, dark red = absent); 4 = cell HISTORY length N over Nmax (black = absent, 15% grey = just created, white = converged); 5 = cell ACTIVITY (red = reset this frame by the DI gradient / an eviction, green = created or seeded from its coarser parent this frame - newly visible, or re-cut at a finer level as you approach -, grey = steady, black = absent; a static scene must settle to all grey); 6 = SERVABLE (tail mode: white where the cell is converged and fresh enough to serve a bounce hit, so a static view goes white in ~1 s). Needs r_rtgiCache 1", 0, 6 );
idCVar r_rtgiCacheReset( "r_rtgiCacheReset", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT GI irradiance cache: let the ReSTIR DI gradient restart a cell's history where the direct light changed by more than 35% (a switched / swinging light), so the cached indirect term follows at the DI's own speed instead of lingering for the history length. 0 = cells only ever change through the history blend. Diagnostic A/B: if the cell activity view (r_rtgiCacheDebug 5) keeps flashing red in a static scene, compare 0 and 1" );
idCVar r_rtgiCacheServeError( "r_rtgiCacheServeError", "0.33", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT GI irradiance cache, TAIL mode (dude_rtx 2) only: how a cell decides it is converged enough to SERVE a bounce hit (its stored outgoing radiance replaces the live shade). > 0 = serve by VARIANCE: only once the mean's relative standard error - sqrt( (mean of L^2 - mean L^2) / N ) / mean - is within this fraction, so a cell that has aged in but is still noisy keeps shading live until it settles. 0.33 (default) is the tuned sweet spot (~0.2 ms of gather over the age gate, for a cleaner serve-in); lower = stricter (fewer, cleaner serves), higher = looser. 0 = serve by AGE instead: as soon as the cell has >= 4 frames of history - the original C6 behaviour, kept for the A/B. Debug view 6 (r_rtgiCacheDebug 6) shows which cells pass. The whole cache is still off (bit-identical) unless r_rtgiCache is on", 0.0f, 1.0f );
// RT world-space light grid (docs/rtx-light-grid.md): a ReGIR candidate grid that replaces the DI gather's exhaustive
// per-pixel RIS over every light with a small per-cell importance-sampled set - a PERFORMANCE feature. Reconfiguring
// (buffer size): r_rtLightGridSlots / Res / Cascades (join ComputeGfxCfgSignature).
idCVar r_rtLightGrid( "r_rtLightGrid", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT light grid (docs/rtx-light-grid.md, Vulkan + RT hardware, dude_rtx 2): a world-space grid of camera-centred cells, each filled once per frame with a few IMPORTANCE-sampled light candidates. The ReSTIR DI gather and the GI bounce then pick from their cell's candidates instead of weighing EVERY scene light per pixel - the gather's biggest cost at O(lights)/pixel. A perf win that grows with the light count; unbiased (same brightness as exhaustive, a little noisier, cleaned by the denoiser). 0 = off (exhaustive / uniform RIS as before, bit-identical)" );
idCVar r_rtLightGridSlots( "r_rtLightGridSlots", "32", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT light grid: light candidates stored per cell. More = less candidate noise, closer to exhaustive (a cell with more lights that matter than slots drops the dimmest), but a bigger grid buffer and a dearer gather. A change re-allocates the grid. Emissive mode uses r_rtEmissiveGridSlots instead when that is larger", 1, 32 );
idCVar r_rtLightGridRes( "r_rtLightGridRes", "28", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT light grid: cells per axis in each cascade (the grid is res^3 x cascades cells). Higher = finer candidate sets (sharper where lights change over short distances) but more cells to fill and store. A change re-allocates the grid", 8, 64 );
idCVar r_rtLightGridCascades( "r_rtLightGridCascades", "4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "RT light grid: number of camera-centred cascades (each twice the cell size and reach of the last). More = the grid reaches further from the camera before a point falls back to the old RIS path. At the default 12-unit cell base the 4 cascades reach about +-192 / 384 / 768 / 1536 units, which covers a Doom area (rarely over 2000 across). A change re-allocates the grid", 1, 4 );
idCVar r_rtLightGridCellBase( "r_rtLightGridCellBase", "12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT light grid: cascade-0 cell size in world units (each further cascade doubles it). Smaller = finer candidate sets near the camera AND the near cascade covers less, so more of a room spills into the farther cascades (the nesting you see in the debug view) - at the cost of less total reach for the same cascade count, and the grid re-centres in smaller steps as you move. 12 keeps cascade-0 at +-192 units (a room, not the whole level) with +-1536 reach; raise toward 16+ for more reach, lower toward 8 for finer near cells and more cascade use. Read per frame, no re-allocation", 8.0f, 512.0f );
idCVar r_rtLightGridRankLinear( "r_rtLightGridRankLinear", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT light grid: rank a cell's candidates by the light's OWN falloff - colour x (1 - distance / radius), what a Doom light actually delivers at the cell - instead of the inverse-square proxy colour x radius^2 / distance^2, which overrates a light near its rim by an order of magnitude and lets big ambience lights hog the slots of every cell near their edge (a room's own fixtures were evicted in emissive mode, 2026-10-20). 0 = the inverse-square rank. Always off while r_rtEmissiveLighting is on (the inverse-square rank keeps a room's fixtures in their cells there)", 0, 1 );
idCVar r_rtLightGridJitter( "r_rtLightGridJitter", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT light grid: each pixel (and each GI bounce hit) looks its candidates up at a position jittered by up to half a cell, so neighbouring pixels read neighbouring cells. A cell holds only r_rtLightGridSlots candidates; a light that one cell keeps and the next drops then shows as per-pixel noise the denoiser averages instead of a cell-shaped step (emissive mode's many emitters made the steps visible, 2026-10-20). RTXDI's standard ReGIR jitter. 0 = exact cell lookup", 0, 1 );
idCVar r_rtLightGridDebug( "r_rtLightGridDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "RT light grid debug overlay over the finished view (needs r_rtLightGrid 1): 0 = off, 1 = CASCADE (each cascade its own tint, dark = beyond the grid) - shows the cascades nesting around you, 2 = candidate COUNT per cell (black = empty, white = full, RED = OVERFLOW: more in-range lights than slots -> some dropped / fell back, white = full), 3 = the cell's DOMINANT stored light (a stable per-light tint) - neighbouring cells should agree, 4 = cell IMPORTANCE (sumImp, tonemapped)", 0, 4 );
// RT emissive triangle table (docs/rtx-emissive-triangles.md E1): the data foundation of the experimental emissive-
// lighting mode (docs/rtx-experimental-lighting.md). A GPU table of every emissive (additive-stage) triangle in the
// loaded map - including the pure light-source surfaces (neon tubes, light tiles) the RT caster set excludes - with
// the flux + geometry an area-light sampler needs, plus a flux CDF. E1 builds the table only; nothing consumes it
// until E2, so off = bit-identical and on = bit-identical too (only the debug overlay shows it). VK + RT hardware.
idCVar r_rtEmissiveTable( "r_rtEmissiveTable", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT emissive triangle table (docs/rtx-emissive-triangles.md, Vulkan + RT hardware): at level load, gather every triangle of the map's self-illuminated surfaces (materials with an additive glow stage: neon tubes, light tiles, LED panels, lava) - world and static props, including the pure light-source surfaces that cast no shadow and so are not in the ray-tracing scene - into a GPU table with each triangle's world position, emissive texture, and FLUX (area x mean glow brightness x r_rtEmissiveScale), plus a flux-ordered CDF for area-light sampling. The foundation of the experimental emissive-lighting mode: nothing consumes it yet (E2), so the picture is unchanged either way - use r_rtEmissiveTableDebug to see it. Screens (GUI / video surfaces) are left out: they keep the fill lights of r_emissiveSurfaces" );
idCVar r_rtEmissiveTableDebug( "r_rtEmissiveTableDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "RT emissive table debug overlay (needs r_rtEmissiveTable 1): draws the table's triangles over the view, depth-tested against the scene, and writes each painted surface's MATERIAL NAME over it (nearest 48, outlined). 0 = off, 1 = a stable tint per emissive TEXTURE (which surfaces are in the table, and which material each belongs to), 2 = per-triangle FLUX relative to the table's mean (blue = faint, green = average, red/white = the strongest emitters), 3 = the EMITTED COLOUR the table would light the scene with (glow texture at the triangle's st x stage colour x r_rtEmissiveScale; the colour an E2 sample would return - so a bright white screen here that should be dim is a calibration finding), 4 = LIGHT COVERAGE: green = inside a placed light's volume (that light is fixtured: dropped once the emitters light the scene), red = no placed light covers it, 5 = the PLACED LIGHTS as spheres coloured by the census: red = fixtured (an emitter in its volume: dropped in emissive mode), green = ambience (kept), yellow = parallel, blue = fog / blend", 0, 5 );
idCVar r_rtEmissiveHideFlares( "r_rtEmissiveHideFlares", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "emitter mode (docs/rtx-experimental-lighting.md): do not draw the FLARE sprites that sit on light fixtures which are emitters in the RT emissive table (a light entity's flare model: a camera-facing additive sprite within 64 units of an emissive triangle). The flare is a painted stand-in for glare; once the fixture itself is the light it only adds a second, fake highlight. Needs r_rtEmissiveTable. 0 = flares drawn as in vanilla" );
idCVar r_rtEmissiveFlareInfer( "r_rtEmissiveFlareInfer", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT emissive table: INFER emitters from flares. A light fixture whose material has no glow stage (its 'tube' is a white patch in the diffuse map lit by the placed light - the EnPro door frames) is found through the flare sprite the mapper put on it: the nearest opaque surface under a flare that has a diffuse map and no additive stage becomes an emitter whose diffuse texels brighter than 60% emit. Marked 'flare-inferred' in the census / overlay. 0 = only materials with a real glow stage are emitters" );
// EMISSIVE MODE (docs/rtx-experimental-lighting.md D6 / E2): the one master switch. Emitters (the RT emissive table) light
// the scene as area lights: the ReSTIR DI gather draws emissive-triangle candidates next to the scene lights.
idCVar r_rtEmissiveLighting( "r_rtEmissiveLighting", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "EMISSIVE MODE (docs/rtx-experimental-lighting.md, Vulkan + RT hardware, dude_rtx 2 / ReSTIR DI): the map's self-illuminated surfaces (neon tubes, light tiles, LED panels, lava, screens' bezels - the RT emissive table) LIGHT the scene as real area lights: every emitting surface is a row of the scene-light buffer next to the placed lights (the light grid, the reservoir, the denoiser's directional data and the GI bounce treat it as one more light), shaded by a sample point on its real shape with physical falloff and one visibility ray through the real geometry (grilles shadow for real); the placed lights whose fixture it is are dropped. Implies r_rtEmissiveTable and hides the fixtures' flare sprites. The master opt-in / opt-out of the whole mode; 0 = bit-identical to today's picture" );
idCVar r_rtEmissiveIntensity( "r_rtEmissiveIntensity", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: how bright the emitters light the scene, relative to the placed lights (a plain multiplier on every emitter's radiance; the per-material intensity table refines it later). The placed lights use Doom's linear falloff and the emitters an inverse-square one, so there is no exact equivalence - tune by eye on a lit corridor", 0.0f, 64.0f );
idCVar r_rtEmissiveBounce( "r_rtEmissiveBounce", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "The map's glowing surfaces light the GI bounce's hit points like any light (they are rows of the scene-light buffer the bounce walks). Works in BOTH modes: with Emissive Mode off the placed lights keep the direct term and the emitters only feed the GI (bounce-only rows; needs r_rtEmissiveTable 1, Path Tracing tier); with it on, walls lit only by emitters bounce. 0 = off (the experimental emissive feature ships OFF in 0.9.6)", 0, 1 );
idCVar r_rtEmissivePowerMatch( "r_rtEmissivePowerMatch", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Emissive mode: an emitter inherits the POWER of the placed light it replaces (the E5 census pairs them): its radiance is set from that light's colour and radius and the emitter's own area, so a small fixture lights its room as its light did and a big panel glows softly. An emitter inside no placed light is matched to a light of its own colour at r_rtEmissiveReachDefault. With this on r_rtEmissiveIntensity is a trim around 1. 0 = every emitter lights at its authored glow brightness x the intensity (small fixtures go dark)", 0, 1 );
idCVar r_rtEmissiveGridSlots( "r_rtEmissiveGridSlots", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Emissive mode: light-grid candidates per cell while the emitters light the scene (the larger of this and r_rtLightGridSlots). Emissive mode puts more lights in range of a cell than the placed lights did - a fixture with two emitting surfaces is two rows, an uncovered emitter reaches r_rtEmissiveReachDefault - and a cell that cannot hold them drops the dimmest: a fixture's own room goes dark in cell-shaped patches. More = dearer gather. A change re-allocates the grid", 1, 32 );
idCVar r_rtEmissivePhysical( "r_rtEmissivePhysical", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Emissive mode: PHYSICAL emitters (docs/rtx-experimental-lighting.md). A fixture lights with its radiance and the inverse square only: no reach, no boost, no fade at a radius - its row ends where its contribution drops below r_rtEmissiveCullIrradiance. The radiance comes from emitters.cfg (the per-material table the editor writes; rtEmissiveBakeTable seeds it from the power match) or, for a material without an entry, from the power match. Pairs with r_rtEmissiveAutoExposure: physical light has far more range than Doom's hand-lit maps. 0 = the compatibility layer (reach, knee, fade: the placed lights' look)", 0, 1 );
idCVar r_rtEmissiveCullIrradiance( "r_rtEmissiveCullIrradiance", "0.003", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Physical emitters: the irradiance (radiance x area / distance^2) below which an emitter no longer counts - sets each row's cull radius sqrt( L x A / this ). Lower = farther reach, more candidates per light-grid cell", 0.0001f, 1.0f );
idCVar r_rtEmissiveAutoExposure( "r_rtEmissiveAutoExposure", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Physical emitters: switch the HDR eye adaptation on (r_hdrEyeAdaptation's own range / speed / key / metering knobs apply) while Emissive Mode is on with r_rtEmissivePhysical - physical light has far more dynamic range than the placed lights the maps were lit for", 0, 1 );
idCVar r_rtEmissiveCheapTarget( "r_rtEmissiveCheapTarget", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Emissive mode: the ReSTIR DI gather weighs each EMITTER candidate with an analytic estimate (its row's power over distance squared, the reach boost and fade, the receiver cosine) instead of area-sampling every candidate, and area-samples only the chosen one. Unbiased (RIS with any positive target); the per-pixel noise can differ. On by default (user, 2026-10-27: with r_rtEmissiveSamples 2 it makes emissive mode playable). 0 = every emitter candidate is area-sampled r_rtEmissiveSamples times (the original)" );
idCVar r_rtEmissiveSamples( "r_rtEmissiveSamples", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Emissive mode: area-light samples averaged per emitter per pixel in the ReSTIR DI shade. A placed light is a point the reservoir picks reliably; an emitter spreads its energy, so at distance a single sample is rarely the winner and the denoiser clamps the sparse spikes - the fixture reads far dimmer than the placed light it replaced. More samples smooth the emitter's radiance so it survives the denoiser at range (1 = the old single sample; dearer gather: each sample is ~1.75 ms of gather on mars_city1 at 1440p, and 1 / 4 / 10 looked the same to the user on 2026-10-27; 2 is the default, the user's choice)", 1, 16 );
idCVar r_rtEmissiveMaxRows( "r_rtEmissiveMaxRows", "4096", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Emissive mode: how many emitting FIXTURES a view's scene-light buffer may carry as light rows (docs/rtx-experimental-lighting.md E2 v3). Each fixture of the RT emissive table becomes one light row next to the placed lights; the light grid ranks them per cell like any light, so the per-pixel cost does not grow with this. Beyond the cap the strongest at the eye win (the console line 'emissive mode: ...' says CAP HIT). 0 = no emitter lights", 0, 4096 );
idCVar r_rtEmissiveFixtureMinArea( "r_rtEmissiveFixtureMinArea", "32", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: the EFFECTIVE emitting area (world units^2 of lit texels x their brightness x the stage colour - an 8 x 8 patch at half brightness = 32) a surface needs to count as a light FIXTURE at all. Below it the glow lights at its authored brightness only; past it a power-matched patch burns bright enough to light its own housing at point-blank range (the electric box, 2026-10-20), so indicators must stay below. A fixture drops the placed light it sits in and inherits (part of) its power; a smaller glow - a computer LED, the electric box's red lamp, a button - is an INDICATOR: it never drops a light and lights at its authored brightness only. A change rebuilds the census", 0.0f, 4096.0f );
idCVar r_rtEmissiveFixtureFullArea( "r_rtEmissiveFixtureFullArea", "64", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: the effective emitting area (lit area x brightness) at which an emitter inherits its placed light's FULL power (a white 8 x 8 patch, a tube). Between r_rtEmissiveFixtureMinArea and this the inherited share ramps up linearly, so a dim or tiny glow stands in for only a fraction of its light", 1.0f, 4096.0f );
idCVar r_rtEmissiveMaxRadiance( "r_rtEmissiveMaxRadiance", "64", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: the brightest radiance the power match may give one emitter (a small patch replacing a big light would otherwise blaze). 64 = roughly a bare tube", 1.0f, 4096.0f );
idCVar r_rtEmissiveReach( "r_rtEmissiveReach", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: how far an emitter's light REACHES, as a multiple of the authored radius of the placed light it replaced (the census knows which fixture each emitter sits in). A Doom 3 light falls off linearly out to its radius, an area emitter with the inverse square - right beside the fixture, dark on the far wall. With reach, past 1/4 of the inherited radius the falloff relaxes from inverse-square toward Doom's linear law, so a hall fixture lights the hall it was authored for and a small panel stays local. 1 = the authored radius, 2 = twice as far, 0 = pure physics (inverse square everywhere)", 0.0f, 8.0f );
idCVar r_rtEmissiveReachDefault( "r_rtEmissiveReachDefault", "300", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Emissive mode: the reach (world units) of an emitter that sits inside NO placed light (nothing to inherit from): a glow panel in a dark corner. 0 = pure physics for those", 0.0f, 4096.0f );
idCVar r_rtEmissiveScale( "r_rtEmissiveScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RT emissive table: global multiplier on every emissive triangle's flux and emitted colour (a stand-in for the per-material intensity table of the experimental-lighting plan's D2). A change rebuilds the table", 0.0f, 64.0f );
idCVar r_rtReflTemporal( "r_rtReflTemporal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RR6: accumulate the RT reflection (r_rtReflections) across frames through the same temporal pipeline SSR uses (reprojected by camera + object motion, neighbourhood-clamped), and UPSCALE it: the ray-query trace stays at the cheap r_ssrResScale, but the history is full-res, so the sharp reflection is reconstructed at full resolution instead of bilinear-upsampled. Fixes both the firefly grid (where SSR persistently misses) and the fractional-resolution 'striping'. 0 = the pre-RR6 look (one sharp ray composited straight onto the scene)" );
idCVar r_rtReflJitter( "r_rtReflJitter", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR6c: scale of the per-frame RT-reflection sample-grid jitter (needs r_rtReflTemporal), in trace-buffer texels. 1 = ±0.5 texel = exactly one interpolation cycle, so the fractional-upscale 'stripes' average out in the full-res history. Higher = more sub-pixel coverage (softer); lower = sharper but more residual stripe/grid. 0 = no jitter (no upscale reconstruction, stripes return)", 0.0f, 4.0f );
idCVar r_rtReflBump( "r_rtReflBump", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR10: how much the RT reflection follows the surface's NORMAL MAP (0 = flat geometric mirror; 1 = full bump, warping the reflection over tile/grout relief exactly like SSR). The single-ray aliasing this used to cause is now averaged by the temporal upscale (r_rtReflTemporal), so it defaults to full. Lower it toward 0 for a cleaner, glassier mirror if a rough surface sparkles", 0.0f, 1.0f );
idCVar r_rtReflBlur( "r_rtReflBlur", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR6b: box-blur the RT reflection before compositing, to dissolve the residual SPATIALLY-STATIC firefly grid. RT fills the grout pixels SSR can't reach with a flat-shaded approximation that steps against SSR's neighbours; HDR preserves that step (LDR clips it) into a dot temporal can't remove. Blurring spreads each fill into a low-frequency haze and dims isolated 1px speckles far more than large off-screen fills. Higher = softer reflection, fewer dots; 0 = sharp (RR6a look, dots visible when still)", 0.0f, 1.0f );
idCVar r_rtReflRealLights( "r_rtReflRealLights", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RR16 (docs/rtx-reflections.md): shade an RT reflection hit with the scene's REAL lights (gathered into a device buffer the reflection pass loops), so a reflection is lit correctly whether or not the reflected surface is on screen. 1 = real per-light diffuse; 0 = the old single fixed key light. Diffuse only, confined to each light volume by r_rtReflLightFrustum (no shadow rays - RR16d is parked); VK + RT + HDR only" );
idCVar r_volumetrics( "r_volumetrics", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "volumetric lighting (docs/volumetric-lighting.md): the air scatters the scene's real lights toward the camera - visible cones under spot lights, shafts through grates, monsters cutting dark wedges out of a lit haze. A low-res ray march of each view ray through the shared scene-light buffer, composited into the HDR scene after the stock fog. NON-VANILLA LOOK: adds light to pixels stock never lit and lifts blacks inside lit volumes - off by default, turned on only by the Path Tracing preset. Vulkan + ray-query hardware + r_hdr" );
idCVar r_volShadows( "r_volShadows", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "volumetric lighting: trace one any-hit visibility ray per reaching light per march sample, so occluders cut shadows through the lit air and a light behind a wall does not glow through it. Needs the RT scene (r_rtSunShadows / r_rtAllLights / r_rtao build it) - silently unshadowed without one. 0 = unshadowed glow volumes (cheaper; lights leak through walls). Lights with noShadows never trace" );
idCVar r_volIntensity( "r_volIntensity", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: brightness of the scattered light (a plain multiplier on the result)", 0.0f, 16.0f );
idCVar r_volDensity( "r_volDensity", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: how much haze is in the air. 1 = thin (a ~500-unit room scatters about half of a full-strength light toward the eye); higher = smokier. Unlike r_volIntensity it also drives r_volExtinction", 0.0f, 16.0f );
idCVar r_volAnisotropy( "r_volAnisotropy", "0.4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: Henyey-Greenstein g. 0 = the haze glows equally from every angle; higher = forward scattering, the volume is brighter when you look TOWARD the lamp through it and dimmer looking away (dust/mist behave like ~0.3-0.6)", -0.9f, 0.9f );
idCVar r_volLights( "r_volLights", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "volumetric lighting: which lights scatter. 1 = projected/spot and parallel (sun) lights only - the authored feature lights, whose cones read as shafts; 2 = also point lights (every fill light becomes a faint glow ball: hazier, flatter, costlier)", 1, 2 );
idCVar r_volSteps( "r_volSteps", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "volumetric lighting: march samples per pixel (4..64). Sample positions are jittered per pixel and the result is temporally accumulated + blurred, so few steps show as grain rather than banding; the per-step march cost scales linearly (on top of a fixed blur/upsample/temporal cost). Default 6 (user-tuned: the point where the temporal+blur reconstruction still hides the step banding)", 4, 64 );
idCVar r_volResScale( "r_volResScale", "0.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: march resolution as a fraction of the screen, snapped to Quarter (0.25) or Half (0.5). The signal is low-frequency, so Quarter is the intended setting", 0.25f, 0.5f );
idCVar r_volMaxDist( "r_volMaxDist", "2048", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: how far along the view ray the march reaches (world units). The steps are spread over this distance (or the opaque depth if nearer), so a smaller value concentrates them", 128.0f, 16384.0f );
idCVar r_volExtinction( "r_volExtinction", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: let the haze also DIM the scene behind it (Beer-Lambert, scaled by r_volDensity). 0 = off (default): darkening distant surfaces is a second look change on top of the first, and the stock fog lights already own that job; 1 = physically matched to the scattering", 0.0f, 4.0f );
idCVar r_volPointLightScale( "r_volPointLightScale", "0.12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: brightness of POINT (omni) lights in the haze relative to projected/spot and parallel lights (needs r_volLights 2). Doom 3 rooms hold many large fill lights; at 1 their summed glow drowns the authored light cones, lower values keep the cones the feature and the fill lights a faint ambience", 0.0f, 4.0f );
idCVar r_volFogPointLightScale( "r_volFogPointLightScale", "0.78", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: what r_volPointLightScale rises to INSIDE a stock fog volume (r_volLights 2). Foggy rooms hold more haze, so their point lights glow more in the air; the rise follows the fog's own thickness (a thin, long-distance fog only goes part of the way) and fades in at the volume's edges. Never lowers the scale; set it equal to r_volPointLightScale to disable the boost", 0.0f, 4.0f );
idCVar r_volAoIntensity( "r_volAoIntensity", "1.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: ambient-occlusion intensity WHILE r_volumetrics is active. The haze adds light everywhere inside lit volumes and flattens contact shading; a stronger AO restores the grounding. Used in place of r_rtaoIntensity / r_ssaoIntensity (whichever producer runs) when it is HIGHER than them - neither cvar is rewritten, and turning volumetrics off gives their own values back. 0 = leave AO alone", 0.0f, 4.0f );
idCVar r_volLargeLightRadius( "r_volLargeLightRadius", "1200", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: lights whose volume is larger than this (bounding radius, world units) fade out of the haze, by (this / size) squared. A cave-wide sky-glow or outdoor fill light is ambience, not a shaft: integrated over hundreds of metres of air it washes the whole view in its own colour (the pale-cyan veil over the Hell lava cave) and its sheer depth brings the march grain back. Room-scale lights are unaffected. 0 = no size fade", 0.0f, 16384.0f );
idCVar r_volFlashlightScale( "r_volFlashlightScale", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: brightness of the player's OWN first-person weapon lights (the flashlight beam, muzzle flashes) in the haze. The flashlight starts at the eye, so its cone fogs the whole view more than any world light; 1 = like every other light, 0 = the flashlight lights surfaces but never the air", 0.0f, 4.0f );
idCVar r_volPointShadows( "r_volPointShadows", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "volumetric lighting: trace visibility rays for POINT (omni) lights too (r_volLights 2). They are the majority of the rays (~0.8 ms measured) for lights that r_volPointLightScale keeps faint; 0 = point-light haze is unshadowed - cheaper, but a fill light's glow can show faintly through a wall inside its light box. Projected/spot lights always trace (r_volShadows)" );
idCVar r_volBlur( "r_volBlur", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "volumetric lighting: radius (in low-res march texels) of the depth-aware blur applied to the march before it is upsampled. The march dithers its sample depths per pixel; this blur is what resolves that dither into a smooth volume - 1 (3x3) acts like ~9x the steps, 2 (5x5) ~25x and softer shaft edges. 0 = off: the haze is visibly made of moving dots unless r_volTemporalFeedback is very high", 0, 3 );
idCVar r_volTemporal( "r_volTemporal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "volumetric lighting: re-jitter the march sample depths every frame and accumulate the result across frames (reprojected as the camera moves), so a dozen steps resolve into a smooth volume instead of a dot pattern. 0 = raw single-frame march (static grain)" );
idCVar r_volTemporalFeedback( "r_volTemporalFeedback", "0.8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "volumetric lighting: fraction of history kept per frame with r_volTemporal. Higher = smoother but shafts lag more behind fast camera motion and moving/flickering lights; lower = snappier, grainier", 0.0f, 0.97f );
idCVar r_volFastResolve( "r_volFastResolve", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "volumetric lighting perf: accumulate the temporal history in the LOW-res march grid and do the depth-aware bilateral upsample ONCE at composite (the Wronski/Hillaire froxel pattern), instead of upsampling to full res first and then running the temporal + composite at full res. Collapses three full-res passes into one, and makes the history 16x smaller. Near-lossless (same froxel data; reproject/clamp happens in the native low-res grid). 0 = the old upsample-then-full-res-temporal path (for A/B)" );
idCVar r_rtReflLightFrustum( "r_rtReflLightFrustum", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RR16c (docs/rtx-reflections.md): confine each real light shading an RT reflection hit (r_rtReflRealLights) to its actual Doom 3 light volume - the projection frustum of a spot light, the BOX of a point light - instead of a sphere around its origin. 1 = faithful reach: a spot no longer lights what is beside/behind it, a point light stops at its box faces (soft-edged, like the light textures); 0 = the RR16b origin+radius sphere for A/B. Uniform inside the volume (the projection/falloff texture pattern is not sampled). Near-free" );
idCVar r_rtReflLightScale( "r_rtReflLightScale", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR16: overall brightness of the real-light diffuse shading at an RT reflection hit (r_rtReflRealLights). The point-light approximation has no projected-light central brightness and a plain linear falloff, so reflected surfaces come out dimmer than they look on-screen; this scales the summed light contribution to compensate. Raise until reflected walls/floors match their on-screen brightness (default 2); lower if bright areas of the reflection blow out. Does not affect self-lit sources (r_rtReflEmissive)", 0.0f, 8.0f );
idCVar r_rtReflEmissive( "r_rtReflEmissive", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR16f: how brightly a self-illuminated LIGHT-SOURCE surface (neon tubes, ceiling light tiles - materials with an additive glow stage) glows in an RT reflection. Such a surface is lit by NO external light (it lights the room, not itself), so under real-light shading it shades to ~dark and 'goes missing' in an off-screen reflection; this adds back its own glow colour. 1 = roughly the source's own direct brightness (tune so a light source in the reflection matches how it looks on-screen); higher = hotter (can blow out and overpower the rest of the reflection), 0 = off. Needs r_rtReflRealLights", 0.0f, 16.0f );
idCVar r_rtReflResScale( "r_rtReflResScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "resolution the RT reflection is TRACED at, as a fraction of the screen (the result is temporally upscaled back to full res, RR6c). SNAPPED to an integer divisor - Full (1), Half (0.5), Quarter (0.25) - because fractional ratios strobe the reflection. A SEPARATE value from the SSR resolution (r_ssrResScale) so the RT trace scales independently: Half is ~4x cheaper on the reflection pass for a modest softening, Quarter cheaper still. Only the RT reflection (r_rtReflections) uses this", 0.25f, 1.0f );
idCVar r_rtReflGlassScale( "r_rtReflGlassScale", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "RR5c (docs/rtx-reflections.md): brightness of the RT reflection on GLASS/visor surfaces (environment_rt / bumpyenvironment_rt) - goggles, hazmat visors, the dropship cockpit pane. These panes reflect a NEARLY-BLACK cube in vanilla (env/bland ~0.035, env/gen1 ~0.022) which IS their authored glass reflectivity; the RT reflection is scaled by that cube's PERCEPTUAL brightness (sqrt of its average, so the near-black glass still reflects visibly) so it reads like glass with a grazing Fresnel boost instead of a mirror, and windows (env/gen2) stay shinier than goggles. This is the taste knob on top: default 2 = the user-calibrated glass look, 1 = subtler, higher = brighter/shinier toward a mirror, 0 = no RT reflection on glass (the pane goes clear). Only the RT tier (r_rtReflections) uses it", 0.0f, 4.0f );
idCVar r_rtMovingLights( "r_rtMovingLights", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced shadows for MOVING point lights (Vulkan + RT hardware): a point light whose pose changed recently (muzzle flash, projectiles, monster-carried lights) skips its 6-face cube re-render entirely and traces one ray toward the light instead - the exact case the shadow cache can never help (a moving light must re-render every frame on the map path), while a ray costs the same whether the light moves or not. Stationary lights keep the cached cube path; a light that stops moving re-caches after a short hysteresis (shadow look transitions RT-hard <-> PCF-soft at that boundary). Option A of docs/shadow-research.md 2026-08 recency check" );
idCVar r_rtPerforatedShadows( "r_rtPerforatedShadows", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced shadows: let perforated (alpha-tested, noShadows) grates/fences cast - the shadow ray samples the grate texture's alpha where it crosses one, so the holes let light through. Darkness follows r_shadowMapPerforatedStrength; also needs r_shadowMapPerforated. 0 = grates cast nothing under ray-traced lights" );
idCVar r_rtAllLights( "r_rtAllLights", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ray-traced shadows for EVERY shadow-casting light (Vulkan + RT hardware): all lights take the route r_rtSunShadows / r_rtMovingLights use - one hard shadow ray per lit pixel toward the light, pixel-exact at any distance - instead of a shadow map (or stencil volumes when shadow mapping is off). No shadow maps are rendered at all while it is on. Costs one ray per lit pixel PER LIGHT every frame, where a still light's cached cube map is free - expect it to be heavier in light-dense rooms. Perforated grates/fences keep casting through r_rtPerforatedShadows. r_rtShadowBlur softens these shadows too. Implies the ray-tracing scene" );
idCVar r_rtReStirDI( "r_rtReStirDI", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR direct lighting (docs/rtx-restir-di.md, Vulkan + RT hardware): reservoir spatiotemporal importance resampling so the scene's lights are direct-lit at ONE shaded sample + ONE shadow ray per pixel, instead of r_rtAllLights' ray-per-light-per-pixel loop - the per-pixel cost stops scaling with light count. The first piece of the H7 full-RT tier (dude_rtx 2, 'Path Tracing'). STAGE 1 BRING-UP: runs the RIS resampling pass (unshadowed, no reuse yet) into its own buffer; nothing consumes it in the lit scene yet, so the scene is byte-identical unless r_rtReStirDIDebug shows it. Staged 0-6 in the doc" );
idCVar r_rtLightTextures( "r_rtLightTextures", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT scene lights sample their real light textures (Vulkan + RT hardware): the per-view scene-light buffer that ReSTIR DI (r_rtReStirDI) and the RT GI bounce (r_rtgi) light from carries each light's projection cookie + falloff ramp as bindless textures, and those passes attenuate with them exactly as the forward interaction draw does - a spot shows its real cone/blob, a coloured gobo colours its light, a scrolling cookie scrolls. 0 = the analytic frustum attenuation (uniform box/pyramid, no cookie shape or colour). Stage G of docs/rtx-restir-di.md" );
idCVar r_rtReStirDIApply( "r_rtReStirDIApply", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI LIGHTS THE SCENE (docs/rtx-restir-di.md Stage D, Vulkan + RT hardware, needs r_rtReStirDI): the denoised DI is shaded with the G-buffer material (albedo + GGX roughness/metalness + spec map) and added into the HDR scene as the direct lighting of every opaque world surface, and the forward per-light interaction draws (and their shadow work) are SKIPPED for every light the DI covers - the dude_rtx 2 direct-lighting path, replacing r_rtAllLights' ray-per-light loop. The view weapon, translucent surfaces, blood films, fog/blend lights, ambient lights and sub-views stay on the forward path. 0 = DI is debug-only and the forward path is untouched (default). A/B against 0: same look, cheaper in light-dense rooms" );
idCVar r_rtReStirDIDynamicForward( "r_rtReStirDIDynamicForward", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI: keep the first-person weapon lights (flashlight, muzzle flash: allowLightInViewID) on the FORWARD per-light path instead of DI (docs/rtx-restir-di.md). 1 = weapon lights forward (default) - they are masked out of DI's field anyway; every other light (including moving, flickering and (re)appearing lights) rides DI + the R1 gradient confidence, which resets NRD history exactly where the lighting changes. 0 = none, every light goes through DI (laggy flashlight; A/B only). R6 removed the old mode-1 fast-light rule set + the handover crossfade - the gradient made them unnecessary." );
idCVar r_rtReStirDICombineGI( "r_rtReStirDICombineGI", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI + RT GI together in ONE buffer: with r_rtgi on, the DI gather also traces the one-bounce GI ray at the same pixel and folds the bounce (finished as rtgi_apply would: albedo * intensity, fog-faded) into the DI radiance, so ONE NRD denoise and ONE apply serve direct + indirect - about the cost of DI alone, and the GI checkerboard rides DI's. RTGI's own pass stands down; DI's resolution, checkerboard, history and NRD knobs then shape the bounce too, and the direct term's light distance drives the blur radius for both. 0 = DI only (RTGI stays stood down while DI is on)" );
idCVar r_rtReStirDISH( "r_rtReStirDISH", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI: denoise as spherical harmonics (NRD RELAX_DIFFUSE_SH) - the gather also records the incoming light DIRECTION, and the apply re-resolves the FULL-resolution bump normal's cosine per pixel, so bump-mapped shading detail (skin pores, wrinkles, cloth weave) survives the denoiser's spatial blur. The gather stores di = the full IRRADIANCE (cosine IN, identical to the non-SH buffer); the apply recovers detail as a brightness-neutral RATIO cos(N_full.L)/cosGath, where cosGath is di's own reduced-res cosine blended with the SAME 4-tap as di - so on a smooth/native surface the ratio is exactly 1 (= the non-SH look, no banding) and detail appears only where the bump normal diverges. 0 = plain radiance denoise (no bump detail, but identical overall shading)" );
idCVar r_rtReStirDISpecReshade( "r_rtReStirDISpecReshade", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI (needs r_rtReStirDISH + r_rtReStirDIApply): shade the gather + denoise DIFFUSE-ONLY, then rebuild the specular highlight at FULL resolution in the apply from the SH light direction + the full-res bump normal + material - a GGX lobe exactly as interaction.frag's. The plain SH path re-applies only a flat diffuse cosine, so a baked, reduced-res, diffuse-denoised highlight reads soft and shapeless; this restores the sharp glint that traces skin pores, wet surfaces and metal in the DI tier, matching the forward path. 0 = plain SH (specular folded into the diffuse denoise, softer)" );
idCVar r_rtReStirDICandidates( "r_rtReStirDICandidates", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI: light candidates per pixel before the weighted-reservoir pick. 0 = EVERY light in the scene-light buffer is weighed (exhaustive RIS: no candidate noise, so a dominant far light - a sun through windows - is sampled densely wherever it reaches; the buffer holds at most 256 lights and the cheap sphere / box rejects keep this near the cost of the GI bounce's light loop). N > 0 = draw N candidates uniformly at random (the textbook recipe for thousands of lights): a light that is drawn rarely arrives as sparse bright spikes that the denoiser's anti-firefly clips, so it reads dimmer than forward. Clamped 0..128", 0, 128 );
idCVar r_rtReStirDICheapTarget( "r_rtReStirDICheapTarget", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI: weigh the gather's ~38 candidate lights with a CHEAP target function (the unshadowed irradiance - colour x cookie/falloff x N.L - WITHOUT the apply-mode GGX material response), and compute the full PBR shade only ONCE, for the winning light. The RIS target may be any cheap positive approximation (the per-pixel material factor barely shifts which light wins), so this cuts the gather's biggest cost - ~38 full GGX evals per pixel down to ~38 cheap ones + 1 full - at a tiny variance increase on glossy surfaces. 1 = cheap target (default), 0 = full GGX target per candidate (the A/B; exactly the old behaviour). Only meaningful with r_rtReStirDIApply" );
idCVar r_rtReStirDIDebug( "r_rtReStirDIDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "ReSTIR DI debug view (blits the DI buffer over the finished scene, LINEAR - the HDR pipeline exposes it; the weapon reads as no-DI like rtgi_debug; r_rtReStirDIApply is suppressed while this is on): 0 = off, 1 = the RAW 1-spp gather before the denoise (under r_rtReStirDICheckerboard the traced half is visibly packed into the buffer's LEFT half - the proof the checkerboard is live; at a reduced r_rtReStirDIResScale it is upscaled), 2 = the NRD-denoised DI when the denoise ran this frame (else raw), 3 = NRD's own VALIDATION overlay (R0: accumulated frames / history resets / antilag per pixel - the lag diagnostic), 4 = the denoiser's accumulated HISTORY LENGTH per pixel (black = fresh, white = full history; R3 keys its disocclusion boost on this), 5 = the R1 gradient CONFIDENCE fed to NRD (white = trust history, dark = a lighting change detected there), 6 = the raw R1 gradient (red = |change|, green = magnitude). Needs r_rtReStirDI 1; 7 = R3a: the pixels the gather boosted this frame and WHY (red = NRD history short at the previous position, green = no previous pixel is this surface, blue = hysteresis / entered the screen; packed left under checkerboard) - in a still view this must be black bar the weapon and the screen edges; 8 = the CHOSEN LIGHT per pixel: each light-buffer row (placed light or emitter row) in its own hashed colour, black = no light reached the pixel - a region that should be lit by one fixture but shows another colour or black says which candidate the gather picked there", 0, 8 );
// ReSTIR DI NRD spatial-cleanup knobs (docs/rtx-restir-di.md) still read by ReLAX - clean the 1-spp noise it leaves
// on fine detail (edge-stopped) + moving silhouettes (thin history). The ReBLUR-only prepass/blur-radius were trimmed at R6.
idCVar r_rtReStirDIHistoryFix( "r_rtReStirDIHistoryFix", "5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI: NRD ReLAX historyFixFrameNum - frames of strong SPATIAL reconstruction applied where temporal history is thin (fresh disocclusion / a moving limb), i.e. the lever for noise on moving parts. Clamped below the fast-history window (~5). 0 = keep NRD default (3)", 0, 8 );
idCVar r_rtReStirDIEdgeStop( "r_rtReStirDIEdgeStop", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI: scales NRD ReLAX's lobeAngleFraction (normal edge-stopping). >1 lets the spatial passes blur ACROSS depth/normal edges - the lever for sub-pixel grate noise the other knobs can't reach, at the cost of softening that detail. 0 = keep NRD default (no scaling); 1 = also no change", 0.0f, 16.0f );
idCVar r_rtReStirDIResScale( "r_rtReStirDIResScale", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI: fraction of the screen the DI gather (and its NRD denoise) runs at, upscaled back to full res for display. Direct lighting is higher-frequency than GI, so dropping below full softens shadow/contact detail more than it does for RTGI - but it is the biggest cost lever (~1/scale^2). 1 = full res (default); composes with Checkerboard (which then traces half of these pixels). Clamped 0.25..1", 0.25f, 1.0f );
idCVar r_rtReStirDIBilateralUpscale( "r_rtReStirDIBilateralUpscale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI: when the gather runs below full resolution, upscale it with a depth+normal-aware (joint-bilateral) filter instead of plain bilinear, so the DI stops bleeding across silhouettes/creases. A few extra taps in the debug overlay. 0 = plain bilinear. Inert at 100% resolution", 0, 1 );
idCVar r_rtReStirDICheckerboard( "r_rtReStirDICheckerboard", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI: trace a FULL-RES horizontal checkerboard - only half the pixels (packed into the buffer's left half, the traced parity alternating each frame) - and let NRD reconstruct the other half from a full-res guide pass + motion vectors, instead of shading every pixel. ~half the gather + visibility rays at full resolution (sharp edges preserved, unlike a reduced-res scale), the 'half pixel from NRD' GPU save. Needs the velocity prepass; even render width. 0 = off (every pixel traced)", 0, 1 );
idCVar r_rtReStirDIHistory( "r_rtReStirDIHistory", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI denoiser history length: max frames NRD ReBLUR accumulates for the DI radiance (its own knob - DI no longer borrows r_rtgiHistory). The 1-spp gather needs temporal accumulation to look smooth: too short and single pixels 'fry' (noise/flicker), too long and a switched-off/occluded light lingers before it fades. ~12 is the balance; raise for smoother-but-laggier, lower for snappier-but-noisier. NRD's own default is 30. Mirrors RTGI's History Length (same 2..63 range)", 2, 63 );
idCVar r_rtReStirDIHistoryMs( "r_rtReStirDIHistoryMs", "360", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI denoiser history in MILLISECONDS instead of frames (R0, docs/rtx-restir-di.md Plan R0): >0 converts to frames every frame from a 0.2 s low-passed frame rate (NRD GetMaxAccumulatedFrameNum: 'minimizes lag at low FPS and maximizes IQ at high FPS'); the fast history scales with it. 0 = r_rtReStirDIHistory in frames, as before", 0.0f, 2000.0f );
idCVar r_rtReStirDIFastClamp( "r_rtReStirDIFastClamp", "1.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI denoiser: NRD ReBLUR fastHistoryClampingSigmaScale [1;3] - how tightly the slow history is clamped to the fast one. 2.0 = NRD default (today); 1.5 'works well even for dirty signals' (NRD sample). Lower = less lag, more flicker", 1.0f, 3.0f );
idCVar r_rtReStirDIAntilag( "r_rtReStirDIAntilag", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI denoiser: NRD ReBLUR built-in luminance antilag. 1 = on at NRD defaults (today); 0 = inert, the A/B NRD recommends for an initial integration ('Initial integration should be done with disabled antilag') and the baseline for the R1 gradient confidence" );
idCVar r_rtReStirDIDisocclusion( "r_rtReStirDIDisocclusion", "0.016", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI denoiser: NRD disocclusionThreshold - relative depth difference (slope-scaled) above which the reprojected history is rejected as a different surface. 0.01 = NRD default (today); NRD's range is 0.01..0.02. Looser = fewer FALSE history resets on moving objects / the weapon (watch the 'diff frames' trail in r_rtReStirDIDebug 3), at the cost of some bleed across real depth edges", 0.005f, 0.05f );
idCVar r_rtReStirDIGradient( "r_rtReStirDIGradient", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI R1 (docs/rtx-restir-di.md Plan R1): temporal GRADIENT confidence for the NRD denoise - re-shade last frame's chosen light at the reprojected surface under this frame's lights (1/9 of the pixels, one shade + one ray each), A-trous filter the change, and feed NRD IN_DIFF_CONFIDENCE so history is dropped ONLY where the lighting really changed (a light switching, moving, the flashlight sweeping) - the RTXDI / Quake II RTX recipe, NRD's 'best way to mitigate temporal lags'. Replaces the old luminance confidence (r_rtReStirDIConfidence, which measured noise). USER-VALIDATED 2026-10-12 (default on); 0 = off for the A/B" );
idCVar r_rtReStirDIGradientPower( "r_rtReStirDIGradientPower", "14", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient confidence: sharpening power of (1 - relative change) -> confidence (RTXDI 8, Remix 8). Higher = a smaller change already rejects history", 1.0f, 32.0f );
idCVar r_rtReStirDIGradientHistory( "r_rtReStirDIGradientHistory", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient confidence: temporal smoothing length (frames) applied ONLY while confidence RISES after a change (Remix: drops apply at once, recoveries are held so ReSTIR has time to stabilize; RTXDI/Remix 8 - but they hold for reservoir reuse, which DI does not have: its gather is fresh every frame, so 2 only bridges gradient flicker; 8 held the confidence down for half a second after every blink and trailed behind movers). 0 = no smoothing", 0.0f, 32.0f );
idCVar r_rtReStirDIGradientFloor( "r_rtReStirDIGradientFloor", "0.12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient confidence: minimum confidence (Remix 0.1). Under ReBLUR the effective floor is at least historyFix / history (NRD: a lower confidence triggers the history-fix reconstruction - the wide wash), so this only matters below that ratio or under ReLAX", 0.0f, 1.0f );
idCVar r_rtReStirDIGradientBias( "r_rtReStirDIGradientBias", "0.157", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient confidence: darkness bias added to the change's denominator (RTXDI darknessBias) so dark-on-dark flicker does not read as a 100% change. In the demodulated, light-scaled luminance units of the gather", 0.0f, 1.0f );
idCVar r_rtReStirDIGradientDeadzone( "r_rtReStirDIGradientDeadzone", "0.22", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient confidence: relative per-light change below which nothing counts as a lighting change. Doom's authored flicker tables pulse many lights by 5-15% every frame (the 20% rule keeps them in DI), and a per-light comparison is noise-free, so this is a real threshold: 0.2 ignores a 20% pulse (user-tuned 2026-10-12; 0.1 still showed grey under pulsing lights), a light switching off is still a 100% change of its own value. 0 = every change counts", 0.0f, 0.9f );
idCVar r_rtReStirDIGradientStatic( "r_rtReStirDIGradientStatic", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R1 gradient: a surface point that moved less than this many full-res PIXELS' worth of world units between the frames is static and is re-shaded at its exact stored previous position / normal / camera, so a static world under a static light compares bit-identical (the FSR2 jitter + reprojection otherwise land the two shades on sub-pixel-different points: a different bump texel / cookie texel / shadow edge reads as a few-percent change, and the power-8 confidence turns that into grey frying). Movers keep the current surface. 0 = always the current surface", 0.0f, 16.0f );
idCVar r_rtReStirDIGradientStratum( "r_rtReStirDIGradientStratum", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R1 gradient: the stratum size - one re-shade + one ray per NxN block of the gather res. 3 = RTXDI (1/9 of the pixels, ~0.35 ms at 1440p x 0.75); NRD says 1/5 res is sufficient (5 = 1/25, ~0.13 ms). The A-trous reach scales with it", 2, 6 );
idCVar r_rtReStirDIGradientFilterPasses( "r_rtReStirDIGradientFilterPasses", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R1 gradient: A-trous filter passes over the gradient (strides 1/2/4/8). 4 = RTXDI (31 px reach in gradient space = ~93 screen px at stratum 3): a change resets history well around itself, which keeps the reset smooth but also widens the soft, short-history region around a moving shadow. 2 (default, user-tuned 2026-10-12: a walking NPC's shadow edge improved) hugs the change tighter; 0 = unfiltered", 0, 4 );
idCVar r_rtReStirDIRelaxFast( "r_rtReStirDIRelaxFast", "8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R2 ReLAX: diffuseMaxFastAccumulatedFrameNum. Remix direct light: 1 ('to reduce trail'); NRD default 6", 1, 63 );
idCVar r_rtReStirDIRelaxPrepass( "r_rtReStirDIRelaxPrepass", "24", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R2 ReLAX: diffusePrepassBlurRadius (px) - the pre-accumulation blur. Remix direct: 0; NRD default 30. Our gather has no spatial reuse yet (R3), so a few px may be needed against the 1-spp noise", 0.0f, 100.0f );
idCVar r_rtReStirDIRelaxPhi( "r_rtReStirDIRelaxPhi", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R2 ReLAX: diffusePhiLuminance, the A-trous luminance edge-stopping sensitivity. RTXDI sample 1.0, NRD default 2.0. Lower = sharper shadow edges, more residual noise", 0.1f, 8.0f );
idCVar r_rtReStirDIRelaxAtrous( "r_rtReStirDIRelaxAtrous", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R2 ReLAX: atrousIterationNum [2;8]. NRD default 5; user-tuned to 5 (2026-10-14: the short-history flicker-response tuning trades temporal for spatial, so more A-trous passes keep it clean)", 2, 8 );
idCVar r_rtReStirDIRelaxVarianceHist( "r_rtReStirDIRelaxVarianceHist", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R2 ReLAX: spatialVarianceEstimationHistoryThreshold - below this history length the variance is estimated spatially. RTXDI sample 1, NRD default 3", 0, 10 );
idCVar r_rtReStirDIRelaxConfRelax( "r_rtReStirDIRelaxConfRelax", "0.31", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R2 ReLAX: confidenceDrivenRelaxationMultiplier - where the R1 gradient confidence is low, relax the A-trous luminance / normal edge stopping (Remix direct: 0.7 with 1.4 / 0.6). 0 = off", 0.0f, 2.0f );
idCVar r_rtReStirDIRelaxFirefly( "r_rtReStirDIRelaxFirefly", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "ReSTIR DI R2 ReLAX: enableAntiFirefly (NRD default off, +7-10% ReLAX cost; Remix / RTXDI sample on). Default off (user, 2026-10-14): the anti-firefly clips bright flicker transients, hurting flickering-light response" );
idCVar r_rtReStirDIBoost( "r_rtReStirDIBoost", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ReSTIR DI R3a disocclusion boost (docs/rtx-restir-di.md Plan R3): K independent light picks + K visibility rays per pixel where the pixel is YOUNG - just revealed (a walking NPC's wake, a screen edge while turning, a camera cut) or NRD's accumulated history still below r_rtReStirDIBoostHistory frames. One ray leaves such a pixel so noisy that the denoiser widens its filter and smears the neighbouring shadow across the wake for the whole refill (the dark trail behind a walking NPC, the look-away wash); K picks share the one light loop and their K rays average to 1/K the variance. Hysteresis: a boosted pixel stays boosted until its history reaches twice the threshold (capped at the history length), so the ray count never flips frame to frame. 0 / 1 = off, 2..4 = K (2 = default since 2026-10-12: the user's cost / quality choice - a tighter NPC wake and cleaner look-away for ~1 ms while moving; 4 doubles the ray part). RTXDI / Remix: numDisocclusionBoostSamples 8 / 4", 0, 4 );
idCVar r_rtReStirDIBoostHistory( "r_rtReStirDIBoostHistory", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "ReSTIR DI R3a: NRD history length (frames, read from last frame's denoised .w) below which a pixel gets the boosted picks; it then stays boosted until 2x this (capped at r_rtReStirDIHistory). 0 = the history trigger is OFF: only a true disocclusion (a mover's wake, floor revealed while turning) and pixels entering the screen are boosted, held until the history reaches 4 frames - a short NRD history over whole areas is what flickering / animated lights leave every frame on purpose, and boosting their footprints cost ~1.4 ms standing still (2026-10-12). Remix: disocclusionFrames 8", 0.0f, 63.0f );
idCVar r_nrdSigma( "r_nrdSigma", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "NRD: keep the SIGMA_SHADOW denoiser in the instance (R0, docs/rtx-nrd.md N0). Nothing has dispatched it since H3 soft shadows were abandoned, yet it cost its pipelines + pool textures on every NRD (re)create; 0 drops it (default), 1 = the per-light penumbra experiment can use it. Changing this rebuilds the NRD instance" );
idCVar r_rtShadowBlur( "r_rtShadowBlur", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "soften the hard ray-traced shadows (r_rtSunShadows, r_rtMovingLights) with a screen-space blur (docs/rtx-shadow-blur.md, Vulkan + RT hardware): the same one ray per pixel, traced per light right before it draws, then blurred by an amount that follows the ray's occluder distance - sharp at contact, softer with the gap, at most 24 pixels. Deterministic: no noise, nothing accumulated over frames. 0 = the hard shadows exactly as before. Which lights are ray-served is unchanged; needs the depth/normal prepass (SSAO, RTAO or motion vectors on), else the lights keep their hard ray" );
idCVar r_rtShadowBlurStagger( "r_rtShadowBlurStagger", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "r_rtShadowBlur perf: lights far from the viewer refresh their blurred shadow mask less often - light ORIGIN nearer than r_rtShadowBlurStaggerNear every frame, up to r_rtShadowBlurStaggerFar every 2nd frame, beyond that every 3rd (parallel suns always every frame). A reused mask is looked up reprojected through the camera it was made with, and any pixel it cannot answer for (hidden or off screen back then) traces a hard ray for that frame. Costs one view-sized RGBA8 mask of VRAM per staggered light (~15 MB at 1440p). Moving casters' shadows update at the light's reduced rate; a light that itself moves always refreshes" );
idCVar r_rtShadowBlurStaggerNear( "r_rtShadowBlurStaggerNear", "192", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "r_rtShadowBlurStagger: lights whose origin is nearer to the viewer than this (world units) refresh every frame; beyond it every 2nd frame. 0 = every light is at least half rate", 0.0f, 8192.0f );
idCVar r_rtShadowBlurStaggerFar( "r_rtShadowBlurStaggerFar", "420", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "r_rtShadowBlurStagger: lights whose origin is farther from the viewer than this (world units) refresh every 3rd frame. 0 = every light at a third of the rate", 0.0f, 16384.0f );
idCVar r_rtShadowBlurIntensity( "r_rtShadowBlurIntensity", "0.13", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "r_rtShadowBlur: how strongly the ray-traced shadows are blurred. Scales the blur width everywhere; the width itself still follows the geometry (none where the caster touches the surface, more as the gap grows) and is capped at 24 pixels, so high values mostly soften the near-contact edges further. 0 = no blur (the hard shadow); default 0.13 (user-tuned 2026-10-19, was 0.65)", 0.0f, 4.0f );
idCVar r_rtShadowBlurCheckerboard( "r_rtShadowBlurCheckerboard", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "r_rtShadowBlur perf: trace the shadow ray at a STATIC screen checkerboard (half the pixels) instead of one ray per pixel; the skipped pixels are reconstructed inside the horizontal blur pass from their two traced neighbours (no extra pass). The parity is fixed, not per-frame, so the deterministic 'same camera -> same mask' guarantee holds (no shimmer). Cuts the rtblur.ray cost substantially and scales with light count (measured ~1.3 ms in a ~14-light scene); the only cost is a ~1-pixel softening at the very hardest contact edges. 0 = one ray per pixel (the pre-checkerboard look)" );
idCVar r_rtShadowBlurFastTiles( "r_rtShadowBlurFastTiles", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "r_rtShadowBlur perf: the tile-classification pass reads a compact 2-byte/pixel summary the ray pass writes, instead of the 8-byte RGBA16F ray target. Image-identical; 0 = the old path (for A/B)" );
idCVar r_rtShadowBlurCurve( "r_rtShadowBlurCurve", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "r_rtShadowBlur: shape of the progressive blur - how fast a shadow softens as it gets further from the object casting it. Width = intensity * g^curve, where g = (caster-to-surface distance) / (caster-to-light distance). 1 = geometric growth (what a real light does); below 1 = softens sooner and then levels off (more uniform blur); above 1 = stays crisp for longer near the object, then widens fast. Default 0.5 (user-tuned 2026-10-19, was 0.9)", 0.25f, 4.0f );
idCVar r_rtShadowBlurMinPenumbra( "r_rtShadowBlurMinPenumbra", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "r_rtShadowBlur perf: a distant and/or small light casts a shadow penumbra finer than this many screen pixels - softening it is invisible, so skip its blur entirely and keep the crisp inline hard ray (saves the whole per-light ray+tiles+blur chain). The estimate is 2 * lightRadius(world) * pixels-per-unit / receiver-depth, i.e. the light's apparent angular penumbra; parallel suns (fixed ~1deg penumbra everywhere) are never skipped. Default 1.0 (sub-pixel). Raise to skip more aggressively; 0 = never skip (blur every ray-served light)", 0.0f, 8.0f );
idCVar r_rtTlasDirty( "r_rtTlasDirty", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "RT scene: skip the per-frame TLAS rebuild when nothing dynamic changed (no mover moved this frame, no animated caster visible). The last-built TLAS persists and still holds the current poses, so an idle scene pays no per-frame acceleration-structure cost. 0 = rebuild every frame unconditionally (for A/B)" );

// DUDE: emissive fill lights — interactive GUI screens (monitors, keypads, wall
// panels) glow but cast no light in Doom 3's model, so they read as decals pasted
// onto an unlit wall. These spawn a small shadowless point light per visible screen
// to ground it. Non-vanilla; enhancement backends only. See tr_light.cpp.
idCVar r_emissiveSurfaces( "r_emissiveSurfaces", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "let emissive surfaces (GUI screens, monitors, video panels) cast a small fill light onto nearby geometry so they don't look detached (non-vanilla; opengl3/Vulkan only)" );
idCVar r_emissiveLightScale( "r_emissiveLightScale", "0.50", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "brightness of emissive-surface fill lights (0 = off, 1 = a full-strength light); keep low for subtle bleed", 0.0f, 4.0f );
idCVar r_emissiveLightRadius( "r_emissiveLightRadius", "1.80", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "reach of GUI-screen fill lights, as a multiple of the screen's own size (small/medium screens; large ones roll off toward r_emissiveLightMaxReach)", 0.25f, 16.0f );
idCVar r_emissiveLightMaxReach( "r_emissiveLightMaxReach", "120", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "soft cap (world units) that a fill light's reach saturates toward, so big signs don't cast across the whole room; small/medium screens stay linear (r_emissiveLightRadius). Raise toward 512 for near-linear scaling", 32.0f, 512.0f );
idCVar r_emissiveLightFalloff( "r_emissiveLightFalloff", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far a screen fill light's glow reaches and how gently it fades: 0 = a tight, bright pool that drops off sharply, 1 = a soft glow that reaches further out. 0.5 = default (matches the shipped reach)", 0.0f, 1.0f );
idCVar r_emissiveLightSaturation( "r_emissiveLightSaturation", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how much of a screen fill light's sampled colour to keep: 0 = white, 1 = full screen hue. Lower reads as natural bleed, higher as a coloured spotlight", 0.0f, 1.0f );
idCVar r_emissiveLightLimit( "r_emissiveLightLimit", "24", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "max screen fill lights kept per view (nearest/biggest win); extras reap on the normal timeout. 0 = unlimited", 0, 256 );
idCVar r_emissiveLightSpread( "r_emissiveLightSpread", "1.60", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "width of the projected fill cone as a multiple of its reach: low = a tight beam, high = a wide near-hemisphere that wraps around the screen (but still clipped behind the mount)", 0.5f, 3.5f );
idCVar r_emissiveLightSpecular( "r_emissiveLightSpecular", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen fill lights add specular highlights (1, more visible on the weapon) or diffuse-only soft fill (0, calmer)" );

// DUDE: self-lit pickup items (armor, medkits, ammo lights, powerups) glow via additive
// material stages but cast no light in vanilla. This gives each one a very faint point
// light tinted from its glow texture — only noticeable in near-total darkness. Rides the
// emissive fill-light machinery above. Non-vanilla; enhancement backends only.
idCVar r_itemGlow( "r_itemGlow", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "self-lit pickup items (armor, medkits, ammo, powerups) cast a faint glow of their light's colour; 0 = vanilla (off), 1 = full faint glow, only visible in near darkness (non-vanilla; opengl3/Vulkan only)", 0.0f, 1.0f );

// DUDE: GTAO screen-space ambient occlusion. Darkens only the ambient light term
// (not direct/dynamic lights), so it stays correct as lighting changes and fixes
// Doom 3's flat/plastic model look. Non-vanilla; enhancement backends only.
// See docs/ssao-gtao.md.
// DUDE: GPU tessellation of enemy/prop meshes (Vulkan only; docs/tessellation.md).
// PN-triangle smoothing rounds the low-poly silhouettes of characters and props;
// off = bit-for-bit vanilla (no tess pipeline is built). GL3's 3.3 core context
// has no tessellation stages, so this is inert there.
idCVar r_tessellation( "r_tessellation", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "PN-triangle GPU tessellation that smooths enemy/prop mesh silhouettes (non-vanilla; Vulkan only)" );
idCVar r_tessLevel( "r_tessLevel", "5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "tessellation subdivision level for enemies/props (1 = flat, higher = smoother silhouettes at more GPU cost; capped at 32 / the device limit)", 1.0f, 32.0f );
idCVar r_tessMaxDist( "r_tessMaxDist", "160", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "view distance (world units) beyond which tessellation rolls back toward flat, an LOD/perf guard. 0 = uniform level everywhere", 0.0f, 8192.0f );
idCVar r_tessMinEdge( "r_tessMinEdge", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "minimum triangle edge length (world units) to tessellate; triangles finer than this stay flat. Now just a floor against degenerate slivers: the things it used to protect -- eyeballs, teeth, glasses -- are excluded by material, and a character's iris is held clear by r_tessEyeOffset instead. Raise it to trade facial detail for triangles (0.72 leaves roughly half a face flat). 0 = tessellate everything", 0.0f, 64.0f );
idCVar r_tessDisplace( "r_tessDisplace", "0.12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "height displacement on tessellated characters: amplitude, in world units, of the SIGNED height field pushed along the surface normal. Most stock characters ship the authored _h map their bump was baked from and that is used directly; materials without one get a height integrated out of the normal map. 0 = pure PN smoothing, positive reproduces the authored relief (pits recede, bumps rise), negative inverts it. Vulkan only", -8.0f, 8.0f );
// DUDE: a character's iris is a translucent `blend filter` decal at `sort decal`, sitting
// ~0.008 world units off the face skin, tinting the eye white painted into the face texture.
// PN tessellation bulges the skin between its vertices by up to ~0.1 units -- far more than
// that gap -- so the face crosses the card and the iris is occluded. The card is a separate
// mesh and cannot follow the deformation, so it is lifted clear instead. Tuned in-game:
// the worst-case bulge is ~0.1, but the iris sits where the face moves far less than that.
idCVar r_tessEyeOffset( "r_tessEyeOffset", "0.035", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far a character's iris is lifted off the face, in world units, so a tessellated face cannot bulge through it and hide the eye. Enough to clear the PN bulge where the iris actually sits; too much and the iris visibly floats. 0 = off (vanilla placement; eyes clip under tessellation). Vulkan only, and inert when r_tessellation is off", 0.0f, 2.0f );
idCVar r_tessDebug( "r_tessDebug", "0", CVAR_RENDERER | CVAR_BOOL, "diagnostic: print each material name accepted for tessellation once. Walk up to a mis-tessellated character and read the console to find the leaked material (Vulkan)" );

idCVar r_ssao( "r_ssao", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space ambient occlusion (GTAO) applied to the ambient light term; adds contact shadowing and depth to models (non-vanilla; opengl3/Vulkan only)" );
idCVar r_ssaoIntensity( "r_ssaoIntensity", "1.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO strength: scales the occlusion darkening (0 = none, 1.2 = default, higher = deeper creases)", 0.0f, 4.0f );
idCVar r_ssaoFloor( "r_ssaoFloor", "0.03", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO minimum ambient visibility: fully-occluded areas darken to at most this (0 = can reach black, 1 = no darkening). Keeps creases from crushing to black in dark scenes", 0.0f, 1.0f );
idCVar r_ssaoDirectLight( "r_ssaoDirectLight", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how strongly SSAO darkens direct (dynamic) light's diffuse, 0..1. Doom 3 has almost no ambient, so this is what makes AO visible in normal scenes. Lower it if AO looks baked-in under moving lights; 0 = ambient-only (most faithful)", 0.0f, 1.0f );
idCVar r_ssaoRadius( "r_ssaoRadius", "48", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO sampling radius in world units; larger reaches for broad occlusion, smaller keeps it to tight contact creases", 1.0f, 256.0f );
idCVar r_ssaoSlices( "r_ssaoSlices", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "SSAO horizon-search directions (slices) per pixel; more = smoother, less directional noise, more GPU cost", 1, 8 );
idCVar r_ssaoSteps( "r_ssaoSteps", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "SSAO samples marched along each direction; more = more accurate horizons at range, more GPU cost", 1, 12 );
idCVar r_ssaoResScale( "r_ssaoResScale", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO buffer resolution as a fraction of the screen (0.5 = half ... 1.0 = full); lower is faster and softer, upsampled bilaterally", 0.25f, 1.0f );
idCVar r_ssaoBentNormal( "r_ssaoBentNormal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "shade the ambient along the bent (average unoccluded) normal for directional occlusion, instead of a flat darkening" );
idCVar r_ssaoBentStrength( "r_ssaoBentStrength", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far to bias the ambient cube lookup from the surface normal toward the bent normal (0 = surface normal, 1 = fully bent). Only matters where there is ambient light; needs r_ssaoBentNormal", 0.0f, 1.0f );
idCVar r_ssaoNormalBuffer( "r_ssaoNormalBuffer", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "feed SSAO from a real bump-mapped normal G-buffer (an extra opaque geometry pass) instead of normals reconstructed from depth; picks up normal-map detail and removes faceting, at the cost of one geometry pass. 0 = reconstruct from depth (cheaper)" );
idCVar r_ssaoSpecular( "r_ssaoSpecular", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "also attenuate specular highlights in occluded areas (stronger anti-plastic, mild fidelity departure); helps in scenes with little ambient fill" );
idCVar r_ssaoMergeNormal( "r_ssaoMergeNormal", "1", CVAR_RENDERER | CVAR_BOOL, "Vulkan: produce the SSAO normal G-buffer (+ SSR rough/metal and velocity MRTs when wanted) inside the depth prepass — one opaque geometry pass instead of two (docs/ssao-normal-merge.md). Default on; 0 = the standalone RB_RHI_NormalPrepass, kept as the A/B + GL3 path" );
idCVar r_ssaoDebug( "r_ssaoDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "SSAO debug view: 0 = off, 1 = show the AO buffer, 2 = show bent normals, 3 = show the normal G-buffer", 0, 3 );
idCVar r_ssaoTemporal( "r_ssaoTemporal", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "accumulate SSAO across frames via camera reprojection: smooths the horizon-search noise and lets slices/steps run lower for the same look. Static-world reprojection (no motion vectors); ghosting is bounded by a neighbourhood clamp. Non-vanilla; opengl3/Vulkan only" );
idCVar r_ssaoTemporalFeedback( "r_ssaoTemporalFeedback", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO temporal history weight: fraction of the reprojected previous-frame AO kept each frame (higher = smoother/steadier but more latency and ghosting; 0 = no accumulation). Needs r_ssaoTemporal", 0.0f, 0.97f );
// Temporal-funded sample budget (docs/ssao-perf-optimization.md Phase 4). Verdict 2026-09-16:
// slightly softer than brute force, so it's preset-wired ON for every SSAO tier except the
// top two — Nightmare / Path Tracing keep the brute-force 5x10 look.
idCVar r_ssaoTemporalTrade( "r_ssaoTemporalTrade", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "SSAO temporal-funded sample budget: 1 = 4 slices x 6 steps with temporal accumulation forced ON (overriding r_ssaoSlices/Steps/Temporal) — cheaper than brute-force sampling, slightly softer. Preset-wired: on for Medium/High/Ultra, off for Nightmare/Path Tracing (brute force). Where the per-object velocity buffer exists (VK, r_motionVectors) the temporal reprojection tracks moving objects" );
idCVar r_temporalResetDist( "r_temporalResetDist", "128", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far (world units) the view origin may jump in one rendered frame before the shared temporal history (SSAO/SSR reprojection, and later motion vectors / FSR2) is treated as a discontinuity and reset, killing cross-cut / teleport ghosting. 0 disables the check (history never force-resets). opengl3/Vulkan only", 0.0f, 100000.0f );
idCVar r_motionVectors( "r_motionVectors", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Vulkan: write per-object screen-space motion vectors into a 3rd MRT of the normal G-buffer prepass (docs/fsr-temporal-pipeline.md R1/A2). Foundation for FSR2 and reduced temporal SSAO/SSR ghosting on moving objects. Forces the normal prepass on (an extra opaque pass) even when SSAO/SSR are off. VK-only; a no-op on the GL3/legacy backends (no float-colour render target)" );
idCVar r_mvDebug( "r_mvDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "motion-vector debug overlay (needs r_motionVectors): 0 = off, 1 = direction (red = +x, green = +y, grey = static), 2 = magnitude (black = no motion). STRAFE or walk forward over static geometry: the surfaces tint by depth (near > far); the tint flips when you reverse. A camera PAN concentrates velocity at the screen edges (perspective), so it is a poor test for the geometry", 0, 2 );
idCVar r_weaponMotionVectors( "r_weaponMotionVectors", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "give the first-person view weapon REAL motion vectors in the velocity prepass instead of a hard zero. Fixes FSR2 / temporal reprojection garbling the weapon and its GUI displays as they bob/sway while moving (the depth hack only compresses Z, so the screen-space xy velocity is valid; degenerate near-plane samples are still zeroed per-pixel). 1 = on (default), 0 = the old zero-velocity weapon (instant A/B). Needs r_motionVectors / r_fsr; VK-only" );
idCVar r_mvDebugScale( "r_mvDebugScale", "1", CVAR_RENDERER | CVAR_FLOAT, "live gain multiplier for the r_mvDebug overlay. Per-frame screen velocities are tiny, so crank this up (e.g. 4-10) to see slow object / camera motion clearly. Debug only", 0.0f, 200.0f );
// DUDE SSAO Phase 1: prefiltered linear-depth mip chain (docs/ssao-perf-optimization.md). The horizon
// search reads a coarser mip for farther steps, so far taps touch a small cache-local footprint instead
// of scattering across full-res _currentDepth. Visually near-identical; a GPU-time win that scales with
// r_ssaoRadius. Adds a cheap linearize + mip-gen pass per view. On by default; toggle for an A/B.
idCVar r_ssaoDepthMip( "r_ssaoDepthMip", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "SSAO: march the horizon search over a prefiltered linear-depth mip chain (far steps read coarse mips) instead of full-res depth. Cheaper on GPU at a wide radius, visually near-identical. 0 = full-res depth every tap (opengl3/Vulkan only)" );
idCVar r_ssaoDepthMipBias( "r_ssaoDepthMipBias", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO depth-mip LOD aggressiveness: LOD = log2(stepPixels * this). Higher drops to coarser mips sooner (faster, but coarse depth smears occlusion across silhouettes into halos); lower keeps steps on finer mips (sharper, keeps most of the speedup). Needs r_ssaoDepthMip", 0.05f, 4.0f );
idCVar r_ssaoDepthMipMaxLod( "r_ssaoDepthMipMaxLod", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "SSAO depth-mip coarseness cap: the horizon march never reads a mip coarser than this level, even for far taps. The coarsest mips (box-averaged depth) are where occlusion smears across silhouettes into halos, so a lower cap kills halos at a small speed cost; higher restores the last bit of speedup. Clamped to the built chain (<=5) and >=1 while r_ssaoDepthMip is on", 1, 5 );

// DUDE: baked ambient-occlusion (occlusion) maps. Per-material AO textures declared with
// the `occlusionmap` material keyword, multiplied into the ambient (and, scaled, direct-
// light diffuse) exactly like SSAO. Non-vanilla; enhancement backends only. Inert on stock
// assets, which never declare an occlusion stage. See docs/occlusion-maps.md.
idCVar r_occlusionMapPrecache( "r_occlusionMapPrecache", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "load a level's baked occlusion maps up front at level load instead of lazily on first sight (removes the per-model hitch the first time each entity is lit). Needs r_occlusionMaps; 1 = precache, 0 = lazy. opengl3/Vulkan only" );
idCVar r_occlusionMaps( "r_occlusionMaps", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "use per-material baked ambient-occlusion maps (the `occlusionmap` material stage) to darken creases in the ambient and direct-light diffuse. Complements SSAO; only affects materials that ship an occlusion map (none in stock Doom 3). Non-vanilla; opengl3/Vulkan only" );
idCVar r_occlusionMapScale( "r_occlusionMapScale", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "strength of baked occlusion maps on the ambient term, 0..1 (0 = off, 1 = the map at full darkening)", 0.0f, 1.0f );
idCVar r_occlusionMapDirect( "r_occlusionMapDirect", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how strongly baked occlusion maps darken direct (dynamic) light's diffuse, 0..1, as a fraction of r_occlusionMapScale. Doom 3 is mostly dynamic light, so this is what makes the map visible; lower it if AO looks baked-in under moving lights, 0 = ambient-only", 0.0f, 1.0f );
idCVar r_occlusionMapsAutoBake( "r_occlusionMapsAutoBake", "0", CVAR_RENDERER | CVAR_BOOL, "DEV: when a model-entity surface has no explicit or cached occlusion map, bake one on first sight (writes generated/aomaps, one-time hitch per model). Off by default; use the bakeAO/bakeAOFolder commands for offline baking" );

// DUDE: parallax occlusion mapping (non-vanilla; Vulkan enhancement). Per-pixel surface
// relief on materials that carry height data -- auto-captured from a `heightmap(...)` operand
// in the bump program (the source Doom 3 bakes its normals from), or an explicit `parallaxmap`
// stage. Off = bit-for-bit vanilla with zero extra resident textures: no height map is loaded
// unless r_parallax is set when the material is parsed. See docs/parallax.md.
// DUDE weapon-reload depth-of-field: as the weapon reloads the game eases
// r_weaponReloadFocus 0->1->0 (idPlayerView) and this blurs the world beyond the
// gun, focusing on the reload. Non-vanilla, opt-in, off by default; opengl3/Vulkan.
idCVar r_dof( "r_dof", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "weapon-reload depth-of-field: blur the world as the weapon reloads (opengl3/Vulkan). Non-vanilla, off by default" );
idCVar r_dofBlurRadius( "r_dofBlurRadius", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "weapon-reload DoF: max world blur radius in pixels at full focus", 0.0f, 64.0f );
idCVar r_dofFocusStart( "r_dofFocusStart", "0.35", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "weapon-reload DoF: scene raw-depth where the blur begins (the weapon stays sharp below this)", 0.0f, 1.0f );
idCVar r_dofFocusEnd( "r_dofFocusEnd", "0.72", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "weapon-reload DoF: scene raw-depth where the blur reaches full strength", 0.0f, 1.0f );
// game->renderer bridge, written by idPlayerView every frame (not a user knob):
// the 0..1 reload focus envelope the DoF pass scales its blur by.
idCVar r_weaponReloadFocus( "r_weaponReloadFocus", "0", CVAR_RENDERER | CVAR_FLOAT, "weapon-reload DoF strength (set by the game: eases 0->1->0 across the reload animation)" );
idCVar r_parallax( "r_parallax", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "parallax occlusion mapping: per-pixel surface relief on materials that carry height data (auto-captured from `heightmap(...)` in the bump program, or an explicit `parallaxmap` stage). Non-vanilla; opengl3/Vulkan. Takes effect on the next reloadDecls/map load" );
idCVar r_parallaxScale( "r_parallaxScale", "0.12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "global multiplier on per-material parallax height scale, 0..4 (0 = flat, higher exaggerates the relief; 0.12 = tuned default)", 0.0f, 4.0f );
idCVar r_parallaxMinSteps( "r_parallaxMinSteps", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "parallax occlusion mapping: height-field march steps when viewing head-on (cheaper). Ramps up to r_parallaxMaxSteps at grazing angles", 1, 32 );
idCVar r_parallaxMaxSteps( "r_parallaxMaxSteps", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "parallax occlusion mapping: height-field march steps at grazing angles (higher = fewer swimming artifacts, costlier). Loop is capped at 32", 1, 32 );
idCVar r_parallaxShadow( "r_parallaxShadow", "0.6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "parallax self-shadowing strength, 0..1 (0 = off): the marched relief casts contact shadows from the light direction. Adds a second (half-step) march per lit pixel. Only active when r_parallax is on", 0.0f, 1.0f );

// DUDE: dampen the cube-map ("sheen") reflection on glass etc. The enhancement
// backends light the scene brighter than the original renderer, so the environment
// reflection reads too strong; 0.7 (-30%) matches the legacy look, 1.0 is untouched.
idCVar r_gl3ReflectionScale( "r_gl3ReflectionScale", "0.7", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: brightness of cube-map glass reflections (1 = untouched; 0.7 compensates for the brighter enhanced scene; non-vanilla, opengl3/Vulkan only)", 0.0f, 2.0f );

// DUDE: decal sheen control. Texture mod packs give overlay decals a specularmap (and often an
// additive "texgen reflect" cube stage) so blood reads wet — but it also turns every splat and
// dirt stain into chrome under any light. Blood decals scale both sources by r_decalBloodSheen;
// every other translucent decal (dirt, stains, drips) goes matte with r_decalMatte. Inert on the
// stock assets: their overlay decals carry neither a specularmap nor a cube stage. Glass is not a
// decal although id and Raven sort window panes "sort decal" (R_DecalSheenScale).
idCVar r_decalBloodSheen( "r_decalBloodSheen", "0.35", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "sheen (specular highlight + cube reflection) on blood decals: 0 = matte, 1 = as authored by the material (non-vanilla, opengl3/Vulkan only; only mod-pack decals have any sheen to scale)", 0.0f, 1.0f );
idCVar r_decalMatte( "r_decalMatte", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "translucent non-blood decals (dirt, stains, drips) get no specular highlight and no cube reflection (non-vanilla, opengl3/Vulkan only; 0 = as authored by the material)" );

// DUDE: dirt on metal is not metal. Overlay decals (stock or mod) strip the PBR metalness of the
// surface under them by their coverage, so stains read matte against the plate around them.
idCVar r_decalMetalMask( "r_decalMetalMask", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how much an overlay decal's coverage removes the metalness of the surface under it: 0 = off, 1 = a fully opaque decal leaves bare dielectric (non-vanilla, opengl3/Vulkan only; needs r_pbr)", 0.0f, 1.0f );
idCVar r_decalRoughen( "r_decalRoughen", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how much an overlay decal's coverage roughens the surface under it, breaking up its highlight and its reflection: 0 = off, 1 = a fully opaque decal leaves a matte surface (non-vanilla, opengl3/Vulkan only; needs r_pbr)", 0.0f, 1.0f );
// DUDE wet blood film (docs/readme-changes.md 1.7): blood decals are lit as wet surfaces of their
// own, off relief generated from the splat itself. Defaults are the user's in-game tuning
// (2026-09-19). On by default, but it only ever runs where r_pbr does (High preset and up), so
// the Potato..Medium presets keep their matte, faithful blood.
idCVar r_decalWetBlood( "r_decalWetBlood", "0.057", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "wet blood: every blood decal - stock or mod, map-placed or spawned by the game - glints off the lights around it as a wet film with its own relief. Strength of the highlight, 0 = off (non-vanilla, opengl3/Vulkan only; needs r_pbr)", 0.0f, 0.5f );
idCVar r_decalWetBloodRelief( "r_decalWetBloodRelief", "0.17", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "wet blood: how strongly the film's surface follows the blood's own height (bulging edges, ripples inside). 0 = a flat film, which only glints at the exact mirror angle of a light", 0.0f, 1.0f );
idCVar r_decalWetBloodThreshold( "r_decalWetBloodThreshold", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "wet blood: how much blood a texel must carry before the film shows at all (0..1 of the decal's own coverage; the film feathers in over the next 0.25). Splat textures trail off into a mist too faint to see - without a floor the floor around a splat glints as if wet", 0.0f, 0.95f );
idCVar r_decalWetBloodRough( "r_decalWetBloodRough", "0.18", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "wet blood: roughness of the film. Lower = tighter, sharper glints (fresh blood); higher = a broad soft sheen (drying blood)", 0.05f, 1.0f );

/*
===================
R_DecalSheenScale

1 for everything that is not an overlay decal. Blood is told apart by the "blood" content flag
or by name (the packs tag only some of their blood materials). Opaque non-blood decals are left
alone: those are vents, wires, posters and bullet holes — real objects whose shine is intended.
Glass is left alone too: window panes are "sort decal" + translucent in the stock data (Doom 3's
outdoor_glass1, RoE's grid glass, 52 Quake 4 panes such as marineglass*), and their reflect-cube
stage, masked by their own makealpha smudges, IS the visible pane - r_decalMatte took it away.
They carry the "glass" surface type or, for a few Quake 4 ones ("materialType glass" only, and
RoE's d3exglass), "glass" in the name.
===================
*/
bool R_DecalIsBlood( const idMaterial *material ) {
	const char *name = material->GetName();
	if ( idStr::FindText( name, "glow", false ) >= 0 ) {
		return false;	// the alpha-blended "glow splats" are plasma / hell goo, not blood
	}
	return ( material->GetContentFlags() & CONTENTS_BLOOD )
		|| idStr::FindText( name, "blood", false ) >= 0 || idStr::FindText( name, "splat", false ) >= 0
		|| idStr::FindText( name, "smear", false ) >= 0 || idStr::FindText( name, "handprint", false ) >= 0;
}

float R_DecalSheenScale( const idMaterial *material ) {
	if ( material == NULL || material->GetSort() != SS_DECAL ) {
		return 1.0f;
	}
	if ( material->GetSurfaceType() == SURFTYPE_GLASS || idStr::FindText( material->GetName(), "glass", false ) >= 0 ) {
		return 1.0f;
	}
	if ( R_DecalIsBlood( material ) ) {
		return r_decalBloodSheen.GetFloat();
	}
	if ( material->Coverage() == MC_TRANSLUCENT && r_decalMatte.GetBool() ) {
		return 0.0f;
	}
	return 1.0f;
}

// DUDE: gate for the non-vanilla "Enhancements" (see tr_local.h). True for the
// RHI backends — opengl3 today, vulkan once it lands (Phase 4); the legacy ARB2
// path stays vanilla-faithful.
bool R_BackendSupportsEnhancements() {
	return glConfig.rhiBackend;
}

// True only when the active RHI is a backend with ray-query hardware support (Vulkan on
// an RTX/RDNA2+ GPU). Gates the RT-shadow menu toggles so they grey out on GL3 and on
// non-RT GPUs, where r_rtSunShadows / r_rtMovingLights are inert.
bool R_SupportsRayTracing() {
	if ( !glConfig.rhiBackend ) {
		return false;
	}
	rhi::RHI *r = rhi::GetRHI();
	return r != NULL && r->SupportsRayQuery();
}

// define qgl functions
#define QGLPROC(name, rettype, args) rettype (APIENTRYP q##name) args;
#include "renderer/qgl_proc.h"

void ( APIENTRY * qglMultiTexCoord2fARB )( GLenum texture, GLfloat s, GLfloat t );
void ( APIENTRY * qglMultiTexCoord2fvARB )( GLenum texture, GLfloat *st );
void ( APIENTRY * qglActiveTextureARB )( GLenum texture );
void ( APIENTRY * qglClientActiveTextureARB )( GLenum texture );

void (APIENTRY *qglTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);

void (APIENTRY * qglColorTableEXT)( int, int, int, int, int, const void * );

// EXT_stencil_two_side
PFNGLACTIVESTENCILFACEEXTPROC			qglActiveStencilFaceEXT;

// ARB_texture_compression
PFNGLCOMPRESSEDTEXIMAGE2DARBPROC		qglCompressedTexImage2DARB;
PFNGLGETCOMPRESSEDTEXIMAGEARBPROC		qglGetCompressedTexImageARB;

// ARB_vertex_buffer_object
PFNGLBINDBUFFERARBPROC					qglBindBufferARB;
PFNGLDELETEBUFFERSARBPROC				qglDeleteBuffersARB;
PFNGLGENBUFFERSARBPROC					qglGenBuffersARB;
PFNGLISBUFFERARBPROC					qglIsBufferARB;
PFNGLBUFFERDATAARBPROC					qglBufferDataARB;
PFNGLBUFFERSUBDATAARBPROC				qglBufferSubDataARB;
PFNGLGETBUFFERSUBDATAARBPROC			qglGetBufferSubDataARB;
PFNGLMAPBUFFERARBPROC					qglMapBufferARB;
PFNGLUNMAPBUFFERARBPROC					qglUnmapBufferARB;
PFNGLGETBUFFERPARAMETERIVARBPROC		qglGetBufferParameterivARB;
PFNGLGETBUFFERPOINTERVARBPROC			qglGetBufferPointervARB;

// ARB_vertex_program / ARB_fragment_program
PFNGLVERTEXATTRIBPOINTERARBPROC			qglVertexAttribPointerARB;
PFNGLENABLEVERTEXATTRIBARRAYARBPROC		qglEnableVertexAttribArrayARB;
PFNGLDISABLEVERTEXATTRIBARRAYARBPROC	qglDisableVertexAttribArrayARB;
PFNGLPROGRAMSTRINGARBPROC				qglProgramStringARB;
PFNGLBINDPROGRAMARBPROC					qglBindProgramARB;
PFNGLGENPROGRAMSARBPROC					qglGenProgramsARB;
PFNGLPROGRAMENVPARAMETER4FVARBPROC		qglProgramEnvParameter4fvARB;
PFNGLPROGRAMLOCALPARAMETER4FVARBPROC	qglProgramLocalParameter4fvARB;

// GL_EXT_depth_bounds_test
PFNGLDEPTHBOUNDSEXTPROC                 qglDepthBoundsEXT;

// DG: couldn't find any extension for this, it's supported in GL2.0 and newer, incl OpenGL ES2.0
PFNGLSTENCILOPSEPARATEPROC qglStencilOpSeparate;

// GL_ARB_debug_output
PFNGLDEBUGMESSAGECALLBACKARBPROC        qglDebugMessageCallbackARB;

// eez: This is a slight hack for letting us select the desired screenshot format in other functions
//  This is a hack to avoid adding another function parameter to idRenderSystem::TakeScreenshot(),
//  which would break the API of the DUDE SDK for mods.
//  Note that this is reset to -1 (which means: use value of r_screenshotFormat) at the end of
//  idRenderSystemLocal::TakeScreenshot(), so if your code wants to enforce a specific format,
//  it must set g_screenshotFormat accordingly before each call to TakeScreenshot().
int g_screenshotFormat = -1;

enum {
	// Not all GL.h header know about GL_DEBUG_SEVERITY_NOTIFICATION_*.
	QGL_DEBUG_SEVERITY_NOTIFICATION = 0x826B
};

/*
 * Callback function for debug output.
 */
static void APIENTRY
DebugCallback( GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
              const GLchar *message, const void *userParam )
{
	const char* sourceStr = "Source: Unknown";
	const char* typeStr = "Type: Unknown";
	const char* severityStr = "Severity: Unknown";

	switch (severity)
	{
#define SVRCASE(X, STR)  case GL_DEBUG_SEVERITY_ ## X ## _ARB : severityStr = STR; break;
		case QGL_DEBUG_SEVERITY_NOTIFICATION: return;
		SVRCASE(HIGH, "Severity: High")
		SVRCASE(MEDIUM, "Severity: Medium")
		SVRCASE(LOW, "Severity: Low")
#undef SVRCASE
	}

	switch (source)
	{
#define SRCCASE(X)  case GL_DEBUG_SOURCE_ ## X ## _ARB: sourceStr = "Source: " #X; break;
		SRCCASE(API);
		SRCCASE(WINDOW_SYSTEM);
		SRCCASE(SHADER_COMPILER);
		SRCCASE(THIRD_PARTY);
		SRCCASE(APPLICATION);
		SRCCASE(OTHER);
#undef SRCCASE
	}

	switch(type)
	{
#define TYPECASE(X)  case GL_DEBUG_TYPE_ ## X ## _ARB: typeStr = "Type: " #X; break;
		TYPECASE(ERROR);
		TYPECASE(DEPRECATED_BEHAVIOR);
		TYPECASE(UNDEFINED_BEHAVIOR);
		TYPECASE(PORTABILITY);
		TYPECASE(PERFORMANCE);
		TYPECASE(OTHER);
#undef TYPECASE
	}

	common->Warning( "GLDBG %s %s %s: %s\n", sourceStr, typeStr, severityStr, message );

}

/*
=================
R_CheckExtension
=================
*/
bool R_CheckExtension( const char *name ) {
	if ( !strstr( glConfig.extensions_string, name ) ) {
		common->Printf( "X..%s not found\n", name );
		return false;
	}

	common->Printf( "...using %s\n", name );
	return true;
}

/*
==================
R_CheckPortableExtensions

==================
*/
static void R_CheckPortableExtensions( void ) {
	glConfig.glVersion = atof( glConfig.version_string );

	// GL_ARB_multitexture
	glConfig.multitextureAvailable = R_CheckExtension( "GL_ARB_multitexture" );
	if ( glConfig.multitextureAvailable ) {
		qglMultiTexCoord2fARB = (void(APIENTRY *)(GLenum, GLfloat, GLfloat))GLimp_ExtensionPointer( "glMultiTexCoord2fARB" );
		qglMultiTexCoord2fvARB = (void(APIENTRY *)(GLenum, GLfloat *))GLimp_ExtensionPointer( "glMultiTexCoord2fvARB" );
		qglActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer( "glActiveTextureARB" );
		qglClientActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer( "glClientActiveTextureARB" );
		qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, (GLint *)&glConfig.maxTextureUnits );
		if ( glConfig.maxTextureUnits > MAX_MULTITEXTURE_UNITS ) {
			glConfig.maxTextureUnits = MAX_MULTITEXTURE_UNITS;
		}
		if ( glConfig.maxTextureUnits < 2 ) {
			glConfig.multitextureAvailable = false;	// shouldn't ever happen
		}
		qglGetIntegerv( GL_MAX_TEXTURE_COORDS_ARB, (GLint *)&glConfig.maxTextureCoords );
		qglGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS_ARB, (GLint *)&glConfig.maxTextureImageUnits );
	}

	// GL_ARB_texture_env_combine
	glConfig.textureEnvCombineAvailable = R_CheckExtension( "GL_ARB_texture_env_combine" );

	// GL_ARB_texture_cube_map
	glConfig.cubeMapAvailable = R_CheckExtension( "GL_ARB_texture_cube_map" );

	// GL_ARB_texture_env_dot3
	glConfig.envDot3Available = R_CheckExtension( "GL_ARB_texture_env_dot3" );

	// GL_ARB_texture_env_add
	glConfig.textureEnvAddAvailable = R_CheckExtension( "GL_ARB_texture_env_add" );

	// GL_ARB_texture_non_power_of_two
	glConfig.textureNonPowerOfTwoAvailable = R_CheckExtension( "GL_ARB_texture_non_power_of_two" );

	// GL_ARB_texture_compression + GL_S3_s3tc
	// DRI drivers may have GL_ARB_texture_compression but no GL_EXT_texture_compression_s3tc
	if ( R_CheckExtension( "GL_ARB_texture_compression" ) && R_CheckExtension( "GL_EXT_texture_compression_s3tc" ) ) {
		glConfig.textureCompressionAvailable = true;
		qglCompressedTexImage2DARB = (PFNGLCOMPRESSEDTEXIMAGE2DARBPROC)GLimp_ExtensionPointer( "glCompressedTexImage2DARB" );
		qglGetCompressedTexImageARB = (PFNGLGETCOMPRESSEDTEXIMAGEARBPROC)GLimp_ExtensionPointer( "glGetCompressedTexImageARB" );
		if ( R_CheckExtension( "GL_ARB_texture_compression_bptc" ) ) {
			glConfig.bptcTextureCompressionAvailable = true;
		}
	} else {
		glConfig.textureCompressionAvailable = false;
		glConfig.bptcTextureCompressionAvailable = false;
	}

	// GL_EXT_texture_filter_anisotropic
	glConfig.anisotropicAvailable = R_CheckExtension( "GL_EXT_texture_filter_anisotropic" );
	if ( glConfig.anisotropicAvailable ) {
		qglGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glConfig.maxTextureAnisotropy );
		common->Printf( "   maxTextureAnisotropy: %g\n", glConfig.maxTextureAnisotropy );
	} else {
		glConfig.maxTextureAnisotropy = 1;
	}

	// GL_EXT_texture_lod_bias
	// The actual extension is broken as specificed, storing the state in the texture unit instead
	// of the texture object.  The behavior in GL 1.4 is the behavior we use.
	if ( glConfig.glVersion >= 1.4 || R_CheckExtension( "GL_EXT_texture_lod" ) ) {
		common->Printf( "...using %s\n", "GL_1.4_texture_lod_bias" );
		glConfig.textureLODBiasAvailable = true;
	} else {
		common->Printf( "X..%s not found\n", "GL_1.4_texture_lod_bias" );
		glConfig.textureLODBiasAvailable = false;
	}

	// GL_EXT_shared_texture_palette
	glConfig.sharedTexturePaletteAvailable = R_CheckExtension( "GL_EXT_shared_texture_palette" );
	if ( glConfig.sharedTexturePaletteAvailable ) {
		qglColorTableEXT = ( void ( APIENTRY * ) ( int, int, int, int, int, const void * ) ) GLimp_ExtensionPointer( "glColorTableEXT" );
	}

	// GL_EXT_texture3D (not currently used for anything)
	glConfig.texture3DAvailable = R_CheckExtension( "GL_EXT_texture3D" );
	if ( glConfig.texture3DAvailable ) {
		qglTexImage3D =
			(void (APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *) )
			GLimp_ExtensionPointer( "glTexImage3D" );
	}

	// EXT_stencil_wrap
	// This isn't very important, but some pathological case might cause a clamp error and give a shadow bug.
	// Nvidia also believes that future hardware may be able to run faster with this enabled to avoid the
	// serialization of clamping.
	if ( R_CheckExtension( "GL_EXT_stencil_wrap" ) ) {
		tr.stencilIncr = GL_INCR_WRAP_EXT;
		tr.stencilDecr = GL_DECR_WRAP_EXT;
	} else {
		tr.stencilIncr = GL_INCR;
		tr.stencilDecr = GL_DECR;
	}

	// GL_EXT_stencil_two_side
	glConfig.twoSidedStencilAvailable = R_CheckExtension( "GL_EXT_stencil_two_side" );
	if ( glConfig.twoSidedStencilAvailable )
		qglActiveStencilFaceEXT = (PFNGLACTIVESTENCILFACEEXTPROC)GLimp_ExtensionPointer( "glActiveStencilFaceEXT" );

	if( glConfig.glVersion >= 2.0) {
		common->Printf( "...got GL2.0+ glStencilOpSeparate()\n" );
		qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)GLimp_ExtensionPointer( "glStencilOpSeparate" );
	} else if( R_CheckExtension( "GL_ATI_separate_stencil" ) ) {
		common->Printf( "...got glStencilOpSeparateATI() (GL_ATI_separate_stencil)\n" );
		// the ATI version of glStencilOpSeparate() has the same signature and should also
		// behave identical to the GL2 version (in Mesa3D it's just an alias)
		qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)GLimp_ExtensionPointer( "glStencilOpSeparateATI" );
	} else {
		common->Printf( "X..don't have glStencilOpSeparateATI() or (GL2.0+) glStencilOpSeparate()\n" );
		qglStencilOpSeparate = NULL;
	}

	// ARB_vertex_buffer_object
	glConfig.ARBVertexBufferObjectAvailable = R_CheckExtension( "GL_ARB_vertex_buffer_object" );
	if(glConfig.ARBVertexBufferObjectAvailable) {
		qglBindBufferARB = (PFNGLBINDBUFFERARBPROC)GLimp_ExtensionPointer( "glBindBufferARB");
		qglDeleteBuffersARB = (PFNGLDELETEBUFFERSARBPROC)GLimp_ExtensionPointer( "glDeleteBuffersARB");
		qglGenBuffersARB = (PFNGLGENBUFFERSARBPROC)GLimp_ExtensionPointer( "glGenBuffersARB");
		qglIsBufferARB = (PFNGLISBUFFERARBPROC)GLimp_ExtensionPointer( "glIsBufferARB");
		qglBufferDataARB = (PFNGLBUFFERDATAARBPROC)GLimp_ExtensionPointer( "glBufferDataARB");
		qglBufferSubDataARB = (PFNGLBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glBufferSubDataARB");
		qglGetBufferSubDataARB = (PFNGLGETBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glGetBufferSubDataARB");
		qglMapBufferARB = (PFNGLMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glMapBufferARB");
		qglUnmapBufferARB = (PFNGLUNMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glUnmapBufferARB");
		qglGetBufferParameterivARB = (PFNGLGETBUFFERPARAMETERIVARBPROC)GLimp_ExtensionPointer( "glGetBufferParameterivARB");
		qglGetBufferPointervARB = (PFNGLGETBUFFERPOINTERVARBPROC)GLimp_ExtensionPointer( "glGetBufferPointervARB");
	}

	// ARB_vertex_program
	glConfig.ARBVertexProgramAvailable = R_CheckExtension( "GL_ARB_vertex_program" );
	if (glConfig.ARBVertexProgramAvailable) {
		qglVertexAttribPointerARB = (PFNGLVERTEXATTRIBPOINTERARBPROC)GLimp_ExtensionPointer( "glVertexAttribPointerARB" );
		qglEnableVertexAttribArrayARB = (PFNGLENABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer( "glEnableVertexAttribArrayARB" );
		qglDisableVertexAttribArrayARB = (PFNGLDISABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer( "glDisableVertexAttribArrayARB" );
		qglProgramStringARB = (PFNGLPROGRAMSTRINGARBPROC)GLimp_ExtensionPointer( "glProgramStringARB" );
		qglBindProgramARB = (PFNGLBINDPROGRAMARBPROC)GLimp_ExtensionPointer( "glBindProgramARB" );
		qglGenProgramsARB = (PFNGLGENPROGRAMSARBPROC)GLimp_ExtensionPointer( "glGenProgramsARB" );
		qglProgramEnvParameter4fvARB = (PFNGLPROGRAMENVPARAMETER4FVARBPROC)GLimp_ExtensionPointer( "glProgramEnvParameter4fvARB" );
		qglProgramLocalParameter4fvARB = (PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)GLimp_ExtensionPointer( "glProgramLocalParameter4fvARB" );
	}

	// ARB_fragment_program
	if ( r_inhibitFragmentProgram.GetBool() ) {
		glConfig.ARBFragmentProgramAvailable = false;
	} else {
		glConfig.ARBFragmentProgramAvailable = R_CheckExtension( "GL_ARB_fragment_program" );
		if (glConfig.ARBFragmentProgramAvailable) {
			// these are the same as ARB_vertex_program
			qglProgramStringARB = (PFNGLPROGRAMSTRINGARBPROC)GLimp_ExtensionPointer( "glProgramStringARB" );
			qglBindProgramARB = (PFNGLBINDPROGRAMARBPROC)GLimp_ExtensionPointer( "glBindProgramARB" );
			qglProgramEnvParameter4fvARB = (PFNGLPROGRAMENVPARAMETER4FVARBPROC)GLimp_ExtensionPointer( "glProgramEnvParameter4fvARB" );
			qglProgramLocalParameter4fvARB = (PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)GLimp_ExtensionPointer( "glProgramLocalParameter4fvARB" );
		}
	}

	// check for minimum set
	if ( !glConfig.multitextureAvailable || !glConfig.textureEnvCombineAvailable || !glConfig.cubeMapAvailable
		|| !glConfig.envDot3Available ) {
			common->Error( "%s", common->GetLanguageDict()->GetString( "#str_06780" ) );
	}

	// GL_EXT_depth_bounds_test
	glConfig.depthBoundsTestAvailable = R_CheckExtension( "EXT_depth_bounds_test" );
	if ( glConfig.depthBoundsTestAvailable ) {
		qglDepthBoundsEXT = (PFNGLDEPTHBOUNDSEXTPROC)GLimp_ExtensionPointer( "glDepthBoundsEXT" );
	}

	// GL_ARB_debug_output
	glConfig.glDebugOutputAvailable = false;
	if ( glConfig.haveDebugContext ) {
		if ( strstr( glConfig.extensions_string, "GL_ARB_debug_output" ) ) {
			glConfig.glDebugOutputAvailable = true;
			qglDebugMessageCallbackARB = (PFNGLDEBUGMESSAGECALLBACKARBPROC)GLimp_ExtensionPointer( "glDebugMessageCallbackARB" );
			if ( r_glDebugContext.GetBool() ) {
				common->Printf( "...using GL_ARB_debug_output (r_glDebugContext is set)\n" );
				qglDebugMessageCallbackARB(DebugCallback, NULL);
				qglEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
			} else {
				common->Printf( "...found GL_ARB_debug_output, but not using it (r_glDebugContext is not set)\n" );
			}
		} else {
			common->Printf( "X..GL_ARB_debug_output not found\n" );
			qglDebugMessageCallbackARB = NULL;
			if ( r_glDebugContext.GetBool() ) {
				common->Warning( "r_glDebugContext is set, but can't be used because GL_ARB_debug_output is not supported" );
			}
		}
	} else {
		if ( strstr( glConfig.extensions_string, "GL_ARB_debug_output" ) ) {
			if ( r_glDebugContext.GetBool() ) {
				common->Printf( "...found GL_ARB_debug_output, but not using it (no debug context)\n" );
			} else {
				common->Printf( "...found GL_ARB_debug_output, but not using it (r_glDebugContext is not set)\n" );
			}
		} else {
			common->Printf( "X..GL_ARB_debug_output not found\n" );
		}
	}
}


/*
====================
R_GetModeInfo

r_mode is normally a small non-negative integer that
looks resolutions up in a table, but if it is set to -1,
the values from r_customWidth, and r_customHeight
will be used instead.
====================
*/
typedef struct vidmode_s {
	const char *description;
	int         width, height;
} vidmode_t;

vidmode_t r_vidModes[] = {
	{ "Mode  0: 320x240",		320,	240 },
	{ "Mode  1: 400x300",		400,	300 },
	{ "Mode  2: 512x384",		512,	384 },
	{ "Mode  3: 640x480",		640,	480 },
	{ "Mode  4: 800x600",		800,	600 },
	{ "Mode  5: 1024x768",		1024,	768 },
	{ "Mode  6: 1152x864",		1152,	864 },
	{ "Mode  7: 1280x1024",		1280,	1024 },
	{ "Mode  8: 1600x1200",		1600,	1200 },
	// DG: from here on: modes I added.
	{ "Mode  9: 1280x720",		1280,	720 },
	{ "Mode 10: 1366x768",		1366,	768 },
	{ "Mode 11: 1440x900",		1440,	900 },
	{ "Mode 12: 1400x1050",		1400,	1050 },
	{ "Mode 13: 1600x900",		1600,	900 },
	{ "Mode 14: 1680x1050",		1680,	1050 },
	{ "Mode 15: 1920x1080",		1920,	1080 },
	{ "Mode 16: 1920x1200",		1920,	1200 },
	{ "Mode 17: 2048x1152",		2048,	1152 },
	{ "Mode 18: 2560x1600",		2560,	1600 },
	{ "Mode 19: 3200x2400",		3200,	2400 },
	{ "Mode 20: 3840x2160",		3840,   2160 },
	{ "Mode 21: 4096x2304",		4096,   2304 },
	{ "Mode 22: 2880x1800",		2880,   1800 },
	{ "Mode 23: 2560x1440",		2560,   1440 },
	{ "Mode 24: 1440x1080",		1440,   1080 },
	{ "Mode 25: 1280x800",		1280,	800 },
	// 21:9 resolutions
	{ "Mode 26: 2560x1080",		2560,   1080 },
	{ "Mode 27: 3440x1440",		3440,   1440 },
	{ "Mode 28: 3840x1600",		3840,   1600 },
	{ "Mode 29: 5120x2160",		5120,   2160 },
	// 32:9 resolutions
	{ "Mode 30: 3840x1080",		3840,   1080 },
	{ "Mode 31: 5120x1440",		5120,   1440 },
	{ "Mode 32: 7680x2160",		7680,   2160 },
};
// DG: made this an enum so even stupid compilers accept it as array length below
enum {	s_numVidModes = sizeof( r_vidModes ) / sizeof( r_vidModes[0] ) };

bool R_GetModeInfo( int *width, int *height, int mode ) {
	vidmode_t	*vm;

	if ( mode < -1 ) {
		return false;
	}
	if ( mode >= s_numVidModes ) {
		return false;
	}

	if ( mode == -1 ) {
		*width = r_customWidth.GetInteger();
		*height = r_customHeight.GetInteger();
		return true;
	}

	vm = &r_vidModes[mode];

	if ( width ) {
		*width  = vm->width;
	}
	if ( height ) {
		*height = vm->height;
	}

	return true;
}

// DG: I added all this vidModeInfoPtr stuff, so I can have a second list of vidmodes
//     that are sorted (by width, height), instead of just r_mode index.
//     That way I can add modes without breaking r_mode, but still display them
//     sorted in the menu.

struct vidModePtr {
	vidmode_t* vidMode;
	int modeIndex;
};

static vidModePtr sortedVidModes[s_numVidModes];

static int vidModeCmp(const void* vm1, const void* vm2)
{
	const vidModePtr* v1 = static_cast<const vidModePtr*>(vm1);
	const vidModePtr* v2 = static_cast<const vidModePtr*>(vm2);

	// sort primarily by width, secondarily by height
	int wdiff = v1->vidMode->width - v2->vidMode->width;
	return (wdiff != 0) ? wdiff : (v1->vidMode->height - v2->vidMode->height);
}

static void initSortedVidModes()
{
	if(sortedVidModes[0].vidMode != NULL)
	{
		// already initialized
		return;
	}

	for(int i=0; i<s_numVidModes; ++i)
	{
		sortedVidModes[i].modeIndex = i;
		sortedVidModes[i].vidMode = &r_vidModes[i];
	}

	qsort(sortedVidModes, s_numVidModes, sizeof(vidModePtr), vidModeCmp);
}

// DG: the following two functions are part of a horrible hack in ChoiceWindow.cpp
//     to overwrite the default resolution list in the system options menu

// "r_custom*;640x480;800x600;1024x768;..."
idStr R_GetVidModeListString(bool addCustom)
{
	idStr ret = addCustom ? "r_custom*" : "";

	for(int i=0; i<s_numVidModes; ++i)
	{
		// for some reason, modes 0-2 are not used. maybe too small for GUI?
		if(sortedVidModes[i].modeIndex >= 3 && sortedVidModes[i].vidMode != NULL)
		{
			idStr modeStr;
			sprintf(modeStr, ";%dx%d", sortedVidModes[i].vidMode->width, sortedVidModes[i].vidMode->height);
			ret += modeStr;
		}
	}
	return ret;
}

// r_mode values for resolutions from R_GetVidModeListString(): "-1;3;4;5;..."
idStr R_GetVidModeValsString(bool addCustom)
{
	idStr ret = addCustom ? "-1" : ""; // for custom resolutions using r_customWidth/r_customHeight
	for(int i=0; i<s_numVidModes; ++i)
	{
		// for some reason, modes 0-2 are not used. maybe too small for GUI?
		if(sortedVidModes[i].modeIndex >= 3 && sortedVidModes[i].vidMode != NULL)
		{
			ret += ";";
			ret += sortedVidModes[i].modeIndex;
		}
	}
	return ret;
}
// DG end


/*
==================
R_InitOpenGL

This function is responsible for initializing a valid OpenGL subsystem
for rendering.  This is done by calling the system specific GLimp_Init,
which gives us a working OGL subsystem, then setting all necessary openGL
state, including images, vertex programs, and display lists.

Changes to the vertex cache size or smp state require a vid_restart.

If glConfig.isInitialized is false, no rendering can take place, but
all renderSystem functions will still operate properly, notably the material
and model information functions.
==================
*/
void R_InitOpenGL( void ) {
	GLint			temp;
	glimpParms_t	parms;
	int				i;

	common->Printf( "----- Initializing OpenGL -----\n" );

	if ( glConfig.isInitialized ) {
		common->FatalError( "R_InitOpenGL called while active" );
	}

	// Backend selection (docs/vulkan-port.md Phase 3 / Phase 4). "opengl3" runs
	// the GL 3.3 core backend; "vulkan"/"vulkan-rt" run the Vulkan backend
	// when the build has it AND the SDL probe passes — the cvar is archived, so
	// an unsupported system falls back to GL instead of wedging the boot.
	// coreProfile = "a GL core context is live"; rhiBackend = "the frontend
	// routes through the RHI executor" (both for opengl3, only the latter for
	// vulkan).
	bool vulkanMode = false;
	glConfig.coreProfile = false;
	glConfig.rhiBackend = false;
	if ( idStr::Icmp( r_graphicsAPI.GetString(), "opengl3" ) == 0 ) {
		glConfig.coreProfile = true;
		glConfig.rhiBackend = true;
		rhi::SetActiveBackend( rhi::BT_GL3 );
		common->Printf( "r_graphicsAPI opengl3: GL 3.3 core backend\n" );
	} else if ( idStr::Icmp( r_graphicsAPI.GetString(), "vulkan" ) == 0
	         || idStr::Icmp( r_graphicsAPI.GetString(), "vulkan-rt" ) == 0 ) {
#ifdef DHEWM3_VULKAN
		if ( GLimp_VulkanProbe() ) {
			vulkanMode = true;
			glConfig.rhiBackend = true;
			rhi::SetActiveBackend( rhi::BT_VULKAN );
			common->Printf( "r_graphicsAPI %s: Vulkan backend\n", r_graphicsAPI.GetString() );
		} else {
			common->Warning( "r_graphicsAPI \"%s\": this system can't do SDL+Vulkan (see probe warnings); using OpenGL", r_graphicsAPI.GetString() );
		}
#else
		common->Warning( "r_graphicsAPI \"%s\" requested but this build has no Vulkan support (rebuild with -DDHEWM3_VULKAN=ON); using OpenGL", r_graphicsAPI.GetString() );
#endif
	} else if ( idStr::Icmp( r_graphicsAPI.GetString(), "opengl" ) != 0 ) {
		common->Warning( "r_graphicsAPI \"%s\": unknown backend, using OpenGL (opengl / opengl3 / vulkan / vulkan-rt)", r_graphicsAPI.GetString() );
	}

	// DUDE: publish the active-backend flag as a read-only cvar so game code (which
	// can't see glConfig) can branch on it — e.g. the berserk vision captures at full
	// resolution on the RHI backends but leaves the legacy 512x256 path untouched.
	cvarSystem->SetCVarBool( "r_rhiActive", glConfig.rhiBackend );

	// in case we had an error while doing a tiled rendering
	tr.viewportOffset[0] = 0;
	tr.viewportOffset[1] = 0;

	initSortedVidModes();

	//
	// initialize OS specific portions of the renderSystem
	//
	for ( i = 0 ; i < 2 ; i++ ) {
		// set the parameters we are trying
		R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, r_mode.GetInteger() );

		parms.width = glConfig.vidWidth;
		parms.height = glConfig.vidHeight;
		parms.fullScreen = r_fullscreen.GetBool();
		parms.fullScreenDesktop = r_fullscreenDesktop.GetBool();
		parms.displayHz = r_displayRefresh.GetInteger();
		parms.multiSamples = r_multiSamples.GetInteger();
		parms.stereo = false;
		parms.coreProfile = glConfig.coreProfile;
		parms.vulkan = vulkanMode;

		if ( GLimp_Init( parms ) ) {
			// it worked
			break;
		}

		if ( i == 1 ) {
			common->FatalError( "Unable to initialize OpenGL" );
		}

		// if we failed, set everything back to "safe mode"
		// and try again
		r_mode.SetInteger( 3 );
		r_fullscreen.SetInteger( 0 );
		r_displayRefresh.SetInteger( 0 );
		r_multiSamples.SetInteger( 0 );
	}

#ifdef DHEWM3_VULKAN
	// Vulkan mode: no GL context exists — skip qgl loading and every GL query;
	// the backend's Init() fills the glConfig identity/limits fields from the
	// VkPhysicalDevice instead.
	if ( !vulkanMode )
#endif
	{
// load qgl function pointers
#define QGLPROC(name, rettype, args) \
	q##name = (rettype(APIENTRYP)args)GLimp_ExtensionPointer(#name); \
	if (!q##name) \
		common->FatalError("Unable to initialize OpenGL (%s)", #name);

#include "renderer/qgl_proc.h"
	}

	// input and sound systems need to be tied to the new window
	Sys_InitInput();
	soundSystem->InitHW();

#ifdef DHEWM3_VULKAN
	if ( vulkanMode ) {
		// a previous GL session (vid_restart backend switch) left the qgl
		// pointers loaded; zero them so any stray GL call is a clean NULL
		// crash / guardable check instead of stale-pointer UB without a context
#define QGLPROC(name, rettype, args) q##name = NULL;
#include "renderer/qgl_proc.h"

		// nothing downstream may strstr() a NULL; the backend overwrites the
		// identity strings and limits during Init()
		glConfig.vendor_string = "";
		glConfig.renderer_string = "";
		glConfig.version_string = "";
		glConfig.extensions_string = "";
		glConfig.maxTextureSize = 4096;
		glConfig.maxCubeMapSize = 1024;
		glConfig.vidMemMB = 0;

		glConfig.isInitialized = true;

		// the RHI enhancement paths check these before touching the image
		// generators; none of the GL image path is valid here yet (M2+)
		glConfig.multitextureAvailable = false;
		glConfig.allowARB2Path = true;
		glConfig.cubeMapAvailable = true;
		glConfig.texture3DAvailable = false;
		glConfig.textureNonPowerOfTwoAvailable = true;
		glConfig.textureCompressionAvailable = false;

		if ( !rhi::GetRHI()->Init() ) {
			common->FatalError( "Vulkan backend initialization failed (see warnings above) - start with +set r_graphicsAPI opengl3 to use GL" );
		}

		common->Printf( "Vulkan device: %s\n", glConfig.renderer_string );
		common->Printf( "Vulkan driver: %s\n", glConfig.version_string );

		// M6: ImGui via imgui_impl_vulkan — needs the live backend (instance,
		// device, swapchain render pass), so init here rather than at window
		// creation like the GL paths (glimp defers when windowIsVulkan)
		D3::ImGuiHooks::Init( GLimp_GetSDLWindow(), NULL );
	} else
#endif
	{
	// get our config strings
	glConfig.vendor_string = (const char *)qglGetString(GL_VENDOR);
	glConfig.renderer_string = (const char *)qglGetString(GL_RENDERER);
	glConfig.version_string = (const char *)qglGetString(GL_VERSION);
	if ( glConfig.coreProfile ) {
		// core profiles return NULL for glGetString(GL_EXTENSIONS); build the
		// list via glGetStringi so nothing downstream trips over a NULL
		static idStr coreExtensions;
		coreExtensions.Clear();
		typedef const GLubyte * (APIENTRYP PFNGETSTRINGI)( GLenum, GLuint );
		PFNGETSTRINGI getStringi = (PFNGETSTRINGI)GLimp_ExtensionPointer( "glGetStringi" );
		GLint numExt = 0;
		qglGetIntegerv( 0x821D /* GL_NUM_EXTENSIONS */, &numExt );
		if ( getStringi ) {
			for ( GLint e = 0; e < numExt; e++ ) {
				if ( e ) coreExtensions.Append( ' ' );
				coreExtensions.Append( (const char *)getStringi( GL_EXTENSIONS, e ) );
			}
		}
		glConfig.extensions_string = coreExtensions.c_str();
	} else {
		glConfig.extensions_string = (const char *)qglGetString(GL_EXTENSIONS);
	}

	// OpenGL driver constants
	qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &temp );
	glConfig.maxTextureSize = temp;

	// stubbed or broken drivers may have reported 0...
	if ( glConfig.maxTextureSize <= 0 ) {
		glConfig.maxTextureSize = 256;
	}

	// cube-map limit, used to clamp shadow-map cube resolution (GL 3.3 guarantees
	// at least 1024; real cards report 8192-32768). core since GL 1.3.
	temp = 0;
	qglGetIntegerv( GL_MAX_CUBE_MAP_TEXTURE_SIZE, &temp );
	glConfig.maxCubeMapSize = temp > 0 ? temp : 1024;

	// total VRAM via vendor extensions, used to auto-size the shadow-map cache
	// budget (r_shadowMapCacheMB -1 = half of this). No core query exists, so we
	// try NVIDIA then AMD and leave 0 (unknown) if neither is present.
	glConfig.vidMemMB = 0;
	if ( R_CheckExtension( "GL_NVX_gpu_memory_info" ) ) {
		GLint kb = 0;
		qglGetError();
		qglGetIntegerv( GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kb );
		if ( qglGetError() == GL_NO_ERROR && kb > 0 ) {
			glConfig.vidMemMB = kb / 1024;
		}
	}
	if ( glConfig.vidMemMB == 0 && R_CheckExtension( "GL_ATI_meminfo" ) ) {
		GLint info[4] = { 0, 0, 0, 0 };		// [0] = total free texture pool, KB
		qglGetError();
		qglGetIntegerv( GL_TEXTURE_FREE_MEMORY_ATI, info );
		if ( qglGetError() == GL_NO_ERROR && info[0] > 0 ) {
			glConfig.vidMemMB = info[0] / 1024;	// free (not total), but a usable proxy
		}
	}

	glConfig.isInitialized = true;

	common->Printf("OpenGL vendor: %s\n", glConfig.vendor_string );
	common->Printf("OpenGL renderer: %s\n", glConfig.renderer_string );
	common->Printf("OpenGL version: %s\n", glConfig.version_string );

	if ( glConfig.coreProfile ) {
		// DUDE GL3 backend: skip the legacy extension probing and ARB program
		// setup entirely — none of it is valid (or needed) on a core context.
		// SetBackEndRenderer is satisfied via allowARB2Path (the legacy draw
		// paths are gated off in RB_ExecuteBackEndCommands).
		glConfig.multitextureAvailable = false;
		glConfig.allowARB2Path = true;

		// Give the vertexCache real VBOs on the core profile. Core GL has no
		// client-side vertex/index arrays, so without persistent buffers every
		// visible surface has to be re-streamed to the GPU each frame (the
		// "Vertex cache is SLOW" path — a ~10x hit vs the classic backend, which
		// uploads static geometry once and reuses it). The vertexCache's VBO
		// machinery is written against the ARB_vertex_buffer_object entry points,
		// which are ABI-identical to the core buffer functions guaranteed by a
		// 3.x core context — wire the qgl*ARB pointers straight to them.
		qglBindBufferARB           = (PFNGLBINDBUFFERARBPROC)GLimp_ExtensionPointer( "glBindBuffer" );
		qglDeleteBuffersARB        = (PFNGLDELETEBUFFERSARBPROC)GLimp_ExtensionPointer( "glDeleteBuffers" );
		qglGenBuffersARB           = (PFNGLGENBUFFERSARBPROC)GLimp_ExtensionPointer( "glGenBuffers" );
		qglIsBufferARB             = (PFNGLISBUFFERARBPROC)GLimp_ExtensionPointer( "glIsBuffer" );
		qglBufferDataARB           = (PFNGLBUFFERDATAARBPROC)GLimp_ExtensionPointer( "glBufferData" );
		qglBufferSubDataARB        = (PFNGLBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glBufferSubData" );
		qglGetBufferSubDataARB     = (PFNGLGETBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer( "glGetBufferSubData" );
		qglMapBufferARB            = (PFNGLMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glMapBuffer" );
		qglUnmapBufferARB          = (PFNGLUNMAPBUFFERARBPROC)GLimp_ExtensionPointer( "glUnmapBuffer" );
		qglGetBufferParameterivARB = (PFNGLGETBUFFERPARAMETERIVARBPROC)GLimp_ExtensionPointer( "glGetBufferParameteriv" );
		qglGetBufferPointervARB    = (PFNGLGETBUFFERPOINTERVARBPROC)GLimp_ExtensionPointer( "glGetBufferPointerv" );
		glConfig.ARBVertexBufferObjectAvailable =
			qglGenBuffersARB && qglBindBufferARB && qglBufferDataARB
			&& qglBufferSubDataARB && qglDeleteBuffersARB;

		// core GL can only source indices from a bound ELEMENT_ARRAY_BUFFER, so
		// keep static index data resident in VBOs too (the front end fills
		// tri->indexCache only when this is set); otherwise indices would still
		// have to be streamed every frame.
		if ( glConfig.ARBVertexBufferObjectAvailable ) {
			r_useIndexBuffers.SetBool( true );
		}

		// the image path needs compressed uploads (retail pk4s ship .dds) —
		// glCompressedTexImage2D is core since 1.3, S3TC is an extension even
		// on core contexts; glTexImage3D likewise core
		qglCompressedTexImage2DARB = (PFNGLCOMPRESSEDTEXIMAGE2DARBPROC)GLimp_ExtensionPointer( "glCompressedTexImage2D" );
		qglGetCompressedTexImageARB = (PFNGLGETCOMPRESSEDTEXIMAGEARBPROC)GLimp_ExtensionPointer( "glGetCompressedTexImage" );
		qglTexImage3D = (void (APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *))GLimp_ExtensionPointer( "glTexImage3D" );
		glConfig.textureCompressionAvailable = qglCompressedTexImage2DARB != NULL
			&& strstr( glConfig.extensions_string, "GL_EXT_texture_compression_s3tc" ) != NULL;
		glConfig.bptcTextureCompressionAvailable = glConfig.textureCompressionAvailable
			&& strstr( glConfig.extensions_string, "GL_ARB_texture_compression_bptc" ) != NULL;

		// stencil shadow support: wrap ops are core since GL 1.4 and
		// glStencilOpSeparate since 2.0 (single-pass two-sided volumes)
		tr.stencilIncr = GL_INCR_WRAP_EXT;
		tr.stencilDecr = GL_DECR_WRAP_EXT;
		qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)GLimp_ExtensionPointer( "glStencilOpSeparate" );

		// capabilities that are core since GL 1.3/2.0 but normally only set by
		// the (skipped) extension probing — the image generators early-out
		// without these (an empty _normalCubeMap kills all diffuse lighting)
		glConfig.cubeMapAvailable = true;
		glConfig.texture3DAvailable = qglTexImage3D != NULL;
		glConfig.textureNonPowerOfTwoAvailable = true;

		// Anisotropic filtering: same story — the image path gates the TexParameter on
		// anisotropicAvailable and ChangeTextureFilter clamps image_anisotropy to
		// maxTextureAnisotropy, both left at 0/false here without the probe, so AF would be
		// dead on GL3 and the settings slider greyed out. GL_EXT_texture_filter_anisotropic
		// (ARB_ variant core since 4.6) is present on effectively every GL 3.3 driver.
		glConfig.anisotropicAvailable =
			strstr( glConfig.extensions_string, "GL_EXT_texture_filter_anisotropic" ) != NULL
			|| strstr( glConfig.extensions_string, "GL_ARB_texture_filter_anisotropic" ) != NULL;
		glConfig.maxTextureAnisotropy = 1;
		if ( glConfig.anisotropicAvailable ) {
			qglGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glConfig.maxTextureAnisotropy );
		}

		// Phase 3 Chunk B: bring up the backend proper — core function
		// pointers, GLSL program cache, per-draw UBO ring, VAOs. Runs again
		// after vid_restart with the fresh context.
		if ( !rhi::GetRHI()->Init() ) {
			common->Error( "GL3 backend initialization failed (see warnings above)" );
		}
	} else {
		// recheck all the extensions (FIXME: this might be dangerous)
		R_CheckPortableExtensions();

		// parse our vertex and fragment programs, possibly disably support for
		// one of the paths if there was an error
		R_ARB2_Init();

		cmdSystem->AddCommand( "reloadARBprograms", R_ReloadARBPrograms_f, CMD_FL_RENDERER, "reloads ARB programs" );
		R_ReloadARBPrograms_f( idCmdArgs() );
	}
	}	// end of the !vulkanMode GL init block

	// allocate the vertex array range or vertex objects
	// (Vulkan: ARBVertexBufferObjectAvailable stays false → CPU-side cache)
	vertexCache.Init();

	// select which renderSystem we are going to use
	r_renderer.SetModified();
	tr.SetBackEndRenderer();

	// allocate the frame data, which may be more if smp is enabled
	R_InitFrameData();

	// Reset our gamma
	r_gammaInShader.ClearModified();
	if ( r_gammaInShader.GetBool() ) {
		common->Printf( "Will apply r_gamma and r_brightness in shaders (r_gammaInShader 1)\n" );
	} else {
		common->Printf( "Will apply r_gamma and r_brightness in hardware (possibly on all screens; r_gammaInShader 0)\n" );
		R_SetColorMappings();
	}

#ifdef _WIN32
	static bool glCheck = false;
	if ( !glCheck && win32.osversion.dwMajorVersion == 6 ) {
		glCheck = true;
		if ( !idStr::Icmp( glConfig.vendor_string, "Microsoft" ) && idStr::FindText( glConfig.renderer_string, "OpenGL-D3D" ) != -1 ) {
			if ( cvarSystem->GetCVarBool( "r_fullscreen" ) ) {
				cmdSystem->BufferCommandText( CMD_EXEC_NOW, "vid_restart partial windowed\n" );
				Sys_GrabMouseCursor( false );
			}
			int ret = MessageBox( NULL, "Please install OpenGL drivers from your graphics hardware vendor to run " GAME_NAME ".\nYour OpenGL functionality is limited.",
				"Insufficient OpenGL capabilities", MB_OKCANCEL | MB_ICONWARNING | MB_TASKMODAL );
			if ( ret == IDCANCEL ) {
				cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
				cmdSystem->ExecuteCommandBuffer();
			}
			if ( cvarSystem->GetCVarBool( "r_fullscreen" ) ) {
				cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
			}
		}
	}
#endif
}

/*
==================
GL_CheckErrors
==================
*/
void GL_CheckErrors( void ) {
	int		err;
	char	s[64];
	int		i;

	// DUDE Phase 4: no GL context under the Vulkan backend — the qgl pointers
	// were never loaded (validation layers play this role there)
	if ( qglGetError == NULL ) {
		return;
	}

	// check for up to 10 errors pending
	for ( i = 0 ; i < 10 ; i++ ) {
		err = qglGetError();
		if ( err == GL_NO_ERROR ) {
			return;
		}
		switch( err ) {
			case GL_INVALID_ENUM:
				strcpy( s, "GL_INVALID_ENUM" );
				break;
			case GL_INVALID_VALUE:
				strcpy( s, "GL_INVALID_VALUE" );
				break;
			case GL_INVALID_OPERATION:
				strcpy( s, "GL_INVALID_OPERATION" );
				break;
			case GL_STACK_OVERFLOW:
				strcpy( s, "GL_STACK_OVERFLOW" );
				break;
			case GL_STACK_UNDERFLOW:
				strcpy( s, "GL_STACK_UNDERFLOW" );
				break;
			case GL_OUT_OF_MEMORY:
				strcpy( s, "GL_OUT_OF_MEMORY" );
				break;
			default:
				idStr::snPrintf( s, sizeof(s), "%i", err);
				break;
		}

		if ( !r_ignoreGLErrors.GetBool() ) {
			common->Printf( "GL_CheckErrors: %s\n", s );
		}
	}
}

/*
=====================
R_ReloadSurface_f

Reload the material displayed by r_showSurfaceInfo
=====================
*/
static void R_ReloadSurface_f( const idCmdArgs &args ) {
	modelTrace_t mt;
	idVec3 start, end;

	// start far enough away that we don't hit the player model
	start = tr.primaryView->renderView.vieworg + tr.primaryView->renderView.viewaxis[0] * 16;
	end = start + tr.primaryView->renderView.viewaxis[0] * 1000.0f;
	if ( !tr.primaryWorld->Trace( mt, start, end, 0.0f, false ) ) {
		return;
	}

	common->Printf( "Reloading %s\n", mt.material->GetName() );

	// reload the decl
	mt.material->base->Reload();

	// reload any images used by the decl
	mt.material->ReloadImages( false );
}

/*
=====================
R_ReloadPbrTable_f

DUDE PBR (docs/pbr-materials.md Phase B): re-read the pbr table files and
re-apply the lookup to every already-parsed material, so entries in
pbr/pbr_overrides.cfg can be tuned live in-game without a decl reload.
=====================
*/
static void R_ReloadPbrTable_f( const idCmdArgs &args ) {
	int applied = R_PbrTableReloadApply();
	common->Printf( "reloadPbrTable: re-applied to %d parsed materials\n", applied );
}

/*
=====================
R_PbrPickCrosshairMaterial

Trace from the primary view and return the material under the crosshair — the same
pick r_showSurfaceInfo / reloadSurface use — for the in-game PBR material editor
(Com_DrawPbrMaterialEditor). NULL if there's no view yet or the trace misses.
=====================
*/
const idMaterial *R_PbrPickCrosshairMaterial( void ) {
	if ( !tr.primaryView || !tr.primaryWorld ) {
		return NULL;
	}
	modelTrace_t mt;
	idVec3 start = tr.primaryView->renderView.vieworg + tr.primaryView->renderView.viewaxis[0] * 16;
	idVec3 end = start + tr.primaryView->renderView.viewaxis[0] * 1000.0f;
	// skip decal overlays first: grime like textures/decals/stainwall sits coplanar
	// in front of the wall and isn't lit, so editing its PBR does nothing — land on
	// the lit surface behind it. Fall back to a normal trace if the ray hits only
	// decals (a standalone decal with nothing behind), so it's still selectable.
	if ( tr.primaryWorld->TraceExt( mt, start, end, 0.0f, false, false, true ) ) {
		return mt.material;
	}
	if ( tr.primaryWorld->Trace( mt, start, end, 0.0f, false ) ) {
		return mt.material;
	}
	return NULL;
}



/*
==============
R_ListModes_f
==============
*/
static void R_ListModes_f( const idCmdArgs &args ) {
	int i;

	common->Printf( "\n" );
	for ( i = 0; i < s_numVidModes; i++ ) {
		common->Printf( "%s\n", r_vidModes[i].description );
	}
	common->Printf( "\n" );
}

/*
==============
R_ListParallaxMaps_f

DUDE (docs/parallax.md): list every material stage that captured a parallax height
source, with its scale and the resolved height texture. Phase-A verification that
auto-capture picked the right `_h.tga` on stock assets. Requires r_parallax to have
been set when the materials were parsed (run `reloadDecls` after enabling it), since
the height maps are only loaded then. Optional substring filters by material name.
==============
*/
static void R_ListParallaxMaps_f( const idCmdArgs &args ) {
	if ( !r_parallax.GetBool() ) {
		common->Printf( "r_parallax is 0 -- no height maps are captured. Set r_parallax 1 and run reloadDecls.\n" );
		return;
	}

	// idStr::Filter globs, so wrap a bare word in wildcards to act as a substring match
	// (`listParallaxMaps rock` -> `*rock*`); an arg that already has wildcards is used as-is.
	idStr filter;
	if ( args.Argc() > 1 ) {
		filter = args.Argv( 1 );
		if ( filter.Find( '*' ) < 0 && filter.Find( '?' ) < 0 ) {
			filter = idStr( "*" ) + filter + "*";
		}
	}

	int withParallax = 0;
	int total = declManager->GetNumDecls( DECL_MATERIAL );
	for ( int i = 0; i < total; i++ ) {
		const idMaterial *mat = declManager->MaterialByIndex( i, false );
		if ( !mat ) {
			continue;
		}
		if ( filter.Length() && !idStr::Filter( filter, mat->GetName(), false ) ) {
			continue;
		}
		for ( int s = 0; s < mat->GetNumStages(); s++ ) {
			const shaderStageExtra_t &extra = mat->GetStageExtra( mat->GetStage( s ) );
			if ( !extra.parallaxImage ) {
				continue;
			}
			common->Printf( "%-44s scale %4.1f  %s\n", mat->GetName(), extra.parallaxScale,
							extra.parallaxImage->imgName.c_str() );
			withParallax++;
		}
	}
	common->Printf( "%i parallax height stage(s) across %i material(s)\n", withParallax, total );
}



/*
=============
R_TestImage_f

Display the given image centered on the screen.
testimage <number>
testimage <filename>
=============
*/
void R_TestImage_f( const idCmdArgs &args ) {
	int imageNum;

	if ( tr.testVideo ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if ( args.Argc() != 2 ) {
		return;
	}

	if ( idStr::IsNumeric( args.Argv(1) ) ) {
		imageNum = atoi( args.Argv(1) );
		if ( imageNum >= 0 && imageNum < globalImages->images.Num() ) {
			tr.testImage = globalImages->images[imageNum];
		}
	} else {
		tr.testImage = globalImages->ImageFromFile( args.Argv( 1 ), TF_DEFAULT, false, TR_REPEAT, TD_DEFAULT );
	}
}

/*
=============
R_TestVideo_f

Plays the cinematic file in a testImage
=============
*/
void R_TestVideo_f( const idCmdArgs &args ) {
	if ( tr.testVideo ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if ( args.Argc() < 2 ) {
		return;
	}

	tr.testImage = globalImages->ImageFromFile( "_scratch", TF_DEFAULT, false, TR_REPEAT, TD_DEFAULT );
	tr.testVideo = idCinematic::Alloc();
	tr.testVideo->InitFromFile( args.Argv( 1 ), true );

	cinData_t	cin;
	cin = tr.testVideo->ImageForTime( 0 );
	if ( !cin.image ) {
		delete tr.testVideo;
		tr.testVideo = NULL;
		tr.testImage = NULL;
		return;
	}

	common->Printf( "%i x %i images\n", cin.imageWidth, cin.imageHeight );

	int	len = tr.testVideo->AnimationLength();
	common->Printf( "%5.1f seconds of video\n", len * 0.001 );

	tr.testVideoStartTime = tr.primaryRenderView.time * 0.001;

	// try to play the matching wav file
	idStr	wavString = args.Argv( ( args.Argc() == 2 ) ? 1 : 2 );
	wavString.StripFileExtension();
	wavString = wavString + ".wav";
	session->sw->PlayShaderDirectly( wavString.c_str() );
}

static int R_QsortSurfaceAreas( const void *a, const void *b ) {
	const idMaterial	*ea, *eb;
	int	ac, bc;

	ea = *(idMaterial **)a;
	if ( !ea->EverReferenced() ) {
		ac = 0;
	} else {
		ac = ea->GetSurfaceArea();
	}
	eb = *(idMaterial **)b;
	if ( !eb->EverReferenced() ) {
		bc = 0;
	} else {
		bc = eb->GetSurfaceArea();
	}

	if ( ac < bc ) {
		return -1;
	}
	if ( ac > bc ) {
		return 1;
	}

	return idStr::Icmp( ea->GetName(), eb->GetName() );
}


/*
===================
R_ReportSurfaceAreas_f

Prints a list of the materials sorted by surface area
===================
*/
void R_ReportSurfaceAreas_f( const idCmdArgs &args ) {
	int		i, count;
	idMaterial	**list;

	count = declManager->GetNumDecls( DECL_MATERIAL );
	list = (idMaterial **)_alloca( count * sizeof( *list ) );

	for ( i = 0 ; i < count ; i++ ) {
		list[i] = (idMaterial *)declManager->DeclByIndex( DECL_MATERIAL, i, false );
	}

	qsort( list, count, sizeof( list[0] ), R_QsortSurfaceAreas );

	// skip over ones with 0 area
	for ( i = 0 ; i < count ; i++ ) {
		if ( list[i]->GetSurfaceArea() > 0 ) {
			break;
		}
	}

	// DUDE: the per-triangle area walk is gated behind developer at model load (Model.cpp
	// FinishSurfaces) -- say so instead of printing an empty list
	if ( i == count ) {
		common->Printf( "no surface areas accumulated (only totalled at map load, and only with developer 1)\n" );
		return;
	}

	for ( ; i < count ; i++ ) {
		// report size in "editor blocks"
		int	blocks = list[i]->GetSurfaceArea() / 4096.0;
		common->Printf( "%7i %s\n", blocks, list[i]->GetName() );
	}
}

/*
===================
R_ReportImageDuplication_f

Checks for images with the same hash value and does a better comparison
===================
*/
void R_ReportImageDuplication_f( const idCmdArgs &args ) {
	int		i, j;

	common->Printf( "Images with duplicated contents:\n" );

	int	count = 0;

	for ( i = 0 ; i < globalImages->images.Num() ; i++ ) {
		idImage	*image1 = globalImages->images[i];

		if ( image1->isPartialImage ) {
			// ignore background loading stubs
			continue;
		}
		if ( image1->generatorFunction ) {
			// ignore procedural images
			continue;
		}
		if ( image1->cubeFiles != CF_2D ) {
			// ignore cube maps
			continue;
		}
		if ( image1->defaulted ) {
			continue;
		}
		byte	*data1;
		int		w1, h1;

		R_LoadImageProgram( image1->imgName, &data1, &w1, &h1, NULL );

		for ( j = 0 ; j < i ; j++ ) {
			idImage	*image2 = globalImages->images[j];

			if ( image2->isPartialImage ) {
				continue;
			}
			if ( image2->generatorFunction ) {
				continue;
			}
			if ( image2->cubeFiles != CF_2D ) {
				continue;
			}
			if ( image2->defaulted ) {
				continue;
			}
			if ( image1->imageHash != image2->imageHash ) {
				continue;
			}
			if ( image2->uploadWidth != image1->uploadWidth
				|| image2->uploadHeight != image1->uploadHeight ) {
				continue;
			}
			if ( !idStr::Icmp( image1->imgName, image2->imgName ) ) {
				// ignore same image-with-different-parms
				continue;
			}

			byte	*data2;
			int		w2, h2;

			R_LoadImageProgram( image2->imgName, &data2, &w2, &h2, NULL );

			if ( w2 != w1 || h2 != h1 ) {
				R_StaticFree( data2 );
				continue;
			}

			if ( memcmp( data1, data2, w1*h1*4 ) ) {
				R_StaticFree( data2 );
				continue;
			}

			R_StaticFree( data2 );

			common->Printf( "%s == %s\n", image1->imgName.c_str(), image2->imgName.c_str() );
			session->UpdateScreen( true );
			count++;
			break;
		}

		R_StaticFree( data1 );
	}
	common->Printf( "%i / %i collisions\n", count, globalImages->images.Num() );
}

/*
==============================================================================

						THROUGHPUT BENCHMARKING

==============================================================================
*/

/*
================
R_RenderingFPS
================
*/
static float R_RenderingFPS( const renderView_t *renderView ) {
	qglFinish();

	int		start = Sys_Milliseconds();
	static const int SAMPLE_MSEC = 1000;
	int		end;
	int		count = 0;

	while( 1 ) {
		// render
		renderSystem->BeginFrame( glConfig.vidWidth, glConfig.vidHeight );
		tr.primaryWorld->RenderScene( renderView );
		renderSystem->EndFrame( NULL, NULL );
		qglFinish();
		count++;
		end = Sys_Milliseconds();
		if ( end - start > SAMPLE_MSEC ) {
			break;
		}
	}

	float fps = count * 1000.0 / ( end - start );

	return fps;
}

/*
================
R_Benchmark_f
================
*/
void R_Benchmark_f( const idCmdArgs &args ) {
	float	fps, msec;
	renderView_t	view;

	if ( !tr.primaryView ) {
		common->Printf( "No primaryView for benchmarking\n" );
		return;
	}
	view = tr.primaryRenderView;

	for ( int size = 100 ; size >= 10 ; size -= 10 ) {
		r_screenFraction.SetInteger( size );
		fps = R_RenderingFPS( &view );
		int	kpix = glConfig.vidWidth * glConfig.vidHeight * ( size * 0.01 ) * ( size * 0.01 ) * 0.001;
		msec = 1000.0 / fps;
		common->Printf( "kpix: %4i  msec:%5.1f fps:%5.1f\n", kpix, msec, fps );
	}

	// enable r_singleTriangle 1 while r_screenFraction is still at 10
	r_singleTriangle.SetBool( 1 );
	fps = R_RenderingFPS( &view );
	msec = 1000.0 / fps;
	common->Printf( "single tri  msec:%5.1f fps:%5.1f\n", msec, fps );
	r_singleTriangle.SetBool( 0 );
	r_screenFraction.SetInteger( 100 );

	// enable r_skipRenderContext 1
	r_skipRenderContext.SetBool( true );
	fps = R_RenderingFPS( &view );
	msec = 1000.0 / fps;
	common->Printf( "no context  msec:%5.1f fps:%5.1f\n", msec, fps );
	r_skipRenderContext.SetBool( false );
}


/*
==============================================================================

						SCREEN SHOTS

==============================================================================
*/

/*
====================
R_ReadTiledPixels

Allows the rendering of an image larger than the actual window by
tiling it into window-sized chunks and rendering each chunk separately

If ref isn't specified, the full session UpdateScreen will be done.
====================
*/
void R_ReadTiledPixels( int width, int height, byte *buffer, renderView_t *ref = NULL ) {
	// M6: the Vulkan backend serves RB_RHI_CaptureNextSwap through the RHI
	// readback (rhiBackend path below); only a qgl-less legacy path is broken
	if ( qglReadPixels == NULL && !glConfig.rhiBackend ) {
		common->Warning( "screenshots need the readback path (no GL and no RHI backend)" );
		memset( buffer, 0, width * height * 3 );
		return;
	}
	// include extra space for OpenGL padding to word boundaries
	byte	*temp = (byte *)R_StaticAlloc( (glConfig.vidWidth+3) * glConfig.vidHeight * 3 );

	int	oldWidth = glConfig.vidWidth;
	int oldHeight = glConfig.vidHeight;

	tr.tiledViewport[0] = width;
	tr.tiledViewport[1] = height;

	// disable scissor, so we don't need to adjust all those rects
	r_useScissor.SetBool( false );

	for ( int xo = 0 ; xo < width ; xo += oldWidth ) {
		for ( int yo = 0 ; yo < height ; yo += oldHeight ) {
			tr.viewportOffset[0] = -xo;
			tr.viewportOffset[1] = -yo;

			// DUDE GL3 backend: capture GL_BACK at swap time — front-buffer
			// reads below return garbage on composited desktops
			if ( glConfig.rhiBackend ) {
				RB_RHI_CaptureNextSwap( temp );
			}

			if ( ref ) {
				tr.BeginFrame( oldWidth, oldHeight );
				tr.primaryWorld->RenderScene( ref );
				tr.EndFrame( NULL, NULL );
			} else {
				session->UpdateScreen(false);
			}

			int w = oldWidth;
			if ( xo + w > width ) {
				w = width - xo;
			}
			int h = oldHeight;
			if ( yo + h > height ) {
				h = height - yo;
			}

			if ( glConfig.rhiBackend ) {
				// already filled by the executor at swap; nothing to read here
			} else if ( glConfig.isWayland ) {
				// DG: Native Wayland (=> not XWayland) doesn't seem to support reading
				//     from the front buffer - screenshot is black then..
				//     So just read from the default (probably back-) buffer
				qglReadPixels( 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, temp );
			} else {
				// DG: It's probably better to restore the glReadBuffer mode after reading the pixels..
				//     (at least with XWayland on GNOME changing resolutions is wonky when not doing this)
				GLint oldReadBuf = GL_BACK;
				qglGetIntegerv( GL_READ_BUFFER, &oldReadBuf );
				qglReadBuffer( GL_FRONT );

				qglReadPixels( 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, temp );

				qglReadBuffer( oldReadBuf );
			}

			int	row = ( w * 3 + 3 ) & ~3;		// OpenGL pads to dword boundaries

			for ( int y = 0 ; y < h ; y++ ) {
				memcpy( buffer + ( ( yo + y )* width + xo ) * 3,
					temp + y * row, w * 3 );
			}
		}
	}

	r_useScissor.SetBool( true );

	tr.viewportOffset[0] = 0;
	tr.viewportOffset[1] = 0;
	tr.tiledViewport[0] = 0;
	tr.tiledViewport[1] = 0;

	R_StaticFree( temp );

	glConfig.vidWidth = oldWidth;
	glConfig.vidHeight = oldHeight;
}

/*
==================
WriteScreenshotForSTBIW

Callback to each stbi_write_* function
==================
*/
static void WriteScreenshotForSTBIW(void* context, void* data, int size)
{
	idFile* f = (idFile*)context;
	f->Write(data, size);
}

/*
==================
TakeScreenshot

Move to tr_imagefiles.c...

Will automatically tile render large screen shots if necessary
Downsample is the number of steps to mipmap the image before saving it
If ref == NULL, session->updateScreen will be used
==================
*/
void idRenderSystemLocal::TakeScreenshot( int width, int height, const char *fileName, int blends, renderView_t *ref ) {
	byte		*buffer, *swapBuffer;
	int			i, j;

	takingScreenshot = true;

	int	pix = width * height;
	int lineSize = width * 3;

	buffer = (byte *)R_StaticAlloc(pix*3);
	swapBuffer = (byte*)R_StaticAlloc(lineSize);

	if ( blends <= 1 ) {
		R_ReadTiledPixels( width, height, buffer, ref );
	} else {
		unsigned short *shortBuffer = (unsigned short *)R_StaticAlloc(pix*2*3);
		memset( shortBuffer, 0, pix*2*3 );

		// enable anti-aliasing jitter
		r_jitter.SetBool( true );

		for ( i = 0 ; i < blends ; i++ ) {
			R_ReadTiledPixels( width, height, buffer, ref );

			for ( j = 0 ; j < pix*3 ; j++ ) {
				shortBuffer[j] += buffer[j];
			}
		}

		// divide back to bytes
		for ( i = 0 ; i < pix*3 ; i++ ) {
			buffer[i] = shortBuffer[i] / blends;
		}

		R_StaticFree( shortBuffer );
		r_jitter.SetBool( false );
	}

	// The buffer is upside down, we need to flip it the right way.
	for (i = 0; i < height / 2; ++i) {
		byte* line1 = &buffer[i * lineSize];
		byte* line2 = &buffer[(height - i - 1) * lineSize];
		memcpy(swapBuffer, line1, lineSize);
		memcpy(line1, line2, lineSize);
		memcpy(line2, swapBuffer, lineSize);
	}
	
	idFile* f;
	if (strstr(fileName, "viewnote")) {
		f = fileSystem->OpenFileWrite( fileName, "fs_cdpath" );
	}
	else {
		f = fileSystem->OpenFileWrite( fileName );
	}

	// If no specific format is requested, default to using the CVar value.
	if (g_screenshotFormat == -1) {
		g_screenshotFormat = cvarSystem->GetCVarInteger( "r_screenshotFormat" );
	}

	switch (g_screenshotFormat) {
		default:
			stbi_write_tga_to_func( WriteScreenshotForSTBIW, f, width, height, 3, buffer );
			break;
		case 1:
			stbi_write_bmp_to_func( WriteScreenshotForSTBIW, f, width, height, 3, buffer);
			break;
		case 2:
			stbi_write_png_compression_level = idMath::ClampInt( 0, 9, r_screenshotPngCompression.GetInteger() );
			stbi_write_png_to_func( WriteScreenshotForSTBIW, f, width, height, 3, buffer, 3 * width );
			break;
		case 3:
			stbi_write_jpg_to_func( WriteScreenshotForSTBIW, f, width, height, 3, buffer, idMath::ClampInt( 1, 100, r_screenshotJpgQuality.GetInteger() ) );
			break;
	}

	g_screenshotFormat = -1;

	fileSystem->CloseFile(f);

	R_StaticFree( buffer );
	R_StaticFree( swapBuffer );

	takingScreenshot = false;

}


/*
==================
R_ScreenshotFilename

Returns a filename with digits appended
if we have saved a previous screenshot, don't scan
from the beginning, because recording demo avis can involve
thousands of shots
==================
*/
void R_ScreenshotFilename( int &lastNumber, const char *base, idStr &fileName ) {
	int	a,b,c,d, e;

	bool fsrestrict = cvarSystem->GetCVarBool( "fs_restrict" );
	cvarSystem->SetCVarBool( "fs_restrict", false );

	int format = cvarSystem->GetCVarInteger("r_screenshotFormat");

	lastNumber++;
	if ( lastNumber > 99999 ) {
		lastNumber = 99999;
	}
	for ( ; lastNumber < 99999 ; lastNumber++ ) {
		int	frac = lastNumber;

		a = frac / 10000;
		frac -= a*10000;
		b = frac / 1000;
		frac -= b*1000;
		c = frac / 100;
		frac -= c*100;
		d = frac / 10;
		frac -= d*10;
		e = frac;

		switch (format) {
			default:
				sprintf(fileName, "%s%i%i%i%i%i.tga", base, a, b, c, d, e);
				break;
			case 1:
				sprintf(fileName, "%s%i%i%i%i%i.bmp", base, a, b, c, d, e);
				break;
			case 2:
				sprintf(fileName, "%s%i%i%i%i%i.png", base, a, b, c, d, e);
				break;
			case 3:
				sprintf(fileName, "%s%i%i%i%i%i.jpg", base, a, b, c, d, e);
				break;
		}
		
		if ( lastNumber == 99999 ) {
			break;
		}
		int len = fileSystem->ReadFile( fileName, NULL, NULL );
		if ( len <= 0 ) {
			break;
		}
		// check again...
	}
	cvarSystem->SetCVarBool( "fs_restrict", fsrestrict );
}

/*
==================
R_BlendedScreenShot

screenshot
screenshot [filename]
screenshot [width] [height]
screenshot [width] [height] [samples]
==================
*/
#define	MAX_BLENDS	256	// to keep the accumulation in shorts
void R_ScreenShot_f( const idCmdArgs &args ) {
	static int lastNumber = 0;
	idStr checkname;

	int width = glConfig.vidWidth;
	int height = glConfig.vidHeight;
	int	blends = 0;

	switch ( args.Argc() ) {
	case 1:
		width = glConfig.vidWidth;
		height = glConfig.vidHeight;
		blends = 1;
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	case 2:
		width = glConfig.vidWidth;
		height = glConfig.vidHeight;
		blends = 1;
		checkname = args.Argv( 1 );
		break;
	case 3:
		width = atoi( args.Argv( 1 ) );
		height = atoi( args.Argv( 2 ) );
		blends = 1;
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	case 4:
		width = atoi( args.Argv( 1 ) );
		height = atoi( args.Argv( 2 ) );
		blends = atoi( args.Argv( 3 ) );
		if ( blends < 1 ) {
			blends = 1;
		}
		if ( blends > MAX_BLENDS ) {
			blends = MAX_BLENDS;
		}
		R_ScreenshotFilename( lastNumber, "screenshots/shot", checkname );
		break;
	default:
		common->Printf( "usage: screenshot\n       screenshot <filename>\n       screenshot <width> <height>\n       screenshot <width> <height> <blends>\n" );
		return;
	}

	// put the console away
	console->Close();

	tr.TakeScreenshot( width, height, checkname, blends, NULL );

	common->Printf( "Wrote %s\n", checkname.c_str() );
}

/*
===============
R_StencilShot
Save out a screenshot showing the stencil buffer expanded by 16x range
===============
*/
void R_StencilShot( void ) {
	// DUDE Phase 4 M1: GL readback only; nothing to read under Vulkan yet
	if ( qglReadPixels == NULL ) {
		common->Warning( "stencilShot is not supported on the Vulkan backend yet" );
		return;
	}
	byte		*buffer;
	int			i, c;

	int	width = tr.GetScreenWidth();
	int	height = tr.GetScreenHeight();

	int	pix = width * height;

	c = pix * 3 + 18;
	buffer = (byte *)Mem_Alloc(c);
	memset (buffer, 0, 18);

	byte *byteBuffer = (byte *)Mem_Alloc(pix);

	qglReadPixels( 0, 0, width, height, GL_STENCIL_INDEX , GL_UNSIGNED_BYTE, byteBuffer );

	for ( i = 0 ; i < pix ; i++ ) {
		buffer[18+i*3] =
		buffer[18+i*3+1] =
			//		buffer[18+i*3+2] = ( byteBuffer[i] & 15 ) * 16;
		buffer[18+i*3+2] = byteBuffer[i];
	}

	// fill in the header (this is vertically flipped, which qglReadPixels emits)
	buffer[2] = 2;		// uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;	// pixel size

	fileSystem->WriteFile( "screenshots/stencilShot.tga", buffer, c, "fs_savepath" );

	Mem_Free( buffer );
	Mem_Free( byteBuffer );
}

/*
==================
R_EnvShot_f

envshot <basename>

Saves out env/<basename>_ft.tga, etc
==================
*/
void R_EnvShot_f( const idCmdArgs &args ) {
	idStr		fullname;
	const char	*baseName;
	int			i;
	idMat3		axis[6];
	renderView_t	ref;
	viewDef_t	primary;
	int			blends;
	const char	*extensions[6] =  { "_px.tga", "_nx.tga", "_py.tga", "_ny.tga",
		"_pz.tga", "_nz.tga" };
	int			size;

	if ( args.Argc() != 2 && args.Argc() != 3 && args.Argc() != 4 ) {
		common->Printf( "USAGE: envshot <basename> [size] [blends]\n" );
		return;
	}
	baseName = args.Argv( 1 );

	blends = 1;
	if ( args.Argc() == 4 ) {
		size = atoi( args.Argv( 2 ) );
		blends = atoi( args.Argv( 3 ) );
	} else if ( args.Argc() == 3 ) {
		size = atoi( args.Argv( 2 ) );
		blends = 1;
	} else {
		size = 256;
		blends = 1;
	}

	if ( !tr.primaryView ) {
		common->Printf( "No primary view.\n" );
		return;
	}

	primary = *tr.primaryView;

	memset( &axis, 0, sizeof( axis ) );
	axis[0][0][0] = 1;
	axis[0][1][2] = 1;
	axis[0][2][1] = 1;

	axis[1][0][0] = -1;
	axis[1][1][2] = -1;
	axis[1][2][1] = 1;

	axis[2][0][1] = 1;
	axis[2][1][0] = -1;
	axis[2][2][2] = -1;

	axis[3][0][1] = -1;
	axis[3][1][0] = -1;
	axis[3][2][2] = 1;

	axis[4][0][2] = 1;
	axis[4][1][0] = -1;
	axis[4][2][1] = 1;

	axis[5][0][2] = -1;
	axis[5][1][0] = 1;
	axis[5][2][1] = 1;

	for ( i = 0 ; i < 6 ; i++ ) {
		ref = primary.renderView;
		ref.x = ref.y = 0;
		ref.fov_x = ref.fov_y = 90;
		// DUDE: virtual 640x480 units, NOT pixels — RenderViewToViewport scales
		// by crop/640 x crop/480. The old glConfig.vidWidth/Height here only
		// worked on 4:3 windows; on widescreen the viewport overshot the square
		// capture target and every face saved a crop of its 90-degree view, so
		// the resulting cubemap seams never matched (see R_BakeGlassProbe_f).
		ref.width = SCREEN_WIDTH;
		ref.height = SCREEN_HEIGHT;
		ref.viewaxis = axis[i];
		sprintf( fullname, "env/%s%s", baseName, extensions[i] );
		g_screenshotFormat = 0;
		tr.TakeScreenshot( size, size, fullname, blends, &ref );
	}

	common->Printf( "Wrote %s, etc\n", fullname.c_str() );
}

/*
==================
R_GlassProbeBasePath

DUDE glass probes (docs/ssr.md): extensionless base path for a map area's probe
faces, fs_savepath-relative — envprobes/<map>/area<N>. The bake command appends
the native cube suffixes (_px.tga etc, the same layout envshot writes and the
cubeMap keyword loads).
==================
*/
void R_GlassProbeBasePath( const char *mapName, int area, idStr &out ) {
	idStr clean = mapName;
	clean.BackSlashesToSlashes();
	clean.StripFileExtension();
	if ( clean.Icmpn( "maps/", 5 ) == 0 ) {
		clean = clean.Right( clean.Length() - 5 );
	}
	clean.Replace( "/", "_" );
	out = va( "envprobes/%s/area%03d", clean.c_str(), area );
}

/*
==================
R_BakeGlassProbe_f

DUDE glass probes (docs/ssr.md): bakes the environment probe of the portal area
the viewer stands in — six envshot-style 90-degree captures from the current
eye position, written as a native-layout cubemap under fs_savepath. Glass
surfaces then reflect the actual room at any angle (probe as the SSR-miss
fallback) instead of Doom 3's generic env/gen* blur. Usually queued
automatically when a probe is missing (r_ssrGlassProbeBake); run
"bakeGlassProbe force" to re-capture from a better vantage or after visual
settings changes. The view weapon is kept out of the capture
(tr.takingEnvProbe, see R_AddModelSurfaces).
==================
*/
void R_BakeGlassProbe_f( const idCmdArgs &args ) {
	const char	*extensions[6] = { "_px.tga", "_nx.tga", "_py.tga", "_ny.tga",
		"_pz.tga", "_nz.tga" };


	const bool force = args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "force" ) == 0;
	if ( !tr.primaryView || !tr.primaryWorld ) {
		common->Printf( "bakeGlassProbe: no primary view\n" );
		return;
	}
	const int area = tr.primaryWorld->PointInArea( tr.primaryView->renderView.vieworg );
	if ( area < 0 ) {
		common->Printf( "bakeGlassProbe: view origin is not in any portal area\n" );
		return;
	}
	idStr base;
	R_GlassProbeBasePath( tr.primaryWorld->mapName, area, base );

	ID_TIME_T ts;
	if ( !force && fileSystem->ReadFile( va( "%s_px.tga", base.c_str() ), NULL, &ts ) >= 0 ) {
		// already baked (auto-queue raced a manual bake); just make sure it's loaded
		RB_RHI_InvalidateGlassProbe( area );
		return;
	}

	const int size = idMath::ClampInt( 64, 1024, r_ssrGlassProbeSize.GetInteger() );

	// the same six axis sets envshot uses for the native cube layout
	idMat3 axis[6];
	memset( &axis, 0, sizeof( axis ) );
	axis[0][0][0] = 1;	axis[0][1][2] = 1;	axis[0][2][1] = 1;
	axis[1][0][0] = -1;	axis[1][1][2] = -1;	axis[1][2][1] = 1;
	axis[2][0][1] = 1;	axis[2][1][0] = -1;	axis[2][2][2] = -1;
	axis[3][0][1] = -1;	axis[3][1][0] = -1;	axis[3][2][2] = 1;
	axis[4][0][2] = 1;	axis[4][1][0] = -1;	axis[4][2][1] = 1;
	axis[5][0][2] = -1;	axis[5][1][0] = 1;	axis[5][2][1] = 1;

	const viewDef_t primary = *tr.primaryView;
	renderView_t ref;
	idStr fullname;

	tr.takingEnvProbe = true;
	for ( int i = 0; i < 6; i++ ) {
		ref = primary.renderView;
		ref.x = ref.y = 0;
		ref.fov_x = ref.fov_y = 90;
		// renderView sizes are VIRTUAL 640x480 units: RenderViewToViewport
		// scales them by crop/640 x crop/480, so the full virtual screen maps
		// exactly onto the square tiled capture target. Passing real pixel
		// sizes here (the vanilla envshot recipe) breaks on widescreen windows:
		// the viewport overshoots the target and each face saves only a crop of
		// its 90-degree view, so the cube faces can't tile (seam mismatch).
		ref.width = SCREEN_WIDTH;
		ref.height = SCREEN_HEIGHT;
		ref.viewaxis = axis[i];
		fullname = va( "%s%s", base.c_str(), extensions[i] );
		g_screenshotFormat = 0;		// probes are always TGA (the cube loader's format)
		tr.TakeScreenshot( size, size, fullname.c_str(), 1, &ref );
	}
	tr.takingEnvProbe = false;

	RB_RHI_InvalidateGlassProbe( area );
	common->Printf( "bakeGlassProbe: wrote %s_*.tga\n", base.c_str() );
}

//============================================================================

static idMat3		cubeAxis[6];


/*
==================
R_SampleCubeMap
==================
*/
void R_SampleCubeMap( const idVec3 &dir, int size, byte *buffers[6], byte result[4] ) {
	float	adir[3];
	int		axis, x, y;

	adir[0] = fabs(dir[0]);
	adir[1] = fabs(dir[1]);
	adir[2] = fabs(dir[2]);

	if ( dir[0] >= adir[1] && dir[0] >= adir[2] ) {
		axis = 0;
	} else if ( -dir[0] >= adir[1] && -dir[0] >= adir[2] ) {
		axis = 1;
	} else if ( dir[1] >= adir[0] && dir[1] >= adir[2] ) {
		axis = 2;
	} else if ( -dir[1] >= adir[0] && -dir[1] >= adir[2] ) {
		axis = 3;
	} else if ( dir[2] >= adir[1] && dir[2] >= adir[2] ) {
		axis = 4;
	} else {
		axis = 5;
	}

	float	fx = (dir * cubeAxis[axis][1]) / (dir * cubeAxis[axis][0]);
	float	fy = (dir * cubeAxis[axis][2]) / (dir * cubeAxis[axis][0]);

	fx = -fx;
	fy = -fy;
	x = size * 0.5 * (fx + 1);
	y = size * 0.5 * (fy + 1);
	if ( x < 0 ) {
		x = 0;
	} else if ( x >= size ) {
		x = size-1;
	}
	if ( y < 0 ) {
		y = 0;
	} else if ( y >= size ) {
		y = size-1;
	}

	result[0] = buffers[axis][(y*size+x)*4+0];
	result[1] = buffers[axis][(y*size+x)*4+1];
	result[2] = buffers[axis][(y*size+x)*4+2];
	result[3] = buffers[axis][(y*size+x)*4+3];
}

/*
==================
R_MakeAmbientMap_f

R_MakeAmbientMap_f <basename> [size]

Saves out env/<basename>_amb_ft.tga, etc
==================
*/
void R_MakeAmbientMap_f( const idCmdArgs &args ) {
	idStr fullname;
	const char	*baseName;
	int			i;
	const char	*extensions[6] =  { "_px.tga", "_nx.tga", "_py.tga", "_ny.tga",
		"_pz.tga", "_nz.tga" };
	int			outSize;
	byte		*buffers[6];
	int			width, height;

	if ( args.Argc() != 2 && args.Argc() != 3 ) {
		common->Printf( "USAGE: ambientshot <basename> [size]\n" );
		return;
	}
	baseName = args.Argv( 1 );

	if ( args.Argc() == 3 ) {
		outSize = atoi( args.Argv( 2 ) );
	} else {
		outSize = 32;
	}

	memset( &cubeAxis, 0, sizeof( cubeAxis ) );
	cubeAxis[0][0][0] = 1;
	cubeAxis[0][1][2] = 1;
	cubeAxis[0][2][1] = 1;

	cubeAxis[1][0][0] = -1;
	cubeAxis[1][1][2] = -1;
	cubeAxis[1][2][1] = 1;

	cubeAxis[2][0][1] = 1;
	cubeAxis[2][1][0] = -1;
	cubeAxis[2][2][2] = -1;

	cubeAxis[3][0][1] = -1;
	cubeAxis[3][1][0] = -1;
	cubeAxis[3][2][2] = 1;

	cubeAxis[4][0][2] = 1;
	cubeAxis[4][1][0] = -1;
	cubeAxis[4][2][1] = 1;

	cubeAxis[5][0][2] = -1;
	cubeAxis[5][1][0] = 1;
	cubeAxis[5][2][1] = 1;

	// read all of the images
	for ( i = 0 ; i < 6 ; i++ ) {
		sprintf( fullname, "env/%s%s", baseName, extensions[i] );
		common->Printf( "loading %s\n", fullname.c_str() );
		session->UpdateScreen();
		R_LoadImage( fullname, &buffers[i], &width, &height, NULL, true );
		if ( !buffers[i] ) {
			common->Printf( "failed.\n" );
			for ( i-- ; i >= 0 ; i-- ) {
				Mem_Free( buffers[i] );
			}
			return;
		}
	}

	// resample with hemispherical blending
	int	samples = 1000;

	byte	*outBuffer = (byte *)_alloca( outSize * outSize * 4 );

	for ( int map = 0 ; map < 2 ; map++ ) {
		for ( i = 0 ; i < 6 ; i++ ) {
			for ( int x = 0 ; x < outSize ; x++ ) {
				for ( int y = 0 ; y < outSize ; y++ ) {
					idVec3	dir;
					float	total[3];

					dir = cubeAxis[i][0] + -( -1 + 2.0*x/(outSize-1) ) * cubeAxis[i][1] + -( -1 + 2.0*y/(outSize-1) ) * cubeAxis[i][2];
					dir.Normalize();
					total[0] = total[1] = total[2] = 0;
	//samples = 1;
					float	limit = map ? 0.95 : 0.25;		// small for specular, almost hemisphere for ambient

					for ( int s = 0 ; s < samples ; s++ ) {
						// pick a random direction vector that is inside the unit sphere but not behind dir,
						// which is a robust way to evenly sample a hemisphere
						idVec3	test;
						while( 1 ) {
							for ( int j = 0 ; j < 3 ; j++ ) {
								test[j] = -1 + 2 * (rand()&0x7fff)/(float)0x7fff;
							}
							if ( test.Length() > 1.0 ) {
								continue;
							}
							test.Normalize();
							if ( test * dir > limit ) {	// don't do a complete hemisphere
								break;
							}
						}
						byte	result[4];
	//test = dir;
						R_SampleCubeMap( test, width, buffers, result );
						total[0] += result[0];
						total[1] += result[1];
						total[2] += result[2];
					}
					outBuffer[(y*outSize+x)*4+0] = total[0] / samples;
					outBuffer[(y*outSize+x)*4+1] = total[1] / samples;
					outBuffer[(y*outSize+x)*4+2] = total[2] / samples;
					outBuffer[(y*outSize+x)*4+3] = 255;
				}
			}

			if ( map == 0 ) {
				sprintf( fullname, "env/%s_amb%s", baseName, extensions[i] );
			} else {
				sprintf( fullname, "env/%s_spec%s", baseName, extensions[i] );
			}
			common->Printf( "writing %s\n", fullname.c_str() );
			session->UpdateScreen();
			R_WriteTGA( fullname, outBuffer, outSize, outSize );
		}
	}

	for ( i = 0 ; i < 6 ; i++ ) {
		if ( buffers[i] ) {
			Mem_Free( buffers[i] );
		}
	}
}

//============================================================================


/*
===============
R_SetColorMappings
===============
*/
void R_SetColorMappings( void ) {

	if ( r_gammaInShader.GetBool() ) {
		// nothing to do here
		return;
	}

	int		i, j;
	float	g, b;
	int		inf;
	unsigned short gammaTable[256];

	b = r_brightness.GetFloat();
	g = r_gamma.GetFloat();

	for ( i = 0; i < 256; i++ ) {
		j = i * b;
		if (j > 255) {
			j = 255;
		}

		if ( g == 1 ) {
			inf = (j<<8) | j;
		} else {
			inf = 0xffff * pow ( j/255.0f, 1.0f / g ) + 0.5f;
		}
		if (inf < 0) {
			inf = 0;
		}
		if (inf > 0xffff) {
			inf = 0xffff;
		}

		gammaTable[i] = inf;
	}

	GLimp_SetGamma( gammaTable, gammaTable, gammaTable );
}


/*
================
GfxInfo_f
================
*/
static void GfxInfo_f( const idCmdArgs &args ) {
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	const char* fsmode = fsstrings[r_fullscreen.GetBool()];
	if ( r_fullscreen.GetBool() && r_fullscreenDesktop.GetBool() )
		fsmode = "desktop-fullscreen";

	common->Printf( "\nGL_VENDOR: %s\n", glConfig.vendor_string );
	common->Printf( "GL_RENDERER: %s\n", glConfig.renderer_string );
	common->Printf( "GL_VERSION: %s\n", glConfig.version_string );
	common->Printf( "GL_EXTENSIONS: %s\n", glConfig.extensions_string );
	common->Printf( "GL_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	common->Printf( "GL_MAX_CUBE_MAP_TEXTURE_SIZE: %d\n", glConfig.maxCubeMapSize );
	if ( glConfig.vidMemMB > 0 ) {
		common->Printf( "video memory: %d MB\n", glConfig.vidMemMB );
	}
	common->Printf( "GL_MAX_TEXTURE_UNITS_ARB: %d\n", glConfig.maxTextureUnits );
	common->Printf( "GL_MAX_TEXTURE_COORDS_ARB: %d\n", glConfig.maxTextureCoords );
	common->Printf( "GL_MAX_TEXTURE_IMAGE_UNITS_ARB: %d\n", glConfig.maxTextureImageUnits );
	common->Printf( "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
	common->Printf( "MODE: %d, %d x %d %s hz:", r_mode.GetInteger(), glConfig.vidWidth, glConfig.vidHeight, fsmode );

	if ( glConfig.displayFrequency ) {
		common->Printf( "%d\n", glConfig.displayFrequency );
	} else {
		common->Printf( "N/A\n" );
	}
	common->Printf( "Logical Window size: %g x %g\n", glConfig.winWidth, glConfig.winHeight );

	const char *active[2] = { "", " (ACTIVE)" };

	if ( glConfig.allowARB2Path ) {
		common->Printf( "ARB2 path ENABLED%s\n", active[tr.backEndRenderer == BE_ARB2] );
	} else {
		common->Printf( "ARB2 path disabled\n" );
	}

	if ( r_finish.GetBool() ) {
		common->Printf( "Forcing glFinish\n" );
	} else {
		common->Printf( "glFinish not forced\n" );
	}

	bool tss = glConfig.twoSidedStencilAvailable;

	if ( !r_useTwoSidedStencil.GetBool() && tss ) {
		common->Printf( "Two sided stencil available but disabled\n" );
	} else if ( !tss ) {
		common->Printf( "Two sided stencil not available\n" );
	} else if ( tss ) {
		common->Printf( "Using two sided stencil\n" );
	}

	if ( vertexCache.IsFast() ) {
		common->Printf( "Vertex cache is fast\n" );
	} else {
		common->Printf( "Vertex cache is SLOW\n" );
	}
}

/*
=================
R_ResetBackendGpuCaches

Re-arm every once-cached backend GPU shader/scene handle so it recompiles against a rebuilt (or
not-yet-built) backend. A full vid_restart destroys the VK device + its shader table, and a mod
switch (ReloadEngine) runs Shutdown then re-init; without this the stale handles dangle and MD5
models draw at bind pose (T-pose). One place to add the next cache so the two callers can't drift.
=================
*/
static void R_ResetBackendGpuCaches( void ) {
	extern void R_MD5_ResetGpuShaders( void );
	extern void R_ResetGpuCullShader( void );
	extern void R_RtResetSceneState( void );
	extern void R_RHI_ResetEnvShaders( void );
	R_MD5_ResetGpuShaders();
	R_ResetGpuCullShader();
	R_RtResetSceneState();
	R_RHI_ResetEnvShaders();
}

/*
=================
R_VidRestart_f
=================
*/
void R_VidRestart_f( const idCmdArgs &args ) {
	int	err;

	// if OpenGL isn't started, do nothing
	if ( !glConfig.isInitialized ) {
		return;
	}

	bool full = true;
	bool forceWindow = false;
	for ( int i = 1 ; i < args.Argc() ; i++ ) {
		if ( idStr::Icmp( args.Argv( i ), "partial" ) == 0 ) {
			full = false;
			continue;
		}
		if ( idStr::Icmp( args.Argv( i ), "windowed" ) == 0 ) {
			forceWindow = true;
			continue;
		}
	}

	// DG: allow enforcing full vid restarts (when vid_restart is called from the menu or whatever)
	//     to let users work around driver bugs or whatever, like
	//     https://github.com/dhewm/dhewm3/issues/587#issuecomment-2206937752
	if ( r_vidRestartAlwaysFull.GetBool() ) {
		full = true;
	}

	// DG: in partial mode, try to just resize the window (and make it fullscreen or windowed)
	//     instead of doing a full vid_restart. Still falls back to a full vid_restart
	//     in case this doesn't work (for example because MSAA settings have changed)
	if ( !full ) {
		int wantedWidth=0, wantedHeight=0;
		if ( !R_GetModeInfo( &wantedWidth, &wantedHeight, r_mode.GetInteger() ) ) {
			common->Warning( "vid_restart: R_GetModeInfo() failed?!\n" );
		} else {
			glimpParms_t	parms;
			parms.width = wantedWidth;
			parms.height = wantedHeight;

			parms.fullScreen = ( forceWindow ) ? false : r_fullscreen.GetBool();
			parms.fullScreenDesktop = r_fullscreenDesktop.GetBool();
			parms.displayHz = r_displayRefresh.GetInteger();
			// "vid_restart partial windowed" is used in case of errors to return to windowed mode
			// before things explode more. in that case just keep whatever MSAA setting is active
			parms.multiSamples = forceWindow ? -1 : r_multiSamples.GetInteger();
			parms.stereo = false;

			if ( GLimp_SetScreenParms( parms ) ) {
				common->Printf( "'vid_restart partial' succeeded in changing resolution and/or fullscreen mode\n" );
				return;
			}
		}
	}

	// DG: notify the game DLL about the reloadImages and (non-partial) vid_restart commands
	if(gameCallbacks.reloadImagesCB != NULL)
	{
		gameCallbacks.reloadImagesCB(gameCallbacks.reloadImagesUserArg, args);
	}

	// this could take a while, so give them the cursor back ASAP
	Sys_GrabMouseCursor( false );

	// dump ambient caches
	renderModelManager->FreeModelVertexCaches();

	// a full vid_restart destroys the backend (VK device + its shader table), so re-arm the
	// once-cached backend GPU shaders/scene to recompile against the rebuilt backend.
	R_ResetBackendGpuCaches();

	// free any current world interaction surfaces and vertex caches
	R_FreeDerivedData();

	// make sure the defered frees are actually freed
	R_ToggleSmpFrame();
	R_ToggleSmpFrame();

	// free the vertex caches so they will be regenerated again
	vertexCache.PurgeAll();

	// sound and input are tied to the window we are about to destroy

	// free all of our texture numbers
	soundSystem->ShutdownHW();
	Sys_ShutdownInput();
	globalImages->PurgeAllImages();
	// strong teardown of the GL3 core backend while its context is still current, so
	// nothing survives the window recreate: delete its GPU objects and forget the
	// cached render-target handles (HDR/SSAO/SSR/shadow). Without this, switching
	// backends from Video Options left a dead framebuffer bound and the frame came up
	// white/garbled. No-op on the legacy backend. See RB_RHI_Shutdown.
	RB_RHI_Shutdown();
	// free the context and close the window
	GLimp_Shutdown();
	glConfig.isInitialized = false;

	// create the new context and vertex cache
	bool latch = cvarSystem->GetCVarBool( "r_fullscreen" );
	if ( forceWindow ) {
		cvarSystem->SetCVarBool( "r_fullscreen", false );
	}
	R_InitOpenGL();
	cvarSystem->SetCVarBool( "r_fullscreen", latch );

	// regenerate all images
	globalImages->ReloadAllImages();

	// make sure the regeneration doesn't use anything no longer valid
	tr.viewCount++;
	tr.viewDef = NULL;

	// regenerate all necessary interactions
	R_RegenerateWorld_f( idCmdArgs() );

	// check for problems (DUDE Phase 4: qgl is NULL under the Vulkan backend)
	if ( qglGetError != NULL ) {
		err = qglGetError();
		if ( err != GL_NO_ERROR ) {
			common->Printf( "glGetError() = 0x%x\n", err );
		}
	}

	// start sound playing again
	soundSystem->SetMute( false );
}


/*
=================
R_InitMaterials
=================
*/
void R_InitMaterials( void ) {
	tr.defaultMaterial = declManager->FindMaterial( "_default", false );
	if ( !tr.defaultMaterial ) {
		common->FatalError( "_default material not found" );
	}
	declManager->FindMaterial( "_default", false );

	// needed by R_DeriveLightData
	declManager->FindMaterial( "lights/defaultPointLight" );
	declManager->FindMaterial( "lights/defaultProjectedLight" );
}


/*
=================
R_SizeUp_f

Keybinding command
=================
*/
static void R_SizeUp_f( const idCmdArgs &args ) {
	if ( r_screenFraction.GetInteger() + 10 > 100 ) {
		r_screenFraction.SetInteger( 100 );
	} else {
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() + 10 );
	}
}


/*
=================
R_SizeDown_f

Keybinding command
=================
*/
static void R_SizeDown_f( const idCmdArgs &args ) {
	if ( r_screenFraction.GetInteger() - 10 < 10 ) {
		r_screenFraction.SetInteger( 10 );
	} else {
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() - 10 );
	}
}


/*
===============
TouchGui_f

  this is called from the main thread
===============
*/
void R_TouchGui_f( const idCmdArgs &args ) {
	const char	*gui = args.Argv( 1 );

	if ( !gui[0] ) {
		common->Printf( "USAGE: touchGui <guiName>\n" );
		return;
	}

	common->Printf( "touchGui %s\n", gui );
	session->UpdateScreen();
	uiManager->Touch( gui );
}

/*
=================
R_InitCvars
=================
*/
void R_InitCvars( void ) {
	// update latched cvars here
}

/*
=================
R_InitCommands
=================
*/
void R_InitCommands( void ) {
	cmdSystem->AddCommand( "MakeMegaTexture", idMegaTexture::MakeMegaTexture_f, CMD_FL_RENDERER|CMD_FL_CHEAT, "processes giant images" );
	cmdSystem->AddCommand( "sizeUp", R_SizeUp_f, CMD_FL_RENDERER, "makes the rendered view larger" );
	cmdSystem->AddCommand( "rtEmissiveProbe", R_RtEmissiveProbe_f, CMD_FL_RENDERER, "names the surface under the crosshair (material, stages, entity) and reports whether / why the RT emissive table treats it as an emitter" );
	cmdSystem->AddCommand( "rtEmissiveBakeTable", R_RtEmissiveBakeTable_f, CMD_FL_RENDERER, "emitters.cfg: add an entry for every emitting material of the loaded map that has none, with the power match's radiance multiplier (the editor's starting point); existing entries are kept" );
	cmdSystem->AddCommand( "rtEmissiveSaveTable", R_RtEmissiveMatTableSave_f, CMD_FL_RENDERER, "emitters.cfg: write the in-memory emitter table (the editor's edits) to the save path" );
	cmdSystem->AddCommand( "rtEmissiveReloadTable", R_RtEmissiveMatTableReload_f, CMD_FL_RENDERER, "emitters.cfg: re-read the emitter table and rebuild the RT emissive table" );
	cmdSystem->AddCommand( "sizeDown", R_SizeDown_f, CMD_FL_RENDERER, "makes the rendered view smaller" );
	cmdSystem->AddCommand( "reloadGuis", R_ReloadGuis_f, CMD_FL_RENDERER, "reloads guis" );
	cmdSystem->AddCommand( "listGuis", R_ListGuis_f, CMD_FL_RENDERER, "lists guis" );
	cmdSystem->AddCommand( "touchGui", R_TouchGui_f, CMD_FL_RENDERER, "touches a gui" );
	cmdSystem->AddCommand( "screenshot", R_ScreenShot_f, CMD_FL_RENDERER, "takes a screenshot" );
	cmdSystem->AddCommand( "envshot", R_EnvShot_f, CMD_FL_RENDERER, "takes an environment shot" );
	cmdSystem->AddCommand( "bakeGlassProbe", R_BakeGlassProbe_f, CMD_FL_RENDERER, "bakes the current area's glass reflection probe (docs/ssr.md); 'force' re-captures" );
	cmdSystem->AddCommand( "makeAmbientMap", R_MakeAmbientMap_f, CMD_FL_RENDERER|CMD_FL_CHEAT, "makes an ambient map" );
	cmdSystem->AddCommand( "benchmark", R_Benchmark_f, CMD_FL_RENDERER, "benchmark" );
	cmdSystem->AddCommand( "gfxInfo", GfxInfo_f, CMD_FL_RENDERER, "show graphics info" );
	cmdSystem->AddCommand( "modulateLights", R_ModulateLights_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "modifies shader parms on all lights" );
	cmdSystem->AddCommand( "testImage", R_TestImage_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given image centered on screen", idCmdSystem::ArgCompletion_ImageName );
	cmdSystem->AddCommand( "testVideo", R_TestVideo_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given cinematic", idCmdSystem::ArgCompletion_VideoName );
	cmdSystem->AddCommand( "reportSurfaceAreas", R_ReportSurfaceAreas_f, CMD_FL_RENDERER, "lists all used materials sorted by surface area" );
	cmdSystem->AddCommand( "reportImageDuplication", R_ReportImageDuplication_f, CMD_FL_RENDERER, "checks all referenced images for duplications" );
	cmdSystem->AddCommand( "regenerateWorld", R_RegenerateWorld_f, CMD_FL_RENDERER, "regenerates all interactions" );
	cmdSystem->AddCommand( "showInteractionMemory", R_ShowInteractionMemory_f, CMD_FL_RENDERER, "shows memory used by interactions" );
	cmdSystem->AddCommand( "showTriSurfMemory", R_ShowTriSurfMemory_f, CMD_FL_RENDERER, "shows memory used by triangle surfaces" );
	cmdSystem->AddCommand( "vid_restart", R_VidRestart_f, CMD_FL_RENDERER, "restarts renderSystem" );
	cmdSystem->AddCommand( "listRenderEntityDefs", R_ListRenderEntityDefs_f, CMD_FL_RENDERER, "lists the entity defs" );
	cmdSystem->AddCommand( "listRenderLightDefs", R_ListRenderLightDefs_f, CMD_FL_RENDERER, "lists the light defs" );
	cmdSystem->AddCommand( "listModes", R_ListModes_f, CMD_FL_RENDERER, "lists all video modes" );
	cmdSystem->AddCommand( "listParallaxMaps", R_ListParallaxMaps_f, CMD_FL_RENDERER, "lists materials with a captured parallax height map (docs/parallax.md); needs r_parallax 1 + reloadDecls" );
	cmdSystem->AddCommand( "reloadSurface", R_ReloadSurface_f, CMD_FL_RENDERER, "reloads the decl and images for selected surface" );
	cmdSystem->AddCommand( "reloadPbrTable", R_ReloadPbrTable_f, CMD_FL_RENDERER, "re-reads pbr/pbr_materials.cfg + pbr/pbr_overrides.cfg and re-applies to loaded materials (docs/pbr-materials.md)" );
}

/*
===============
idRenderSystemLocal::Clear
===============
*/
void idRenderSystemLocal::Clear( void ) {
	registered = false;
	specialEffects = 0;
	memset( specialEffectParms, 0, sizeof( specialEffectParms ) );
	frameCount = 0;
	viewCount = 0;
	staticAllocCount = 0;
	frameShaderTime = 0.0f;
	viewportOffset[0] = 0;
	viewportOffset[1] = 0;
	tiledViewport[0] = 0;
	tiledViewport[1] = 0;
	backEndRenderer = BE_BAD;
	backEndRendererHasVertexPrograms = false;
	backEndRendererMaxLight = 1.0f;
	ambientLightVector.Zero();
	sortOffset = 0;
	worlds.Clear();
	primaryWorld = NULL;
	memset( &primaryRenderView, 0, sizeof( primaryRenderView ) );
	primaryView = NULL;
	defaultMaterial = NULL;
	testImage = NULL;
	ambientCubeImage = NULL;
	viewDef = NULL;
	memset( &pc, 0, sizeof( pc ) );
	memset( &lastFrameStats, 0, sizeof( lastFrameStats ) );
	memset( &lockSurfacesCmd, 0, sizeof( lockSurfacesCmd ) );
	memset( &identitySpace, 0, sizeof( identitySpace ) );
	stencilIncr = 0;
	stencilDecr = 0;
	memset( renderCrops, 0, sizeof( renderCrops ) );
	currentRenderCrop = 0;
	guiRecursionLevel = 0;
	guiModel = NULL;
	demoGuiModel = NULL;
	takingScreenshot = false;
	takingEnvProbe = false;
	allowNoSpecular = false;
}

/*
===============
idRenderSystemLocal::Init
===============
*/
void idRenderSystemLocal::Init( void ) {
	// clear all our internal state
	viewCount = 1;		// so cleared structures never match viewCount
	// we used to memset tr, but now that it is a class, we can't, so
	// there may be other state we need to reset

	ambientLightVector[0] = 0.5f;
	ambientLightVector[1] = 0.5f - 0.385f;
	ambientLightVector[2] = 0.8925f;
	ambientLightVector[3] = 1.0f;

	memset( &backEnd, 0, sizeof( backEnd ) );

	R_InitCvars();

	R_InitCommands();

	guiModel = new idGuiModel;
	guiModel->Clear();

	demoGuiModel = new idGuiModel;
	demoGuiModel->Clear();

	R_InitTriSurfData();

	globalImages->Init();

	idCinematic::InitCinematic( );

	R_InitMaterials();

	renderModelManager->Init();

	// DUDE: Quake 4's effects (renderer/bse/)
	BSE_Init();

	// set the identity space
	identitySpace.modelMatrix[0*4+0] = 1.0f;
	identitySpace.modelMatrix[1*4+1] = 1.0f;
	identitySpace.modelMatrix[2*4+2] = 1.0f;

	origWidth = origHeight = 0; // DG: for resetting width/height in EndFrame()
}

/*
===============
idRenderSystemLocal::Shutdown
===============
*/
void idRenderSystemLocal::Shutdown( void ) {
	common->Printf( "idRenderSystem::Shutdown()\n" );

	// Re-arm the once-cached backend GPU shaders/scene before the backend teardown below frees
	// their modules: a mod switch (ReloadEngine) runs this full Shutdown then re-inits, and without
	// the reset the stale handles would dispatch against destroyed pipelines so every MD5 model
	// draws at bind pose (T-pose). See Model_md5.cpp.
	R_ResetBackendGpuCaches();

	common->SetRefreshOnPrint( false ); // without a renderer there's nothing to refresh

	R_DoneFreeType( );

	if ( glConfig.isInitialized ) {
		globalImages->PurgeAllImages();
	}

	BSE_Shutdown();

	renderModelManager->Shutdown();

	idCinematic::ShutdownCinematic( );

	globalImages->Shutdown();

	// free frame memory
	R_ShutdownFrameData();

	// free the vertex cache, which should have nothing allocated now
	vertexCache.Shutdown();

	R_ShutdownTriSurfData();

	RB_ShutdownDebugTools();

	delete guiModel;
	delete demoGuiModel;

	Clear();

	ShutdownOpenGL();
}

/*
========================
idRenderSystemLocal::BeginLevelLoad
========================
*/
void idRenderSystemLocal::BeginLevelLoad( void ) {
	renderModelManager->BeginLevelLoad();
	globalImages->BeginLevelLoad();
}

/*
========================
idRenderSystemLocal::EndLevelLoad
========================
*/
void idRenderSystemLocal::EndLevelLoad( void ) {
	renderModelManager->EndLevelLoad();
	globalImages->EndLevelLoad();
	if ( r_forceLoadImages.GetBool() ) {
		RB_ShowImages();
	}
	// DG: check if the levels worldspawn has "allow_nospecular" set, which tells us that
	//     the map author wants "nospecular" parms of lights to be respected by the renderer
	//     (Vanilla Doom3 didn't, even though some official levels have it set, so to not
	//      change the look of the original game it must be enabled in the worldspawn of new maps)
	//     See also the r_supportNoSpecular CVar (the allowNoSpecular set here only makes
	//     a difference if r_supportNoSpecular is -1, which is the default)
	allowNoSpecular = false;
	if ( gameEdit != NULL ) {
		idEntity* ent = gameEdit->FindEntity( "world" ); // the worldspawn always called "world"
		const idDict* ws = gameEdit->EntityGetSpawnArgs( ent );
		if ( ws != NULL ) {
			allowNoSpecular = ws->GetBool( "allow_nospecular", "0" );
			if ( allowNoSpecular ) {
				common->Printf( "This map allows lights to use the 'nospecular' parm on lights\n" );
			}
		}
	}
}

/*
========================
idRenderSystemLocal::InitOpenGL
========================
*/
void idRenderSystemLocal::InitOpenGL( void ) {
	// if OpenGL isn't started, start it now
	if ( !glConfig.isInitialized ) {
		int	err;

		R_InitOpenGL();

		globalImages->ReloadAllImages();

		// DUDE Phase 4: qgl is NULL under the Vulkan backend (validation
		// layers replace the GL error model there)
		if ( qglGetError != NULL ) {
			err = qglGetError();
			if ( err != GL_NO_ERROR ) {
				common->Printf( "glGetError() = 0x%x\n", err );
			}
		}
	}
}

/*
========================
idRenderSystemLocal::ShutdownOpenGL
========================
*/
void idRenderSystemLocal::ShutdownOpenGL( void ) {

	R_ShutdownFrameData();

	// as the input is tied to the window, it should be shut down when the window
	// is destroyed (relevant when starting a mod which also recreates window)
	Sys_ShutdownInput();

	// release the GL3 core backend's GPU objects while the context is still current
	// (no-op on the legacy backend). Keeps the teardown clean on full shutdown / mod
	// restart, matching the vid_restart path. See RB_RHI_Shutdown.
	RB_RHI_Shutdown();

	// free the context and close the window
	GLimp_Shutdown();

	glConfig.isInitialized = false;
}

/*
========================
idRenderSystemLocal::IsOpenGLRunning
========================
*/
bool idRenderSystemLocal::IsOpenGLRunning( void ) const {
	if ( !glConfig.isInitialized ) {
		return false;
	}
	return true;
}

/*
========================
idRenderSystemLocal::IsFullScreen
========================
*/
bool idRenderSystemLocal::IsFullScreen( void ) const {
	return glConfig.isFullscreen;
}

/*
========================
idRenderSystemLocal::GetScreenWidth
========================
*/
int idRenderSystemLocal::GetScreenWidth( void ) const {
	return glConfig.vidWidth;
}

/*
========================
idRenderSystemLocal::GetScreenHeight
========================
*/
int idRenderSystemLocal::GetScreenHeight( void ) const {
	return glConfig.vidHeight;
}
