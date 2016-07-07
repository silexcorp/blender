/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 *
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Lukas Toenne
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/strands.c
 *  \ingroup bke
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_kdtree.h"
#include "BLI_math.h"
#include "BLI_rand.h"

#include "BKE_DerivedMesh.h"
#include "BKE_mesh_sample.h"
#include "BKE_strands.h"

#include "GPU_buffers.h"
#include "GPU_strands.h"

Strands *BKE_strands_new(void)
{
	Strands *strands = MEM_callocN(sizeof(Strands), "strands");
	return strands;
}

Strands *BKE_strands_copy(Strands *strands)
{
	Strands *nstrands = MEM_dupallocN(strands);
	
	if (strands->curves) {
		nstrands->curves = MEM_dupallocN(strands->curves);
	}
	if (strands->verts) {
		nstrands->verts = MEM_dupallocN(strands->verts);
	}
	
	/* lazy initialized */
	nstrands->gpu_shader = NULL;
	nstrands->data_final = NULL;
	
	return nstrands;
}

void BKE_strands_free(Strands *strands)
{
	if (strands->gpu_shader)
		GPU_strand_shader_free(strands->gpu_shader);
	
	if (strands->data_final)
		BKE_strand_data_free(strands->data_final);
	
	if (strands->curves)
		MEM_freeN(strands->curves);
	if (strands->verts)
		MEM_freeN(strands->verts);
	MEM_freeN(strands);
}

bool BKE_strands_get_location(const StrandCurve *curve, DerivedMesh *root_dm, float loc[3])
{
	float nor[3], tang[3];
	if (BKE_mesh_sample_eval(root_dm, &curve->root, loc, nor, tang)) {
		return true;
	}
	else {
		zero_v3(loc);
		return false;
	}
}

bool BKE_strands_get_matrix(const StrandCurve *curve, DerivedMesh *root_dm, float mat[4][4])
{
	if (BKE_mesh_sample_eval(root_dm, &curve->root, mat[3], mat[2], mat[0])) {
		cross_v3_v3v3(mat[1], mat[2], mat[0]);
		mat[0][3] = 0.0f;
		mat[1][3] = 0.0f;
		mat[2][3] = 0.0f;
		mat[3][3] = 1.0f;
		return true;
	}
	else {
		unit_m4(mat);
		return false;
	}
}

bool BKE_strands_get_fiber_location(const StrandFiber *fiber, DerivedMesh *root_dm, float loc[3])
{
	float nor[3], tang[3];
	if (BKE_mesh_sample_eval(root_dm, &fiber->root, loc, nor, tang)) {
		return true;
	}
	else {
		zero_v3(loc);
		return false;
	}
}

bool BKE_strands_get_fiber_matrix(const StrandFiber *fiber, DerivedMesh *root_dm, float mat[4][4])
{
	if (BKE_mesh_sample_eval(root_dm, &fiber->root, mat[3], mat[2], mat[0])) {
		cross_v3_v3v3(mat[1], mat[2], mat[0]);
		mat[0][3] = 0.0f;
		mat[1][3] = 0.0f;
		mat[2][3] = 0.0f;
		mat[3][3] = 1.0f;
		return true;
	}
	else {
		unit_m4(mat);
		return false;
	}
}

/* ------------------------------------------------------------------------- */

StrandData *BKE_strand_data_calc(Strands *strands, DerivedMesh *scalp,
                                 StrandFiber *fibers, int num_fibers)
{
	StrandData *data = MEM_callocN(sizeof(StrandData), "StrandData");
	
	data->totverts = strands->totverts;
	data->totcurves = strands->totcurves;
	data->totfibers = num_fibers;
	data->verts = MEM_mallocN(sizeof(StrandVertexData) * data->totverts, "StrandVertexData");
	data->curves = MEM_mallocN(sizeof(StrandCurveData) * data->totcurves, "StrandCurveData");
	data->fibers = MEM_mallocN(sizeof(StrandFiberData) * data->totfibers, "StrandFiberData");
	
	int c;
	StrandCurve *scurve = strands->curves;
	StrandCurveData *curve = data->curves;
	for (c = 0; c < data->totcurves; ++c, ++scurve, ++curve) {
		curve->verts_begin = scurve->verts_begin;
		curve->num_verts = scurve->num_verts;
		
		BKE_strands_get_matrix(scurve, scalp, curve->mat);
		
		int v;
		StrandVertex *svert = strands->verts + scurve->verts_begin;
		StrandVertexData *vert = data->verts + curve->verts_begin;
		for (v = 0; v < curve->num_verts; ++v, ++svert, ++vert) {
			mul_v3_m4v3(vert->co, curve->mat, svert->co);
		}
	}
	
	int i;
	StrandFiber *sfiber = fibers;
	StrandFiberData *fiber = data->fibers;
	for (i = 0; i < data->totfibers; ++i, ++sfiber, ++fiber) {
		float nor[3], tang[3];
		BKE_mesh_sample_eval(scalp, &sfiber->root, fiber->co, nor, tang);
		
		int k;
		for (k = 0; k < 4; ++k) {
			fiber->control_index[k] = sfiber->control_index[k];
			fiber->control_weight[k] = sfiber->control_weight[k];
		}
	}
	
	return data;
}

void BKE_strand_data_free(StrandData *data)
{
	if (data) {
		GPU_strands_buffer_free(data->gpu_buffer);
		
		if (data->verts)
			MEM_freeN(data->verts);
		if (data->curves)
			MEM_freeN(data->curves);
		if (data->fibers)
			MEM_freeN(data->fibers);
		MEM_freeN(data);
	}
}

/* ------------------------------------------------------------------------- */

void BKE_strands_test_init(struct Strands *strands, struct DerivedMesh *scalp,
                           int totcurves, int maxverts,
                           unsigned int seed)
{
	unsigned int seed2 = seed ^ 0xdeadbeef;
	RNG *rng = BLI_rng_new(seed2);
	
	MeshSampleGenerator *gen = BKE_mesh_sample_gen_surface_random(scalp, seed);
	unsigned int i, k;
	
	/* First generate all curves, to define vertex counts */
	StrandCurve *curves = MEM_mallocN(sizeof(StrandCurve) * totcurves, "StrandCurve buffer");
	StrandCurve *c = curves;
	unsigned int totverts = 0;
	for (i = 0; i < totcurves; ++i, ++c) {
		int num_verts = (int)(BLI_rng_get_float(rng) * (float)(maxverts + 1));
		CLAMP(num_verts, 2, maxverts);
		
		if (BKE_mesh_sample_generate(gen, &c->root)) {
			c->verts_begin = totverts;
			c->num_verts = num_verts;
			totverts += num_verts;
		}
		else {
			/* clear remaining samples */
			memset(c, 0, sizeof(StrandCurve) * totcurves - i);
			break;
		}
	}
	
	/* Now generate vertices */
	float segment_length = (maxverts > 1) ? 1.0f / (maxverts - 1) : 0.0f;
	
	StrandVertex *verts = MEM_mallocN(sizeof(StrandVertex) * totverts, "StrandVertex buffer");
	c = curves;
	StrandVertex *v = verts;
	for (i = 0; i < totcurves; ++i, ++c) {
		for (k = 0; k < c->num_verts; ++k, ++v) {
			v->co[0] = 0.0f;
			v->co[1] = 0.0f;
			v->co[2] = segment_length * k;
		}
	}
	
	BKE_mesh_sample_free_generator(gen);
	
	if (strands->curves)
		MEM_freeN(strands->curves);
	if (strands->verts)
		MEM_freeN(strands->verts);
	strands->curves = curves;
	strands->verts = verts;
	strands->totcurves = totcurves;
	strands->totverts = totverts;
	
	BLI_rng_free(rng);
}

BLI_INLINE void verify_fiber_weights(StrandFiber *fiber)
{
	const float *w = fiber->control_weight;
	
	BLI_assert(w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f && w[3] >= 0.0f);
	float sum = w[0] + w[1] + w[2] + w[3];
	float epsilon = 1.0e-2;
	BLI_assert(sum > 1.0f - epsilon && sum < 1.0f + epsilon);
	UNUSED_VARS(sum, epsilon);
	
	BLI_assert(w[0] >= w[1] && w[1] >= w[2] && w[2] >= w[3]);
}

static void sort_fiber_weights(StrandFiber *fiber)
{
	unsigned int *idx = fiber->control_index;
	float *w = fiber->control_weight;

#define FIBERSWAP(a, b) \
	SWAP(unsigned int, idx[a], idx[b]); \
	SWAP(float, w[a], w[b]);

	for (int k = 0; k < 3; ++k) {
		int maxi = k;
		float maxw = w[k];
		for (int i = k+1; i < 4; ++i) {
			if (w[i] > maxw) {
				maxi = i;
				maxw = w[i];
			}
		}
		if (maxi != k)
			FIBERSWAP(k, maxi);
	}
	
#undef FIBERSWAP
}

static void strands_calc_weights(const Strands *strands, struct DerivedMesh *scalp, StrandFiber *fibers, int num_fibers)
{
	float (*strandloc)[3] = MEM_mallocN(sizeof(float) * 3 * strands->totcurves, "strand locations");
	KDTree *tree = BLI_kdtree_new(strands->totcurves);
	
	{
		int c;
		const StrandCurve *curve = strands->curves;
		for (c = 0; c < strands->totcurves; ++c, ++curve) {
			if (BKE_strands_get_location(curve, scalp, strandloc[c]))
				BLI_kdtree_insert(tree, c, strandloc[c]);
		}
		BLI_kdtree_balance(tree);
	}
	
	int i;
	StrandFiber *fiber = fibers;
	for (i = 0; i < num_fibers; ++i, ++fiber) {
		float loc[3], nor[3], tang[3];
		if (BKE_mesh_sample_eval(scalp, &fiber->root, loc, nor, tang)) {
			
			/* Use the 3 closest strands for interpolation.
			 * Note that we have up to 4 possible weights, but we
			 * only look for a triangle with this method.
			 */
			KDTreeNearest nearest[3];
			float *sloc[3] = {NULL};
			int k, found = BLI_kdtree_find_nearest_n(tree, loc, nearest, 3);
			for (k = 0; k < found; ++k) {
				fiber->control_index[k] = nearest[k].index;
				sloc[k] = strandloc[nearest[k].index];
			}
			
			/* calculate barycentric interpolation weights */
			if (found == 3) {
				float closest[3];
				closest_on_tri_to_point_v3(closest, loc, sloc[0], sloc[1], sloc[2]);
				
				float w[4];
				interp_weights_face_v3(w, sloc[0], sloc[1], sloc[2], NULL, closest);
				copy_v3_v3(fiber->control_weight, w);
				/* float precisions issues can cause slightly negative weights */
				CLAMP3(fiber->control_weight, 0.0f, 1.0f);
			}
			else if (found == 2) {
				fiber->control_weight[1] = line_point_factor_v3(loc, sloc[0], sloc[1]);
				fiber->control_weight[0] = 1.0f - fiber->control_weight[1];
				/* float precisions issues can cause slightly negative weights */
				CLAMP2(fiber->control_weight, 0.0f, 1.0f);
			}
			else if (found == 1) {
				fiber->control_weight[0] = 1.0f;
			}
			
			sort_fiber_weights(fiber);
			verify_fiber_weights(fiber);
		}
	}
	
	BLI_kdtree_free(tree);
	MEM_freeN(strandloc);
}

StrandFiber *BKE_strands_scatter(Strands *strands,
                                struct DerivedMesh *scalp, unsigned int amount,
                                unsigned int seed)
{
	MeshSampleGenerator *gen = BKE_mesh_sample_gen_surface_random(scalp, seed);
	unsigned int i;
	
	StrandFiber *fibers = MEM_mallocN(sizeof(StrandFiber) * amount, "StrandFiber");
	StrandFiber *fiber;
	
	for (i = 0, fiber = fibers; i < amount; ++i, ++fiber) {
		if (BKE_mesh_sample_generate(gen, &fiber->root)) {
			int k;
			/* influencing control strands are determined later */
			for (k = 0; k < 4; ++k) {
				fiber->control_index[k] = STRAND_INDEX_NONE;
				fiber->control_weight[k] = 0.0f;
			}
		}
		else {
			/* clear remaining samples */
			memset(fiber, 0, sizeof(StrandFiber) * amount - i);
			break;
		}
	}
	
	BKE_mesh_sample_free_generator(gen);
	
	strands_calc_weights(strands, scalp, fibers, amount);
	
	return fibers;
}

#if 0
StrandData *BKE_strand_data_interpolate(StrandInfo *strands, unsigned int num_strands,
                                        const StrandCurve *controls, struct StrandCurveParams *params)
{
	StrandData *data = MEM_callocN(sizeof(StrandData), "strand interpolation data");
	StrandInfo *s;
	StrandCurve *c;
	StrandVertex *v;
	unsigned int verts_begin;
	unsigned int i;
	
	data->totcurves = num_strands;
	data->totverts = num_strands * params->max_verts;
	data->curves = MEM_mallocN(sizeof(StrandCurve) * data->totcurves, "strand curves");
	data->verts = MEM_mallocN(sizeof(StrandVertex) * data->totverts, "strand vertices");
	
	UNUSED_VARS(controls);
	verts_begin = 0;
	v = data->verts;
	for (i = 0, s = strands, c = data->curves; i < num_strands; ++i) {
		unsigned int k;
		
		c->num_verts = params->max_verts;
		c->verts_begin = verts_begin;
		
		if (params->scalp) {
			BKE_mesh_sample_eval(params->scalp, &s->root, c->rootmat[3], c->rootmat[2], c->rootmat[0]);
			cross_v3_v3v3(c->rootmat[1], c->rootmat[2], c->rootmat[0]);
		}
		else {
			unit_m4(c->rootmat);
		}
		
		for (k = 0; k < c->num_verts; ++k, ++v) {
			v->co[0] = 0.0f;
			v->co[1] = 0.0f;
			if (c->num_verts > 1)
				v->co[2] = k / (c->num_verts - 1);
		}
		
		verts_begin += c->num_verts;
	}
	
	return data;
}

void BKE_strand_data_free(StrandData *data)
{
	if (data) {
		if (data->curves)
			MEM_freeN(data->curves);
		if (data->verts)
			MEM_freeN(data->verts);
		MEM_freeN(data);
	}
}
#endif
