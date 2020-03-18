//--------------------------------------------------------------------------------------------------
// Copyright (c) 2020 Marcus Geelnard
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

#ifndef BUILDCACHE_TI_C6X_WRAPPER_HPP_
#define BUILDCACHE_TI_C6X_WRAPPER_HPP_

#include <wrappers/program_wrapper.hpp>

namespace bcache {
/// @brief A program wrapper for the TI C6x compiler.
class ti_c6x_wrapper_t : public program_wrapper_t {
public:
  ti_c6x_wrapper_t(const string_list_t& args);

  bool can_handle_command() override;

protected:
  void resolve_args() override;
  std::string preprocess_source() override;
  string_list_t get_relevant_arguments() override;
  std::string get_program_id() override;
  std::map<std::string, expected_file_t> get_build_files() override;

  string_list_t m_resolved_args;

private:
  void append_response_file(const std::string& response_file);
};
}  // namespace bcache
#endif  // BUILDCACHE_TI_C6X_WRAPPER_HPP_