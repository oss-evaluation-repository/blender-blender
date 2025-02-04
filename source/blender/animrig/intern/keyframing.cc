/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

#include <cmath>
#include <string>

#include <fmt/format.h>

#include "ANIM_action.hh"
#include "ANIM_animdata.hh"
#include "ANIM_fcurve.hh"
#include "ANIM_keyframing.hh"
#include "ANIM_rna.hh"
#include "ANIM_visualkey.hh"

#include "BKE_action.h"
#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_fcurve.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_nla.h"
#include "BKE_report.hh"

#include "DNA_scene_types.h"

#include "BLI_bit_vector.hh"
#include "BLI_dynstr.h"
#include "BLI_math_base.h"
#include "BLI_utildefines.h"
#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_anim_types.h"
#include "MEM_guardedalloc.h"
#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.h"

#include "WM_types.hh"

namespace blender::animrig {

CombinedKeyingResult::CombinedKeyingResult()
{
  result_counter.fill(0);
}

void CombinedKeyingResult::add(const SingleKeyingResult result, const int count)
{
  result_counter[int(result)] += count;
}

void CombinedKeyingResult::merge(const CombinedKeyingResult &other)
{
  for (int i = 0; i < result_counter.size(); i++) {
    result_counter[i] += other.result_counter[i];
  }
}

int CombinedKeyingResult::get_count(const SingleKeyingResult result) const
{
  return result_counter[int(result)];
}

bool CombinedKeyingResult::has_errors() const
{
  /* For loop starts at 1 to skip the SUCCESS flag. Assumes that SUCCESS is 0 and the rest of the
   * enum are sequential values. */
  static_assert(int(SingleKeyingResult::SUCCESS) == 0);
  for (int i = 1; i < result_counter.size(); i++) {
    if (result_counter[i] > 0) {
      return true;
    }
  }
  return false;
}

void CombinedKeyingResult::generate_reports(ReportList *reports)
{
  if (!this->has_errors() && this->get_count(SingleKeyingResult::SUCCESS) == 0) {
    BKE_reportf(
        reports, RPT_WARNING, "No keys have been inserted and no errors have been reported.");
    return;
  }

  Vector<std::string> errors;
  if (this->get_count(SingleKeyingResult::UNKNOWN_FAILURE) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::UNKNOWN_FAILURE);
    errors.append(
        fmt::format(RPT_("There were {:d} keying failures for unknown reasons."), error_count));
  }

  if (this->get_count(SingleKeyingResult::CANNOT_CREATE_FCURVE) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::CANNOT_CREATE_FCURVE);
    errors.append(fmt::format(RPT_("Could not create {:d} F-Curve(s). This can happen when only "
                                   "inserting to available F-Curves."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::FCURVE_NOT_KEYFRAMEABLE) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::FCURVE_NOT_KEYFRAMEABLE);
    errors.append(
        fmt::format(RPT_("{:d} F-Curve(s) are not keyframeable. They might be locked or sampled."),
                    error_count));
  }

  if (this->get_count(SingleKeyingResult::NO_KEY_NEEDED) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::NO_KEY_NEEDED);
    errors.append(fmt::format(
        RPT_("Due to the setting 'Only Insert Needed', {:d} keyframe(s) have not been inserted."),
        error_count));
  }

  if (this->get_count(SingleKeyingResult::UNABLE_TO_INSERT_TO_NLA_STACK) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::UNABLE_TO_INSERT_TO_NLA_STACK);
    errors.append(
        fmt::format(RPT_("Due to the NLA stack setup, {:d} keyframe(s) have not been inserted."),
                    error_count));
  }

  if (this->get_count(SingleKeyingResult::ID_NOT_EDITABLE) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::ID_NOT_EDITABLE);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "they are not editable."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::ID_NOT_ANIMATABLE) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::ID_NOT_ANIMATABLE);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "they cannot be animated."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::CANNOT_RESOLVE_PATH) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::CANNOT_RESOLVE_PATH);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "the RNA path wasn't valid for them."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::NO_VALID_LAYER) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::NO_VALID_LAYER);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "there were no layers that could accept the keys."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::NO_VALID_STRIP) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::NO_VALID_STRIP);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "there were no strips that could accept the keys."),
                              error_count));
  }

  if (this->get_count(SingleKeyingResult::NO_VALID_BINDING) > 0) {
    const int error_count = this->get_count(SingleKeyingResult::NO_VALID_BINDING);
    errors.append(fmt::format(RPT_("Inserting keys on {:d} data-block(s) has been skipped because "
                                   "of missing animation bindings."),
                              error_count));
  }

  if (errors.is_empty()) {
    BKE_report(reports, RPT_WARNING, "Encountered unhandled error during keyframing");
    return;
  }

  if (errors.size() == 1) {
    BKE_report(reports, RPT_ERROR, errors[0].c_str());
    return;
  }

  std::string error_message = RPT_("Inserting keyframes failed:");
  for (const std::string &error : errors) {
    error_message.append(fmt::format("\n- {}", error));
  }
  BKE_report(reports, RPT_ERROR, error_message.c_str());
}

const char *default_channel_group_for_path(const PointerRNA *animated_struct,
                                           const StringRef prop_rna_path)
{
  if (animated_struct->type == &RNA_PoseBone) {
    bPoseChannel *pose_channel = static_cast<bPoseChannel *>(animated_struct->data);
    return pose_channel->name;
  }

  if (animated_struct->type == &RNA_Object) {
    if (prop_rna_path.find("location") != StringRef::not_found ||
        prop_rna_path.find("rotation") != StringRef::not_found ||
        prop_rna_path.find("scale") != StringRef::not_found)
    {
      /* NOTE: Keep this label in sync with the "ID" case in
       * keyingsets_utils.py :: get_transform_generators_base_info()
       */
      return "Object Transforms";
    }
  }

  return nullptr;
}

void update_autoflags_fcurve_direct(FCurve *fcu, PropertyRNA *prop)
{
  /* Set additional flags for the F-Curve (i.e. only integer values). */
  fcu->flag &= ~(FCURVE_INT_VALUES | FCURVE_DISCRETE_VALUES);
  switch (RNA_property_type(prop)) {
    case PROP_FLOAT:
      /* Do nothing. */
      break;
    case PROP_INT:
      /* Do integer (only 'whole' numbers) interpolation between all points. */
      fcu->flag |= FCURVE_INT_VALUES;
      break;
    default:
      /* Do 'discrete' (i.e. enum, boolean values which cannot take any intermediate
       * values at all) interpolation between all points.
       *    - however, we must also ensure that evaluated values are only integers still.
       */
      fcu->flag |= (FCURVE_DISCRETE_VALUES | FCURVE_INT_VALUES);
      break;
  }
}

bool is_keying_flag(const Scene *scene, const eKeying_Flag flag)
{
  if (scene) {
    return (scene->toolsettings->keying_flag & flag) || (U.keying_flag & flag);
  }
  return U.keying_flag & flag;
}

eInsertKeyFlags get_keyframing_flags(Scene *scene)
{
  eInsertKeyFlags flag = INSERTKEY_NOFLAGS;

  /* Visual keying. */
  if (is_keying_flag(scene, KEYING_FLAG_VISUALKEY)) {
    flag |= INSERTKEY_MATRIX;
  }

  /* Cycle-aware keyframe insertion - preserve cycle period and flow. */
  if (is_keying_flag(scene, KEYING_FLAG_CYCLEAWARE)) {
    flag |= INSERTKEY_CYCLE_AWARE;
  }

  if (is_keying_flag(scene, MANUALKEY_FLAG_INSERTNEEDED)) {
    flag |= INSERTKEY_NEEDED;
  }

  return flag;
}

bool key_insertion_may_create_fcurve(const eInsertKeyFlags insert_key_flags)
{
  return (insert_key_flags & (INSERTKEY_REPLACE | INSERTKEY_AVAILABLE)) == 0;
}

/** Used to make curves newly added to a cyclic Action cycle with the correct period. */
static void make_new_fcurve_cyclic(FCurve *fcu, const blender::float2 &action_range)
{
  /* The curve must contain one (newly-added) keyframe. */
  if (fcu->totvert != 1 || !fcu->bezt) {
    return;
  }

  const float period = action_range[1] - action_range[0];

  if (period < 0.1f) {
    return;
  }

  /* Move the keyframe into the range. */
  const float frame_offset = fcu->bezt[0].vec[1][0] - action_range[0];
  const float fix = floorf(frame_offset / period) * period;

  fcu->bezt[0].vec[0][0] -= fix;
  fcu->bezt[0].vec[1][0] -= fix;
  fcu->bezt[0].vec[2][0] -= fix;

  /* Duplicate and offset the keyframe. */
  fcu->bezt = static_cast<BezTriple *>(MEM_reallocN(fcu->bezt, sizeof(BezTriple) * 2));
  fcu->totvert = 2;

  fcu->bezt[1] = fcu->bezt[0];
  fcu->bezt[1].vec[0][0] += period;
  fcu->bezt[1].vec[1][0] += period;
  fcu->bezt[1].vec[2][0] += period;

  if (!fcu->modifiers.first) {
    add_fmodifier(&fcu->modifiers, FMODIFIER_TYPE_CYCLES, fcu);
  }
}

/* Check indices that were intended to be remapped and report any failed remaps. */
static void get_keyframe_values_create_reports(ReportList *reports,
                                               PointerRNA ptr,
                                               PropertyRNA *prop,
                                               const int index,
                                               const int count,
                                               const bool force_all,
                                               const BitSpan successful_remaps)
{

  DynStr *ds_failed_indices = BLI_dynstr_new();

  int total_failed = 0;
  for (int i = 0; i < count; i++) {
    const bool cur_index_evaluated = ELEM(index, i, -1) || force_all;
    if (!cur_index_evaluated) {
      /* `values[i]` was never intended to be remapped. */
      continue;
    }

    if (successful_remaps[i]) {
      /* `values[i]` successfully remapped. */
      continue;
    }

    total_failed++;
    /* Report that `values[i]` were intended to be remapped but failed remapping process. */
    BLI_dynstr_appendf(ds_failed_indices, "%d, ", i);
  }

  if (total_failed == 0) {
    BLI_dynstr_free(ds_failed_indices);
    return;
  }

  char *str_failed_indices = BLI_dynstr_get_cstring(ds_failed_indices);
  BLI_dynstr_free(ds_failed_indices);

  BKE_reportf(reports,
              RPT_WARNING,
              "Could not insert %i keyframe(s) due to zero NLA influence, base value, or value "
              "remapping failed: %s.%s for indices [%s]",
              total_failed,
              ptr.owner_id->name,
              RNA_property_ui_name(prop),
              str_failed_indices);

  MEM_freeN(str_failed_indices);
}

static Vector<float> get_keyframe_values(PointerRNA *ptr, PropertyRNA *prop, const bool visual_key)
{
  Vector<float> values;

  if (visual_key && visualkey_can_use(ptr, prop)) {
    /* Visual-keying is only available for object and pchan datablocks, as
     * it works by keyframing using a value extracted from the final matrix
     * instead of using the kt system to extract a value.
     */
    values = visualkey_get_values(ptr, prop);
  }
  else {
    values = get_rna_values(ptr, prop);
  }
  return values;
}

static BitVector<> nla_map_keyframe_values_and_generate_reports(
    const MutableSpan<float> values,
    const int index,
    PointerRNA &ptr,
    PropertyRNA &prop,
    NlaKeyframingContext *nla_context,
    const AnimationEvalContext *anim_eval_context,
    ReportList *reports,
    bool *force_all)
{
  BitVector<> successful_remaps(values.size(), false);
  BKE_animsys_nla_remap_keyframe_values(
      nla_context, &ptr, &prop, values, index, anim_eval_context, force_all, successful_remaps);
  get_keyframe_values_create_reports(
      reports, ptr, &prop, index, values.size(), false, successful_remaps);
  return successful_remaps;
}

/**
 * Move the point where a key is about to be inserted to be inside the main cycle range.
 * Returns the type of the cycle if it is enabled and valid.
 */
static eFCU_Cycle_Type remap_cyclic_keyframe_location(FCurve *fcu, float *px, float *py)
{
  if (fcu->totvert < 2 || !fcu->bezt) {
    return FCU_CYCLE_NONE;
  }

  eFCU_Cycle_Type type = BKE_fcurve_get_cycle_type(fcu);

  if (type == FCU_CYCLE_NONE) {
    return FCU_CYCLE_NONE;
  }

  BezTriple *first = &fcu->bezt[0], *last = &fcu->bezt[fcu->totvert - 1];
  const float start = first->vec[1][0], end = last->vec[1][0];

  if (start >= end) {
    return FCU_CYCLE_NONE;
  }

  if (*px < start || *px > end) {
    float period = end - start;
    float step = floorf((*px - start) / period);
    *px -= step * period;

    if (type == FCU_CYCLE_OFFSET) {
      /* Nasty check to handle the case when the modes are different better. */
      FMod_Cycles *data = static_cast<FMod_Cycles *>(((FModifier *)fcu->modifiers.first)->data);
      short mode = (step >= 0) ? data->after_mode : data->before_mode;

      if (mode == FCM_EXTRAPOLATE_CYCLIC_OFFSET) {
        *py -= step * (last->vec[1][1] - first->vec[1][1]);
      }
    }
  }

  return type;
}

static float nla_time_remap(const AnimationEvalContext *anim_eval_context,
                            PointerRNA *id_ptr,
                            AnimData *adt,
                            bAction *act,
                            ListBase *nla_cache,
                            NlaKeyframingContext **r_nla_context)
{
  if (adt && adt->action == act) {
    *r_nla_context = BKE_animsys_get_nla_keyframing_context(
        nla_cache, id_ptr, adt, anim_eval_context);

    const float remapped_frame = BKE_nla_tweakedit_remap(
        adt, anim_eval_context->eval_time, NLATIME_CONVERT_UNMAP);
    return remapped_frame;
  }

  *r_nla_context = nullptr;
  return anim_eval_context->eval_time;
}

/* Insert the specified keyframe value into a single F-Curve. */
static SingleKeyingResult insert_keyframe_value(
    FCurve *fcu, float cfra, float curval, eBezTriple_KeyframeType keytype, eInsertKeyFlags flag)
{
  if (!BKE_fcurve_is_keyframable(fcu)) {
    return SingleKeyingResult::FCURVE_NOT_KEYFRAMEABLE;
  }

  /* Adjust coordinates for cycle aware insertion. */
  if (flag & INSERTKEY_CYCLE_AWARE) {
    if (remap_cyclic_keyframe_location(fcu, &cfra, &curval) != FCU_CYCLE_PERFECT) {
      /* Inhibit action from insert_vert_fcurve unless it's a perfect cycle. */
      flag &= ~INSERTKEY_CYCLE_AWARE;
    }
  }

  KeyframeSettings settings = get_keyframe_settings((flag & INSERTKEY_NO_USERPREF) == 0);
  settings.keyframe_type = keytype;

  return insert_vert_fcurve(fcu, {cfra, curval}, settings, flag);
}

bool insert_keyframe_direct(ReportList *reports,
                            PointerRNA ptr,
                            PropertyRNA *prop,
                            FCurve *fcu,
                            const AnimationEvalContext *anim_eval_context,
                            eBezTriple_KeyframeType keytype,
                            NlaKeyframingContext *nla_context,
                            eInsertKeyFlags flag)
{

  if (fcu == nullptr) {
    BKE_report(reports, RPT_ERROR, "No F-Curve to add keyframes to");
    return false;
  }

  if ((ptr.owner_id == nullptr) && (ptr.data == nullptr)) {
    BKE_report(
        reports, RPT_ERROR, "No RNA pointer available to retrieve values for keyframing from");
    return false;
  }

  if (prop == nullptr) {
    PointerRNA tmp_ptr;

    if (RNA_path_resolve_property(&ptr, fcu->rna_path, &tmp_ptr, &prop) == false) {
      const char *idname = (ptr.owner_id) ? ptr.owner_id->name : RPT_("<No ID pointer>");

      BKE_reportf(reports,
                  RPT_ERROR,
                  "Could not insert keyframe, as RNA path is invalid for the given ID (ID = %s, "
                  "path = %s)",
                  idname,
                  fcu->rna_path);
      return false;
    }

    /* Property found, so overwrite 'ptr' to make later code easier. */
    ptr = tmp_ptr;
  }

  /* Update F-Curve flags to ensure proper behavior for property type. */
  update_autoflags_fcurve_direct(fcu, prop);

  const int index = fcu->array_index;
  const bool visual_keyframing = flag & INSERTKEY_MATRIX;
  Vector<float> values = get_keyframe_values(&ptr, prop, visual_keyframing);

  BitVector<> successful_remaps = nla_map_keyframe_values_and_generate_reports(
      values.as_mutable_span(),
      index,
      ptr,
      *prop,
      nla_context,
      anim_eval_context,
      reports,
      nullptr);

  float current_value = 0.0f;
  if (index >= 0 && index < values.size()) {
    current_value = values[index];
  }

  /* This happens if NLA rejects this insertion. */
  if (!successful_remaps[index]) {
    return false;
  }

  const float cfra = anim_eval_context->eval_time;
  const SingleKeyingResult result = insert_keyframe_value(fcu, cfra, current_value, keytype, flag);

  if (result != SingleKeyingResult::SUCCESS) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Failed to insert keys on F-Curve with path '%s[%d]', ensure that it is not "
                "locked or sampled, and try removing F-Modifiers",
                fcu->rna_path,
                fcu->array_index);
  }
  return result == SingleKeyingResult::SUCCESS;
}

/** Find or create the FCurve based on the given path, and insert the specified value into it. */
static SingleKeyingResult insert_keyframe_fcurve_value(Main *bmain,
                                                       PointerRNA *ptr,
                                                       PropertyRNA *prop,
                                                       bAction *act,
                                                       const char group[],
                                                       const char rna_path[],
                                                       int array_index,
                                                       const float fcurve_frame,
                                                       float curval,
                                                       eBezTriple_KeyframeType keytype,
                                                       eInsertKeyFlags flag)
{
  /* Make sure the F-Curve exists.
   * - if we're replacing keyframes only, DO NOT create new F-Curves if they do not exist yet
   *   but still try to get the F-Curve if it exists...
   */

  FCurve *fcu = key_insertion_may_create_fcurve(flag) ?
                    action_fcurve_ensure(bmain, act, group, ptr, rna_path, array_index) :
                    action_fcurve_find(act, rna_path, array_index);

  /* We may not have a F-Curve when we're replacing only. */
  if (!fcu) {
    return SingleKeyingResult::CANNOT_CREATE_FCURVE;
  }

  const bool is_new_curve = (fcu->totvert == 0);

  /* If the curve has only one key, make it cyclic if appropriate. */
  const bool is_cyclic_action = (flag & INSERTKEY_CYCLE_AWARE) && BKE_action_is_cyclic(act);

  if (is_cyclic_action && fcu->totvert == 1) {
    make_new_fcurve_cyclic(fcu, {act->frame_start, act->frame_end});
  }

  /* Update F-Curve flags to ensure proper behavior for property type. */
  update_autoflags_fcurve_direct(fcu, prop);

  const SingleKeyingResult result = insert_keyframe_value(
      fcu, fcurve_frame, curval, keytype, flag);

  /* If the curve is new, make it cyclic if appropriate. */
  if (is_cyclic_action && is_new_curve) {
    make_new_fcurve_cyclic(fcu, {act->frame_start, act->frame_end});
  }

  return result;
}

CombinedKeyingResult insert_keyframe(Main *bmain,
                                     ID &id,
                                     const char group[],
                                     const char rna_path[],
                                     int array_index,
                                     const AnimationEvalContext *anim_eval_context,
                                     eBezTriple_KeyframeType keytype,
                                     eInsertKeyFlags flag)
{
  CombinedKeyingResult combined_result;

  if (!BKE_id_is_editable(bmain, &id)) {
    combined_result.add(SingleKeyingResult::ID_NOT_EDITABLE);
    return combined_result;
  }

  PointerRNA ptr;
  PropertyRNA *prop = nullptr;
  PointerRNA id_ptr = RNA_id_pointer_create(&id);
  if (RNA_path_resolve_property(&id_ptr, rna_path, &ptr, &prop) == false) {
    combined_result.add(SingleKeyingResult::CANNOT_RESOLVE_PATH);
    return combined_result;
  }

  bAction *act = id_action_ensure(bmain, &id);
  if (act == nullptr) {
    combined_result.add(SingleKeyingResult::ID_NOT_ANIMATABLE);
    return combined_result;
  }

  /* Apply NLA-mapping to frame to use (if applicable). */
  NlaKeyframingContext *nla_context = nullptr;
  ListBase nla_cache = {nullptr, nullptr};
  AnimData *adt = BKE_animdata_from_id(&id);
  const float nla_mapped_frame = nla_time_remap(
      anim_eval_context, &id_ptr, adt, act, &nla_cache, &nla_context);

  const bool visual_keyframing = flag & INSERTKEY_MATRIX;
  Vector<float> values = get_keyframe_values(&ptr, prop, visual_keyframing);

  bool force_all;
  BitVector<> successful_remaps(values.size(), false);
  BKE_animsys_nla_remap_keyframe_values(nla_context,
                                        &ptr,
                                        prop,
                                        values,
                                        array_index,
                                        anim_eval_context,
                                        &force_all,
                                        successful_remaps);

  /* Key the entire array. */
  int key_count = 0;
  if (array_index == -1 || force_all) {
    /* In force mode, if any of the curves succeeds, drop the replace mode and restart. */
    if (force_all && (flag & (INSERTKEY_REPLACE | INSERTKEY_AVAILABLE)) != 0) {
      int exclude = -1;

      for (array_index = 0; array_index < values.size(); array_index++) {
        if (!successful_remaps[array_index]) {
          continue;
        }
        const SingleKeyingResult result = insert_keyframe_fcurve_value(bmain,
                                                                       &ptr,
                                                                       prop,
                                                                       act,
                                                                       group,
                                                                       rna_path,
                                                                       array_index,
                                                                       nla_mapped_frame,
                                                                       values[array_index],
                                                                       keytype,
                                                                       flag);
        combined_result.add(result);
        if (result == SingleKeyingResult::SUCCESS) {
          key_count++;
          exclude = array_index;
          break;
        }
      }

      if (exclude != -1) {
        flag &= ~(INSERTKEY_REPLACE | INSERTKEY_AVAILABLE);

        for (array_index = 0; array_index < values.size(); array_index++) {
          if (!successful_remaps[array_index]) {
            continue;
          }

          if (array_index != exclude) {
            const SingleKeyingResult result = insert_keyframe_fcurve_value(bmain,
                                                                           &ptr,
                                                                           prop,
                                                                           act,
                                                                           group,
                                                                           rna_path,
                                                                           array_index,
                                                                           nla_mapped_frame,
                                                                           values[array_index],
                                                                           keytype,
                                                                           flag);
            combined_result.add(result);
            if (result == SingleKeyingResult::SUCCESS) {
              key_count++;
            }
          }
        }
      }
    }
    /* Simply insert all channels. */
    else {
      for (array_index = 0; array_index < values.size(); array_index++) {
        if (!successful_remaps[array_index]) {
          continue;
        }

        const SingleKeyingResult result = insert_keyframe_fcurve_value(bmain,
                                                                       &ptr,
                                                                       prop,
                                                                       act,
                                                                       group,
                                                                       rna_path,
                                                                       array_index,
                                                                       nla_mapped_frame,
                                                                       values[array_index],
                                                                       keytype,
                                                                       flag);
        combined_result.add(result);
        if (result == SingleKeyingResult::SUCCESS) {
          key_count++;
        }
      }
    }
  }
  /* Key a single index. */
  else {
    if (array_index >= 0 && array_index < values.size() && successful_remaps[array_index]) {
      const SingleKeyingResult result = insert_keyframe_fcurve_value(bmain,
                                                                     &ptr,
                                                                     prop,
                                                                     act,
                                                                     group,
                                                                     rna_path,
                                                                     array_index,
                                                                     nla_mapped_frame,
                                                                     values[array_index],
                                                                     keytype,
                                                                     flag);
      combined_result.add(result);
      if (result == SingleKeyingResult::SUCCESS) {
        key_count++;
      }
    }
  }

  BKE_animsys_free_nla_keyframing_context_cache(&nla_cache);

  if (key_count > 0) {
    if (act != nullptr) {
      DEG_id_tag_update(&act->id, ID_RECALC_ANIMATION_NO_FLUSH);
    }
    if (adt != nullptr && adt->action != nullptr && adt->action != act) {
      DEG_id_tag_update(&adt->action->id, ID_RECALC_ANIMATION_NO_FLUSH);
    }
  }

  return combined_result;
}

/* ************************************************** */
/* KEYFRAME DELETION */

/* Main Keyframing API call:
 * Use this when validation of necessary animation data isn't necessary as it
 * already exists. It will delete a keyframe at the current frame.
 *
 * The flag argument is used for special settings that alter the behavior of
 * the keyframe deletion. These include the quick refresh options.
 */

static void deg_tag_after_keyframe_delete(Main *bmain, ID *id, AnimData *adt)
{
  if (adt->action == nullptr) {
    /* In the case last f-curve was removed need to inform dependency graph
     * about relations update, since it needs to get rid of animation operation
     * for this data-block. */
    DEG_id_tag_update_ex(bmain, id, ID_RECALC_ANIMATION_NO_FLUSH);
    DEG_relations_tag_update(bmain);
  }
  else {
    DEG_id_tag_update_ex(bmain, &adt->action->id, ID_RECALC_ANIMATION_NO_FLUSH);
  }
}

int delete_keyframe(Main *bmain,
                    ReportList *reports,
                    ID *id,
                    bAction *act,
                    const char rna_path[],
                    int array_index,
                    float cfra)
{
  AnimData *adt = BKE_animdata_from_id(id);

  if (ELEM(nullptr, id, adt)) {
    BKE_report(reports, RPT_ERROR, "No ID block and/or AnimData to delete keyframe from");
    return 0;
  }

  PointerRNA ptr;
  PropertyRNA *prop;
  PointerRNA id_ptr = RNA_id_pointer_create(id);
  if (RNA_path_resolve_property(&id_ptr, rna_path, &ptr, &prop) == false) {
    BKE_reportf(
        reports,
        RPT_ERROR,
        "Could not delete keyframe, as RNA path is invalid for the given ID (ID = %s, path = %s)",
        id->name,
        rna_path);
    return 0;
  }

  if (act == nullptr) {
    if (adt->action) {
      act = adt->action;
      cfra = BKE_nla_tweakedit_remap(adt, cfra, NLATIME_CONVERT_UNMAP);
    }
    else {
      BKE_reportf(reports, RPT_ERROR, "No action to delete keyframes from for ID = %s", id->name);
      return 0;
    }
  }

  int array_index_max = array_index + 1;

  if (array_index == -1) {
    array_index = 0;
    array_index_max = RNA_property_array_length(&ptr, prop);

    /* For single properties, increase max_index so that the property itself gets included,
     * but don't do this for standard arrays since that can cause corruption issues
     * (extra unused curves).
     */
    if (array_index_max == array_index) {
      array_index_max++;
    }
  }

  /* Will only loop once unless the array index was -1. */
  int key_count = 0;
  for (; array_index < array_index_max; array_index++) {
    FCurve *fcu = action_fcurve_find(act, rna_path, array_index);

    if (fcu == nullptr) {
      continue;
    }

    if (BKE_fcurve_is_protected(fcu)) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Not deleting keyframe for locked F-Curve '%s' for %s '%s'",
                  fcu->rna_path,
                  BKE_idtype_idcode_to_name(GS(id->name)),
                  id->name + 2);
      continue;
    }

    key_count += delete_keyframe_fcurve(adt, fcu, cfra);
  }
  if (key_count) {
    deg_tag_after_keyframe_delete(bmain, id, adt);
  }

  return key_count;
}

/* ************************************************** */
/* KEYFRAME CLEAR */

int clear_keyframe(Main *bmain,
                   ReportList *reports,
                   ID *id,
                   bAction *act,
                   const char rna_path[],
                   int array_index,
                   eInsertKeyFlags /*flag*/)
{
  AnimData *adt = BKE_animdata_from_id(id);

  if (ELEM(nullptr, id, adt)) {
    BKE_report(reports, RPT_ERROR, "No ID block and/or AnimData to delete keyframe from");
    return 0;
  }

  PointerRNA ptr;
  PropertyRNA *prop;
  PointerRNA id_ptr = RNA_id_pointer_create(id);
  if (RNA_path_resolve_property(&id_ptr, rna_path, &ptr, &prop) == false) {
    BKE_reportf(
        reports,
        RPT_ERROR,
        "Could not clear keyframe, as RNA path is invalid for the given ID (ID = %s, path = %s)",
        id->name,
        rna_path);
    return 0;
  }

  if (act == nullptr) {
    if (adt->action) {
      act = adt->action;
    }
    else {
      BKE_reportf(reports, RPT_ERROR, "No action to delete keyframes from for ID = %s", id->name);
      return 0;
    }
  }

  int array_index_max = array_index + 1;
  if (array_index == -1) {
    array_index = 0;
    array_index_max = RNA_property_array_length(&ptr, prop);

    /* For single properties, increase max_index so that the property itself gets included,
     * but don't do this for standard arrays since that can cause corruption issues
     * (extra unused curves).
     */
    if (array_index_max == array_index) {
      array_index_max++;
    }
  }

  int key_count = 0;
  /* Will only loop once unless the array index was -1. */
  for (; array_index < array_index_max; array_index++) {
    FCurve *fcu = action_fcurve_find(act, rna_path, array_index);

    if (fcu == nullptr) {
      continue;
    }

    if (BKE_fcurve_is_protected(fcu)) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Not clearing all keyframes from locked F-Curve '%s' for %s '%s'",
                  fcu->rna_path,
                  BKE_idtype_idcode_to_name(GS(id->name)),
                  id->name + 2);
      continue;
    }

    animdata_fcurve_delete(nullptr, adt, fcu);

    key_count++;
  }
  if (key_count) {
    deg_tag_after_keyframe_delete(bmain, id, adt);
  }

  return key_count;
}

static CombinedKeyingResult insert_key_legacy_action(Main *bmain,
                                                     bAction *action,
                                                     PointerRNA *ptr,
                                                     PropertyRNA *prop,
                                                     const std::string &rna_path,
                                                     const float frame,
                                                     const Span<float> values,
                                                     eInsertKeyFlags insert_key_flag,
                                                     eBezTriple_KeyframeType key_type,
                                                     const BitSpan keying_mask)
{
  BLI_assert(bmain != nullptr);
  BLI_assert(action != nullptr);
  BLI_assert(action->wrap().is_action_legacy());

  const char *group = default_channel_group_for_path(ptr, rna_path);

  int property_array_index = 0;
  CombinedKeyingResult combined_result;
  for (float value : values) {
    if (!keying_mask[property_array_index]) {
      combined_result.add(SingleKeyingResult::UNABLE_TO_INSERT_TO_NLA_STACK);
      property_array_index++;
      continue;
    }
    const SingleKeyingResult keying_result = insert_keyframe_fcurve_value(bmain,
                                                                          ptr,
                                                                          prop,
                                                                          action,
                                                                          group,
                                                                          rna_path.c_str(),
                                                                          property_array_index,
                                                                          frame,
                                                                          value,
                                                                          key_type,
                                                                          insert_key_flag);
    combined_result.add(keying_result);
    property_array_index++;
  }
  return combined_result;
}

struct KeyInsertData {
  float2 position;
  int array_index;
};

static SingleKeyingResult insert_key_layer(Layer &layer,
                                           Binding &binding,
                                           const std::string &rna_path,
                                           const KeyInsertData &key_data,
                                           const KeyframeSettings &key_settings,
                                           const eInsertKeyFlags insert_key_flags)
{
  /* TODO: we currently assume there will always be precisely one strip, which
   * is infinite and has no time offset. This will not hold true in the future
   * when we add support for multiple strips. */
  BLI_assert(layer.strips().size() == 1);
  Strip *strip = layer.strip(0);
  BLI_assert(strip->is_infinite());
  BLI_assert(strip->frame_offset == 0.0);

  return strip->as<KeyframeStrip>().keyframe_insert(
      binding, rna_path, key_data.array_index, key_data.position, key_settings, insert_key_flags);
}

static CombinedKeyingResult insert_key_layered_action(Action &action,
                                                      const int32_t binding_handle,
                                                      PointerRNA *rna_pointer,
                                                      const blender::Span<RNAPath> rna_paths,
                                                      const float scene_frame,
                                                      const KeyframeSettings &key_settings,
                                                      const eInsertKeyFlags insert_key_flags)
{
  BLI_assert(action.is_action_layered());

  ID *id = rna_pointer->owner_id;
  CombinedKeyingResult combined_result;

  Binding *binding = action.binding_for_handle(binding_handle);
  if (binding == nullptr) {
    binding = &action.binding_add_for_id(*id);
    const bool success = action.assign_id(binding, *id);
    UNUSED_VARS_NDEBUG(success);
    BLI_assert_msg(
        success,
        "With a new Binding, the only reason this could fail is that the ID itself cannot be "
        "animated, which should have been caught and handled by higher-level functions.");
  }

  /* Ensure that at least one layer exists. If not, create the default layer
   * with the default infinite keyframe strip. */
  action.layer_ensure_at_least_one();

  /* TODO: we currently assume this will always successfully find a layer.
   * However, that may not be true in the future when we implement features like
   * layer locking: if layers already exist, but they are all locked, then the
   * default layer won't be added by the line above, but there also won't be any
   * layers we can insert keys into. */
  Layer *layer = action.get_layer_for_keyframing();
  BLI_assert(layer != nullptr);

  const bool use_visual_keyframing = insert_key_flags & INSERTKEY_MATRIX;

  for (const RNAPath &rna_path : rna_paths) {
    PointerRNA ptr;
    PropertyRNA *prop = nullptr;
    const bool path_resolved = RNA_path_resolve_property(
        rna_pointer, rna_path.path.c_str(), &ptr, &prop);
    if (!path_resolved) {
      std::fprintf(stderr,
                   "Failed to insert key on binding %s due to unresolved RNA path: %s\n",
                   binding->name,
                   rna_path.path.c_str());
      combined_result.add(SingleKeyingResult::CANNOT_RESOLVE_PATH);
      continue;
    }
    const std::optional<std::string> rna_path_id_to_prop = RNA_path_from_ID_to_property(&ptr,
                                                                                        prop);
    BLI_assert(rna_path_id_to_prop.has_value());
    Vector<float> rna_values = get_keyframe_values(&ptr, prop, use_visual_keyframing);

    for (const int property_index : rna_values.index_range()) {
      /* If we're only keying one array element, skip all elements other than
       * that one. */
      if (rna_path.index.has_value() && *rna_path.index != property_index) {
        continue;
      }

      const KeyInsertData key_data = {{scene_frame, rna_values[property_index]}, property_index};
      const SingleKeyingResult result = insert_key_layer(
          *layer, *binding, *rna_path_id_to_prop, key_data, key_settings, insert_key_flags);
      combined_result.add(result);
    }
  }

  DEG_id_tag_update(&action.id, ID_RECALC_ANIMATION_NO_FLUSH);

  return combined_result;
}

CombinedKeyingResult insert_key_rna(PointerRNA *rna_pointer,
                                    const blender::Span<RNAPath> rna_paths,
                                    const float scene_frame,
                                    const eInsertKeyFlags insert_key_flags,
                                    const eBezTriple_KeyframeType key_type,
                                    Main *bmain,
                                    const AnimationEvalContext &anim_eval_context)
{
  ID *id = rna_pointer->owner_id;
  CombinedKeyingResult combined_result;

  /* Init animdata if none available yet. */
  AnimData *adt = BKE_animdata_ensure_id(id);
  if (adt == nullptr) {
    combined_result.add(SingleKeyingResult::ID_NOT_ANIMATABLE);
    return combined_result;
  }

  bAction *action = id_action_ensure(bmain, id);
  BLI_assert(action != nullptr);

  if (USER_EXPERIMENTAL_TEST(&U, use_animation_baklava) && action->wrap().is_action_layered()) {
    /* TODO: Don't hardcode key settings. */
    KeyframeSettings key_settings;
    key_settings.keyframe_type = key_type;
    key_settings.handle = HD_AUTO_ANIM;
    key_settings.interpolation = BEZT_IPO_BEZ;
    return insert_key_layered_action(action->wrap(),
                                     adt->binding_handle,
                                     rna_pointer,
                                     rna_paths,
                                     scene_frame,
                                     key_settings,
                                     insert_key_flags);
  }

  /* Keyframing functions can deal with the nla_context being a nullptr. */
  ListBase nla_cache = {nullptr, nullptr};
  NlaKeyframingContext *nla_context = nullptr;

  if (adt && adt->action == action) {
    PointerRNA id_pointer = RNA_id_pointer_create(id);
    nla_context = BKE_animsys_get_nla_keyframing_context(
        &nla_cache, &id_pointer, adt, &anim_eval_context);
  }

  const float nla_frame = BKE_nla_tweakedit_remap(adt, scene_frame, NLATIME_CONVERT_UNMAP);
  const bool visual_keyframing = insert_key_flags & INSERTKEY_MATRIX;

  for (const RNAPath &rna_path : rna_paths) {
    PointerRNA ptr;
    PropertyRNA *prop = nullptr;
    const bool path_resolved = RNA_path_resolve_property(
        rna_pointer, rna_path.path.c_str(), &ptr, &prop);
    if (!path_resolved) {
      combined_result.add(SingleKeyingResult::CANNOT_RESOLVE_PATH);
      continue;
    }
    const std::optional<std::string> rna_path_id_to_prop = RNA_path_from_ID_to_property(&ptr,
                                                                                        prop);
    Vector<float> rna_values = get_keyframe_values(&ptr, prop, visual_keyframing);

    BitVector<> successful_remaps(rna_values.size(), false);
    BKE_animsys_nla_remap_keyframe_values(nla_context,
                                          rna_pointer,
                                          prop,
                                          rna_values.as_mutable_span(),
                                          rna_path.index.value_or(-1),
                                          &anim_eval_context,
                                          nullptr,
                                          successful_remaps);
    const CombinedKeyingResult result = insert_key_legacy_action(bmain,
                                                                 action,
                                                                 rna_pointer,
                                                                 prop,
                                                                 rna_path_id_to_prop->c_str(),
                                                                 nla_frame,
                                                                 rna_values.as_span(),
                                                                 insert_key_flags,
                                                                 key_type,
                                                                 successful_remaps);
    combined_result.merge(result);
  }
  BKE_animsys_free_nla_keyframing_context_cache(&nla_cache);

  return combined_result;
}

}  // namespace blender::animrig
