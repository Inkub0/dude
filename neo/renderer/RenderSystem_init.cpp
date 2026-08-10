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
idCVar r_graphicsAPI( "r_graphicsAPI", "opengl", CVAR_RENDERER | CVAR_ARCHIVE, "rendering backend: opengl (legacy, default), opengl3 (GL 3.3 core, in development), vulkan, vulkan-rt (in development)" );
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
idCVar r_flareSize( "r_flareSize", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "size of light flares/glares from the material deform (1 = vanilla Doom 3, lower to dampen, 0 = off)" );

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
idCVar r_useSoftParticles( "r_useSoftParticles", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Soften particle transitions when player walks through them or they cross solid geometry. Needs r_enableDepthCapture. Can slow down rendering!" ); // #3878

// DUDE: dim smoke/steam/dust particles where the scene behind them is dark, so
// smoke fades into shadow instead of glowing as grey blobs. Alpha-blended smoke is
// always dimmed; additive smoke (e.g. textures/particles/smokepuff steam) is dimmed
// when the material name reads as smoke, so additive fire/sparks/glares stay bright.
// Builds on soft particles (reuses the captured-scene render path); opengl3/Vulkan only.
idCVar r_smokeDarkBlend( "r_smokeDarkBlend", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "GL3: fade smoke/steam/dust particles into darkness. On a black background they show at only r_smokeDarkBlendFloor opacity, ramping to full by r_smokeDarkBlendKnee background brightness. Alpha smoke always; additive smoke matched by material name (fire/sparks/glares stay bright). Needs Soft Particles. Non-vanilla, opengl3/Vulkan only." );
idCVar r_smokeDarkBlendFloor( "r_smokeDarkBlendFloor", "0.6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: smoke opacity over a fully black background, as a fraction (0.6 = 60%). 1 = no dimming. See r_smokeDarkBlend.", 0.0f, 1.0f );
idCVar r_smokeDarkBlendKnee( "r_smokeDarkBlendKnee", "0.15", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: background luminance (0..1) at which smoke returns to full opacity (0.15 = 15%). Lower = smoke recovers full opacity in dimmer light. See r_smokeDarkBlend.", 0.05f, 1.0f );

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
idCVar r_pbrMetalMetalness( "r_pbrMetalMetalness", "0.8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "metalness of the bare-metal category (grates, pipes, machined steel, chrome). Below 1 so metals keep a sliver of diffuse and don't go black between lights in Doom 3's near-zero ambient; raise toward 1 for harder metals once the environment glow (r_pbrEnvScale) or SSR gives them something to reflect (docs/pbr-materials.md sec. 5)", 0.0f, 1.0f );
idCVar r_pbrMetalDiffuse( "r_pbrMetalDiffuse", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how much of a metal's albedo colour survives in the diffuse term. Physical PBR kills diffuse entirely on metals (0), pushing the colour into reflections Doom 3 has no environment to supply, so metals read dark and off-colour; raising this keeps the asset's painted colour while the metallic specular still rides on top (1 = keep all, 0 = physical). A stylized, energy-relaxed metal for the enhancement path (docs/pbr-materials.md sec. 5)", 0.0f, 1.0f );
idCVar r_pbrToksvigBase( "r_pbrToksvigBase", "0.08", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "normal-variance baseline subtracted before the Toksvig specular widening. Low = more anti-firefly widening but softer highlights; high = tighter highlights but white pixel spikes on seams return. 0.08 = calibrated split alongside the 2026-08-01 SSR/reflection retune (docs/pbr-materials.md sec. 4)", 0.0f, 0.6f );
idCVar r_pbrFireflyClamp( "r_pbrFireflyClamp", "12", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "upper bound on the GGX specular lobe. Prevents isolated normal-map texels from spiking to clipped white; also the ceiling that skin/tight-roughness highlight cores ride at. Raise for hotter highlight cores (pairs well with r_hdr), lower to flatten", 1.0f, 16.0f );

// live per-category material values (Developer-tab sliders). These supersede the
// baked numbers in pbr/pbr_materials.cfg for entries tagged with the matching
// category column; hand-written pbr_overrides.cfg entries always win instead.
// Defaults mirror the classifier's CATEGORIES table (tools/pbr_classify.py).
idCVar r_pbrSkinRoughness( "r_pbrSkinRoughness", "0.4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for human heads/faces (the `skin` category). Low = tight oily sheen that makes facial detail pop (the firefly clamp bounds the core so it can't burn out)", 0.03f, 1.0f );
idCVar r_pbrSkinWetness( "r_pbrSkinWetness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "skin-only multiplier on the specular energy, modelling the water/sweat film on faces (1 = dry baseline). Wetness is NOT metalness — metallic skin would tint and darken like bronze; a wet look pairs this raised with Skin Roughness lowered", 0.0f, 4.0f );
idCVar r_pbrEyesRoughness( "r_pbrEyesRoughness", "0.15", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for eyes and teeth (the `eyes` category): cornea and enamel are the hardest, wettest surfaces on a face — low values give flashlight catchlights. Shares the skin wetness film", 0.03f, 1.0f );
idCVar r_pbrFleshRoughness( "r_pbrFleshRoughness", "0.55", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for body flesh, meat and hell-growth (the `flesh` category)", 0.03f, 1.0f );
idCVar r_pbrFleshWetness( "r_pbrFleshWetness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "flesh-only multiplier on the specular energy: the slime/gore film on demons, viscera and hell-growth (1 = dry baseline). For glistening horror flesh raise this and lower flesh roughness", 0.0f, 4.0f );
idCVar r_pbrCeramicRoughness( "r_pbrCeramicRoughness", "0.45", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for the `ceramic_sheen` category: painted floors plus ceramic tile on floors and walls (e.g. the washroom). Tighter than the wall panelling so lights stretch into streaks — the wet-floor / glazed-tile look. Metalness is shared with painted metal", 0.03f, 1.0f );
idCVar r_pbrMetalRoughness( "r_pbrMetalRoughness", "0.32", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for bare metal — grates, pipes, machined steel, chrome (the `metal` category, metalness 1)", 0.03f, 1.0f );
idCVar r_pbrPaintedRoughness( "r_pbrPaintedRoughness", "0.55", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for painted/coated metal — the station panelling bulk (the `metal_painted` category)", 0.03f, 1.0f );
idCVar r_pbrPaintedMetalness( "r_pbrPaintedMetalness", "0.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "metalness for painted/coated metal: paint is a dielectric, so keep this low — it models scuff-through to the metal beneath", 0.0f, 1.0f );
idCVar r_pbrRustRoughness( "r_pbrRustRoughness", "0.78", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for rusted/corroded metal (the `metal_rust` category — materials named rust/oxid/corro/dirty/stain)", 0.03f, 1.0f );
idCVar r_pbrRustMetalness( "r_pbrRustMetalness", "0.4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "metalness for rusted metal: rust itself is a dielectric oxide, so this models the patchy mix of bare metal and oxide", 0.0f, 1.0f );
idCVar r_pbrEnvScale( "r_pbrEnvScale", "0.3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "Phase C.1 metal environment floor: stock Doom 3 has no environment probes, so metals reflect an F0-tinted share of each light's own energy instead of going black where the specular lobe misses. Scales with the light and its shadow, so metals still go dark in darkness. 0 = off (docs/pbr-materials.md sec. 5)", 0.0f, 2.0f );
idCVar r_pbrStoneRoughness( "r_pbrStoneRoughness", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness for rock, concrete, brick and plaster (the `stone` category)", 0.03f, 1.0f );

// DUDE PBR Phase C.2 screen-space reflections (docs/ssr.md). Opt-in; independent of
// r_pbr (the classifier table drives per-pixel reflectivity either way) and of r_hdr.
idCVar r_ssr( "r_ssr", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space reflections on glossy/metallic surfaces (GL3 backend). Reflection strength follows the PBR material table: polished floors and bare metal mirror the on-screen scene" );
idCVar r_ssrIntensity( "r_ssrIntensity", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "reflection strength multiplier", 0.0f, 4.0f );
idCVar r_ssrMaxRoughness( "r_ssrMaxRoughness", "0.56", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "roughness cutoff: surfaces rougher than this reflect nothing (fade starts at 70% of it). Default 0.56 keeps the ceramic floors (roughness 0.45) reflecting at partial strength; 0.45 or lower excludes them. v1 reflections are sharp-only, so keep this low — rough surfaces would show implausible mirror images", 0.02f, 1.0f );
idCVar r_ssrSteps( "r_ssrSteps", "26", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "ray-march samples per pixel; more = longer/cleaner reflections, higher GPU cost (r_gl3GpuTime)", 4, 64 );
idCVar r_ssrMaxDistance( "r_ssrMaxDistance", "1024", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "reflection ray reach in world units", 64.0f, 8192.0f );
idCVar r_ssrThickness( "r_ssrThickness", "26", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "assumed surface thickness (world units) when testing ray hits; too low = gaps in reflections, too high = smearing behind edges", 1.0f, 256.0f );
idCVar r_ssrResScale( "r_ssrResScale", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "resolution the reflection march runs at, as a fraction of the screen. The Fresnel/material weighting stays full-res (ssr_composite), so lowering this only softens the reflected image — half res is ~4x cheaper", 0.25f, 1.0f );
idCVar r_ssrHiZ( "r_ssrHiZ", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space reflections: use a min-Z depth pyramid so the ray leaps provably-empty march span instead of stepping it (dev A/B; exact off = today's full-res march). Modest, scene-dependent win, near-zero on grazing reflective floors" );
idCVar r_ssrHiZLevel( "r_ssrHiZLevel", "4", CVAR_RENDERER | CVAR_INTEGER, "with r_ssrHiZ: coarse mip level the march leaps at (higher = bigger blocks / longer leaps but a coarser nearest-surface bound). Dev tuning knob; 4 measured best on the dev box", 1, 5 );
idCVar r_ssrTemporal( "r_ssrTemporal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "accumulate reflections across frames (reprojected by camera motion) so the march's jitter grain resolves into a clean image. Neighbourhood-clamped to limit ghosting" );
idCVar r_ssrTemporalFeedback( "r_ssrTemporalFeedback", "0.96", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "fraction of reflection history kept per frame with r_ssrTemporal: higher = smoother but slower to react, lower = noisier but snappier. Variance clipping + hit-aware blending keep ghosting bounded even this high", 0.0f, 0.97f );
idCVar r_ssrGlassProbes( "r_ssrGlassProbes", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "with r_ssr: glass (cube-reflection) surfaces reflect a baked cubemap of the actual room (envprobes/<map>/ under fs_savepath, captured by bakeGlassProbe) instead of Doom 3's generic env/gen* cubemap, so panes mirror the real room at any viewing angle" );
idCVar r_ssrGlassProbeScale( "r_ssrGlassProbeScale", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "brightness of the baked room probe on glass, on top of the stage colour, r_gl3ReflectionScale and the energy normalization to the replaced env/gen* cube. Only applies while a probe is bound (bump-mapped glass ignores it — that shader takes no stage colour, matching vanilla). Default 0.5 from in-game calibration", 0.0f, 4.0f );
idCVar r_ssrGlassProbeBake( "r_ssrGlassProbeBake", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "auto-capture a missing glass probe for the area the player stands in (6 offscreen renders = a one-time hitch per area, cached to disk forever). 0 = only the manual bakeGlassProbe command writes probes" );
idCVar r_ssrGlassProbeSize( "r_ssrGlassProbeSize", "256", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "face resolution of baked glass probes; re-bake with 'bakeGlassProbe force' after changing", 64, 1024 );

// DUDE Phase 3.5 shadow mapping (GL3/Vulkan only; stencil stays the faithful
// default). Global mode for now: 0 = stencil shadow volumes (vanilla), 1 =
// shadow maps for projected/spot lights (point + parallel lights fall back to
// stencil until implemented). See docs/port-phases.md Phase 8.
idCVar r_shadowMapping( "r_shadowMapping", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "shadow technique: 0 = stencil volumes (faithful), 1 = shadow maps where supported" );
idCVar r_shadowMapSize( "r_shadowMapSize", "1024", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "shadow map resolution (per light), power of two", 256, 4096 );
idCVar r_shadowMapBias( "r_shadowMapBias", "0.0008", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for world/perforated receivers (acne suppression). Tuned tight against r_shadowMapNormalOffset, which now carries most of the anti-acne load geometrically", 0.0f, 0.5f );
idCVar r_shadowMapModelBias( "r_shadowMapModelBias", "0.0012", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for model (non-world) receivers; models usually need more. Tuned tight against r_shadowMapNormalOffset", 0.0f, 0.5f );
idCVar r_shadowMapFlashlightBias( "r_shadowMapFlashlightBias", "0.001", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map depth-compare bias for the player flashlight; its grazing narrow cone needs a much smaller bias than other lights (0.0001-0.005) to avoid peter-panning", 0.0f, 0.5f );
idCVar r_shadowMapSlopeBias( "r_shadowMapSlopeBias", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "shadow map slope-scaled bias: grows the depth bias by this * tan(surface-to-light angle) to kill banded acne where light grazes a surface. 0 = flat constant bias (old behaviour)", 0.0f, 8.0f );
idCVar r_shadowMapDebug( "r_shadowMapDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "shadow-map debug: 1 = per-view light classification summary, 2 = also per-light readout (technique, occluder counts, dist/radius)", 0, 2 );
idCVar r_shadowMapCacheDebug( "r_shadowMapCacheDebug", "0", CVAR_RENDERER | CVAR_BOOL, "print a once/sec cube-shadow cache breakdown: hit rate + cube re-renders split by cause (cold = new/evicted light, warm-caster = an occluder moved, warm-light = the light moved), plus scratch/dynamic/deferred/evictions/faces. Dev diagnostic for shadow-cache work" );
idCVar r_shadowMapCachePerFace( "r_shadowMapCachePerFace", "1", CVAR_RENDERER | CVAR_BOOL, "cube-shadow cache: on a warm miss, re-render only the cube faces an occluder actually moved across, instead of all six (fidelity-identical). 0 = re-render the whole cube (old behaviour, for A/B)" );
idCVar r_shadowMapCacheSplit( "r_shadowMapCacheSplit", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "cube-shadow cache: when a moving/animated caster (monster) is near a static point light, cache the world (static) cube and re-render only the movers into a small dynamic cube each frame, sampling min(static, dynamic). Keeps such lights cached instead of bypassing the cache entirely. Fidelity-identical. 0 = old behaviour (whole cube on the scratch path every frame, for A/B)" );
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
idCVar r_shadowMapCacheMB( "r_shadowMapCacheMB", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "VRAM budget for the shadow-map cache, in MB. -1 = auto (half of detected video memory), 0 = unlimited", -1, 32768 );
idCVar r_shadowMapBudgetHysteresis( "r_shadowMapBudgetHysteresis", "30", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "point-light shadow budget stickiness (percent). A light that was cube-shadowed in the last few frames keeps this on-screen-size score bonus, so it isn't kicked out of the r_shadowMapPointLimit set by a marginally bigger newcomer. Stops shadows flickering on/off at the budget boundary as the camera turns. 0 = off (rank purely by on-screen size)", 0, 500 );
idCVar r_shadowMapMaxUpdates( "r_shadowMapMaxUpdates", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "max cached cube shadow maps re-rendered per view when a light or a moving rigid caster (door, lift, fan) changes. Excess lights reuse their previous (1-frame-stale) cube this view and refresh on a later one, spreading a burst of updates across frames. Only defers lights that already hold a cached cube; cold and dynamic (animated-caster) lights always render. 0 = unlimited (no staggering)", 0, 128 );
idCVar r_shadowMapStencilRadius( "r_shadowMapStencilRadius", "255", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "lights whose largest light_radius axis exceeds this (world units) fall back to Carmack stencil shadows instead of a shadow map. Large 'sun replacement' lights look better as stencil (no cube-map pixelation on distant shadows) and cost no shadow-map VRAM. 0 = every light uses shadow maps", 0.0f, 16384.0f );
idCVar r_shadowMapSkipStencilBuild( "r_shadowMapSkipStencilBuild", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "when a light will be shadow-MAPPED rather than stencil-shadowed, skip building its CPU stencil shadow volume for animated casters (it would never be drawn). The shadow-map caster path still shadows them. A pure CPU front-end win in shadow-mapped scenes; self-heals on r_shadowMapping toggle. 0 = always build volumes (old behaviour, for A/B)" );
// DUDE sun shadow maps (docs/shadow-research.md item 1): oversize-omni / parallel "sun"
// lights get a per-view fitted virtual 2D shadow map instead of Carmack stencil volumes.
idCVar r_shadowMapSun( "r_shadowMapSun", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "oversize 'sun replacement' omni lights and parallel lights render a per-view fitted 2D shadow map instead of falling back to stencil volumes (needs r_shadowMapping). 0 = old stencil fallback" );
idCVar r_shadowMapSunBias( "r_shadowMapSunBias", "0.0008", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "depth-compare bias for sun shadow maps; the sun map's depth unit spans the whole fitted view region, so this is smaller than the per-light biases. Tuned tight against r_shadowMapNormalOffset", 0.0f, 0.1f );
idCVar r_shadowMapSunRange( "r_shadowMapSunRange", "3000", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far ahead of the camera (world units) the sun shadow map covers; bigger = longer shadow reach but coarser texels", 512.0f, 16384.0f );
idCVar r_shadowMapNormalOffset( "r_shadowMapNormalOffset", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "normal-offset shadow bias in shadow texels: the cube and sun shadow lookups sample from a point pushed along the surface normal by about this many texels' world size, killing grazing-angle acne geometrically instead of with a large depth bias. 0 = off", 0.0f, 8.0f );

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
idCVar r_tessMinEdge( "r_tessMinEdge", "0.72", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "minimum triangle edge length (world units) to tessellate; triangles finer than this stay flat, so small dense clusters (eyeballs, fine facial detail) don't over-inflate while big low-poly silhouette triangles still smooth. 0 = tessellate everything, higher = only the largest triangles", 0.0f, 64.0f );
idCVar r_tessWeldSeams( "r_tessWeldSeams", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "weld coincident vertex normals on animated (md5) meshes so a model built from mirrored/UV-split halves deforms as one piece under tessellation + displacement, instead of the seam opening. Off = vanilla normals. Only applied to meshes that actually tessellate: requires r_tessellation on and the Vulkan backend, and skips any surface the tessellator leaves flat, so non-tessellated geometry keeps vanilla normals" );
idCVar r_tessWeldThreshold( "r_tessWeldThreshold", "0.7", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how aggressively r_tessWeldSeams welds: only coincident normals whose dot product is at least this get averaged. 1 = weld only identical normals, lower = also weld sharper creases (0.7 closes mirror/UV seams while preserving hard edges)", 0.0f, 1.0f );
idCVar r_tessDisplace( "r_tessDisplace", "-0.25", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "normal-map displacement on tessellated characters: push detail along the surface normal by this many world units (Doom 3 has no runtime height maps, so height is approximated from the bump map's blue channel). 0 = pure PN smoothing, positive raises detail, negative carves it in. Vulkan only", -8.0f, 8.0f );
idCVar r_tessDebug( "r_tessDebug", "0", CVAR_RENDERER | CVAR_BOOL, "diagnostic: print each material name accepted for tessellation once. Walk up to a mis-tessellated character and read the console to find the leaked material (Vulkan)" );

idCVar r_ssao( "r_ssao", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "screen-space ambient occlusion (GTAO) applied to the ambient light term; adds contact shadowing and depth to models (non-vanilla; opengl3/Vulkan only)" );
idCVar r_ssaoIntensity( "r_ssaoIntensity", "1.2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO strength: scales the occlusion darkening (0 = none, 1.2 = default, higher = deeper creases)", 0.0f, 4.0f );
idCVar r_ssaoFloor( "r_ssaoFloor", "0.03", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO minimum ambient visibility: fully-occluded areas darken to at most this (0 = can reach black, 1 = no darkening). Keeps creases from crushing to black in dark scenes", 0.0f, 1.0f );
idCVar r_ssaoDirectLight( "r_ssaoDirectLight", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how strongly SSAO darkens direct (dynamic) light's diffuse, 0..1. Doom 3 has almost no ambient, so this is what makes AO visible in normal scenes. Lower it if AO looks baked-in under moving lights; 0 = ambient-only (most faithful)", 0.0f, 1.0f );
idCVar r_ssaoRadius( "r_ssaoRadius", "36", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO sampling radius in world units; larger reaches for broad occlusion, smaller keeps it to tight contact creases", 1.0f, 256.0f );
idCVar r_ssaoSlices( "r_ssaoSlices", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "SSAO horizon-search directions (slices) per pixel; more = smoother, less directional noise, more GPU cost", 1, 8 );
idCVar r_ssaoSteps( "r_ssaoSteps", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "SSAO samples marched along each direction; more = more accurate horizons at range, more GPU cost", 1, 12 );
idCVar r_ssaoResScale( "r_ssaoResScale", "0.75", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO buffer resolution as a fraction of the screen (0.5 = half ... 1.0 = full); lower is faster and softer, upsampled bilaterally", 0.25f, 1.0f );
idCVar r_ssaoBentNormal( "r_ssaoBentNormal", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "shade the ambient along the bent (average unoccluded) normal for directional occlusion, instead of a flat darkening" );
idCVar r_ssaoBentStrength( "r_ssaoBentStrength", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how far to bias the ambient cube lookup from the surface normal toward the bent normal (0 = surface normal, 1 = fully bent). Only matters where there is ambient light; needs r_ssaoBentNormal", 0.0f, 1.0f );
idCVar r_ssaoNormalBuffer( "r_ssaoNormalBuffer", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "feed SSAO from a real bump-mapped normal G-buffer (an extra opaque geometry pass) instead of normals reconstructed from depth; picks up normal-map detail and removes faceting, at the cost of one geometry pass. 0 = reconstruct from depth (cheaper)" );
idCVar r_ssaoSpecular( "r_ssaoSpecular", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "also attenuate specular highlights in occluded areas (stronger anti-plastic, mild fidelity departure); helps in scenes with little ambient fill" );
idCVar r_ssaoDebug( "r_ssaoDebug", "0", CVAR_RENDERER | CVAR_INTEGER, "SSAO debug view: 0 = off, 1 = show the AO buffer, 2 = show bent normals, 3 = show the normal G-buffer", 0, 3 );
idCVar r_ssaoTemporal( "r_ssaoTemporal", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "accumulate SSAO across frames via camera reprojection: smooths the horizon-search noise and lets slices/steps run lower for the same look. Static-world reprojection (no motion vectors); ghosting is bounded by a neighbourhood clamp. Non-vanilla; opengl3/Vulkan only" );
idCVar r_ssaoTemporalFeedback( "r_ssaoTemporalFeedback", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "SSAO temporal history weight: fraction of the reprojected previous-frame AO kept each frame (higher = smoother/steadier but more latency and ghosting; 0 = no accumulation). Needs r_ssaoTemporal", 0.0f, 0.97f );
// DUDE SSAO Phase 1: prefiltered linear-depth mip chain (docs/ssao-perf-optimization.md). The horizon
// search reads a coarser mip for farther steps, so far taps touch a small cache-local footprint instead
// of scattering across full-res _currentDepth. Visually near-identical; a GPU-time win that scales with
// r_ssaoRadius. Adds a cheap linearize + mip-gen pass per view. On by default; toggle for an A/B.
idCVar r_ssaoDepthMip( "r_ssaoDepthMip", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "SSAO: march the horizon search over a prefiltered linear-depth mip chain (far steps read coarse mips) instead of full-res depth. Cheaper on GPU at a wide radius, visually near-identical. 0 = full-res depth every tap (opengl3/Vulkan only)" );
idCVar r_ssaoDepthMipBias( "r_ssaoDepthMipBias", "0.2", CVAR_RENDERER | CVAR_FLOAT, "SSAO depth-mip LOD aggressiveness: LOD = log2(stepPixels * this). Higher drops to coarser mips sooner (faster, but coarse depth smears occlusion across silhouettes into halos); lower keeps steps on finer mips (sharper, keeps most of the speedup). Needs r_ssaoDepthMip", 0.05f, 4.0f );

// DUDE: baked ambient-occlusion (occlusion) maps. Per-material AO textures declared with
// the `occlusionmap` material keyword, multiplied into the ambient (and, scaled, direct-
// light diffuse) exactly like SSAO. Non-vanilla; enhancement backends only. Inert on stock
// assets, which never declare an occlusion stage. See docs/occlusion-maps.md.
idCVar r_occlusionMaps( "r_occlusionMaps", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "use per-material baked ambient-occlusion maps (the `occlusionmap` material stage) to darken creases in the ambient and direct-light diffuse. Complements SSAO; only affects materials that ship an occlusion map (none in stock Doom 3). Non-vanilla; opengl3/Vulkan only" );
idCVar r_occlusionMapScale( "r_occlusionMapScale", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "strength of baked occlusion maps on the ambient term, 0..1 (0 = off, 1 = the map at full darkening)", 0.0f, 1.0f );
idCVar r_occlusionMapDirect( "r_occlusionMapDirect", "0.9", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "how strongly baked occlusion maps darken direct (dynamic) light's diffuse, 0..1, as a fraction of r_occlusionMapScale. Doom 3 is mostly dynamic light, so this is what makes the map visible; lower it if AO looks baked-in under moving lights, 0 = ambient-only", 0.0f, 1.0f );
idCVar r_occlusionMapsAutoBake( "r_occlusionMapsAutoBake", "0", CVAR_RENDERER | CVAR_BOOL, "DEV: when a model-entity surface has no explicit or cached occlusion map, bake one on first sight (writes generated/aomaps, one-time hitch per model). Off by default; use the bakeAO/bakeAOFolder commands for offline baking" );

// DUDE: parallax occlusion mapping (non-vanilla; Vulkan enhancement). Per-pixel surface
// relief on materials that carry height data -- auto-captured from a `heightmap(...)` operand
// in the bump program (the source Doom 3 bakes its normals from), or an explicit `parallaxmap`
// stage. Off = bit-for-bit vanilla with zero extra resident textures: no height map is loaded
// unless r_parallax is set when the material is parsed. See docs/parallax.md.
idCVar r_parallax( "r_parallax", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "parallax occlusion mapping: per-pixel surface relief on materials that carry height data (auto-captured from `heightmap(...)` in the bump program, or an explicit `parallaxmap` stage). Non-vanilla; Vulkan only. Takes effect on the next reloadDecls/map load" );
idCVar r_parallaxScale( "r_parallaxScale", "0.1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "global multiplier on per-material parallax height scale, 0..4 (0 = flat, higher exaggerates the relief; 0.1 = tuned default)", 0.0f, 4.0f );
idCVar r_parallaxMinSteps( "r_parallaxMinSteps", "6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "parallax occlusion mapping: height-field march steps when viewing head-on (cheaper). Ramps up to r_parallaxMaxSteps at grazing angles", 1, 32 );
idCVar r_parallaxMaxSteps( "r_parallaxMaxSteps", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "parallax occlusion mapping: height-field march steps at grazing angles (higher = fewer swimming artifacts, costlier). Loop is capped at 32", 1, 32 );
idCVar r_parallaxShadow( "r_parallaxShadow", "0.6", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "parallax self-shadowing strength, 0..1 (0 = off): the marched relief casts contact shadows from the light direction. Adds a second (half-step) march per lit pixel. Only active when r_parallax is on", 0.0f, 1.0f );

// DUDE: dampen the cube-map ("sheen") reflection on glass etc. The enhancement
// backends light the scene brighter than the original renderer, so the environment
// reflection reads too strong; 0.7 (-30%) matches the legacy look, 1.0 is untouched.
idCVar r_gl3ReflectionScale( "r_gl3ReflectionScale", "0.7", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "GL3: brightness of cube-map glass reflections (1 = untouched; 0.7 compensates for the brighter enhanced scene; non-vanilla, opengl3/Vulkan only)", 0.0f, 2.0f );

// DUDE: gate for the non-vanilla "Enhancements" (see tr_local.h). True for the
// RHI backends — opengl3 today, vulkan once it lands (Phase 4); the legacy ARB2
// path stays vanilla-faithful.
bool R_BackendSupportsEnhancements() {
	return glConfig.rhiBackend;
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
	// the GL 3.3 core backend; "vulkan" runs the Vulkan backend (M1 bring-up)
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
			common->Printf( "r_graphicsAPI %s: Vulkan backend (M1 bring-up: clear + present only)\n", r_graphicsAPI.GetString() );
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
	if ( tr.primaryWorld->Trace( mt, start, end, 0.0f, false, false, true ) ) {
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
			const shaderStage_t *st = mat->GetStage( s );
			if ( !st->parallaxImage ) {
				continue;
			}
			common->Printf( "%-44s scale %4.1f  %s\n", mat->GetName(), st->parallaxScale,
							st->parallaxImage->imgName.c_str() );
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

	common->SetRefreshOnPrint( false ); // without a renderer there's nothing to refresh

	R_DoneFreeType( );

	if ( glConfig.isInitialized ) {
		globalImages->PurgeAllImages();
	}

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
