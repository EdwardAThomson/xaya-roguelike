#include "logic.hpp"

#include "moveprocessor.hpp"
#include "schema.hpp"

#include <glog/logging.h>

namespace rog
{

void
RoguelikeLogic::SetupSchema (xaya::SQLiteDatabase& db)
{
  SetupDatabaseSchema (db);
}

void
RoguelikeLogic::SetGenesisBlock (const unsigned height,
                                  const std::string& hashHex)
{
  genesisHeight = height;
  genesisHash = hashHex;
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
  /* The initial state is an empty database with no players or segments.  */
}

void
RoguelikeLogic::UpdateState (xaya::SQLiteDatabase& db,
                              const Json::Value& blockData)
{
  const unsigned height = blockData["block"]["height"].asUInt ();

  db.AccessDatabase ([&] (sqlite3* handle)
    {
      int64_t nextId = 1;

      /* Load next segment ID from existing data.  */
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (handle,
        "SELECT COALESCE(MAX(`id`), 0) + 1 FROM `segments`",
        -1, &stmt, nullptr);
      sqlite3_step (stmt);
      nextId = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      MoveProcessor proc (handle, height, nextId);
      proc.ProcessAll (blockData["moves"]);
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
