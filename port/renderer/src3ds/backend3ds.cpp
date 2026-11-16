#include "backend3ds.h"
extern float g_screenTexgen[4];
extern "C" void n3dsLog(const char* fmt, ...);   // (Claude v100) vtx.cpp: SCREEN texgen mapping

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace t2p { bool g_beProfOn = false; }   // (Claude v119 diag) replay hits: parts of draw() into g_replayProf[6..11]
namespace t2p { extern uint64_t g_replayProf[16]; }
extern "C" {   // (Claude v119) citro3d/source/record.c
size_t C3Dx_SharedSize(void); size_t C3Dx_OwnSize(void); bool C3Dx_Clean(void); void C3Dx_Capture(void* sharedOut, void* ownOut);
u32 C3Dx_Emit(const void* shared, const void* own, int group, u32* out, u32 cap, const u32* fogLutData, u32 units, int count, const void* indices);
void C3Dx_Apply(const void* shared, const void* own, int group, C3D_Tex* const texSlots[3]); bool C3Dx_Append(const u32* words, u32 n);
void C3Dx_Uniforms(const u8* ids, const float (*rows)[4], int n); void C3Dx_ForgetProgram(const shaderProgram_s* p);
bool C3Dx_IndexOffset(const void*,const void*,u32*);
bool C3Dx_FrameKick(void);   // (Claude v120) renderqueue.c
}
extern "C" void C3Di_UpdateContext(void);   // citro3d (exported, not in its public headers): writes the pending context state to the command buffer
// (Claude) Console GPU-hang bisection, round 5 (gpu_3ds.cpp picks the variant from 3ds/ww3ds/hwtest.txt; 0 = normal): variants 1-4 change the
// generated vertex shaders' outputs (vtx.cpp g_shaderOutTest); 5 skips textured draws (control: runs).
int g_hwTest = 0;
static int g_hwDrawNo = 0;
extern "C" void C3Di_DrawElementsUpdated(GPU_Primitive_t primitive, int count, int type, const void* indices);   // n3ds/citro3d (Claude v77)
namespace t2p {

// ---------------------------------------------------------------- the plan's numbering is libctru's: pin every value it relies on
static constexpr bool same(long a, long b) { return a == b; }
static_assert(same(GPU_BYTE, ctru::BYTE) && same(GPU_UNSIGNED_BYTE, ctru::UNSIGNED_BYTE) && same(GPU_SHORT, ctru::SHORT) && same(GPU_FLOAT, ctru::FLOAT), "attribute formats");
static_assert(same(GPU_CLAMP_TO_EDGE, ctru::CLAMP_TO_EDGE) && same(GPU_REPEAT, ctru::REPEAT) && same(GPU_MIRRORED_REPEAT, ctru::MIRRORED_REPEAT), "wrap modes");
static_assert(same(GPU_NEAREST, ctru::NEAREST) && same(GPU_LINEAR, ctru::LINEAR), "filters");
static_assert(same(GPU_CULL_NONE, ctru::CULL_NONE) && same(GPU_CULL_FRONT_CCW, ctru::CULL_FRONT_CCW) && same(GPU_CULL_BACK_CCW, ctru::CULL_BACK_CCW), "cull modes");
static_assert(same(GPU_WRITE_RED, ctru::WRITE_RED) && same(GPU_WRITE_GREEN, ctru::WRITE_GREEN) && same(GPU_WRITE_BLUE, ctru::WRITE_BLUE) && same(GPU_WRITE_ALPHA, ctru::WRITE_ALPHA) &&
              same(GPU_WRITE_DEPTH, ctru::WRITE_DEPTH), "write mask bits");
static_assert(same(GPU_NO_FOG, ctru::NO_FOG) && same(GPU_FOG, ctru::FOG), "fog modes");
static_assert(same(GPU_NEVER, PF_NEVER) && same(GPU_ALWAYS, PF_ALWAYS) && same(GPU_EQUAL, PF_EQUAL) && same(GPU_NOTEQUAL, PF_NOTEQUAL) && same(GPU_LESS, PF_LESS) &&
              same(GPU_LEQUAL, PF_LEQUAL) && same(GPU_GREATER, PF_GREATER) && same(GPU_GEQUAL, PF_GEQUAL), "test functions");
static_assert(GPU_BLEND_ADD == 0 && GPU_BLEND_SUBTRACT == 1 && GPU_BLEND_REVERSE_SUBTRACT == 2 && GPU_BLEND_MIN == 3 && GPU_BLEND_MAX == 4, "blend equations");
static_assert(GPU_ZERO == 0 && GPU_ONE == 1 && GPU_SRC_COLOR == 2 && GPU_ONE_MINUS_SRC_COLOR == 3 && GPU_DST_COLOR == 4 && GPU_ONE_MINUS_DST_COLOR == 5 &&
              GPU_SRC_ALPHA == 6 && GPU_ONE_MINUS_SRC_ALPHA == 7 && GPU_DST_ALPHA == 8 && GPU_ONE_MINUS_DST_ALPHA == 9, "blend factors");
static_assert(GPU_LOGICOP_CLEAR == 0 && GPU_LOGICOP_AND == 1 && GPU_LOGICOP_AND_REVERSE == 2 && GPU_LOGICOP_COPY == 3 && GPU_LOGICOP_SET == 4 && GPU_LOGICOP_COPY_INVERTED == 5 &&
              GPU_LOGICOP_NOOP == 6 && GPU_LOGICOP_INVERT == 7 && GPU_LOGICOP_NAND == 8 && GPU_LOGICOP_OR == 9 && GPU_LOGICOP_NOR == 10 && GPU_LOGICOP_XOR == 11 &&
              GPU_LOGICOP_EQUIV == 12 && GPU_LOGICOP_AND_INVERTED == 13 && GPU_LOGICOP_OR_REVERSE == 14 && GPU_LOGICOP_OR_INVERTED == 15, "logic ops");
static_assert(same(GPU_RGBA8, PTF_RGBA8) && same(GPU_RGB8, PTF_RGB8) && same(GPU_RGBA5551, PTF_RGB5A1) && same(GPU_RGB565, PTF_RGB565) && same(GPU_RGBA4, PTF_RGBA4) &&
              same(GPU_LA8, PTF_IA8) && same(GPU_HILO8, PTF_HILO8) && same(GPU_L8, PTF_I8) && same(GPU_A8, PTF_A8) && same(GPU_LA4, PTF_IA4) && same(GPU_L4, PTF_I4) &&
              same(GPU_A4, PTF_A4) && same(GPU_ETC1, PTF_ETC1) && same(GPU_ETC1A4, PTF_ETC1A4), "texture formats");
// C3D_TexEnv is the five register words of a texenv stage: the translator's HwStage is copied over it
static_assert(sizeof(C3D_TexEnv) == sizeof(HwStage) && sizeof(HwStage) == 20, "texenv size");
static_assert(offsetof(C3D_TexEnv, srcRgb) == 0 && offsetof(C3D_TexEnv, srcAlpha) == 2 && offsetof(C3D_TexEnv, opAll) == 4 && offsetof(C3D_TexEnv, funcRgb) == 8 &&
              offsetof(C3D_TexEnv, funcAlpha) == 10 && offsetof(C3D_TexEnv, color) == 12 && offsetof(C3D_TexEnv, scaleRgb) == 16 && offsetof(C3D_TexEnv, scaleAlpha) == 18, "texenv layout");
static_assert(offsetof(HwStage, sources) == 0 && offsetof(HwStage, modifiers) == 4 && offsetof(HwStage, ops) == 8 && offsetof(HwStage, constColor) == 12 && offsetof(HwStage, scales) == 16, "HwStage layout");
static_assert(sizeof(C3D_FogLut) == 128 * 4, "fog table size");
static_assert(sizeof(PVec) == 16, "PVec");

struct Backend3ds::ShaderHandle {
  DVLB_s* dvlb = nullptr;
  shaderProgram_s prog;
};

struct Backend3ds::GpuTex {  // what TexEntry::user points at
  C3D_Tex tex;
  uint64_t gen = 0;          // unique per upload (a freed C3D_Tex's address can be reused, the generation cannot)
  C3D_RenderTarget* rt = nullptr;   // rendered copies: the target the blit draws into
  C3D_RenderTarget* rtLevel[8] = {}; // (Claude v106) synthesized mipmapped textures (allocRenderedMips): one target per level (rtLevel[0] == rt)
  int levels = 1;
  bool view = false;         // a window onto someone else's memory (the main target's colour buffer): nothing to free
  // (Claude v100) padded view: the console samples a non-power-of-two texture with power-of-two row addressing (scrambled columns,
  // the "lines" and broken clouds/blur). `src`'s colour buffer is repacked into `stage` (power-of-two VRAM) before each read.
  C3D_RenderTarget* src = nullptr;
  void* stage = nullptr;
  uint32_t vramBytes = 0;    // (Claude v133) VRAM held by a rendered texture (allocRendered*), for the [copy] report
};
namespace {
uint64_t g_texGen = 0;
bool npotView() { static const bool v = getenv("N3DS_NPOT_VIEW") != nullptr; return v; }   // (Claude v100) old unpadded views, for A/B (read after env.txt is loaded)
}  // namespace

bool Backend3ds::fail(const std::string& s) {
  err_ = s;
  if (log_) log_(s);
  return false;
}

bool Backend3ds::init(size_t arenaBytes) {
  shutdown();
  arena_ = static_cast<u8*>(linearAlloc(arenaBytes));
  if (!arena_) return fail("linearAlloc for the draw arena failed");
  arenaSize_ = arenaBytes;
  arenaUsed_ = 0;
  if (!C3D_TexInit(&dummy_, 8, 8, GPU_RGBA8)) return fail("placeholder texture");
  std::memset(dummy_.data, 0, dummy_.size);
  C3D_TexFlush(&dummy_);
  dummyOk_ = true;
  invalidateCaches();
  return true;
}

void Backend3ds::freeGpuTex(GpuTex* g) {
  if (!g) return;
  if (g->vramBytes) { st_.rtLive--; st_.rtVramBytes -= g->vramBytes; }
  if (g->rt) C3D_RenderTargetDelete(g->rt);
  for (int l = 1; l < 8; l++) if (g->rtLevel[l]) C3D_RenderTargetDelete(g->rtLevel[l]);
  if (!g->view) C3D_TexDelete(&g->tex);
  if (g->stage) vramFree(g->stage);
  delete g;
}

void Backend3ds::trimRenderPool(size_t keep) {
  while (rtPool_.size() > keep) { freeGpuTex(rtPool_.front()); rtPool_.erase(rtPool_.begin()); }
}

void Backend3ds::shutdown() {
  uniformLayouts_.clear();commandBlocksCache_.clear();commandBlocksBytes_=0;
  for (auto& gb : garbage_) freeGpuTex(gb.first);
  for (GpuTex* g : rtPool_) freeGpuTex(g);
  rtPool_.clear();
  garbage_.clear();
  if (dummyOk_) { C3D_TexDelete(&dummy_); dummyOk_ = false; }
  if (arena_) { linearFree(arena_); arena_ = nullptr; }
  arenaSize_ = arenaUsed_ = 0;
}

void Backend3ds::invalidateCaches() {
  program_ = nullptr;
  attrValid_ = texenvValid_ = effectValid_ = scissorValid_ = false;
  std::memset(uniformValid_, 0, sizeof(uniformValid_));
  for (int u = 0; u < 3; u++) { bound_[u] = nullptr; boundKey_[u] = 0; }
}

void Backend3ds::beginFrame() {
  g_hwDrawNo = 0;
  arenaUsed_ = arenaFlushed_ = 0; splitAt_ = splitStep_; sinceKick_ = 0;
  frameNo_++;
  size_t keep = 0;
  for (size_t i = 0; i < garbage_.size(); i++) {
    if (garbage_[i].second < frameNo_ - 1) {   // released two or more frames ago: no queued list can still read it
      GpuTex* g = garbage_[i].first;
      if (g->rt) rtPool_.push_back(g);         // render targets cannot be deleted inside a frame (see trimRenderPool)
      else freeGpuTex(g);
    }
    else garbage_[keep++] = garbage_[i];
  }
  garbage_.resize(keep);
}

float Backend3ds::cmdUsage() {
  u32 size = 0, offset = 0;
  GPUCMD_GetBuffer(nullptr, &size, &offset);
  return size ? float(offset) / float(size) : 1.0f;
}

void Backend3ds::flushArena() {
  if (arenaUsed_ > arenaFlushed_) GSPGPU_FlushDataCache(arena_ + arenaFlushed_, u32(arenaUsed_ - arenaFlushed_));
  arenaFlushed_ = arenaUsed_;
}

void Backend3ds::beginTarget(C3D_RenderTarget* t) {
  C3D_FrameDrawOn(t);          // also resets the viewport and the scissor
  invalidateCaches();
}

void* Backend3ds::arenaAlloc(size_t bytes, size_t align) {
  const size_t at = (arenaUsed_ + align - 1) & ~(align - 1);
  if (!arena_ || at + bytes > arenaSize_) return nullptr;
  arenaUsed_ = at + bytes;
  if (arenaUsed_ > st_.arenaPeak) st_.arenaPeak = arenaUsed_;
  return arena_ + at;
}

bool Backend3ds::reserveGeometry(size_t bytes,size_t count,uint8_t*& vb,uint16_t*& ib) {
  const size_t mark=arenaUsed_;
  vb=static_cast<uint8_t*>(arenaAlloc(bytes,16));
  ib=count<=SIZE_MAX/2?static_cast<uint16_t*>(arenaAlloc(count*2,16)):nullptr;
  if(!vb||!ib){rewindGeometry(mark);vb=nullptr;ib=nullptr;return false;}
  return true;
}
void Backend3ds::rewindGeometry(size_t mark) {
  if(mark<=arenaUsed_){arenaUsed_=mark;arenaFlushed_=std::min(arenaFlushed_,mark);}
}

bool Backend3ds::loadShader(C3dShader& s) {
  if (s.user) return true;
  if (!s.error.empty() || !s.bin.ok) return fail("shader unusable: " + s.error);
  ShaderHandle* h = new (std::nothrow) ShaderHandle();
  if (!h) return fail("out of memory (shader handle)");
  // DVLB_ParseFile keeps pointers into the buffer: s.bin.shbin stays untouched for the life of the program
  h->dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(s.bin.shbin.data()), u32(s.bin.shbin.size()));
  if (!h->dvlb || h->dvlb->numDVLE != 1) { if (h->dvlb) DVLB_Free(h->dvlb); delete h; return fail("DVLB_ParseFile rejected the SHBIN"); }
  shaderProgramInit(&h->prog);
  if (R_FAILED(shaderProgramSetVsh(&h->prog, &h->dvlb->DVLE[0]))) { DVLB_Free(h->dvlb); delete h; return fail("shaderProgramSetVsh failed"); }
  // the plan addresses uniforms by register id: libctru's name lookup must agree
  for (const auto& kv : s.uniforms) {
    const int loc = shaderInstanceGetUniformLocation(h->prog.vertexShader, kv.first.c_str());
    if (loc != kv.second.id) {
      shaderProgramFree(&h->prog); DVLB_Free(h->dvlb); delete h;
      return fail("uniform " + kv.first + ": libctru says register " + std::to_string(loc) + ", the plan says " + std::to_string(kv.second.id));
    }
  }
  s.user = reinterpret_cast<uintptr_t>(h);
  st_.shadersLoaded++;
  return true;
}

void Backend3ds::releaseShader(C3dShader& s) {
  ShaderHandle* h = reinterpret_cast<ShaderHandle*>(s.user);
  if (!h) return;
  if (program_ == h) invalidateCaches();
  C3Dx_ForgetProgram(&h->prog);   // (Claude v119) citro3d's context must not keep a freed program
  shaderProgramFree(&h->prog);
  DVLB_Free(h->dvlb);
  delete h;
  s.user = 0;
}

uint8_t* Backend3ds::allocateTextureOutput(TexEntry& e,int fmt,int w,int h,int levels,const std::vector<size_t>& offsets,size_t bytes) {
  if(e.user||levels<1||levels>16||offsets.size()!=size_t(levels)||w<8||h<8||w>1024||h>1024)return nullptr;
  auto* g=new(std::nothrow) GpuTex();if(!g)return nullptr;
  C3D_TexInitParams p{};p.width=u16(w);p.height=u16(h);p.maxLevel=u8(levels-1);p.format=GPU_TEXCOLOR(fmt);p.type=GPU_TEX_2D;p.onVram=false;
  if(!C3D_TexInitWithParams(&g->tex,nullptr,p)&&!(linearPressure&&linearPressure()&&C3D_TexInitWithParams(&g->tex,nullptr,p))){delete g;return nullptr;}
  for(int l=0;l<levels;++l){size_t end=l+1<levels?offsets[l+1]:bytes;
    if(end<offsets[l]||end-offsets[l]!=C3D_TexCalcLevelSize(g->tex.size,l)){C3D_TexDelete(&g->tex);delete g;return nullptr;}}
  g->gen=++g_texGen;e.user=reinterpret_cast<uintptr_t>(g);
  return static_cast<uint8_t*>(g->tex.data);
}
void Backend3ds::finishTextureOutput(TexEntry& e){
  auto* g=reinterpret_cast<GpuTex*>(e.user);if(!g)return;
  C3D_TexFlush(&g->tex);st_.texturesUploaded++;st_.textureBytes+=e.tex.encodedBytes;
}

bool Backend3ds::uploadTexture(TexEntry& e) {
  struct UploadClock { uint64_t t; ~UploadClock() { g_texProf[4] += texProfTicks() - t; } } uploadClock{texProfTicks()};
  const MappedTexture& m = e.tex;
  if (!m.ok || m.levels < 1 || m.levels > 16 || m.levelOffset.size() != size_t(m.levels)) return fail("texture entry is not uploadable");
  if (m.width < 8 || m.height < 8 || m.width > 1024 || m.height > 1024 || (m.width & (m.width - 1)) || (m.height & (m.height - 1))) return fail("texture size " + std::to_string(m.width) + "x" + std::to_string(m.height));
  if (m.levels - 1 > C3D_TexCalcMaxLevel(m.width, m.height)) return fail("more mip levels than the size has");
  GpuTex* g = new (std::nothrow) GpuTex();
  if (!g) return fail("out of memory (texture handle)");
  std::memset(&g->tex, 0, sizeof(g->tex));
  C3D_TexInitParams p;
  std::memset(&p, 0, sizeof(p));
  p.width = u16(m.width); p.height = u16(m.height); p.maxLevel = u8(m.levels - 1); p.format = GPU_TEXCOLOR(m.fmt); p.type = GPU_TEX_2D; p.onVram = false;
  if (!C3D_TexInitWithParams(&g->tex, nullptr, p) && !(linearPressure && linearPressure() && C3D_TexInitWithParams(&g->tex, nullptr, p))) { delete g; return fail("C3D_TexInitWithParams failed (linear memory exhausted?)"); }
  for (int l = 0; l < m.levels; l++) {
    const size_t end = l + 1 < m.levels ? m.levelOffset[size_t(l) + 1] : m.data.size();
    if (end < m.levelOffset[size_t(l)] || end - m.levelOffset[size_t(l)] != C3D_TexCalcLevelSize(g->tex.size, l)) {
      const std::string msg = "mip level " + std::to_string(l) + " has " + std::to_string(end - m.levelOffset[size_t(l)]) + " bytes, citro3d expects " + std::to_string(C3D_TexCalcLevelSize(g->tex.size, l));
      C3D_TexDelete(&g->tex); delete g;
      return fail(msg);
    }
  }
  for (int l = 0; l < m.levels; l++) C3D_TexLoadImage(&g->tex, m.data.data() + m.levelOffset[size_t(l)], GPU_TEXFACE_2D, l);
  C3D_TexFlush(&g->tex);
  g->gen = ++g_texGen;
  e.user = reinterpret_cast<uintptr_t>(g);
  st_.texturesUploaded++;
  st_.textureBytes += m.data.size();
  // the GPU copy is the only one needed from here on (an evicted or stale entry is converted again): the host copy goes, so the converted textures
  // are not held twice (host heap and linear memory; the host heap is what the game's MEM1 / ARAM leave over)
  std::vector<uint8_t>().swap(const_cast<MappedTexture&>(m).data);
  return true;
}

const void* Backend3ds::textureLevel0(const TexEntry& e, size_t& bytes, int& width, int& height) const {
  const GpuTex* g = reinterpret_cast<const GpuTex*>(e.user);
  if (!g || !g->tex.data) { bytes = 0; return nullptr; }
  width = g->tex.width; height = g->tex.height; bytes = C3D_TexCalcLevelSize(g->tex.size, 0);
  return g->tex.data;
}

void Backend3ds::release(const TexEntry& e) {
  GpuTex* g = reinterpret_cast<GpuTex*>(e.user);
  if (!g) return;
  for (int u = 0; u < 3; u++) if (boundKey_[u]) { bound_[u] = nullptr; boundKey_[u] = 0; }   // rebind whatever the next draw needs
  const_cast<TexEntry&>(e).user = 0;
  garbage_.push_back(std::make_pair(g, frameNo_));
}

bool Backend3ds::setMainView(TexEntry& view, C3D_RenderTarget* t) {
  if (!t || !t->frameBuf.colorBuf) return fail("main view: the target has no colour buffer");
  if (t->frameBuf.colorFmt != GPU_RB_RGBA8) return fail("main view: the colour buffer is not RGBA8");
  if (view.user) release(view);
  GpuTex* g = new (std::nothrow) GpuTex();
  if (!g) return fail("out of memory (main view)");
  std::memset(&g->tex, 0, sizeof(g->tex));
  unsigned w = t->frameBuf.width, h = t->frameBuf.height;
  g->tex.data = t->frameBuf.colorBuf;
  auto pow2 = [](unsigned v) { unsigned p = 8; while (p < v) p <<= 1; return p; };
  const unsigned pw = pow2(w), ph = pow2(h);
  if ((pw != w || ph != h) && !npotView() && (w % 8) == 0 && (h % 8) == 0) {
    g->stage = vramAlloc(size_t(pw) * ph * 4);
    if (g->stage) {
      g->src = t; g->tex.data = g->stage; w = pw; h = ph;
      ::n3dsLog("[view] padded %ux%u -> %ux%u\n", unsigned(t->frameBuf.width), unsigned(t->frameBuf.height), pw, ph);
      // ::g_screenTexgen (vtx.cpp): the scratch read (SCREEN texgen) lands in the used corner; row 0 is at t = 1
      ::g_screenTexgen[0] = 0.5f * float(t->frameBuf.width) / float(pw);
      ::g_screenTexgen[1] = 0.5f * float(t->frameBuf.height) / float(ph);
      ::g_screenTexgen[2] = 0.0f;
      ::g_screenTexgen[3] = 1.0f - float(t->frameBuf.height) / float(ph);   // memory row 0 is at t = 1: the used rows are t in [1 - h / ph, 1]
    }
  }
  g->tex.fmt = GPU_RGBA8;
  g->tex.size = size_t(w) * h * 4;
  g->tex.width = u16(w); g->tex.height = u16(h);
  g->tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
  g->view = true;
  g->gen = ++g_texGen;
  view.tex.ok = true; view.tex.fmt = PTF_RGBA8; view.tex.width = int(w); view.tex.height = int(h); view.tex.levels = 1;
  view.user = reinterpret_cast<uintptr_t>(g);
  return true;
}

bool Backend3ds::allocRendered(TexEntry& e) {
  const MappedTexture& m = e.tex;
  if (!m.ok || m.width < 8 || m.height < 8 || m.width > 1024 || m.height > 1024 || (m.width & (m.width - 1)) || (m.height & (m.height - 1))) return fail("rendered texture size " + std::to_string(m.width) + "x" + std::to_string(m.height));
  if (m.fmt != PTF_RGBA8 && m.fmt != PTF_RGB8 && m.fmt != PTF_RGB5A1 && m.fmt != PTF_RGB565 && m.fmt != PTF_RGBA4) return fail("rendered texture format " + std::to_string(m.fmt) + " is not renderable");
  for (size_t i = rtPool_.size(); i-- > 0;) {
    GpuTex* p = rtPool_[i];
    if (p->levels == 1 && p->tex.width == m.width && p->tex.height == m.height && p->tex.fmt == GPU_TEXCOLOR(m.fmt)) {
      rtPool_.erase(rtPool_.begin() + std::ptrdiff_t(i));
      p->gen = ++g_texGen;
      e.user = reinterpret_cast<uintptr_t>(p);
      return true;
    }
  }
  GpuTex* g = new (std::nothrow) GpuTex();
  if (!g) return fail("out of memory (rendered texture handle)");
  std::memset(&g->tex, 0, sizeof(g->tex));
  if (!C3D_TexInitVRAM(&g->tex, u16(m.width), u16(m.height), GPU_TEXCOLOR(m.fmt))) { delete g; return fail("C3D_TexInitVRAM failed (VRAM exhausted?)"); }   // render targets must be in VRAM (citro3d)
  g->rt = C3D_RenderTargetCreateFromTex(&g->tex, GPU_TEXFACE_2D, 0, -1);
  if (!g->rt) { C3D_TexDelete(&g->tex); delete g; return fail("C3D_RenderTargetCreateFromTex failed"); }
  g->gen = ++g_texGen;
  g->vramBytes = g->tex.size; st_.rtLive++; st_.rtVramBytes += g->vramBytes;
  e.user = reinterpret_cast<uintptr_t>(g);
  st_.texturesUploaded++;
  st_.textureBytes += size_t(m.width) * m.height * (m.fmt == PTF_RGBA8 ? 4 : m.fmt == PTF_RGB8 ? 3 : 2);
  return true;
}

// (Claude v106) a rendered texture with `levels` mip levels, each with its own render target (the sea's warped texture, renderer3ds.cpp warpOne).
// Render targets must be in VRAM (citro3d). Pooled like rendered copies (released ones come back through rtPool_).
bool Backend3ds::allocRenderedMips(TexEntry& e, int levels) {
  const MappedTexture& m = e.tex;
  if (levels < 1 || levels > 8 || !m.ok || (m.width >> (levels - 1)) < 8 || (m.height >> (levels - 1)) < 8 || m.width > 1024 || m.height > 1024 ||
      (m.width & (m.width - 1)) || (m.height & (m.height - 1)) || (m.fmt != PTF_RGBA8 && m.fmt != PTF_RGBA4)) return fail("mipmapped rendered texture " + std::to_string(m.width) + "x" + std::to_string(m.height) + " levels " + std::to_string(levels));
  for (size_t i = rtPool_.size(); i-- > 0;) {
    GpuTex* p = rtPool_[i];
    if (p->levels == levels && p->tex.width == m.width && p->tex.height == m.height && p->tex.fmt == GPU_TEXCOLOR(m.fmt)) {
      rtPool_.erase(rtPool_.begin() + std::ptrdiff_t(i));
      p->gen = ++g_texGen;
      e.user = reinterpret_cast<uintptr_t>(p);
      return true;
    }
  }
  GpuTex* g = new (std::nothrow) GpuTex();
  if (!g) return fail("out of memory (rendered texture handle)");
  std::memset(&g->tex, 0, sizeof(g->tex));
  C3D_TexInitParams P{};
  P.width = u16(m.width); P.height = u16(m.height); P.maxLevel = u8(levels - 1); P.format = GPU_TEXCOLOR(m.fmt); P.type = GPU_TEX_2D; P.onVram = true;
  if (!C3D_TexInitWithParams(&g->tex, nullptr, P)) { delete g; return fail("C3D_TexInitWithParams failed (mipmapped rendered texture: VRAM full?)"); }   // render targets live in VRAM
  g->levels = levels;
  g->vramBytes = g->tex.size; st_.rtLive++; st_.rtVramBytes += g->vramBytes;
  for (int l = 0; l < levels; l++) {
    g->rtLevel[l] = C3D_RenderTargetCreateFromTex(&g->tex, GPU_TEXFACE_2D, l, -1);
    if (!g->rtLevel[l]) { for (int k = 0; k < l; k++) C3D_RenderTargetDelete(g->rtLevel[k]); C3D_TexDelete(&g->tex); delete g; return fail("C3D_RenderTargetCreateFromTex failed (level)"); }
  }
  g->rt = g->rtLevel[0];
  g->gen = ++g_texGen;
  e.user = reinterpret_cast<uintptr_t>(g);
  st_.texturesUploaded++;
  st_.textureBytes += size_t(g->tex.size);
  return true;
}

bool Backend3ds::drawIntoLevel(TexEntry& e, int level, C3dPlan& L, C3D_RenderTarget* mainTarget) {
  GpuTex* g = reinterpret_cast<GpuTex*>(e.user);
  if (!g || level < 0 || level >= g->levels || !g->rtLevel[level]) return fail("drawIntoLevel: no target for level " + std::to_string(level));
  C3D_FrameDrawOn(g->rtLevel[level]);
  invalidateCaches();
  const bool ok = draw(L);
  C3D_FrameDrawOn(mainTarget);
  invalidateCaches();
  return ok;
}

bool Backend3ds::textureImage(const TexEntry& e, const void** data, uint32_t* bytes) const {
  if (!e.user) return false;
  const GpuTex* g = reinterpret_cast<const GpuTex*>(e.user);
  if (g->view || !g->tex.data) return false;
  *data = g->tex.data;
  *bytes = uint32_t(g->tex.size);
  return true;
}

// (Claude v100) brings every padded view the plan samples up to date: a GX TextureCopy of the source colour buffer (tiled 8x8, one tile
// row = width*8*4 bytes) into the staging texture's wider tile rows. Queued in order with the frame's command lists (split first).
void Backend3ds::refreshViews(C3dPlan& L) {
  for (int u = 0; u < 3; u++) {
    if (!L.units[u].enabled || !L.units[u].entry || !L.units[u].entry->user) continue;
    GpuTex* g = reinterpret_cast<GpuTex*>(L.units[u].entry->user);
    if (!g->view || !g->src || !g->stage) continue;
    const C3D_FrameBuf& fb = g->src->frameBuf;
    const u32 line = u32(fb.width) * 8 * 4, outLine = u32(g->tex.width) * 8 * 4;
    flushArena();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);   // also flushes the target's render cache (citro3d adds FRAMEBUFFER_FLUSH)
    GX_TextureCopy((u32*)fb.colorBuf, GX_BUFFER_DIM(line >> 4, 0), (u32*)g->stage, GX_BUFFER_DIM(line >> 4, (outLine - line) >> 4),
                   line * (u32(fb.height) / 8), GX_TRANSFER_RAW_COPY(1));   // raw copy (bit 3): without it the PPF runs a tiling display transfer
    if (++st_.viewRepacks == 1) ::n3dsLog("[view] first repack (%lu bytes)\n", (unsigned long)(line * (u32(fb.height) / 8)));
  }
}

// (Claude v101) draw-ID frame (console-only texture garbage hunt): texenv stage 5 replaces RGB with the constant (id & 255, id >> 8, 0xA5) and keeps
// the computed alpha (alpha test cut-outs stay), blending writes the colour unchanged, fog off. One log line per draw names its textures.
int g_drawIdMode = 0;
static unsigned g_drawIdNo = 0;
void Backend3ds::beginDrawIdFrame(bool on) { g_drawIdMode = on ? 1 : 0; g_drawIdNo = 0; texenvValid_ = effectValid_ = false; }
void Backend3ds::drawIdOverride(C3dPlan& L) {
  const unsigned id = ++g_drawIdNo;
  C3D_TexEnv* e = C3D_GetTexEnv(5);
  if (L.texenv.used < 6) C3D_TexEnvInit(e);
  C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
  C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
  C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
  C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_1);
  C3D_TexEnvColor(e, 0xFF000000u | (0xA5u << 16) | (((id >> 8) & 0xFF) << 8) | (id & 0xFF));
  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
  C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
  C3Di_UpdateContext();
  texenvValid_ = effectValid_ = false;
  char line[512]; int n = std::snprintf(line, sizeof line, "[drawid] %u idx %u tev %d", id, unsigned(L.indexCount), L.texenv.used);
  for (int u = 0; u < 3 && n < int(sizeof line) - 100; u++) {
    if (!L.units[u].enabled || !L.units[u].entry) continue;
    const TexEntry& t = *L.units[u].entry; const GpuTex* g = reinterpret_cast<const GpuTex*>(t.user); const C3dUnit& U = L.units[u];
    n += std::snprintf(line + n, sizeof line - size_t(n), " | u%d guest %08x fmt %d %dx%d lv %d data %p%s%s wrap %d/%d filt %d/%d/%d lod %d-%d bias %u", u, unsigned(t.addr), int(t.tex.fmt),
                       t.tex.width, t.tex.height, t.tex.levels, g ? g->tex.data : nullptr, t.rendered ? " rendered" : "", (g && g->view) ? " view" : "",
                       int(U.wrapS), int(U.wrapT), int(U.magFilter), int(U.minFilter), int(U.mipFilter), int(U.minLevel), int(U.maxLevel), unsigned(U.lodBias));
  }
  ::n3dsLog("[drawid-state] %u shader %016llx clear %d blit %d depth %d/%u mask %u alpha %d/%u/%u scissor %d,%d,%d,%d stride %u vertices %u\n",
      id,(unsigned long long)(L.shader?L.shader->key:0),
      int(L.shader&&L.shader->source.find("copy-clear:")!=std::string::npos),
      int(L.shader&&L.shader->source.find("EFB copy blit:")!=std::string::npos),
      int(L.fx.depthTest),unsigned(L.fx.depthFunc),unsigned(L.fx.writeMask),int(L.fx.alphaTest),unsigned(L.fx.alphaFunc),unsigned(L.fx.alphaRef),
      L.scissor.x0,L.scissor.y0,L.scissor.x1,L.scissor.y1,L.attrs.stride,L.vertexBytes);
  ::n3dsLog("%s\n", line);
}

bool Backend3ds::copy(EfbCopyPlan& P, TextureCache& cache, C3D_RenderTarget* mainTarget) {
  if (!P.ok) { st_.refused++; return fail("copy plan refused: " + P.why); }
  bool done = true;
  if (P.toTexture) {
    TexEntry* e = cache.addRendered(P.tex);
    if (!e || !allocRendered(*e)) {
      // (Claude, shadow fix) no memory for the copy's texture: the copy is lost, but its clear must still run. Returning here left the
      // real-shadow casters' silhouettes (and their depth) in the frame, so the scene could not draw over them: grey character
      // silhouettes on console, where linear memory is tight.
      cache.invalidateRange(P.tex.addr, P.tex.guestBytes);
      err_ = "copy: no memory for the rendered texture";
      st_.copyAllocFails++;
      done = false;
    } else {
    GpuTex* g = reinterpret_cast<GpuTex*>(e->user);
    refreshViews(P.blit);
    C3D_FrameDrawOn(g->rt);
    invalidateCaches();
    const bool blitted = draw(P.blit);
    C3D_FrameDrawOn(mainTarget);
    invalidateCaches();
    if (!blitted) {
      const std::string why = err_;
      cache.invalidateRange(P.tex.addr, P.tex.guestBytes);
      err_ = why;
      done = false;
    } else {
      st_.copies++;
    }
    }
  }
  if (P.clears) {
    if (!P.toTexture) { C3D_FrameDrawOn(mainTarget); invalidateCaches(); }
    if (!draw(P.clear)) done = false;
    else st_.clears++;
  }
  return done;
}

bool Backend3ds::drawTwoPass(C3dMultiPlan& M, C3D_RenderTarget* scratch, C3D_RenderTarget* mainTarget) {
  if (!M.ok) { st_.refused++; return fail("two-pass plan refused: " + M.why); }
  if (!scratch || !mainTarget) { st_.refused++; return fail("two-pass draw: no scratch or main target"); }
  C3D_FrameDrawOn(scratch);        // citro3d flushes the buffer being left and resets the viewport / scissor
  invalidateCaches();
  bool ok = true;
  if (M.hasClear) ok = draw(M.clear);
  if (ok) ok = draw(M.first);
  C3D_FrameDrawOn(mainTarget);     // the scratch's colour buffer is now readable as a texture
  invalidateCaches();
  if (!ok) return false;
  refreshViews(M.second);           // the second pass would read garbage: the main target keeps its picture
  if (!draw(M.second)) return false;
  st_.twoPass++;
  return true;
}

bool Backend3ds::draw(C3dPlan& L) {
  if (g_hwTest) {
    const int no = g_hwDrawNo++;
    (void)no;
    if ((g_hwTest == 5 || g_hwTest == 4) && L.ok && L.units[0].enabled) return true;   // 4: only the padded untextured shaders draw
  }
  if (!L.ok) { st_.refused++; return fail("plan refused: " + L.why); }
  if (!loadShader(*L.shader)) { st_.refused++; return false; }
  const float use = cmdUsage();
  if (use >= 0.97f) { st_.noCmdBuf++; return fail("command buffer full"); }
  // hand the finished part to the GPU; its vertex data must be in RAM first (C3D_FrameEnd only flushes at the end). (Claude v120) every
  // kickDraws_ draws the GPU also starts on it at once (C3Dx_FrameKick) instead of at C3D_FrameEnd: it renders while the CPU builds the rest
  if (use >= splitAt_ || (kickDraws_ && ++sinceKick_ >= kickDraws_)) {
    flushArena();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    if (kickDraws_ && C3Dx_FrameKick()) st_.kicks++;
    splitAt_ = use + splitStep_;
    sinceKick_ = 0;
    st_.splits++;
  }
  for (int u = 0; u < 3; u++)
    if (L.units[u].enabled && !L.units[u].entry->user && !uploadTexture(*L.units[u].entry)) { st_.refused++; return false; }

  // vertex and index data go to linear memory (the GPU reads them there)
  const uint64_t pt0 = t2pProfTicks();
  auto inArena=[this](const void* p,size_t bytes) {
    const uintptr_t at=reinterpret_cast<uintptr_t>(p),base=reinterpret_cast<uintptr_t>(arena_);
    return arena_ && at>=base && at-base<=arenaUsed_ && bytes<=arenaUsed_-(at-base);
  };
  const bool direct=L.linearGeometry||(inArena(L.vertices,L.vertexBytes)&&inArena(L.indices,size_t(L.indexCount)*2));
  u8* vb=direct?const_cast<u8*>(L.vertices):static_cast<u8*>(arenaAlloc(L.vertexBytes,16));
  u16* ib=direct?const_cast<u16*>(L.indices):static_cast<u16*>(arenaAlloc(size_t(L.indexCount)*2,16));
  if (!vb || !ib) { st_.noArena++; return fail("draw arena exhausted"); }
  if(!direct){std::memcpy(vb,L.vertices,L.vertexBytes);std::memcpy(ib,L.indices,size_t(L.indexCount)*2);}
  if (t2pProfEnabled()) g_t2pProf[15] += t2pProfTicks() - pt0;

  uint64_t rq = g_beProfOn ? svcGetSystemTick() : 0;   // (Claude v119)
  auto rmark = [&rq](int k) { if (g_beProfOn) { const uint64_t t = svcGetSystemTick(); g_replayProf[k] += t - rq; rq = t; } };
  uint64_t bq = t2pProfTicks();   // (Claude v77 profiling, render-side slots 0..6; only with the replay's costs=1)
  auto bmark = [&bq](int slot) { if (t2pProfEnabled()) { const uint64_t t = t2pProfTicks(); g_t2pProf[slot] += t - bq; bq = t; } };
  // shader
  ShaderHandle* h = reinterpret_cast<ShaderHandle*>(L.shader->user);
  if (h != program_) {
    C3D_BindProgram(&h->prog);
    program_ = h;
    // Binding uploads only this program's float24 constants; the other GPU
    // uniform registers retain their values. Keep their exact shadow entries
    // across material shaders instead of re-uploading every matrix/light row.
    const shaderInstance_s* vs=h->prog.vertexShader;
    for (unsigned i=0;i<vs->numFloat24Uniforms;++i) {
      const unsigned id=vs->float24Uniforms[i].id;
      if (id<96) uniformValid_[id]=false;
    }
  }

  // attributes
  if (!attrValid_ || attrHash_ != L.hashAttrs) {
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    for (int i = 0; i < L.attrs.count; i++)
      if (AttrInfo_AddLoader(ai, L.attrs.loader[i].reg, GPU_FORMATS(L.attrs.loader[i].format), L.attrs.loader[i].count) < 0) return fail("AttrInfo_AddLoader failed");
    attrHash_ = L.hashAttrs; attrValid_ = true;
  } else {
    st_.stateSkips++;
  }
  C3D_BufInfo* bi = C3D_GetBufInfo();
  BufInfo_Init(bi);
  if (L.attrs.planar) {   // (Claude v77) one buffer per attribute, attribute i read from buffer i
    for (int i = 0; i < L.attrs.count; i++)
      if (BufInfo_Add(bi, vb + L.attrs.offset[i], L.attrs.size[i], 1, uint64_t(i)) < 0) return fail("BufInfo_Add failed (planar attribute)");
  } else
  if (BufInfo_Add(bi, vb, L.attrs.stride, L.attrs.count, L.attrs.permutation) < 0) return fail("BufInfo_Add failed (vertex data below the linear heap base?)");

  bmark(0); rmark(6);   // shader + attributes + buffers
  // uniforms
  for (const C3dUniform& u : L.uniforms) {
    for (int r = 0; r < u.count; r++) {
      const int id = u.id + r;
      const float* v = u.rows[r].v;
      if (uniformValid_[id] && std::memcmp(uniformShadow_[id], v, 16) == 0) continue;
      C3D_FVUnifSet(GPU_VERTEX_SHADER, id, v[0], v[1], v[2], v[3]);
      std::memcpy(uniformShadow_[id], v, 16);
      uniformValid_[id] = true;
    }
  }

  bmark(1); rmark(7);   // uniforms
  // texenv
  if (!texenvValid_ || texenvHash_ != L.hashTexEnv) {
    for (int i = 0; i < 6; i++) std::memcpy(C3D_GetTexEnv(i), &L.texenv.stage[i], sizeof(HwStage));
    C3D_TexEnvBufUpdate(C3D_RGB, int(L.texenv.updateBuffer >> 8) & 0xF);
    C3D_TexEnvBufUpdate(C3D_Alpha, int(L.texenv.updateBuffer >> 12) & 0xF);
    C3D_TexEnvBufColor(L.texenv.bufferColor);
    texenvHash_ = L.hashTexEnv; texenvValid_ = true;
  } else {
    st_.stateSkips++;
  }
  bmark(2);
  const bool hwTex = g_hwTest && L.units[0].enabled;
  (void)hwTex;

  // texture units. Every enabled unit binds its own shallow copy of the cached C3D_Tex: citro3d reads param / lodParam when the draw is issued, and two units
  // may show the same image with different samplers.
  for (int u = 0; u < 3; u++) {
    const C3dUnit& U = L.units[u];
    if (!U.enabled) {
      if (boundKey_[u] != 1) {
        if (u == 0) C3D_TexBind(0, nullptr);
        else C3D_TexBind(u, &dummy_);
        boundKey_[u] = 1; bound_[u] = nullptr;
      }
      continue;
    }
    GpuTex* g = reinterpret_cast<GpuTex*>(U.entry->user);
    const TextureBindingKey key=textureBindingKey(g->gen,U);
    if (textureBindingMemo_ && boundKey_[u]==2 && boundSampler_[u]==key) { st_.stateSkips++; continue; }
    slot_[u] = g->tex;
    C3D_Tex* t = &slot_[u];
    C3D_TexSetFilter(t, GPU_TEXTURE_FILTER_PARAM(U.magFilter), GPU_TEXTURE_FILTER_PARAM(U.minFilter));
    C3D_TexSetFilterMipmap(t, GPU_TEXTURE_FILTER_PARAM(U.mipFilter));
    C3D_TexSetWrap(t, GPU_TEXTURE_WRAP_PARAM(U.wrapS), GPU_TEXTURE_WRAP_PARAM(U.wrapT));
    t->param = (t->param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(U.projective ? GPU_TEX_PROJECTION : GPU_TEX_2D);
    t->lodBias = U.lodBias;
    t->minLevel = U.minLevel;
    t->maxLevel = U.maxLevel;      // the register field: the allocation (g->tex) keeps its own level count
    C3D_TexBind(u, t);
    boundKey_[u] = 2; boundSampler_[u]=key; bound_[u] = t;
  }

  bmark(3); rmark(8);   // texenv + texture units
  // fixed-function state
  if (!effectValid_ || effectHash_ != L.hashEffect) {
    const C3dEffect& E = L.fx;
    C3D_DepthMap(E.zBuffer, E.zScale, E.zOffset);
    C3D_CullFace(GPU_CULLMODE(E.cull));
    C3D_DepthTest(E.depthTest, GPU_TESTFUNC(E.depthFunc), GPU_WRITEMASK(E.writeMask));
    C3D_AlphaTest(E.alphaTest, GPU_TESTFUNC(E.alphaFunc), E.alphaRef);
    if (E.logicOp) C3D_ColorLogicOp(GPU_LOGICOP(E.logic));
    else C3D_AlphaBlend(GPU_BLENDEQUATION(E.eqRgb), GPU_BLENDEQUATION(E.eqAlpha), GPU_BLENDFACTOR(E.srcRgb), GPU_BLENDFACTOR(E.dstRgb), GPU_BLENDFACTOR(E.srcAlpha), GPU_BLENDFACTOR(E.dstAlpha));
    C3D_BlendingColor(E.blendColor);
    if (E.fog) {
      std::memcpy(fogLut_.data, L.fogLut, sizeof(fogLut_.data));
      C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
      C3D_FogColor(E.fogColor);
      C3D_FogLutBind(&fogLut_);        // citro3d uploads the table when the next draw is issued
    } else {
      C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    }
    effectHash_ = L.hashEffect; effectValid_ = true;
  } else {
    st_.stateSkips++;
  }

  if (!scissorValid_ || scissor_.x0 != L.scissor.x0 || scissor_.y0 != L.scissor.y0 || scissor_.x1 != L.scissor.x1 || scissor_.y1 != L.scissor.y1) {
    C3D_SetScissor(GPU_SCISSOR_NORMAL, u32(L.scissor.x0), u32(L.scissor.y0), u32(L.scissor.x1), u32(L.scissor.y1));
    scissor_ = L.scissor; scissorValid_ = true;
  }

  // (Claude 2026-11-11) Only the texture units this draw samples are switched on. citro3d enables every unit that has a texture bound, and
  // units 1 and 2 always have one (the placeholder: C3D_TexBind cannot unbind them). The emulator ignores an enabled unit nobody reads; the
  // console's GPU never finished the first textured draw (units 1 and 2 on, the shader feeding coordinates to unit 0 only): the frame's
  // command list stayed in flight for ever. citro3d writes its own enable bits in C3Di_UpdateContext, so that runs first (the draw call's
  // own update then has nothing left to write) and byte 0 of TEXUNIT_CONFIG is overwritten with the real set.
  {
    u32 units = 0;
    for (int u = 0; u < 3; u++) if (L.units[u].enabled) units |= 1u << u;
    bmark(4); rmark(9);   // effect + scissor
    C3Di_UpdateContext();
    bmark(5); rmark(10);   // C3Di_UpdateContext
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x1, units);
  }
  if (g_drawIdMode) drawIdOverride(L);   // (Claude v101) DIAGNOSTIC frame: every draw writes its number as a flat colour
  // (Claude v77) the context was just updated above and nothing citro3d tracks changed since (n3ds/citro3d drawElements.c): same commands
  C3Di_DrawElementsUpdated(GPU_TRIANGLES, int(L.indexCount), C3D_UNSIGNED_SHORT, ib);
  bmark(6); rmark(11);   // draw call
  st_.draws++;
  lastHashes_[0] = L.hashAttrs; lastHashes_[1] = L.hashTexEnv; lastHashes_[2] = L.hashEffect; lastIndices_ = ib; lastIndexCount_ = L.indexCount;
  return true;
}

bool Backend3ds::loadPreparedDraw(const char* path,bool verify) {
 preparedDraw_.reset();preparedVerify_=verify;preparedBuilds_=preparedSubmits_=preparedMismatches_=0;
 FILE* f=std::fopen(path,"rb");if(!f)return false;
 if(std::fseek(f,0,SEEK_END)){std::fclose(f);return false;}long n=std::ftell(f);
 if(n<0||n>128*1024||std::fseek(f,0,SEEK_SET)){std::fclose(f);return false;}
 try{std::vector<uint8_t> bytes(size_t(n),0);const bool read=std::fread(bytes.data(),1,bytes.size(),f)==bytes.size();std::fclose(f);f=nullptr;
  auto p=std::make_unique<PreparedCommands>();if(!read||!PreparedCommands::load(bytes.data(),bytes.size(),preparedDrawFingerprint,*p)||!compatiblePreparedDraw(*p))return false;
  preparedDraw_=std::move(p);return true;
 }catch(...){if(f)std::fclose(f);return false;}
}

bool Backend3ds::recordDraw(const C3dPlan& L, RecordedDraw& R) {
  R.ok = false;R.prepared=false;
  if (g_hwTest || g_drawIdMode || !L.ok || !L.linearGeometry || !L.shader || !L.shader->user) return false;
  // the context must hold exactly this plan's state: draw() issued it last and nothing citro3d tracks changed since
  if (lastIndices_ != L.indices || lastIndexCount_ != L.indexCount || lastHashes_[0] != L.hashAttrs || lastHashes_[1] != L.hashTexEnv ||
      lastHashes_[2] != L.hashEffect || reinterpret_cast<const ShaderHandle*>(L.shader->user) != program_ || !C3Dx_Clean()) return false;
  if (!scissorValid_ || scissor_.x0 != L.scissor.x0 || scissor_.y0 != L.scissor.y0 || scissor_.x1 != L.scissor.x1 || scissor_.y1 != L.scissor.y1) return false;
  std::vector<uint8_t> shared(C3Dx_SharedSize());
  R.state.resize(C3Dx_OwnSize());
  C3Dx_Capture(shared.data(), R.state.data());
  u32 units = 0;
  for (int u = 0; u < 3; u++) if (L.units[u].enabled) units |= 1u << u;
  static u32 scratch[2048];
  auto emit = [&](std::vector<uint32_t>& w, uint16_t* at, int slot, int g) {
    u32 n=0;
    if(g==7 && preparedDraw_){u32 offset=0;
      const uint32_t inputs[3]={units,0,L.indexCount};uint32_t values[3];std::memcpy(values,inputs,sizeof values);
      if(C3Dx_IndexOffset(R.state.data(),L.indices,&offset)){values[1]=offset;
        if(preparedDraw_->bindInto(values,3,scratch,2048)){
          n=u32(preparedDraw_->words.size());R.prepared=true;
          if(preparedVerify_){u32 baseline[2048];const u32 count=C3Dx_Emit(shared.data(),R.state.data(),g,baseline,2048,L.fx.fog?L.fogLut:nullptr,units,int(L.indexCount),L.indices);
            if(count!=n||std::memcmp(baseline,scratch,n*4)){++preparedMismatches_;R.prepared=false;n=0;}}
          if(R.prepared)++preparedBuilds_;
        }
      }
    }
    if(!n)n=C3Dx_Emit(shared.data(), R.state.data(), g, scratch, 2048, L.fx.fog ? L.fogLut : nullptr, units, int(L.indexCount), L.indices);
    if ((n & 1) || (g == 7 && n == 0) || w.size() + n > 65535) return false;
    at[slot] = uint16_t(w.size()); w.insert(w.end(), scratch, scratch + n); at[slot + 1] = uint16_t(w.size());
    return true;
  };
  R.words.clear();
  { const int own[4] = {1, 5, 6, 7}; for (int i = 0; i < 4; i++) if (!emit(R.words, R.at, i, own[i])) return false; }
  R.words.shrink_to_fit();
  // the material: one copy per (attributes, TEV, effect) hash triple; the hashes are exact state identities (C3dPlan, draw() skips on them)
  uint64_t mk = L.hashAttrs * 0x9E3779B97F4A7C15ull ^ (L.hashTexEnv + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full ^ L.hashEffect;
  for (;;) {
    auto it = mats_.find(mk);
    if (it == mats_.end()) {
      auto m = std::make_shared<RecordedMaterial>();
      m->hashAttrs = L.hashAttrs; m->hashTexEnv = L.hashTexEnv; m->hashEffect = L.hashEffect;
      const int sh[4] = {0, 2, 3, 4};
      for (int i = 0; i < 4; i++) if (!emit(m->words, m->at, i, sh[i])) return false;
      m->words.shrink_to_fit(); m->state = std::move(shared);
      matBytes_ += m->words.size() * 4 + m->state.size() + 64;
      mats_.emplace(mk, m); R.mat = m;
      break;
    }
    const RecordedMaterial& m = *it->second;
    if (m.hashAttrs == L.hashAttrs && m.hashTexEnv == L.hashTexEnv && m.hashEffect == L.hashEffect) { R.mat = it->second; break; }
    mk = mk * 0x100000001B3ull + 1;   // a different triple with the same key: the next slot
  }
  if (mats_.size() > 4096) {   // drop materials no recorded draw holds any more
    for (auto it = mats_.begin(); it != mats_.end();)
      if (it->second.use_count() == 1) { matBytes_ -= it->second->words.size() * 4 + it->second->state.size() + 64; it = mats_.erase(it); } else ++it;
  }
  R.program = reinterpret_cast<const void*>(L.shader->user);
  for (int u = 0; u < 3; u++) {
    const C3dUnit& U = L.units[u];
    if (!U.enabled) { R.unitKey[u] = 1; continue; }
    if (!U.entry || !U.entry->user) return false;
    const GpuTex* g = reinterpret_cast<const GpuTex*>(U.entry->user);
    R.unitKey[u] = 2; R.sampler[u] = textureBindingKey(g->gen, U); R.gen[u] = g->gen;
  }
  R.scissor = L.scissor;
  R.layout.reset();
  if(commandBlockEnable_){
    auto layout=std::make_shared<UniformLayout>();
    for(size_t i=0;i<L.uniforms.size()&&i<256;++i){const auto& u=L.uniforms[i];for(int r=0;r<u.count&&r<256;++r){int id=u.id+r;if(id>=0&&id<96)layout->rows.push_back({uint8_t(id),uint8_t(i),uint8_t(r)});}}
    std::sort(layout->rows.begin(),layout->rows.end(),[](auto a,auto b){return a.id<b.id;});
    bool unique=layout->rows.size()<=96;for(size_t i=1;i<layout->rows.size();++i)if(layout->rows[i-1].id==layout->rows[i].id)unique=false;
    if(unique){
      for(const auto& existing:uniformLayouts_)if(existing->rows.size()==layout->rows.size()&&(!layout->rows.size()||!std::memcmp(existing->rows.data(),layout->rows.data(),layout->rows.size()*sizeof(UniformLayout::Row)))){R.layout=existing;break;}
      if(!R.layout){if(uniformLayouts_.size()>=128)uniformLayouts_.clear();uniformLayouts_.push_back(layout);R.layout=std::move(layout);}
    }
  }
  R.serial=nextRecordedSerial_++;
  R.ok = true;
  return true;
}

bool Backend3ds::drawRecorded(C3dPlan& L, RecordedDraw& R) {
  auto viaDraw = [&] { const bool d = draw(L); if (d) recordDraw(L, R); return d; };   // the plain path, then a fresh recording of it
  if (g_hwTest || g_drawIdMode || !L.ok) return draw(L);
  if (!R.ok) return viaDraw();
  if (!loadShader(*L.shader)) { st_.refused++; return false; }
  ShaderHandle* h = reinterpret_cast<ShaderHandle*>(L.shader->user);
  if (h != R.program) return viaDraw();
  for (int u = 0; u < 3; u++) {
    if (!L.units[u].enabled) continue;
    if (!L.units[u].entry->user || reinterpret_cast<const GpuTex*>(L.units[u].entry->user)->gen != R.gen[u]) return viaDraw();   // upload / new texture
  }
  const float use = cmdUsage();
  if (use >= 0.97f) { st_.noCmdBuf++; return fail("command buffer full"); }
  if (use >= splitAt_ || (kickDraws_ && ++sinceKick_ >= kickDraws_)) {   // (Claude v120) as in draw()
    flushArena();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    if (kickDraws_ && C3Dx_FrameKick()) st_.kicks++;
    splitAt_ = use + splitStep_;
    sinceKick_ = 0;
    st_.splits++;
  }
  uint64_t rq = g_beProfOn ? svcGetSystemTick() : 0;
  auto rmark = [&rq](int k) { if (g_beProfOn) { const uint64_t t = svcGetSystemTick(); g_replayProf[k] += t - rq; rq = t; } };
  if (!C3Dx_Clean()) C3Di_UpdateContext();   // state of citro3d's own still to send (a new target, a frame begin)
  if (h != program_) {
    C3D_BindProgram(&h->prog);
    program_ = h;
    const shaderInstance_s* vs = h->prog.vertexShader;
    for (unsigned i = 0; i < vs->numFloat24Uniforms; ++i) { const unsigned id = vs->float24Uniforms[i].id; if (id < 96) uniformValid_[id] = false; }
    C3Di_UpdateContext();   // the program (and the attribute state the context holds, which is what the GPU has)
  }
  rmark(6);
  {   // the rows that differ from what the GPU has, in id order (a shader's uniforms do not overlap), straight to the command buffer
    u8 ids[96]; float rows[96][4]; int n = 0; bool sorted = true;
    for (const C3dUniform& u : L.uniforms) {
      for (int r = 0; r < u.count; r++) {
        const int id = u.id + r;
        if (id < 0 || id >= 96) continue;
        const float* v = u.rows[r].v;
        if (uniformValid_[id] && std::memcmp(uniformShadow_[id], v, 16) == 0) continue;
        std::memcpy(uniformShadow_[id], v, 16);
        uniformValid_[id] = true;
        if (n && ids[n - 1] >= id) sorted = false;
        ids[n] = u8(id); std::memcpy(rows[n], v, 16); n++;
      }
    }
    if (!sorted) {   // insertion sort by id (rare: plan uniforms out of register order)
      for (int i = 1; i < n; i++) for (int j = i; j > 0 && ids[j - 1] > ids[j]; j--) { std::swap(ids[j - 1], ids[j]); float t[4]; std::memcpy(t, rows[j], 16); std::memcpy(rows[j], rows[j - 1], 16); std::memcpy(rows[j - 1], t, 16); }
    }
    if (n) C3Dx_Uniforms(ids, rows, n);
  }
  rmark(7);
  C3D_Tex* const slots[3] = {&slot_[0], &slot_[1], &slot_[2]};
  const RecordedMaterial& M = *R.mat;
  const void* SS = M.state.data(); const void* OS = R.state.data();
  bool ok = true;
  auto putM = [&](int i, int g) { ok = ok && C3Dx_Append(&M.words[M.at[i]], u32(M.at[i + 1] - M.at[i])); C3Dx_Apply(SS, OS, g, slots); };
  auto putO = [&](int i, int g) { ok = ok && C3Dx_Append(&R.words[R.at[i]], u32(R.at[i + 1] - R.at[i])); C3Dx_Apply(SS, OS, g, slots); };
  if (!attrValid_ || attrHash_ != M.hashAttrs) { putM(0, 0); attrHash_ = M.hashAttrs; attrValid_ = true; } else st_.stateSkips++;
  putO(0, 1);
  bool tevBuf = false;
  if (!effectValid_ || effectHash_ != M.hashEffect) {
    if (L.fx.fog) std::memcpy(fogLut_.data, L.fogLut, sizeof(fogLut_.data));
    putM(1, 2); effectHash_ = M.hashEffect; effectValid_ = true; tevBuf = true;
  } else st_.stateSkips++;
  if (!texenvValid_ || texenvHash_ != M.hashTexEnv) { putM(2, 3); texenvHash_ = M.hashTexEnv; texenvValid_ = true; tevBuf = true; } else st_.stateSkips++;
  if (tevBuf) putM(3, 4);
  bool texSame = textureBindingMemo_;
  for (int u = 0; u < 3 && texSame; u++)
    texSame = R.unitKey[u] == 1 ? boundKey_[u] == 1 : (boundKey_[u] == 2 && boundSampler_[u] == R.sampler[u]);
  if (!texSame) {
    putO(1, 5);
    for (int u = 0; u < 3; u++) { boundKey_[u] = R.unitKey[u]; boundSampler_[u] = R.sampler[u]; bound_[u] = R.unitKey[u] == 2 ? &slot_[u] : nullptr; }
  } else st_.stateSkips++;
  rmark(8);
  if (!scissorValid_ || scissor_.x0 != R.scissor.x0 || scissor_.y0 != R.scissor.y0 || scissor_.x1 != R.scissor.x1 || scissor_.y1 != R.scissor.y1) {
    putO(2, 6); scissor_ = R.scissor; scissorValid_ = true;
  }
  rmark(9);
  putO(3, 7);if(R.prepared)++preparedSubmits_;
  rmark(11);
  if (!ok) { invalidateCaches(); return fail("command buffer full (recorded draw)"); }   // the trackers claim groups that did not fit: all re-sent next time
  st_.draws++; recordedDraws_++;
  lastIndices_ = nullptr;   // not a draw() plan: recordDraw refuses until the next draw()
  return true;
}


bool Backend3ds::makeUniformCommands(const UniformRowWords* rows,size_t n,std::vector<uint32_t>& out) {
  if(n>96||(!rows&&n))return false;
  UniformRowWords sorted[96];
  if(n)std::memcpy(sorted,rows,n*sizeof(UniformRowWords));
  for(size_t i=0;i<n;++i){
    if(sorted[i].reg>=96)return false;
    for(size_t j=i;j>0&&sorted[j-1].reg>sorted[j].reg;--j)std::swap(sorted[j-1],sorted[j]);
  }
  for(size_t i=1;i<n;++i)if(sorted[i-1].reg==sorted[i].reg)return false;
  // <=96 rows and <=96 runs: <=768 command words, including padding. Allocate
  // before switching the live pointer so an allocation failure cannot corrupt it.
  std::vector<uint32_t> fresh(1024);
  u32* saved=nullptr;u32 size=0,offset=0;GPUCMD_GetBuffer(&saved,&size,&offset);
  struct Restore {u32* p;u32 size,offset;~Restore(){GPUCMD_SetBuffer(p,size,offset);}} restore{saved,size,offset};
  GPUCMD_SetBuffer(fresh.data(),u32(fresh.size()),0);
  size_t i=0;
  while(i<n){
    size_t j=i+1;while(j<n&&sorted[j].reg==sorted[j-1].reg+1)++j;
    u32 data[384];
    for(size_t k=i;k<j;++k){
      // C3D_FVec's memory order is the GPU order. Use the same named members as
      // C3Dx_Uniforms, preserving every source bit (including NaNs and signed0).
      C3D_FVec v;std::memcpy(&v.x,&sorted[k].xyzw[0],4);std::memcpy(&v.y,&sorted[k].xyzw[1],4);
      std::memcpy(&v.z,&sorted[k].xyzw[2],4);std::memcpy(&v.w,&sorted[k].xyzw[3],4);
      std::memcpy(data+(k-i)*4,&v,16);
    }
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG,0x80000000|sorted[i].reg);
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA,data,u32((j-i)*4));i=j;
  }
  u32 used=0;GPUCMD_GetBuffer(nullptr,nullptr,&used);
  if(used>fresh.size()||(used&1))return false;
  GPUCMD_SetBuffer(saved,size,offset); // restore before moving/freeing the temporary
  fresh.resize(used);out=std::move(fresh);return true;
}

}  // namespace t2p

namespace t2p {
extern "C" void C3Dx_UniformsCommit(const u8*,const float (*)[4],int);
int Backend3ds::drawRecordedBlock(const RecordedItem* items,size_t count){
  if(!items||!items[0].record||count<2||count>8||g_hwTest||g_drawIdMode)return -1;
  if(kickDraws_&&sinceKick_+int(count)>=kickDraws_)return -1;
  auto& first=*items[0].record;
  if(!first.ok||!first.serial||!first.mat||!first.layout||program_!=first.program||!C3Dx_Clean())return -1;
  // A block never changes shader/material/texture/scissor. Uniforms, vertex
  // buffers and index draws remain ordered; no EFB/target event enters a block.
  u8 ids[96];const size_t nrows=first.layout->rows.size();for(size_t i=0;i<nrows;++i)ids[i]=first.layout->rows[i].id;
  const float* rowPointers[8][96];uint32_t masks[7][3]={};
  for(size_t k=0;k<count;++k){
    if(!items[k].plan||!items[k].record)return -1;
    const auto& L=*items[k].plan;const auto& R=*items[k].record;
    if(!L.ok||!L.shader||reinterpret_cast<const void*>(L.shader->user)!=first.program||!R.ok||!R.serial||R.program!=first.program||R.mat!=first.mat||R.layout!=first.layout||
       R.scissor.x0!=first.scissor.x0||R.scissor.y0!=first.scissor.y0||R.scissor.x1!=first.scissor.x1||R.scissor.y1!=first.scissor.y1)return -1;
    for(int u=0;u<3;++u){
      if(R.unitKey[u]!=first.unitKey[u]||(R.unitKey[u]==2&&!(R.sampler[u]==first.sampler[u])))return -1;
      if(L.units[u].enabled&&(!L.units[u].entry||!L.units[u].entry->user||reinterpret_cast<const GpuTex*>(L.units[u].entry->user)->gen!=R.gen[u]))return -1;
    }
    for(size_t i=0;i<nrows;++i){const auto row=first.layout->rows[i];rowPointers[k][i]=L.uniforms[row.uniform].rows[row.row].v;}
  }
  for(size_t k=1;k<count;++k)for(size_t i=0;i<nrows;++i){const unsigned id=ids[i];if(std::memcmp(rowPointers[k-1][i],rowPointers[k][i],16))masks[k-1][id/32]|=1u<<(id%32);}
  CommandBlock* block=nullptr;
  for(auto& b:commandBlocksCache_){if(b.serials.size()!=count||std::memcmp(b.masks,masks,sizeof masks))continue;bool same=true;for(size_t k=0;k<count;++k)if(b.serials[k]!=items[k].record->serial){same=false;break;}if(same){block=&b;break;}}
  if(!block){
    try {
    CommandBlock b;std::memcpy(b.masks,masks,sizeof masks);for(size_t k=0;k<count;++k)b.serials.push_back(items[k].record->serial);
    // Compile exactly the uniform changes between adjacent draws. The mask is
    // revalidated from current float bits before every cache reuse.
    for(size_t k=1;k<count;++k){
      UniformRowWords tags[96];size_t changed=0;
      for(size_t i=0;i<nrows;++i){const unsigned id=ids[i];if(!(masks[k-1][id/32]&(1u<<(id%32))))continue;
        tags[changed].reg=u8(id);for(size_t j=0;j<4;++j)tags[changed].xyzw[j]=0x3e800001u+uint32_t(changed*4+j);
        b.inputRows.push_back(uint16_t((k<<8)|i));++changed;}
      std::vector<uint32_t> uniforms;if(!makeUniformCommands(tags,changed,uniforms))return -1;
      std::vector<uint16_t> where(changed*4,0xffff);
      for(size_t i=0;i<uniforms.size();++i){const uint32_t v=uniforms[i];if(v>=0x3e800001u&&v<0x3e800001u+changed*4){size_t at=v-0x3e800001u;if(where[at]!=0xffff)return -1;where[at]=uint16_t(i);uniforms[i]=0;}}
      for(auto w:where)if(w==0xffff)return -1;
      const auto& R=*items[k].record;
      if(b.words.size()+uniforms.size()+R.at[1]-R.at[0]+R.at[4]-R.at[3]>8192)return -1;
      const size_t base=b.words.size();b.words.insert(b.words.end(),uniforms.begin(),uniforms.end());for(auto w:where)b.patches.push_back(uint16_t(base+w));
      b.words.insert(b.words.end(),R.words.begin()+R.at[0],R.words.begin()+R.at[1]);
      b.words.insert(b.words.end(),R.words.begin()+R.at[3],R.words.begin()+R.at[4]);
    }
    const size_t bytes=b.words.capacity()*4+b.patches.capacity()*2+b.serials.capacity()*8+b.inputRows.capacity()*2+sizeof b;
    if(bytes>128u<<10)return -1;
    if(commandBlocksBytes_+bytes>(128u<<10)){commandBlocksCache_.clear();commandBlocksBytes_=0;}
    commandBlocksCache_.push_back(std::move(b));commandBlocksBytes_+=bytes;block=&commandBlocksCache_.back();
    } catch(...) { return -1; }
  }
  // Preflight before the first draw: avoid a different split inside the run.
  u32* buffer=nullptr;u32 capacity=0,offset=0;GPUCMD_GetBuffer(&buffer,&capacity,&offset);
  const size_t reserve=block->words.size()+4096; // first draw/context/shader worst case: refuse a program change below
  if(!buffer||offset>capacity||(offset&1)||reserve>capacity-offset||program_!=first.program||!C3Dx_Clean()||
     float(offset+reserve)/float(capacity)>=splitAt_)return -1;
  // All allocations/eligibility checks finished. From here every word has space.
  if(!drawRecorded(*items[0].plan,*items[0].record))return 0;
  size_t patch=0;
  for(const auto encoded:block->inputRows){const float* row=rowPointers[encoded>>8][encoded&255];for(size_t j=0;j<4;++j){uint32_t v;std::memcpy(&v,row+j,4);block->words[block->patches[patch++]]=v;}}
  if(!C3Dx_Append(block->words.data(),u32(block->words.size()))){invalidateCaches();return 1;}
  // Commit only rows actually touched after the first draw. Static rows already
  // have the correct CPU shadow; no redundant material/light/projection copies.
  u8 finalIds[96];float finalRows[96][4];int finalCount=0;
  uint32_t changed[3]={};for(size_t k=1;k<count;++k)for(int j=0;j<3;++j)changed[j]|=masks[k-1][j];
  for(size_t i=0;i<nrows;++i){const unsigned id=ids[i];if(!(changed[id/32]&(1u<<(id%32))))continue;
    finalIds[finalCount]=u8(id);std::memcpy(finalRows[finalCount++],rowPointers[count-1][i],16);
    std::memcpy(uniformShadow_[id],rowPointers[count-1][i],16);uniformValid_[id]=true;}
  C3Dx_UniformsCommit(finalIds,finalRows,finalCount);
  C3D_Tex* const slots[3]={&slot_[0],&slot_[1],&slot_[2]};const auto& last=*items[count-1].record;
  C3Dx_Apply(last.mat->state.data(),last.state.data(),1,slots);C3Dx_Apply(last.mat->state.data(),last.state.data(),7,slots);
  if(kickDraws_)sinceKick_+=int(count-1);
  st_.draws+=uint32_t(count-1);recordedDraws_+=uint32_t(count-1);++commandBlocks_;commandBlockDraws_+=uint32_t(count);lastIndices_=nullptr;
  return int(count);
}
}
