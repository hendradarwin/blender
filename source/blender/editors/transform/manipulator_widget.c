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
 * The Original Code is Copyright (C) 2005 Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/transform/transform_manipulator.c
 *  \ingroup edtransform
 */


#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "DNA_armature_types.h"
#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"
#include "DNA_meta_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "RNA_access.h"

#include "BKE_action.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_global.h"
#include "BKE_particle.h"
#include "BKE_pointcache.h"
#include "BKE_editmesh.h"
#include "BKE_lattice.h"

#include "BIF_gl.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm.h"

#include "ED_armature.h"
#include "ED_curve.h"
#include "ED_object.h"
#include "ED_particle.h"
#include "ED_view3d.h"
#include "ED_screen.h"

#include "UI_resources.h"
#include "UI_interface.h"

/* local module include */
#include "transform.h"

#include "MEM_guardedalloc.h"

#include "GPU_select.h"

/* drawing flags */

#define MAN_TRANS_X		(1 << 0)
#define MAN_TRANS_Y		(1 << 1)
#define MAN_TRANS_Z		(1 << 2)
#define MAN_TRANS_C		(MAN_TRANS_X | MAN_TRANS_Y | MAN_TRANS_Z)

#define MAN_ROT_X		(1 << 3)
#define MAN_ROT_Y		(1 << 4)
#define MAN_ROT_Z		(1 << 5)
#define MAN_ROT_V		(1 << 6)
#define MAN_ROT_T		(1 << 7)
#define MAN_ROT_C		(MAN_ROT_X | MAN_ROT_Y | MAN_ROT_Z | MAN_ROT_V | MAN_ROT_T)

#define MAN_SCALE_X		(1 << 8)
#define MAN_SCALE_Y		(1 << 9)
#define MAN_SCALE_Z		(1 << 10)
#define MAN_SCALE_C		(MAN_SCALE_X | MAN_SCALE_Y | MAN_SCALE_Z)

/* return codes for select */
enum {
	MAN_SEL_TRANS_X	= 0,
	MAN_SEL_TRANS_Y,
	MAN_SEL_TRANS_Z,

	MAN_SEL_ROT_X,
	MAN_SEL_ROT_Y,
	MAN_SEL_ROT_Z,
	MAN_SEL_ROT_V,
	MAN_SEL_ROT_T,

	MAN_SEL_SCALE_X,
	MAN_SEL_SCALE_Y,
	MAN_SEL_SCALE_Z,

	/* those two stay at the end so the rest can be inferred with bitshifting */
	MAN_SEL_SCALE_C,
	MAN_SEL_TRANS_C,

	MAN_SEL_MAX
};

/* axes as index - XXX combine with return codes for select */
enum {
	MAN_AXIS_TRANS_X = 0,
	MAN_AXIS_TRANS_Y,
	MAN_AXIS_TRANS_Z,

	MAN_AXIS_ROT_X,
	MAN_AXIS_ROT_Y,
	MAN_AXIS_ROT_Z,
};

/* axis types */
enum {
	MAN_AXES_ALL = 0,
	MAN_AXES_TRANSLATE,
	MAN_AXES_ROTATE,
};

/* color codes */

#define MAN_RGB     0
#define MAN_GHOST   1
#define MAN_MOVECOL 2

/* threshold for testing view aligned manipulator axis */
#define TW_AXIS_DOT_MIN 0.02f
#define TW_AXIS_DOT_MAX 0.1f

/* loop over axes of given type */
#define MAN_ITER_AXES_BEGIN(axis_type) \
	for (i = (axis_type == MAN_AXES_ROTATE ? MAN_AXIS_ROT_X : MAN_AXIS_TRANS_X); \
	     i < (axis_type == MAN_AXES_TRANSLATE ? MAN_AXIS_TRANS_Z + 1 : MAN_AXIS_ROT_Z + 1); \
	     i++) \
	{ \
		axis = manipulator_get_axis_from_index(manipulator, i);

#define MAN_ITER_AXES_END \
	} (void)0

static wmWidget *manipulator_get_axis_from_index(const ManipulatorGroup *manipulator, const short index)
{
	wmWidget *axis = NULL;

	BLI_assert(IN_RANGE_INCL(index, 0.0f, 5.0f));

	switch (index) {
		case MAN_AXIS_TRANS_X:
			axis = manipulator->translate_x;
			break;
		case MAN_AXIS_TRANS_Y:
			axis = manipulator->translate_y;
			break;
		case MAN_AXIS_TRANS_Z:
			axis = manipulator->translate_z;
			break;
		case MAN_AXIS_ROT_X:
			axis = manipulator->rotate_x;
			break;
		case MAN_AXIS_ROT_Y:
			axis = manipulator->rotate_y;
			break;
		case MAN_AXIS_ROT_Z:
			axis = manipulator->rotate_z;
			break;
	}

	return axis;
}

/* transform widget center calc helper for below */
static void calc_tw_center(Scene *scene, const float co[3])
{
	float *twcent = scene->twcent;
	float *min = scene->twmin;
	float *max = scene->twmax;

	minmax_v3v3_v3(min, max, co);
	add_v3_v3(twcent, co);
}

static void protectflag_to_drawflags(short protectflag, short *drawflags)
{
	if (protectflag & OB_LOCK_LOCX)
		*drawflags &= ~MAN_TRANS_X;
	if (protectflag & OB_LOCK_LOCY)
		*drawflags &= ~MAN_TRANS_Y;
	if (protectflag & OB_LOCK_LOCZ)
		*drawflags &= ~MAN_TRANS_Z;

	if (protectflag & OB_LOCK_ROTX)
		*drawflags &= ~MAN_ROT_X;
	if (protectflag & OB_LOCK_ROTY)
		*drawflags &= ~MAN_ROT_Y;
	if (protectflag & OB_LOCK_ROTZ)
		*drawflags &= ~MAN_ROT_Z;

	if (protectflag & OB_LOCK_SCALEX)
		*drawflags &= ~MAN_SCALE_X;
	if (protectflag & OB_LOCK_SCALEY)
		*drawflags &= ~MAN_SCALE_Y;
	if (protectflag & OB_LOCK_SCALEZ)
		*drawflags &= ~MAN_SCALE_Z;
}

/* for pose mode */
static void stats_pose(Scene *scene, RegionView3D *rv3d, bPoseChannel *pchan)
{
	Bone *bone = pchan->bone;

	if (bone) {
		calc_tw_center(scene, pchan->pose_head);
		protectflag_to_drawflags(pchan->protectflag, &rv3d->twdrawflag);
	}
}

/* for editmode*/
static void stats_editbone(RegionView3D *rv3d, const EditBone *ebo)
{
	if (ebo->flag & BONE_EDITMODE_LOCKED)
		protectflag_to_drawflags(OB_LOCK_LOC | OB_LOCK_ROT | OB_LOCK_SCALE, &rv3d->twdrawflag);
}

/* could move into BLI_math however this is only useful for display/editing purposes */
static void axis_angle_to_gimbal_axis(float gmat[3][3], const float axis[3], const float angle)
{
	/* X/Y are arbitrary axies, most importantly Z is the axis of rotation */

	float cross_vec[3];
	float quat[4];

	/* this is an un-scientific method to get a vector to cross with
	 * XYZ intentionally YZX */
	cross_vec[0] = axis[1];
	cross_vec[1] = axis[2];
	cross_vec[2] = axis[0];

	/* X-axis */
	cross_v3_v3v3(gmat[0], cross_vec, axis);
	normalize_v3(gmat[0]);
	axis_angle_to_quat(quat, axis, angle);
	mul_qt_v3(quat, gmat[0]);

	/* Y-axis */
	axis_angle_to_quat(quat, axis, M_PI / 2.0);
	copy_v3_v3(gmat[1], gmat[0]);
	mul_qt_v3(quat, gmat[1]);

	/* Z-axis */
	copy_v3_v3(gmat[2], axis);

	normalize_m3(gmat);
}


static int test_rotmode_euler(short rotmode)
{
	return (ELEM(rotmode, ROT_MODE_AXISANGLE, ROT_MODE_QUAT)) ? 0 : 1;
}

bool gimbal_axis(Object *ob, float gmat[3][3])
{
	if (ob) {
		if (ob->mode & OB_MODE_POSE) {
			bPoseChannel *pchan = BKE_pose_channel_active(ob);

			if (pchan) {
				float mat[3][3], tmat[3][3], obmat[3][3];
				if (test_rotmode_euler(pchan->rotmode)) {
					eulO_to_gimbal_axis(mat, pchan->eul, pchan->rotmode);
				}
				else if (pchan->rotmode == ROT_MODE_AXISANGLE) {
					axis_angle_to_gimbal_axis(mat, pchan->rotAxis, pchan->rotAngle);
				}
				else { /* quat */
					return 0;
				}


				/* apply bone transformation */
				mul_m3_m3m3(tmat, pchan->bone->bone_mat, mat);

				if (pchan->parent) {
					float parent_mat[3][3];

					copy_m3_m4(parent_mat, pchan->parent->pose_mat);
					mul_m3_m3m3(mat, parent_mat, tmat);

					/* needed if object transformation isn't identity */
					copy_m3_m4(obmat, ob->obmat);
					mul_m3_m3m3(gmat, obmat, mat);
				}
				else {
					/* needed if object transformation isn't identity */
					copy_m3_m4(obmat, ob->obmat);
					mul_m3_m3m3(gmat, obmat, tmat);
				}

				normalize_m3(gmat);
				return 1;
			}
		}
		else {
			if (test_rotmode_euler(ob->rotmode)) {
				eulO_to_gimbal_axis(gmat, ob->rot, ob->rotmode);
			}
			else if (ob->rotmode == ROT_MODE_AXISANGLE) {
				axis_angle_to_gimbal_axis(gmat, ob->rotAxis, ob->rotAngle);
			}
			else { /* quat */
				return 0;
			}

			if (ob->parent) {
				float parent_mat[3][3];
				copy_m3_m4(parent_mat, ob->parent->obmat);
				normalize_m3(parent_mat);
				mul_m3_m3m3(gmat, parent_mat, gmat);
			}
			return 1;
		}
	}

	return 0;
}


/* centroid, boundbox, of selection */
/* returns total items selected */
static int calc_manipulator_stats(const bContext *C)
{
	const ScrArea *sa = CTX_wm_area(C);
	const ARegion *ar = CTX_wm_region(C);
	const ToolSettings *ts = CTX_data_tool_settings(C);
	const View3D *v3d = sa->spacedata.first;
	RegionView3D *rv3d = ar->regiondata;
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	Object *ob = OBACT;
	Base *base;
	int a, totsel = 0;

	/* transform widget matrix */
	unit_m4(rv3d->twmat);

	rv3d->twdrawflag = 0xFFFF;

	/* transform widget centroid/center */
	INIT_MINMAX(scene->twmin, scene->twmax);
	zero_v3(scene->twcent);

	if (obedit) {
		ob = obedit;
		if ((ob->lay & v3d->lay) == 0) return 0;

		if (obedit->type == OB_MESH) {
			BMEditMesh *em = BKE_editmesh_from_object(obedit);
			BMEditSelection ese;
			float vec[3] = {0, 0, 0};

			/* USE LAST SELECTE WITH ACTIVE */
			if ((v3d->around == V3D_ACTIVE) && BM_select_history_active_get(em->bm, &ese)) {
				BM_editselection_center(&ese, vec);
				calc_tw_center(scene, vec);
				totsel = 1;
			}
			else {
				BMesh *bm = em->bm;
				BMVert *eve;

				BMIter iter;

				/* do vertices/edges/faces for center depending on selection
				 * mode. note we can't use just vertex selection flag because
				 * it is not flush down on changes */
				if (ts->selectmode & SCE_SELECT_VERTEX) {
					BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
						if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
							if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
								totsel++;
								calc_tw_center(scene, eve->co);
							}
						}
					}
				}
				else if (ts->selectmode & SCE_SELECT_EDGE) {
					BMIter itersub;
					BMEdge *eed;
					BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
						if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
							/* check the vertex has a selected edge, only add it once */
							BM_ITER_ELEM (eed, &itersub, eve, BM_EDGES_OF_VERT) {
								if (BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
									totsel++;
									calc_tw_center(scene, eve->co);
									break;
								}
							}
						}
					}
				}
				else {
					BMIter itersub;
					BMFace *efa;
					BM_ITER_MESH (eve, &iter, bm, BM_VERTS_OF_MESH) {
						if (!BM_elem_flag_test(eve, BM_ELEM_HIDDEN)) {
							/* check the vertex has a selected face, only add it once */
							BM_ITER_ELEM (efa, &itersub, eve, BM_FACES_OF_VERT) {
								if (BM_elem_flag_test(efa, BM_ELEM_SELECT)) {
									totsel++;
									calc_tw_center(scene, eve->co);
									break;
								}
							}
						}
					}
				}
			}
		} /* end editmesh */
		else if (obedit->type == OB_ARMATURE) {
			const bArmature *arm = obedit->data;
			EditBone *ebo;

			if ((v3d->around == V3D_ACTIVE) && (ebo = arm->act_edbone)) {
				/* doesn't check selection or visibility intentionally */
				if (ebo->flag & BONE_TIPSEL) {
					calc_tw_center(scene, ebo->tail);
					totsel++;
				}
				if ((ebo->flag & BONE_ROOTSEL) ||
				    ((ebo->flag & BONE_TIPSEL) == false))  /* ensure we get at least one point */
				{
					calc_tw_center(scene, ebo->head);
					totsel++;
				}
				stats_editbone(rv3d, ebo);
			}
			else {
				for (ebo = arm->edbo->first; ebo; ebo = ebo->next) {
					if (EBONE_VISIBLE(arm, ebo)) {
						if (ebo->flag & BONE_TIPSEL) {
							calc_tw_center(scene, ebo->tail);
							totsel++;
						}
						if (ebo->flag & BONE_ROOTSEL) {
							calc_tw_center(scene, ebo->head);
							totsel++;
						}
						if (ebo->flag & BONE_SELECTED) {
							stats_editbone(rv3d, ebo);
						}
					}
				}
			}
		}
		else if (ELEM(obedit->type, OB_CURVE, OB_SURF)) {
			Curve *cu = obedit->data;
			float center[3];

			if (v3d->around == V3D_ACTIVE && ED_curve_active_center(cu, center)) {
				calc_tw_center(scene, center);
				totsel++;
			}
			else {
				Nurb *nu;
				BezTriple *bezt;
				BPoint *bp;
				const ListBase *nurbs = BKE_curve_editNurbs_get(cu);

				nu = nurbs->first;
				while (nu) {
					if (nu->type == CU_BEZIER) {
						bezt = nu->bezt;
						a = nu->pntsu;
						while (a--) {
							/* exceptions
							 * if handles are hidden then only check the center points.
							 * If the center knot is selected then only use this as the center point.
							 */
							if (cu->drawflag & CU_HIDE_HANDLES) {
								if (bezt->f2 & SELECT) {
									calc_tw_center(scene, bezt->vec[1]);
									totsel++;
								}
							}
							else if (bezt->f2 & SELECT) {
								calc_tw_center(scene, bezt->vec[1]);
								totsel++;
							}
							else {
								if (bezt->f1 & SELECT) {
									calc_tw_center(scene, bezt->vec[(v3d->around == V3D_LOCAL) ? 1 : 0]);
									totsel++;
								}
								if (bezt->f3 & SELECT) {
									calc_tw_center(scene, bezt->vec[(v3d->around == V3D_LOCAL) ? 1 : 2]);
									totsel++;
								}
							}
							bezt++;
						}
					}
					else {
						bp = nu->bp;
						a = nu->pntsu * nu->pntsv;
						while (a--) {
							if (bp->f1 & SELECT) {
								calc_tw_center(scene, bp->vec);
								totsel++;
							}
							bp++;
						}
					}
					nu = nu->next;
				}
			}
		}
		else if (obedit->type == OB_MBALL) {
			MetaBall *mb = (MetaBall *)obedit->data;
			MetaElem *ml;

			if ((v3d->around == V3D_ACTIVE) && (ml = mb->lastelem)) {
				calc_tw_center(scene, &ml->x);
				totsel++;
			}
			else {
				for (ml = mb->editelems->first; ml; ml = ml->next) {
					if (ml->flag & SELECT) {
						calc_tw_center(scene, &ml->x);
						totsel++;
					}
				}
			}
		}
		else if (obedit->type == OB_LATTICE) {
			Lattice *lt = ((Lattice *)obedit->data)->editlatt->latt;
			BPoint *bp;

			if ((v3d->around == V3D_ACTIVE) && (bp = BKE_lattice_active_point_get(lt))) {
				calc_tw_center(scene, bp->vec);
				totsel++;
			}
			else {
				bp = lt->def;
				a = lt->pntsu * lt->pntsv * lt->pntsw;
				while (a--) {
					if (bp->f1 & SELECT) {
						calc_tw_center(scene, bp->vec);
						totsel++;
					}
					bp++;
				}
			}
		}

		/* selection center */
		if (totsel) {
			mul_v3_fl(scene->twcent, 1.0f / (float)totsel);   // centroid!
			mul_m4_v3(obedit->obmat, scene->twcent);
			mul_m4_v3(obedit->obmat, scene->twmin);
			mul_m4_v3(obedit->obmat, scene->twmax);
		}
	}
	else if (ob && (ob->mode & OB_MODE_POSE)) {
		bPoseChannel *pchan;
		int mode = TFM_ROTATION; /* mislead counting bones... bah. We don't know the manipulator mode, could be mixed */
		bool ok = false;

		if ((ob->lay & v3d->lay) == 0) return 0;

		if ((v3d->around == V3D_ACTIVE) && (pchan = BKE_pose_channel_active(ob))) {
			/* doesn't check selection or visibility intentionally */
			Bone *bone = pchan->bone;
			if (bone) {
				stats_pose(scene, rv3d, pchan);
				totsel = 1;
				ok = true;
			}
		}
		else {
			totsel = count_set_pose_transflags(&mode, 0, ob);

			if (totsel) {
				/* use channels to get stats */
				for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
					Bone *bone = pchan->bone;
					if (bone && (bone->flag & BONE_TRANSFORM)) {
						stats_pose(scene, rv3d, pchan);
					}
				}
				ok = true;
			}
		}

		if (ok) {
			mul_v3_fl(scene->twcent, 1.0f / (float)totsel);   // centroid!
			mul_m4_v3(ob->obmat, scene->twcent);
			mul_m4_v3(ob->obmat, scene->twmin);
			mul_m4_v3(ob->obmat, scene->twmax);
		}
	}
	else if (ob && (ob->mode & OB_MODE_ALL_PAINT)) {
		/* pass */
	}
	else if (ob && ob->mode & OB_MODE_PARTICLE_EDIT) {
		const PTCacheEdit *edit = PE_get_current(scene, ob);
		PTCacheEditPoint *point;
		PTCacheEditKey *ek;
		int k;

		if (edit) {
			point = edit->points;
			for (a = 0; a < edit->totpoint; a++, point++) {
				if (point->flag & PEP_HIDE) continue;

				for (k = 0, ek = point->keys; k < point->totkey; k++, ek++) {
					if (ek->flag & PEK_SELECT) {
						calc_tw_center(scene, ek->flag & PEK_USE_WCO ? ek->world_co : ek->co);
						totsel++;
					}
				}
			}

			/* selection center */
			if (totsel)
				mul_v3_fl(scene->twcent, 1.0f / (float)totsel);  // centroid!
		}
	}
	else {
		/* we need the one selected object, if its not active */
		ob = OBACT;
		if (ob && !(ob->flag & SELECT)) ob = NULL;

		for (base = scene->base.first; base; base = base->next) {
			if (TESTBASELIB(v3d, base)) {
				if (ob == NULL)
					ob = base->object;
				calc_tw_center(scene, base->object->obmat[3]);
				protectflag_to_drawflags(base->object->protectflag, &rv3d->twdrawflag);
				totsel++;
			}
		}

		/* selection center */
		if (totsel) {
			mul_v3_fl(scene->twcent, 1.0f / (float)totsel);   // centroid!
		}
	}

	/* global, local or normal orientation? */
	if (ob && totsel) {
		switch (v3d->twmode) {
			case V3D_MANIP_GLOBAL:
			{
				break; /* nothing to do */
			}
			case V3D_MANIP_GIMBAL:
			{
				float mat[3][3];
				if (gimbal_axis(ob, mat)) {
					copy_m4_m3(rv3d->twmat, mat);
					break;
				}
				/* if not gimbal, fall through to normal */
				/* fall-through */
			}
			case V3D_MANIP_NORMAL:
			{
				if (obedit || ob->mode & OB_MODE_POSE) {
					float mat[3][3];
					ED_getTransformOrientationMatrix(C, mat, (v3d->around == V3D_ACTIVE));
					copy_m4_m3(rv3d->twmat, mat);
					break;
				}
				/* no break we define 'normal' as 'local' in Object mode */
				/* fall-through */
			}
			case V3D_MANIP_LOCAL:
			{
				if (ob->mode & OB_MODE_POSE) {
					/* each bone moves on its own local axis, but  to avoid confusion,
					 * use the active pones axis for display [#33575], this works as expected on a single bone
					 * and users who select many bones will understand whats going on and what local means
					 * when they start transforming */
					float mat[3][3];
					ED_getTransformOrientationMatrix(C, mat, (v3d->around == V3D_ACTIVE));
					copy_m4_m3(rv3d->twmat, mat);
					break;
				}
				copy_m4_m4(rv3d->twmat, ob->obmat);
				normalize_m4(rv3d->twmat);
				break;
			}
			case V3D_MANIP_VIEW:
			{
				float mat[3][3];
				copy_m3_m4(mat, rv3d->viewinv);
				normalize_m3(mat);
				copy_m4_m3(rv3d->twmat, mat);
				break;
			}
			default: /* V3D_MANIP_CUSTOM */
			{
				float mat[3][3];
				if (applyTransformOrientation(C, mat, NULL)) {
					copy_m4_m3(rv3d->twmat, mat);
				}
				break;
			}
		}
	}

	return totsel;
}

/* don't draw axis perpendicular to the view */
static void test_manipulator_axis(const bContext *C)
{
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	float view_vec[3], axis_vec[3];
	float idot;
	int i;

	const int twdrawflag_axis[3] = {
	    (MAN_TRANS_X | MAN_SCALE_X),
	    (MAN_TRANS_Y | MAN_SCALE_Y),
	    (MAN_TRANS_Z | MAN_SCALE_Z)};

	ED_view3d_global_to_vector(rv3d, rv3d->twmat[3], view_vec);

	for (i = 0; i < 3; i++) {
		normalize_v3_v3(axis_vec, rv3d->twmat[i]);
		rv3d->tw_idot[i] = idot = 1.0f - fabsf(dot_v3v3(view_vec, axis_vec));
		if (idot < TW_AXIS_DOT_MIN) {
			rv3d->twdrawflag &= ~twdrawflag_axis[i];
		}
	}
}


/* ******************** DRAWING STUFFIES *********** */

static float screen_aligned(const RegionView3D *rv3d, float mat[4][4])
{
	glTranslatef(mat[3][0], mat[3][1], mat[3][2]);

	/* sets view screen aligned */
	glRotatef(-360.0f * saacos(rv3d->viewquat[0]) / (float)M_PI, rv3d->viewquat[1], rv3d->viewquat[2], rv3d->viewquat[3]);

	return len_v3(mat[0]); /* draw scale */
}


/* radring = radius of doughnut rings
 * radhole = radius hole
 * start = starting segment (based on nrings)
 * end   = end segment
 * nsides = amount of points in ring
 * nrigns = amount of rings
 */
static void partial_doughnut(const float radring, const float radhole, const int start,
                             const int end, const int nsides, const int nrings)
{
	float theta, phi, theta1;
	float cos_theta, sin_theta;
	float cos_theta1, sin_theta1;
	float ring_delta, side_delta;
	int i, j, do_caps = true;

	if (start == 0 && end == nrings) do_caps = false;

	ring_delta = 2.0f * (float)M_PI / (float)nrings;
	side_delta = 2.0f * (float)M_PI / (float)nsides;

	theta = (float)M_PI + 0.5f * ring_delta;
	cos_theta = cosf(theta);
	sin_theta = sinf(theta);

	for (i = nrings - 1; i >= 0; i--) {
		theta1 = theta + ring_delta;
		cos_theta1 = cosf(theta1);
		sin_theta1 = sinf(theta1);

		if (do_caps && i == start) {  // cap
			glBegin(GL_POLYGON);
			phi = 0.0;
			for (j = nsides; j >= 0; j--) {
				float cos_phi, sin_phi, dist;

				phi += side_delta;
				cos_phi = cosf(phi);
				sin_phi = sinf(phi);
				dist = radhole + radring * cos_phi;

				glVertex3f(cos_theta1 * dist, -sin_theta1 * dist,  radring * sin_phi);
			}
			glEnd();
		}
		if (i >= start && i <= end) {
			glBegin(GL_QUAD_STRIP);
			phi = 0.0;
			for (j = nsides; j >= 0; j--) {
				float cos_phi, sin_phi, dist;

				phi += side_delta;
				cos_phi = cosf(phi);
				sin_phi = sinf(phi);
				dist = radhole + radring * cos_phi;

				glVertex3f(cos_theta1 * dist, -sin_theta1 * dist, radring * sin_phi);
				glVertex3f(cos_theta * dist, -sin_theta * dist,  radring * sin_phi);
			}
			glEnd();
		}

		if (do_caps && i == end) {    // cap
			glBegin(GL_POLYGON);
			phi = 0.0;
			for (j = nsides; j >= 0; j--) {
				float cos_phi, sin_phi, dist;

				phi -= side_delta;
				cos_phi = cosf(phi);
				sin_phi = sinf(phi);
				dist = radhole + radring * cos_phi;

				glVertex3f(cos_theta * dist, -sin_theta * dist,  radring * sin_phi);
			}
			glEnd();
		}


		theta = theta1;
		cos_theta = cos_theta1;
		sin_theta = sin_theta1;
	}
}

static char axisBlendAngle(const float idot)
{
	if (idot > TW_AXIS_DOT_MAX) {
		return 255;
	}
	else if (idot < TW_AXIS_DOT_MIN) {
		return 0;
	}
	else {
		return (char)(255.0f * (idot - TW_AXIS_DOT_MIN) / (TW_AXIS_DOT_MAX - TW_AXIS_DOT_MIN));
	}
}

/* three colors can be set:
 * gray for ghosting
 * moving: in transform theme color
 * else the red/green/blue
 */
static void manipulator_setcolor(const View3D *v3d, const char axis, const int colcode,
                                 const unsigned char alpha, const bool highlight)
{
	unsigned char col[4] = {0};
	const int offset = (highlight) ? 80 : 0;

	col[3] = alpha;

	if (colcode == MAN_GHOST) {
		col[3] = 70;
	}
	else if (colcode == MAN_MOVECOL) {
		UI_GetThemeColor3ubv(TH_TRANSFORM, col);
	}
	else {
		switch (axis) {
			case 'C':
				UI_GetThemeColor3ubv(TH_TRANSFORM, col);
				if (v3d->twmode == V3D_MANIP_LOCAL) {
					col[0] = col[0] > 200 ? 255 : col[0] + 55;
					col[1] = col[1] > 200 ? 255 : col[1] + 55;
					col[2] = col[2] > 200 ? 255 : col[2] + 55;
				}
				else if (v3d->twmode == V3D_MANIP_NORMAL) {
					col[0] = col[0] < 55 ? 0 : col[0] - 55;
					col[1] = col[1] < 55 ? 0 : col[1] - 55;
					col[2] = col[2] < 55 ? 0 : col[2] - 55;
				}
				break;
			case 'X':
				UI_GetThemeColorShade3ubv(TH_AXIS_X, offset, col);
				break;
			case 'Y':
				UI_GetThemeColorShade3ubv(TH_AXIS_Y, offset, col);
				break;
			case 'Z':
				UI_GetThemeColorShade3ubv(TH_AXIS_Z, offset, col);
				break;
			default:
				BLI_assert(0);
				break;
		}
	}

	glColor4ubv(col);
}

static void manipulator_axis_order(const RegionView3D *rv3d, int r_axis_order[3])
{
	float axis_values[3];
	float vec[3];

	ED_view3d_global_to_vector(rv3d, rv3d->twmat[3], vec);

	axis_values[0] = -dot_v3v3(rv3d->twmat[0], vec);
	axis_values[1] = -dot_v3v3(rv3d->twmat[1], vec);
	axis_values[2] = -dot_v3v3(rv3d->twmat[2], vec);

	axis_sort_v3(axis_values, r_axis_order);
}

/* viewmatrix should have been set OK, also no shademode! */
static void draw_manipulator_axes_single(const View3D *v3d, const RegionView3D *rv3d, const int colcode,
                                         const int flagx, const int flagy, const int flagz, const int axis,
                                         const int selectionbase, const int highlight)
{
	switch (axis) {
		case 0:
			/* axes */
			if (flagx) {
				if (selectionbase != -1) {
					if      (flagx & MAN_SCALE_X) GPU_select_load_id(selectionbase);
					else if (flagx & MAN_TRANS_X) GPU_select_load_id(selectionbase);
				}
				else {
					manipulator_setcolor(v3d, 'X', colcode, axisBlendAngle(rv3d->tw_idot[0]), (highlight & (MAN_TRANS_X | MAN_SCALE_X)) != 0);
				}
				glBegin(GL_LINES);
				glVertex3f(0.2f, 0.0f, 0.0f);
				glVertex3f(1.0f, 0.0f, 0.0f);
				glEnd();
			}
			break;
		case 1:
			if (flagy) {
				if (selectionbase != -1) {
					if      (flagy & MAN_SCALE_Y) GPU_select_load_id(selectionbase);
					else if (flagy & MAN_TRANS_Y) GPU_select_load_id(selectionbase);
				}
				else {
					manipulator_setcolor(v3d, 'Y', colcode, axisBlendAngle(rv3d->tw_idot[1]), (highlight & (MAN_TRANS_Y | MAN_SCALE_Y)) != 0);
				}
				glBegin(GL_LINES);
				glVertex3f(0.0f, 0.2f, 0.0f);
				glVertex3f(0.0f, 1.0f, 0.0f);
				glEnd();
			}
			break;
		case 2:
			if (flagz) {
				if (selectionbase != -1) {
					if      (flagz & MAN_SCALE_Z) GPU_select_load_id(selectionbase);
					else if (flagz & MAN_TRANS_Z) GPU_select_load_id(selectionbase);
				}
				else {
					manipulator_setcolor(v3d, 'Z', colcode, axisBlendAngle(rv3d->tw_idot[2]), (highlight & (MAN_TRANS_Z | MAN_SCALE_Z)) != 0);
				}
				glBegin(GL_LINES);
				glVertex3f(0.0f, 0.0f, 0.2f);
				glVertex3f(0.0f, 0.0f, 1.0f);
				glEnd();
			}
			break;
	}
}
static void draw_manipulator_axes(const View3D *v3d, const RegionView3D *rv3d, const int colcode, const int flagx,
                                  const int flagy, const int flagz, const int axis_order[3],
                                  const int selectionbase, const int highlight)
{
	int i;
	for (i = 0; i < 3; i++) {
		draw_manipulator_axes_single(v3d, rv3d, colcode, flagx, flagy, flagz, axis_order[i], selectionbase, highlight);
	}
}

static void preOrthoFront(const bool ortho, float twmat[4][4], int axis)
{
	if (ortho == false) {
		float omat[4][4];
		copy_m4_m4(omat, twmat);
		orthogonalize_m4(omat, axis);
		glPushMatrix();
		glMultMatrixf(omat);
		glFrontFace(is_negative_m4(omat) ? GL_CW : GL_CCW);
	}
}

static void postOrtho(const bool ortho)
{
	if (ortho == false) {
		glPopMatrix();
	}
}

static void draw_manipulator_rotate(View3D *v3d, RegionView3D *rv3d, const int drawflags, int highlight,
                                    const int combo, const int selectionbase, const bool is_moving)
{
	const float cywid = 0.33f * 0.01f * (float)U.tw_handlesize;
	const float cusize = cywid * 0.65f;
	const int arcs = (G.debug_value != 2);
	const int colcode = (is_moving) ? MAN_MOVECOL : MAN_RGB;
	double plane[4];
	float matt[4][4];
	float size, unitmat[4][4];
	bool ortho;

	/* when called while moving in mixed mode, do not draw when... */
	if ((drawflags & MAN_ROT_C) == 0) return;

	/* Init stuff */
	glDisable(GL_DEPTH_TEST);
	unit_m4(unitmat);

	/* prepare for screen aligned draw */
	size = len_v3(rv3d->twmat[0]);
	glPushMatrix();
	glTranslatef(rv3d->twmat[3][0], rv3d->twmat[3][1], rv3d->twmat[3][2]);

	if (arcs) {
		/* clipplane makes nice handles, calc here because of multmatrix but with translate! */
		copy_v3db_v3fl(plane, rv3d->viewinv[2]);
		plane[3] = -0.02f * size; /* clip just a bit more */
		glClipPlane(GL_CLIP_PLANE0, plane);
	}
	/* sets view screen aligned */
	glRotatef(-360.0f * saacos(rv3d->viewquat[0]) / (float)M_PI, rv3d->viewquat[1], rv3d->viewquat[2], rv3d->viewquat[3]);

	/* Screen aligned help circle */
	if (arcs) {
		if (selectionbase == -1) {
			UI_ThemeColorShade(TH_BACK, -30);
			drawcircball(GL_LINE_LOOP, unitmat[3], size, unitmat);
		}
	}

	/* Screen aligned trackball rot circle */
	if (drawflags & MAN_ROT_T) {
		if (selectionbase != -1) GPU_select_load_id(selectionbase);
		else UI_ThemeColor(TH_TRANSFORM);

		drawcircball(GL_LINE_LOOP, unitmat[3], 0.2f * size, unitmat);
	}

	/* Screen aligned view rot circle */
	if (drawflags & MAN_ROT_V) {
		if (selectionbase != -1) GPU_select_load_id(selectionbase);
		else UI_ThemeColor(TH_TRANSFORM);
		drawcircball(GL_LINE_LOOP, unitmat[3], 1.2f * size, unitmat);

		if (is_moving) {
			float vec[3];
			vec[0] = 0; /* XXX (float)(t->imval[0] - t->center2d[0]); */
			vec[1] = 0; /* XXX (float)(t->imval[1] - t->center2d[1]); */
			vec[2] = 0.0f;
			normalize_v3(vec);
			mul_v3_fl(vec, 1.2f * size);
			glBegin(GL_LINES);
			glVertex3f(0.0f, 0.0f, 0.0f);
			glVertex3fv(vec);
			glEnd();
		}
	}
	glPopMatrix();


	ortho = is_orthogonal_m4(rv3d->twmat);

	/* apply the transform delta */
	if (is_moving) {
		copy_m4_m4(matt, rv3d->twmat); /* to copy the parts outside of [3][3] */
		/* XXX mul_m4_m3m4(matt, t->mat, rv3d->twmat); */
		if (ortho) {
			glMultMatrixf(matt);
			glFrontFace(is_negative_m4(matt) ? GL_CW : GL_CCW);
		}
	}
	else {
		if (ortho) {
			glFrontFace(is_negative_m4(rv3d->twmat) ? GL_CW : GL_CCW);
			glMultMatrixf(rv3d->twmat);
		}
	}

	/* axes */
	if (arcs == 0) {
		if (selectionbase == -1) {
			if ((combo & V3D_MANIP_SCALE) == 0) {
				/* axis */
				if ((drawflags & MAN_ROT_X) || (is_moving && (drawflags & MAN_ROT_Z))) {
					preOrthoFront(ortho, rv3d->twmat, 2);
					manipulator_setcolor(v3d, 'X', colcode, 255, (highlight & MAN_ROT_X) != 0);
					glBegin(GL_LINES);
					glVertex3f(0.2f, 0.0f, 0.0f);
					glVertex3f(1.0f, 0.0f, 0.0f);
					glEnd();
					postOrtho(ortho);
				}
				if ((drawflags & MAN_ROT_Y) || (is_moving && (drawflags & MAN_ROT_X))) {
					preOrthoFront(ortho, rv3d->twmat, 0);
					manipulator_setcolor(v3d, 'Y', colcode, 255, (highlight & MAN_ROT_Y) != 0);
					glBegin(GL_LINES);
					glVertex3f(0.0f, 0.2f, 0.0f);
					glVertex3f(0.0f, 1.0f, 0.0f);
					glEnd();
					postOrtho(ortho);
				}
				if ((drawflags & MAN_ROT_Z) || (is_moving && (drawflags & MAN_ROT_Y))) {
					preOrthoFront(ortho, rv3d->twmat, 1);
					manipulator_setcolor(v3d, 'Z', colcode, 255, (highlight & MAN_ROT_Y) != 0);
					glBegin(GL_LINES);
					glVertex3f(0.0f, 0.0f, 0.2f);
					glVertex3f(0.0f, 0.0f, 1.0f);
					glEnd();
					postOrtho(ortho);
				}
			}
		}
	}

	if (arcs == 0 && is_moving) {

		/* Z circle */
		if (drawflags & MAN_ROT_Z) {
			preOrthoFront(ortho, matt, 2);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Z', colcode, 255, (highlight & MAN_ROT_Z) != 0);
			drawcircball(GL_LINE_LOOP, unitmat[3], 1.0, unitmat);
			postOrtho(ortho);
		}
		/* X circle */
		if (drawflags & MAN_ROT_X) {
			preOrthoFront(ortho, matt, 0);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'X', colcode, 255, (highlight & MAN_ROT_X) != 0);
			glRotatef(90.0, 0.0, 1.0, 0.0);
			drawcircball(GL_LINE_LOOP, unitmat[3], 1.0, unitmat);
			glRotatef(-90.0, 0.0, 1.0, 0.0);
			postOrtho(ortho);
		}
		/* Y circle */
		if (drawflags & MAN_ROT_Y) {
			preOrthoFront(ortho, matt, 1);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Y', colcode, 255, (highlight & MAN_ROT_Y) != 0);
			glRotatef(-90.0, 1.0, 0.0, 0.0);
			drawcircball(GL_LINE_LOOP, unitmat[3], 1.0, unitmat);
			glRotatef(90.0, 1.0, 0.0, 0.0);
			postOrtho(ortho);
		}

		if (arcs) glDisable(GL_CLIP_PLANE0);
	}
	/* donut arcs */
	if (arcs) {
		glEnable(GL_CLIP_PLANE0);

		/* Z circle */
		if (drawflags & MAN_ROT_Z) {
			preOrthoFront(ortho, rv3d->twmat, 2);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Z', colcode, 255, (highlight & MAN_ROT_Z) != 0);
			partial_doughnut(cusize / 4.0f, 1.0f, 0, 48, 8, 48);
			postOrtho(ortho);
		}
		/* X circle */
		if (drawflags & MAN_ROT_X) {
			preOrthoFront(ortho, rv3d->twmat, 0);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'X', colcode, 255, (highlight & MAN_ROT_X) != 0);
			glRotatef(90.0, 0.0, 1.0, 0.0);
			partial_doughnut(cusize / 4.0f, 1.0f, 0, 48, 8, 48);
			glRotatef(-90.0, 0.0, 1.0, 0.0);
			postOrtho(ortho);
		}
		/* Y circle */
		if (drawflags & MAN_ROT_Y) {
			preOrthoFront(ortho, rv3d->twmat, 1);
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Y', colcode, 255, (highlight & MAN_ROT_Y) != 0);
			glRotatef(-90.0, 1.0, 0.0, 0.0);
			partial_doughnut(cusize / 4.0f, 1.0f, 0, 48, 8, 48);
			glRotatef(90.0, 1.0, 0.0, 0.0);
			postOrtho(ortho);
		}

		glDisable(GL_CLIP_PLANE0);
	}

	if (arcs == 0) {

		/* Z handle on X axis */
		if (drawflags & MAN_ROT_Z) {
			preOrthoFront(ortho, rv3d->twmat, 2);
			glPushMatrix();
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Z', colcode, 255, (highlight & MAN_ROT_Z) != 0);

			partial_doughnut(0.7f * cusize, 1.0f, 31, 33, 8, 64);

			glPopMatrix();
			postOrtho(ortho);
		}

		/* Y handle on X axis */
		if (drawflags & MAN_ROT_Y) {
			preOrthoFront(ortho, rv3d->twmat, 1);
			glPushMatrix();
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'Y', colcode, 255, (highlight & MAN_ROT_Y) != 0);

			glRotatef(90.0, 1.0, 0.0, 0.0);
			glRotatef(90.0, 0.0, 0.0, 1.0);
			partial_doughnut(0.7f * cusize, 1.0f, 31, 33, 8, 64);

			glPopMatrix();
			postOrtho(ortho);
		}

		/* X handle on Z axis */
		if (drawflags & MAN_ROT_X) {
			preOrthoFront(ortho, rv3d->twmat, 0);
			glPushMatrix();
			if (selectionbase != -1) GPU_select_load_id(selectionbase);
			else manipulator_setcolor(v3d, 'X', colcode, 255, (highlight & MAN_ROT_X) != 0);

			glRotatef(-90.0, 0.0, 1.0, 0.0);
			glRotatef(90.0, 0.0, 0.0, 1.0);
			partial_doughnut(0.7f * cusize, 1.0f, 31, 33, 8, 64);

			glPopMatrix();
			postOrtho(ortho);
		}

	}

	/* restore */
	glLoadMatrixf(rv3d->viewmat);
	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
}

static void drawsolidcube(const float size)
{
	const float cube[8][3] = {
		{-1.0, -1.0, -1.0},
		{-1.0, -1.0,  1.0},
		{-1.0,  1.0,  1.0},
		{-1.0,  1.0, -1.0},
		{ 1.0, -1.0, -1.0},
		{ 1.0, -1.0,  1.0},
		{ 1.0,  1.0,  1.0},
		{ 1.0,  1.0, -1.0},
	};
	float n[3] = {0.0f};

	glPushMatrix();
	glScalef(size, size, size);

	glBegin(GL_QUADS);
	n[0] = -1.0;
	glNormal3fv(n);
	glVertex3fv(cube[0]); glVertex3fv(cube[1]); glVertex3fv(cube[2]); glVertex3fv(cube[3]);
	n[0] = 0;
	glEnd();

	glBegin(GL_QUADS);
	n[1] = -1.0;
	glNormal3fv(n);
	glVertex3fv(cube[0]); glVertex3fv(cube[4]); glVertex3fv(cube[5]); glVertex3fv(cube[1]);
	n[1] = 0;
	glEnd();

	glBegin(GL_QUADS);
	n[0] = 1.0;
	glNormal3fv(n);
	glVertex3fv(cube[4]); glVertex3fv(cube[7]); glVertex3fv(cube[6]); glVertex3fv(cube[5]);
	n[0] = 0;
	glEnd();

	glBegin(GL_QUADS);
	n[1] = 1.0;
	glNormal3fv(n);
	glVertex3fv(cube[7]); glVertex3fv(cube[3]); glVertex3fv(cube[2]); glVertex3fv(cube[6]);
	n[1] = 0;
	glEnd();

	glBegin(GL_QUADS);
	n[2] = 1.0;
	glNormal3fv(n);
	glVertex3fv(cube[1]); glVertex3fv(cube[5]); glVertex3fv(cube[6]); glVertex3fv(cube[2]);
	n[2] = 0;
	glEnd();

	glBegin(GL_QUADS);
	n[2] = -1.0;
	glNormal3fv(n);
	glVertex3fv(cube[7]); glVertex3fv(cube[4]); glVertex3fv(cube[0]); glVertex3fv(cube[3]);
	glEnd();

	glPopMatrix();
}


static void draw_manipulator_scale(const View3D *v3d, RegionView3D *rv3d, int drawflags, const int highlight,
                                   const int combo, const int colcode, const int selectionbase, const bool is_moving)
{
	const float cywid = 0.25f * 0.01f * (float)U.tw_handlesize;
	const float cusize = cywid * 0.75f;
	int axis_order[3] = {2, 0, 1};
	float dz;
	int i;

	/* when called while moving in mixed mode, do not draw when... */
	if ((drawflags & MAN_SCALE_C) == 0) return;

	manipulator_axis_order(rv3d, axis_order);

	glDisable(GL_DEPTH_TEST);

	/* not in combo mode */
	if ((combo & (V3D_MANIP_TRANSLATE | V3D_MANIP_ROTATE)) == 0) {
		float size, unitmat[4][4];
		int shift = 0; // XXX

		/* center circle, do not add to selection when shift is pressed (planar constraint)  */
		if (selectionbase != -1 && shift == 0) GPU_select_load_id(selectionbase);
		else manipulator_setcolor(v3d, 'C', colcode, 255, (highlight & MAN_SCALE_C) != 0);

		glPushMatrix();
		size = screen_aligned(rv3d, rv3d->twmat);
		unit_m4(unitmat);
		drawcircball(GL_LINE_LOOP, unitmat[3], 0.2f * size, unitmat);
		glPopMatrix();

		dz = 1.0;
	}
	else {
		dz = 1.0f - 4.0f * cusize;
	}

	if (is_moving) {
		float matt[4][4];

		copy_m4_m4(matt, rv3d->twmat); // to copy the parts outside of [3][3]
		// XXX mul_m4_m3m4(matt, t->mat, rv3d->twmat);
		glMultMatrixf(matt);
		glFrontFace(is_negative_m4(matt) ? GL_CW : GL_CCW);
	}
	else {
		glMultMatrixf(rv3d->twmat);
		glFrontFace(is_negative_m4(rv3d->twmat) ? GL_CW : GL_CCW);
	}

	/* axis */

	/* in combo mode, this is always drawn as first type */
	draw_manipulator_axes(v3d, rv3d, colcode,
	                      drawflags & MAN_SCALE_X, drawflags & MAN_SCALE_Y, drawflags & MAN_SCALE_Z,
	                      axis_order, selectionbase, highlight);

	for (i = 0; i < 3; i++) {
		switch (axis_order[i]) {
			case 0: /* X cube */
				if (drawflags & MAN_SCALE_X) {
					glTranslatef(dz, 0.0, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'X', colcode, axisBlendAngle(rv3d->tw_idot[0]), (highlight & MAN_SCALE_X) != 0);
					drawsolidcube(cusize);
					glTranslatef(-dz, 0.0, 0.0);
				}
				break;
			case 1: /* Y cube */
				if (drawflags & MAN_SCALE_Y) {
					glTranslatef(0.0, dz, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'Y', colcode, axisBlendAngle(rv3d->tw_idot[1]), (highlight & MAN_SCALE_Y) != 0);
					drawsolidcube(cusize);
					glTranslatef(0.0, -dz, 0.0);
				}
				break;
			case 2: /* Z cube */
				if (drawflags & MAN_SCALE_Z) {
					glTranslatef(0.0, 0.0, dz);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'Z', colcode, axisBlendAngle(rv3d->tw_idot[2]), (highlight & MAN_SCALE_Z) != 0);
					drawsolidcube(cusize);
					glTranslatef(0.0, 0.0, -dz);
				}
				break;
		}
	}

	/* if shiftkey, center point as last, for selectbuffer order */
	if (selectionbase != -1) {
		int shift = 0; // XXX

		if (shift) {
			glTranslatef(0.0, -dz, 0.0);
			GPU_select_load_id(selectionbase);
			glBegin(GL_POINTS);
			glVertex3f(0.0, 0.0, 0.0);
			glEnd();
		}
	}

	/* restore */
	glLoadMatrixf(rv3d->viewmat);

	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);
	glFrontFace(GL_CCW);
}

#if 0
static void draw_cone(GLUquadricObj *qobj, float len, float width)
{
	glTranslatef(0.0, 0.0, -0.5f * len);
	gluCylinder(qobj, width, 0.0, len, 8, 1);
	gluQuadricOrientation(qobj, GLU_INSIDE);
	gluDisk(qobj, 0.0, width, 8, 1);
	gluQuadricOrientation(qobj, GLU_OUTSIDE);
	glTranslatef(0.0, 0.0, 0.5f * len);
}
#endif

static void draw_cylinder(GLUquadricObj *qobj, const float len, float width)
{
	width *= 0.8f;   // just for beauty

	glTranslatef(0.0, 0.0, -0.5f * len);
	gluCylinder(qobj, width, width, len, 8, 1);
	gluQuadricOrientation(qobj, GLU_INSIDE);
	gluDisk(qobj, 0.0, width, 8, 1);
	gluQuadricOrientation(qobj, GLU_OUTSIDE);
	glTranslatef(0.0, 0.0, len);
	gluDisk(qobj, 0.0, width, 8, 1);
	glTranslatef(0.0, 0.0, -0.5f * len);
}


static void draw_manipulator_translate(const View3D *v3d, RegionView3D *rv3d, const int drawflags,
                                       const int highlightflags, const int UNUSED(combo), const int colcode,
                                       const int selectionbase, const bool UNUSED(is_moving))
{
	GLUquadricObj *qobj;
	float cylen = 0.01f * (float)U.tw_handlesize;
	float cywid = 0.25f * cylen, dz, size;
	float unitmat[4][4];
	int shift = 0; // XXX
	int axis_order[3] = {0, 1, 2};
	int i;

	/* when called while moving in mixed mode, do not draw when... */
	if ((drawflags & MAN_TRANS_C) == 0) return;

	manipulator_axis_order(rv3d, axis_order);

	// XXX if (moving) glTranslatef(t->vec[0], t->vec[1], t->vec[2]);
	glDisable(GL_DEPTH_TEST);

	/* center circle, do not add to selection when shift is pressed (planar constraint) */
	if (selectionbase != -1 && shift == 0) GPU_select_load_id(selectionbase);
	else manipulator_setcolor(v3d, 'C', colcode, 255, (highlightflags & MAN_TRANS_C) != 0);

	glPushMatrix();
	size = screen_aligned(rv3d, rv3d->twmat);
	unit_m4(unitmat);
	drawcircball(GL_LINE_LOOP, unitmat[3], 0.2f * size, unitmat);
	glPopMatrix();

	glPushMatrix();
	/* and now apply matrix, we move to local matrix drawing */
	glMultMatrixf(rv3d->twmat);

#if 0
	// translate drawn as last, only axis when no combo with scale, or for ghosting
	if ((combo & V3D_MANIP_SCALE) == 0 || colcode == MAN_GHOST) {
		draw_manipulator_axes(v3d, rv3d, colcode,
		                      drawflags & MAN_TRANS_X, 0, drawflags & MAN_TRANS_Z,
		                      axis_order, selectionbase, highlightflags);
	}

	/* offset in combo mode, for rotate a bit more */
	if (combo & (V3D_MANIP_ROTATE)) dz = 1.0f + 2.0f * cylen;
	else if (combo & (V3D_MANIP_SCALE)) dz = 1.0f + 0.5f * cylen;
	else dz = 1.0f;

	qobj = gluNewQuadric();
	gluQuadricDrawStyle(qobj, GLU_FILL);

	for (i = 0; i < 3; i++) {
		switch (axis_order[i]) {
			case 0: /* Z Cone */
				if (drawflags & MAN_TRANS_Z) {
					glPushMatrix();
					glTranslatef(0.0, 0.0, dz);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'Z', colcode, axisBlendAngle(rv3d->tw_idot[2]), (highlightflags & MAN_TRANS_Z) != 0);
					draw_cone(qobj, cylen, cywid);
					glPopMatrix();
				}
				break;
			case 1: /* X Cone */
				if (drawflags & MAN_TRANS_X) {
					glPushMatrix();
					glTranslatef(dz, 0.0, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'X', colcode, axisBlendAngle(rv3d->tw_idot[0]), (highlightflags & MAN_TRANS_X) != 0);
					glRotatef(90.0, 0.0, 1.0, 0.0);
					draw_cone(qobj, cylen, cywid);
					glPopMatrix();
				}
				break;
			case 2: /* Y Cone */
				if (drawflags & MAN_TRANS_Y) {
					glPushMatrix();
					glTranslatef(0.0, dz, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					else manipulator_setcolor(v3d, 'Y', colcode, axisBlendAngle(rv3d->tw_idot[1]), (highlightflags & MAN_TRANS_Y) != 0);
					glRotatef(-90.0, 1.0, 0.0, 0.0);
					draw_cone(qobj, cylen, cywid);
					glPopMatrix();
				}
				break;
		}
	}

	gluDeleteQuadric(qobj);
#else
	(void)qobj, (void)cywid, (void)i, (void)dz;
#endif

	glPopMatrix();

	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);

}

static void draw_manipulator_rotate_cyl(const View3D *v3d, RegionView3D *rv3d, const int drawflags,
                                        const int highlight, const int combo, const int colcode,
                                        const int selectionbase, const bool is_moving)
{
	GLUquadricObj *qobj;
	float size;
	float cylen = 0.01f * (float)U.tw_handlesize;
	float cywid = 0.25f * cylen;
	int axis_order[3] = {2, 0, 1};
	int i;

	/* when called while moving in mixed mode, do not draw when... */
	if ((drawflags & MAN_ROT_C) == 0) return;

	manipulator_axis_order(rv3d, axis_order);

	/* prepare for screen aligned draw */
	glPushMatrix();
	size = screen_aligned(rv3d, rv3d->twmat);

	glDisable(GL_DEPTH_TEST);

	qobj = gluNewQuadric();

	/* Screen aligned view rot circle */
	if (drawflags & MAN_ROT_V) {
		float unitmat[4][4];

		unit_m4(unitmat);

		if (selectionbase != -1) GPU_select_load_id(selectionbase);
		UI_ThemeColor(TH_TRANSFORM);
		drawcircball(GL_LINE_LOOP, unitmat[3], 1.2f * size, unitmat);

		if (is_moving) {
			float vec[3];
			vec[0] = 0; // XXX (float)(t->imval[0] - t->center2d[0]);
			vec[1] = 0; // XXX (float)(t->imval[1] - t->center2d[1]);
			vec[2] = 0.0f;
			normalize_v3(vec);
			mul_v3_fl(vec, 1.2f * size);
			glBegin(GL_LINES);
			glVertex3f(0.0, 0.0, 0.0);
			glVertex3fv(vec);
			glEnd();
		}
	}
	glPopMatrix();

	/* apply the transform delta */
	if (is_moving) {
		float matt[4][4];
		copy_m4_m4(matt, rv3d->twmat); // to copy the parts outside of [3][3]
		// XXX      if (t->flag & T_USES_MANIPULATOR) {
		// XXX          mul_m4_m3m4(matt, t->mat, rv3d->twmat);
		// XXX }
		glMultMatrixf(matt);
	}
	else {
		glMultMatrixf(rv3d->twmat);
	}

	glFrontFace(is_negative_m4(rv3d->twmat) ? GL_CW : GL_CCW);

	/* axis */
	if (selectionbase != -1) {

		// only draw axis when combo didn't draw scale axes
		if ((combo & V3D_MANIP_SCALE) == 0) {
			draw_manipulator_axes(v3d, rv3d, colcode,
			                      drawflags & MAN_ROT_X, drawflags & MAN_ROT_Y, drawflags & MAN_ROT_Z,
			                      axis_order, selectionbase, highlight);
		}

		/* only has to be set when not in picking */
		gluQuadricDrawStyle(qobj, GLU_FILL);
	}

	for (i = 0; i < 3; i++) {
		switch (axis_order[i]) {
			case 0: /* X cylinder */
				if (drawflags & MAN_ROT_X) {
					glTranslatef(1.0, 0.0, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					glRotatef(90.0, 0.0, 1.0, 0.0);
					manipulator_setcolor(v3d, 'X', colcode, 255, (highlight & MAN_ROT_X) != 0);
					draw_cylinder(qobj, cylen, cywid);
					glRotatef(-90.0, 0.0, 1.0, 0.0);
					glTranslatef(-1.0, 0.0, 0.0);
				}
				break;
			case 1: /* Y cylinder */
				if (drawflags & MAN_ROT_Y) {
					glTranslatef(0.0, 1.0, 0.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					glRotatef(-90.0, 1.0, 0.0, 0.0);
					manipulator_setcolor(v3d, 'Y', colcode, 255, (highlight & MAN_ROT_Y) != 0);
					draw_cylinder(qobj, cylen, cywid);
					glRotatef(90.0, 1.0, 0.0, 0.0);
					glTranslatef(0.0, -1.0, 0.0);
				}
				break;
			case 2: /* Z cylinder */
				if (drawflags & MAN_ROT_Z) {
					glTranslatef(0.0, 0.0, 1.0);
					if (selectionbase != -1) GPU_select_load_id(selectionbase);
					manipulator_setcolor(v3d, 'Z', colcode, 255, (highlight & MAN_ROT_Z) != 0);
					draw_cylinder(qobj, cylen, cywid);
					glTranslatef(0.0, 0.0, -1.0);
				}
				break;
		}
	}

	/* restore */

	gluDeleteQuadric(qobj);
	glLoadMatrixf(rv3d->viewmat);

	if (v3d->zbuf) glEnable(GL_DEPTH_TEST);

}


/* ********************************************* */

/* main call, does calc centers & orientation too */
static int drawflags = 0xFFFF;       /* only for the calls below, belongs in scene...? */

static int manipulator_flags_from_active(const int active)
{
	int val;

	if (active != -1) {
		if (active == MAN_SEL_TRANS_C) {
			val = MAN_TRANS_C;
		}
		else if (active == MAN_SEL_SCALE_C) {
			val = MAN_SCALE_C;
		}
		else {
			val = 1 << active;
		}
	}
	else
		val = 0;

	return val;
}

static void WIDGET_manipulator_draw(const bContext *C, wmWidget *UNUSED(widget))
{
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
	View3D *v3d = sa->spacedata.first;
	RegionView3D *rv3d = ar->regiondata;

	if (G.debug_value == 0)
		return;

	if (v3d->twflag & V3D_DRAW_MANIPULATOR) {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_BLEND);
		if (v3d->twtype & V3D_MANIP_ROTATE) {

			if (G.debug_value == 3) {
				if (G.moving & (G_TRANSFORM_OBJ | G_TRANSFORM_EDIT))
					draw_manipulator_rotate_cyl(v3d, rv3d, drawflags, 0, v3d->twtype, MAN_MOVECOL, true, -1);
				else
					draw_manipulator_rotate_cyl(v3d, rv3d, drawflags, 0, v3d->twtype, MAN_RGB, false, -1);
			}
			else {
				draw_manipulator_rotate(v3d, rv3d, drawflags, 0, v3d->twtype, false, -1);
			}
		}
		if (v3d->twtype & V3D_MANIP_SCALE) {
			draw_manipulator_scale(v3d, rv3d, drawflags, 0, v3d->twtype, MAN_RGB, false, -1);
		}
		if (v3d->twtype & V3D_MANIP_TRANSLATE) {
			draw_manipulator_translate(v3d, rv3d, drawflags, 0, v3d->twtype, MAN_RGB, false, -1);
		}

		glDisable(GL_BLEND);
	}
}

int WIDGETGROUP_manipulator_poll(const struct bContext *C, struct wmWidgetGroupType *UNUSED(wgrouptype))
{
	/* it's a given we only use this in 3D view */
	ScrArea *sa = CTX_wm_area(C);
	View3D *v3d = sa->spacedata.first;

	return ((v3d->twflag & V3D_USE_MANIPULATOR) != 0);
}

static void WIDGET_manipulator_render_3d_intersect(const struct bContext *C, struct wmWidget *UNUSED(widget), int selectionbase)
{
	ScrArea *sa = CTX_wm_area(C);
	View3D *v3d = sa->spacedata.first;
	ARegion *ar = CTX_wm_region(C);
	RegionView3D *rv3d = ar->regiondata;

	/* do the drawing */
	if (v3d->twtype & V3D_MANIP_ROTATE) {
		//if (G.debug_value == 3) draw_manipulator_rotate_cyl(v3d, rv3d, MAN_ROT_C & rv3d->twdrawflag, 0, v3d->twtype, MAN_RGB, false, selectionbase);
		//else draw_manipulator_rotate(v3d, rv3d, MAN_ROT_C & rv3d->twdrawflag, 0, v3d->twtype, false, selectionbase);
	}
	if (v3d->twtype & V3D_MANIP_SCALE)
		draw_manipulator_scale(v3d, rv3d, MAN_SCALE_C & rv3d->twdrawflag, 0, v3d->twtype, MAN_RGB, false, selectionbase);
	//if (v3d->twtype & V3D_MANIP_TRANSLATE)
	//	draw_manipulator_translate(v3d, rv3d, MAN_TRANS_C & rv3d->twdrawflag, 0, v3d->twtype, MAN_RGB, false, selectionbase);
}

/* return 0; nothing happened */
static int WIDGET_manipulator_handler(bContext *C, const struct wmEvent *event, wmWidget *UNUSED(widget))
{
	const ScrArea *sa = CTX_wm_area(C);
	const View3D *v3d = sa->spacedata.first;
	int constraint_axis[3] = {0, 0, 0};
	int val;
	int shift = event->shift;

	struct IDProperty *properties = NULL;	/* operator properties, assigned to ptr->data and can be written to a file */
	struct PointerRNA *ptr = NULL;			/* rna pointer to access properties */

	val = manipulator_flags_from_active(0);

	if (!((v3d->twflag & V3D_USE_MANIPULATOR) && (v3d->twflag & V3D_DRAW_MANIPULATOR)) ||
	    !(event->keymodifier == 0 || event->keymodifier == KM_SHIFT) ||
		!((event->val == KM_PRESS) && (event->type == LEFTMOUSE)))
	{
		return OPERATOR_PASS_THROUGH;
	}

	if (val) {
		if (val & MAN_TRANS_C) {
			switch (val) {
				case MAN_TRANS_C:
					break;
				case MAN_TRANS_X:
					if (shift) {
						constraint_axis[1] = 1;
						constraint_axis[2] = 1;
					}
					else
						constraint_axis[0] = 1;
					break;
				case MAN_TRANS_Y:
					if (shift) {
						constraint_axis[0] = 1;
						constraint_axis[2] = 1;
					}
					else
						constraint_axis[1] = 1;
					break;
				case MAN_TRANS_Z:
					if (shift) {
						constraint_axis[0] = 1;
						constraint_axis[1] = 1;
					}
					else
						constraint_axis[2] = 1;
					break;
			}
			WM_operator_properties_alloc(&ptr, &properties, "TRANSFORM_OT_translate");
			/* Force orientation */
			RNA_boolean_set(ptr, "release_confirm", true);
			RNA_enum_set(ptr, "constraint_orientation", v3d->twmode);
			RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
			WM_operator_name_call(C, "TRANSFORM_OT_translate", WM_OP_INVOKE_DEFAULT, ptr);
		}
		else if (val & MAN_SCALE_C) {
			switch (val) {
				case MAN_SCALE_X:
					if (shift) {
						constraint_axis[1] = 1;
						constraint_axis[2] = 1;
					}
					else
						constraint_axis[0] = 1;
					break;
				case MAN_SCALE_Y:
					if (shift) {
						constraint_axis[0] = 1;
						constraint_axis[2] = 1;
					}
					else
						constraint_axis[1] = 1;
					break;
				case MAN_SCALE_Z:
					if (shift) {
						constraint_axis[0] = 1;
						constraint_axis[1] = 1;
					}
					else
						constraint_axis[2] = 1;
					break;
			}
			WM_operator_properties_alloc(&ptr, &properties, "TRANSFORM_OT_resize");
			/* Force orientation */
			RNA_boolean_set(ptr, "release_confirm", true);
			RNA_enum_set(ptr, "constraint_orientation", v3d->twmode);
			RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
			WM_operator_name_call(C, "TRANSFORM_OT_resize", WM_OP_INVOKE_DEFAULT, ptr);
		}
		else if (val == MAN_ROT_T) { /* trackball need special case, init is different */
			/* Do not pass op->ptr!!! trackball has no "constraint" properties!
			 * See [#34621], it's a miracle it did not cause more problems!!! */
			/* However, we need to copy the "release_confirm" property, but only if defined, see T41112. */
			PointerRNA props_ptr;
			wmOperatorType *ot = WM_operatortype_find("TRANSFORM_OT_trackball", true);
			WM_operator_properties_create_ptr(&props_ptr, ot);
			RNA_boolean_set(&props_ptr, "release_confirm", true);
			WM_operator_name_call_ptr(C, ot, WM_OP_INVOKE_DEFAULT, &props_ptr);
			WM_operator_properties_free(&props_ptr);
		}
		else if (val & MAN_ROT_C) {
			switch (val) {
				case MAN_ROT_X:
					constraint_axis[0] = 1;
					break;
				case MAN_ROT_Y:
					constraint_axis[1] = 1;
					break;
				case MAN_ROT_Z:
					constraint_axis[2] = 1;
					break;
			}
			WM_operator_properties_alloc(&ptr, &properties, "TRANSFORM_OT_rotate");
			/* Force orientation */
			RNA_boolean_set(ptr, "release_confirm", true);
			RNA_enum_set(ptr, "constraint_orientation", v3d->twmode);
			RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
			WM_operator_name_call(C, "TRANSFORM_OT_rotate", WM_OP_INVOKE_DEFAULT, ptr);
		}
	}

	if (ptr) {
		WM_operator_properties_free(ptr);
		MEM_freeN(ptr);
	}

	return (val) ? OPERATOR_FINISHED : OPERATOR_PASS_THROUGH;
}

#if 0
/* return 0; nothing happened */
int WIDGET_manipulator_handler_trans(bContext *C, const struct wmEvent *event, wmWidget *widget, struct PointerRNA *ptr)
{
	ScrArea *sa = CTX_wm_area(C);
	View3D *v3d = sa->spacedata.first;
	int constraint_axis[3] = {0, 0, 0};
	int shift = event->shift;
	int direction = GET_INT_FROM_POINTER(WM_widget_customdata(widget));

	if (!((v3d->twflag & V3D_USE_MANIPULATOR) && (v3d->twflag & V3D_DRAW_MANIPULATOR)) ||
	    !(event->keymodifier == 0 || event->keymodifier == KM_SHIFT))
	{
		;//return OPERATOR_PASS_THROUGH;
	}

	if (shift) {
		int d = 0;
		for (; d < 3; d++) {
			if (d != direction)
				constraint_axis[d] = 1;
		}
	}
	else
		constraint_axis[direction] = 1;

	/* Force orientation */
	RNA_boolean_set(ptr, "release_confirm", true);
	RNA_enum_set(ptr, "constraint_orientation", v3d->twmode);
	RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
	RNA_boolean_set(ptr, "use_widget_input", true);

	return OPERATOR_FINISHED;
}

/* return 0; nothing happened */
int WIDGET_manipulator_handler_rot(bContext *C, const struct wmEvent *UNUSED(event), wmWidget *widget, struct PointerRNA *ptr)
{
	ScrArea *sa = CTX_wm_area(C);
	View3D *v3d = sa->spacedata.first;
	int constraint_axis[3] = {0, 0, 0};
	int direction = GET_INT_FROM_POINTER(WM_widget_customdata(widget));

	if (!(v3d->twflag & V3D_USE_MANIPULATOR) && (v3d->twflag & V3D_DRAW_MANIPULATOR))
	{
		;//return OPERATOR_PASS_THROUGH;
	}

	constraint_axis[direction] = 1;

	/* Force orientation */
	RNA_boolean_set(ptr, "release_confirm", true);
	RNA_enum_set(ptr, "constraint_orientation", v3d->twmode);
	RNA_boolean_set_array(ptr, "constraint_axis", constraint_axis);
	RNA_boolean_set(ptr, "use_widget_input", true);

	return OPERATOR_FINISHED;
}

static void WIDGETGROUP_manipulator_free(struct wmWidgetGroup *wgroup)
{
	ManipulatorGroup *manipulator = WM_widgetgroup_customdata(wgroup);

	MEM_freeN(manipulator);
}
#endif

static ManipulatorGroup *manipulator_widgetgroup_create(struct wmWidgetGroup *wgroup)
{
	ManipulatorGroup *manipulator = MEM_callocN(sizeof(ManipulatorGroup), "manipulator_data");
	wmWidget *axis;

	float color_green[4] = {0.27f, 1.0f, 0.27f, 1.0f};
	float color_red[4] = {1.0f, 0.27f, 0.27f, 1.0f};
	float color_blue[4] = {0.27f, 0.27f, 1.0f, 1.0f};
	short i;

	manipulator->translate_x = WIDGET_arrow_new(wgroup, "translate_x", WIDGET_ARROW_STYLE_NORMAL);
	manipulator->translate_y = WIDGET_arrow_new(wgroup, "translate_y", WIDGET_ARROW_STYLE_NORMAL);
	manipulator->translate_z = WIDGET_arrow_new(wgroup, "translate_z", WIDGET_ARROW_STYLE_NORMAL);
	manipulator->rotate_x = WIDGET_dial_new(wgroup, "rotate_x", WIDGET_DIAL_STYLE_RING_CLIPPED);
	manipulator->rotate_y = WIDGET_dial_new(wgroup, "rotate_y", WIDGET_DIAL_STYLE_RING_CLIPPED);
	manipulator->rotate_z = WIDGET_dial_new(wgroup, "rotate_z", WIDGET_DIAL_STYLE_RING_CLIPPED);

	MAN_ITER_AXES_BEGIN(MAN_AXES_ALL)
	{
		switch (i) {
			case MAN_AXIS_TRANS_X:
				WIDGET_arrow_set_color(axis, color_red);
				break;
			case MAN_AXIS_TRANS_Y:
				WIDGET_arrow_set_color(axis, color_green);
				break;
			case MAN_AXIS_TRANS_Z:
				WIDGET_arrow_set_color(axis, color_blue);
				break;
			case MAN_AXIS_ROT_X:
				WIDGET_dial_set_color(axis, color_red);
				break;
			case MAN_AXIS_ROT_Y:
				WIDGET_dial_set_color(axis, color_green);
				break;
			case MAN_AXIS_ROT_Z:
				WIDGET_dial_set_color(axis, color_blue);
				break;
		}
	}
	MAN_ITER_AXES_END;

	return manipulator;
}

void WIDGETGROUP_manipulator_draw(const struct bContext *C, struct wmWidgetGroup *wgroup)
{
	ScrArea *sa = CTX_wm_area(C);
	ARegion *ar = CTX_wm_region(C);
	Scene *scene = CTX_data_scene(C);
	View3D *v3d = sa->spacedata.first;
	RegionView3D *rv3d = ar->regiondata;
	ManipulatorGroup *manipulator;
	wmWidget *axis;

	int totsel;
	short i;

	manipulator = manipulator_widgetgroup_create(wgroup);

	v3d->twflag &= ~V3D_DRAW_MANIPULATOR;

	totsel = calc_manipulator_stats(C);
	if (totsel == 0) {
		MAN_ITER_AXES_BEGIN(MAN_AXES_ALL)
		{
			WM_widget_flag_enable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;
		return;
	}

	v3d->twflag |= V3D_DRAW_MANIPULATOR;

	/* now we can define center */
	switch (v3d->around) {
		case V3D_CENTER:
		case V3D_ACTIVE:
		{
			Object *ob;
			if (((v3d->around == V3D_ACTIVE) && (scene->obedit == NULL)) &&
					((ob = OBACT) && !(ob->mode & OB_MODE_POSE)))
			{
				copy_v3_v3(rv3d->twmat[3], ob->obmat[3]);
			}
			else {
				mid_v3_v3v3(rv3d->twmat[3], scene->twmin, scene->twmax);
			}
			break;
		}
		case V3D_LOCAL:
		case V3D_CENTROID:
			copy_v3_v3(rv3d->twmat[3], scene->twcent);
			break;
		case V3D_CURSOR:
			copy_v3_v3(rv3d->twmat[3], ED_view3d_cursor3d_get(scene, v3d));
			break;
	}

	mul_mat3_m4_fl(rv3d->twmat, ED_view3d_pixel_size(rv3d, rv3d->twmat[3]) * U.tw_size);

	/* when looking through a selected camera, the manipulator can be at the
	 * exact same position as the view, skip so we don't break selection */
	if (fabsf(mat4_to_scale(rv3d->twmat)) < 1e-7f) {
		MAN_ITER_AXES_BEGIN(MAN_AXES_ALL)
		{
			WM_widget_flag_enable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;

		return;
	}

	test_manipulator_axis(C);
	drawflags = rv3d->twdrawflag;    /* set in calc_manipulator_stats */

	MAN_ITER_AXES_BEGIN(MAN_AXES_TRANSLATE)
	{
		WM_widget_operator(axis, "TRANSFORM_OT_translate");
	}
	MAN_ITER_AXES_END;
	MAN_ITER_AXES_BEGIN(MAN_AXES_ROTATE)
	{
		WM_widget_operator(axis, "TRANSFORM_OT_rotate");
	}
	MAN_ITER_AXES_END;

	if (v3d->twtype & V3D_MANIP_TRANSLATE) {
		MAN_ITER_AXES_BEGIN(MAN_AXES_TRANSLATE)
		{
			/* should be added according to the order of axis */
			WM_widget_set_origin(axis, rv3d->twmat[3]);
			WIDGET_arrow_set_direction(axis, rv3d->twmat[i]);

			WM_widget_flag_disable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;
	}
	else {
		MAN_ITER_AXES_BEGIN(MAN_AXES_TRANSLATE)
		{
			WM_widget_flag_enable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;
	}

	if (v3d->twtype & V3D_MANIP_ROTATE) {
		/* should be added according to the order of axis */
		MAN_ITER_AXES_BEGIN(MAN_AXES_ROTATE)
		{
			WM_widget_set_origin(axis, rv3d->twmat[3]);
			WIDGET_dial_set_direction(axis, rv3d->twmat[i - 3]);

			WM_widget_flag_disable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;
	}
	else {
		MAN_ITER_AXES_BEGIN(MAN_AXES_ROTATE)
		{
			WM_widget_flag_enable(axis, WM_WIDGET_HIDDEN);
		}
		MAN_ITER_AXES_END;
	}
}

void WIDGETGROUP_object_manipulator_draw(const struct bContext *C, struct wmWidgetGroup *wgroup)
{
	Object *ob = ED_object_active_context((bContext *)C);
	ManipulatorGroup *manipulator = MEM_callocN(sizeof(ManipulatorGroup), "manipulator_data");
	RegionView3D *rv3d = CTX_wm_region_view3d(C);
	Scene *scene = CTX_data_scene(C);
	wmWidget *axis;
	short i;

	float color_green[4] = {0.25f, 1.0f, 0.25f, 1.0f};
	float color_red[4] = {1.0f, 0.25f, 0.25f, 1.0f};
	float color_blue[4] = {0.25f, 0.25f, 1.0f, 1.0f};

	if (ob->wgroup == NULL) {
		ob->wgroup = wgroup;
	}

	/* XXX - share this stuff between manipulator draw methods */

	copy_v3_v3(rv3d->twmat[3], scene->twcent);

	manipulator->translate_x = WIDGET_arrow_new(wgroup, "translate_x", WIDGET_ARROW_STYLE_NORMAL);
	WIDGET_arrow_set_color(manipulator->translate_x, color_red);
	manipulator->translate_y = WIDGET_arrow_new(wgroup, "translate_y", WIDGET_ARROW_STYLE_NORMAL);
	WIDGET_arrow_set_color(manipulator->translate_y, color_green);
	manipulator->translate_z = WIDGET_arrow_new(wgroup, "translate_z", WIDGET_ARROW_STYLE_NORMAL);
	WIDGET_arrow_set_color(manipulator->translate_z, color_blue);

	test_manipulator_axis(C);
	drawflags = rv3d->twdrawflag;    /* set in calc_manipulator_stats */

	MAN_ITER_AXES_BEGIN(MAN_AXES_TRANSLATE)
	{
		WM_widget_set_origin(axis, rv3d->twmat[3]);
		WIDGET_arrow_set_direction(axis, rv3d->twmat[i]);
		WM_widget_flag_disable(axis, WM_WIDGET_HIDDEN);
	}
	MAN_ITER_AXES_END;
}