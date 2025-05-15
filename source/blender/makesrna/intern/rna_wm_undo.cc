#include "BLI_listbase.h"
#include "BLI_utildefines.h"
#include "BLI_string.h"

#include "DNA_windowmanager_types.h"
#include "BKE_undo_system.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_types.hh"

#include "rna_internal.hh" 

#ifdef RNA_RUNTIME

extern StructRNA RNA_UndoStep;

static UndoStack *get_undo_stack(PointerRNA *ptr)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(ptr->data);
  return wm->undo_stack;
}

static int rna_UndoStack_steps_length(PointerRNA *ptr)
{
  UndoStack *ustack = get_undo_stack(ptr);
  return BLI_listbase_count(&ustack->steps);
}

static PointerRNA rna_UndoStack_steps_get(CollectionPropertyIterator *iter)
{
  UndoStep *us = static_cast<UndoStep *>(rna_iterator_listbase_get(iter));
  return RNA_pointer_create_with_parent(iter->parent, &RNA_UndoStep, us);
}

static void rna_UndoStack_steps_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  UndoStack *ustack = get_undo_stack(ptr);
  rna_iterator_listbase_begin(iter, ptr, &ustack->steps, nullptr);
}

static int rna_UndoStack_active_index_get(PointerRNA *ptr)
{
  UndoStack *ustack = get_undo_stack(ptr);
  int index = 0;
  LISTBASE_FOREACH_INDEX (UndoStep *, us, &ustack->steps, index) {
    if (us == ustack->step_active) {
      return index;
    }
  }
  return -1;
}

static void rna_UndoStep_name_get(PointerRNA *ptr, char *value)
{
  UndoStep *us = static_cast<UndoStep *>(ptr->data);
  BLI_strncpy(value, us->name, sizeof(us->name));
}

static int rna_UndoStep_name_length(PointerRNA *ptr)
{
  UndoStep *us = static_cast<UndoStep *>(ptr->data);
  return strlen(us->name);
}

static void rna_UndoStep_type_name_get(PointerRNA *ptr, char *value)
{
  UndoStep *us = static_cast<UndoStep *>(ptr->data);
  if (us->type && us->type->name) {
    BLI_strncpy(value, us->type->name, 64);
  }
  else {
    value[0] = '\0';
  }
}

static int rna_UndoStep_type_name_length(PointerRNA *ptr)
{
  UndoStep *us = static_cast<UndoStep *>(ptr->data);
  return us->type && us->type->name ? strlen(us->type->name) : 0;
}

static bool rna_UndoStep_skip_get(PointerRNA *ptr)
{
  UndoStep *us = static_cast<UndoStep *>(ptr->data);
  return us ? us->skip : false;
}

#endif // RNA_RUNTIME

void RNA_def_undo(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /*
   * UndoStep
   */
  srna = RNA_def_struct(brna, "UndoStep", nullptr);
  RNA_def_struct_ui_text(srna, "Undo Step", "A single step in the undo history");
  RNA_def_struct_flag(srna, STRUCT_NO_DATABLOCK_IDPROPERTIES);

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(prop,
                              "rna_UndoStep_name_get",
                              "rna_UndoStep_name_length",
                              nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Name", "Label of the undo step");

  prop = RNA_def_property(srna, "type", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(prop,
                              "rna_UndoStep_type_name_get",
                              "rna_UndoStep_type_name_length",
                              nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Type", "Type name of the undo step");

  prop = RNA_def_property(srna, "skip", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_boolean_funcs(prop,
                              "rna_UndoStep_skip_get",
                              nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE | PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, 
                          "Skip", 
                          "If true, this step should not be shown to the user for undo/redo selection.");

  /*
   * UndoStack
   */
  srna = RNA_def_struct(brna, "UndoStack", nullptr);
  RNA_def_struct_ui_text(srna, "Undo Stack", "Read-only access to the undo stack");
  RNA_def_struct_flag(srna, STRUCT_NO_DATABLOCK_IDPROPERTIES);

  prop = RNA_def_property(srna, "steps", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "UndoStep");
  RNA_def_property_ui_text(prop, "Steps", "List of undo steps");
  RNA_def_property_collection_funcs(prop,
                                    "rna_UndoStack_steps_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_UndoStack_steps_get",
                                    "rna_UndoStack_steps_length",
                                    nullptr, nullptr, nullptr);

  prop = RNA_def_property(srna, "active_index", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_int_funcs(prop, "rna_UndoStack_active_index_get", nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Active Index", "Index of currently active undo step");
}
