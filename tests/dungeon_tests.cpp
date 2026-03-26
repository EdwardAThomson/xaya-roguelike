#include "dungeon.hpp"

#include <gtest/gtest.h>

#include <queue>
#include <set>
#include <string>

namespace rog
{
namespace
{

class DungeonTests : public testing::Test
{

protected:

  /**
   * Counts reachable non-wall tiles from a starting floor tile using
   * flood fill.  Gates are treated as passable.
   */
  int FloodFillCount (const Dungeon& d)
  {
    /* Find the first floor tile.  */
    int startX = -1, startY = -1;
    for (int y = 0; y < Dungeon::HEIGHT && startX < 0; y++)
      for (int x = 0; x < Dungeon::WIDTH && startX < 0; x++)
        if (d.GetTile (x, y) == Tile::Floor)
          {
            startX = x;
            startY = y;
          }

    if (startX < 0)
      return 0;

    std::set<std::pair<int, int>> visited;
    std::queue<std::pair<int, int>> queue;
    queue.push ({startX, startY});
    visited.insert ({startX, startY});

    static const int dx[] = {0, 0, -1, 1};
    static const int dy[] = {-1, 1, 0, 0};

    while (!queue.empty ())
      {
        auto [cx, cy] = queue.front ();
        queue.pop ();

        for (int i = 0; i < 4; i++)
          {
            const int nx = cx + dx[i];
            const int ny = cy + dy[i];

            if (nx < 0 || nx >= Dungeon::WIDTH
                || ny < 0 || ny >= Dungeon::HEIGHT)
              continue;

            const auto tile = d.GetTile (nx, ny);
            if (tile == Tile::Wall)
              continue;

            if (visited.count ({nx, ny}))
              continue;

            visited.insert ({nx, ny});
            queue.push ({nx, ny});
          }
      }

    return static_cast<int> (visited.size ());
  }

};

// ============================================================
// Determinism
// ============================================================

TEST_F (DungeonTests, SameSeedSameResult)
{
  auto d1 = Dungeon::Generate ("test_seed_abc", 1);
  auto d2 = Dungeon::Generate ("test_seed_abc", 1);

  /* Every tile must be identical.  */
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      EXPECT_EQ (d1.GetTile (x, y), d2.GetTile (x, y))
          << "Mismatch at (" << x << ", " << y << ")";

  /* Same rooms.  */
  ASSERT_EQ (d1.GetRooms ().size (), d2.GetRooms ().size ());
  for (size_t i = 0; i < d1.GetRooms ().size (); i++)
    {
      EXPECT_EQ (d1.GetRooms ()[i].x, d2.GetRooms ()[i].x);
      EXPECT_EQ (d1.GetRooms ()[i].y, d2.GetRooms ()[i].y);
      EXPECT_EQ (d1.GetRooms ()[i].width, d2.GetRooms ()[i].width);
      EXPECT_EQ (d1.GetRooms ()[i].height, d2.GetRooms ()[i].height);
    }

  /* Same gates.  */
  ASSERT_EQ (d1.GetGates ().size (), d2.GetGates ().size ());
  for (size_t i = 0; i < d1.GetGates ().size (); i++)
    {
      EXPECT_EQ (d1.GetGates ()[i].x, d2.GetGates ()[i].x);
      EXPECT_EQ (d1.GetGates ()[i].y, d2.GetGates ()[i].y);
      EXPECT_EQ (d1.GetGates ()[i].direction, d2.GetGates ()[i].direction);
    }
}

TEST_F (DungeonTests, DifferentSeedsDifferentResult)
{
  auto d1 = Dungeon::Generate ("seed_alpha", 1);
  auto d2 = Dungeon::Generate ("seed_beta", 1);

  /* At least some tiles should differ.  */
  int diffs = 0;
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      if (d1.GetTile (x, y) != d2.GetTile (x, y))
        diffs++;

  EXPECT_GT (diffs, 0);
}

TEST_F (DungeonTests, DifferentDepthDifferentResult)
{
  auto d1 = Dungeon::Generate ("same_seed", 1);
  auto d2 = Dungeon::Generate ("same_seed", 5);

  int diffs = 0;
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      if (d1.GetTile (x, y) != d2.GetTile (x, y))
        diffs++;

  EXPECT_GT (diffs, 0);
}

// ============================================================
// Room properties
// ============================================================

TEST_F (DungeonTests, RoomCountInRange)
{
  /* Test across several seeds.  */
  for (int i = 0; i < 20; i++)
    {
      auto d = Dungeon::Generate ("room_count_" + std::to_string (i), 1);
      const auto& rooms = d.GetRooms ();
      EXPECT_GE (static_cast<int> (rooms.size ()), 1)
          << "Seed index " << i;
      EXPECT_LE (static_cast<int> (rooms.size ()), Dungeon::MAX_ROOMS)
          << "Seed index " << i;
    }
}

TEST_F (DungeonTests, RoomBoundsValid)
{
  auto d = Dungeon::Generate ("bounds_test", 3);
  for (const auto& room : d.GetRooms ())
    {
      EXPECT_GE (room.x, 1);
      EXPECT_GE (room.y, 1);
      EXPECT_LT (room.x + room.width, Dungeon::WIDTH - 1);
      EXPECT_LT (room.y + room.height, Dungeon::HEIGHT - 1);
      EXPECT_GE (room.width, Dungeon::MIN_ROOM_WIDTH);
      EXPECT_LE (room.width, Dungeon::MAX_ROOM_WIDTH);
      EXPECT_GE (room.height, Dungeon::MIN_ROOM_HEIGHT);
      EXPECT_LE (room.height, Dungeon::MAX_ROOM_HEIGHT);
    }
}

TEST_F (DungeonTests, RoomsDoNotOverlap)
{
  auto d = Dungeon::Generate ("overlap_test", 2);
  const auto& rooms = d.GetRooms ();
  for (size_t i = 0; i < rooms.size (); i++)
    for (size_t j = i + 1; j < rooms.size (); j++)
      {
        /* Check 1-tile buffer between rooms.  */
        const bool overlapX = rooms[i].x <= rooms[j].x + rooms[j].width
            && rooms[i].x + rooms[i].width >= rooms[j].x;
        const bool overlapY = rooms[i].y <= rooms[j].y + rooms[j].height
            && rooms[i].y + rooms[i].height >= rooms[j].y;
        EXPECT_FALSE (overlapX && overlapY)
            << "Rooms " << i << " and " << j << " overlap";
      }
}

// ============================================================
// Gates
// ============================================================

TEST_F (DungeonTests, FourGates)
{
  auto d = Dungeon::Generate ("gate_test", 1);
  ASSERT_EQ (d.GetGates ().size (), 4u);

  bool hasNorth = false, hasSouth = false, hasEast = false, hasWest = false;
  for (const auto& g : d.GetGates ())
    {
      if (g.direction == "north")
        {
          EXPECT_EQ (g.y, 0);
          EXPECT_GE (g.x, Dungeon::GATE_MARGIN);
          EXPECT_LE (g.x, Dungeon::WIDTH - Dungeon::GATE_MARGIN - 1);
          hasNorth = true;
        }
      else if (g.direction == "south")
        {
          EXPECT_EQ (g.y, Dungeon::HEIGHT - 1);
          hasSouth = true;
        }
      else if (g.direction == "west")
        {
          EXPECT_EQ (g.x, 0);
          EXPECT_GE (g.y, Dungeon::GATE_MARGIN);
          EXPECT_LE (g.y, Dungeon::HEIGHT - Dungeon::GATE_MARGIN - 1);
          hasWest = true;
        }
      else if (g.direction == "east")
        {
          EXPECT_EQ (g.x, Dungeon::WIDTH - 1);
          hasEast = true;
        }

      /* Gate tile should be marked as Gate in the grid.  */
      EXPECT_EQ (d.GetTile (g.x, g.y), Tile::Gate);
    }

  EXPECT_TRUE (hasNorth);
  EXPECT_TRUE (hasSouth);
  EXPECT_TRUE (hasEast);
  EXPECT_TRUE (hasWest);
}

// ============================================================
// Connectivity
// ============================================================

TEST_F (DungeonTests, AllFloorsReachable)
{
  /* Test across several seeds.  */
  for (int i = 0; i < 10; i++)
    {
      auto d = Dungeon::Generate ("connectivity_" + std::to_string (i), 1);

      const int floorCount = d.CountTiles (Tile::Floor);
      const int gateCount = d.CountTiles (Tile::Gate);
      const int reachable = FloodFillCount (d);

      /* All floor + gate tiles should be reachable from any floor tile.  */
      EXPECT_EQ (reachable, floorCount + gateCount)
          << "Seed index " << i << ": not all tiles reachable";
    }
}

// ============================================================
// Tile counts
// ============================================================

TEST_F (DungeonTests, ReasonableFloorCount)
{
  auto d = Dungeon::Generate ("floor_count", 1);
  const int floors = d.CountTiles (Tile::Floor);
  const int total = Dungeon::WIDTH * Dungeon::HEIGHT;

  /* Floors should be a meaningful fraction but not the whole map.  */
  EXPECT_GT (floors, total / 10);
  EXPECT_LT (floors, total * 3 / 4);
}

TEST_F (DungeonTests, ExactlyFourGateTiles)
{
  auto d = Dungeon::Generate ("gate_tiles", 1);
  EXPECT_EQ (d.CountTiles (Tile::Gate), 4);
}

// ============================================================
// Random floor position
// ============================================================

TEST_F (DungeonTests, RandomFloorPositionIsFloor)
{
  auto d = Dungeon::Generate ("floor_pos", 1);
  std::mt19937 rng (42);

  for (int i = 0; i < 50; i++)
    {
      auto [x, y] = d.GetRandomFloorPosition (rng);
      EXPECT_GE (x, 0);
      EXPECT_GE (y, 0);
      EXPECT_EQ (d.GetTile (x, y), Tile::Floor);
    }
}

// ============================================================
// Depth stored
// ============================================================

TEST_F (DungeonTests, DepthStored)
{
  auto d = Dungeon::Generate ("depth_test", 7);
  EXPECT_EQ (d.GetDepth (), 7);
}

} // anonymous namespace
} // namespace rog
