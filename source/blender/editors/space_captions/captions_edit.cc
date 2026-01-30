#include "BLI_utildefines.h"
#include "BLI_vector.hh"        
#include "BLI_vector_set.hh"    
#include "BLI_span.hh"          
#include "BLI_set.hh"           
#include "BLI_map.hh"
#include "BLI_listbase_iterator.hh"        

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"

#include "captions_intern.hh"

#include "BKE_context.hh"
#include "BKE_scene.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_types.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"
#include "SEQ_add.hh"


namespace blender {

    static int get_extend_right(int start_frame, int channel, ListBaseT<struct CaptionsStripRef> *refs, int max_extend) {
        
        int next_strip_start = start_frame + max_extend;
        for (CaptionsStripRef &ref : *refs) {
            Strip *strip = ref.strip;

            /* Only check strips on the same channel */
            if (strip->channel != channel) {
                continue;
            }

            float other_start = strip->left_handle();
            if (other_start >= start_frame && other_start < next_strip_start) {
                next_strip_start = other_start;
            }
        }

        return next_strip_start - start_frame;
    }

    /* -------------------------------------------------------------------- */
    /** \name Add Caption Operator
     * \{ */

    static wmOperatorStatus captions_add_exec(bContext *C, wmOperator *op)
    {
        seq::LoadData load_data;
        memset(&load_data, 0, sizeof(load_data));
        Scene *scene = CTX_data_sequencer_scene(C);
        Editing *ed = seq::editing_ensure(scene);

        int start_frame = scene->r.cfra;
        int channel = ed->captions_act_channel->index;

        /* Maybe add RNA option for that */
        load_data.start_frame = start_frame;
        load_data.channel = channel;
        load_data.effect.type = STRIP_TYPE_TEXT;

        int length = 100; // TODO: change to DEFAULT_IMG_STRIP_LENGTH 
        if (RNA_struct_find_property(op->ptr, "length")) {
            length = RNA_int_get(op->ptr, "length");
        }
        length = get_extend_right(start_frame, channel, &ed->captions_strips, length);

        load_data.effect.length = length;

        Strip *strip = seq::add_effect_strip(scene, &ed->seqbase, &load_data);

        DEG_id_tag_update(&scene->id, ID_RECALC_SEQUENCER_STRIPS);

        ed->captions_cache_dirty = true;
        tag_redraw(CTX_wm_region(C), scene);

        WM_main_add_notifier(NC_SCENE | ND_SEQUENCER | NA_ADDED, CTX_data_sequencer_scene(C));

        return OPERATOR_FINISHED;
    }

    static bool captions_add_poll(bContext *C)
    {
        Scene *scene = CTX_data_sequencer_scene(C);
        const int cfra = scene->r.cfra;
        Editing *ed = seq::editing_ensure(scene);

        if(ed -> captions_cache_dirty) {
            update_current_strips(scene);
        }

        for (CaptionsStripRef &ref : ed->captions_strips) {
            Strip *strip = ref.strip;
            if (strip->intersects_frame(scene, cfra)) {
                return false;
            }
        }

        return true;
    }

    void CAPTIONS_OT_caption_add(wmOperatorType *ot)
    {
        /* Identifiers. */
        ot->name = "Add Caption";
        ot->idname = "CAPTIONS_OT_caption_add";
        ot->description = "Add a new caption at the current frame";

        /* API callbacks. */
        //  ot->invoke = sequencer_snap_invoke;
        ot->exec = captions_add_exec;
        ot->poll = captions_add_poll; // TODO: Make it check for intersection with strip and for active space, I guess.

        /* Flags. */
        ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

        /* Properties. */
        PropertyRNA *prop = RNA_def_int(ot->srna,
                         "length",
                         100,  // TODO: change to DEFAULT_IMG_STRIP_LENGTH
                         MINAFRAME,
                         MAXFRAME,
                         "Length",
                         "Length of the caption strip in frames",
                         1,
                         500);
        RNA_def_property_flag(prop, PROP_SKIP_SAVE);

    }

} // namespace blender