//--------------------------------------------------------------------------------------------------
// Copyright (c) 2018 Marcus Geelnard
//
// This software is provided 'as-is', without any express or implied warranty. In no event will the
// authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose, including commercial
// applications, and to alter it and redistribute it freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not claim that you wrote
//     the original software. If you use this software in a product, an acknowledgment in the
//     product documentation would be appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//     being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//--------------------------------------------------------------------------------------------------

#ifndef BUILDCACHE_HASHER_HPP_
#define BUILDCACHE_HASHER_HPP_

#include <cstdint>
#include <string>

extern "C" {
#include <md4/md4.h>
}

namespace bcache {

class hasher_t {
public:
  class hash_t {
  public:
    uint8_t* data() {
      return &m_data[0];
    }

    const uint8_t* data() const {
      return &m_data[0];
    }

    /// @brief Convert a hash to a hexadecimal string.
    /// @returns A hexadecimal string representation of the given hash.
    const std::string as_string() const {
      static const char digits[17] = "0123456789abcdef";
      std::string result(SIZE * 2, '0');
      for (size_t i = 0; i < SIZE; ++i) {
        result[i * 2] = digits[m_data[i] >> 4];
        result[i * 2 + 1] = digits[m_data[i] & 0x0fu];
      }
      return result;
    }

  private:
    // The hash size is 128 bits.
    static const size_t SIZE = 16u;

    uint8_t m_data[SIZE];
  };

  hasher_t() {
    MD4_Init(&m_ctx);
  }

  /// @brief Update the hash with more data.
  /// @param text The data to hash.
  void update(const std::string& text) {
    MD4_Update(&m_ctx, text.data(), text.size());
  }

  /// @brief Finalize the hash calculation.
  /// @param[out] result The result of the hash.
  /// @note This method must only be called once.
  // void final(hash_t& result) {
  hash_t final() {
    hash_t result;
    MD4_Final(result.data(), &m_ctx);
    return result;
  }

private:
  MD4_CTX m_ctx;
};

}  // namespace bcache

#endif  // BUILDCACHE_HASHER_HPP_