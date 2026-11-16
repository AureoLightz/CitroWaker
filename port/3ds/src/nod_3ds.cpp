// The subset of nod (the disc image library Aurora's DVD layer uses) that dvd.cpp / fst.cpp call, for a plain
// GameCube disc image (.iso / .gcm). The 3DS build cannot use the prebuilt nod (Rust, x86/arm64 only).
//
// GCM layout: boot.bin at 0 (game id, version, ...; 0x420 DOL offset, 0x424 FST offset, 0x428 FST size), bi2.bin at
// 0x440 (0x2000 bytes), the apploader at 0x2440. FST: 12-byte entries (u8 kind, u24 name offset, u32 file offset or
// parent, u32 length or next index), entry 0 is the root (its length = entry count), then the name table.
#include <nod.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

struct NodHandle {
  enum Kind { Disc, Partition, File } kind = Disc;
  // disc
  NodDiscStream stream{};
  std::mutex* mu = nullptr;
  uint8_t header[0x440] = {};
  std::vector<uint8_t> fst, dol, bi2, apploader;
  bool metaLoaded = false;
  // partition / file
  NodHandle* disc = nullptr;
  uint64_t start = 0, size = 0, pos = 0;
};

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool readAt(NodHandle* d, uint64_t off, void* out, size_t len) {
  std::lock_guard lock{*d->mu};
  int64_t got = 0;
  while (size_t(got) < len) {
    const int64_t n = d->stream.read_at(d->stream.user_data, off + got, static_cast<uint8_t*>(out) + got, len - size_t(got));
    if (n <= 0) return false;
    got += n;
  }
  return true;
}

bool loadMeta(NodHandle* d) {
  if (d->metaLoaded) return true;
  const uint32_t fstOff = be32(d->header + 0x424), fstSize = be32(d->header + 0x428);
  d->fst.resize(fstSize);
  if (!readAt(d, fstOff, d->fst.data(), fstSize)) return false;
  d->bi2.resize(0x2000);
  if (!readAt(d, 0x440, d->bi2.data(), d->bi2.size())) return false;
  uint8_t alh[0x20];
  if (!readAt(d, 0x2440, alh, sizeof alh)) return false;
  d->apploader.resize(0x20 + be32(alh + 0x14) + be32(alh + 0x18));
  if (!readAt(d, 0x2440, d->apploader.data(), d->apploader.size())) return false;
  d->metaLoaded = true;
  return true;
}
}  // namespace

extern "C" {

const char* nod_error_message(void) { return "nod (3DS): GameCube disc image error"; }

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions*, NodHandle** out) {
  auto* d = new NodHandle;
  d->kind = NodHandle::Disc;
  d->stream = *stream;
  d->mu = new std::mutex;
  if (!readAt(d, 0, d->header, sizeof d->header) || !NOD_MAGIC_EQ(d->header + 0x1C, GCN_MAGIC)) {
    if (d->stream.close) d->stream.close(d->stream.user_data);
    delete d->mu;
    delete d;
    return NOD_RESULT_ERR_FORMAT;
  }
  *out = d;
  return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions*, NodHandle** out) {
  if (!disc || disc->kind != NodHandle::Disc || kind != NOD_PARTITION_KIND_DATA) return NOD_RESULT_ERR_NOT_FOUND;
  if (!loadMeta(disc)) return NOD_RESULT_ERR_IO;
  auto* p = new NodHandle;
  p->kind = NodHandle::Partition;
  p->disc = disc;
  *out = p;
  return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition(NodHandle* disc, uint32_t index, const NodPartitionOptions* o, NodHandle** out) {
  if (index != 0) return NOD_RESULT_ERR_NOT_FOUND;
  return nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, o, out);
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  if (!disc || disc->kind != NodHandle::Disc) return NOD_RESULT_ERR_INVALID_HANDLE;
  static_assert(sizeof(NodDiscHeader) <= 0x440, "header");
  std::memcpy(out, disc->header, sizeof *out);
  return NOD_RESULT_OK;
}

namespace {
bool loadDol(NodHandle* d) {   // only DVDGetDOLLocation wants it (3.6 MB): read on first use
  if (!d->dol.empty()) return true;
  const uint32_t dolOff = be32(d->header + 0x420);
  uint8_t dh[0x100];
  if (!readAt(d, dolOff, dh, sizeof dh)) return false;
  uint32_t dolSize = 0x100;
  for (int i = 0; i < 18; i++) {   // 7 text + 11 data sections: file offsets at 0x00, sizes at 0x90
    const uint32_t o = be32(dh + 4 * i), s = be32(dh + 0x90 + 4 * i);
    if (o && s && o + s > dolSize) dolSize = o + s;
  }
  d->dol.resize(dolSize);
  if (!readAt(d, dolOff, d->dol.data(), dolSize)) return false;
  return true;
}
}  // namespace

NodResult nod_partition_meta(const NodHandle* part, NodPartitionMeta* out) {
  if (!part || part->kind != NodHandle::Partition) return NOD_RESULT_ERR_INVALID_HANDLE;
  NodHandle* d = part->disc;
  if (!loadDol(d)) return NOD_RESULT_ERR_IO;
  std::memset(out, 0, sizeof *out);
  out->raw_boot = {d->header, sizeof d->header};
  out->raw_bi2 = {d->bi2.data(), d->bi2.size()};
  out->raw_apploader = {d->apploader.data(), d->apploader.size()};
  out->raw_dol = {d->dol.data(), d->dol.size()};
  out->raw_fst = {d->fst.data(), d->fst.size()};
  return NOD_RESULT_OK;
}

void nod_partition_iterate_fst(const NodHandle* part, NodFstCallback cb, void* user) {
  if (!part || part->kind != NodHandle::Partition) return;
  const std::vector<uint8_t>& f = part->disc->fst;
  if (f.size() < 12) return;
  const uint32_t count = be32(&f[8]);
  if (uint64_t(count) * 12 > f.size()) return;
  const char* names = reinterpret_cast<const char*>(&f[size_t(count) * 12]);
  const size_t namesLen = f.size() - size_t(count) * 12;
  for (uint32_t i = 1; i < count;) {
    const uint8_t* e = &f[size_t(i) * 12];
    const uint32_t nameOff = be32(e) & 0xFFFFFF;
    const bool dir = e[0] != 0;
    const char* name = nameOff < namesLen ? names + nameOff : "";
    const uint32_t r = cb(i, dir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE, name, be32(e + 8), user);
    if (r == NOD_FST_STOP) break;
    i = r > i ? r : i + 1;
  }
}

NodResult nod_partition_open_file(NodHandle* part, uint32_t fstIndex, NodHandle** out) {
  if (!part || part->kind != NodHandle::Partition) return NOD_RESULT_ERR_INVALID_HANDLE;
  const std::vector<uint8_t>& f = part->disc->fst;
  const uint32_t count = f.size() >= 12 ? be32(&f[8]) : 0;
  if (fstIndex == 0 || fstIndex >= count) return NOD_RESULT_ERR_NOT_FOUND;
  const uint8_t* e = &f[size_t(fstIndex) * 12];
  if (e[0] != 0) return NOD_RESULT_ERR_NOT_FOUND;   // a directory
  auto* h = new NodHandle;
  h->kind = NodHandle::File;
  h->disc = part->disc;
  h->start = be32(e + 4);
  h->size = be32(e + 8);
  *out = h;
  return NOD_RESULT_OK;
}

void nod_free(NodHandle* h) {
  if (!h) return;
  if (h->kind == NodHandle::Disc) {
    if (h->stream.close) h->stream.close(h->stream.user_data);
    delete h->mu;
  }
  delete h;
}

int64_t nod_read(NodHandle* h, uint8_t* buf, size_t len) {
  if (!h || h->kind != NodHandle::File) return -1;
  if (h->pos >= h->size) return 0;
  if (len > h->size - h->pos) len = size_t(h->size - h->pos);
  if (!readAt(h->disc, h->start + h->pos, buf, len)) return -1;
  h->pos += len;
  return int64_t(len);
}

int64_t nod_seek(NodHandle* h, int64_t offset, int32_t whence) {
  if (!h || h->kind != NodHandle::File) return -1;
  int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(h->pos) : int64_t(h->size);
  const int64_t p = base + offset;
  if (p < 0) return -1;
  h->pos = uint64_t(p);
  return p;
}

}  // extern "C"
