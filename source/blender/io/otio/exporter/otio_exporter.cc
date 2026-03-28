#include "otio_exporter.hh"

#include "BKE_context.hh"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"
#include "BKE_main.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "SEQ_iterator.hh"
#include "SEQ_sequencer.hh"
#include "RNA_access.hh"
#include "BKE_report.hh"

#include "opentimelineio/serialization.h"
#include "opentimelineio/errorStatus.h"
 
 #include "opentimelineio/clip.h"
 #include "opentimelineio/externalReference.h"
 #include "opentimelineio/gap.h"
 #include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentime/rationalTime.h"
#include "opentime/timeRange.h"

#include <algorithm>

/**
 * Create a timeline object
 * Create a stack object
 * Create a track object
 * 
 * Initially iterate over all stripes
 * and make a seperate List of audio and video stripes
 * then seperate them by channels
 * Create clip object from each strips 
 * and add them to track by their channel number
 * 
*/
namespace blender::io::otio {

static void test_otio(const char * /*filepath*/)
{
  /* Basic test of OTIO classes using SerializableObject::Retainer for reference counting. */
  OTIO_NS::SerializableObject::Retainer<OTIO_NS::Timeline> timeline(new OTIO_NS::Timeline);

  /* No explicit delete needed, Retainer handles it. */
}

/* Create a MediaReference object from a Strip */
static OTIO_NS::MediaReference *build_media_reference(const char *basepath, Strip *strip)
{
  char filepath[1024] = "";
  bool has_media = false;

  if (strip->type == STRIP_TYPE_MOVIE && strip->data) {
    if (strip->data->dirpath[0]) {
      BLI_strncpy(filepath, strip->data->dirpath, sizeof(filepath));
      if (strip->data->stripdata) {
        BLI_path_append(filepath, sizeof(filepath), strip->data->stripdata[0].filename);
        has_media = true;
      }
    }
    else if (strip->data->stripdata) {
      BLI_strncpy(filepath, strip->data->stripdata[0].filename, sizeof(filepath));
      has_media = true;
    }
  }
  else if ((strip->type == STRIP_TYPE_SOUND || strip->type == STRIP_TYPE_SOUND_HD) &&
           strip->sound) {
    BLI_strncpy(filepath, strip->sound->filepath, sizeof(filepath));
    has_media = true;
  }

  if (!has_media) {
    /* Handle strips without direct media references (e.g., Effect strips) */
    printf("No media reference for strip: %s (Type: %d)\n", strip->name, strip->type);
    return nullptr;
  }

  /* Resolve Blender's relative path (//) to an absolute path */
  BLI_path_abs(filepath, basepath);
  printf("Resolved Path: %s\n", filepath);

  return new OTIO_NS::ExternalReference(filepath);
}

/* Create a Clip object from a Strip */
static OTIO_NS::SerializableObject::Retainer<OTIO_NS::Clip> build_clip(const char *basepath,
                                                                Strip *strip,
                                                                double fps)
{
  /* Calculate Clip length and source range */
  int clip_length = strip->enddisp - strip->startdisp;
  if (clip_length <= 0) {
    /* Fallback to length if enddisp is not properly set or 0-length strip */
    clip_length = strip->len > 0 ? strip->len : 1;
  }

  opentime::RationalTime clip_duration(clip_length, fps);
  opentime::RationalTime source_start(strip->startofs, fps);
  opentime::TimeRange source_range(source_start, clip_duration);

  /* Resolve Media Reference */
  OTIO_NS::SerializableObject::Retainer<OTIO_NS::MediaReference> media_ref = build_media_reference(
      basepath, strip);

  OTIO_NS::Clip *clip = new OTIO_NS::Clip(strip->name[0] ? strip->name : "Clip", media_ref, source_range);

  std::string media_path = "none";
  if (clip->media_reference() &&
      dynamic_cast<OTIO_NS::ExternalReference *>(clip->media_reference())) {
    media_path = dynamic_cast<OTIO_NS::ExternalReference *>(clip->media_reference())->target_url();
  }

  printf("Created Clip: %s, duration: %f frames, media: %s\n",
         clip->name().c_str(),
         clip->duration().value(),
         media_path.c_str());

  return clip;
}

/*Build Track with Clips on a channel*/
static OTIO_NS::SerializableObject::Retainer<OTIO_NS::Track> build_track(Main *bmain, Scene *scene, blender::Span<Strip *> strip_list)
{
    OTIO_NS::SerializableObject::Retainer<OTIO_NS::Track> track = new OTIO_NS::Track();
    
    if (strip_list.is_empty()) {
        return track;
    }

    /* Sort strips by start time on the timeline */
    blender::Vector<Strip *> sorted_strips(strip_list);
    std::sort(sorted_strips.begin(), sorted_strips.end(), [](Strip *a, Strip *b) {
        return a->startdisp < b->startdisp;
    });

    /* Determine track kind from the first strip */
    Strip *first_strip = sorted_strips.first();
    if (first_strip->type == STRIP_TYPE_SOUND || first_strip->type == STRIP_TYPE_SOUND_HD) {
        track->set_kind(OTIO_NS::Track::Kind::audio);
    } else {
        track->set_kind(OTIO_NS::Track::Kind::video);
    }

    double fps = (double)scene->r.frs_sec / (double)scene->r.frs_sec_base;
    int current_frame = scene->r.sfra;
    const char *basepath = BKE_main_blendfile_path(bmain);

    /* Loop through the Strips with Gap checking */
    for (Strip *strip : sorted_strips) {
        if (strip->startdisp > current_frame) {
            /* Create Gap */
            int gap_length = strip->startdisp - current_frame;
            opentime::RationalTime gap_duration(gap_length, fps);
            OTIO_NS::Gap *gap = new OTIO_NS::Gap(gap_duration);
            track->append_child(gap);
        }

        /* Create Clip */
        OTIO_NS::SerializableObject::Retainer<OTIO_NS::Clip> clip = build_clip(basepath, strip, fps);
        track->append_child(clip);
        
        current_frame = std::max(current_frame, strip->startdisp + (int)clip->duration().value());
    }

    return track;
}

void test_stripe_iteration(bContext *C, wmOperator *op)
{
    UNUSED_VARS(op);
    Scene *scene = CTX_data_sequencer_scene(C);
    Editing *ed = seq::editing_get(scene);
    if (!ed) {
        printf("No Sequence Editor found in the current scene.\n");
        return;
    }

    blender::Map<int, blender::Vector<Strip *>> audio_strip_list;
    blender::Map<int, blender::Vector<Strip *>> video_strip_list;

    seq::foreach_strip(&ed->seqbase, [&](Strip *strip) -> bool {
        if (!strip) {
            return true;
        }

        if (strip->type == STRIP_TYPE_SOUND || strip->type == STRIP_TYPE_SOUND_HD) {
            audio_strip_list.lookup_or_add_default(strip->channel).append(strip);
        } else {
            video_strip_list.lookup_or_add_default(strip->channel).append(strip);
        }

        printf("Strip: %s\n Channel: %d\n Type: %d\n length: %d\n startofs: %f\n endofs: %f\n startdisp: %d\n enddisp: %d\n saturation: %f\n mul: %f\n streamindex(short): %d\n effect strip inputs: [input1: %s, input2: %s]\n speed factor: %f\n starting frame(sfra): %d\n\n", 
            strip->name[0] ? strip->name : "<none>", 
            strip->channel,
            strip->type,
            strip->len,
            strip->startofs,
            strip->endofs,
            strip->startdisp,
            strip->enddisp,
            strip->sat,
            strip->mul,
            strip->streamindex,
            strip->input1 ? strip->input1->name : "<none>",
            strip->input2 ? strip->input2->name : "<none>",
            strip->speed_factor,
            strip->sfra
        );
        return true;
    });

    /* Build Stack */
    OTIO_NS::SerializableObject::Retainer<OTIO_NS::Stack> stack = new OTIO_NS::Stack();

    /* 1. Sort Video Channels */
    blender::Vector<int> sorted_video_keys;
    for (const int key : video_strip_list.keys()) {
        sorted_video_keys.append(key);
    }
    std::sort(sorted_video_keys.begin(), sorted_video_keys.end());

    /* 2. Sort Audio Channels */
    blender::Vector<int> sorted_audio_keys;
    for (const int key : audio_strip_list.keys()) {
        sorted_audio_keys.append(key);
    }
    std::sort(sorted_audio_keys.begin(), sorted_audio_keys.end());

    /* 3. Build Video Tracks (First) */
    for (const int key : sorted_video_keys) {
        printf("Processing Video Channel: %d\n", key);
        OTIO_NS::SerializableObject::Retainer<OTIO_NS::Track> track = build_track(
            CTX_data_main(C), scene, video_strip_list.lookup(key));
        char track_name[64];
        BLI_snprintf(track_name, sizeof(track_name), "Video Channel %d", key);
        track->set_name(track_name);
        stack->append_child(track);
        printf("Added Video Track to Stack: %s\n", track->name().c_str());
    }

    /* 4. Build Audio Tracks (Second) */
    for (const int key : sorted_audio_keys) {
        printf("Processing Audio Channel: %d\n", key);
        OTIO_NS::SerializableObject::Retainer<OTIO_NS::Track> track = build_track(
            CTX_data_main(C), scene, audio_strip_list.lookup(key));
        char track_name[64];
        BLI_snprintf(track_name, sizeof(track_name), "Audio Channel %d", key);
        track->set_name(track_name);
        stack->append_child(track);
        printf("Added Audio Track to Stack: %s\n", track->name().c_str());
    }

    /* 5. Build Timeline */
    OTIO_NS::SerializableObject::Retainer<OTIO_NS::Timeline> timeline = new OTIO_NS::Timeline(
        scene->id.name + 2); /* scene->id.name starts with "SN" */
    timeline->set_tracks(stack);

    printf("Built Timeline: %s with %d tracks in stack\n", 
           timeline->name().c_str(), (int)stack->children().size());

    /* 6. Write to File */
    char filepath[1024];
    RNA_string_get(op->ptr, "filepath", filepath);

    if (filepath[0] == '\0') {
        printf("No filepath provided, skipping file write.\n");
        return;
    }

    OTIO_NS::ErrorStatus error_status;
    bool success = timeline->to_json_file(filepath, &error_status);

    if (!success) {
        printf("Failed to write OTIO file: %s\n", error_status.details.c_str());
        BKE_reportf(op->reports, RPT_ERROR, "Failed to write OTIO file: %s", error_status.details.c_str());
    } else {
        printf("Successfully wrote OTIO file to: %s\n", filepath);
        BKE_reportf(op->reports, RPT_INFO, "Exported OTIO: %s", filepath);
    }
}

} // namespace blender::io::otio

