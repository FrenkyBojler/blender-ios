/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

/* Light Path Expression (LPE) Pass Evaluation
 *
 * Helper functions for evaluating light path expressions and writing
 * to LPE render passes during path tracing.
 */

/* LPE Event Types - OSL Light Path Expression Specification
 * Events are stored as single character codes that can be combined:
 * - C: Camera
 * - R: Reflection
 * - T: Transmission
 * - V: Volume scatter
 * - L: Light
 * - O: Emission (object)
 * - B: Background
 * - D: Diffuse scatter
 * - G: Glossy scatter
 * - S: Singular/Specular scatter (sharp reflection/refraction)
 * - s: Straight transmission (transparent, no direction change)
 *
 * Each event is stored as a single character in path string.
 * Path examples: "CDL" = Camera -> Diffuse -> Light
 *                "CRDL" = Camera -> Reflection+Diffuse -> Light
 *                "CsL" = Camera -> Straight (transparent) -> Light
 */

#define LPE_MAX_EVENTS 64 /* 8 x 64 bits / 8 bits per char = 64 events */
#define LPE_EVENT_CHUNKS \
  8 /* Number of uint64_t chunks to store events (8 chunks x 8 events = 64) */
#define LPE_MAX_EXPRESSION_LENGTH \
  64 /* Maximum length for an LPE expression string (increased for complex paths) */
#define LPE_MAX_PASSES 32 /* Maximum number of LPE passes (increased for flexibility) */

/* Event character codes matching the parser */
#define LPE_EVENT_CAMERA 'C'
#define LPE_EVENT_REFLECTION 'R'
#define LPE_EVENT_TRANSMISSION 'T'
#define LPE_EVENT_VOLUME 'V'
#define LPE_EVENT_LIGHT 'L'
#define LPE_EVENT_EMISSION 'O'
#define LPE_EVENT_BACKGROUND 'B'
#define LPE_EVENT_DIFFUSE 'D'
#define LPE_EVENT_GLOSSY 'G'
#define LPE_EVENT_SINGULAR 'S'
#define LPE_EVENT_STRAIGHT 's'
#define LPE_EVENT_ALBEDO 'A'

/* Helper functions to access chunked lpe_events array */
ccl_device_inline uint64_t kernel_lpe_get_chunk(ccl_global IntegratorState state, int chunk_idx)
{
  switch (chunk_idx) {
    case 0:
      return INTEGRATOR_STATE(state, path, lpe_events_0);
    case 1:
      return INTEGRATOR_STATE(state, path, lpe_events_1);
    case 2:
      return INTEGRATOR_STATE(state, path, lpe_events_2);
    case 3:
      return INTEGRATOR_STATE(state, path, lpe_events_3);
    case 4:
      return INTEGRATOR_STATE(state, path, lpe_events_4);
    case 5:
      return INTEGRATOR_STATE(state, path, lpe_events_5);
    case 6:
      return INTEGRATOR_STATE(state, path, lpe_events_6);
    case 7:
      return INTEGRATOR_STATE(state, path, lpe_events_7);
    default:
      return 0;
  }
}

ccl_device_inline void kernel_lpe_set_chunk(ccl_global IntegratorState state,
                                            int chunk_idx,
                                            uint64_t value)
{
  switch (chunk_idx) {
    case 0:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_0) = value;
      break;
    case 1:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_1) = value;
      break;
    case 2:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_2) = value;
      break;
    case 3:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_3) = value;
      break;
    case 4:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_4) = value;
      break;
    case 5:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_5) = value;
      break;
    case 6:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_6) = value;
      break;
    case 7:
      INTEGRATOR_STATE_WRITE(state, path, lpe_events_7) = value;
      break;
  }
}

ccl_device_inline uint64_t kernel_lpe_get_shadow_chunk(IntegratorShadowState state, int chunk_idx)
{
  switch (chunk_idx) {
    case 0:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_0);
    case 1:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_1);
    case 2:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_2);
    case 3:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_3);
    case 4:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_4);
    case 5:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_5);
    case 6:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_6);
    case 7:
      return INTEGRATOR_STATE(state, shadow_path, lpe_events_7);
    default:
      return 0;
  }
}

ccl_device_inline void kernel_lpe_set_shadow_chunk(IntegratorShadowState state,
                                                   int chunk_idx,
                                                   uint64_t value)
{
  switch (chunk_idx) {
    case 0:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_0) = value;
      break;
    case 1:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_1) = value;
      break;
    case 2:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_2) = value;
      break;
    case 3:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_3) = value;
      break;
    case 4:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_4) = value;
      break;
    case 5:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_5) = value;
      break;
    case 6:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_6) = value;
      break;
    case 7:
      INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_events_7) = value;
      break;
  }
}

/* Add an event character to the LPE path string */
ccl_device_inline void kernel_lpe_add_event(ccl_global IntegratorState state, char event_char)
{
  const uint event_count = INTEGRATOR_STATE(state, path, lpe_event_count);

  /* Only store up to MAX events */
  if (event_count >= LPE_MAX_EVENTS) {
    return;
  }

  /* Calculate which chunk and bit offset within that chunk */
  const int chunk_idx = event_count / 8;        /* Each chunk holds 8 events */
  const int bit_offset = (event_count % 8) * 8; /* 8 bits per event */

  /* Get current chunk, add event, and write back */
  uint64_t events = kernel_lpe_get_chunk(state, chunk_idx);
  events |= ((uint64_t)event_char << bit_offset);
  kernel_lpe_set_chunk(state, chunk_idx, events);

  INTEGRATOR_STATE_WRITE(state, path, lpe_event_count) = event_count + 1;
}

/* Initialize LPE state for a camera ray */
ccl_device_inline void kernel_lpe_init_camera_ray(ccl_global IntegratorState state)
{
  /* Initialize all LPE event chunks to zero */
  for (int i = 0; i < LPE_EVENT_CHUNKS; i++) {
    kernel_lpe_set_chunk(state, i, 0);
  }

  INTEGRATOR_STATE_WRITE(state, path, lpe_event_count) = 0;
  INTEGRATOR_STATE_WRITE(state, path, lpe_lightgroup_id) = 0;
  INTEGRATOR_STATE_WRITE(state, path, lpe_object_id) = OBJECT_NONE;
  INTEGRATOR_STATE_WRITE(state, path, lpe_material_id) = SHADER_NONE;
  INTEGRATOR_STATE_WRITE(state, path, lpe_pass_id) = -1;

  /* Add camera event as first event */
  kernel_lpe_add_event(state, LPE_EVENT_CAMERA);
}

/* Record a volume scattering event */
ccl_device_inline void kernel_lpe_record_volume_event(ccl_global IntegratorState state)
{
  kernel_lpe_add_event(state, LPE_EVENT_VOLUME);
}

/* Record a light hit */
ccl_device_inline void kernel_lpe_record_light_hit(ccl_global IntegratorState state)
{
  kernel_lpe_add_event(state, LPE_EVENT_LIGHT);
}

/* Record a diffuse reflection bounce */
ccl_device_inline void kernel_lpe_record_diffuse_bounce(ccl_global IntegratorState state)
{
  /* For OSL LPE, diffuse reflection is just 'D' */
  kernel_lpe_add_event(state, LPE_EVENT_DIFFUSE);
}

/* Record a glossy reflection bounce */
ccl_device_inline void kernel_lpe_record_glossy_bounce(ccl_global IntegratorState state)
{
  /* For OSL LPE, glossy reflection is 'G' */
  kernel_lpe_add_event(state, LPE_EVENT_GLOSSY);
}

/* Record a singular reflection bounce */
ccl_device_inline void kernel_lpe_record_singular_bounce(ccl_global IntegratorState state)
{
  /* For OSL LPE, singular/specular reflection is 'S' */
  kernel_lpe_add_event(state, LPE_EVENT_SINGULAR);
}

/* Record a straight transparent transmission */
ccl_device_inline void kernel_lpe_record_straight_bounce(ccl_global IntegratorState state)
{
  /* For OSL LPE, straight/transparent transmission is 's' */
  kernel_lpe_add_event(state, LPE_EVENT_STRAIGHT);
}

/* Record a transmission bounce (diffuse) */
ccl_device_inline void kernel_lpe_record_transmission_bounce(ccl_global IntegratorState state)
{
  /* For OSL LPE, transmission is 'T' */
  kernel_lpe_add_event(state, LPE_EVENT_TRANSMISSION);
}

/* Record a reflection event */
ccl_device_inline void kernel_lpe_record_reflection_event(ccl_global IntegratorState state)
{
  kernel_lpe_add_event(state, LPE_EVENT_REFLECTION);
}

/* Record an emission event */
ccl_device_inline void kernel_lpe_record_emission_event(ccl_global IntegratorState state,
                                                        int lightgroup)
{
  kernel_lpe_add_event(state, LPE_EVENT_EMISSION);
  INTEGRATOR_STATE_WRITE(state, path, lpe_lightgroup_id) = lightgroup;
}

/* Record a background hit */
ccl_device_inline void kernel_lpe_record_background_hit(ccl_global IntegratorState state)
{
  kernel_lpe_add_event(state, LPE_EVENT_BACKGROUND);
}

/* Record object and material IDs for LPE tag filtering */
ccl_device_inline void kernel_lpe_record_surface_interaction(ccl_global IntegratorState state,
                                                             int object_id,
                                                             int material_id)
{
  INTEGRATOR_STATE_WRITE(state, path, lpe_object_id) = object_id;
  INTEGRATOR_STATE_WRITE(state, path, lpe_material_id) = (material_id & SHADER_MASK);
}

/* Record bounce from a specific closure (stub - events already recorded in path_state_next) */
ccl_device_inline void kernel_lpe_record_bounce_from_closure(ccl_global IntegratorState state,
                                                             int closure_type)
{
  (void)state;
  (void)closure_type;
}

/* Extract path string from packed events */
ccl_device_inline void kernel_lpe_extract_path(ccl_global IntegratorState state,
                                               ccl_private char *path_str,
                                               int max_len)
{
  const uint event_count = INTEGRATOR_STATE(state, path, lpe_event_count);
  const int len = min((int)event_count, max_len - 1);

  /* Extract events from all chunks */
  for (int i = 0; i < len; i++) {
    const int chunk_idx = i / 8;
    const int bit_offset = (i % 8) * 8;
    const uint64_t chunk = kernel_lpe_get_chunk(state, chunk_idx);
    path_str[i] = (char)((chunk >> bit_offset) & 0xFF);
  }
  path_str[len] = '\0';
}

/* Check if a tag '...' in the pattern matches the current context
 * Returns tag length if matched (to skip), 0 if no tag or doesn't match
 * Tag formats:
 * - '#ID' for light groups (after L/O/B events)
 * - 'object:#ID' or 'obj:#ID' for objects (after R/T/D/G/S events)
 * - 'material:#ID' or 'mat:#ID' for materials (after any surface event)
 * - '*' for wildcard (matches any)
 * Note: Negation is handled via character sets [^'...']
 */
ccl_device_inline int kernel_lpe_check_tag(ccl_private const char *pattern,
                                           uint16_t current_lightgroup_id,
                                           int current_object_id,
                                           int current_material_id,
                                           char event_char,
                                           bool is_negated)
{
  if (pattern[0] != '\'') {
    return 0;
  }

  int pos = 1;

  /* Check for wildcard '*' */
  if (pattern[pos] == '*' && pattern[pos + 1] == '\'') {
    /* Wildcard matches any light group/object/material */
    return pos + 2;
  }

  /* Parse tag type prefix (object:, obj:, material:, mat:, lightgroup:, lgroup:, lgp:) */
  enum TagType { TAG_LIGHTGROUP, TAG_OBJECT, TAG_MATERIAL, TAG_UNKNOWN };
  TagType tag_type = TAG_UNKNOWN;

  /* Check for explicit tag type prefix */
  if (pattern[pos] == 'o' && pattern[pos + 1] == 'b' && pattern[pos + 2] == 'j' &&
      pattern[pos + 3] == ':')
  {
    tag_type = TAG_OBJECT;
    pos += 4;
  }
  else if (pattern[pos] == 'o' && pattern[pos + 1] == 'b' && pattern[pos + 2] == 'j' &&
           pattern[pos + 3] == 'e' && pattern[pos + 4] == 'c' && pattern[pos + 5] == 't' &&
           pattern[pos + 6] == ':')
  {
    tag_type = TAG_OBJECT;
    pos += 7;
  }
  else if (pattern[pos] == 'm' && pattern[pos + 1] == 'a' && pattern[pos + 2] == 't' &&
           pattern[pos + 3] == ':')
  {
    tag_type = TAG_MATERIAL;
    pos += 4;
  }
  else if (pattern[pos] == 'm' && pattern[pos + 1] == 'a' && pattern[pos + 2] == 't' &&
           pattern[pos + 3] == 'e' && pattern[pos + 4] == 'r' && pattern[pos + 5] == 'i' &&
           pattern[pos + 6] == 'a' && pattern[pos + 7] == 'l' && pattern[pos + 8] == ':')
  {
    tag_type = TAG_MATERIAL;
    pos += 9;
  }
  else if (pattern[pos] == 'l' && pattern[pos + 1] == 'g' && pattern[pos + 2] == 'p' &&
           pattern[pos + 3] == ':')
  {
    tag_type = TAG_LIGHTGROUP;
    pos += 4;
  }
  else if (pattern[pos] == 'l' && pattern[pos + 1] == 'g' && pattern[pos + 2] == 'r' &&
           pattern[pos + 3] == 'o' && pattern[pos + 4] == 'u' && pattern[pos + 5] == 'p' &&
           pattern[pos + 6] == ':')
  {
    tag_type = TAG_LIGHTGROUP;
    pos += 7;
  }
  else if (pattern[pos] == 'l' && pattern[pos + 1] == 'i' && pattern[pos + 2] == 'g' &&
           pattern[pos + 3] == 'h' && pattern[pos + 4] == 't' && pattern[pos + 5] == 'g' &&
           pattern[pos + 6] == 'r' && pattern[pos + 7] == 'o' && pattern[pos + 8] == 'u' &&
           pattern[pos + 9] == 'p' && pattern[pos + 10] == ':')
  {
    tag_type = TAG_LIGHTGROUP;
    pos += 11;
  }
  /* If no prefix and starts with #, infer type from event character */
  else if (pattern[pos] == '#') {
    /* Infer tag type based on previous event:
     * - L/O/B → light group
     * - R/T/D/G/S → object (for now, could also be material) */
    if (event_char == 'L' || event_char == 'O' || event_char == 'B') {
      tag_type = TAG_LIGHTGROUP;
    }
    else {
      tag_type = TAG_OBJECT;
    }
  }

  /* Parse numeric ID after # */
  if (pattern[pos] == '#') {
    pos++;
    int tag_id = 0;

    /* Parse integer ID */
    while (pattern[pos] >= '0' && pattern[pos] <= '9') {
      tag_id = tag_id * 10 + (pattern[pos] - '0');
      pos++;
    }

    if (pattern[pos] == '\'') {
      pos++; /* Skip closing ' */

      /* Match ID based on tag type */
      bool matches = false;
      if (tag_type == TAG_LIGHTGROUP) {
        matches = (tag_id == (int)current_lightgroup_id);
      }
      else if (tag_type == TAG_OBJECT) {
        matches = (tag_id == current_object_id);
      }
      else if (tag_type == TAG_MATERIAL) {
        matches = (tag_id == current_material_id);
      }

      if (is_negated) {
        matches = !matches;
      }

      if (matches) {
        return pos; /* Tag matched, return length to skip */
      }
      else {
        return -1; /* Tag present but doesn't match - fail entire pattern */
      }
    }
  }

  /* Skip unknown/unsupported tag format */
  while (pattern[pos] != '\0' && pattern[pos] != '\'') {
    pos++;
  }
  if (pattern[pos] == '\'') {
    pos++;
  }
  return pos; /* Skip tag */
}

/* Check if character matches a character class [ABC] or negated [^ABC] or tag [^'label']
 * Also handles tag matching within character sets for negation
 */
ccl_device_inline bool kernel_lpe_matches_char_class(char c,
                                                     ccl_private const char *pattern,
                                                     int *class_end,
                                                     uint16_t lightgroup_id,
                                                     int object_id,
                                                     int material_id,
                                                     char event_char)
{
  int i = 0;
  bool found = false;
  bool is_negated = false;
  bool has_tag = false;
  bool tag_matches = false;

  /* Check for negation */
  if (pattern[i] == '^') {
    is_negated = true;
    i++;
  }

  /* Check for tag in character set: [^'label'] or ['label'] */
  if (pattern[i] == '\'') {
    has_tag = true;
    /* Find the end of the tag */
    int tag_len = kernel_lpe_check_tag(&pattern[i], lightgroup_id, object_id, material_id, event_char, is_negated);
    if (tag_len > 0) {
      tag_matches = true;
      i += tag_len;
    }
    else if (tag_len < 0) {
      /* Tag present but doesn't match */
      tag_matches = false;
      /* Skip to end of tag */
      i++; /* Skip opening ' */
      while (pattern[i] != '\0' && pattern[i] != '\'') {
        i++;
      }
      if (pattern[i] == '\'') {
        i++;
      }
    }
  }
  else {
    /* Regular character class */
    while (pattern[i] != '\0' && pattern[i] != ']') {
      if (pattern[i] == c) {
        found = true;
      }
      i++;
    }
  }

  /* class_end points after the ']' */
  *class_end = (pattern[i] == ']') ? i + 1 : i;

  /* For tags in character sets, return tag match result (negation already handled in kernel_lpe_check_tag) */
  if (has_tag) {
    return tag_matches;
  }

  /* For regular character classes, invert result if negated */
  return is_negated ? !found : found;
}

/* Parse quantifier {n} or {n,m} and return min/max counts
 * Also handles event counting {EVENT=N} syntax
 * Returns total length of quantifier including braces
 * If no comma: min=max=n (exactly n)
 * If comma but no max: max=-1 (n or more)
 * If EVENT=N format: sets event_char and exact_count */
ccl_device_inline int kernel_lpe_parse_quantifier(ccl_private const char *pattern,
                                                  int *min_count,
                                                  int *max_count,
                                                  char *event_char,
                                                  int *exact_count)
{
  if (pattern[0] != '{') {
    return 0;
  }

  *event_char = '\0';
  *exact_count = -1;
  int pos = 1;
  *min_count = 0;
  *max_count = 0;

  /* Check for EVENT=N syntax (e.g., {D=3}) */
  if (pattern[pos] >= 'A' && pattern[pos] <= 'Z') {
    *event_char = pattern[pos];
    pos++;

    if (pattern[pos] == '=') {
      pos++;
      *exact_count = 0;
      while (pattern[pos] >= '0' && pattern[pos] <= '9') {
        *exact_count = *exact_count * 10 + (pattern[pos] - '0');
        pos++;
      }

      if (pattern[pos] == '}') {
        pos++;
        return pos;
      }
    }
    return 0; /* Invalid format */
  }

  /* Parse first number for regular quantifier */
  while (pattern[pos] >= '0' && pattern[pos] <= '9') {
    *min_count = *min_count * 10 + (pattern[pos] - '0');
    pos++;
  }

  /* Check for comma (range) */
  if (pattern[pos] == ',') {
    pos++;
    /* Check if there's a max value */
    if (pattern[pos] >= '0' && pattern[pos] <= '9') {
      while (pattern[pos] >= '0' && pattern[pos] <= '9') {
        *max_count = *max_count * 10 + (pattern[pos] - '0');
        pos++;
      }
    }
    else {
      /* No max specified = unlimited (use -1 to indicate) */
      *max_count = -1;
    }
  }
  else {
    /* No comma = exact count */
    *max_count = *min_count;
  }

  /* Expect closing brace */
  if (pattern[pos] == '}') {
    pos++;
    return pos;
  }

  /* Invalid quantifier */
  return 0;
}

/* Count occurrences of a specific event character in path */
ccl_device_inline int kernel_lpe_count_event(ccl_private const char *path, char event)
{
  int count = 0;
  for (int i = 0; path[i] != '\0'; i++) {
    if (path[i] == event) {
      count++;
    }
  }
  return count;
}

/* LPE pattern matching with operator support
 * Supports: | (OR), - (SUBTRACT), ! (NEGATE), and all basic wildcards
 * This is the main entry point for LPE matching
 */
ccl_device_inline bool kernel_lpe_matches_with_operators(ccl_private const char *path,
                                                         ccl_private const char *pattern,
                                                         uint16_t lightgroup_id,
                                                         int object_id,
                                                         int material_id);

/* Advanced LPE pattern matching with wildcard support (single pattern)
 * Supports:
 * - . : matches any single character
 * - * : zero or more of the preceding character/class
 * - + : one or more of the preceding character/class
 * - {n} : exactly n repetitions
 * - {n,m} : between n and m repetitions (inclusive)
 * - {n,} : n or more repetitions
 * - {EVENT=N} : count constraint - path must have exactly N occurrences of EVENT
 * - [ABC] or [^ABC] : character class or negated character class
 * - <#ID> or <^ID> : light group tag filtering (after L/O/B events)
 * - <object:#ID> or <obj:#ID> : object tag filtering
 * - <material:#ID> or <mat:#ID> : material tag filtering
 * - Literal characters
 */
ccl_device_inline bool kernel_lpe_matches(ccl_private const char *path,
                                          ccl_private const char *pattern,
                                          uint16_t lightgroup_id,
                                          int object_id,
                                          int material_id)
{
  int path_pos = 0;
  int pattern_pos = 0;

  while (pattern[pattern_pos] != '\0') {
    char p = pattern[pattern_pos];
    char next_p = pattern[pattern_pos + 1];

    /* Handle event counter {EVENT=N} */
    if (p == '{' && next_p >= 'A' && next_p <= 'Z') {
      int min_count, max_count;
      char event_char;
      int exact_count;
      int quant_len = kernel_lpe_parse_quantifier(
          &pattern[pattern_pos], &min_count, &max_count, &event_char, &exact_count);

      if (quant_len > 0 && exact_count >= 0) {
        /* This is an event counter {EVENT=N} */
        int actual_count = kernel_lpe_count_event(path, event_char);
        if (actual_count != exact_count) {
          return false;
        }
        pattern_pos += quant_len;
        continue;
      }
    }

    /* Handle character class [ABC] */
    if (p == '[') {
      /* Check if this is a post-event tag modifier [^'label'] or ['label'] */
      /* In that case, skip this section and let the single character handler deal with it */
      int peek = pattern_pos + 1;
      bool is_tag_modifier = false;

      if (pattern[peek] == '^' && pattern[peek + 1] == '\'') {
        is_tag_modifier = true;
      }
      else if (pattern[peek] == '\'') {
        is_tag_modifier = true;
      }

      /* If it's a tag modifier following an event we just matched, skip to single char handler */
      if (is_tag_modifier && path_pos > 0) {
        /* This will be handled by the post-event tag filter in the single character section */
        /* Do nothing here - fall through to single character matching */
      }
      else {
        /* Regular character class matching */
        int class_end = 0;
        bool matched = kernel_lpe_matches_char_class(
            path[path_pos], &pattern[pattern_pos + 1], &class_end,
            lightgroup_id, object_id, material_id, path[path_pos]);

      /* Check for quantifiers after character class */
      char quantifier = pattern[pattern_pos + 1 + class_end];

      /* Handle {n} or {n,m} quantifier */
      if (quantifier == '{') {
        int min_count, max_count;
        char event_char;
        int exact_count;
        int quant_len = kernel_lpe_parse_quantifier(&pattern[pattern_pos + 1 + class_end],
                                                    &min_count,
                                                    &max_count,
                                                    &event_char,
                                                    &exact_count);

        /* Event counter should not follow character class */
        if (exact_count >= 0) {
          return false;
        }

        if (quant_len > 0) {
          /* Match exactly min_count times (required) */
          int count = 0;
          int save_pos = path_pos;
          for (; count < min_count; count++) {
            if (path[path_pos] == '\0') {
              return false;
            }
            int tmp = 0;
            if (!kernel_lpe_matches_char_class(path[path_pos], &pattern[pattern_pos + 1], &tmp,
                                              lightgroup_id, object_id, material_id, path[path_pos])) {
              return false;
            }
            path_pos++;
          }

          pattern_pos += 1 + class_end + quant_len;

          /* If max_count == min_count, we're done with exact match */
          if (max_count == min_count) {
            continue;
          }

          /* Try matching between min_count and max_count (or unlimited if max_count == -1) */
          int max_additional = (max_count == -1) ? 100 : (max_count - min_count);
          for (int extra = 0; extra <= max_additional && path[path_pos] != '\0'; extra++) {
            if (kernel_lpe_matches(
                    &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
            {
              return true;
            }
            /* Try one more match */
            int tmp = 0;
            if (kernel_lpe_matches_char_class(path[path_pos], &pattern[save_pos + 1], &tmp,
                                             lightgroup_id, object_id, material_id, path[path_pos])) {
              path_pos++;
            }
            else {
              break;
            }
          }
          return kernel_lpe_matches(
              &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
        }
      }
      /* Handle * quantifier */
      else if (quantifier == '*') {
        /* [ABC]* - zero or more */
        const int class_start = pattern_pos + 1; /* Save position of class content */
        pattern_pos += 2 + class_end;
        while (path[path_pos] != '\0') {
          if (kernel_lpe_matches(
                  &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
          {
            return true;
          }
          int tmp = 0;
          if (kernel_lpe_matches_char_class(path[path_pos], &pattern[class_start], &tmp,
                                           lightgroup_id, object_id, material_id, path[path_pos])) {
            path_pos++;
          }
          else {
            break;
          }
        }
        return kernel_lpe_matches(
            &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
      }
      /* Handle + quantifier */
      else if (quantifier == '+') {
        /* [ABC]+ - one or more */
        if (!matched || path[path_pos] == '\0') {
          return false;
        }
        path_pos++;
        const int class_start = pattern_pos + 1; /* Save position of class content */
        pattern_pos += 2 + class_end;

        while (path[path_pos] != '\0') {
          if (kernel_lpe_matches(
                  &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
          {
            return true;
          }
          int tmp = 0;
          if (kernel_lpe_matches_char_class(path[path_pos], &pattern[class_start], &tmp,
                                           lightgroup_id, object_id, material_id, path[path_pos])) {
            path_pos++;
          }
          else {
            break;
          }
        }
        return kernel_lpe_matches(
            &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
      }
      else {
        /* [ABC] - single match */
        if (!matched || path[path_pos] == '\0') {
          return false;
        }
        path_pos++;
        pattern_pos += 1 + class_end;
      }
      }
    }
    /* Handle {n} or {n,m} after single character */
    else if (next_p == '{') {
      int min_count, max_count;
      char event_char;
      int exact_count;
      int quant_len = kernel_lpe_parse_quantifier(
          &pattern[pattern_pos + 1], &min_count, &max_count, &event_char, &exact_count);

      /* Event counter should have been handled earlier, skip if found here */
      if (exact_count >= 0) {
        return false; /* Invalid: event counter must not follow a character */
      }

      if (quant_len > 0) {
        /* Match exactly min_count times (required) */
        for (int count = 0; count < min_count; count++) {
          if (path[path_pos] == '\0' || (p != '.' && path[path_pos] != p)) {
            return false;
          }
          path_pos++;
        }

        pattern_pos += 1 + quant_len;

        /* If max_count == min_count, we're done with exact match */
        if (max_count == min_count) {
          continue;
        }

        /* Try matching between min_count and max_count (or unlimited if max_count == -1) */
        int max_additional = (max_count == -1) ? 100 : (max_count - min_count);
        for (int extra = 0; extra <= max_additional && path[path_pos] != '\0'; extra++) {
          if (kernel_lpe_matches(
                  &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
          {
            return true;
          }
          /* Try one more match */
          if (p == '.' || path[path_pos] == p) {
            path_pos++;
          }
          else {
            break;
          }
        }
        return kernel_lpe_matches(
            &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
      }
    }
    /* Handle X* - zero or more X */
    else if (next_p == '*') {
      pattern_pos += 2;

      while (path[path_pos] != '\0') {
        if (kernel_lpe_matches(
                &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
        {
          return true;
        }
        if (p == '.' || path[path_pos] == p) {
          path_pos++;
        }
        else {
          break;
        }
      }
      return kernel_lpe_matches(
          &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
    }
    /* Handle X+ - one or more X */
    else if (next_p == '+') {
      /* Must match at least once */
      if (path[path_pos] == '\0' || (p != '.' && p != path[path_pos])) {
        return false;
      }
      path_pos++;
      pattern_pos += 2;

      while (path[path_pos] != '\0') {
        if (kernel_lpe_matches(
                &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id))
        {
          return true;
        }
        if (p == '.' || path[path_pos] == p) {
          path_pos++;
        }
        else {
          break;
        }
      }
      return kernel_lpe_matches(
          &path[path_pos], &pattern[pattern_pos], lightgroup_id, object_id, material_id);
    }
    /* Match single character */
    else {
      if (path[path_pos] == '\0') {
        return false;
      }

      /* '.' matches any character, otherwise must match exactly */
      if (p != '.' && p != path[path_pos]) {
        return false;
      }

      /* Advance positions */
      path_pos++;
      pattern_pos++;

      /* Check for tag filter after any event (except C which is always first) */
      if (p != 'C' && pattern[pattern_pos] == '\'') {
        int tag_len = kernel_lpe_check_tag(
            &pattern[pattern_pos], lightgroup_id, object_id, material_id, p, false);
        if (tag_len < 0) {
          /* Tag present but doesn't match - pattern fails */
          return false;
        }
        else if (tag_len > 0) {
          /* Tag matched - skip it in pattern */
          pattern_pos += tag_len;
        }
      }
      /* Check for character set tag filter [^'label'] or ['label'] after event */
      else if (p != 'C' && pattern[pattern_pos] == '[') {
        /* Peek ahead to see if this is a tag filter */
        int peek = pattern_pos + 1;
        bool is_negated = false;

        if (pattern[peek] == '^') {
          is_negated = true;
          peek++;
        }

        /* If it's a tag (starts with '), treat it as a post-event modifier */
        if (pattern[peek] == '\'') {
          /* Skip the [ and optional ^ */
          int tag_start = peek;
          int tag_len = kernel_lpe_check_tag(
              &pattern[tag_start], lightgroup_id, object_id, material_id, p, is_negated);

          if (tag_len < 0) {
            /* Tag present but doesn't match - pattern fails */
            return false;
          }
          else if (tag_len > 0) {
            /* Tag matched - skip [, optional ^, tag, and ] */
            pattern_pos = tag_start + tag_len;
            if (pattern[pattern_pos] == ']') {
              pattern_pos++;
            }
          }
        }
      }
    }
  }

  /* Both must be at end for a match */
  return path[path_pos] == '\0';
}

/* LPE pattern matching with operator support
 * Handles: () (GROUPING), | (OR), - (SUBTRACT), ! (NEGATE) operators
 * Operators | and - require spacing: " | " and " - "
 */
ccl_device_inline bool kernel_lpe_matches_with_operators(ccl_private const char *path,
                                                         ccl_private const char *pattern,
                                                         uint16_t lightgroup_id,
                                                         int object_id,
                                                         int material_id)
{
  /* Check for leading negation operator ! */
  bool has_negation = false;
  int pos = 0;

  if (pattern[pos] == '!') {
    has_negation = true;
    pos++;
    while (pattern[pos] == ' ')
      pos++; /* Skip spaces after ! */
  }

  /* Parse and match patterns with operators */
  char sub_pattern[LPE_MAX_EXPRESSION_LENGTH];
  int sub_pos = 0;
  bool result = false;
  bool first_pattern = true;
  char last_operator = '\0';
  int paren_depth = 0;

  while (pattern[pos] != '\0') {
    /* Handle parentheses - extract and evaluate recursively */
    if (pattern[pos] == '(' && paren_depth == 0) {
      /* Find matching closing parenthesis */
      int paren_start = pos + 1;
      int depth = 1;
      pos++;

      while (pattern[pos] != '\0' && depth > 0) {
        if (pattern[pos] == '(') {
          depth++;
        }
        else if (pattern[pos] == ')') {
          depth--;
        }
        pos++;
      }

      /* Extract content between parentheses */
      int paren_len = pos - paren_start - 1;
      char paren_content[LPE_MAX_EXPRESSION_LENGTH];
      for (int i = 0; i < paren_len && i < LPE_MAX_EXPRESSION_LENGTH - 1; i++) {
        paren_content[i] = pattern[paren_start + i];
      }
      paren_content[paren_len] = '\0';

      /* Evaluate parenthesized expression recursively */
      bool paren_result = kernel_lpe_matches_with_operators(
          path, paren_content, lightgroup_id, object_id, material_id);

      /* Treat parenthesized result as a sub-pattern result */
      if (first_pattern) {
        result = paren_result;
        first_pattern = false;
      }
      else {
        if (last_operator == '|' || last_operator == '+') {
          result = result || paren_result;
        }
        else if (last_operator == '-') {
          result = result && !paren_result;
        }
      }

      /* Check if there's an operator after the closing paren */
      if (pattern[pos] == ' ' && pattern[pos + 1] != '\0' && pattern[pos + 2] == ' ') {
        char op = pattern[pos + 1];
        if (op == '|' || op == '-' || op == '+') {
          last_operator = op;
          pos += 3;
        }
      }

      continue;
    }

    /* Check for operator with spaces: " | ", " - ", or " + " (only outside parentheses) */
    if (paren_depth == 0 && pattern[pos] == ' ' && pattern[pos + 1] != '\0' &&
        pattern[pos + 2] == ' ')
    {
      char op = pattern[pos + 1];
      if (op == '|' || op == '-' || op == '+') {
        /* Terminate current sub-pattern */
        sub_pattern[sub_pos] = '\0';

        /* Match current sub-pattern only if it's not empty */
        if (sub_pos > 0) {
          bool current = kernel_lpe_matches(
              path, sub_pattern, lightgroup_id, object_id, material_id);

          /* Apply operator */
          if (first_pattern) {
            result = current;
            first_pattern = false;
          }
          else {
            if (last_operator == '|' || last_operator == '+') {
              result = result || current; /* Union */
            }
            else if (last_operator == '-') {
              result = result && !current; /* Difference */
            }
          }
        }

        /* Save operator for next iteration */
        last_operator = op;

        /* Reset for next sub-pattern */
        sub_pos = 0;
        pos += 3; /* Skip " op " */
        continue;
      }
    }

    /* Track parenthesis depth (for nested handling in future) */
    if (pattern[pos] == '(') {
      paren_depth++;
    }
    else if (pattern[pos] == ')') {
      paren_depth--;
    }

    /* Build current sub-pattern */
    if (sub_pos < LPE_MAX_EXPRESSION_LENGTH - 1) {
      sub_pattern[sub_pos++] = pattern[pos];
    }
    pos++;
  }

  /* Match final sub-pattern if not empty */
  if (sub_pos > 0) {
    sub_pattern[sub_pos] = '\0';
    bool current = kernel_lpe_matches(path, sub_pattern, lightgroup_id, object_id, material_id);

    if (first_pattern) {
      result = current;
    }
    else {
      if (last_operator == '|' || last_operator == '+') {
        result = result || current;
      }
      else if (last_operator == '-') {
        result = result && !current;
      }
    }
  }

  /* Apply negation if present */
  if (has_negation) {
    result = !result;
  }

  return result;
}

/* Decompress LPE expression from lookup table */
ccl_device_inline void kernel_lpe_decompress_expression(ccl_global const float *lpe_data,
                                                        ccl_private char *expression)
{
  for (int i = 0; i < LPE_MAX_EXPRESSION_LENGTH; i += 4) {
    uint packed = __float_as_uint(lpe_data[i / 4]);
    expression[i + 0] = (char)((packed >> 0) & 0xFF);
    expression[i + 1] = (char)((packed >> 8) & 0xFF);
    expression[i + 2] = (char)((packed >> 16) & 0xFF);
    expression[i + 3] = (char)((packed >> 24) & 0xFF);
  }
}

/* Write LPE contribution to a pass with a final event */
ccl_device_inline void kernel_lpe_write_pass(KernelGlobals kg,
                                             ccl_global IntegratorState state,
                                             ccl_global float *ccl_restrict render_buffer,
                                             Spectrum contribution,
                                             char final_event)
{
  /* Early exit if no LPE passes configured */
  if (kernel_data.film.pass_lpe == PASS_UNUSED || kernel_data.film.num_lpe_passes == 0) {
    return;
  }

  /* Save current event state to restore later */
  uint64_t saved_events[LPE_EVENT_CHUNKS];
  for (int i = 0; i < LPE_EVENT_CHUNKS; i++) {
    saved_events[i] = kernel_lpe_get_chunk(state, i);
  }
  const uint8_t saved_count = INTEGRATOR_STATE(state, path, lpe_event_count);

  /* Add final event for matching (Light, Background, or Emission) */
  kernel_lpe_add_event(state, final_event);

  /* Extract current path string with final event included */
  char path_str[LPE_MAX_EVENTS + 1];
  kernel_lpe_extract_path(state, path_str, LPE_MAX_EVENTS + 1);

  /* Get the base offset for LPE passes in the render buffer */
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  /* Get LPE expression data from lookup tables */
  const int lpe_offset = kernel_data.film.lpe_expressions_offset;

  /* Get current light group, object, and material IDs from state */
  const uint16_t lightgroup_id = INTEGRATOR_STATE(state, path, lpe_lightgroup_id);
  const int object_id = INTEGRATOR_STATE(state, path, lpe_object_id);
  const int material_id = INTEGRATOR_STATE(state, path, lpe_material_id);

  /* Current buffer offset - starts at first LPE pass and increments by 3 for each pass */
  int current_lpe_offset = kernel_data.film.pass_lpe;

  for (int pass_id = 0; pass_id < kernel_data.film.num_lpe_passes; pass_id++) {
    ccl_global const float *lpe_data = &kernel_data_fetch(
        lookup_table, lpe_offset + pass_id * LPE_MAX_EXPRESSION_LENGTH / 4);

    char expression[LPE_MAX_EXPRESSION_LENGTH];
    kernel_lpe_decompress_expression(lpe_data, expression);

    if (kernel_lpe_matches_with_operators(
            path_str, expression, lightgroup_id, object_id, material_id))
    {
      film_write_pass_spectrum(buffer + current_lpe_offset, contribution);
    }

    current_lpe_offset += 3;
  }

  /* Restore original event state */
  for (int i = 0; i < LPE_EVENT_CHUNKS; i++) {
    kernel_lpe_set_chunk(state, i, saved_events[i]);
  }
  INTEGRATOR_STATE_WRITE(state, path, lpe_event_count) = saved_count;
}

/* Write LPE contribution to a pass from shadow ray with a final event */
ccl_device_inline void kernel_lpe_write_pass(KernelGlobals kg,
                                             IntegratorShadowState state,
                                             ccl_global float *ccl_restrict render_buffer,
                                             Spectrum contribution,
                                             char final_event)
{
  if (kernel_data.film.pass_lpe == PASS_UNUSED || kernel_data.film.num_lpe_passes == 0) {
    return;
  }

  /* Save current event state to restore later */
  uint64_t saved_events[LPE_EVENT_CHUNKS];
  for (int i = 0; i < LPE_EVENT_CHUNKS; i++) {
    saved_events[i] = kernel_lpe_get_shadow_chunk(state, i);
  }
  const uint8_t saved_count = INTEGRATOR_STATE(state, shadow_path, lpe_event_count);

  /* Add final event using chunked storage */
  uint8_t event_count = INTEGRATOR_STATE(state, shadow_path, lpe_event_count);
  if (event_count < LPE_MAX_EVENTS) {
    const int chunk_idx = event_count / 8;
    const int bit_offset = (event_count % 8) * 8;
    uint64_t chunk = kernel_lpe_get_shadow_chunk(state, chunk_idx);
    chunk |= ((uint64_t)final_event << bit_offset);
    kernel_lpe_set_shadow_chunk(state, chunk_idx, chunk);
    INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_event_count) = event_count + 1;
  }

  /* Extract path string from all chunks */
  char path_str[LPE_MAX_EVENTS + 1];
  event_count = INTEGRATOR_STATE(state, shadow_path, lpe_event_count);
  int len = min((int)event_count, LPE_MAX_EVENTS);
  for (int i = 0; i < len; i++) {
    const int chunk_idx = i / 8;
    const int bit_offset = (i % 8) * 8;
    const uint64_t chunk = kernel_lpe_get_shadow_chunk(state, chunk_idx);
    path_str[i] = (char)((chunk >> bit_offset) & 0xFF);
  }
  path_str[len] = '\0';

  const uint32_t render_pixel_index = INTEGRATOR_STATE(state, shadow_path, render_pixel_index);
  const uint64_t render_buffer_offset = (uint64_t)render_pixel_index *
                                        kernel_data.film.pass_stride;
  ccl_global float *buffer = render_buffer + render_buffer_offset;

  const int lpe_offset = kernel_data.film.lpe_expressions_offset;

  /* Get current light group, object, and material IDs from shadow state */
  const uint16_t lightgroup_id = INTEGRATOR_STATE(state, shadow_path, lpe_lightgroup_id);
  const int object_id = INTEGRATOR_STATE(state, shadow_path, lpe_object_id);
  const int material_id = INTEGRATOR_STATE(state, shadow_path, lpe_material_id);

  int current_lpe_offset = kernel_data.film.pass_lpe;

  for (int pass_id = 0; pass_id < kernel_data.film.num_lpe_passes; pass_id++) {
    ccl_global const float *lpe_data = &kernel_data_fetch(
        lookup_table, lpe_offset + pass_id * LPE_MAX_EXPRESSION_LENGTH / 4);

    char expression[LPE_MAX_EXPRESSION_LENGTH];
    kernel_lpe_decompress_expression(lpe_data, expression);

    if (kernel_lpe_matches_with_operators(
            path_str, expression, lightgroup_id, object_id, material_id))
    {
      film_write_pass_spectrum(buffer + current_lpe_offset, contribution);
    }

    current_lpe_offset += 3;
  }

  /* Restore original event state */
  for (int i = 0; i < LPE_EVENT_CHUNKS; i++) {
    kernel_lpe_set_shadow_chunk(state, i, saved_events[i]);
  }
  INTEGRATOR_STATE_WRITE(state, shadow_path, lpe_event_count) = saved_count;
}

CCL_NAMESPACE_END
