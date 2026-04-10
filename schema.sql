-- Roguelike GSP Schema
-- Game ID: "rog"

-- META: world-level configuration (one row).
CREATE TABLE IF NOT EXISTS `meta` (
  `key`   TEXT PRIMARY KEY NOT NULL,
  `value` TEXT NOT NULL
);

-- PLAYERS: persistent character data.
-- Stats model matches the JS Character class.
CREATE TABLE IF NOT EXISTS `players` (
  `name`              TEXT PRIMARY KEY NOT NULL,
  `level`             INTEGER NOT NULL DEFAULT 1,
  `xp`                INTEGER NOT NULL DEFAULT 0,
  `gold`              INTEGER NOT NULL DEFAULT 0,
  `strength`          INTEGER NOT NULL DEFAULT 10,
  `dexterity`         INTEGER NOT NULL DEFAULT 10,
  `constitution`      INTEGER NOT NULL DEFAULT 10,
  `intelligence`      INTEGER NOT NULL DEFAULT 10,
  `skill_points`      INTEGER NOT NULL DEFAULT 0,
  `stat_points`       INTEGER NOT NULL DEFAULT 0,
  `registered_height` INTEGER NOT NULL,
  `kills`             INTEGER NOT NULL DEFAULT 0,
  `deaths`            INTEGER NOT NULL DEFAULT 0,
  `visits_completed`  INTEGER NOT NULL DEFAULT 0,
  `hp`                   INTEGER NOT NULL DEFAULT 100,
  `max_hp`               INTEGER NOT NULL DEFAULT 100,
  `current_segment`      INTEGER NOT NULL DEFAULT 0,
  `in_channel`           INTEGER NOT NULL DEFAULT 0,
  `last_discover_height` INTEGER NOT NULL DEFAULT 0
);

-- INVENTORY: persistent item storage.
-- slot values: bag, weapon, offhand, head, body, feet, ring, amulet
CREATE TABLE IF NOT EXISTS `inventory` (
  `rowid`     INTEGER PRIMARY KEY AUTOINCREMENT,
  `name`      TEXT NOT NULL,
  `item_id`   TEXT NOT NULL,
  `quantity`  INTEGER NOT NULL DEFAULT 1,
  `slot`      TEXT NOT NULL DEFAULT 'bag',
  `item_data` TEXT NULL
);

CREATE INDEX IF NOT EXISTS `inventory_by_player`
    ON `inventory` (`name`);

-- KNOWN SPELLS: permanently unlocked spells.
CREATE TABLE IF NOT EXISTS `known_spells` (
  `name`     TEXT NOT NULL,
  `spell_id` TEXT NOT NULL,
  PRIMARY KEY (`name`, `spell_id`)
);

-- SEGMENTS: permanent map locations.  A segment persists forever once
-- discovered; its dungeon layout is derived deterministically from seed+depth.
CREATE TABLE IF NOT EXISTS `segments` (
  `id`             INTEGER PRIMARY KEY NOT NULL,
  `discoverer`     TEXT NOT NULL,
  `seed`           TEXT NOT NULL,
  `depth`          INTEGER NOT NULL,
  `max_players`    INTEGER NOT NULL DEFAULT 4,
  `created_height` INTEGER NOT NULL,
  `confirmed`      INTEGER NOT NULL DEFAULT 0,
  `world_x`        INTEGER NOT NULL DEFAULT 0,
  `world_y`        INTEGER NOT NULL DEFAULT 0
);

CREATE UNIQUE INDEX IF NOT EXISTS `segments_by_world_pos`
    ON `segments` (`world_x`, `world_y`);

-- VISITS: temporary expeditions into segments.
-- Each visit has a lifecycle: open -> active -> completed/expired.
CREATE TABLE IF NOT EXISTS `visits` (
  `id`             INTEGER PRIMARY KEY NOT NULL,
  `segment_id`     INTEGER NOT NULL,
  `initiator`      TEXT NOT NULL,
  `status`         TEXT NOT NULL DEFAULT 'open',
  `created_height` INTEGER NOT NULL,
  `started_height` INTEGER NULL,
  `settled_height` INTEGER NULL
);

CREATE INDEX IF NOT EXISTS `visits_by_status`
    ON `visits` (`status`);

CREATE INDEX IF NOT EXISTS `visits_by_segment`
    ON `visits` (`segment_id`);

-- VISIT PARTICIPANTS
CREATE TABLE IF NOT EXISTS `visit_participants` (
  `visit_id`      INTEGER NOT NULL,
  `name`          TEXT NOT NULL,
  `joined_height` INTEGER NOT NULL,
  PRIMARY KEY (`visit_id`, `name`)
);

-- VISIT RESULTS: per-player outcomes from completed visits.
CREATE TABLE IF NOT EXISTS `visit_results` (
  `visit_id`    INTEGER NOT NULL,
  `name`        TEXT NOT NULL,
  `survived`      INTEGER NOT NULL DEFAULT 0,
  `xp_gained`     INTEGER NOT NULL DEFAULT 0,
  `gold_gained`   INTEGER NOT NULL DEFAULT 0,
  `kills`         INTEGER NOT NULL DEFAULT 0,
  `hp_remaining`  INTEGER NOT NULL DEFAULT 0,
  `exit_gate`     TEXT NULL,
  PRIMARY KEY (`visit_id`, `name`)
);

-- LOOT CLAIMS: items awarded from completed visits.
CREATE TABLE IF NOT EXISTS `loot_claims` (
  `visit_id`  INTEGER NOT NULL,
  `name`      TEXT NOT NULL,
  `item_id`   TEXT NOT NULL,
  `quantity`  INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS `loot_claims_by_visit`
    ON `loot_claims` (`visit_id`);

-- SEGMENT GATES: cached gate positions for each segment.
CREATE TABLE IF NOT EXISTS `segment_gates` (
  `segment_id` INTEGER NOT NULL,
  `direction`  TEXT NOT NULL,
  `x`          INTEGER NOT NULL,
  `y`          INTEGER NOT NULL,
  PRIMARY KEY (`segment_id`, `direction`)
);

-- SEGMENT LINKS: overworld graph connecting segments via gates.
-- Bidirectional: each connection has two rows.
CREATE TABLE IF NOT EXISTS `segment_links` (
  `from_segment`   INTEGER NOT NULL,
  `from_direction` TEXT NOT NULL,
  `to_segment`     INTEGER NOT NULL,
  `to_direction`   TEXT NOT NULL,
  PRIMARY KEY (`from_segment`, `from_direction`)
);

CREATE INDEX IF NOT EXISTS `segment_links_to`
    ON `segment_links` (`to_segment`, `to_direction`);
