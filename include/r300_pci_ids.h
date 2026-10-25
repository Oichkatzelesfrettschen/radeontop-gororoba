// R300-class IGP device ids (RS400/RS480/RS482/RS485/RS485M).  Kept separate from
// r600_pci_ids.h because getamdgpuids.sh regenerates that file from the amdgpu
// id tables and would drop hand-maintained entries.  These parts predate
// UVD/VCE: engine-busy is read from RBBM_STATUS (0x0E40), not the R600+
// GRBM_STATUS -- see initbits() and getgrbm_pci_r300() in detect.c.
CHIPSET(0x5954, RS480_5954, RS480)
CHIPSET(0x5955, RS480_5955, RS480)
CHIPSET(0x5974, RS485M_5974, RS480)
CHIPSET(0x5975, RS482_5975, RS480)
CHIPSET(0x5a41, RS400_5A41, RS480)
CHIPSET(0x5a42, RS400_5A42, RS480)
CHIPSET(0x5a61, RS400_5A61, RS480)
CHIPSET(0x5a62, RS400_5A62, RS480)
