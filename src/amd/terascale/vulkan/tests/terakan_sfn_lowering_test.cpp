/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "compiler/glsl_types.h"
#include "gallium/drivers/r600/sfn/sfn_instr_alu.h"
#include "gallium/drivers/r600/sfn/sfn_instr_export.h"
#include "gallium/drivers/r600/sfn/sfn_instr_fetch.h"
#include "gallium/drivers/r600/sfn/sfn_instr_mem.h"
#include "gallium/drivers/r600/sfn/sfn_assembler.h"
#include "gallium/drivers/r600/sfn/sfn_memorypool.h"
#include "gallium/drivers/r600/sfn/sfn_nir.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_alu.h"
#include "gallium/drivers/r600/sfn/sfn_scheduler.h"
#include "gallium/drivers/r600/sfn/sfn_shader_fs.h"
#include "gallium/drivers/r600/sfn/sfn_split_address_loads.h"
#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

extern "C" bool terakan_nir_lower_subgroups(nir_shader *shader);

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         std::fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);      \
         std::abort();                                                                              \
      }                                                                                             \
   } while (0)

static void
test_dead_required_instruction_readiness()
{
   using namespace r600;

   init_pool();

   auto producer_dest = new Register(128, 0, pin_none);
   AluInstr producer(
      op1_mov, producer_dest, new LiteralConstant(1), {alu_write, alu_last_instr});
   AluInstr dead_fetch_result(
      op1_mov, new Register(129, 0, pin_none), producer_dest, {alu_write, alu_last_instr});
   AluInstr barrier(op0_group_barrier);

   producer.set_blockid(0, 0);
   dead_fetch_result.set_blockid(0, 1);
   barrier.set_blockid(1, 0);
   barrier.add_required_instr(&dead_fetch_result);

   CHECK(!dead_fetch_result.ready());
   CHECK(dead_fetch_result.set_dead());
   CHECK(barrier.ready());

   release_pool();
}

static void
test_fetch_dead_required_instruction_readiness()
{
   using namespace r600;

   init_pool();

   auto source = new Register(128, 0, pin_none);
   AluInstr dead_fetch(
      op1_mov, new Register(129, 0, pin_none), new LiteralConstant(1),
      {alu_write, alu_last_instr});
   FetchInstr fetch(vc_fetch,
                    RegisterVec4(130),
                    {0, 7, 7, 7},
                    source,
                    0,
                    vertex_data,
                    fmt_32,
                    vtx_nf_int,
                    vtx_es_none,
                    4,
                    nullptr);

   dead_fetch.set_blockid(0, 0);
   fetch.set_blockid(0, 1);
   fetch.add_required_instr(&dead_fetch);

   CHECK(!fetch.ready());
   CHECK(dead_fetch.set_dead());
   CHECK(fetch.ready());

   release_pool();
}

static void
test_rat_dead_required_instruction_readiness()
{
   using namespace r600;

   init_pool();

   AluInstr dead_dependency(
      op1_mov, new Register(128, 0, pin_none), new LiteralConstant(1),
      {alu_write, alu_last_instr});
   RatInstr rat(cf_mem_rat,
                RatInstr::NOP_RTN,
                RegisterVec4(129),
                RegisterVec4(130),
                0,
                nullptr,
                1,
                0xf,
                0);

   dead_dependency.set_blockid(0, 0);
   rat.set_blockid(0, 1);
   rat.add_required_instr(&dead_dependency);

   CHECK(!rat.ready());
   CHECK(dead_dependency.set_dead());
   CHECK(rat.ready());

   release_pool();
}

static void
test_r700_mem_export_assembler_lowering()
{
   using namespace r600;

   /* Exercise the SFN instruction path rather than just r600_bytecode_output.
    * A real R700 compute dispatch still needs ES state and SX aperture setup;
    * this test deliberately stops at bytecode construction. */
   init_pool();
   r600_shader_key key = {};
   FragmentShaderR600 shader(key);
   shader.set_chip_class(ISA_CC_R700);
   shader.set_chip_family(CHIP_RV710);
   shader.emit_instruction(new MemRingOutInstr(cf_mem_export,
                                                MemRingOutInstr::mem_write_ind,
                                                RegisterVec4(5, false),
                                                0x123,
                                                1,
                                                new Register(7, 0, pin_none)));

   r600_shader compiled = {};
   compiled.processor_type = PIPE_SHADER_COMPUTE;
   r600_bytecode_init(&compiled.bc, R700, CHIP_RV710, false);
   auto *isa = static_cast<r600_isa *>(calloc(1, sizeof(r600_isa)));
   CHECK(isa != nullptr);
   CHECK(!r600_isa_init(R700, isa));
   compiled.bc.isa = isa;
   Assembler assembler(&compiled, key);
   CHECK(assembler.lower(&shader));
   CHECK(!r600_bytecode_build(&compiled.bc));
   CHECK(compiled.bc.ndw == 2);
   /* The assembler closes the sole CF instruction: EOP is set here. The
    * lower-level encoding fixture deliberately checks a non-final CF pair. */
   CHECK(compiled.bc.bytecode[0] == 0x0382a123);
   CHECK(compiled.bc.bytecode[1] == 0x9d201000);

   r600_bytecode_clear(&compiled.bc);
   r600_isa_destroy(isa);
   release_pool();
}

static r600::Shader *
shader_from_string(std::string const & source)
{
   using namespace r600;

   std::istringstream input(source);
   std::string line;
   r600_shader_key key = {};
   key.ps.nr_cbufs = 1;
   auto shader = new FragmentShaderEG(key);
   shader->reset_shader_id();

   std::getline(input, line);
   CHECK(line == "FS");
   while (std::getline(input, line)) {
      if (line.empty())
         continue;
      if (line == "SHADER")
         break;
      std::istringstream property(line);
      CHECK(shader->add_info_from_string(property));
   }
   while (std::getline(input, line)) {
      if (!line.empty())
         shader->emit_instruction_from_string(line);
   }
   return shader;
}

static void
test_address_load_ignores_future_register_write()
{
   using namespace r600;

   static char const source[] = R"(FS
CHIPCLASS EVERGREEN
PROP MAX_COLOR_EXPORTS:1
PROP COLOR_EXPORTS:1
PROP COLOR_EXPORT_MASK:15
OUTPUT LOC:0 FRAG_RESULT:2 MASK:15
SYSVALUES R0.xy__
REGISTERS R1.x
ARRAYS A2[4].x
SHADER
BLOCK_START
  TEX SAMPLE S5.xyzw : R0.xy__ RID:0 SID:0 NNNN
BLOCK_END
BLOCK_START
  ALU MOV R1.x : L[0x0] {W}
  ALU MOV S2.x@group : A2[R1.x].x {WL}
BLOCK_END
BLOCK_START
  EXPORT_DONE PIXEL 0 S2.xxxx
BLOCK_END
)";

   init_pool();
   Shader * shader = shader_from_string(source);
   CHECK(shader);
   CHECK(split_address_loads(*shader));
   unsigned address_load_count = 0;
   for (Block * block : shader->func()) {
      for (Instr * instruction : *block) {
         AluInstr * const alu = instruction->as_alu();
         if (!alu || alu->opcode() != op1_mova_int)
            continue;
         ++address_load_count;
         for (Instr * dependency : alu->required_instr()) {
            CHECK(dependency->block_id() == alu->block_id());
            CHECK(dependency->index() < alu->index());
         }
      }
   }
   CHECK(address_load_count == 1);
   Shader * scheduled = schedule(shader);
   CHECK(scheduled);
   if (scheduled != shader)
      delete shader;
   delete scheduled;
   release_pool();
}

static void
test_address_load_ignores_loop_carried_future_parent()
{
   using namespace r600;

   static char const source[] = R"(FS
CHIPCLASS EVERGREEN
PROP MAX_COLOR_EXPORTS:1
PROP COLOR_EXPORTS:1
PROP COLOR_EXPORT_MASK:15
OUTPUT LOC:0 FRAG_RESULT:2 MASK:15
REGISTERS R1.x R3.x
ARRAYS A2[4].x
SHADER
BLOCK_START
  ALU MOV R1.x : L[0x0] {W}
  ALU MOV R3.x : L[0x1] {W}
BLOCK_END
BLOCK_START
  ALU ADD S4.x : R3.x L[0x1] {W}
  ALU ADD S5.x : S4.x L[0x1] {W}
  ALU ADD S6.x@group : A2[R1.x].x S5.x {WL}
BLOCK_END
BLOCK_START
  ALU MOV R3.x : L[0x2] {W}
  EXPORT_DONE PIXEL 0 S6.xxxx
BLOCK_END
)";

   init_pool();
   Shader *shader = shader_from_string(source);
   CHECK(shader);
   CHECK(split_address_loads(*shader));
   unsigned address_load_count = 0;
   unsigned preceding_dependency_count = 0;
   for (Block *block : shader->func()) {
      for (Instr *instruction : *block) {
         AluInstr *const alu = instruction->as_alu();
         if (!alu || alu->opcode() != op1_mova_int)
            continue;
         ++address_load_count;
         for (Instr *dependency : alu->required_instr()) {
            CHECK(dependency->block_id() < alu->block_id() ||
                  (dependency->block_id() == alu->block_id() &&
                   dependency->index() < alu->index()));
            ++preceding_dependency_count;
         }
      }
   }
   CHECK(address_load_count == 1);
   CHECK(preceding_dependency_count >= 1);
   delete shader;
   release_pool();
}

static void
test_shared_store_lowering()
{
   static nir_shader_compiler_options const options = {};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "terakan SFN shared IO test");

   nir_def * const value = nir_imm_ivec2(&b, 0x11223344, 0x55667788);
   nir_def * const address = nir_imm_int(&b, 16);
   nir_intrinsic_instr * const store =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_store_shared);
   nir_def_init(&store->instr, &store->def, 2, 32);
   store->num_components = 2;
   store->src[0] = nir_src_for_ssa(value);
   store->src[1] = nir_src_for_ssa(address);
   nir_intrinsic_set_align(store, 8, 0);
   nir_intrinsic_set_access(store, (gl_access_qualifier)0);
   nir_intrinsic_set_write_mask(store, 0x3);
   nir_builder_instr_insert(&b, &store->instr);

   CHECK(r600_lower_shared_io(b.shader));

   unsigned generic_store_count = 0;
   unsigned local_store_count = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
            if (intrinsic->intrinsic == nir_intrinsic_store_shared)
               ++generic_store_count;
            if (intrinsic->intrinsic == nir_intrinsic_store_local_shared_r600) {
               ++local_store_count;
               CHECK(intrinsic->num_components == 2);
               CHECK(nir_intrinsic_write_mask(intrinsic) == 0x3);
               CHECK(intrinsic->src[0].ssa->num_components == 2);
            }
         }
      }
   }

   CHECK(generic_store_count == 0);
   CHECK(local_store_count == 1);

   ralloc_free(b.shader);
}

static void
test_front_face_lowering()
{
   static nir_shader_compiler_options const options = {};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "terakan front face test");

   nir_def * const barycentric =
      nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_intrinsic_instr * const load =
      nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_interpolated_input);
   nir_def_init(&load->instr, &load->def, 1, 32);
   load->num_components = 1;
   load->src[0] = nir_src_for_ssa(barycentric);
   load->src[1] = nir_src_for_ssa(nir_imm_int(&b, 0));
   nir_intrinsic_set_base(load, 0);
   nir_intrinsic_set_component(load, 0);
   nir_intrinsic_set_dest_type(load, nir_type_bool32);
   nir_io_semantics semantics = {};
   semantics.location = VARYING_SLOT_FACE;
   semantics.num_slots = 1;
   nir_intrinsic_set_io_semantics(load, semantics);
   nir_builder_instr_insert(&b, &load->instr);

   CHECK(r600_lower_fs_special_inputs(b.shader));

   unsigned interpolated_count = 0;
   unsigned input_count = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr * const intrinsic = nir_instr_as_intrinsic(instr);
            if (intrinsic->intrinsic == nir_intrinsic_load_interpolated_input)
               ++interpolated_count;
            if (intrinsic->intrinsic == nir_intrinsic_load_input) {
               ++input_count;
               CHECK(nir_intrinsic_io_semantics(intrinsic).location == VARYING_SLOT_FACE);
               CHECK(nir_intrinsic_dest_type(intrinsic) == nir_type_bool32);
               CHECK(intrinsic->def.num_components == 1);
            }
         }
      }
   }

   CHECK(interpolated_count == 0);
   CHECK(input_count == 1);

   ralloc_free(b.shader);
}

static void
test_half_pack_lowering()
{
   static nir_shader_compiler_options const options = {};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "terakan half pack test");

   nir_def * const values = nir_vec2(&b, nir_imm_float(&b, 1.0f), nir_imm_float(&b, -2.0f));
   nir_def * const packed = nir_pack_half_2x16(&b, values);
   nir_store_var(&b, nir_local_variable_create(
                        nir_shader_get_entrypoint(b.shader), glsl_uint_type(), "packed"),
                 packed, 1);

   CHECK(r600_nir_lower_pack_unpack_2x16(b.shader));

   unsigned generic_pack_count = 0;
   unsigned split_pack_count = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_op const op = nir_instr_as_alu(instr)->op;
            generic_pack_count += op == nir_op_pack_half_2x16;
            split_pack_count += op == nir_op_pack_half_2x16_split;
         }
      }
   }

   CHECK(generic_pack_count == 0);
   CHECK(split_pack_count == 1);

   ralloc_free(b.shader);
}

static void
test_uniform_subgroup_lowering()
{
   static nir_shader_compiler_options const options = {};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "terakan subgroup test");

   nir_def * const uniform_value = nir_imm_float(&b, 0.5f);
   nir_def * const first = nir_read_first_invocation(&b, uniform_value);
   nir_store_var(&b, nir_local_variable_create(
                        nir_shader_get_entrypoint(b.shader), glsl_float_type(), "first"),
                 first, 1);

   static nir_lower_subgroups_options const subgroup_options = {
      .ballot_bit_size = 32,
      .ballot_components = 1,
   };
   CHECK(nir_opt_uniform_subgroup(b.shader, &subgroup_options));

   unsigned read_first_count = 0;
   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               read_first_count +=
                  nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_read_first_invocation;
            }
         }
      }
   }

   CHECK(read_first_count == 0);

   ralloc_free(b.shader);
}

static void
test_singleton_subgroup_lowering()
{
   static nir_shader_compiler_options const options = {};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options,
                                     "terakan singleton subgroup test");

   nir_def * const value = nir_load_local_invocation_index(&b);
   nir_def * const first = nir_read_first_invocation(&b, value);
   nir_def * const minimum = nir_reduce(&b, first, .reduction_op = nir_op_umin);
   nir_def * const exclusive =
      nir_exclusive_scan(&b, minimum, .reduction_op = nir_op_iadd);
   nir_def * const ballot = nir_ballot(&b, 4, 32, nir_uge_imm(&b, minimum, 1));
   nir_def * const ballot_count = nir_ballot_bit_count_reduce(&b, 32, ballot);
   nir_store_var(&b, nir_local_variable_create(
                        nir_shader_get_entrypoint(b.shader), glsl_uint_type(), "result"),
                 nir_iadd(&b, nir_iadd(&b, minimum, exclusive), ballot_count), 1);

   CHECK(terakan_nir_lower_subgroups(b.shader));

   nir_foreach_function_impl(impl, b.shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_read_first_invocation:
            case nir_intrinsic_reduce:
            case nir_intrinsic_exclusive_scan:
            case nir_intrinsic_ballot:
            case nir_intrinsic_ballot_bit_count_reduce:
               CHECK(false);
            default:
               break;
            }
         }
      }
   }

   ralloc_free(b.shader);
}

int
main()
{
   glsl_type_singleton_init_or_ref();
   test_dead_required_instruction_readiness();
   test_fetch_dead_required_instruction_readiness();
   test_rat_dead_required_instruction_readiness();
   test_r700_mem_export_assembler_lowering();
   test_address_load_ignores_future_register_write();
   test_address_load_ignores_loop_carried_future_parent();
   test_shared_store_lowering();
   test_front_face_lowering();
   test_half_pack_lowering();
   test_uniform_subgroup_lowering();
   test_singleton_subgroup_lowering();
   glsl_type_singleton_decref();
   return EXIT_SUCCESS;
}
