// Only the standalone network target supplies these process globals. The
// complete Hermes test executable already links their production definitions.
#ifdef HERMES_NETWORK_LIFECYCLE_TESTS
  #include <src/config.h>
  #include <src/logging.h>

namespace config {
  stream_t stream {};
  sunshine_t sunshine {};
}  // namespace config

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;
#endif
