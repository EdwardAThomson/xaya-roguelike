#include "dungeon.hpp"
#include "hash.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rog
{

namespace
{

/**
 * Creates a seeded mt19937 from a string using FNV-1a hash.
 * Cross-language deterministic — same input produces the same seed
 * in C++, TypeScript, or any other implementation of FNV-1a + MT19937.
 */
std::mt19937
SeedFromString (const std::string& seed, const int depth)
{
  const auto h = HashSeed (seed + ":" + std::to_string (depth));
  return std::mt19937 (h);
}

/** Returns a random int in [min, max] inclusive.  */
int
RandRange (std::mt19937& rng, const int min, const int max)
{
  std::uniform_int_distribution<int> dist (min, max);
  return dist (rng);
}

} // anonymous namespace

/* ************************************************************************** */

void
Dungeon::Clear ()
{
  for (auto& row : tiles)
    row.fill (Tile::Wall);
}

void
Dungeon::CarveRoom (const Room& room)
{
  for (int y = room.y; y < room.y + room.height; y++)
    for (int x = room.x; x < room.x + room.width; x++)
      tiles[y][x] = Tile::Floor;
}

bool
Dungeon::RoomsOverlap (const Room& a, const Room& b)
{
  return a.x <= b.x + b.width + ROOM_BUFFER
      && a.x + a.width + ROOM_BUFFER >= b.x
      && a.y <= b.y + b.height + ROOM_BUFFER
      && a.y + a.height + ROOM_BUFFER >= b.y;
}

void
Dungeon::CarveHorizontalCorridor (int x1, int x2, const int y)
{
  if (x1 > x2)
    std::swap (x1, x2);
  for (int x = x1; x <= x2; x++)
    if (y >= 0 && y < HEIGHT && x >= 0 && x < WIDTH)
      tiles[y][x] = Tile::Floor;
}

void
Dungeon::CarveVerticalCorridor (int y1, int y2, const int x)
{
  if (y1 > y2)
    std::swap (y1, y2);
  for (int y = y1; y <= y2; y++)
    if (y >= 0 && y < HEIGHT && x >= 0 && x < WIDTH)
      tiles[y][x] = Tile::Floor;
}

void
Dungeon::CarveLCorridor (const int x1, const int y1,
                          const int x2, const int y2,
                          std::mt19937& rng)
{
  if (RandRange (rng, 0, 1) == 0)
    {
      /* Horizontal first, then vertical.  */
      CarveHorizontalCorridor (x1, x2, y1);
      CarveVerticalCorridor (y1, y2, x2);
    }
  else
    {
      /* Vertical first, then horizontal.  */
      CarveVerticalCorridor (y1, y2, x1);
      CarveHorizontalCorridor (x1, x2, y2);
    }
}

void
Dungeon::GenerateRooms (std::mt19937& rng)
{
  const int numRooms = RandRange (rng, MIN_ROOMS, MAX_ROOMS);
  const int maxAttempts = numRooms * 3;

  for (int attempt = 0; attempt < maxAttempts
       && static_cast<int> (rooms.size ()) < numRooms; attempt++)
    {
      Room room;
      room.width = RandRange (rng, MIN_ROOM_WIDTH, MAX_ROOM_WIDTH);
      room.height = RandRange (rng, MIN_ROOM_HEIGHT, MAX_ROOM_HEIGHT);
      room.x = RandRange (rng, 1, WIDTH - room.width - 2);
      room.y = RandRange (rng, 1, HEIGHT - room.height - 2);

      bool overlaps = false;
      for (const auto& existing : rooms)
        {
          if (RoomsOverlap (room, existing))
            {
              overlaps = true;
              break;
            }
        }

      if (!overlaps)
        {
          CarveRoom (room);
          rooms.push_back (room);
        }
    }
}

void
Dungeon::ConnectRooms (std::mt19937& rng)
{
  if (rooms.size () < 2)
    return;

  /* Connect consecutive rooms.  */
  for (size_t i = 0; i + 1 < rooms.size (); i++)
    {
      CarveLCorridor (rooms[i].centerX (), rooms[i].centerY (),
                       rooms[i + 1].centerX (), rooms[i + 1].centerY (),
                       rng);
    }

  /* Loop closure: connect first to last.  */
  if (rooms.size () > 2)
    {
      CarveLCorridor (rooms.front ().centerX (), rooms.front ().centerY (),
                       rooms.back ().centerX (), rooms.back ().centerY (),
                       rng);
    }
}

void
Dungeon::PlaceGate (const int x, const int y, const std::string& direction,
                     std::mt19937& rng)
{
  Gate g;
  g.x = x;
  g.y = y;
  g.direction = direction;
  gates.push_back (g);
  tiles[y][x] = Tile::Gate;
  ConnectGateToNearestRoom (g, rng);
}

void
Dungeon::PlaceGates (std::mt19937& rng)
{
  PlaceGate (RandRange (rng, GATE_MARGIN, WIDTH - GATE_MARGIN - 1),
             0, "north", rng);
  PlaceGate (RandRange (rng, GATE_MARGIN, WIDTH - GATE_MARGIN - 1),
             HEIGHT - 1, "south", rng);
  PlaceGate (0, RandRange (rng, GATE_MARGIN, HEIGHT - GATE_MARGIN - 1),
             "west", rng);
  PlaceGate (WIDTH - 1, RandRange (rng, GATE_MARGIN, HEIGHT - GATE_MARGIN - 1),
             "east", rng);
}

void
Dungeon::ConnectGateToNearestRoom (const Gate& gate, std::mt19937& rng)
{
  if (rooms.empty ())
    return;

  /* Find nearest room center.  */
  int bestDist = std::numeric_limits<int>::max ();
  int bestIdx = 0;
  for (size_t i = 0; i < rooms.size (); i++)
    {
      const int dx = gate.x - rooms[i].centerX ();
      const int dy = gate.y - rooms[i].centerY ();
      const int dist = std::abs (dx) + std::abs (dy);
      if (dist < bestDist)
        {
          bestDist = dist;
          bestIdx = static_cast<int> (i);
        }
    }

  /* Carve corridor from one tile inward from the gate to the room center.  */
  int startX = gate.x;
  int startY = gate.y;
  if (gate.direction == "north")
    startY = 1;
  else if (gate.direction == "south")
    startY = HEIGHT - 2;
  else if (gate.direction == "west")
    startX = 1;
  else if (gate.direction == "east")
    startX = WIDTH - 2;

  CarveLCorridor (startX, startY,
                   rooms[bestIdx].centerX (), rooms[bestIdx].centerY (),
                   rng);
}

/* ************************************************************************** */

Dungeon
Dungeon::Generate (const std::string& seed, const int depth)
{
  Dungeon d;
  d.depth = depth;
  d.Clear ();

  auto rng = SeedFromString (seed, depth);
  d.GenerateRooms (rng);
  d.ConnectRooms (rng);
  d.PlaceGates (rng);

  return d;
}

Dungeon
Dungeon::Generate (const std::string& seed, const int depth,
                   const std::vector<Gate>& constraints)
{
  Dungeon d;
  d.depth = depth;
  d.Clear ();

  auto rng = SeedFromString (seed, depth);
  d.GenerateRooms (rng);
  d.ConnectRooms (rng);

  /* Track which directions are already constrained.  */
  bool hasNorth = false, hasSouth = false, hasEast = false, hasWest = false;
  for (const auto& c : constraints)
    {
      d.PlaceGate (c.x, c.y, c.direction, rng);
      if (c.direction == "north") hasNorth = true;
      else if (c.direction == "south") hasSouth = true;
      else if (c.direction == "east") hasEast = true;
      else if (c.direction == "west") hasWest = true;
    }

  /* Fill in any remaining directions with random positions.  */
  if (!hasNorth)
    d.PlaceGate (RandRange (rng, GATE_MARGIN, WIDTH - GATE_MARGIN - 1),
                 0, "north", rng);
  if (!hasSouth)
    d.PlaceGate (RandRange (rng, GATE_MARGIN, WIDTH - GATE_MARGIN - 1),
                 HEIGHT - 1, "south", rng);
  if (!hasWest)
    d.PlaceGate (0, RandRange (rng, GATE_MARGIN, HEIGHT - GATE_MARGIN - 1),
                 "west", rng);
  if (!hasEast)
    d.PlaceGate (WIDTH - 1, RandRange (rng, GATE_MARGIN, HEIGHT - GATE_MARGIN - 1),
                 "east", rng);

  return d;
}

Tile
Dungeon::GetTile (const int x, const int y) const
{
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
    return Tile::Wall;
  return tiles[y][x];
}

void
Dungeon::SetTile (const int x, const int y, const Tile t)
{
  if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
    tiles[y][x] = t;
}

std::pair<int, int>
Dungeon::GetRandomFloorPosition (std::mt19937& rng) const
{
  /* Collect all floor positions and pick one.  */
  std::vector<std::pair<int, int>> floors;
  for (int y = 0; y < HEIGHT; y++)
    for (int x = 0; x < WIDTH; x++)
      if (tiles[y][x] == Tile::Floor)
        floors.push_back ({x, y});

  if (floors.empty ())
    return {-1, -1};

  std::uniform_int_distribution<size_t> dist (0, floors.size () - 1);
  return floors[dist (rng)];
}

int
Dungeon::CountTiles (const Tile t) const
{
  int count = 0;
  for (int y = 0; y < HEIGHT; y++)
    for (int x = 0; x < WIDTH; x++)
      if (tiles[y][x] == t)
        count++;
  return count;
}

} // namespace rog
