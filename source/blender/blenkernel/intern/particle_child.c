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

/** \file blender/blenkernel/intern/particle_child.c
 *  \ingroup bke
 */

#include "BLI_math.h"
#include "BLI_noise.h"

#include "DNA_material_types.h"

#include "BKE_particle.h"

struct Material;

void do_kink(ParticleKey *state, ParticleKey *par, float *par_rot, float time, float freq, float shape, float amplitude, float flat,
             short type, short axis, float obmat[4][4], int smooth_start);
float do_clump(ParticleKey *state, ParticleKey *par, float time, float clumpfac, float clumppow, float pa_clump);
void do_child_modifiers(ParticleSimulationData *sim,
                        ParticleTexture *ptex, ParticleKey *par, float *par_rot, ChildParticle *cpa,
                        const float orco[3], float mat[4][4], ParticleKey *state, float t);

static void get_strand_normal(Material *ma, const float surfnor[3], float surfdist, float nor[3])
{
	float cross[3], nstrand[3], vnor[3], blend;

	if (!((ma->mode & MA_STR_SURFDIFF) || (ma->strand_surfnor > 0.0f)))
		return;

	if (ma->mode & MA_STR_SURFDIFF) {
		cross_v3_v3v3(cross, surfnor, nor);
		cross_v3_v3v3(nstrand, nor, cross);

		blend = dot_v3v3(nstrand, surfnor);
		CLAMP(blend, 0.0f, 1.0f);

		interp_v3_v3v3(vnor, nstrand, surfnor, blend);
		normalize_v3(vnor);
	}
	else {
		copy_v3_v3(vnor, nor);
	}
	
	if (ma->strand_surfnor > 0.0f) {
		if (ma->strand_surfnor > surfdist) {
			blend = (ma->strand_surfnor - surfdist) / ma->strand_surfnor;
			interp_v3_v3v3(vnor, vnor, surfnor, blend);
			normalize_v3(vnor);
		}
	}

	copy_v3_v3(nor, vnor);
}

/* ------------------------------------------------------------------------- */

typedef struct ParticlePathIterator {
	ParticleCacheKey *key;
	int index;
	float time;
	
	ParticleCacheKey *parent_key;
	float parent_rotation[4];
} ParticlePathIterator;

static void psys_path_iter_get(ParticlePathIterator *iter, ParticleCacheKey *keys, int totkeys, ParticleCacheKey *parent, int index)
{
	BLI_assert(index >= 0 && index < totkeys);
	
	iter->key = keys + index;
	iter->index = index;
	iter->time = (float)index / (float)(totkeys - 1);
	
	if (parent) {
		iter->parent_key = parent + index;
		if (index > 0)
			mul_qt_qtqt(iter->parent_rotation, iter->parent_key->rot, parent->rot);
		else
			copy_qt_qt(iter->parent_rotation, parent->rot);
	}
	else {
		iter->parent_key = NULL;
		unit_qt(iter->parent_rotation);
	}
}

typedef struct ParticlePathModifier {
	struct ParticlePathModifier *next, *prev;
	
	void (*apply)(ParticleCacheKey *keys, int totkeys, ParticleCacheKey *parent_keys);
} ParticlePathModifier;

/* ------------------------------------------------------------------------- */

static bool check_path_length(int k, ParticleCacheKey *keys, ParticleCacheKey *key, float max_length, float step_length, float *cur_length, float dvec[3])
{
	if (*cur_length + step_length > max_length) {
		sub_v3_v3v3(dvec, key->co, (key-1)->co);
		mul_v3_fl(dvec, (max_length - *cur_length) / step_length);
		add_v3_v3v3(key->co, (key-1)->co, dvec);
		keys->steps = k;
		/* something over the maximum step value */
		return false;
	}
	else {
		*cur_length += step_length;
		return true;
	}
}

void psys_apply_child_modifiers(ParticleThreadContext *ctx, struct ListBase *modifiers,
                                ChildParticle *cpa, ParticleTexture *ptex, const float orco[3], const float ornor[3], float hairmat[4][4],
                                ParticleCacheKey *keys, ParticleCacheKey *parent_keys)
{
	struct ParticleSettings *part = ctx->sim.psys->part;
	struct Material *ma = ctx->ma;
	const bool draw_col_ma = (part->draw_col == PART_DRAW_COL_MAT);
	
	const int totkeys = ctx->steps + 1;
	const float step_length = 1.0f / (float)ctx->steps;
	const float max_length = ptex->length;
	
	ParticlePathModifier *mod;
	ParticleCacheKey *key;
	int k;
	float cur_length;
	
#if 0 /* TODO for the future: use true particle modifiers that work on the whole curve */
	for (mod = modifiers->first; mod; mod = mod->next) {
		mod->apply(keys, totkeys, parent_keys);
	}
#else
	(void)modifiers;
	(void)mod;
	
	{
		ParticlePathIterator iter;
		for (k = 0, key = keys; k < totkeys; k++, key++) {
			psys_path_iter_get(&iter, keys, totkeys, parent_keys, k);
			
			/* apply different deformations to the child path */
			do_child_modifiers(&ctx->sim, ptex, (ParticleKey *)iter.parent_key, iter.parent_rotation, cpa, orco, hairmat, (ParticleKey *)key, iter.time);
		}
	}
#endif
	
	cur_length = 0.0f;
	/* we have to correct velocity because of kink & clump */
	for (k = 0, key = keys; k < totkeys; ++k, ++key) {
		if (k >= 2) {
			sub_v3_v3v3((key-1)->vel, key->co, (key-2)->co);
			mul_v3_fl((key-1)->vel, 0.5);
			
			if (ma && draw_col_ma)
				get_strand_normal(ma, ornor, cur_length, (key-1)->vel);
		}
		else if (k == totkeys-1) {
			/* last key */
			sub_v3_v3v3(key->vel, key->co, (key-1)->co);
		}
		
		if (k > 1) {
			float dvec[3];
			/* check if path needs to be cut before actual end of data points */
			if (!check_path_length(k, keys, key, max_length, step_length, &cur_length, dvec))
				break;
		}
		
		if (ma && draw_col_ma) {
			copy_v3_v3(key->col, &ma->r);
			get_strand_normal(ma, ornor, cur_length, key->vel);
		}
	}
}

/* ------------------------------------------------------------------------- */

void do_kink(ParticleKey *state, ParticleKey *par, float *par_rot, float time, float freq, float shape, float amplitude, float flat, short type, short axis, float obmat[4][4], int smooth_start)
{
	float kink[3] = {1.f, 0.f, 0.f}, par_vec[3], q1[4] = {1.f, 0.f, 0.f, 0.f};
	float t, dt = 1.f, result[3];

	if (par == NULL || type == PART_KINK_NO)
		return;

	CLAMP(time, 0.f, 1.f);

	if (shape != 0.0f && type != PART_KINK_BRAID) {
		if (shape < 0.0f)
			time = (float)pow(time, 1.f + shape);
		else
			time = (float)pow(time, 1.f / (1.f - shape));
	}

	t = time * freq * (float)M_PI;
	
	if (smooth_start) {
		dt = fabsf(t);
		/* smooth the beginning of kink */
		CLAMP(dt, 0.f, (float)M_PI);
		dt = sinf(dt / 2.f);
	}

	if (type != PART_KINK_RADIAL) {
		float temp[3];

		kink[axis] = 1.f;

		if (obmat)
			mul_mat3_m4_v3(obmat, kink);
		
		if (par_rot)
			mul_qt_v3(par_rot, kink);

		/* make sure kink is normal to strand */
		project_v3_v3v3(temp, kink, par->vel);
		sub_v3_v3(kink, temp);
		normalize_v3(kink);
	}

	copy_v3_v3(result, state->co);
	sub_v3_v3v3(par_vec, par->co, state->co);

	switch (type) {
		case PART_KINK_CURL:
		{
			float curl_offset[3];
			
			/* rotate kink vector around strand tangent */
			mul_v3_v3fl(curl_offset, kink, amplitude);
			axis_angle_to_quat(q1, par->vel, t);
			mul_qt_v3(q1, curl_offset);
			
			interp_v3_v3v3(par_vec, state->co, par->co, flat);
			add_v3_v3v3(result, par_vec, curl_offset);
			break;
		}
		case PART_KINK_RADIAL:
		{
			if (flat > 0.f) {
				float proj[3];
				/* flatten along strand */
				project_v3_v3v3(proj, par_vec, par->vel);
				madd_v3_v3fl(result, proj, flat);
			}

			madd_v3_v3fl(result, par_vec, -amplitude * sinf(t));
			break;
		}
		case PART_KINK_WAVE:
		{
			madd_v3_v3fl(result, kink, amplitude * sinf(t));

			if (flat > 0.f) {
				float proj[3];
				/* flatten along wave */
				project_v3_v3v3(proj, par_vec, kink);
				madd_v3_v3fl(result, proj, flat);

				/* flatten along strand */
				project_v3_v3v3(proj, par_vec, par->vel);
				madd_v3_v3fl(result, proj, flat);
			}
			break;
		}
		case PART_KINK_BRAID:
		{
			float y_vec[3] = {0.f, 1.f, 0.f};
			float z_vec[3] = {0.f, 0.f, 1.f};
			float vec_one[3], state_co[3];
			float inp_y, inp_z, length;
		
			if (par_rot) {
				mul_qt_v3(par_rot, y_vec);
				mul_qt_v3(par_rot, z_vec);
			}

			negate_v3(par_vec);
			normalize_v3_v3(vec_one, par_vec);

			inp_y = dot_v3v3(y_vec, vec_one);
			inp_z = dot_v3v3(z_vec, vec_one);

			if (inp_y > 0.5f) {
				copy_v3_v3(state_co, y_vec);

				mul_v3_fl(y_vec, amplitude * cosf(t));
				mul_v3_fl(z_vec, amplitude / 2.f * sinf(2.f * t));
			}
			else if (inp_z > 0.0f) {
				mul_v3_v3fl(state_co, z_vec, sinf((float)M_PI / 3.f));
				madd_v3_v3fl(state_co, y_vec, -0.5f);

				mul_v3_fl(y_vec, -amplitude * cosf(t + (float)M_PI / 3.f));
				mul_v3_fl(z_vec, amplitude / 2.f * cosf(2.f * t + (float)M_PI / 6.f));
			}
			else {
				mul_v3_v3fl(state_co, z_vec, -sinf((float)M_PI / 3.f));
				madd_v3_v3fl(state_co, y_vec, -0.5f);

				mul_v3_fl(y_vec, amplitude * -sinf(t + (float)M_PI / 6.f));
				mul_v3_fl(z_vec, amplitude / 2.f * -sinf(2.f * t + (float)M_PI / 3.f));
			}

			mul_v3_fl(state_co, amplitude);
			add_v3_v3(state_co, par->co);
			sub_v3_v3v3(par_vec, state->co, state_co);

			length = normalize_v3(par_vec);
			mul_v3_fl(par_vec, MIN2(length, amplitude / 2.f));

			add_v3_v3v3(state_co, par->co, y_vec);
			add_v3_v3(state_co, z_vec);
			add_v3_v3(state_co, par_vec);

			shape = 2.f * (float)M_PI * (1.f + shape);

			if (t < shape) {
				shape = t / shape;
				shape = (float)sqrt((double)shape);
				interp_v3_v3v3(result, result, state_co, shape);
			}
			else {
				copy_v3_v3(result, state_co);
			}
			break;
		}
	}

	/* blend the start of the kink */
	if (dt < 1.f)
		interp_v3_v3v3(state->co, state->co, result, dt);
	else
		copy_v3_v3(state->co, result);
}

float do_clump(ParticleKey *state, ParticleKey *par, float time, float clumpfac, float clumppow, float pa_clump)
{
	float clump = 0.f;

	if (par && clumpfac != 0.0f) {
		float cpow;

		if (clumppow < 0.0f)
			cpow = 1.0f + clumppow;
		else
			cpow = 1.0f + 9.0f * clumppow;

		if (clumpfac < 0.0f) /* clump roots instead of tips */
			clump = -clumpfac * pa_clump * (float)pow(1.0 - (double)time, (double)cpow);
		else
			clump = clumpfac * pa_clump * (float)pow((double)time, (double)cpow);

		interp_v3_v3v3(state->co, state->co, par->co, clump);
	}

	return clump;
}

static void do_rough(const float loc[3], float mat[4][4], float t, float fac, float size, float thres, ParticleKey *state)
{
	float rough[3];
	float rco[3];

	if (thres != 0.0f) {
		if (fabsf((float)(-1.5f + loc[0] + loc[1] + loc[2])) < 1.5f * thres) {
			return;
		}
	}

	copy_v3_v3(rco, loc);
	mul_v3_fl(rco, t);
	rough[0] = -1.0f + 2.0f * BLI_gTurbulence(size, rco[0], rco[1], rco[2], 2, 0, 2);
	rough[1] = -1.0f + 2.0f * BLI_gTurbulence(size, rco[1], rco[2], rco[0], 2, 0, 2);
	rough[2] = -1.0f + 2.0f * BLI_gTurbulence(size, rco[2], rco[0], rco[1], 2, 0, 2);

	madd_v3_v3fl(state->co, mat[0], fac * rough[0]);
	madd_v3_v3fl(state->co, mat[1], fac * rough[1]);
	madd_v3_v3fl(state->co, mat[2], fac * rough[2]);
}

static void do_rough_end(float *loc, float mat[4][4], float t, float fac, float shape, ParticleKey *state)
{
	float rough[2];
	float roughfac;

	roughfac = fac * (float)pow((double)t, shape);
	copy_v2_v2(rough, loc);
	rough[0] = -1.0f + 2.0f * rough[0];
	rough[1] = -1.0f + 2.0f * rough[1];
	mul_v2_fl(rough, roughfac);

	madd_v3_v3fl(state->co, mat[0], rough[0]);
	madd_v3_v3fl(state->co, mat[1], rough[1]);
}

void do_child_modifiers(ParticleSimulationData *sim, ParticleTexture *ptex, ParticleKey *par, float *par_rot, ChildParticle *cpa, const float orco[3], float mat[4][4], ParticleKey *state, float t)
{
	ParticleSettings *part = sim->psys->part;
	int i = cpa - sim->psys->child;
	int guided = 0;

	float kink_freq = part->kink_freq;
	float rough1 = part->rough1;
	float rough2 = part->rough2;
	float rough_end = part->rough_end;

	if (ptex) {
		kink_freq *= ptex->kink;
		rough1 *= ptex->rough1;
		rough2 *= ptex->rough2;
		rough_end *= ptex->roughe;
	}

	if (part->flag & PART_CHILD_EFFECT)
		/* state is safe to cast, since only co and vel are used */
		guided = do_guides(sim->psys->effectors, (ParticleKey *)state, cpa->parent, t);

	if (guided == 0) {
		float clump = do_clump(state, par, t, part->clumpfac, part->clumppow, ptex ? ptex->clump : 1.f);

		if (kink_freq != 0.f) {
			float kink_amp = part->kink_amp * (1.f - part->kink_amp_clump * clump);

			do_kink(state, par, par_rot, t, kink_freq, part->kink_shape,
			        kink_amp, part->kink_flat, part->kink, part->kink_axis,
			        sim->ob->obmat, sim->psys->part->childtype == PART_CHILD_FACES);
		}
	}

	if (rough1 > 0.f)
		do_rough(orco, mat, t, rough1, part->rough1_size, 0.0, state);

	if (rough2 > 0.f) {
		float vec[3];
		psys_frand_vec(sim->psys, i + 27, vec);
		do_rough(vec, mat, t, rough2, part->rough2_size, part->rough2_thres, state);
	}

	if (rough_end > 0.f) {
		float vec[3];
		psys_frand_vec(sim->psys, i + 27, vec);
		do_rough_end(vec, mat, t, rough_end, part->rough_end_shape, state);
	}
}
