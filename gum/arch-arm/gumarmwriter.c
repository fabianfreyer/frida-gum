/*
 * Copyright (C) 2010-2017 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumarmwriter.h"

#include "gumarmreg.h"
#include "gumlibc.h"
#include "gummemory.h"
#include "gumprocess.h"

#include <string.h>

#define GUM_MAX_LITERAL_REF_COUNT 100

struct _GumArmLiteralRef
{
  guint32 * insn;
  guint32 val;
};

GumArmWriter *
gum_arm_writer_new (gpointer code_address)
{
  GumArmWriter * writer;

  writer = g_slice_new (GumArmWriter);

  gum_arm_writer_init (writer, code_address);

  return writer;
}

GumArmWriter *
gum_arm_writer_ref (GumArmWriter * writer)
{
  g_atomic_int_inc (&writer->ref_count);

  return writer;
}

void
gum_arm_writer_unref (GumArmWriter * writer)
{
  if (g_atomic_int_dec_and_test (&writer->ref_count))
  {
    gum_arm_writer_clear (writer);

    g_slice_free (GumArmWriter, writer);
  }
}

void
gum_arm_writer_init (GumArmWriter * writer,
                     gpointer code_address)
{
  writer->ref_count = 1;

  writer->literal_refs = g_new (GumArmLiteralRef, GUM_MAX_LITERAL_REF_COUNT);

  gum_arm_writer_reset (writer, code_address);
}

void
gum_arm_writer_clear (GumArmWriter * writer)
{
  gum_arm_writer_flush (writer);

  g_free (writer->literal_refs);
}

void
gum_arm_writer_reset (GumArmWriter * writer,
                      gpointer code_address)
{
  writer->target_os = gum_process_get_native_os ();

  writer->base = code_address;
  writer->code = code_address;
  writer->pc = GUM_ADDRESS (code_address);

  writer->literal_refs_len = 0;
}

void
gum_arm_writer_set_target_os (GumArmWriter * self,
                              GumOS os)
{
  self->target_os = os;
}

gpointer
gum_arm_writer_cur (GumArmWriter * self)
{
  return self->code;
}

guint
gum_arm_writer_offset (GumArmWriter * self)
{
  return (guint) (self->code - self->base) * sizeof (guint32);
}

void
gum_arm_writer_skip (GumArmWriter * self,
                     guint n_bytes)
{
  self->code = (guint32 *) (((guint8 *) self->code) + n_bytes);
  self->pc += n_bytes;
}

gboolean
gum_arm_writer_flush (GumArmWriter * self)
{
  if (self->literal_refs_len > 0)
  {
    guint32 * first_slot, * last_slot;
    guint ref_idx;

    first_slot = self->code;
    last_slot = first_slot;

    for (ref_idx = 0; ref_idx != self->literal_refs_len; ref_idx++)
    {
      GumArmLiteralRef * r;
      guint32 * cur_slot;
      gint64 distance_in_words;
      guint32 insn;

      r = &self->literal_refs[ref_idx];

      for (cur_slot = first_slot; cur_slot != last_slot; cur_slot++)
      {
        if (*cur_slot == r->val)
          break;
      }

      if (cur_slot == last_slot)
      {
        *cur_slot = r->val;
        last_slot++;
      }

      distance_in_words = cur_slot - (r->insn + 2);

      insn = GUINT32_FROM_LE (*r->insn);
      insn |= ABS (distance_in_words) * 4;
      if (distance_in_words >= 0)
        insn |= 1 << 23;
      *r->insn = GUINT32_TO_LE (insn);
    }
    self->literal_refs_len = 0;

    self->code = last_slot;
    self->pc += (guint8 *) last_slot - (guint8 *) first_slot;
  }

  return TRUE;
}

static gboolean
gum_arm_writer_add_literal_reference_here (GumArmWriter * self,
                                           guint32 val)
{
  GumArmLiteralRef * r;

  if (self->literal_refs_len == GUM_MAX_LITERAL_REF_COUNT)
    return FALSE;

  r = &self->literal_refs[self->literal_refs_len++];
  r->insn = self->code;
  r->val = val;

  return TRUE;
}

gboolean
gum_arm_writer_put_b_imm (GumArmWriter * self,
                          GumAddress target)
{
  gint32 distance_in_bytes, distance_in_words;

  distance_in_bytes = target - (self->pc + 8);
  if (!GUM_IS_WITHIN_INT26_RANGE (distance_in_bytes))
    return FALSE;

  distance_in_words = distance_in_bytes / 4;

  gum_arm_writer_put_instruction (self, 0xea000000 |
      (distance_in_words & GUM_INT24_MASK));

  return TRUE;
}

gboolean
gum_arm_writer_put_ldr_reg_address (GumArmWriter * self,
                                    arm_reg reg,
                                    GumAddress address)
{
  return gum_arm_writer_put_ldr_reg_u32 (self, reg, (guint32) address);
}

gboolean
gum_arm_writer_put_ldr_reg_u32 (GumArmWriter * self,
                                arm_reg reg,
                                guint32 val)
{
  GumArmRegInfo ri;

  gum_arm_reg_describe (reg, &ri);

  if (!gum_arm_writer_add_literal_reference_here (self, val))
    return FALSE;
  gum_arm_writer_put_instruction (self, 0xe51f0000 | (ri.index << 12));

  return TRUE;
}

void
gum_arm_writer_put_add_reg_reg_imm (GumArmWriter * self,
                                    arm_reg dst_reg,
                                    arm_reg src_reg,
                                    guint32 imm_val)
{
  GumArmRegInfo rd, rs;

  gum_arm_reg_describe (dst_reg, &rd);
  gum_arm_reg_describe (src_reg, &rs);

  gum_arm_writer_put_instruction (self, 0xe2800000 | rd.index << 12 |
      rs.index << 16 | (imm_val & GUM_INT12_MASK));
}

void
gum_arm_writer_put_ldr_reg_reg_imm (GumArmWriter * self,
                                    arm_reg dst_reg,
                                    arm_reg src_reg,
                                    guint32 imm_val)
{
  GumArmRegInfo rd, rs;

  gum_arm_reg_describe (dst_reg, &rd);
  gum_arm_reg_describe (src_reg, &rs);

  gum_arm_writer_put_instruction (self, 0xe5900000 | rd.index << 12 |
      rs.index << 16 | (imm_val & GUM_INT12_MASK));
}

void
gum_arm_writer_put_nop (GumArmWriter * self)
{
  gum_arm_writer_put_instruction (self, 0xe1a00000);
}

void
gum_arm_writer_put_breakpoint (GumArmWriter * self)
{
  switch (self->target_os)
  {
    case GUM_OS_LINUX:
    case GUM_OS_ANDROID:
    default: /* TODO: handle other OSes */
      gum_arm_writer_put_instruction (self, 0xe7f001f0);
      break;
  }
}

void
gum_arm_writer_put_instruction (GumArmWriter * self,
                                guint32 insn)
{
  *self->code++ = GUINT32_TO_LE (insn);
  self->pc += 4;
}

gboolean
gum_arm_writer_put_bytes (GumArmWriter * self,
                          const guint8 * data,
                          guint n)
{
  if (n % 4 != 0)
    return FALSE;

  gum_memcpy (self->code, data, n);
  self->code += n / sizeof (guint32);
  self->pc += n;

  return TRUE;
}
