#include "dungeon.hpp"
#include "hash.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <queue>
#include <set>
#include <string>
#include <vector>

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
// Constrained gates
// ============================================================

TEST_F (DungeonTests, ConstrainedGatePosition)
{
  /* Force the north gate to x=30 and south gate to x=50.  */
  std::vector<Gate> constraints;
  constraints.push_back ({30, 0, "north"});
  constraints.push_back ({50, Dungeon::HEIGHT - 1, "south"});

  auto d = Dungeon::Generate ("constrained_test", 1, constraints);

  ASSERT_EQ (d.GetGates ().size (), 4u);

  bool foundNorth = false, foundSouth = false;
  for (const auto& g : d.GetGates ())
    {
      if (g.direction == "north")
        {
          EXPECT_EQ (g.x, 30);
          EXPECT_EQ (g.y, 0);
          foundNorth = true;
        }
      else if (g.direction == "south")
        {
          EXPECT_EQ (g.x, 50);
          EXPECT_EQ (g.y, Dungeon::HEIGHT - 1);
          foundSouth = true;
        }
    }
  EXPECT_TRUE (foundNorth);
  EXPECT_TRUE (foundSouth);
}

TEST_F (DungeonTests, ConstrainedGatesStillConnected)
{
  /* Force all 4 gates to specific positions.  */
  std::vector<Gate> constraints;
  constraints.push_back ({10, 0, "north"});
  constraints.push_back ({70, Dungeon::HEIGHT - 1, "south"});
  constraints.push_back ({0, 20, "west"});
  constraints.push_back ({Dungeon::WIDTH - 1, 10, "east"});

  for (int i = 0; i < 5; i++)
    {
      auto d = Dungeon::Generate ("conn_constrained_" + std::to_string (i),
                                   1, constraints);

      const int floorCount = d.CountTiles (Tile::Floor);
      const int gateCount = d.CountTiles (Tile::Gate);
      const int reachable = FloodFillCount (d);

      EXPECT_EQ (reachable, floorCount + gateCount)
          << "Seed index " << i << ": constrained gates not reachable";
    }
}

TEST_F (DungeonTests, EmptyConstraintsSameAsNoConstraints)
{
  auto d1 = Dungeon::Generate ("empty_constraints", 1);
  auto d2 = Dungeon::Generate ("empty_constraints", 1, {});

  /* With empty constraints, the constrained overload should produce
     the same result as the unconstrained one.  */
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      EXPECT_EQ (d1.GetTile (x, y), d2.GetTile (x, y));
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

// ============================================================
// Cross-language determinism parity (mirrors the TS test in
// xaya-roguelike-frontend/src/game/parity_test.ts).  Both compute a
// canonical signature of a constrained dungeon + entry-gate spawn and
// hash it with the shared SHA-256 HashSeed.  If the C++ and TS dungeon
// generators ever drift, these two baked constants diverge and one side
// fails — catching a determinism break before it silently rejects
// settlements.
// ============================================================

/* Canonical signature: depth, entry-gate spawn, gates (sorted by
   direction), and the row-major tile grid.  MUST match the TS
   dungeonSignature() byte-for-byte.  */
std::string
DungeonSignature (const std::string& seed, const int depth,
                  const std::vector<Gate>& constraints,
                  const std::string& entryDir)
{
  const Dungeon d = constraints.empty ()
      ? Dungeon::Generate (seed, depth)
      : Dungeon::Generate (seed, depth, constraints);

  /* Entry-gate spawn — mirrors DungeonGame::Create.  */
  int sx = Dungeon::WIDTH / 2, sy = Dungeon::HEIGHT / 2;
  bool spawned = false;
  if (!entryDir.empty ())
    for (const auto& g : d.GetGates ())
      if (g.direction == entryDir)
        {
          sx = g.x;
          sy = g.y;
          if (entryDir == "north") sy += 1;
          else if (entryDir == "south") sy -= 1;
          else if (entryDir == "east") sx -= 1;
          else if (entryDir == "west") sx += 1;
          spawned = true;
          break;
        }
  if (!spawned)
    {
      const auto& rooms = d.GetRooms ();
      if (!rooms.empty ())
        {
          sx = rooms[0].centerX ();
          sy = rooms[0].centerY ();
        }
    }

  std::vector<Gate> gates (d.GetGates ().begin (), d.GetGates ().end ());
  std::sort (gates.begin (), gates.end (),
             [] (const Gate& a, const Gate& b)
               { return a.direction < b.direction; });

  std::string s = "depth=" + std::to_string (depth)
      + ";spawn=" + std::to_string (sx) + "," + std::to_string (sy)
      + ";gates=";
  for (const auto& g : gates)
    s += g.direction + ":" + std::to_string (g.x) + ","
         + std::to_string (g.y) + ";";
  s += ";tiles=";
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      s += std::to_string (static_cast<int> (d.GetTile (x, y)));
  return s;
}

TEST_F (DungeonTests, CrossLanguageParity)
{
  /* Fixed inputs shared with parity_test.ts: a segment whose WEST gate is
     aligned to a neighbour at row 20, entered from the west.  */
  const std::vector<Gate> constraints { Gate { 0, 20, "west" } };
  const uint32_t h = HashSeed (
      DungeonSignature ("paritytest", 3, constraints, "west"));

  /* Baked value, confirmed identical on the TS side (parity_test.ts).  If
     this fails, the C++ and TS dungeon generators have diverged.  */
  EXPECT_EQ (h, 1455554007u);
}

} // anonymous namespace
} // namespace rog
