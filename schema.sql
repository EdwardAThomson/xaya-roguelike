-- Roguelike GSP Schema
-- Game ID: "rog"

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
  `segments_completed` INTEGER NOT NULL DEFAULT 0
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

-- SEGMENTS: dungeon runs (will map to game channels in Layer 2).
CREATE TABLE IF NOT EXISTS `segments` (
  `id`             INTEGER PRIMARY KEY NOT NULL,
  `discoverer`     TEXT NOT NULL,
  `seed`           TEXT NOT NULL,
  `depth`          INTEGER NOT NULL,
  `max_players`    INTEGER NOT NULL DEFAULT 4,
  `status`         TEXT NOT NULL DEFAULT 'open',
  `created_height` INTEGER NOT NULL,
  `started_height` INTEGER NULL,
  `settled_height` INTEGER NULL
);

CREATE INDEX IF NOT EXISTS `segments_by_status`
    ON `segments` (`status`);

-- SEGMENT PARTICIPANTS
CREATE TABLE IF NOT EXISTS `segment_participants` (
  `segment_id`    INTEGER NOT NULL,
  `name`          TEXT NOT NULL,
  `joined_height` INTEGER NOT NULL,
  PRIMARY KEY (`segment_id`, `name`)
);

-- SEGMENT RESULTS: per-player outcomes from completed segments.
CREATE TABLE IF NOT EXISTS `segment_results` (
  `segment_id` INTEGER NOT NULL,
  `name`       TEXT NOT NULL,
  `survived`   INTEGER NOT NULL DEFAULT 0,
  `xp_gained`  INTEGER NOT NULL DEFAULT 0,
  `gold_gained` INTEGER NOT NULL DEFAULT 0,
  `kills`      INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (`segment_id`, `name`)
);

-- LOOT CLAIMS: items awarded from completed segments.
CREATE TABLE IF NOT EXISTS `loot_claims` (
  `segment_id` INTEGER NOT NULL,
  `name`       TEXT NOT NULL,
  `item_id`    TEXT NOT NULL,
  `quantity`   INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX IF NOT EXISTS `loot_claims_by_segment`
    ON `loot_claims` (`segment_id`);
