/* SPDX-FileCopyrightText: 2009 by Janne Karhu. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_listBase.h"

typedef enum eBoidRuleType {
  eBoidRuleType_None = 0,
  /** go to goal assigned object or loudest assigned signal source */
  eBoidRuleType_Goal = 1,
  /** get away from assigned object or loudest assigned signal source */
  eBoidRuleType_Avoid = 2,
  /** Maneuver to avoid collisions with other boids and deflector object in near future. */
  eBoidRuleType_AvoidCollision = 3,
  /** keep from going through other boids */
  eBoidRuleType_Separate = 4,
  /** move to center of neighbors and match their velocity */
  eBoidRuleType_Flock = 5,
  /** follow a boid or assigned object */
  eBoidRuleType_FollowLeader = 6,
  /** Maintain speed, flight level or wander. */
  eBoidRuleType_AverageSpeed = 7,
  /** go to closest enemy and attack when in range */
  eBoidRuleType_Fight = 8,
#if 0
  /** go to enemy closest to target and attack when in range */
  eBoidRuleType_Protect = 9,
  /** find a deflector move to its other side from closest enemy */
  eBoidRuleType_Hide = 10,
  /** move along a assigned curve or closest curve in a group */
  eBoidRuleType_FollowPath = 11,
  /** move next to a deflector object's in direction of its tangent */
  eBoidRuleType_FollowWall = 12,
#endif
} eBoidRuleType;

/* boidrule->flag */
enum {
  BOIDRULE_CURRENT = 1 << 0,
  BOIDRULE_IN_AIR = 1 << 2,
  BOIDRULE_ON_LAND = 1 << 3,
};
typedef struct BoidRule {
  struct BoidRule *next = nullptr, *prev = nullptr;
  int type = 0, flag = 0;
  char name[32] = "";
} BoidRule;
enum {
  BRULE_GOAL_AVOID_PREDICT = 1 << 0,
  BRULE_GOAL_AVOID_ARRIVE = 1 << 1,
  BRULE_GOAL_AVOID_SIGNAL = 1 << 2,
};
typedef struct BoidRuleGoalAvoid {
  BoidRule rule;
  struct Object *ob = nullptr;
  int options = 0;
  float fear_factor = 0;

  /* signals */
  int signal_id = 0, channels = 0;
} BoidRuleGoalAvoid;
enum {
  BRULE_ACOLL_WITH_BOIDS = 1 << 0,
  BRULE_ACOLL_WITH_DEFLECTORS = 1 << 1,
};
typedef struct BoidRuleAvoidCollision {
  BoidRule rule;
  int options = 0;
  float look_ahead = 0;
} BoidRuleAvoidCollision;
#define BRULE_LEADER_IN_LINE (1 << 0)
typedef struct BoidRuleFollowLeader {
  BoidRule rule;
  struct Object *ob = nullptr;
  float loc[3] = {}, oloc[3] = {};
  float cfra = 0, distance = 0;
  int options = 0, queue_size = 0;
} BoidRuleFollowLeader;
typedef struct BoidRuleAverageSpeed {
  BoidRule rule;
  float wander = 0, level = 0, speed = 0;
  char _pad0[4] = {};
} BoidRuleAverageSpeed;
typedef struct BoidRuleFight {
  BoidRule rule;
  float distance = 0, flee_distance = 0;
} BoidRuleFight;

typedef enum eBoidMode {
  eBoidMode_InAir = 0,
  eBoidMode_OnLand = 1,
  eBoidMode_Climbing = 2,
  eBoidMode_Falling = 3,
  eBoidMode_Liftoff = 4,
} eBoidMode;

typedef struct BoidData {
  float health = 0, acc[3] = {};
  short state_id = 0, mode = 0;
} BoidData;

/* Planned for near future. */
// typedef enum BoidConditionMode {
//  eBoidConditionType_Then = 0,
//  eBoidConditionType_And = 1,
//  eBoidConditionType_Or = 2,
//  NUM_BOID_CONDITION_MODES
//} BoidConditionMode;
// typedef enum BoidConditionType {
//  eBoidConditionType_None = 0,
//  eBoidConditionType_Signal = 1,
//  eBoidConditionType_NoSignal = 2,
//  eBoidConditionType_HealthBelow = 3,
//  eBoidConditionType_HealthAbove = 4,
//  eBoidConditionType_See = 5,
//  eBoidConditionType_NotSee = 6,
//  eBoidConditionType_StateTime = 7,
//  eBoidConditionType_Touching = 8,
//  NUM_BOID_CONDITION_TYPES
//} BoidConditionType = 0;
// typedef struct BoidCondition {
//  struct BoidCondition *next = nullptr, *prev = nullptr;
//  int state_id = 0;
//  short type = 0, mode = 0;
//  float threshold = 0, probability = 0;
//
//  /* signals */
//  int signal_id = 0, channels = 0;
//} BoidCondition = 0;

typedef enum eBoidRulesetType {
  eBoidRulesetType_Fuzzy = 0,
  eBoidRulesetType_Random = 1,
  eBoidRulesetType_Average = 2,
} eBoidRulesetType;
#define BOIDSTATE_CURRENT 1
typedef struct BoidState {
  struct BoidState *next = nullptr, *prev = nullptr;
  ListBase rules = {nullptr, nullptr};
  ListBase conditions = {nullptr, nullptr};
  ListBase actions = {nullptr, nullptr};
  char name[32] = "";
  int id = 0, flag = 0;

  /* rules */
  int ruleset_type = 0;
  float rule_fuzziness = 0;

  /* signal */
  int signal_id = 0, channels = 0;
  float volume = 0, falloff = 0;
} BoidState;

/* Planned for near future. */
// typedef struct BoidSignal {
//  struct BoidSignal *next = nullptr, *prev = nullptr;
//  float loc[3] = {};
//  float volume = 0, falloff = 0;
//  int id = 0;
//} BoidSignal = 0;
// typedef struct BoidSignalDefine {
//  struct BoidSignalDefine *next = nullptr, *prev = nullptr;
//  int id = 0, _pad[4] = {};
//  char name[32] = "";
//} BoidSignalDefine = 0;

// typedef struct BoidSimulationData {
//  ListBase signal_defines = {nullptr, nullptr};/* list of defined signals */
//  ListBase signals[20] = {};   /* gathers signals from all channels */
//  struct KDTree_3d *signaltrees[20] = {};
//  char channel_names[20][32] = {};
//  int last_signal_id = 0;     /* used for incrementing signal ids */
//  int flag = 0;               /* switches for drawing stuff */
//} BoidSimulationData = 0;

typedef struct BoidSettings {
  int options = 0, last_state_id = 0;

  float landing_smoothness = 0, height = 0;
  float banking = 0, pitch = 0;

  float health = 0, aggression = 0;
  float strength = 0, accuracy = 0, range = 0;

  /* flying related */
  float air_min_speed = 0, air_max_speed = 0;
  float air_max_acc = 0, air_max_ave = 0;
  float air_personal_space = 0;

  /* walk/run related */
  float land_jump_speed = 0, land_max_speed = 0;
  float land_max_acc = 0, land_max_ave = 0;
  float land_personal_space = 0;
  float land_stick_force = 0;

  struct ListBase states = {nullptr, nullptr};
} BoidSettings;

/** #BoidSettings::options */
enum {
  BOID_ALLOW_FLIGHT = 1 << 0,
  BOID_ALLOW_LAND = 1 << 1,
  BOID_ALLOW_CLIMB = 1 << 2,
};

/* boidrule->options */
// #define BOID_RULE_FOLLOW_LINE     (1 << 0)        /* follow leader */
// #define BOID_RULE_PREDICT         (1 << 1)        /* goal/avoid */
// #define BOID_RULE_ARRIVAL         (1 << 2)        /* goal */
// #define BOID_RULE_LAND            (1 << 3)        /* goal */
// #define BOID_RULE_WITH_BOIDS      (1 << 4)        /* avoid collision */
// #define BOID_RULE_WITH_DEFLECTORS (1 << 5)    /* avoid collision */
