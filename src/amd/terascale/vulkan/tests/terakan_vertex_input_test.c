/*
 * Copyright © 2026
 * SPDX-License-Identifier: MIT
 */

#include "terakan_vertex_input.h"

#include "gallium/drivers/r600/evergreend.h"
#include "util/macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define CANARY_BYTE 0xA5
#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

struct guarded_resource_usage {
   uint8_t before[32];
   struct terakan_vertex_input_fs_resource_usage value;
   uint8_t after[32];
};

struct guarded_code {
   uint8_t before[32];
   struct terakan_vertex_input_fs_code value;
   uint8_t after[32];
};

static uint32_t
float32_fetch_word1(void)
{
   struct terascale_format_info const format = {
      .format = TERASCALE_FORMAT_INDEX_32_FLOAT,
      .number_type = TERASCALE_FORMAT_NUMBER_TYPE_FLOAT,
      .swizzle_r = TERASCALE_SWIZZLE_X,
      .swizzle_g = TERASCALE_SWIZZLE_0,
      .swizzle_b = TERASCALE_SWIZZLE_0,
      .swizzle_a = TERASCALE_SWIZZLE_1,
      .supports_sq_vertex_fetch = true,
   };
   return terakan_vertex_input_format_fetch_word1(&format);
}

static uint32_t
float32x4_fetch_word1(void)
{
   struct terascale_format_info const format = {
      .format = TERASCALE_FORMAT_INDEX_32_32_32_32_FLOAT,
      .number_type = TERASCALE_FORMAT_NUMBER_TYPE_FLOAT,
      .swizzle_r = TERASCALE_SWIZZLE_X,
      .swizzle_g = TERASCALE_SWIZZLE_Y,
      .swizzle_b = TERASCALE_SWIZZLE_Z,
      .swizzle_a = TERASCALE_SWIZZLE_W,
      .supports_sq_vertex_fetch = true,
   };
   return terakan_vertex_input_format_fetch_word1(&format);
}

static void
check_canary(uint8_t const * bytes, size_t const size)
{
   for (size_t i = 0; i < size; ++i)
      CHECK(bytes[i] == CANARY_BYTE);
}

static void
create_checked(struct terakan_vertex_input_fs_layout const * layout,
               struct terakan_vertex_input_fs_resource_usage * usage_out,
               struct terakan_vertex_input_fs_code * code_out)
{
   struct guarded_resource_usage usage;
   struct guarded_code code;
   memset(&usage, CANARY_BYTE, sizeof(usage));
   memset(&code, CANARY_BYTE, sizeof(code));

   terakan_vertex_input_create_fs_code(layout, false, &usage.value, &code.value);

   check_canary(usage.before, sizeof(usage.before));
   check_canary(usage.after, sizeof(usage.after));
   check_canary(code.before, sizeof(code.before));
   check_canary(code.after, sizeof(code.after));
   *usage_out = usage.value;
   *code_out = code.value;
}

static void
test_empty(void)
{
   struct terakan_vertex_input_fs_layout const layout = {};
   struct terakan_vertex_input_fs_resource_usage usage;
   struct terakan_vertex_input_fs_code code;
   create_checked(&layout, &usage, &code);
   CHECK(usage.resources_used == 0);
   CHECK(code.fetch_count == 0);
   CHECK(terakan_vertex_input_fs_code_is_no_operation(&code));
}

static void
test_highest_indices(void)
{
   struct terakan_vertex_input_fs_layout layout = {
      .attributes_used = BITFIELD_BIT(31),
   };
   layout.attribute_format_fetch_word1[31] = float32_fetch_word1();
   layout.attribute_bindings[31] = 31;

   struct terakan_vertex_input_fs_resource_usage usage;
   struct terakan_vertex_input_fs_code code;
   create_checked(&layout, &usage, &code);
   CHECK(usage.resources_used == BITFIELD_BIT(31));
   CHECK(usage.resource_bindings_and_truncation[31] == 31);
   CHECK(code.fetch_count == 1);
   CHECK(G_SQ_VTX_WORD0_BUFFER_ID(code.fetch[0]) == 31);
}

static void
test_all_attributes_and_bindings(void)
{
   struct terakan_vertex_input_fs_layout layout = {
      .attributes_used = UINT32_MAX,
   };
   uint32_t const format = float32_fetch_word1();
   for (unsigned i = 0; i < ARRAY_SIZE(layout.attribute_bindings); ++i) {
      layout.attribute_format_fetch_word1[i] = format;
      layout.attribute_bindings[i] = i;
      layout.attribute_offsets[i] = i * sizeof(uint32_t);
   }

   struct terakan_vertex_input_fs_resource_usage usage;
   struct terakan_vertex_input_fs_code code;
   create_checked(&layout, &usage, &code);
   CHECK(usage.resources_used == UINT32_MAX);
   CHECK(code.fetch_count == 32);
   for (unsigned i = 0; i < 32; ++i)
      CHECK(usage.resource_bindings_and_truncation[i] == i);
}

static void
test_unbound_attribute(void)
{
   struct terakan_vertex_input_fs_layout const layout = {
      .attributes_used = BITFIELD_BIT(31),
      /* DATA_FORMAT = INVALID, DST_SEL = XXXX. */
      .attribute_format_fetch_word1[31] = 0,
   };
   struct terakan_vertex_input_fs_resource_usage usage;
   struct terakan_vertex_input_fs_code code;
   create_checked(&layout, &usage, &code);
   CHECK(usage.resources_used == 0);
   CHECK(code.fetch_count == 0);
   CHECK(code.pre_fetch_alu_qwords != 0);
}

static void
test_dynamic_binding_offset_truncation(void)
{
   struct terakan_vertex_input_fs_layout const layout = {
      .attributes_used = BITFIELD_BIT(0),
      .attribute_format_fetch_word1[0] = 0,
      .attribute_bindings[0] = 0,
      .attribute_offsets[0] = 0,
   };
   struct terakan_vertex_input_fs_layout mutable_layout = layout;
   mutable_layout.attribute_format_fetch_word1[0] = float32x4_fetch_word1();

   struct terakan_vertex_input_fs_resource_usage usage;
   struct terakan_vertex_input_fs_code code;
   create_checked(&mutable_layout, &usage, &code);
   CHECK((usage.resources_used & BITFIELD_BIT(0)) != 0);
   CHECK((usage.resource_bindings_and_truncation[0] >> 5) == 0);

   /* A 2-byte VkBuffer binding offset leaves only the first dword naturally aligned.  The
    * previous attribute-only calculation returned zero here, which left the final 12 bytes
    * outside the descriptor bound. */
   CHECK(terakan_vertex_input_fs_resource_truncation(&mutable_layout, &usage, 0, false, 2) == 3);
   CHECK(terakan_vertex_input_fs_resource_truncation(&mutable_layout, &usage, 0, false,
                                                     UINT64_C(0x1000)) == 0);
   CHECK(terakan_vertex_input_fs_resource_truncation(&mutable_layout, &usage, 0, true, 2) == 0);
}

int
main(void)
{
   test_empty();
   test_highest_indices();
   test_all_attributes_and_bindings();
   test_unbound_attribute();
   test_dynamic_binding_offset_truncation();
   return 0;
}
