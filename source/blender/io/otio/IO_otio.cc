#include "IO_otio.hh"

#include "BKE_context.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "ED_screen.hh"
#include "RNA_define.hh"

#include "exporter/otio_exporter.hh"


namespace blender::io::otio {

void register_exporter() {
  /* Placeholder for operator registration. */
}

static wmOperatorStatus wm_otio_export_execute(bContext *C, wmOperator *op)
{
    test_stripe_iteration(C, op);
    return OPERATOR_FINISHED;
}

void OTIO_OT_export(wmOperatorType *ot)
{
    /* identifiers */
    ot->name = "Export OTIO";
    ot->idname = "SEQUENCER_OT_otio_export";
    ot->description = "Export project as OpenTimelineIO file";
    
    /* flags */
    ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

    /* api callbacks */
    ot->exec = wm_otio_export_execute;
    ot->invoke = WM_operator_filesel;

    /* properties */
    RNA_def_string_file_path(ot->srna, "filepath", nullptr, 1024, "File Path", "Filepath used for exporting the OTIO file");

    /* Optional: Ensure this can only run if there is a sequencer scene */
    ot->poll = ED_operator_sequencer_active; 
}

} // namespace blender::io::otio
