// SPDX-License-Identifier: GPL-3.0-only
/** @file linux_session_broker_test_logging.cpp
 *  @brief Real Boost.Log globals for the broker-only CI test executable.
 *
 * The full test_sunshine target gets these from logging.cpp. This small target
 * exercises the production RPC client without linking the entire streaming host.
 */
#include "src/logging.h"

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;

#ifdef HERMES_SESSION_AUDIO_STANDALONE
// The audio-only executable uses production capture/server code without the
// host's config loader. No runtime configuration is needed for private graphs.
namespace config {
  audio_t audio {};
}
#endif
