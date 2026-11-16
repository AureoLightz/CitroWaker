#pragma once
#include <stdlib.h>
#include <cstdio>
#include <cstring>

extern "C" void n3dsUiSetTopHudEnabled(int) __attribute__((weak));

// Optimization settings only: never omit effects, geometry or material passes.
struct Performance3ds {
  int core2=1;   // user 2026-11-10: "use them all" -> core-2 rendering on by default
  bool packedUniforms=true;
  bool planReuse=true;
  bool materialCache=true;
  bool geometryCache=false;
  bool fogPreparation=true;
  bool vertexRuns=true;
  bool vertexPlan=true;
  bool vertexDecode=true;
  bool paletteCache=true;
  bool bottomUi=true;
  bool widescreen=true,topHud=true,frameCounter=false; // Player presentation preferences.
  bool drawTiming=false;
  bool sharedMaterials=false; // immutable decoded TEV/texgen vectors
  bool uniformDelta=false; // shared non-pose identity avoids repacking static rows
  bool modelPrograms=false; // compiled ordered replay program, experimental
  bool preparedMaterials=false; // PC-decoded MDL3 layouts, experimental
  bool modelMaterialIds=false; // audited GD mutation identities, experimental
  bool directSnapshot=false; // immutable program registry per recorded frame, experimental
  bool modelDirect=false; // versioned material/group receiver, experimental
  bool stateTemplates=false; // static decoded-state reuse, experimental
  bool stateCpIncremental=false; // narrow VCD/VAT invalidation, experimental
  bool uniformPieces=false; // independent immutable uniform blocks, experimental
  int directSlots=128; // bounded experimental direct program capacity
  bool directEvents=false; // direct compact recorded commands, experimental
  bool preparedShapes=false; // PC-prepared primitive execution, experimental
  bool preparedDraw=false; // PC-prepared PICA draw words; experimental
  bool materialRecords=false; // owned compiled material references, experimental
  bool modelRecords=false; // indexed PNGP group records, experimental
  bool compactPoseStorage=false; // exact used-row allocation, experimental
  bool commandBlocks=false; // bounded ordered recorded command templates, experimental
  bool pipelineStage8=true;  // T1: bounded 8 MiB staging; default on (user 2026-11-10).
  bool leanState=true;       // T3 (Claude): skip unread diagnostic GxState notes, exact; default on (user 2026-11-10).
  int retainKb=4096;        // (Claude v83) retain_kb: retained-geometry budget (decoded geometry + records + snapshots held for the next frame)
  bool retainGeometry=true;  // (Claude v83) translate-once, step 1: retained geometry in the recorder (hw6 gxretain.h), exact; fixture 43.6 -> 40.5 ms, less heap. 0 = decode every frame
  bool stateInPlace=true;    // Claude: decode each batch's GX state into its event (no 3 KB temporary), exact; default on (fixture -6.4 ms, user 2026-11-10).
  bool wideCull=true; // Match full-width widescreen visibility; 0 selects reference.
  bool fifoThread=true;      // Claude: Aurora FIFO worker (GX parse + decode) on its own core (fifo_core), async end of frame; 0 = inline on the game thread. Default on with fifo_core=3: 73.0 -> 62.5 ms on the fixture (2026-11-10); on core 1 it was 3% slower than inline.
  bool core1Threads=true;    // Claude: secondary threads (audio/DSP emulation, loading, alarms) on core 1; the game thread keeps core 0. Default on (user: "use them all").
  int fifoCore=3;            // Claude: core of the FIFO worker (3 = New 3DS spare core, default; 1 = shared system core). If the console refuses the core, the FIFO runs inline.
  bool decodeHelper=false;   // Claude: vertex float/colour decode on a helper thread (core decode_core) while the FIFO worker goes on; needs fifo_thread=1. Same values.
  int decodeCore=1;
  bool heapFence=true;       // Claude (default on: fixture 92.9 -> 72.9 ms, soak to 3660 clean): core-2 staging fences on the real host heap (in use > 85 MiB or < 3 MiB sbrk room left) instead of the estimate alone.
  bool packetWorker=false,packetWorkerVerify=false; // core1 pure packet pipeline, experimental
  int fastAllocSpans=64;      // (Claude, Nov 15) span cap, 64 KB each. Was 160 (10 MiB): spans never leave their size class, so a long console session
                              // grew to 154 spans (9.9 MB) and starved draw replay of heap (budget pinned at its 512 KB floor, hit rate 51%). Past
                              // the cap small blocks come from newlib, which reuses memory across sizes. performance.ini fast_alloc_spans=32..160
  bool fastAlloc=true;        // Claude v76 (default on: fixture 49.4 -> 47.6 ms, render/recorder lock calls -70%, screenshots identical): per-thread small-block allocator (main_3ds.cpp fa::), no lock on most malloc/free
  bool drawReuse=false;       // Claude v73: cross-frame draw reuse (hw6 src3ds/drawreuse3ds.h), exact
  int drawReuseKb=2560;       // its linear-memory budget
  bool depthFold=true;         // (Claude v90, default on: the console PICA ignores the depth map scale/offset registers; the map goes into the projection instead, pipeline.h) 0 = registers (Azahar only)
  bool planDelta=true;         // (Claude v87, default on: N3DS_PLAN_DELTA_VERIFY found 0 mismatches in ~260 delta plans/frame over the fixture, shots equal the reference; render plan -1 ms/frame) adjacent draws with the same compiled draw and only TEV registers / matrices / view / lights changed: plan rebuilt from the previous plan (geometry, uniforms, texenv, effects), exact; verify with N3DS_PLAN_DELTA_VERIFY=1
  bool audioCore1=true;   // (Claude v114) the game's JAudio thread (sequencer + emulated DSP rendering) on core 1 (APT_SetAppCpuTimeLimit 80%): audio_core1=0 = core 0
  int bgCore=0;   // (Claude v111) default 0: console v110 on core 2 cost the renderer ~20 ms/frame (41 -> 61 ms at ~350 draws) to save ~10 ms of game thread.   // (Claude v110/v111) core for the game threads: 2/3 = New 3DS cores (default 2), 1 = borrow the system core (v104-v110; HOME close error), 0 = game core
  bool depthReverse=true;  // (Claude v107) reversed Z with depth_z (pipeline.h g_picaDepthReverse): float24 precision for distant geometry; 0 = forward
  bool depthZ=true;   // (Claude v107: default on, console W-buffer is unusable; was the v93 experiment)  // (Claude v93) EXPERIMENT: Z depth map for every draw, fog off (pipeline.h g_picaForceZDepth): is the console overlap the W-buffer?
  int foliageFar=3000; // (Claude v100, default 3000 per user 2026-11-13 "whatever increases framerate the most"): grass/flower/tree culling distance limit in game units; 0 = original distance
  bool j3dGeometry=true; // (Claude v97: default on) J3D shapes sent by reference (v95); output identical in the emulator.
  bool j3dPrograms=false; // J3D material command programs, exact, experimental.
  bool textureShards=false; // Experimental PC split pack, bounded8KiB read cache.
  bool texturePack=false;  // (Claude v92) textures converted on the PC ahead of time (N3DS_ROOT/ww_texpack.bin, texpack/mktexpack): cache misses read them instead of converting; exact
  bool residentGeometry=true;  // (Claude v86, default on: exact, fixture 40.0 -> 38.1 ms, soak clean; 0 = pack every frame) translate-once, step 2: packed buffers of retained geometry stay in linear memory across frames (no packing, no arena copy), exact
  bool drawReplay=true;        // (Claude v117) direct path step 1: unchanged draws of the previous frame are submitted from their finished plan (renderer3ds.h ReplayEntry)
  int replayKb=4096;           // (Claude v117; v119 3072 -> 4096: recorded entries are larger) its heap budget at most (entries ~4-5 KB); gpu_3ds.cpp lowers it to keep 7 MiB of the heap free
  bool frameFlushAll=false;    // (Claude v118) true = C3D_FrameEnd(0): flush all 18 MB of linear memory from the CPU cache every frame (pre-v118); false = only the command list + draw arena (the rest is flushed where written)
  bool drawRecord=true;        // (Claude v119) replay hits sent as recorded command words (hw6 Backend3ds::drawRecorded); 0 = through citro3d's setters
  int residentKb=2048;         // its linear-memory budget
  int packetWorkerCore=1;     // Claude: core of the packet worker (1 = system core as before; 0 = game core at lowest app priority, uses its idle time)
  bool recordTightGrowth=false; // 25% decoded-buffer growth slack, experimental
  bool uniformSections=true;  // compact exact immutable uniform rows (ChatGPT; ARM parity exact). (Claude v104) default on: half the uniform bytes per batch on a memory-latency-bound console; 0 = flat 2 KB blocks
  bool recordCompact=true;  // Claude v72: default on (exact: 96/96 ARM parity, fixture screenshots identical; ~0.6-1.2 MB less per recorded frame; v71 console OOM)
  bool texErrorTable=false; // exact faster texture trial, console A/B pending
  bool decodeOverwrite=false; // scalar decode without redundant zero pass (exact; Claude v64: back off until a console A/B shows a gain)
  bool planTemplates=false;
  bool matrixBatch=true;   // (Claude v68) default on: 96 ARM colour+depth parity files exact; replay render cliff -19% / aerial -9%; fixture shots identical
  bool directPack=false; // zero-copy interleaved packing (exact; Claude v64: back off, console build stage looked slower with it)
  bool planarPack=false;   // (Claude v77) one vertex array per attribute (exact: 64/64 ARM parity); with direct_pack, packed straight into GPU memory
  int autoAb=0;            // (Claude v83: off, switches nothing; v81 console A/B: helper 10.1 fps vs inline 11.7) (Claude v81) built-in console A/B: alternate every 120 frames between vertex decode inline and on the decode helper (core decode_core), log both ([ab]); 0 = off. decode_helper=1: always on
  bool packVertexMajor=false;   // (Claude v65) pack each vertex in one pass (exact); console A/B pending
  bool stagingPartial=true;    // (Claude v67) short heap: wait only until the render thread has freed enough, not for the whole frame
  int gpuKick=48;        // (Claude v120) gpu_kick: the GPU starts on the frame's command list during the frame (every gpu_split hand-off, at most this many draws apart) instead of at its end; 0 = old (GPU only after the whole frame is built)
  int gpuSplit=2;        // (Claude v63) percent of the 2 MiB command buffer between GPU hand-offs; 25 = old behaviour
  bool planNotes=false; // unused diagnostic strings off; failures/effects unchanged
  bool vertexPoolFifo=false; // same bounded geometry capacity, preserve frame order
  bool statePool=false; // experimental bounded decoded-vector capacity reuse
  bool stateIncremental=true;  // Claude 2026-11-11: decode only the state sections whose registers changed since the previous batch (exact)
  bool snapshotSkip=true; // (Claude v102) textures the renderer's cache already holds are hashed, not copied, at frame end
  bool snapshotCopies=true; // (Claude v116) EFB copy destinations (GPU textures) are not copied at frame end
  bool earlyRelease=true;   // (Claude v115) early_release: the game thread resumes once the frame is decoded + snapshotted (not after the render hand-off)
  bool realShadows=false;  // (Claude v129: default off - the real-shadow caster pass left grey character silhouettes on screen on console, e.g. Outset; the game's round shadow is used. real_shadows=1 to compare) (Claude v114) real_shadows=0: model-shaped (rendered) shadows replaced by the round simple shadow
  bool skipDraw=true;   // (Claude v114) skip_draw: frames that will not be painted (frame_skip) do not run the actor draw pass either; skip_draw=0 = always run it
  int frameSkip=1;   // (Claude v113: default 1 again: the user wants game speed independent of paint rate; v104-v112 default 0) game logic keeps 30 Hz: up to N frames in a row are not painted while the game runs behind real time (0 = off)
  bool rigidClip=false; // Experimental guarded clip rejection for rigid retained triangles.
  bool paintFractional=false; // Experimental measured-cost paint budget; clock unchanged.
  int paintPacing=1;   // (Claude v120) paint_pacing=1: game logic on an absolute 30 Hz clock (catch-up when behind, sleep when ahead); a tick is painted
                       // only when the render core can take its frame without stalling the game, at a steady cadence (every K ticks, K adapts 1-4).
                       // 0 = v103-v119 frame_skip scheduler (game waits for the renderer: logic speed follows the paint rate)
  int paintMaxGap=2;   // (Claude v120) paint_max_gap: at most this many logic ticks between paints (2-8); a paint then waits for the renderer.
                       // (v121) default 2: console v120 with 6 painted every 4th tick at full logic speed - motion jumped 4 ticks per frame
                       // and felt too fast; with 2 the game slows down evenly when the renderer needs more than 2 ticks
  bool presentPace=false; // (Claude v120) present_pace: the render core submits each painted frame at a steady delay after its logic tick (even display intervals)
  int paintCadence=0;  // (Claude v120) paint_cadence: 0 = adaptive K, 1-4 = paint every K ticks (when the renderer is free)
  bool batchFx=true;  // (Claude v102) horizon clouds / sea waves: one draw per layer with vertex colours (game side, d_kankyo_rain.cpp)
  int poolFloor=512; // (Claude v102) event / uniform pool blocks kept when the heap fence trims (0 = empty them, v86-v101)
  bool directTexture=true; // exact final encoding into GPU storage; avoids multi-MiB host temporary
  bool typedGx=false; // ChatGPT B: ordered direct SDK registers, default off until measured.
  bool uiMapBottom=false; // Claude v80: the minimap's original draws on the bottom screen (ui_route_3ds.h); off until a console A/B
  unsigned errors=0;
  static char* trim(char* s){while(*s==' '||*s=='\t')++s;size_t n=std::strlen(s);while(n&&(s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n'))s[--n]=0;return s;}
  void read(FILE* f){
    char line[256];while(std::fgets(line,sizeof line,f)){
      if(!std::strchr(line,'\n')&&!std::feof(f)){int c;while((c=std::fgetc(f))!='\n'&&c!=EOF){}++errors;continue;}
      if(char* c=std::strpbrk(line,"#;"))*c=0;
      char* key=trim(line);if(!*key)continue;char* eq=std::strchr(key,'=');
      if(!eq){++errors;continue;}*eq=0;key=trim(key);char* val=trim(eq+1);
      if(!std::strcmp(key,"core2")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))core2=*val-'0';
      else if(!std::strcmp(key,"material_cache")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))materialCache=*val=='1';
      else if(!std::strcmp(key,"packed_uniforms")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))packedUniforms=*val=='1';
      else if(!std::strcmp(key,"plan_reuse")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))planReuse=*val=='1';
      else if(!std::strcmp(key,"fog_prepare")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))fogPreparation=*val=='1';
      else if(!std::strcmp(key,"vertex_plan")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))vertexPlan=*val=='1';
      else if(!std::strcmp(key,"bottom_ui")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))bottomUi=*val=='1';
      else if(!std::strcmp(key,"palette_cache")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))paletteCache=*val=='1';
      else if(!std::strcmp(key,"vertex_decode")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))vertexDecode=*val=='1';
      else if(!std::strcmp(key,"draw_timing")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))drawTiming=*val=='1';
      else if(!std::strcmp(key,"pipeline_stage8")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))pipelineStage8=*val=='1';
      else if(!std::strcmp(key,"geometry_cache")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))geometryCache=*val=='1';
      else if(!std::strcmp(key,"lean_state")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))leanState=*val=='1';
      else if(!std::strcmp(key,"state_inplace")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))stateInPlace=*val=='1';
      else if(!std::strcmp(key,"retain_kb")&&std::atoi(val)>=256&&std::atoi(val)<=16384)retainKb=std::atoi(val);
      else if(!std::strcmp(key,"retain_geometry")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))retainGeometry=*val=='1';
      else if(!std::strcmp(key,"vertex_runs")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))vertexRuns=*val=='1';
      else if(!std::strcmp(key,"widescreen")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))widescreen=*val=='1';
      else if(!std::strcmp(key,"frame_counter")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))frameCounter=*val=='1';
      else if(!std::strcmp(key,"wide_cull")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))wideCull=*val=='1';
      else if(!std::strcmp(key,"heap_fence")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))heapFence=*val=='1';
      else if(!std::strcmp(key,"fifo_thread")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))fifoThread=*val=='1';
      else if(!std::strcmp(key,"fifo_core")&&val[0]>='0'&&val[0]<='3'&&!val[1])fifoCore=*val-'0';
      else if(!std::strcmp(key,"decode_helper")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))decodeHelper=*val=='1';
      else if(!std::strcmp(key,"decode_core")&&val[0]>='0'&&val[0]<='3'&&!val[1])decodeCore=*val-'0';
      else if(!std::strcmp(key,"core1_threads")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))core1Threads=*val=='1';
      else if(!std::strcmp(key,"packet_worker")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))packetWorker=*val=='1';
      else if(!std::strcmp(key,"fast_alloc_spans")&&atoi(val)>=32&&atoi(val)<=160)fastAllocSpans=atoi(val);
      else if(!std::strcmp(key,"fast_alloc")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))fastAlloc=*val=='1';
      else if(!std::strcmp(key,"draw_reuse")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))drawReuse=*val=='1';
      else if(!std::strcmp(key,"draw_reuse_kb")&&atoi(val)>=256&&atoi(val)<=8192)drawReuseKb=atoi(val);
      else if(!std::strcmp(key,"depth_fold")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))depthFold=*val=='1';
      else if(!std::strcmp(key,"plan_delta")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))planDelta=*val=='1';
      else if(!std::strcmp(key,"bg_core")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")||!std::strcmp(val,"2")||!std::strcmp(val,"3")))bgCore=*val-'0';
      else if(!std::strcmp(key,"depth_reverse")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))depthReverse=*val=='1';
      else if(!std::strcmp(key,"depth_z")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))depthZ=*val=='1';
      else if(!std::strcmp(key,"snapshot_skip")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))snapshotSkip=*val=='1';
      else if(!std::strcmp(key,"snapshot_copies")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))snapshotCopies=*val=='1';
      else if(!std::strcmp(key,"audio_core1")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))audioCore1=*val=='1';
      else if(!std::strcmp(key,"early_release")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))earlyRelease=*val=='1';
      else if(!std::strcmp(key,"real_shadows")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))realShadows=*val=='1';
      else if(!std::strcmp(key,"skip_draw")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))skipDraw=*val=='1';
      else if(!std::strcmp(key,"frame_skip")){const int v=atoi(val);if(v>=0&&v<=3)frameSkip=v;else ++errors;}
      else if(!std::strcmp(key,"rigid_clip")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))rigidClip=*val=='1';
      else if(!std::strcmp(key,"paint_fractional")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))paintFractional=*val=='1';
      else if(!std::strcmp(key,"paint_pacing")){const int v=atoi(val);if(v>=0&&v<=1)paintPacing=v;else ++errors;}
      else if(!std::strcmp(key,"paint_max_gap")){const int v=atoi(val);if(v>=2&&v<=8)paintMaxGap=v;else ++errors;}
      else if(!std::strcmp(key,"present_pace")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))presentPace=*val=='1';
      else if(!std::strcmp(key,"paint_cadence")){const int v=atoi(val);if(v>=0&&v<=4)paintCadence=v;else ++errors;}
      else if(!std::strcmp(key,"batch_fx")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))batchFx=*val=='1';
      else if(!std::strcmp(key,"pool_floor")){const int v=atoi(val);if(v>=0&&v<=768)poolFloor=v;else ++errors;}
      else if(!std::strcmp(key,"foliage_far")){const int v=atoi(val);if(v>=0&&v<=100000)foliageFar=v;else ++errors;}
      else if(!std::strcmp(key,"j3d_geometry")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))j3dGeometry=*val=='1';
      else if(!std::strcmp(key,"j3d_programs")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))j3dPrograms=*val=='1';
      else if(!std::strcmp(key,"texture_shards")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))textureShards=*val=='1';
      else if(!std::strcmp(key,"texture_pack")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))texturePack=*val=='1';
      else if(!std::strcmp(key,"resident_geometry")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))residentGeometry=*val=='1';
      else if(!std::strcmp(key,"shared_materials")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))sharedMaterials=*val=='1';
      else if(!std::strcmp(key,"uniform_delta")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))uniformDelta=*val=='1';
      else if(!std::strcmp(key,"model_programs")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))modelPrograms=*val=='1';
      else if(!std::strcmp(key,"prepared_materials")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))preparedMaterials=*val=='1';
      else if(!std::strcmp(key,"model_material_ids")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))modelMaterialIds=*val=='1';
      else if(!std::strcmp(key,"direct_snapshot")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))directSnapshot=*val=='1';
      else if(!std::strcmp(key,"model_direct")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))modelDirect=*val=='1';
      else if(!std::strcmp(key,"state_templates")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))stateTemplates=*val=='1';
      else if(!std::strcmp(key,"state_cp_incremental")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))stateCpIncremental=*val=='1';
      else if(!std::strcmp(key,"uniform_pieces")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))uniformPieces=*val=='1';
      else if(!std::strcmp(key,"direct_slots")&&(!std::strcmp(val,"128")||!std::strcmp(val,"256")||!std::strcmp(val,"512")))directSlots=std::atoi(val);
      else if(!std::strcmp(key,"direct_events")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))directEvents=*val=='1';
      else if(!std::strcmp(key,"prepared_shapes")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))preparedShapes=*val=='1';
      else if(!std::strcmp(key,"prepared_draw")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))preparedDraw=*val=='1';
      else if(!std::strcmp(key,"material_records")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))materialRecords=*val=='1';
      else if(!std::strcmp(key,"model_records")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))modelRecords=*val=='1';
      else if(!std::strcmp(key,"compact_pose")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))compactPoseStorage=*val=='1';
      else if(!std::strcmp(key,"command_blocks")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))commandBlocks=*val=='1';
      else if(!std::strcmp(key,"draw_record")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))drawRecord=*val=='1';
      else if(!std::strcmp(key,"frame_flush_all")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))frameFlushAll=*val=='1';
      else if(!std::strcmp(key,"replay_kb")&&atoi(val)>=256&&atoi(val)<=8192)replayKb=atoi(val);
      else if(!std::strcmp(key,"draw_replay")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))drawReplay=*val=='1';
      else if(!std::strcmp(key,"resident_kb")&&atoi(val)>=256&&atoi(val)<=8192)residentKb=atoi(val);
      else if(!std::strcmp(key,"packet_worker_core")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))packetWorkerCore=*val-'0';
      else if(!std::strcmp(key,"packet_worker_verify")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))packetWorkerVerify=*val=='1';
      else if(!std::strcmp(key,"record_tight_growth")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))recordTightGrowth=*val=='1';
      else if(!std::strcmp(key,"uniform_sections")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))uniformSections=*val=='1';
      else if(!std::strcmp(key,"record_compact")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))recordCompact=*val=='1';
      else if(!std::strcmp(key,"tex_error_table")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))texErrorTable=*val=='1';
      else if(!std::strcmp(key,"decode_overwrite")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))decodeOverwrite=*val=='1';
      else if(!std::strcmp(key,"plan_templates")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))planTemplates=*val=='1';
      else if(!std::strcmp(key,"matrix_batch")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))matrixBatch=*val=='1';
      else if(!std::strcmp(key,"planar_pack")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))planarPack=*val=='1';
      else if(!std::strcmp(key,"auto_ab")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))autoAb=*val=='1';
      else if(!std::strcmp(key,"pack_vertex_major")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))packVertexMajor=*val=='1';
      else if(!std::strcmp(key,"staging_partial")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))stagingPartial=*val=='1';
      else if(!std::strcmp(key,"gpu_kick")){const int v=atoi(val);if(v>=0&&v<=4000)gpuKick=v;else ++errors;}
      else if(!std::strcmp(key,"gpu_split")&&atoi(val)>=1&&atoi(val)<=50)gpuSplit=atoi(val);
      else if(!std::strcmp(key,"direct_pack")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))directPack=*val=='1';
      else if(!std::strcmp(key,"plan_notes")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))planNotes=*val=='1';
      else if(!std::strcmp(key,"vertex_pool_fifo")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))vertexPoolFifo=*val=='1';
      else if(!std::strcmp(key,"state_incremental")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))stateIncremental=*val=='1';
      else if(!std::strcmp(key,"state_pool")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))statePool=*val=='1';
      else if(!std::strcmp(key,"direct_texture")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))directTexture=*val=='1';
      else if(!std::strcmp(key,"typed_gx")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))typedGx=*val=='1';
      else if(!std::strcmp(key,"top_hud")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1"))){topHud=*val=='1';if(n3dsUiSetTopHudEnabled)n3dsUiSetTopHudEnabled(topHud);}
      else if(!std::strcmp(key,"ui_map_bottom")&&(!std::strcmp(val,"0")||!std::strcmp(val,"1")))uiMapBottom=*val=='1';
      else ++errors; // Old quality-cut/profile/resolution keys have no effect.
    }
  }
  static const char* example(){return "# Fidelity-preserving optimizations only. Restart after editing.\n# Core-2 rendering: 0=off, 1=on (default)\ncore2=1\n# Experimental exact decoded-geometry reuse, bounded3MiB: 0=off, 1=on\ngeometry_cache=0\n# Exact pre-decode primitive coalescing: 0=off, 1=on (default)\nvertex_runs=1\n# Exact consecutive-material cache shortcut: 0=off, 1=on\nmaterial_cache=1\n# Reusable draw plans: 0=off, 1=on (default)\nplan_reuse=1\n# Fixed uniform storage: 0=off, 1=on (default)\npacked_uniforms=1\n# Identical fog preparation: 0=off, 1=on (default)\nfog_prepare=1\n# Cached vertex-decode recipes: 0=off, 1=on\nvertex_plan=1\n# Specialized exact attribute loops: 0=off, 1=on\nvertex_decode=1\n# Exact palette hash memo: 0=off, 1=on\npalette_cache=1\n# Bottom status panel: 0=blank, 1=panel\nbottom_ui=1\n# Detailed GPU timings: 0=logged frames only, 1=every frame\ndraw_timing=0\n" "# Bounded core2 staging: 0=2MiB, 1=8MiB (default1)\npipeline_stage8=1\n" "# Full-width widescreen culling correction: 0=reference, 1=on\nwide_cull=1\n" "# Secondary threads (audio, loading) on core 1: 0=off, 1=on (default1)\ncore1_threads=1\n" "# GX command translation on its own thread: 0=off (on the game thread), 1=on (default1)\nfifo_thread=1\n# Core of that thread: 3=spare New 3DS core (default3), 1=shared system core\nfifo_core=3\n" "# Core-2 staging waits only when the real heap is short: 0=estimate, 1=real heap (default1)\nheap_fence=1\n" "# Direct SDK register delivery, inline FIFO and CPU recorder only (experimental)\ntyped_gx=0\n# Exact direct texture encoding, 0=reference host upload, 1=default\ndirect_texture=1\n# Experimental bounded TEV/texgen vector-capacity reuse\nstate_pool=0\n# Experimental FIFO geometry-capacity reuse, same memory budget\nvertex_pool_fifo=0\n# GPU plan diagnostic text: 1=reference, 0=omit unread notes (effects unchanged)\nplan_notes=0\n# Experimental direct packing into the existing GPU arena\ndirect_pack=0\n# Experimental adjacent matrix-palette grouping\nmatrix_batch=0\n# Experimental bounded immutable GPU plan templates\nplan_templates=0\n# Experimental fully-overwritten decoded scalar buffers\ndecode_overwrite=0\n# Top HUD uses the full 400px width (hearts/buttons/rupees larger, in the corners): 0=original, 1=on (default1)\ntop_hud=1\n";}
};
