#include "logic.hpp"

#include "channelboard.hpp"
#include "moveprocessor.hpp"
#include "schema.hpp"

#include <glog/logging.h>

#include <string>

namespace rog
{

namespace
{

/** Singleton board rules instance.  */
DungeonBoardRules boardRules;

} // anonymous namespace

void
RoguelikeLogic::SetupSchema (xaya::SQLiteDatabase& db)
{
  SetupDatabaseSchema (db);
  SetupGameChannelsSchema (db);
}

void
RoguelikeLogic::SetGenesisBlock (const unsigned height,
                                  const std::string& hashHex)
{
  genesisHeight = height;
  genesisHash = hashHex;
}

void
RoguelikeLogic::SetDungeonId (const std::string& id)
{
  dungeonId = id;
}

void
RoguelikeLogic::GetInitialStateBlock (unsigned& height,
                                       std::string& hashHex) const
{
  height = genesisHeight;
  hashHex = genesisHash;
}

void
RoguelikeLogic::InitialiseState (xaya::SQLiteDatabase& db)
{
  if (!dungeonId.empty ())
    {
      db.AccessDatabase ([&] (sqlite3* handle)
        {
          sqlite3_stmt* stmt;
          sqlite3_prepare_v2 (handle,
            "INSERT OR REPLACE INTO `meta` (`key`, `value`)"
            " VALUES ('dungeon_id', ?1)",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, dungeonId.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        });
    }
}

void
RoguelikeLogic::UpdateState (xaya::SQLiteDatabase& db,
                              const Json::Value& blockData)
{
  const unsigned height = blockData["block"]["height"].asUInt ();

  db.AccessDatabase ([&] (sqlite3* handle)
    {
      /* Visit ids come from a persisted high-water mark, never from
         MAX(id) over the live table: pruning a segment deletes its visits,
         and re-deriving the counter from what is left would hand the next
         run an id that used to mean a different run.  Segments need no
         counter at all -- a segment is its coordinate.  */
      int64_t nextVisId = 1;

      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (handle,
        "SELECT CAST(`value` AS INTEGER) FROM `meta`"
        " WHERE `key` = 'next_visit_id'",
        -1, &stmt, nullptr);
      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          /* Read it as an integer through SQLite rather than parsing the
             text ourselves: no throw, no locale, and a malformed value
             reads as 0, which the floor below turns into a safe restart
             rather than a crash mid-block.  */
          const int64_t stored = sqlite3_column_int64 (stmt, 0);
          if (stored > nextVisId)
            nextVisId = stored;
        }
      sqlite3_finalize (stmt);

      MoveProcessor proc (handle, height, nextVisId);
      proc.ProcessAll (blockData["moves"]);

      /* Persist the counter for the next block.  */
      sqlite3_prepare_v2 (handle,
        "INSERT INTO `meta` (`key`, `value`) VALUES ('next_visit_id', ?1)"
        " ON CONFLICT(`key`) DO UPDATE SET `value` = ?1",
        -1, &stmt, nullptr);
      const std::string nextVisStr = std::to_string (nextVisId);
      sqlite3_bind_text (stmt, 1, nextVisStr.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    });
}

Json::Value
RoguelikeLogic::GetStateAsJson (const xaya::SQLiteDatabase& db)
{
  return db.ReadDatabase ([&] (sqlite3* handle)
    {
      return StateJsonExtractor (handle).FullState ();
    });
}

const xaya::BoardRules&
RoguelikeLogic::GetBoardRules () const
{
  return boardRules;
}

Json::Value
RoguelikeLogic::GetCustomStateData (xaya::Game& game, const StateCallback& cb)
{
  return SQLiteGame::GetCustomStateData (game, "data",
      [this, &cb] (const xaya::SQLiteDatabase& db)
        {
          return db.ReadDatabase ([&] (sqlite3* handle)
            {
              const StateJsonExtractor ext (handle);
              return cb (ext);
            });
        });
}

} // namespace rog
