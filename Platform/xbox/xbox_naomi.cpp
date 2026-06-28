// ============================================================================
//  xbox_naomi.cpp  --  NAOMI networking devices (Multiboard / NetDimm /
//  NaomiM3Comm). Referenced by the NAOMI platform code but never active on a
//  Dreamcast boot: inert methods, open-bus reads.
// ============================================================================
#include "types.h"
#include "hw/naomi/multiboard.h"
#include "hw/naomi/naomi_m3comm.h"
#include "hw/naomi/netdimm.h"

// ---- Multiboard (NAOMI multi-PCB link) -------------------------------------
Multiboard::Multiboard() {}
Multiboard::~Multiboard() {}
void Multiboard::startSlave() {}

// ---- NaomiM3Comm (M3 comm board) -------------------------------------------
u32  NaomiM3Comm::ReadMem(u32 /*addr*/, u32 /*size*/)            { return 0xFFFFFFFF; }
void NaomiM3Comm::WriteMem(u32 /*addr*/, u32 /*data*/, u32 /*size*/) {}
bool NaomiM3Comm::DmaStart(u32 /*addr*/, u32 /*data*/)          { return false; }
void NaomiM3Comm::closeNetwork() {}

// ---- NetDimm (networked GD-ROM cartridge) ----------------------------------
// With no network it's just a GD-ROM cartridge: delegate the cart ops to the
// GDCartridge base and no-op the network polling.
NetDimm::NetDimm(u32 size) : GDCartridge(size) {}
void NetDimm::Init(LoadProgress* progress, std::vector<u8>* digest) { GDCartridge::Init(progress, digest); }
bool NetDimm::Write(u32 offset, u32 size, u32 data) { return GDCartridge::Write(offset, size, data); }
void NetDimm::Deserialize(Deserializer& deser) { GDCartridge::Deserialize(deser); }
void NetDimm::process() {}
int  NetDimm::schedCallback() { return 0; }
