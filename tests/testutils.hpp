#ifndef ROG_TESTUTILS_HPP
#define ROG_TESTUTILS_HPP

#include <gtest/gtest.h>

#include <json/json.h>
#include <sqlite3.h>

#include <string>

namespace rog
{

/**
 * Parses JSON from a string.
 */
Json::Value ParseJson (const std::string& val);

/**
 * Test fixture with a temporary, in-memory SQLite database and our
 * database schema applied.
 */
class DBTest : public testing::Test
{

private:

  sqlite3* handle;

protected:

  DBTest ();
  ~DBTest () override;

  /**
   * Returns the raw sqlite3 handle.
   */
  sqlite3*
  GetHandle ()
  {
    return handle;
  }

  /**
   * Executes a SQL statement directly.
   */
  void Execute (const std::string& sql);

  /**
   * Inserts a player into the database for testing purposes.
   */
  void InsertPlayer (const std::string& name, unsigned height);

  /**
   * Inserts an inventory item for testing purposes.
   */
  void InsertItem (const std::string& name, const std::string& itemId,
                   int quantity, const std::string& slot);

  /**
   * Helper to query a single integer value from the database.
   */
  int64_t QueryInt (const std::string& sql);

  /**
   * Helper to query a single string value from the database.
   */
  std::string QueryString (const std::string& sql);

};

} // namespace rog

#endif // ROG_TESTUTILS_HPP
