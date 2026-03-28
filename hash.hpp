#ifndef ROG_HASH_HPP
#define ROG_HASH_HPP

#include <openssl/sha.h>

#include <cstdint>
#include <string>

namespace rog
{

/**
 * SHA-256 based seed hash.  Hashes the input string with SHA-256 and
 * returns the first 4 bytes as a uint32_t.  This is cross-platform and
 * cross-language deterministic — SHA-256 is a standard algorithm with
 * identical implementations in C++, TypeScript (Web Crypto API), Python,
 * and every other language.
 *
 * Used to convert seed strings (transaction hashes, dungeon IDs) into
 * 32-bit seeds for the mt19937 RNG.
 */
inline uint32_t
HashSeed (const std::string& data)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256 (reinterpret_cast<const unsigned char*> (data.c_str ()),
          data.size (), digest);

  /* Take the first 4 bytes as a big-endian uint32.  */
  return (static_cast<uint32_t> (digest[0]) << 24)
       | (static_cast<uint32_t> (digest[1]) << 16)
       | (static_cast<uint32_t> (digest[2]) << 8)
       | (static_cast<uint32_t> (digest[3]));
}

} // namespace rog

#endif // ROG_HASH_HPP
