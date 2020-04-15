/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include "DNA_defaults.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_simulation_types.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.h"
#include "BKE_customdata.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_pointcloud.h"

#include "BLT_translation.h"

#include "DEG_depsgraph_query.h"

/* PointCloud datablock */

static void pointcloud_init_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(pointcloud, id));

  MEMCPY_STRUCT_AFTER(pointcloud, DNA_struct_default_get(PointCloud), id);

  CustomData_reset(&pointcloud->pdata);
  CustomData_add_layer(&pointcloud->pdata, CD_LOCATION, CD_CALLOC, NULL, pointcloud->totpoint);
  CustomData_add_layer(&pointcloud->pdata, CD_RADIUS, CD_CALLOC, NULL, pointcloud->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud);
}

void *BKE_pointcloud_add(Main *bmain, const char *name)
{
  PointCloud *pointcloud = BKE_libblock_alloc(bmain, ID_PT, name, 0);

  pointcloud_init_data(&pointcloud->id);

  return pointcloud;
}

static void pointcloud_copy_data(Main *UNUSED(bmain), ID *id_dst, const ID *id_src, const int flag)
{
  PointCloud *pointcloud_dst = (PointCloud *)id_dst;
  const PointCloud *pointcloud_src = (const PointCloud *)id_src;
  pointcloud_dst->mat = MEM_dupallocN(pointcloud_dst->mat);

  const eCDAllocType alloc_type = (flag & LIB_ID_COPY_CD_REFERENCE) ? CD_REFERENCE : CD_DUPLICATE;
  CustomData_copy(&pointcloud_src->pdata,
                  &pointcloud_dst->pdata,
                  CD_MASK_ALL,
                  alloc_type,
                  pointcloud_dst->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud_dst);
}

PointCloud *BKE_pointcloud_copy(Main *bmain, const PointCloud *pointcloud)
{
  PointCloud *pointcloud_copy;
  BKE_id_copy(bmain, &pointcloud->id, (ID **)&pointcloud_copy);
  return pointcloud_copy;
}

static void pointcloud_make_local(Main *bmain, ID *id, const int flags)
{
  BKE_lib_id_make_local_generic(bmain, id, flags);
}

static void pointcloud_free_data(ID *id)
{
  PointCloud *pointcloud = (PointCloud *)id;
  BKE_animdata_free(&pointcloud->id, false);
  BKE_pointcloud_batch_cache_free(pointcloud);
  CustomData_free(&pointcloud->pdata, pointcloud->totpoint);
  MEM_SAFE_FREE(pointcloud->mat);
}

IDTypeInfo IDType_ID_PT = {
    .id_code = ID_PT,
    .id_filter = FILTER_ID_PT,
    .main_listbase_index = INDEX_ID_PT,
    .struct_size = sizeof(PointCloud),
    .name = "PointCloud",
    .name_plural = "pointclouds",
    .translation_context = BLT_I18NCONTEXT_ID_POINTCLOUD,
    .flags = 0,

    .init_data = pointcloud_init_data,
    .copy_data = pointcloud_copy_data,
    .free_data = pointcloud_free_data,
    .make_local = pointcloud_make_local,
};

BoundBox *BKE_pointcloud_boundbox_get(Object *ob)
{
  BLI_assert(ob->type == OB_POINTCLOUD);
  PointCloud *pointcloud = ob->data;

  if (ob->runtime.bb != NULL && (ob->runtime.bb->flag & BOUNDBOX_DIRTY) == 0) {
    return ob->runtime.bb;
  }

  if (ob->runtime.bb == NULL) {
    ob->runtime.bb = MEM_callocN(sizeof(BoundBox), "pointcloud boundbox");

    float min[3], max[3];
    INIT_MINMAX(min, max);

    float(*pointcloud_co)[3] = pointcloud->co;
    float *pointcloud_radius = pointcloud->radius;
    for (int a = 0; a < pointcloud->totpoint; a++) {
      float *co = pointcloud_co[a];
      float radius = (pointcloud_radius) ? pointcloud_radius[a] : 0.0f;
      float co_min[3] = {co[0] - radius, co[1] - radius, co[2] - radius};
      float co_max[3] = {co[0] + radius, co[1] + radius, co[2] + radius};
      DO_MIN(co_min, min);
      DO_MAX(co_max, max);
    }

    BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
  }

  return ob->runtime.bb;
}

void BKE_pointcloud_update_customdata_pointers(PointCloud *pointcloud)
{
  pointcloud->co = CustomData_get_layer(&pointcloud->pdata, CD_LOCATION);
  pointcloud->radius = CustomData_get_layer(&pointcloud->pdata, CD_RADIUS);
}

/* Dependency Graph */

PointCloud *BKE_pointcloud_new_for_eval(const PointCloud *pointcloud_src, int totpoint)
{
  PointCloud *pointcloud_dst = BKE_id_new_nomain(ID_PT, NULL);

  STRNCPY(pointcloud_dst->id.name, pointcloud_src->id.name);
  pointcloud_dst->mat = MEM_dupallocN(pointcloud_src->mat);
  pointcloud_dst->totcol = pointcloud_src->totcol;

  pointcloud_dst->totpoint = totpoint;
  CustomData_copy(
      &pointcloud_src->pdata, &pointcloud_dst->pdata, CD_MASK_ALL, CD_CALLOC, totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud_dst);

  return pointcloud_dst;
}

PointCloud *BKE_pointcloud_copy_for_eval(struct PointCloud *pointcloud_src, bool reference)
{
  int flags = LIB_ID_COPY_LOCALIZE;

  if (reference) {
    flags |= LIB_ID_COPY_CD_REFERENCE;
  }

  PointCloud *result;
  BKE_id_copy_ex(NULL, &pointcloud_src->id, (ID **)&result, flags);
  return result;
}

static PointCloud *pointcloud_evaluate_modifiers(struct Depsgraph *UNUSED(depsgraph),
                                                 Scene *scene,
                                                 Object *UNUSED(object),
                                                 PointCloud *pointcloud_input)
{
  PointCloud *pointcloud = BKE_id_new_nomain(ID_PT, "New Point Cloud");

  Simulation *simulation = pointcloud_input->source_simulation;
  if (simulation == NULL) {
    pointcloud->totpoint = 20;
  }
  else {
    pointcloud->totpoint = BLI_listbase_count(&simulation->nodetree->nodes);
  }
  CustomData_realloc(&pointcloud->pdata, pointcloud->totpoint);
  BKE_pointcloud_update_customdata_pointers(pointcloud);

  RNG *rng = BLI_rng_new(45);

  printf("PointCloud eval\n");

  for (int i = 0; i < pointcloud->totpoint; i++) {
    pointcloud->co[i][0] = 0.1f * scene->r.cfra;
    pointcloud->co[i][1] = BLI_rng_get_float(rng);
    pointcloud->co[i][2] = BLI_rng_get_float(rng);
    pointcloud->radius[i] = 0.1f;
  }

  BLI_rng_free(rng);
  return pointcloud;
}

void BKE_pointcloud_data_update(struct Depsgraph *depsgraph, struct Scene *scene, Object *object)
{
  /* Free any evaluated data and restore original data. */
  BKE_object_free_derived_caches(object);

  /* Evaluate modifiers. */
  PointCloud *pointcloud = object->data;
  PointCloud *pointcloud_eval = pointcloud_evaluate_modifiers(
      depsgraph, scene, object, pointcloud);

  /* Assign evaluated object. */
  const bool is_owned = (pointcloud != pointcloud_eval);
  BKE_object_eval_assign_data(object, &pointcloud_eval->id, is_owned);
}

/* Draw Cache */
void (*BKE_pointcloud_batch_cache_dirty_tag_cb)(PointCloud *pointcloud, int mode) = NULL;
void (*BKE_pointcloud_batch_cache_free_cb)(PointCloud *pointcloud) = NULL;

void BKE_pointcloud_batch_cache_dirty_tag(PointCloud *pointcloud, int mode)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_dirty_tag_cb(pointcloud, mode);
  }
}

void BKE_pointcloud_batch_cache_free(PointCloud *pointcloud)
{
  if (pointcloud->batch_cache) {
    BKE_pointcloud_batch_cache_free_cb(pointcloud);
  }
}
