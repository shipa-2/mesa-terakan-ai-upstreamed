/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#include "sfn_instr_mem.h"

#include "util/bitscan.h"
#include "util/macros.h"

#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "sfn_alu_defines.h"
#include "sfn_instr_alu.h"
#include "sfn_instr_export.h"
#include "sfn_instr_fetch.h"
#include "sfn_instr_tex.h"
#include "sfn_shader.h"
#include "sfn_virtualvalues.h"

namespace r600 {

GDSInstr::GDSInstr(
   ESDOp op, Register *dest, const RegisterVec4& src, int uav_base, PRegister uav_id):
    Resource(this, uav_base, uav_id),
    m_op(op),
    m_dest(dest),
    m_src(src)
{
   set_always_keep();

   m_src.add_use(this);
   if (m_dest)
      m_dest->add_parent(this);
}

bool
GDSInstr::is_equal_to(const GDSInstr& rhs) const
{
#define NE(X) (X != rhs.X)

   if (NE(m_op) || NE(m_src))
      return false;

   sfn_value_equal(m_dest, rhs.m_dest);

   return resource_is_equal(rhs);
}

void
GDSInstr::accept(ConstInstrVisitor& visitor) const
{
   visitor.visit(*this);
}

void
GDSInstr::accept(InstrVisitor& visitor)
{
   visitor.visit(this);
}

bool
GDSInstr::do_ready() const
{
   return m_src.ready(block_id(), index()) && resource_ready(block_id(), index());
}

void
GDSInstr::do_print(std::ostream& os) const
{
   os << "GDS " << lds_ops.at(m_op).name;
   if (m_dest)
      os << *m_dest;
   else
      os << "___";
   os << " " << m_src;
   os << " BASE:" << resource_id();

   print_resource_offset(os);
}

bool
GDSInstr::emit_atomic_counter(nir_intrinsic_instr *intr, Shader& shader)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_atomic_counter_add:
   case nir_intrinsic_atomic_counter_and:
   case nir_intrinsic_atomic_counter_exchange:
   case nir_intrinsic_atomic_counter_max:
   case nir_intrinsic_atomic_counter_min:
   case nir_intrinsic_atomic_counter_or:
   case nir_intrinsic_atomic_counter_xor:
   case nir_intrinsic_atomic_counter_comp_swap:
      return emit_atomic_op2(intr, shader);
   case nir_intrinsic_atomic_counter_read:
   case nir_intrinsic_atomic_counter_post_dec:
      return emit_atomic_read(intr, shader);
   case nir_intrinsic_atomic_counter_inc:
      return emit_atomic_inc(intr, shader);
   case nir_intrinsic_atomic_counter_pre_dec:
      return emit_atomic_pre_dec(intr, shader);
   default:
      return false;
   }
}

uint8_t GDSInstr::allowed_src_chan_mask() const
{
   return m_src.free_chan_mask();
}

static ESDOp
get_opcode(const nir_intrinsic_op opcode)
{
   switch (opcode) {
   case nir_intrinsic_atomic_counter_add:
      return DS_OP_ADD_RET;
   case nir_intrinsic_atomic_counter_and:
      return DS_OP_AND_RET;
   case nir_intrinsic_atomic_counter_exchange:
      return DS_OP_XCHG_RET;
   case nir_intrinsic_atomic_counter_inc:
      return DS_OP_INC_RET;
   case nir_intrinsic_atomic_counter_max:
      return DS_OP_MAX_UINT_RET;
   case nir_intrinsic_atomic_counter_min:
      return DS_OP_MIN_UINT_RET;
   case nir_intrinsic_atomic_counter_or:
      return DS_OP_OR_RET;
   case nir_intrinsic_atomic_counter_read:
      return DS_OP_READ_RET;
   case nir_intrinsic_atomic_counter_xor:
      return DS_OP_XOR_RET;
   case nir_intrinsic_atomic_counter_post_dec:
      return DS_OP_DEC_RET;
   case nir_intrinsic_atomic_counter_comp_swap:
      return DS_OP_CMP_XCHG_RET;
   case nir_intrinsic_atomic_counter_pre_dec:
   default:
      return DS_OP_INVALID;
   }
}

static ESDOp
get_opcode_wo(const nir_intrinsic_op opcode)
{
   switch (opcode) {
   case nir_intrinsic_atomic_counter_add:
      return DS_OP_ADD;
   case nir_intrinsic_atomic_counter_and:
      return DS_OP_AND;
   case nir_intrinsic_atomic_counter_inc:
      return DS_OP_INC;
   case nir_intrinsic_atomic_counter_max:
      return DS_OP_MAX_UINT;
   case nir_intrinsic_atomic_counter_min:
      return DS_OP_MIN_UINT;
   case nir_intrinsic_atomic_counter_or:
      return DS_OP_OR;
   case nir_intrinsic_atomic_counter_xor:
      return DS_OP_XOR;
   case nir_intrinsic_atomic_counter_post_dec:
      return DS_OP_DEC;
   case nir_intrinsic_atomic_counter_comp_swap:
      return DS_OP_CMP_XCHG_RET;
   case nir_intrinsic_atomic_counter_exchange:
      return DS_OP_XCHG_RET;
   case nir_intrinsic_atomic_counter_pre_dec:
   default:
      return DS_OP_INVALID;
   }
}

bool
GDSInstr::emit_atomic_op2(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   bool read_result = !list_is_empty(&instr->def.uses);
	
   ESDOp op =
      read_result ? get_opcode(instr->intrinsic) : get_opcode_wo(instr->intrinsic);

   if (DS_OP_INVALID == op)
      return false;

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += nir_intrinsic_base(instr);

   auto dest = read_result ? vf.dest(instr->def, 0, pin_free) : nullptr;

   PRegister src_as_register = nullptr;
   auto src_val = vf.src(instr->src[1], 0);
   if (!src_val->as_register()) {
      auto temp_src_val = vf.temp_register();
      shader.emit_instruction(
         new AluInstr(op1_mov, temp_src_val, src_val, AluInstr::last_write));
      src_as_register = temp_src_val;
   } else
      src_as_register = src_val->as_register();

   GDSInstr *ir = nullptr;
   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src(nullptr, src_as_register, nullptr, nullptr, pin_free);
      ir = new GDSInstr(op, dest, src, offset, uav_id);

   } else {
      auto dest = vf.dest(instr->def, 0, pin_free);
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], src_val, AluInstr::last_write));
      ir = new GDSInstr(op, dest, tmp, 0, nullptr);
   }
   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_read(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));

   auto dest = vf.dest(instr->def, 0, pin_free);

   GDSInstr *ir = nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src = RegisterVec4(0, true, {7, 7, 7, 7});
      ir = new GDSInstr(DS_OP_READ_RET, dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 7, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      ir = new GDSInstr(DS_OP_READ_RET, dest, tmp, 0, nullptr);
   }

   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_inc(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   bool read_result = !list_is_empty(&instr->def.uses);

   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));

   GDSInstr *ir = nullptr;
   auto dest = read_result ? vf.dest(instr->def, 0, pin_free) : nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
            RegisterVec4 src(nullptr, shader.atomic_update(), nullptr, nullptr, pin_chan);
      ir =
         new GDSInstr(read_result ? DS_OP_ADD_RET : DS_OP_ADD, dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});

      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], shader.atomic_update(), AluInstr::last_write));
      ir = new GDSInstr(read_result ? DS_OP_ADD_RET : DS_OP_ADD, dest, tmp, 0, nullptr);
   }
   shader.emit_instruction(ir);
   return true;
}

bool
GDSInstr::emit_atomic_pre_dec(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();

   bool read_result = !list_is_empty(&instr->def.uses);

   auto opcode = read_result ? DS_OP_SUB_RET : DS_OP_SUB;
	
   auto [offset, uav_id] = shader.evaluate_resource_offset(instr, 0);
   {
   }
   offset += shader.remap_atomic_base(nir_intrinsic_base(instr));


   auto *tmp_dest = read_result ? vf.temp_register() : nullptr;

   GDSInstr *ir = nullptr;

   if (shader.chip_class() < ISA_CC_CAYMAN) {
      RegisterVec4 src(nullptr, shader.atomic_update(), nullptr, nullptr, pin_chan);
      ir = new GDSInstr(opcode, tmp_dest, src, offset, uav_id);
   } else {
      auto tmp = vf.temp_vec4(pin_group, {0, 1, 7, 7});
      if (uav_id)
         shader.emit_instruction(new AluInstr(op3_muladd_uint24,
                                              tmp[0],
                                              uav_id,
                                              vf.literal(4),
                                              vf.literal(4 * offset),
                                              AluInstr::write));
      else
         shader.emit_instruction(
            new AluInstr(op1_mov, tmp[0], vf.literal(4 * offset), AluInstr::write));

      shader.emit_instruction(
         new AluInstr(op1_mov, tmp[1], shader.atomic_update(), AluInstr::last_write));
      ir = new GDSInstr(opcode, tmp_dest, tmp, 0, nullptr);
   }

   shader.emit_instruction(ir);
   if (read_result)
      shader.emit_instruction(new AluInstr(op2_sub_int,
                                           vf.dest(instr->def, 0, pin_free),
                                           tmp_dest,
                                           vf.one_i(),
                                           AluInstr::last_write));
   return true;
}

void GDSInstr::update_indirect_addr(PRegister old_reg, PRegister addr)
{
   (void)old_reg;
   set_resource_offset(addr);
}

RatInstr::RatInstr(ECFOpCode cf_opcode,
                   ERatOp rat_op,
                   const RegisterVec4& data,
                   const RegisterVec4& index,
                   int rat_id,
                   PRegister rat_id_offset,
                   int burst_count,
                   int comp_mask,
                   int element_size):
    Resource(this, rat_id, rat_id_offset),
    m_cf_opcode(cf_opcode),
    m_rat_op(rat_op),
    m_data(data),
    m_index(index),
    m_burst_count(burst_count),
    m_comp_mask(comp_mask),
    m_element_size(element_size)
{
   set_always_keep();
   m_data.add_use(this);
   m_index.add_use(this);
}

void
RatInstr::accept(ConstInstrVisitor& visitor) const
{
   visitor.visit(*this);
}

void
RatInstr::accept(InstrVisitor& visitor)
{
   visitor.visit(this);
}

bool
RatInstr::is_equal_to(const RatInstr& lhs) const
{
   (void)lhs;
   assert(0);
   return false;
}

bool
RatInstr::do_ready() const
{
   if (m_rat_op != STORE_TYPED) {
      for (auto i : required_instr()) {
         if (!i->is_scheduled() && !i->is_dead()) {
            return false;
         }
      }
   }

   /* The resource offset is the index register the RAT id is taken from, so the ALU that
    * loads it has to be scheduled first -- exactly as for GDS, fetch and tex instructions.
    */
   return m_data.ready(block_id(), index()) && m_index.ready(block_id(), index()) &&
          resource_ready(block_id(), index());
}

void
RatInstr::do_print(std::ostream& os) const
{
   os << "MEM_RAT RAT " << resource_id();
   print_resource_offset(os);
   os << " @" << m_index;
   os << " OP:" << m_rat_op << " " << m_data;
   os << " BC:" << m_burst_count << " MASK:" << m_comp_mask << " ES:" << m_element_size;
   if (m_need_ack)
      os << " ACK";
}

void RatInstr::update_indirect_addr(UNUSED PRegister old_reg, PRegister addr)
{
   set_resource_offset(addr);
}

static RatInstr::ERatOp
get_rat_opcode(const nir_atomic_op opcode)
{
   switch (opcode) {
   case nir_atomic_op_iadd:
      return RatInstr::ADD_RTN;
   case nir_atomic_op_iand:
      return RatInstr::AND_RTN;
   case nir_atomic_op_ior:
      return RatInstr::OR_RTN;
   case nir_atomic_op_imin:
      return RatInstr::MIN_INT_RTN;
   case nir_atomic_op_imax:
      return RatInstr::MAX_INT_RTN;
   case nir_atomic_op_umin:
      return RatInstr::MIN_UINT_RTN;
   case nir_atomic_op_umax:
      return RatInstr::MAX_UINT_RTN;
   case nir_atomic_op_ixor:
      return RatInstr::XOR_RTN;
   case nir_atomic_op_cmpxchg:
      return RatInstr::CMPXCHG_INT_RTN;
   case nir_atomic_op_xchg:
      return RatInstr::XCHG_RTN;
   case nir_atomic_op_inc_wrap:
      return RatInstr::WRAP_INC_RTN;
   case nir_atomic_op_dec_wrap:
      return RatInstr::WRAP_DEC_RTN;
   default:
      unreachable("Unsupported atomic");
   }
}

static RatInstr::ERatOp
get_rat_opcode_wo(const nir_atomic_op opcode)
{
   switch (opcode) {
   case nir_atomic_op_iadd:
      return RatInstr::ADD;
   case nir_atomic_op_iand:
      return RatInstr::AND;
   case nir_atomic_op_ior:
      return RatInstr::OR;
   case nir_atomic_op_imin:
      return RatInstr::MIN_INT;
   case nir_atomic_op_imax:
      return RatInstr::MAX_INT;
   case nir_atomic_op_umin:
      return RatInstr::MIN_UINT;
   case nir_atomic_op_umax:
      return RatInstr::MAX_UINT;
   case nir_atomic_op_ixor:
      return RatInstr::XOR;
   case nir_atomic_op_cmpxchg:
      return RatInstr::CMPXCHG_INT;
   case nir_atomic_op_xchg:
      return RatInstr::XCHG_RTN;
   default:
      unreachable("Unsupported atomic");
   }
}

bool
RatInstr::emit(nir_intrinsic_instr *intr, Shader& shader)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_uav_instr_r600:
   case nir_intrinsic_uav_returning_instr_r600:
      return emit_uav_instr(intr, shader);
   case nir_intrinsic_load_ssbo:
      return emit_ssbo_load(intr, shader);
   case nir_intrinsic_store_ssbo:
      return emit_ssbo_store(intr, shader);
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
      return emit_ssbo_atomic_op(intr, shader);
   case nir_intrinsic_store_global:
      return emit_global_store(intr, shader);
   case nir_intrinsic_image_store:
      return emit_image_store(intr, shader);
   case nir_intrinsic_image_load:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
      return emit_image_load_or_atomic(intr, shader);
   case nir_intrinsic_image_size:
      return emit_image_size(intr, shader);
   case nir_intrinsic_image_samples:
      return emit_image_samples(intr, shader);
   case nir_intrinsic_get_ssbo_size:
      return emit_ssbo_size(intr, shader);
   case nir_intrinsic_load_buffer_size_r600:
      return emit_buffer_size_r600(intr, shader);
   default:
      return false;
   }
}

bool
RatInstr::emit_uav_instr(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();

   const RegisterVec4::Swizzle num_components_swizzles[] = {
      {0, 7, 7, 7},
      {0, 1, 7, 7},
      {0, 1, 2, 7},
      {0, 1, 2, 3},
   };

   /* Coordinate vector. */

   unsigned coord_num_components = intrin->src[1].ssa->num_components;
   auto coord = vf.temp_vec4(pin_chgr, num_components_swizzles[coord_num_components - 1]);
   for (unsigned i = 0; i < coord_num_components; ++i) {
      shader.emit_instruction(new AluInstr(op1_mov,
                                           coord[i],
                                           vf.src(intrin->src[1], i),
                                           i == coord_num_components - 1
                                              ? AluInstr::last_write
                                              : AluInstr::write));
   }

   /* Value vector. */

   auto op = static_cast<ERatOp>(nir_intrinsic_uav_op_r600(intrin));

   bool op_returns = (static_cast<unsigned>(op) & 0x20) != 0;
   assert(intrin->intrinsic == (op_returns ? nir_intrinsic_uav_returning_instr_r600
                                           : nir_intrinsic_uav_instr_r600));

   PVirtualValue return_value_index = op_returns ? vf.src(intrin->src[4], 0) : nullptr;

   unsigned value_component_mask;
   RegisterVec4 value;

   if (op == ERatOp::STORE_TYPED) {
      unsigned value_num_components = intrin->src[2].ssa->num_components;
      assert(value_num_components <= 4);
      value_component_mask = BITFIELD_MASK(value_num_components);
      value = vf.temp_vec4(pin_chgr, num_components_swizzles[value_num_components - 1]);
      for (unsigned i = 0; i < value_num_components; ++i) {
         shader.emit_instruction(new AluInstr(op1_mov,
                                              value[i],
                                              vf.src(intrin->src[2], i),
                                              i == value_num_components - 1
                                                 ? AluInstr::last_write
                                                 : AluInstr::write));
      }
   } else {
      unsigned compare_value_component_index =
         shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3;

      value_component_mask = 0b0;

      /* NOP without a return value isn't useful, and assuming that every UAV
       * instruction has at least one value component for simplicity.
       */
      assert(op != ERatOp::NOP);

      auto op_non_rtn = static_cast<ERatOp>(static_cast<unsigned>(op) & 0x1F);
      if (op_non_rtn != ERatOp::NOP) {
         value_component_mask |= BITFIELD_BIT(0);
         if (op_non_rtn == ERatOp::CMPXCHG_INT || op_non_rtn == ERatOp::CMPXCHG_FLT ||
             op_non_rtn == ERatOp::CMPXCHG_FDENORM) {
            value_component_mask |= BITFIELD_BIT(compare_value_component_index);
         }
      }

      /* Return value index in the IMMED buffer. */
      if (op_returns) {
         value_component_mask |= BITFIELD_BIT(1);
      }

      assert(value_component_mask != 0b0);

      RegisterVec4::Swizzle value_swizzle{7, 7, 7, 7};
      u_foreach_bit (value_component, value_component_mask) {
         value_swizzle[value_component] = value_component;
      }
      value = vf.temp_vec4(pin_chgr, value_swizzle);

      unsigned value_last_component_index = util_last_bit(value_component_mask) - 1;

      /* Value. */
      if (value_component_mask & BITFIELD_BIT(0)) {
         shader.emit_instruction(new AluInstr(op1_mov,
                                              value[0],
                                              vf.src(intrin->src[2], 0),
                                              value_last_component_index == 0
                                                 ? AluInstr::last_write
                                                 : AluInstr::write));
      }
      /* Return value index in the IMMED buffer. */
      if (value_component_mask & BITFIELD_BIT(1)) {
         shader.emit_instruction(new AluInstr(op1_mov,
                                              value[1],
                                              return_value_index,
                                              value_last_component_index == 1
                                                 ? AluInstr::last_write
                                                 : AluInstr::write));
      }
      /* Value to compare to. */
      if (value_component_mask & BITFIELD_BIT(compare_value_component_index)) {
         shader.emit_instruction(new AluInstr(op1_mov,
                                              value[compare_value_component_index],
                                              vf.src(intrin->src[3], 0),
                                              value_last_component_index ==
                                                    compare_value_component_index
                                                 ? AluInstr::last_write
                                                 : AluInstr::write));
      }
   }

   /* UAV index. */

   unsigned uav_base = nir_intrinsic_id_base(intrin);
   unsigned immed_base = op_returns ? nir_intrinsic_uav_return_id_base_r600(intrin) : 0;
   PRegister uav_offset = nullptr;
   const nir_const_value *uav_offset_const = nir_src_as_const_value(intrin->src[0]);
   if (uav_offset_const != nullptr) {
      uav_base += uav_offset_const->u32;
      immed_base += uav_offset_const->u32;
   } else {
      uav_offset = vf.src(intrin->src[0], 0)->as_register();
   }

   auto uav_instr = new RatInstr(cf_mem_rat,
                                 static_cast<ERatOp>(nir_intrinsic_uav_op_r600(intrin)),
                                 value,
                                 coord,
                                 uav_base,
                                 uav_offset,
                                 1,
                                 value_component_mask,
                                 0);
   if (op_returns) {
      /* Only returning UAV operations need an acknowledgement so their
       * result can be fetched from the immediate-return buffer. Plain
       * stores are completed by the command-stream cache/stage barrier;
       * marking every scalar STORE_TYPED as acknowledged inserts a
       * RAT/WAIT_ACK chain and can deadlock Evergreen compute.
       */
      uav_instr->set_ack();
      uav_instr->set_instr_flag(Instr::ack_rat_return_write);
   }
   if (nir_intrinsic_access(intrin) & ACCESS_INCLUDE_HELPERS) {
      uav_instr->set_instr_flag(Instr::helper);
   }
   shader.emit_instruction(uav_instr);

   if (op_returns) {
      assert(intrin->def.num_components <= 4);

      auto fetch_instr =
         new FetchInstr(vc_fetch,
                        vf.dest_vec4(intrin->def, pin_group),
                        num_components_swizzles[intrin->def.num_components - 1],
                        return_value_index->as_register(),
                        0,
                        no_index_offset,
                        fmt_invalid,
                        vtx_nf_norm,
                        vtx_es_none,
                        immed_base,
                        uav_offset);
      fetch_instr->set_fetch_flag(FetchInstr::use_const_field);
      fetch_instr->set_fetch_flag(FetchInstr::use_tc);
      fetch_instr->set_fetch_flag(FetchInstr::uncached);
      fetch_instr->set_fetch_flag(FetchInstr::wait_ack);

      unsigned mega_fetch_count = nir_intrinsic_mega_fetch_count_r600(intrin);
      fetch_instr->set_mfc(
         (mega_fetch_count != 0 ? CLAMP(mega_fetch_count, 1u, 64u) : 4) - 1);

      if (nir_intrinsic_access(intrin) & ACCESS_INCLUDE_HELPERS) {
         fetch_instr->set_instr_flag(Instr::helper);
      }

      fetch_instr->add_required_instr(uav_instr);
      shader.chain_ssbo_read(fetch_instr);

      shader.emit_instruction(fetch_instr);
   }

   return true;
}

bool
RatInstr::emit_ssbo_load(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto dest = vf.dest_vec4(intr->def, pin_group);

   /** src0 not used, should be some offset */
   auto addr = vf.src(intr->src[1], 0);
   auto addr_temp = vf.temp_register();

   /** Should be lowered in nir */
   shader.emit_instruction(new AluInstr(
      op2_lshr_int, addr_temp, addr, vf.literal(2), {alu_write, alu_last_instr}));

   const EVTXDataFormat formats[4] = {fmt_32, fmt_32_32, fmt_32_32_32, fmt_32_32_32_32};

   RegisterVec4::Swizzle dest_swz[4] = {
      {0, 7, 7, 7},
      {0, 1, 7, 7},
      {0, 1, 2, 7},
      {0, 1, 2, 3}
   };

   int comp_idx = intr->def.num_components - 1;

   auto [offset, res_offset] = shader.evaluate_resource_offset(intr, 0);
   {
   }

   auto res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + offset + shader.ssbo_image_offset();

   auto ir = new LoadFromBuffer(
      dest, dest_swz[comp_idx], addr_temp, 0, res_id, res_offset, formats[comp_idx]);
   ir->set_fetch_flag(FetchInstr::use_tc);
   ir->set_num_format(vtx_nf_int);

   shader.emit_instruction(ir);
   return true;
}

bool
RatInstr::emit_global_store(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto addr_orig = vf.src(intr->src[1], 0);
   auto addr_vec = vf.temp_vec4(pin_chan, {0, 7, 7, 7});

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, addr_vec[0], addr_orig, vf.literal(2),
                   AluInstr::last_write));

   RegisterVec4::Swizzle value_swz = {0,7,7,7};
   auto mask = nir_intrinsic_write_mask(intr);
   for (int i = 0; i < 4; ++i) {
      if (mask & (1 << i))
         value_swz[i] = i;
   }

   auto value_vec = vf.temp_vec4(pin_chgr, value_swz);

   AluInstr *ir = nullptr;
   for (int i = 0; i < 4; ++i) {
      if (value_swz[i] < 4) {
         ir = new AluInstr(op1_mov, value_vec[i],
                           vf.src(intr->src[0], i), AluInstr::write);
         shader.emit_instruction(ir);
      }
   }
   if (ir)
      ir->set_alu_flag(alu_last_instr);

   auto store = new RatInstr(cf_mem_rat_cacheless,
                             RatInstr::STORE_RAW,
                             value_vec,
                             addr_vec,
                             shader.ssbo_image_offset(),
                             nullptr,
                             1,
                             mask,
                             0);
   shader.emit_instruction(store);
   return true;
}

bool
RatInstr::emit_ssbo_store(nir_intrinsic_instr *instr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto orig_addr = vf.src(instr->src[2], 0);

   auto addr_base = vf.temp_register();

   auto [offset, rat_id] = shader.evaluate_resource_offset(instr, 1);

   /* R700 has no RAT storage path. The documented alternative is one
    * SX_MEMORY_EXPORT aperture. Do not silently reinterpret a dynamic or
    * non-zero descriptor selection as that aperture: configuring which BO
    * owns it is a driver-side operation and is not implemented yet. */
   bool const r700_mem_export = shader.chip_class() == ISA_CC_R700;
   if (r700_mem_export &&
       (offset != 0 || rat_id != nullptr || shader.ssbo_image_offset() != 0))
      return false;

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, addr_base, orig_addr, vf.literal(2), AluInstr::write));

   for (unsigned i = 0; i < nir_src_num_components(instr->src[0]); ++i) {
      auto addr_vec = vf.temp_vec4(pin_group, {0, 1, 2, 7});
      if (i == 0) {
         shader.emit_instruction(
            new AluInstr(op1_mov, addr_vec[0], addr_base, AluInstr::last_write));
      } else {
         shader.emit_instruction(new AluInstr(
            op2_add_int, addr_vec[0], addr_base, vf.literal(i), AluInstr::last_write));
      }
      auto value = vf.src(instr->src[0], i);
      PRegister v = vf.temp_register(0);
      shader.emit_instruction(new AluInstr(op1_mov, v, value, AluInstr::last_write));
      auto value_vec = RegisterVec4(v, nullptr, nullptr, nullptr, pin_chan);
      if (r700_mem_export) {
         /* The index is in DWORDs. The individual-component loop gives each
          * scalar a distinct address and avoids claiming unproven vector
          * layout semantics. The enclosing driver must still program the
          * sole SX aperture before this bytecode is executable. */
         shader.emit_instruction(new MemRingOutInstr(cf_mem_export,
                                                     MemRingOutInstr::mem_write_ind,
                                                     value_vec,
                                                     0,
                                                     1,
                                                     addr_vec[0]));
      } else {
         auto store = new RatInstr(cf_mem_rat,
                                   RatInstr::STORE_TYPED,
                                   value_vec,
                                   addr_vec,
                                   offset + shader.ssbo_image_offset(),
                                   rat_id,
                                   1,
                                   1,
                                   0);
         shader.emit_instruction(store);
      }
   }

   return true;
}

bool
RatInstr::emit_ssbo_atomic_op(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intr, 0);
   {
   }

   bool read_result = !list_is_empty(&intr->def.uses);
   auto opcode = read_result ? get_rat_opcode(nir_intrinsic_atomic_op(intr))
                             : get_rat_opcode_wo(nir_intrinsic_atomic_op(intr));

   auto coord_orig = vf.src(intr->src[1], 0);
   auto coord = vf.temp_register(0);

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});

   shader.emit_instruction(
      new AluInstr(op2_lshr_int, coord, coord_orig, vf.literal(2), AluInstr::last_write));

   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   if (intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intr->src[3], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intr->src[2], 0),
                      {alu_last_instr, alu_write}));
   } else {
      shader.emit_instruction(new AluInstr(
         op1_mov, data_vec4[0], vf.src(intr->src[2], 0), AluInstr::last_write));
   }

   RegisterVec4 out_vec(coord, coord, coord, coord, pin_chgr);

   auto atomic = new RatInstr(cf_mem_rat,
                              opcode,
                              data_vec4,
                              out_vec,
                              imageid + shader.ssbo_image_offset(),
                              image_offset,
                              1,
                              0xf,
                              0);
   shader.emit_instruction(atomic);

   atomic->set_ack();
   if (read_result) {
      atomic->set_instr_flag(ack_rat_return_write);
      auto dest = vf.dest_vec4(intr->def, pin_group);

      auto fetch = new FetchInstr(vc_fetch,
                                  dest,
                                  {0, 1, 2, 3},
                                  shader.rat_return_address(),
                                  0,
                                  no_index_offset,
                                  fmt_32,
                                  vtx_nf_int,
                                  vtx_es_none,
                                  R600_IMAGE_IMMED_RESOURCE_OFFSET + imageid,
                                  image_offset);
      fetch->set_mfc(15);
      fetch->set_fetch_flag(FetchInstr::srf_mode);
      fetch->set_fetch_flag(FetchInstr::use_tc);
      fetch->set_fetch_flag(FetchInstr::vpm);
      fetch->set_fetch_flag(FetchInstr::wait_ack);
      fetch->add_required_instr(atomic);
      shader.chain_ssbo_read(fetch);
      shader.emit_instruction(fetch);
   }

   return true;
}

bool
RatInstr::emit_ssbo_size(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto dest = vf.dest_vec4(intr->def, pin_group);

   auto const_offset = nir_src_as_const_value(intr->src[0]);
   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET;
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      assert(0 && "dynamic buffer offset not supported in buffer_size");

   shader.emit_instruction(new QueryBufferSizeInstr(dest, {0, 1, 2, 3}, res_id));
   return true;
}

bool
RatInstr::emit_buffer_size_r600(nir_intrinsic_instr *intr, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto dest = vf.dest_vec4(intr->def, pin_group);
   auto [offset, resource_offset] = shader.evaluate_resource_offset(intr, 0);
   int const res_id = nir_intrinsic_id_base(intr) + offset;

   shader.emit_instruction(
      new QueryBufferSizeInstr(dest, {0, 1, 2, 3}, res_id, resource_offset));
   return true;
}

bool
RatInstr::emit_image_store(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intrin, 0);
   {
   }

   auto coord_load = vf.src_vec4(intrin->src[1], pin_chan);
   auto coord = vf.temp_vec4(pin_chgr);

   auto value_load = vf.src_vec4(intrin->src[3], pin_chan);
   auto value = vf.temp_vec4(pin_chgr);

   RegisterVec4::Swizzle swizzle = {0, 1, 2, 3};
   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_1D &&
       nir_intrinsic_image_array(intrin))
      swizzle = {0, 2, 1, 3};

   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::last_write;
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[swizzle[i]], coord_load[i], flags));
   }
   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::last_write;
      shader.emit_instruction(new AluInstr(op1_mov, value[i], value_load[i], flags));
   }

   auto op = cf_mem_rat; // nir_intrinsic_access(intrin) & ACCESS_COHERENT ?
                         // cf_mem_rat_cacheless : cf_mem_rat;
   auto store = new RatInstr(
      op, RatInstr::STORE_TYPED, value, coord, imageid, image_offset, 1, 0xf, 0);

   store->set_ack();
   if (nir_intrinsic_access(intrin) & ACCESS_INCLUDE_HELPERS)
      store->set_instr_flag(Instr::helper);

   shader.emit_instruction(store);
   return true;
}

bool
RatInstr::emit_image_load_or_atomic(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();
   auto [imageid, image_offset] = shader.evaluate_resource_offset(intrin, 0);
   {
   }

   bool read_result = !list_is_empty(&intrin->def.uses);
   bool image_load = (intrin->intrinsic == nir_intrinsic_image_load);
   auto opcode = image_load  ? RatInstr::NOP_RTN :
                 read_result ? get_rat_opcode(nir_intrinsic_atomic_op(intrin))
                             : get_rat_opcode_wo(nir_intrinsic_atomic_op(intrin));

   auto coord_orig = vf.src_vec4(intrin->src[1], pin_chan);
   auto coord = vf.temp_vec4(pin_chgr);

   auto data_vec4 = vf.temp_vec4(pin_chgr, {0, 1, 2, 3});

   RegisterVec4::Swizzle swizzle = {0, 1, 2, 3};
   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_1D &&
       nir_intrinsic_image_array(intrin))
      swizzle = {0, 2, 1, 3};

   for (int i = 0; i < 4; ++i) {
      auto flags = i != 3 ? AluInstr::write : AluInstr::last_write;
      shader.emit_instruction(
         new AluInstr(op1_mov, coord[swizzle[i]], coord_orig[i], flags));
   }

   shader.emit_instruction(
      new AluInstr(op1_mov, data_vec4[1], shader.rat_return_address(), AluInstr::write));

   if (intrin->intrinsic == nir_intrinsic_image_atomic_swap) {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intrin->src[4], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov,
                      data_vec4[shader.chip_class() == ISA_CC_CAYMAN ? 2 : 3],
                      vf.src(intrin->src[3], 0),
                      AluInstr::last_write));
   } else {
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[0], vf.src(intrin->src[3], 0), AluInstr::write));
      shader.emit_instruction(
         new AluInstr(op1_mov, data_vec4[2], vf.zero(), AluInstr::last_write));
   }

   auto atomic =
      new RatInstr(cf_mem_rat, opcode, data_vec4, coord, imageid, image_offset, 1, 0xf, 0);
   shader.emit_instruction(atomic);

   atomic->set_ack();
   if (read_result) {
      atomic->set_instr_flag(ack_rat_return_write);
      auto dest = vf.dest_vec4(intrin->def, pin_group);

      pipe_format format = nir_intrinsic_format(intrin);
      unsigned fmt = fmt_32;
      unsigned num_format = 0;
      unsigned format_comp = 0;
      unsigned endian = 0;
      r600_vertex_data_type(format, &fmt, &num_format, &format_comp, &endian);

      auto fetch = new FetchInstr(vc_fetch,
                                  dest,
                                  {0, 1, 2, 3},
                                  shader.rat_return_address(),
                                  0,
                                  no_index_offset,
                                  (EVTXDataFormat)fmt,
                                  (EVFetchNumFormat)num_format,
                                  (EVFetchEndianSwap)endian,
                                  R600_IMAGE_IMMED_RESOURCE_OFFSET + imageid,
                                  image_offset);
      fetch->set_mfc(3);
      fetch->set_fetch_flag(FetchInstr::use_tc);
      fetch->set_fetch_flag(FetchInstr::vpm);
      fetch->set_fetch_flag(FetchInstr::wait_ack);
      if (format_comp)
         fetch->set_fetch_flag(FetchInstr::format_comp_signed);

      shader.chain_ssbo_read(fetch);
      shader.emit_instruction(fetch);
   }

   return true;
}

#define R600_SHADER_BUFFER_INFO_SEL (512 + R600_BUFFER_INFO_OFFSET / 16)

bool
RatInstr::emit_image_size(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto src = RegisterVec4(0, true, {4, 4, 4, 4});

   assert(nir_src_as_uint(intrin->src[1]) == 0);

   auto const_offset = nir_src_as_const_value(intrin->src[0]);
   PRegister dyn_offset = nullptr;

   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + nir_intrinsic_range_base(intrin);
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      dyn_offset = shader.emit_load_to_register(vf.src(intrin->src[0], 0));

   if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_BUF) {
      auto dest = vf.dest_vec4(intrin->def, pin_group);
      shader.emit_instruction(new QueryBufferSizeInstr(dest, {0, 1, 2, 3}, res_id));
      return true;
   } else {

      if (nir_intrinsic_image_dim(intrin) == GLSL_SAMPLER_DIM_CUBE &&
          nir_intrinsic_image_array(intrin) &&
          intrin->def.num_components > 2) {
         /* Need to load the layers from a const buffer */

         auto dest = vf.dest_vec4(intrin->def, pin_group);
         shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                              dest,
                                              {0, 1, 7, 3},
                                              src,
                                              res_id,
                                              dyn_offset));

         shader.set_flag(Shader::sh_txs_cube_array_comp);

         if (const_offset) {
            unsigned lookup_resid = (res_id - R600_IMAGE_REAL_RESOURCE_OFFSET) +
                                    shader.image_size_const_offset();
            shader.emit_instruction(
               new AluInstr(op1_mov,
                            dest[2],
                            vf.uniform(lookup_resid / 4 + R600_SHADER_BUFFER_INFO_SEL,
                                       lookup_resid % 4,
                                       R600_BUFFER_INFO_CONST_BUFFER),
                            AluInstr::last_write));
         } else {
            /* If the addressing is indirect we have to get the z-value by
             * using a binary search */
            auto addr = vf.temp_register();
            auto comp1 = vf.temp_register();
            auto comp2 = vf.temp_register();
            auto low_bit = vf.temp_register();
            auto high_bit = vf.temp_register();

            auto trgt = vf.temp_vec4(pin_group);

            shader.emit_instruction(new AluInstr(op2_lshr_int,
                                                 addr,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.literal(2),
                                                 AluInstr::write));
            shader.emit_instruction(new AluInstr(op2_and_int,
                                                 low_bit,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.one_i(),
                                                 AluInstr::write));
            shader.emit_instruction(new AluInstr(op2_and_int,
                                                 high_bit,
                                                 vf.src(intrin->src[0], 0),
                                                 vf.literal(2),
                                                 AluInstr::last_write));

            shader.emit_instruction(new LoadFromBuffer(trgt,
                                                       {0, 1, 2, 3},
                                                       addr,
                                                       R600_SHADER_BUFFER_INFO_SEL,
                                                       R600_BUFFER_INFO_CONST_BUFFER,
                                                       nullptr,
                                                       fmt_32_32_32_32_float));

            // this may be wrong
            shader.emit_instruction(new AluInstr(
               op3_cnde_int, comp1, high_bit, trgt[0], trgt[2], AluInstr::write));
            shader.emit_instruction(new AluInstr(
               op3_cnde_int, comp2, high_bit, trgt[1], trgt[3], AluInstr::last_write));
            shader.emit_instruction(new AluInstr(
               op3_cnde_int, dest[2], low_bit, comp1, comp2, AluInstr::last_write));
         }
      } else {
         auto dest = vf.dest_vec4(intrin->def, pin_group);
         shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                              dest,
                                              {0, 1, 2, 3},
                                              src,
                                              res_id,
                                              dyn_offset));
      }
   }
   return true;
}

bool
RatInstr::emit_image_samples(nir_intrinsic_instr *intrin, Shader& shader)
{
   auto& vf = shader.value_factory();

   auto src = RegisterVec4(0, true, {4, 4, 4, 4});

   auto tmp =  shader.value_factory().temp_vec4(pin_group);
   auto dest =  shader.value_factory().dest(intrin->def, 0, pin_free);

   auto const_offset = nir_src_as_const_value(intrin->src[0]);
   PRegister dyn_offset = nullptr;

   int res_id = R600_IMAGE_REAL_RESOURCE_OFFSET + nir_intrinsic_range_base(intrin);
   if (const_offset)
      res_id += const_offset[0].u32;
   else
      dyn_offset = shader.emit_load_to_register(vf.src(intrin->src[0], 0));

   shader.emit_instruction(new TexInstr(TexInstr::get_resinfo,
                                        tmp,
                                        {3, 7, 7, 7},
                                        src,
                                        res_id,
                                        dyn_offset));

   shader.emit_instruction(new AluInstr(op1_mov, dest, tmp[0], AluInstr::last_write));
   return true;
}

} // namespace r600
