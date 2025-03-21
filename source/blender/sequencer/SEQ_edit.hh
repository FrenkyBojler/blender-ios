/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

struct Editing;
struct ListBase;
struct Main;
struct Scene;
struct Strip;

namespace blender::seq {

bool strip_swap(Scene *scene, Strip *strip_a, Strip *strip_b, const char **r_error_str);
/**
 * Move sequence to seqbase.
 *
 * \param scene: Scene containing the editing
 * \param seqbase_src: seqbase where `seq` is located
 * \param seq: Sequence to move
 * \param seqbase_dst: Target seqbase
 */
bool strip_move_to_seqbase(Scene *scene,
                           ListBase *seqbase_src,
                           Strip *strip,
                           ListBase *seqbase_dst);
/**
 * Move sequence to meta sequence.
 *
 * \param scene: Scene containing the editing
 * \param strip_src: Sequence to move
 * \param meta_strip_dst: Target Meta sequence
 * \param r_error_str: Error message
 */
bool strip_move_to_meta(Scene *scene,
                        Strip *strip_src,
                        Strip *meta_strip_dst,
                        const char **r_error_str);
/**
 * Flag seq and its users (effects) for removal.
 */
void flag_strips_for_removal(Scene *scene, ListBase *seqbase, Strip *strip);
/**
 * Remove all flagged sequences, return true if sequence is removed.
 */
void remove_flagged_strips(Scene *scene, ListBase *seqbase);
void update_muting(Editing *ed);

enum eSplitMethod {
  SPLIT_SOFT,
  SPLIT_HARD,
};

/**
 * Split Sequence at timeline_frame in two.
 *
 * \param bmain: Main in which Sequence is located
 * \param scene: Scene in which Sequence is located
 * \param seqbase: ListBase in which Sequence is located
 * \param seq: Sequence to be split
 * \param timeline_frame: frame at which seq is split.
 * \param method: affects type of offset to be applied to resize Sequence
 * \return The newly created sequence strip. This is always Sequence on right side.
 */
Strip *strip_split(Main *bmain,
                   Scene *scene,
                   ListBase *seqbase,
                   Strip *strip,
                   int timeline_frame,
                   eSplitMethod method,
                   const char **r_error);
/**
 * Find gap after initial_frame and move strips on right side to close the gap
 *
 * \param scene: Scene in which strips are located
 * \param seqbase: ListBase in which strips are located
 * \param initial_frame: frame on timeline from where gaps are searched for
 * \param remove_all_gaps: remove all gaps instead of one gap
 * \return true if gap is removed, otherwise false
 */
bool remove_gaps(Scene *scene, ListBase *seqbase, int initial_frame, bool remove_all_gaps);
void strip_name_set(Scene *scene, Strip *strip, const char *new_name);

}  // namespace blender::seq
