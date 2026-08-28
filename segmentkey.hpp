#ifndef ROG_SEGMENTKEY_HPP
#define ROG_SEGMENTKEY_HPP

#include <ostream>
#include <string>
#include <tuple>

namespace rog
{

/**
 * A segment's identity: its world coordinate.
 *
 * There is deliberately no surrogate integer id.  A segment IS the place it
 * occupies, (`world_x`, `world_y`) is the primary key of `segments`, and the
 * same pair is how a segment is named in moves, in the RPC output and in the
 * frontend.  An allocated id could be reused once a provisional segment was
 * pruned, silently repointing every cached reference at a different place;
 * a coordinate always means the same place forever.
 *
 * The hub is (0, 0).  It has no row in `segments` -- it is the world's
 * origin, always present and always confirmed.
 */
struct SegmentKey
{

  int x = 0;
  int y = 0;

  SegmentKey () = default;

  SegmentKey (const int xx, const int yy)
    : x(xx), y(yy)
  {}

  /** The hub, at the world origin.  */
  bool
  IsHub () const
  {
    return x == 0 && y == 0;
  }

  /** Canonical short form for logs and messages, e.g. "(1, -2)".  */
  std::string
  ToString () const
  {
    return "(" + std::to_string (x) + ", " + std::to_string (y) + ")";
  }

  friend bool
  operator== (const SegmentKey& a, const SegmentKey& b)
  {
    return a.x == b.x && a.y == b.y;
  }

  friend bool
  operator!= (const SegmentKey& a, const SegmentKey& b)
  {
    return !(a == b);
  }

  /** Total order so keys can go into std::map / std::set deterministically.  */
  friend bool
  operator< (const SegmentKey& a, const SegmentKey& b)
  {
    return std::tie (a.x, a.y) < std::tie (b.x, b.y);
  }

  friend std::ostream&
  operator<< (std::ostream& out, const SegmentKey& k)
  {
    return out << k.ToString ();
  }

};

/**
 * Returns the coordinate one step from `from` in the given direction.
 * An unrecognised direction returns `from` unchanged; callers validate the
 * direction before they get here.
 */
inline SegmentKey
Neighbour (const SegmentKey& from, const std::string& dir)
{
  if (dir == "north") return {from.x, from.y + 1};
  if (dir == "south") return {from.x, from.y - 1};
  if (dir == "east") return {from.x + 1, from.y};
  if (dir == "west") return {from.x - 1, from.y};
  return from;
}

/** The direction opposite the given one ("" if unrecognised).  */
inline std::string
OppositeDirection (const std::string& dir)
{
  if (dir == "north") return "south";
  if (dir == "south") return "north";
  if (dir == "east") return "west";
  if (dir == "west") return "east";
  return "";
}

} // namespace rog

#endif // ROG_SEGMENTKEY_HPP
