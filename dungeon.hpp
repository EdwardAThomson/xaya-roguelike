#ifndef ROG_DUNGEON_HPP
#define ROG_DUNGEON_HPP

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace rog
{

enum class Tile : uint8_t
{
  Wall,
  Floor,
  Gate,
};

struct Room
{
  int x, y, width, height;

  int centerX () const { return x + width / 2; }
  int centerY () const { return y + height / 2; }
};

struct Gate
{
  int x, y;
  std::string direction;  /* "north", "south", "east", "west" */
};

/**
 * A deterministically generated dungeon.  Given the same seed and depth,
 * the dungeon will be identical on every node.
 */
class Dungeon
{

public:

  static constexpr int WIDTH = 80;
  static constexpr int HEIGHT = 40;

  static constexpr int MIN_ROOMS = 8;
  static constexpr int MAX_ROOMS = 15;
  static constexpr int MIN_ROOM_WIDTH = 4;
  static constexpr int MAX_ROOM_WIDTH = 8;
  static constexpr int MIN_ROOM_HEIGHT = 4;
  static constexpr int MAX_ROOM_HEIGHT = 7;
  static constexpr int ROOM_BUFFER = 1;
  static constexpr int GATE_MARGIN = 2;

private:

  std::array<std::array<Tile, WIDTH>, HEIGHT> tiles;
  std::vector<Room> rooms;
  std::vector<Gate> gates;
  int depth;

  /** Fills the entire grid with walls.  */
  void Clear ();

  /** Carves a room into the grid (sets tiles to Floor).  */
  void CarveRoom (const Room& room);

  /** Checks whether two rooms overlap (including buffer).  */
  static bool RoomsOverlap (const Room& a, const Room& b);

  /** Carves a horizontal corridor at row y from x1 to x2.  */
  void CarveHorizontalCorridor (int x1, int x2, int y);

  /** Carves a vertical corridor at column x from y1 to y2.  */
  void CarveVerticalCorridor (int y1, int y2, int x);

  /** Carves an L-shaped corridor between two points.  */
  void CarveLCorridor (int x1, int y1, int x2, int y2, std::mt19937& rng);

  /** Generates rooms within the grid.  */
  void GenerateRooms (std::mt19937& rng);

  /** Connects rooms with corridors.  */
  void ConnectRooms (std::mt19937& rng);

  /** Places gates on the four cardinal walls (random positions).  */
  void PlaceGates (std::mt19937& rng);

  /**
   * Places a single gate at the given position and connects it to the
   * nearest room.  Used for both random and constrained gates.
   */
  void PlaceGate (int x, int y, const std::string& direction,
                   std::mt19937& rng);

  /** Connects a gate to the nearest room via L-shaped corridor.  */
  void ConnectGateToNearestRoom (const Gate& gate, std::mt19937& rng);

public:

  Dungeon () = default;

  /**
   * Generates a dungeon deterministically from a seed string and depth.
   * The same seed+depth will always produce the identical dungeon.
   */
  static Dungeon Generate (const std::string& seed, int depth);

  /**
   * Generates a dungeon with constrained gates.  Any gates provided in
   * the constraints vector are placed at their exact positions; remaining
   * cardinal directions get random gate positions.  This is used when
   * adjacent segments already exist and their gates must align.
   */
  static Dungeon Generate (const std::string& seed, int depth,
                           const std::vector<Gate>& constraints);

  /** Returns the tile at the given position.  */
  Tile GetTile (int x, int y) const;

  /** Sets the tile at the given position.  */
  void SetTile (int x, int y, Tile t);

  /** Returns all generated rooms.  */
  const std::vector<Room>& GetRooms () const { return rooms; }

  /** Returns all generated gates.  */
  const std::vector<Gate>& GetGates () const { return gates; }

  /** Returns the depth.  */
  int GetDepth () const { return depth; }

  /**
   * Returns a random floor position using the given RNG.
   * Useful for placing monsters/items.
   */
  std::pair<int, int> GetRandomFloorPosition (std::mt19937& rng) const;

  /**
   * Counts the number of tiles of the given type.
   */
  int CountTiles (Tile t) const;

};

} // namespace rog

#endif // ROG_DUNGEON_HPP
