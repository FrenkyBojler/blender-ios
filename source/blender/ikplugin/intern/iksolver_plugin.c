/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup ikplugin
 */

#include "MEM_guardedalloc.h"

#include "BIK_api.h"
#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_armature.h"
#include "BKE_constraint.h"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_object_types.h"

#include "IK_solver.h"
#include "iksolver_plugin.h"

#include <string.h> /* memcpy */

#define USE_NONUNIFORM_SCALE

/* ********************** THE IK SOLVER ******************* */

/* allocates PoseTree, and links that to root bone/channel */
/* NOTE: detecting the IK chain is duplicate code...
 * in drawarmature.c and in transform_conversions.c */
static void initialize_posetree(struct Object *UNUSED(ob), bPoseChannel *pchan_tip)
{
  bPoseChannel *curchan, *pchan_root = NULL, *chanlist[256], **oldchan;
  PoseTree *tree;
  PoseTarget *target;
  bConstraint *con;
  bKinematicConstraint *data;
  int a, t, segcount = 0, size, newsize, *oldparent, parent;

  /* find IK constraint, and validate it */
  for (con = pchan_tip->constraints.first; con; con = con->next) {
    if (con->type == CONSTRAINT_TYPE_KINEMATIC) {
      data = (bKinematicConstraint *)con->data;
      if (data->flag & CONSTRAINT_IK_AUTO) {
        break;
      }
      if (data->tar == NULL) {
        continue;
      }
      if (data->tar->type == OB_ARMATURE && data->subtarget[0] == 0) {
        continue;
      }
      if ((con->flag & (CONSTRAINT_DISABLE | CONSTRAINT_OFF)) == 0 && (con->enforce != 0.0f)) {
        break;
      }
    }
  }
  if (con == NULL) {
    return;
  }

  /* exclude tip from chain? */
  if (!(data->flag & CONSTRAINT_IK_TIP)) {
    pchan_tip = pchan_tip->parent;
  }

  /* Find the chain's root & count the segments needed */
  for (curchan = pchan_tip; curchan; curchan = curchan->parent) {
    pchan_root = curchan;

    curchan->flag |= POSE_CHAIN; /* don't forget to clear this */
    chanlist[segcount] = curchan;
    segcount++;

    if (segcount == data->rootbone || segcount > 255) {
      break; /* 255 is weak */
    }
  }
  if (!segcount) {
    return;
  }

  /* setup the chain data */

  /* we make tree-IK, unless all existing targets are in this chain */
  for (tree = pchan_root->iktree.first; tree; tree = tree->next) {
    for (target = tree->targets.first; target; target = target->next) {
      curchan = tree->pchan[target->tip];
      if (curchan->flag & POSE_CHAIN) {
        curchan->flag &= ~POSE_CHAIN;
      }
      else {
        break;
      }
    }
    if (target) {
      break;
    }
  }

  /* create a target */
  target = MEM_callocN(sizeof(PoseTarget), "posetarget");
  target->con = con;
  pchan_tip->flag &= ~POSE_CHAIN;

  if (tree == NULL) {
    /* make new tree */
    tree = MEM_callocN(sizeof(PoseTree), "posetree");

    tree->type = CONSTRAINT_TYPE_KINEMATIC;

    tree->iterations = data->iterations;
    tree->totchannel = segcount;
    tree->stretch = (data->flag & CONSTRAINT_IK_STRETCH);

    tree->pchan = MEM_callocN(segcount * sizeof(void *), "ik tree pchan");
    tree->parent = MEM_callocN(segcount * sizeof(int), "ik tree parent");
    for (a = 0; a < segcount; a++) {
      tree->pchan[a] = chanlist[segcount - a - 1];
      tree->parent[a] = a - 1;
    }
    target->tip = segcount - 1;

    /* AND! link the tree to the root */
    BLI_addtail(&pchan_root->iktree, tree);
  }
  else {
    tree->iterations = MAX2(data->iterations, tree->iterations);
    tree->stretch = tree->stretch && !(data->flag & CONSTRAINT_IK_STRETCH);

    /* Skip common pose channels and add remaining. */
    size = MIN2(segcount, tree->totchannel);
    a = t = 0;
    while (a < size && t < tree->totchannel) {
      /* locate first matching channel */
      for (; t < tree->totchannel && tree->pchan[t] != chanlist[segcount - a - 1]; t++) {
        /* pass */
      }
      if (t >= tree->totchannel) {
        break;
      }
      for (; a < size && t < tree->totchannel && tree->pchan[t] == chanlist[segcount - a - 1];
           a++, t++) {
        /* pass */
      }
    }

    segcount = segcount - a;
    target->tip = tree->totchannel + segcount - 1;

    if (segcount > 0) {
      for (parent = a - 1; parent < tree->totchannel; parent++) {
        if (tree->pchan[parent] == chanlist[segcount - 1]->parent) {
          break;
        }
      }

      /* shouldn't happen, but could with dependency cycles */
      if (parent == tree->totchannel) {
        parent = a - 1;
      }

      /* resize array */
      newsize = tree->totchannel + segcount;
      oldchan = tree->pchan;
      oldparent = tree->parent;

      tree->pchan = MEM_callocN(newsize * sizeof(void *), "ik tree pchan");
      tree->parent = MEM_callocN(newsize * sizeof(int), "ik tree parent");
      memcpy(tree->pchan, oldchan, sizeof(void *) * tree->totchannel);
      memcpy(tree->parent, oldparent, sizeof(int) * tree->totchannel);
      MEM_freeN(oldchan);
      MEM_freeN(oldparent);

      /* add new pose channels at the end, in reverse order */
      for (a = 0; a < segcount; a++) {
        tree->pchan[tree->totchannel + a] = chanlist[segcount - a - 1];
        tree->parent[tree->totchannel + a] = tree->totchannel + a - 1;
      }
      tree->parent[tree->totchannel] = parent;

      tree->totchannel = newsize;
    }

    /* move tree to end of list, for correct evaluation order */
    BLI_remlink(&pchan_root->iktree, tree);
    BLI_addtail(&pchan_root->iktree, tree);
  }

  /* add target to the tree */
  BLI_addtail(&tree->targets, target);
  /* mark root channel having an IK tree */
  pchan_root->flag |= POSE_IKTREE;
}

/* transform from bone(b) to bone(b+1), store in chan_mat */
static void make_dmats(bPoseChannel *pchan)
{
  if (pchan->parent) {
    float iR_parmat[4][4];
    invert_m4_m4(iR_parmat, pchan->parent->pose_mat);
    mul_m4_m4m4(pchan->chan_mat, iR_parmat, pchan->pose_mat); /* delta mat */
  }
  else {
    copy_m4_m4(pchan->chan_mat, pchan->pose_mat);
  }
}

/* applies IK matrix to pchan, IK is done separated */
/* formula: pose_mat(b) = pose_mat(b-1) * diffmat(b-1, b) * ik_mat(b) */
static void where_is_ik_bone(Object *ob,
                             bPoseChannel *pchan,
                             float ik_mat[3][3],
                             float ikfk_blend)
{
  float vec[3], ikmat[4][4], fk_mat[4][4], blended_mat[4][4], ik_result[4][4];

  /* Преобразуем IK-матрицу 3x3 в 4x4 */
  copy_m4_m3(ikmat, ik_mat);

  /* Вычисляем FK-позу на основе chan_mat */
  if (pchan->parent) {
    mul_m4_m4m4(fk_mat, pchan->parent->pose_mat, pchan->chan_mat);
  }
  else {
    copy_m4_m4(fk_mat, pchan->chan_mat);
  }

  /* Применяем FK как базовую позу */
  copy_m4_m4(pchan->pose_mat, fk_mat);

#ifdef USE_NONUNIFORM_SCALE
  /* Корректируем для неравномерного масштаба */
  float scale[3];
  mat4_to_size(scale, pchan->pose_mat);
  normalize_v3_length(pchan->pose_mat[0], scale[1]);
  normalize_v3_length(pchan->pose_mat[2], scale[1]);
#endif

  /* Применяем IK-матрицу к FK-позе для получения IK-результата */
  mul_m4_m4m4(ik_result, pchan->pose_mat, ikmat);

#ifdef USE_NONUNIFORM_SCALE
  float ik_scale[3];
  mat3_to_size(ik_scale, ik_mat);
  normalize_v3_length(ik_result[0], scale[0] * ik_scale[0]);
  normalize_v3_length(ik_result[2], scale[2] * ik_scale[2]);
#endif

  /* Смешиваем FK и IK-результат с использованием ikfk_blend */
  CLAMP(ikfk_blend, 0.0f, 1.0f); /* Ограничиваем ikfk_blend между 0 и 1 */
  blend_m4_m4m4(blended_mat, fk_mat, ik_result, ikfk_blend);
  copy_m4_m4(pchan->pose_mat, blended_mat);

  /* Рассчитываем голову и хвост */
  copy_v3_v3(pchan->pose_head, pchan->pose_mat[3]);
  BKE_pose_where_is_bone_tail(ob, pchan);

  /* Обновляем chan_mat для синхронизации FK с текущей позой (Match) */
  copy_m4_m4(pchan->chan_mat, pchan->pose_mat);

  pchan->flag |= POSE_DONE;
}

/**
 * Called from within the core #BKE_pose_where_is loop, all animation-systems and constraints
 * were executed & assigned. Now as last we do an IK pass.
 */
static void execute_posetree(struct Depsgraph *depsgraph,
                             struct Scene *scene,
                             Object *ob,
                             PoseTree *tree)
{
  float R_parmat[3][3], R_parmat4[4][4], identity[3][3];
  float iR_parmat[3][3];
  float R_bonemat[3][3], R_bonemat4[4][4];
  float goalrot[3][3], goalpos[3];
  float rootmat[4][4], imat[4][4];
  float goal[4][4], goalinv[4][4];
  float irest_basis[3][3], full_basis[3][3];
  float end_pose[4][4], world_pose[4][4];
  float basis[3][3], rest_basis[3][3], start[3], *ikstretch = NULL;
  float resultinf = 0.0f;
  int a, flag, hasstretch = 0, resultblend = 0;
  bPoseChannel *pchan;
  IK_Segment *seg, *parent, **iktree, *iktarget;
  IK_Solver *solver;
  PoseTarget *target;
  bKinematicConstraint *data, *poleangledata = NULL;
  Bone *bone;

  if (tree->totchannel == 0) {
    return;
  }

  iktree = MEM_mallocN(sizeof(void *) * tree->totchannel, "ik tree");

  /* Инициализируем ikfk_blend по умолчанию */
  tree->ikfk_blend = 1.0f; /* Полный IK по умолчанию */

  /* Сохраняем FK-ориентацию для каждого канала */
  float (*fk_orientations)[4][4] = MEM_mallocN(sizeof(float[4][4]) * tree->totchannel, "fk orientations");
  for (a = 0; a < tree->totchannel; a++) {
    pchan = tree->pchan[a];
    copy_m4_m4(fk_orientations[a], pchan->chan_mat); /* Сохраняем исходную FK-позу */
  }

  for (a = 0; a < tree->totchannel; a++) {
    float length;
    pchan = tree->pchan[a];
    bone = pchan->bone;

    /* set DoF flag */
    flag = 0;
    if (!(pchan->ikflag & BONE_IK_NO_XDOF) && !(pchan->ikflag & BONE_IK_NO_XDOF_TEMP)) {
      flag |= IK_XDOF;
    }
    if (!(pchan->ikflag & BONE_IK_NO_YDOF) && !(pchan->ikflag & BONE_IK_NO_YDOF_TEMP)) {
      flag |= IK_YDOF;
    }
    if (!(pchan->ikflag & BONE_IK_NO_ZDOF) && !(pchan->ikflag & BONE_IK_NO_ZDOF_TEMP)) {
      flag |= IK_ZDOF;
    }

    if (tree->stretch && (pchan->ikstretch > 0.0f)) {
      flag |= IK_TRANS_YDOF; /* Поддержка IK Stretch */
      hasstretch = 1;
    }

    seg = iktree[a] = IK_CreateSegment(flag);

    /* find parent */
    if (a == 0) {
      parent = NULL;
    }
    else {
      parent = iktree[tree->parent[a]];
    }

    IK_SetParent(seg, parent);

    unit_m4(R_bonemat4);
    unit_m4(R_parmat4);

    /* get the matrix that transforms from prevbone into this bone */
    mul_m4_m4m4(R_bonemat4, ob->object_to_world, pchan->pose_mat);
    copy_m3_m4(R_bonemat, R_bonemat4);

    /* gather transformations for this IK segment */
    if (pchan->parent) {
      mul_m4_m4m4(R_parmat4, ob->object_to_world, pchan->parent->pose_mat);
      copy_m3_m4(R_parmat, R_parmat4);
    }
    else {
      unit_m3(R_parmat);
    }

    /* bone offset */
    if (pchan->parent && (a > 0)) {
      sub_v3_v3v3(start, pchan->pose_head, pchan->parent->pose_tail);
    }
    else {
      start[0] = start[1] = start[2] = 0.0f;
    }

    /* bone length is fixed */
    length = 1.0f;

    /* basis must be pure rotation */
    normalize_m3(R_bonemat);
    normalize_m3(R_parmat);

    /* compute rest basis and its inverse */
    copy_m3_m3(rest_basis, bone->bone_mat);
    transpose_m3_m3(irest_basis, bone->bone_mat);

    /* compute basis with rest_basis removed */
    invert_m3_m3(iR_parmat, R_parmat);
    mul_m3_m3m3(full_basis, iR_parmat, R_bonemat);
    mul_m3_m3m3(basis, irest_basis, full_basis);

    /* transform offset into local bone space */
    mul_m3_v3(iR_parmat, start);

    IK_SetTransform(seg, start, rest_basis, basis, length);

    if (pchan->ikflag & BONE_IK_XLIMIT) {
      IK_SetLimit(seg, IK_X, pchan->limitmin[0], pchan->limitmax[0]);
    }
    if (pchan->ikflag & BONE_IK_YLIMIT) {
      IK_SetLimit(seg, IK_Y, pchan->limitmin[1], pchan->limitmax[1]);
    }
    if (pchan->ikflag & BONE_IK_ZLIMIT) {
      IK_SetLimit(seg, IK_Z, pchan->limitmin[2], pchan->limitmax[2]);
    }

    IK_SetStiffness(seg, IK_X, pchan->stiffness[0]);
    IK_SetStiffness(seg, IK_Y, pchan->stiffness[1]);
    IK_SetStiffness(seg, IK_Z, pchan->stiffness[2]);

    if (tree->stretch && (pchan->ikstretch > 0.0f)) {
      const float ikstretch_sq = square_f(pchan->ikstretch);
      IK_SetStiffness(seg, IK_TRANS_Y, 1.0f - ikstretch_sq);
      IK_SetLimit(seg, IK_TRANS_Y, IK_STRETCH_STIFF_MIN, IK_STRETCH_STIFF_MAX);
    }
  }

  solver = IK_CreateSolver(iktree[0]);

  /* set solver goals */
  pchan = tree->pchan[0];
  if (pchan->parent) {
    copy_m4_m4(rootmat, pchan->parent->pose_mat);
    normalize_m4(rootmat);
  }
  else {
    unit_m4(rootmat);
  }
  copy_v3_v3(rootmat[3], pchan->pose_head);

  mul_m4_m4m4(imat, ob->object_to_world, rootmat);
  invert_m4_m4_safe(goalinv, imat);

  for (target = tree->targets.first; target; target = target->next) {
    float polepos[3];
    int poleconstrain = 0;

    data = (bKinematicConstraint *)target->con->data;

    /* Устанавливаем ikfk_blend из data->orientweight */
    tree->ikfk_blend = data->orientweight; /* Используем orientweight как ikfk_blend */

    BKE_constraint_target_matrix_get(
        depsgraph, scene, target->con, 0, CONSTRAINT_OBTYPE_OBJECT, ob, rootmat, 1.0);

    mul_m4_m4m4(goal, goalinv, rootmat);
    translate_m4(goal, 0.0f, 1.0f, 0.0f);

    copy_v3_v3(goalpos, goal[3]);
    copy_m3_m4(goalrot, goal);
    normalize_m3(goalrot);

    if (data->poletar) {
      BKE_constraint_target_matrix_get(
          depsgraph, scene, target->con, 1, CONSTRAINT_OBTYPE_OBJECT, ob, rootmat, 1.0);

      if (data->flag & CONSTRAINT_IK_SETANGLE) {
        break;
      }

      mul_m4_m4m4(goal, goalinv, rootmat);
      translate_m4(goal, 0.0f, 1.0f, 0.0f);

      copy_v3_v3(polepos, goal[3]);
      poleconstrain = 1;
      resultblend = 1;
      resultinf = target->con->enforce;

      if (data->flag & CONSTRAINT_IK_GETANGLE) {
        poleangledata = data;
        data->flag &= ~CONSTRAINT_IK_GETANGLE;
      }
    }

    if (!resultblend && target->con->enforce != 1.0f) {
      float q1[4], q2[4], q[4];
      float fac = target->con->enforce;
      float mfac = 1.0f - fac;

      pchan = tree->pchan[target->tip];
      copy_m4_m4(end_pose, pchan->pose_mat);
      copy_v3_v3(end_pose[3], pchan->pose_tail);
      mul_m4_series(world_pose, goalinv, ob->object_to_world, end_pose);

      goalpos[0] = fac * goalpos[0] + mfac * world_pose[3][0];
      goalpos[1] = fac * goalpos[1] + mfac * world_pose[3][1];
      goalpos[2] = fac * goalpos[2] + mfac * world_pose[3][2];

      mat3_to_quat(q1, goalrot);
      mat4_to_quat(q2, world_pose);
      interp_qt_qtqt(q, q1, q2, mfac);
      quat_to_mat3(goalrot, q);
    }

    iktarget = iktree[target->tip];

    if ((data->flag & CONSTRAINT_IK_POS) && data->weight != 0.0f) {
      if (poleconstrain) {
        IK_SolverSetPoleVectorConstraint(
            solver, iktarget, goalpos, polepos, data->poleangle, (poleangledata == data)); /* Swivel Angle */
      }
      IK_SolverAddGoal(solver, iktarget, goalpos, data->weight);
    }
    if ((data->flag & CONSTRAINT_IK_ROT) && (data->weight != 0.0f)) {
      if ((data->flag & CONSTRAINT_IK_AUTO) == 0) {
        IK_SolverAddGoalOrientation(solver, iktarget, goalrot, data->weight);
      }
    }
  }

  /* Решаем IK */
  IK_Solve(solver, 0.0f, tree->iterations);

  if (poleangledata) {
    poleangledata->poleangle = IK_SolverGetPoleAngle(solver);
  }

  IK_FreeSolver(solver);

  /* Сохраняем изменения базиса */
  tree->basis_change = MEM_mallocN(sizeof(float[3][3]) * tree->totchannel, "ik basis change");
  if (hasstretch) {
    ikstretch = MEM_mallocN(sizeof(float) * tree->totchannel, "ik stretch");
  }

  for (a = 0; a < tree->totchannel; a++) {
    IK_GetBasisChange(iktree[a], tree->basis_change[a]);

    if (hasstretch) {
      float parentstretch, stretch;
      pchan = tree->pchan[a];
      parentstretch = (tree->parent[a] >= 0) ? ikstretch[tree->parent[a]] : 1.0f;

      if (tree->stretch && (pchan->ikstretch > 0.0f)) {
        float trans[3], length;
        IK_GetTranslationChange(iktree[a], trans);
        length = 1.0f; /* Фиксированная длина, можно позже сделать динамической */
        ikstretch[a] = (length == 0.0f) ? 1.0f : (trans[1] + length) / length; /* IK Stretch */
      }
      else {
        ikstretch[a] = 1.0;
      }

      stretch = (parentstretch == 0.0f) ? 1.0f : ikstretch[a] / parentstretch;
      mul_v3_fl(tree->basis_change[a][0], stretch);
      mul_v3_fl(tree->basis_change[a][1], stretch);
      mul_v3_fl(tree->basis_change[a][2], stretch);
    }

    if (resultblend && resultinf != 1.0f) {
      unit_m3(identity);
      blend_m3_m3m3(tree->basis_change[a], identity, tree->basis_change[a], resultinf);
    }

    IK_FreeSegment(iktree[a]);
  }

  MEM_freeN(iktree);
  if (ikstretch) {
    MEM_freeN(ikstretch);
  }
  MEM_freeN(fk_orientations);
}

static void free_posetree(PoseTree *tree)
{
  BLI_freelistN(&tree->targets);
  if (tree->pchan) {
    MEM_freeN(tree->pchan);
  }
  if (tree->parent) {
    MEM_freeN(tree->parent);
  }
  if (tree->basis_change) {
    MEM_freeN(tree->basis_change);
  }
  MEM_freeN(tree);
}

/* ------------------------------
 * Plugin API for legacy iksolver */

void iksolver_initialize_tree(struct Depsgraph *UNUSED(depsgraph),
                              struct Scene *UNUSED(scene),
                              struct Object *ob,
                              float UNUSED(ctime))
{
  bPoseChannel *pchan;

  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next) {
    if (pchan->constflag & PCHAN_HAS_IK) { /* flag is set on editing constraints */
      initialize_posetree(ob, pchan);      /* will attach it to root! */
    }
  }
  ob->pose->flag &= ~POSE_WAS_REBUILT;
}

void iksolver_execute_tree(struct Depsgraph *depsgraph,
                           struct Scene *scene,
                           Object *ob,
                           bPoseChannel *pchan_root,
                           float ctime)
{
  while (pchan_root->iktree.first) {
    PoseTree *tree = pchan_root->iktree.first;
    int a;

    /* stop on the first tree that isn't a standard IK chain */
    if (tree->type != CONSTRAINT_TYPE_KINEMATIC) {
      return;
    }

    /* 4. walk over the tree for regular solving */
    for (a = 0; a < tree->totchannel; a++) {
      if (!(tree->pchan[a]->flag & POSE_DONE)) { /* successive trees can set the flag */
        BKE_pose_where_is_bone(depsgraph, scene, ob, tree->pchan[a], ctime, 1);
      }
      /* Tell blender that this channel was controlled by IK,
       * it's cleared on each BKE_pose_where_is(). */
      tree->pchan[a]->flag |= POSE_CHAIN;
    }

    /* 5. execute the IK solver */
    execute_posetree(depsgraph, scene, ob, tree);

    /* 6. apply the differences to the channels,
     *    we need to calculate the original differences first */
    for (a = 0; a < tree->totchannel; a++) {
      make_dmats(tree->pchan[a]);
    }

    for (a = 0; a < tree->totchannel; a++) {
      /* sets POSE_DONE */
      where_is_ik_bone(ob, tree->pchan[a], tree->basis_change[a], tree->ikfk_blend);
    }

    /* 7. and free */
    BLI_remlink(&pchan_root->iktree, tree);
    free_posetree(tree);
  }
}

void iksolver_release_tree(struct Scene *UNUSED(scene), struct Object *ob, float UNUSED(ctime))
{
  iksolver_clear_data(ob->pose);
}

void iksolver_clear_data(bPose *pose)
{
  LISTBASE_FOREACH (bPoseChannel *, pchan, &pose->chanbase) {
    if ((pchan->flag & POSE_IKTREE) == 0) {
      continue;
    }

    while (pchan->iktree.first) {
      PoseTree *tree = pchan->iktree.first;

      /* stop on the first tree that isn't a standard IK chain */
      if (tree->type != CONSTRAINT_TYPE_KINEMATIC) {
        break;
      }

      BLI_remlink(&pchan->iktree, tree);
      free_posetree(tree);
    }
  }
}
