#include "testutils.hpp"

#include <glog/logging.h>

#include <sstream>

extern "C" const char* GetSchemaSQL ();

namespace rog
{

Json::Value
ParseJson (const std::string& val)
{
  std::istringstream in(val);
  Json::Value res;
  in >> res;
  return res;
}

DBTest::DBTest ()
{
  const int rc = sqlite3_open (":memory:", &handle);
  CHECK_EQ (rc, SQLITE_OK) << "Failed to open in-memory database";

  /* Load the schema SQL from the embedded string.  */
  Execute (GetSchemaSQL ());
}

DBTest::~DBTest ()
{
  if (handle != nullptr)
    sqlite3_close (handle);
}

void
DBTest::Execute (const std::string& sql)
{
  char* err = nullptr;
  const int rc = sqlite3_exec (handle, sql.c_str (), nullptr, nullptr, &err);
  if (rc != SQLITE_OK)
    {
      std::string msg = err ? err : "unknown error";
      sqlite3_free (err);
      FAIL () << "SQL execution failed: " << msg << "\nSQL: " << sql;
    }
}

void
DBTest::InsertPlayer (const std::string& name, const unsigned height)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (handle,
    "INSERT INTO `players` (`name`, `registered_height`) VALUES (?1, ?2)",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, height);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

void
DBTest::InsertItem (const std::string& name, const std::string& itemId,
                    const int quantity, const std::string& slot)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (handle,
    "INSERT INTO `inventory` (`name`, `item_id`, `quantity`, `slot`)"
    " VALUES (?1, ?2, ?3, ?4)",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, quantity);
  sqlite3_bind_text (stmt, 4, slot.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

int64_t
DBTest::QueryInt (const std::string& sql)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (handle, sql.c_str (), -1, &stmt, nullptr);
  CHECK_EQ (sqlite3_step (stmt), SQLITE_ROW) << "No row returned for: " << sql;
  const int64_t val = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return val;
}

std::string
DBTest::QueryString (const std::string& sql)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (handle, sql.c_str (), -1, &stmt, nullptr);
  CHECK_EQ (sqlite3_step (stmt), SQLITE_ROW) << "No row returned for: " << sql;
  const char* text = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 0));
  std::string val = text ? text : "";
  sqlite3_finalize (stmt);
  return val;
}

} // namespace rog
