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

#include <wrappers/program_wrapper.hpp>

#include <base/compressor.hpp>
#include <base/debug_utils.hpp>
#include <base/hasher.hpp>
#include <cache/cache_entry.hpp>
#include <cache/data_store.hpp>
#include <config/configuration.hpp>
#include <sys/perf_utils.hpp>
#include <sys/sys_utils.hpp>

#include <iostream>
#include <map>
#include <sstream>

namespace bcache {
namespace {
std::string PROGRAM_ID_CACHE_NAME = "prgid";
time::seconds_t PROGRAM_ID_CACHE_LIFE_TIME = 300;  // Five minutes.
}  // namespace

program_wrapper_t::capabilities_t::capabilities_t(const string_list_t& cap_strings) {
  // Capability options are opt-in (false by default). Furthermore, if a capability is disabled in
  // the user provided configuration, the capability will be disabled.
  for (const auto& str : cap_strings) {
    if (str == "create_target_dirs") {
      m_create_target_dirs = true;
    } else if (!m_direct_mode && str == "direct_mode") {
      m_direct_mode = config::direct_mode();  // Only enable if enabled in the config.
    } else if (str == "force_direct_mode") {
      m_direct_mode = true;
    } else if (str == "hard_links") {
      m_hard_links = config::hard_links();  // Only enable if enabled in the config.
    } else {
      debug::log(debug::ERROR) << "Invalid capability string: " << str;
    }
  }
}

program_wrapper_t::program_wrapper_t(const file::exe_path_t& exe_path, const string_list_t& args)
    : m_exe_path(exe_path), m_unresolved_args(args) {
}

bool program_wrapper_t::handle_command(int& return_code) {
  return_code = 1;

  try {
    // Begin by resolving any response files.
    PERF_START(RESOLVE_ARGS);
    resolve_args();
    PERF_STOP(RESOLVE_ARGS);

    // Get wrapper capabilities.
    PERF_START(GET_CAPABILITIES);
    m_active_capabilities = capabilities_t(get_capabilities());
    PERF_STOP(GET_CAPABILITIES);

    // Get the list of files that are expected to be generated by the command. This is in fact a
    // map of file ID:s to their corresponding file path.
    PERF_START(GET_BUILD_FILES);
    const auto expected_files = get_build_files();
    PERF_STOP(GET_BUILD_FILES);

    // Start a hash.
    hasher_t hasher;

    // Add additional file contents to the resulting hash.
    PERF_START(HASH_EXTRA_FILES);
    for (const auto& extra_file : bcache::config::hash_extra_files()) {
      hasher.update_from_file(extra_file);
    }
    PERF_STOP(HASH_EXTRA_FILES);

    // Hash the program identification (version string or similar).
    PERF_START(GET_PRG_ID);
    hasher.update(get_program_id_cached());
    PERF_STOP(GET_PRG_ID);

    // Hash the (filtered) command line flags and environment variables.
    PERF_START(FILTER_ARGS);
    hasher.update(get_relevant_arguments());
    hasher.update(get_relevant_env_vars());
    PERF_STOP(FILTER_ARGS);

    // This string will be non-empty if we are able to create a direct mode cache lookup hash. If we
    // have a miss in the DM cache, this will be used for creating the DM cache entry.
    std::string direct_hash;

    if (m_active_capabilities.direct_mode()) {
      try {
        const auto input_files = get_input_files();
        if (input_files.size() > 0) {
          // The hash so far is common for direct mode and preprocessor mode. Make a copy and inject
          // a separator sequence to ensure that there can not be any collisions between direct mode
          // and preprocessor mode hashes.
          hasher_t dm_hasher = hasher;
          dm_hasher.inject_separator();

          // Hash the complete command line, as we need things like defines that are usually
          // filtered by get_relevant_arguments().
          dm_hasher.update(m_args);

          // Hash all the input files.
          PERF_START(HASH_INPUT_FILES);
          for (const auto& file : input_files) {
            // Hash the complete source file path. This ensures that we get different direct mode
            // cache entries for different source paths, which should minimize cache thrashing when
            // different work folders are used (e.g. in a CI system with several concurrent
            // executors).
            dm_hasher.update(file::resolve_path(file));
            dm_hasher.inject_separator();

            // Hash the source file content.
            // TODO(m): Check file for disqualifying content (e.g. __TIME__ in C/C++ files).
            dm_hasher.update_from_file(file);
          }
          PERF_STOP(HASH_INPUT_FILES);
          direct_hash = dm_hasher.final().as_string();

          // Look up the hash in the cache.
          if (m_cache.lookup_direct(direct_hash,
                                    expected_files,
                                    m_active_capabilities.hard_links(),
                                    m_active_capabilities.create_target_dirs(),
                                    return_code)) {
            return true;
          }
        }
      } catch (const std::runtime_error& e) {
        // This can happen if one of the input files are missing, for instance.
        debug::log(debug::ERROR) << "Direct mode lookup failed: " << e.what();
      }
    }

    // Hash the preprocessed file contents.
    PERF_START(PREPROCESS);
    hasher.update(preprocess_source());
    PERF_STOP(PREPROCESS);

    // Finalize the hash.
    const auto hash = hasher.final().as_string();

    // Look up the entry in the cache(s).
    if (m_cache.lookup(hash,
                       expected_files,
                       m_active_capabilities.hard_links(),
                       m_active_capabilities.create_target_dirs(),
                       return_code)) {
      if (!direct_hash.empty()) {
        // Add a direct mode cache entry.
        m_cache.add_direct(direct_hash, hash, get_implicit_input_files());
      }

      debug::log(debug::INFO) << "Cache hit (" << hash << ")";
      return true;
    }

    debug::log(debug::INFO) << "Cache miss (" << hash << ")";

    // If the "terminate on a miss" mode is enabled and we didn't find an entry in the cache, we
    // exit with an error code.
    if (config::terminate_on_miss()) {
      string_list_t files;
      for (const auto& file : expected_files) {
        files += file.second.path();
      }
      debug::log(debug::INFO) << "Terminating! Expected files: " << files.join(", ");
      return_code = 1;
      return true;  // Don't fall back to running the command (we have "handled" it).
    }

    // Run the actual program command to produce the build file(s).
    PERF_START(RUN_FOR_MISS);
    const auto result = run_for_miss();
    PERF_STOP(RUN_FOR_MISS);

    // Extract only the file ID:s (and filter out missing optional files).
    std::vector<std::string> file_ids;
    for (const auto& file : expected_files) {
      const auto& expected_file = file.second;
      if (expected_file.required() || file::file_exists(expected_file.path())) {
        file_ids.emplace_back(file.first);
      }
    }

    // Create a new entry in the cache.
    // Note: We do not want to create cache entries for failed program runs. We could, but that
    // would run the risk of caching intermittent faults for instance.
    // And we do not want to create cache entries when the readonly mode is enabled.
    if (result.return_code == 0 && !config::read_only()) {
      // Add the entry to the cache.
      const cache_entry_t entry(
          file_ids,
          config::compress() ? cache_entry_t::comp_mode_t::ALL : cache_entry_t::comp_mode_t::NONE,
          result.std_out,
          result.std_err,
          result.return_code);
      m_cache.add(hash, entry, expected_files, m_active_capabilities.hard_links());

      if (!direct_hash.empty()) {
        // Add a direct mode cache entry.
        m_cache.add_direct(direct_hash, hash, get_implicit_input_files());
      }
    }

    // Everything's ok!
    // Note: Even if the program failed, we've done the expected job (running the program again
    // would just take twice the time and give the same errors).
    return_code = result.return_code;
    return true;
  } catch (std::exception& e) {
    debug::log(debug::DEBUG) << "Exception: " << e.what();
  } catch (...) {
    // Catch-all in order to not propagate exceptions any higher up (we'll return false).
    debug::log(debug::ERROR) << "UNEXPECTED EXCEPTION";
  }

  return false;
}

//--------------------------------------------------------------------------------------------------
// Default wrapper interface implementation. Wrappers are expected to override the parts that are
// relevant.
//--------------------------------------------------------------------------------------------------

void program_wrapper_t::resolve_args() {
  // Default: Make a copy of the unresolved args.
  m_args = m_unresolved_args;
}

string_list_t program_wrapper_t::get_capabilities() {
  // Default: No capabilities are supported.
  string_list_t capabilites;
  return capabilites;
}

std::map<std::string, expected_file_t> program_wrapper_t::get_build_files() {
  // Default: There are no build files generated by the command.
  std::map<std::string, expected_file_t> result;
  return result;
}

std::string program_wrapper_t::get_program_id() {
  // Default: The hash of the program binary serves as the program identification.
  hasher_t hasher;
  hasher.update_from_file(m_exe_path.real_path());
  return hasher.final().as_string();
}

string_list_t program_wrapper_t::get_relevant_arguments() {
  // Default: All arguments are relevant.
  return m_args;
}

std::map<std::string, std::string> program_wrapper_t::get_relevant_env_vars() {
  // Default: There are no relevant environment variables.
  std::map<std::string, std::string> env_vars;
  return env_vars;
}

string_list_t program_wrapper_t::get_input_files() {
  // Default: There are no input files.
  string_list_t result;
  return result;
}

std::string program_wrapper_t::preprocess_source() {
  // Default: There is no prepocessing step.
  return std::string();
}

string_list_t program_wrapper_t::get_implicit_input_files() {
  // Default: No implicit input files.
  string_list_t result;
  return result;
}

sys::run_result_t program_wrapper_t::run_for_miss() {
  // Default: Run the program with the configured prefix.
  return sys::run_with_prefix(m_unresolved_args, false);
}

std::string program_wrapper_t::get_program_id_cached() {
  try {
    // Get an ID of the program executable, based on its path, size and modification time.
    const auto file_info = file::get_file_info(m_exe_path.real_path());
    std::ostringstream ss;
    ss << file_info.path() << ":" << file_info.size() << ":" << file_info.modify_time();
    hasher_t hasher;
    hasher.update(ss.str());
    const auto key = hasher.final().as_string();

    // Look up the program ID in the data store.
    data_store_t store(PROGRAM_ID_CACHE_NAME);
    const auto item = store.get_item(key);
    if (item.is_valid()) {
      debug::log(debug::DEBUG) << "Found cached program ID for " << m_args[0];
      return item.value();
    }

    // We had a miss. Query the program ID and add it to the meta store.
    debug::log(debug::DEBUG) << "Program ID cache miss for " << m_args[0];
    auto program_id = get_program_id();
    store.store_item(key, program_id, PROGRAM_ID_CACHE_LIFE_TIME);
    return program_id;
  } catch (std::exception& e) {
    // Something went wrong. Fall back to querying the program ID.
    debug::log(debug::ERROR) << "Unable to get cached program ID: " << e.what();
  }

  return get_program_id();
}

}  // namespace bcache
