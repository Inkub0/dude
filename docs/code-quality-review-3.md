# Code-quality review 3 (2026-11-10)

Scope: the C++ this fork added between the previous review's merge (3684e21f, 2026-10-01) and master
2fa7ed82 (2026-11-10): 986 commits, about 46k lines over 249 C++ files. Judged for coherence, leanness,
DRY, efficiency and cyclomatic complexity, not as a bug hunt. Excluded: neo/libs (vendored), neo/d3le
(vendored d3xp copy), the GLSL under neo/shaders (its own pass later), the Quake 4 game library in its
own repository, data, docs, tools.

Method: seven read-only review agents, one per area, each from the diff plus the surrounding code;
the findings below are theirs, synthesized. Line numbers are the working-tree lines of master
2fa7ed82 at review time. Tags: **SAFE** = mechanical, output-identical, verifiable by build +
inspection; **RUNTIME** = needs an in-game check. The "Fixed" column names the commit on
`cleanup/code-quality-review-3` when the finding was acted on.

## Overall

No crash or correctness bug surfaced beyond one minor one (the light editor's Del key left empty
entries behind). The debt clusters into four themes:

1. **Per-frame cost that is pure waste**: shader programs looked up by a linear name scan every
   pass, decal materials substring-scanned every draw, the light editor doing linear lookups per
   emitter row with the editor closed, BSE copying every particle it draws, per-frame cvar string
   lookups in the Q4 settings bridge and PlayerView, every string formatted twice on the MinGW build.
2. **Dead code from settled decisions**: the ReSTIR DI ReLAX-only switch left an unread target,
   always-true flags and their ternaries; a hitch-hunt profiling wrapper; a write-only BSE field.
3. **New code skipping existing helpers**: 11 hand-rolled render-target lifecycles beside
   RB_RHI_EnsureRT, 18 new manual texture-unit cache pokes, three copy-pasted worker pools, menu
   widgets re-rolled next to the Cvar* helpers.
4. **God-functions and file splits** (the deferred, structural ones): RB_RHI_ReStirDIPass at 739
   lines, idMD5Mesh::UpdateSurface at about 460, tr_main.cpp grown from 4160 to 6296 lines.

## RHI / Vulkan (neo/renderer/rhi)

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | RhiWorld.cpp:8844 RB_RHI_ReStirDIPass (8844-9583) | High | one function: targets, velocity downsample, young, gather, boost, gradient/filter/confidence/disoccMix chain, guides, NRD denoise, ~120 lines of uniform packing (739 lines, the largest in the RHI) | split into ReStirDIEnsureTargets / PackGatherParms / RunYoungBoost / RunGradientChain / RunGuides over a small diViewCtx | SAFE | deferred (structural) |
| 2 | RhiWorld.cpp:8905-8922, 9083-9089, 9117-9128, 9314-9332, 9441, 9485-9490 (+ rhiRtaoVelRT 8277, RTGI 8553/8669/8703/8736) | High | the lost-context / size-mismatch / destroy / create dance hand-rolled ~11 times; RB_RHI_EnsureRT / EnsureHistoryPair (5669/5692) used only by SSAO, SSR, RtRefl | reuse them; an overload with a create callback (or MRT kind) for the MRT targets | SAFE | deferred (needs VK runtime check) |
| 3 | RhiWorld.cpp:479, 8920, 8929, 9103-9106, 9401-9406 | Med | dead: rhiReStirDIShRT written never read; `relaxMode = true` leaving dead ternaries; diFastF always 0 feeding a dead ReBLUR floor chain; rhiReStirDIRelaxThisView always true | delete, fold the constants | SAFE | ccb53211 |
| 4 | RhiWorld.cpp:346 RB_RHI_BindRTImage | Med | 45 callers poke `tmu[u].current2DMap = -1` by hand after the direct GL bind (18 new); the gradient pass binds units 4/5/6/9/10/11 without one | invalidate inside the helper, drop the pokes | SAFE | 81aecde8 |
| 5 | RhiWorld.cpp:10390, 10564 (+ RtgiDebug, RtEmissive, ReStirDIDebug overlays, Apply 10855) | Med | the world-reconstruct overlay prologue cloned (InvertMatrix, invP00/11, screenCorrection, depthTexRecip, FillDepthParms, windowCoord, viewport+scissor, depth+normal binds): 12 viewport/scissor copies, 6 invP copies | RB_RHI_InitViewReconstructParms + RB_RHI_SetViewViewportScissor | SAFE | deferred |
| 6 | RhiWorld.cpp, 46 + 19 sites | Low-Med | `memset + mvp identity` and the lo/hi memcpy bit-cast of 64-bit addresses repeated | RB_RHI_FullscreenParms(), RB_RHI_PackAddr() | SAFE | deferred |
| 7 | RhiWorld.cpp:10228-10262, 10457-10480, 9165-9176 | Med | the GPU-only storage buffer ensure (create, get address, destroy on failure, warn once, print size) cloned 3x (irradiance cache x2, light grid, DI GI side) | RB_RHI_EnsureDeviceBuffer | SAFE | deferred |
| 8 | RhiBackend.cpp:1563, 1705-1706, 1758 | Med | "AllocStorage, else owned CreateBuffer(BU_STORAGE), destroy after dispatch" x4 (skin, tess bake x2, tess jobs); storageRange convention inconsistent | RB_RHI_TransientStorage | SAFE | deferred |
| 9 | vk/VulkanBackend.cpp:5550, 5852, 6019, 6098, 6721, 6956, 8736, 8830, 9927, 10606, 10757, 10875 | Med | 12 sync submissions repeat begin / checkpoint / record / end / reset fence / guarded submit / guarded wait | BeginUploadSync / EndUploadSync | SAFE | deferred |
| 10 | vk/VulkanBackend.cpp:7941/7986, 8020/8075, 8131 | Med | three storage-image lifecycles clone create / view / RegisterMergeSampleImage / retire-or-wait-idle destroy / InvalidateTextureSets (RtgiDestroyOutput carries it twice) | a StorageImage struct with Ensure / Destroy | SAFE | deferred |
| 11 | vk/VulkanBackend.cpp:4504-4580 vs 5765 | Low | VMA placement policy in two places (CreateBuffer's BU_STORAGE_DEVICE/UPLOAD vs CreateRtBuffer's deviceLocal); CreateBuffer silently drops `data` for device-only buffers | one MakeAci(placement); assert !data on the device path | SAFE | deferred |
| 12 | vk/VulkanBackend.cpp:4941, 5236 LoadShader / LoadComputeShader | Low | linear case-insensitive scan of shaderTable per call, ~10 per frame from the DI pass, 71 call sites; the loop duplicated 4x | hash the lookup (also GL3_FindProgram) | SAFE | 0e80d0e8 |

Deferred-item status: GL/VK split leaking inline is **worse** (12 new GetActiveBackendType checks, 4
copies of the `BT_VULKAN && SupportsRayQuery` gate at 7645/8854/10224/10449, 18 new pokes - the pokes
are gone now); hand-rolled barrier / AS-build / sync-submit boilerplate persists and grew (the new
barriers do go through FragmentStorageBarrier / PostComputeBarrier); ReSTIR DI repeats RTGI's pass
shape almost line for line; per-frame allocation churn is mostly addressed (AllocStorage rings);
god-functions: ReStirDIPass 739, SSR 609 (still monolithic), NormalPrepass 541, GatherSceneLights
535, RtaoDenoise 376, FillDepthBuffer 353, DrawView 348, RtgiPass 306, RtShadowBlurLight 301; the
RTGI render targets (546-564) are missing from RB_RHI_ResetWorldTargets and survive only through the
handle generation check (safe, but a second teardown mechanism).

## Renderer frontend (tr_main, tr_light, Model_md5, Material, Image, RenderWorld, fonts)

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | tr_main.cpp:2740-4670 | High | the E1/E2 emissive table (statics, gather, rows, movers, adopt/append, labels, probe, debug geometry, ~1,900 lines) lives in tr_main, now 6296 lines | move as one block to tr_rtemissive.cpp, with the deferred tr_rtscene / tr_rtvalidate split | SAFE | deferred (file split) |
| 2 | tr_main.cpp:4670-4694 vs 3350-3372 | Med | R_RtResetSceneState hand-clears a subset of what R_RtEmissiveFree clears (misses RowAnim, FlareHidden, SeenModel, FlareCount, MovedGroups) | one forget-only function both call | SAFE | eedb2d98 |
| 3 | Model_md5.cpp:1804-1818 vs 1947-1960 | Med | the prev-position MV buffer block (create, address, roll back, free when off) duplicated for gpuSkinPrevVB and tessDeformPrevVB | R_MD5_SyncPrevBuffer | SAFE | deferred |
| 4 | Model_md5.cpp:1458-1468 vs 1497-1506 (+1305-1315 vs 1510-1521) | Med | EnsureTessBuffersUploaded and BakeTessOnGpu create the same three tess buffers with the same rollback; the bind-vertex staging duplicated | CreateTessGpuBuffers, StageTessBindVerts | SAFE | deferred |
| 5 | Model_md5.cpp:1538-2000 UpdateSurface | Med | ~460 lines: CPU skin, normal weld, GPU skin job, deform-buffer alloc, prev-buffer management, tess job; two RB_RHI_AddTessJob calls differing only in source args | QueueGpuSkin, QueueTessDeform | SAFE | deferred (structural) |
| 6 | Model_md5.cpp:152-200, 937-970 | Low | six copy-pasted lazy-compile shader getters with handle/Tried pairs; a stray comment above the TessBake static | a table + one getter; R_MD5_ResetGpuShaders loops it | SAFE | deferred |
| 7 | Model_md5.cpp:1103-1258, 1365-1410 | Med | the CPU per-vertex relief bake is reached only when the build-time md5tessbake SPIR-V is missing; the pool mostly parallelises trivial index generation; a third hand-rolled worker pool | if the GPU bake is mandatory delete the CPU path; at least share the pool | RUNTIME (pool part SAFE) | pool shared: aa77b67b; the CPU path's fate is the user's call |
| 8 | RenderSystem_init.cpp:822-905 (callers RhiBackend.cpp:3620, RhiWorld.cpp:2228) | Med | R_DecalSheenScale / IsBlood / LitHighlightStage substring-scan material and image names per decal draw per frame | classify once at parse | SAFE | c83d5c43 |
| 9 | Image_load.cpp:690-691, 737-738, 2030-2031 | Med | R_ImageAverageColorRGBA and R_ImageEmissiveExtentsRGBA always run as a pair, two passes over every loaded image (normal and specular maps included; extents does up to 65k samples with a powf each) | one single-pass R_ImageCaptureStats; skip extents for bump/specular or when RT is off; share one sRGB-to-linear helper with tr_main.cpp:2821 | merge SAFE, gating RUNTIME | deferred |
| 10 | tr_main.cpp:3500-3515, 3937-3947, 4250-4258; 4166/4190/4239/4261; 4 scale clamps | Low | the per-entity emissive surface walk written 3x; the SeenModel grow loop 4x; the r_rtEmissiveScale clamp 4x | iterator; AssureSize; accessor | SAFE | eedb2d98 (loops + clamp; the walk iterator deferred) |
| 11 | tr_main.cpp:4103-4127 R_RtEmissiveRefreshMovers | Low | a profiling wrapper from the hitch hunt (5-second statistics printed only under the debug cvar) | call the body directly | SAFE | d73c0739 |
| 12 | tr_font.cpp:418-426 vs 466-474; Material.cpp:1164-1182 | Low | the fontCount -> Small/Medium/Large metrics ladder repeated in the .fontdat branch; four Icmp tests for Q4 table names each returning the constant 0 | SetSizeMetrics; a name array + loop | SAFE | deferred |
| - | RenderWorld.cpp:613-638 UpdateForcedAmbient | Low | rescans every _area model's bounds every frame before its "unchanged" early return | return first | SAFE | c37c292f |
| - | tr_light.cpp:1242/1326 | - | r_instancingSurfaceMask is a global side channel into InstantiateDynamicModel: acceptable given the frozen ABI | - | - | - |

Deferred-item status: tr_main split still deferred and stronger (4160 -> 6296 lines; validators still
in place: R_RtAnimBlasValidate ~360, R_RtWorldValidate ~200, R_RtReflValidate ~130); R_RtRefreshInstances
scratch still fixed; tr_light emissive tagging stands in a new form (R_AddAmbientDrawsurfs at
tr_light.cpp:2349 runs R_RtEmissiveIsFlareMaterial's stage loop per surface per frame for hidden-flare
entities; cacheable per material); PBR -> MaterialPbr.cpp split still deferred (Material.cpp 3634
lines, ~118 PBR references); the MD5 compute-shader caches are all reset in R_MD5_ResetGpuShaders.

## Light editor and tools (RtEmissiveAuthor.cpp, aobake, AAS)

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | RtEmissiveAuthor.cpp:551 R_EmAuthFixtureEmits (RhiWorld.cpp:6746/6791, twice per row per frame) | Med | EmAuth_Sync Icmp + linear FindFixture + linear MaterialEntry with an Icmp for unresolved materials, editor open or not | hash fixtures by id, resolve material pointers once | SAFE | 342b6acc |
| 2 | aobake.cpp:645-660 vs 715-735 | Med | texel enumeration (three AO_Edge barycentrics, inside test, point/normal interpolation) copied between AO_BakeRow and AO_BakeSurfaceGpu | AO_TexelSample | SAFE | deferred |
| 3 | RtEmissiveAuthor.cpp:1020/1094/1106/1123/1048/1455/1610 (+1029/1457/1611, 1102/1742/1748) | Med | find-or-create written out for materials x2, fixtures x2, lights x2; "drop the empty entry" x6 | *IndexRef and Prune* helpers | SAFE | 342b6acc |
| 4 | RtEmissiveAuthor.cpp:455 R_EmAuthOverrideLight (per UpdateLightDef once any override exists) | Med | NumLightOverrides linear scan + KeyForOrigin linear scan over the .map lights + va() + Icmp FindLight; EmAuth_LightCache exists for this | keep an override count; key through EmAuth_LightCache (needs dudePristine set first) | RUNTIME | open |
| 5 | RtEmissiveAuthor.cpp:726, 745, 816, 974, 1406, 1443, 1529, 1864 | Low-Med | "index in range && lightDef != NULL && serial matches" and the hover-light validity hand-written many times; PickLight re-inlines LightShown; LightSolo_f re-implements ToggleSoloHover | EmAuth_LightAlive, EmAuth_HoverLightValid, EmAuth_ToggleSoloLight | SAFE | deferred |
| 6 | aobake_gpu.cpp:239-509 AOGpu_Init | Med | ~270 lines: device pick, device create, pool + fence, pipeline, descriptors; the feature pNext chain built twice | PickDevice / CreateDevice / CreatePipeline / CreateDescriptors | SAFE | deferred |
| 7 | aobake.cpp:999, 740 | Low-Med | AO_BuildGrid always inserts cells even when the GPU traces everything; the GPU batch reserves 1M samples (~36 MB) per surface; after a GPU failure `gpu` stays true and every later surface fails first | lazy grid on fallback; size the batch; clear `gpu` on -1 | RUNTIME | open |
| 8 | RtEmissiveAuthor.cpp:1061, 1227, 1294 | Low | a fixture's emit/scale/reach/follow words formatted three times, light words twice | EmAuth_FixtureWords / LightWords | SAFE | deferred |
| 9 | RtEmissiveAuthor.cpp:1519/1538/1198/1801; 1321-1326 vs 1843-1846 | Low | "set follow, followGen = 0, Changed" x4; the leave-look-mode teardown x2 | EmAuth_SetFollow, EmAuth_LeaveLookMode | SAFE | deferred |
| 10 | RtEmissiveAuthor.cpp:637 in R_EmAuthLookFrame's row loop (834, 868, 875) | Low | FindFixture then FixtureEmits repeats FindFixture, MaterialEntry, ResolveLook, Sync per row per frame (look mode only) | falls out of #1 mostly | SAFE | partly (#1) |
| 11 | AASFile.cpp:1252 vs 1395 | Low | two parallel memory tallies; MemorySize leaves out areaCeilings; ParseFeatures ignores its ExpectTokenString results | one list | SAFE | deferred |
| 12 | assorted | Low | duplicate header comment above EmAuth_MaterialEntry; orphaned comment at aobake_gpu.cpp:519; redundant `lastGen` at 489; `atoi==0 && Cmp=="0"` at 1307; hover text formatted then discarded at 908-913; hemi copy at aobake.cpp:1028; "editEmitter" legacy alias; EmAuth_MenuGeneral calls FileExists every ImGui frame | delete / merge | SAFE | comment fixed in 342b6acc; rest deferred |
| BUG | RtEmissiveAuthor.cpp:1426 | minor | Del on a surface whose material does not emit: FixtureRef creates an empty entry (emit 0) and sets dirty; never written, but accumulates and triggers a spurious recovery-copy write | create only when needed / prune | RUNTIME | 342b6acc (prunes the empty entry) |

The CPU and GPU AO bake paths already share the hemisphere table, cavity, contrast and output
write; the CPU path is the fallback (no GPU, or a GPU failure mid-surface), not dead.

## BSE effects (neo/renderer/bse)

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | BseEffect.cpp:736-747 Insert, :317 granularity | High | a complex segment inserts each new particle at index 0 (moves the whole list of ~330 B entries: O(k*n) per service); the list grows from granularity 32 up to a 2048 budget | Resize(budget) after CalcBudget (SAFE); append + reverse walk for "newest first" (RUNTIME: draw order) | mixed | open (RUNTIME; the Resize alone pre-allocates up to 675 KB per segment, not taken) |
| 2 | BseEffect.cpp:862-864, 935-940, 947-950, :789 | Med | each spawn rebuilds ~13 bseDomain_t from the decl through BSE_MakeDomain -> EffectDomainRange, data that depends only on the decl | precompute in idDeclEffect::Finish | SAFE | deferred |
| 3 | BseEffect.cpp:2091 BuildSegment, :2199 EvaluateLight | Med | each drawn particle copied by value (~330 B, every view, every frame) only to patch endTime for constant segments | copy only in that case | SAFE | 33cf56a7 |
| 4 | BseRender.cpp:1053-1058 R_AddBseSurfaces | Med | RemoveIndex(i--) inside the scan: O(n^2) per view | one-pass compaction | SAFE | 33cf56a7 |
| 5 | BseRender.cpp:546, 554-589 idRenderModelBseLit::InstantiateDynamicModel | Med | per lit effect per view per frame: bseEffects.FindIndex, new idRenderModelFrameSnapshot, one static tri surface per segment (Mem_Alloc); duplicates R_AddBseGeometry's tri build | flag/handle validity; one geometry -> srfTriangles_t helper; frame-allocated verts | RUNTIME | open |
| 6 | BseRender.cpp:389-391 BSE_DeliverRequests | Med | Swap with a local list frees the queue's buffer every bracket that had requests | a static partner | SAFE | 33cf56a7 |
| 7 | BseEffect.cpp:1600-1617 vs 1997-2003, 2143-2150 | Med | the evalTime clamp twice; the tint/fade/size envelope evaluation twice | ParticleEvalTime, EvaluateAppearance | SAFE | deferred |
| 8 | BseEffect.cpp:993-1023, 485-506, 1397-1433 | Med | "attenuate ? (inverse ? 1-a : a)" x5; SegmentAttenuation and OriginAttenuation are the same ramp | BSE_DistanceRamp, BSE_AttenuationFor | SAFE | deferred |
| 9 | DeclEffect.cpp:468-596, 631-744 | Med | two keyword ladders (~20 and 13 branches), most "ParseEffectRange/Scalar into a member + set key" | a {name, key, kind, offsetof} table | SAFE | deferred |
| 10 | BseEffect.cpp:405, 537, 730 | Low | `tmpl->detail > 0 && BSE_Scale() < tmpl->detail` x3 | BelowDetail() | SAFE | deferred |
| 11 | BseEffect.cpp:85-113, 1680-1690, 1050-1055/1199-1204 | Low | three near-identical normalize helpers; "normalize or +x" twice; BSE_BoltCount's clamp restated; two trivial wrappers | one of each | SAFE | deferred |
| 12 | DeclEffect.h:308 (+.cpp 1082/1101/1109); DeclEffect.h:335-337; idBseSegment::Check's ignored return; orphaned comment at 1667; bse_showBounds read per effect at BseRender.cpp:837 | Low | dead / leftovers | remove | SAFE | `temporary` removed ea47492c; rest deferred |
| - | DeclEffect.cpp:982 GetBoundsRadius | Low | 8 x 16 samples per particle segment on every BSE_PlayEffect; cacheable on the decl keyed by gravity | cache | SAFE | deferred |
| - | DeclEffect.cpp:1001, 1032 | Low | 0.002 / 60 / 16 hard-coded, duplicating BSE_TIME_EPSILON / BSE_MAX_LIFE / BSE_MAX_FORKS | share the constants | SAFE | deferred |

## Quake 4 port (neo/quake4, nullgame, DeclQuake4, DeclGuides, GameTitle)

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | RenderWorldQ4.cpp:447,457,463,469,476 | Med | the effect-handle check written 5x | Effect(handle), the counterpart of Entity() | SAFE | c228ae38 |
| 2 | RenderWorldQ4.cpp:286-292, 309-315; SoundSystemQ4.cpp:180-186 | Med | "SetNum then NULL-fill" loop x3 | idList::AssureSize | SAFE | c228ae38 |
| 3 | RenderWorldQ4.cpp:190-205 EntityCallback | Low/Med | re-implements ToEngine line by line | call ToEngine | SAFE | not taken: ToEngine leaves callback/callbackData untouched when the game drops its callback, EntityCallback must clear them |
| 4 | RenderWorldQ4.cpp:244-261 | Low | the entityDefs[h]->suppressSurfaceMask poke duplicated for the main and overlay entity | one helper | SAFE | c228ae38 |
| 5 | RenderWorldQ4.cpp:628-652 | Low | ModelTrace and FastWorldTrace copy the same 7-field conversion | ModelTraceToQ4 | SAFE | c228ae38 |
| 6 | SoundSystemQ4.cpp:323-344 (+119, SettingsQ4.cpp:120) | Low | UpdateEmitter and ModifySound convert parms + frequencyShift the same way; `6*log2f` scale-to-dB in two files | a local helper; a shared ScaleToDb | SAFE | deferred |
| 7 | SettingsQ4.cpp:108-148 Q4Settings_Frame (every frame with Q4 loaded) | Low | 4 cvar lookups by name (s_volume_dB x2, s_device x2) and 2 idStr copies per frame | cache the idCVar* | SAFE | 262df64a |
| 7b | CollisionModelManagerQ4.cpp:322-323 Contacts | Low | _alloca sized by an unclamped maxContacts, whole buffer memset | clamp; drop the memset | SAFE | not taken (the memset zeroes fields the engine may leave; a cap changes what the game gets) |
| 8 | DeclGuides.cpp (57, 165, 201, 296, 377, 433; 312-318 vs 386-392) | Med | the quoted-string skip loop hand-written 6x; the inlineGuide/guide match duplicated; LoadTemplates ~125 lines; std::string/vector/lambdas in an idStr/idList codebase | SkipQuoted, MatchGuideKeyword; split LoadTemplates | SAFE (+ a guide-expansion diff) | deferred |
| 9 | NullGameDressing.cpp:500-640 AddModel | Med | ~140 lines: body entity, head resolution (D3 vs Q4), Q4 copy_joint overrides; AnimFileFor computed twice | AddHead, BuildCopyJointOverrides | SAFE | deferred |
| 10 | NullGameDressing.cpp:76, 255 | Low | nothing cached: every entity re-lexes its model def, inherit chain and .md5anim at load | per-Spawn caches | RUNTIME (check listDressing) | open |
| 11 | NullGame.cpp:656-657 vs NullGameDressing.cpp:422,648 | Low | the nullgame's ParseSpawnArgsToRenderLight/Entity are memset stubs while the dressing implements those parses; `com_title == "quake4"` check duplicated (NullGame.cpp:227, NullGameDressing.cpp:743) | back the stubs with the dressing's parse; one helper | RUNTIME / SAFE | open |
| 12 | DeclQuake4.cpp:46-80, 105-138, 300-327 | Low | three decl Parses repeat the opening steps and the "missing }" warning + MakeDefault | a helper + shared Fail() | SAFE | deferred |

The ~100 one-line forwarders in GameQ4 / RenderWorldQ4 / SoundSystemQ4 / CollisionModelManagerQ4
are what the frozen ABI forces; GameTitle.cpp is already table-driven. Engine-wide 16 direct
GAME_TITLE_* compares remain in 10 files (see framework #8).

## Framework, sound, collision, idlib, sys

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | idlib/Str.cpp:1763 D3_vsnprintfC99 (MinGW / old MSVC) | High | `_vscprintf` measured every string before `_vsnprintf` wrote it: every va(), idStr::Format, Printf formatted twice on the Windows build | write first on a va_copy, measure only on truncation | SAFE | 308ef9dd |
| 2 | snd_cache.cpp:247, Image_init.cpp:2228-2263, Model_md5.cpp:1130 | High | three copies of the same worker pool (thread clamp, atomic index, thread vector, join, Mem/FS thread-safe window, capture flush + warning) | one Com_ParallelFor + window + flush helper | SAFE | aa77b67b (framework/ParallelFor.h, Com_FlushThreadCaptureWarn) |
| 3 | snd_shader.cpp:460 CheckShakesAndOgg (snd_emitter.cpp:701, 936) | Med | runs on every start / parm update of a shaking sound: a linear FindIndex over a grow-only static list just to gate a DPrintf; return ignored; stale pointers after a decl reload; two copied loops | parse-time note only; one loop | SAFE | ce500123 |
| 4 | Common.cpp:1538, 1566 CheckConfigSchema | Med | two identical "move a cvar off its old default" loops; the 3->4 step does it by hand; ~120-line function grows per schema bump | Com_MoveOldDefaults | SAFE | 4a8a8297 (the two loops; the 3->4 step and a step table deferred) |
| 5 | Console.cpp:332, 370 | Med | SCR_DrawEmissiveLabels and SCR_DrawEmAuthHover each draw the 4-offset black halo + text twice (TTF and small-char): four copies; Console.cpp includes renderer/tr_local.h for two getters | SCR_DrawHaloText*; expose the getters through RenderSystem.h | SAFE | 86920793 (halo helpers; the include deferred) |
| 6 | snd_cache.cpp:769-790 DecodeCpu | Med | three near-identical float->short clamp loops (stride 4 / 2 / 1) | one loop over a stride | SAFE | d7c31f1f |
| 7 | Common.cpp:3305-3420 idCommonLocal::Frame | Med | two blocks with function-local statics added inline to an already long Frame(): the com_speeds / GPU-serial print (~30 lines) and the VRR refresh probe / cap (~25) | Com_PrintSpeeds, Com_EffectiveMaxFPS | SAFE | deferred |
| 8 | Common.cpp:434,1455; Session.cpp:2031,2342; snd_world.cpp:1766; DeclManager.cpp:879 | Low/Med | six raw `title == GAME_TITLE_QUAKE4` checks where everything else reads profile flags | profile booleans or GameTitle::IsQuake4() | SAFE | deferred |
| 9 | Common.cpp:1461 vs FileSystem.cpp:2436 | Low | Com_AutoBindGamepad's own Icmp ladder on fs_game / fs_game_base for "d3xp"/"d3le"; FS_KnownModBase / HasD3XP encode that | FS_GameIsRoEBased | SAFE | deferred |
| 10 | FileSystem.cpp:3764 vs 3095 | Low | ReadFileVersions repeats Shutdown's pak teardown by hand | FreePak | SAFE | deferred |
| 11 | Session.cpp:1649,1755; 191,236,3720,3724 | Low | the si_entityFilter read + `_<filter>` append twice; Map_f/DevMap_f copy the filter-arg block; the resume step (SetPlayingSoundWorld, unpause, StartWipe) at three sites | ActiveEntityFilter, ResumeGameSound | SAFE | deferred |
| 12 | LangDict.cpp:~180-205; snd_shader.cpp:48/171; CVarSystem.cpp:353 | Low | FindString repeats the hash probe for the exact and the Raven key; idSoundShader::Init and ParseShader reset the same new fields; CVAR_SPECIAL_CONCAT rebuilds the string with two temporaries per `^ ` (quadratic) | Lookup(key); ResetQ4Fields; compact in place | SAFE | deferred |

Checked with nothing to report: Str.cpp's -173 is the removed `#if 0` allocator plus an
extension refactor; the Heap16 rewrite is equivalent; CM_AllocatedInBlock fixes a real leak; no new
function over 300 lines; no leftover diagnostic cvars in this area.

## Settings menu, UI, game trees

| # | Where | Sev | What | Fix | Tag | Fixed |
|---|-------|-----|------|-----|-----|-------|
| 1 | Dhewm3SettingsMenu.cpp:3517 ApplyEnhancementPreset (283 lines), :3800 DetectEnhancementPreset (136) | Med | ~40 raster and ~20 RT-overlay fields written by hand twice (Set in Apply, == with ad-hoc tolerances in Detect); doubled since the last review; a missed one reads "Custom" | a static column table {idCVar*, offsetof, type, tol} driving both loops | SAFE (if tolerances match exactly) | deferred (structural) |
| 2 | Dhewm3SettingsMenu.cpp:3586, 3766, 3883, 3917 (+3625, 3767, 3902) | Med | the SSR intensity rule `ssr ? 0.8 : 0.5` in 4 places; `rtReflections ? 0.5 : 1.0` in 3 | PresetSsrIntensity, PresetRtReflResScale | SAFE | 9ecced33 |
| 3 | Dhewm3SettingsMenu.cpp:3754-3760 | Low | the Path Tracing DI resolution / checkerboard ladder is an if/else on preset index inside Apply; every other per-tier value lives in rtAugment[] | diResScale / diCheckerboard columns | SAFE | deferred |
| 4 | Dhewm3SettingsMenu.cpp:4946-4950, 4969-4973, 5072-5090, 2537/2541 | Med | r_vkDebugHud, r_vkCrashLogging, the skin SSS sliders and the film-grain sliders re-roll GetCVar + ImGui widget + SetCVar + tip beside the Cvar* helpers; skin bodies mis-indented | CvarCheckbox / CvarSliderF | SAFE | 2649959f (checkboxes + skin sliders; the grain sliders use a different tooltip helper, left) |
| 5 | Dhewm3SettingsMenu.cpp:3893-3894, 3918-3919 and Apply | Low | three cvar-access styles in one function (extern, string API, NamedCvar); 65 string-API calls in the file | NamedCvar or externs | SAFE | deferred |
| 6 | game/PlayerView.cpp:583, 590, 634, 771, 783 | Med | per-frame GetCVarBool("r_rhiActive") x3 and GetCVarFloat("r_berserkFade"); r_berserkFade is the game-to-renderer cvar channel flagged last review, now also read back | cache idCVar* (SAFE); a renderSystem query / renderView field instead of the cvar channel (RUNTIME) | mixed | 701ac3d1 (cached pointers; the channel stays) |
| 7 | game/Player.cpp:6452-6458 == 8384-8390 (d3xp 7609-7615 == 9880-9886) | Low | the third-person noSelfShadow block pasted in Think and ClientPredictionThink, both trees | one member | SAFE | c4e9f571 |
| 8 | game/Player.cpp:7584-7593 (same in d3xp) | Low | inside `if ( g_stopTime && !pm_thirdPerson )` a nested `if ( !pm_thirdPerson )` is always true; a 4-tab comment | drop the test, fix the indent | SAFE | 7c00f8d9 |
| 9 | ui/Window.cpp:4486-4560 (+4419), Window.h:503, SimpleWindow.h:109 | Med | every anchor routine branches on simp vs win and copies the cstAnchor/To/Factor triple field by field (~6 parallel pairs) | one cstAnchorState_t member / accessor (savegame write order unchanged) | SAFE | deferred |
| 10 | ui/UserInterface.cpp:341-420 HudAnchorKind / AnchorHud | Low | the kind is a magic int 0..3 | an enum | SAFE | deferred |
| 11 | ui/GameSSDWindow.cpp (six ReadFromSaveGame pool loops) | Low | the same 8 added lines per loop (count, failed guard, bounds + fail); GetSpecificEntity re-checks bounds 6 more times | a ReadPool template; one bounds table | SAFE | deferred |
| 12 | game/Game_local.h vs d3xp/Game_local.h | Low | the per-map tic length implemented differently (member float vs a mutable global named like a constant) | a member in d3xp too | SAFE | deferred |

game/ and d3xp/ are otherwise in close sync (Anim_Blend, Player, Entity, Weapon added hunks are
byte-identical). The menu's new rows mostly use the helpers (raw widget calls 90 -> 96, helper calls
125 -> 266 since the last review). interpPose_t dedup: still deferred; third person did not add a
fourth prev/cur state machine (count stays at three); render interpolation added a separate
prev/cur history to idAnimator.

## What this branch did

Batch 1 (build green, dude + base + d3xp + d3le + nullgame):

- 308ef9dd perf(idlib): format once in D3_vsnprintfC99 on the old Windows runtimes
- ce500123 perf(sound): note shakes-on-OGG once at parse, not on every sound start
- 81aecde8 refactor(rhi): RB_RHI_BindRTImage invalidates the GL texture-unit cache itself
- ccb53211 cleanup(rhi): drop the ReSTIR DI code the ReLAX-only decision (R6) left dead
- 0e80d0e8 perf(rhi): hash the shader-name lookup in both backends
- d73c0739 cleanup(rt): drop the emissive mover-upkeep profiling wrapper
- eedb2d98 refactor(rt): one emissive-table teardown list, and the two repeated idioms
- c37c292f perf(renderer): UpdateForcedAmbient returns before rescanning the area models
- c83d5c43 perf(renderer): classify decals once at parse, not per draw
- 33cf56a7 perf(bse): no per-particle copy on build, one-pass view compaction, kept request buffer
- ea47492c cleanup(bse): drop idEffectSegment::temporary, written and never read
- 342b6acc refactor(light editor): hashed fixture lookups, resolved materials, one find-or-create and prune

Batch 2 (build green, same targets):

- aa77b67b refactor(framework): one worker pool behind the parallel image, sound and tess-bake loads
- 4a8a8297 refactor(framework): Com_MoveOldDefaults for the config-schema retune steps
- d7c31f1f cleanup(sound): one clamp loop over a stride in DecodeCpu's downsample
- 86920793 cleanup(console): SCR_DrawHaloTextTtf / Small for the emissive labels and the editor hover line
- c228ae38 cleanup(quake4): facade folds - Effect(), AssureSize, one suppress-mask poke, one trace conversion
- 262df64a perf(quake4): Q4Settings_Frame finds s_volume_dB and s_device once
- 9ecced33 cleanup(menu): PresetSsrIntensity / PresetRtReflResScale shared by Apply and Detect
- 2649959f cleanup(menu): the Vulkan HUD / crash-logging checkboxes and the skin sliders use the Cvar* helpers
- 701ac3d1 perf(game): PlayerView finds the renderer cvars it reads every frame once
- c4e9f571 cleanup(game): idPlayer::UpdateBodySelfShadow for the third-person body shadow flags
- 7c00f8d9 cleanup(game): drop the always-true inner third-person test in the stopped-time view

## Needs an in-game A/B (I cannot run the game)

- **GL3**: any post pass that binds a render target then an idImage on the same unit (SSAO, SSR,
  berserk / helltime trails, ReSTIR gradient): 81aecde8 moved the cache invalidation into the
  helper; a wrong bind would show as a stale texture on one unit.
- **Decals** on both backends: blood sheen, matte translucent decals, glass panes, Quake 4's
  wallbullets lit highlights (c83d5c43 classifies at parse; `reloadDecls` must re-classify).
- **ReSTIR DI (Path Tracing tier)**: picture unchanged (ccb53211 folded constants that were
  always the same at runtime).
- **Light editor** in emissive mode: fixtures still resolve their authored emit / look; Del on a
  non-emitting surface no longer dirties the file (342b6acc).
- **BSE** in Quake 4: constant-segment effects (flames, steam) and lit effects unchanged
  (33cf56a7); child effect / debris requests still delivered.
- **Level load**: the parallel image / sound / tess-bake passes still run on all threads and still
  warn once when a worker errors (aa77b67b).
- **Windows build**: string formatting unchanged (308ef9dd) - a MinGW build is needed to exercise it.

## Deferred, for a go / no-go (large or runtime-sensitive)

1. RB_RHI_ReStirDIPass decomposition (739 lines) and the SSR one still open from last time; both
   want the EnsureRT reuse (RHI #2) done in the same pass, with the user validating the Path
   Tracing tier as it goes (the method used for DrawInteraction / DrawWorld / RenderShaderPasses).
2. tr_main.cpp split: tr_rtscene.cpp (builders, refresh), tr_rtvalidate.cpp (self-tests),
   tr_rtemissive.cpp (the E1/E2 table). Pure movement, but 4,000 lines of it.
3. The VK boilerplate helpers: sync-submit pair (12 sites), StorageImage lifecycle (3), device
   buffer ensure (3), transient storage (4). All SAFE on paper; each touches teardown order, so
   r_vkValidation runs are the check.
4. The menu preset column table (Apply / Detect, ~80 lines each way).
5. The BSE and DeclGuides table-driven parsers, the BSE domain precompute, Model_md5's helpers and
   UpdateSurface split.
6. The runtime-sensitive perf items: BSE Insert order / Resize, InstantiateDynamicModel's
   per-frame snapshot, Image_load's stats gating, R_EmAuthOverrideLight's caching, the nullgame
   dressing caches, the CPU tess-bake fallback's fate.
