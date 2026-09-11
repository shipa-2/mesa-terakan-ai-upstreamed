/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_shader_generation.h"
#include "gallium/drivers/r600/r600_asm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool
check_r700_memory_export_encoding(void)
{
   /* AMD R700 ISA 9-25 and 10-10/10-12: indexed write, four DWORDs,
    * source R5, DWORD address R7 + 0x123, ARRAY_SIZE unused (zero).
    * This is an encoding fixture, NOT an executable shader: the registers
    * are not initialized and no export aperture or ES launch is configured.
    * In particular this does not prove a NIR SSBO store lowers to MEM_EXPORT.
    */
   struct r600_isa *isa = calloc(1, sizeof(*isa));
   if (!isa)
      return false;
   struct r600_bytecode bc = {0};
   r600_bytecode_init(&bc, terakan_shader_gfx_level(CHIP_RV710), CHIP_RV710, false);
   if (r600_isa_init(bc.gfx_level, isa)) {
      r600_isa_destroy(isa);
      return false;
   }
   bc.isa = isa;
   struct r600_bytecode_output const output = {
      .op = CF_OP_MEM_EXPORT,
      .type = 1,
      .gpr = 5,
      .index_gpr = 7,
      .array_base = 0x123,
      .array_size = 0,
      .elem_size = 3,
      .comp_mask = 0xf,
      .burst_count = 1,
   };
   bool passed = !r600_bytecode_add_output(&bc, &output) && !r600_bytecode_build(&bc);
   /* Literal words keep this oracle independent of the encoder's field macros.
    * BARRIER=1, CF_INST=0x3a, COMP_MASK=0xf, all other WORD1 fields zero.
    */
   passed = passed && bc.ndw == 2 && bc.bytecode[0] == UINT32_C(0xc382a123) &&
            bc.bytecode[1] == UINT32_C(0x9d00f000);
   if (!passed) {
      fprintf(stderr, "R700 MEM_EXPORT encoding mismatch (ndw=%u)\n", bc.ndw);
      if (bc.bytecode && bc.ndw >= 2)
         fprintf(stderr, "  actual=%08x %08x expected=c382a123 9d00f000\n",
                 bc.bytecode[0], bc.bytecode[1]);
   }
   r600_bytecode_clear(&bc);
   r600_isa_destroy(isa);
   return passed;
}

static bool
check(enum radeon_family const family, enum amd_gfx_level const expected_gfx_level,
      enum r600_chip_class const expected_isa_chip_class)
{
   enum amd_gfx_level const gfx_level = terakan_shader_gfx_level(family);
   enum r600_chip_class const isa_chip_class = terakan_shader_isa_chip_class(family);
   if (gfx_level == expected_gfx_level && isa_chip_class == expected_isa_chip_class)
      return true;

   fprintf(stderr, "family %u: gfx level %u (expected %u), ISA class %u (expected %u)\n", family,
           gfx_level, expected_gfx_level, isa_chip_class, expected_isa_chip_class);
   return false;
}

static bool
check_pci_id(uint32_t const pci_device_id, enum radeon_family const expected_family)
{
   enum radeon_family const family = terakan_shader_family_from_pci_id(pci_device_id);
   if (family == expected_family)
      return true;

   fprintf(stderr, "PCI ID 0x%04x: family %u (expected %u)\n", pci_device_id, family,
           expected_family);
   return false;
}

static bool
check_nir_options(enum radeon_family const family, bool const pre_evergreen)
{
   nir_shader_compiler_options non_fs, fs;
   terakan_shader_nir_options_init(family, &non_fs, &fs);

   bool const passed =
      non_fs.force_indirect_unrolling_sampler == pre_evergreen &&
      non_fs.lower_bit_count == pre_evergreen &&
      non_fs.lower_bitfield_reverse == pre_evergreen && non_fs.has_bfe == !pre_evergreen &&
      non_fs.has_bfm == !pre_evergreen && non_fs.has_bitfield_select == !pre_evergreen &&
      /* Never zero-based on either generation: the base is in `VGT_INDX_OFFSET`, so R0.X already
       * carries it, which is what Vulkan's `VertexIndex` is. Asking NIR to treat it as zero-based
       * made it add the base a second time, and every one of the 144 supported
       * dEQP-VK.draw.*.indexed_draw cases failed.
       */
      !non_fs.vertex_id_zero_based && !non_fs.lower_all_io_to_temps &&
      fs.lower_all_io_to_temps && fs.force_indirect_unrolling_sampler == pre_evergreen &&
      fs.lower_bit_count == pre_evergreen && fs.has_bfe == !pre_evergreen;
   if (!passed) {
      fprintf(stderr, "family %u: incorrect %s NIR options\n", family,
              pre_evergreen ? "pre-Evergreen" : "Evergreen+");
   }
   return passed;
}

int
main(void)
{
   bool passed = true;
   passed &= check_r700_memory_export_encoding();

   passed &= check(CHIP_R600, R600, ISA_CC_R600);
   passed &= check(CHIP_RS880, R600, ISA_CC_R600);

   passed &= check(CHIP_RV770, R700, ISA_CC_R700);
   passed &= check(CHIP_RV730, R700, ISA_CC_R700);
   passed &= check(CHIP_RV710, R700, ISA_CC_R700);
   passed &= check(CHIP_RV740, R700, ISA_CC_R700);

   passed &= check(CHIP_CEDAR, EVERGREEN, ISA_CC_EVERGREEN);
   passed &= check(CHIP_CAICOS, EVERGREEN, ISA_CC_EVERGREEN);
   passed &= check(CHIP_CAYMAN, CAYMAN, ISA_CC_CAYMAN);
   passed &= check(CHIP_ARUBA, CAYMAN, ISA_CC_CAYMAN);

   /* Exercise the same generated PCI-ID table used by physical-device enumeration, not merely
    * hand-picked family enum values. This includes all R600, R700, Evergreen and Cayman IDs in
    * r600_pci_ids.h and catches a missing/incorrect family token in that table.
    */
#define CHIPSET(chipset_pci_id, chipset_name, chipset_family)                                      \
   passed &= check_pci_id(chipset_pci_id, CHIP_##chipset_family);
#include "pci_ids/r600_pci_ids.h"
#undef CHIPSET
   passed &= terakan_shader_family_from_pci_id(UINT32_C(0xFFFFFFFF)) == CHIP_UNKNOWN;

   passed &= check_nir_options(CHIP_R600, true);
   passed &= check_nir_options(CHIP_RV710, true);
   passed &= check_nir_options(CHIP_CEDAR, false);
   passed &= check_nir_options(CHIP_CAYMAN, false);

   return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
