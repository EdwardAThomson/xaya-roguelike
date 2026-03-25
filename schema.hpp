#ifndef ROG_SCHEMA_HPP
#define ROG_SCHEMA_HPP

#include <xayagame/sqlitestorage.hpp>

namespace rog
{

/**
 * Sets up the database schema (if not already present) on the given
 * SQLite connection.
 */
void SetupDatabaseSchema (xaya::SQLiteDatabase& db);

} // namespace rog

#endif // ROG_SCHEMA_HPP
