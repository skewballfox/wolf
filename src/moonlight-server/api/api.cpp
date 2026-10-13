#include <api/api.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <charconv>
#include <cstring>
#include <events/reflectors.hpp>
#include <filesystem>
#include <grp.h>
#include <helpers/utils.hpp>
#include <memory>
#include <optional>
#include <rfl/json.hpp>
#include <unistd.h>
#include <vector>

namespace wolf::api {

using namespace wolf::core;

namespace {

/**
 * Parses an unsigned number in the given base, rejecting empty strings and trailing characters
 */
template <typename T> std::optional<T> parse_number(std::string_view str, int base) {
  T value{};
  auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value, base);
  if (str.empty() || ec != std::errc{} || ptr != str.data() + str.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<gid_t> resolve_gid(const std::string &group_name) {
  if (auto gid = parse_number<gid_t>(group_name, 10)) {
    return gid;
  }

  auto buf_size = sysconf(_SC_GETGR_R_SIZE_MAX);
  std::vector<char> buf(buf_size > 0 ? buf_size : 16384);
  struct group grp = {};
  struct group *result = nullptr;
  if (getgrnam_r(group_name.c_str(), &grp, buf.data(), buf.size(), &result) != 0 || result == nullptr) {
    return std::nullopt;
  }
  return result->gr_gid;
}

} // namespace

bool set_socket_permissions(const std::string &socket_path, const char *group, const char *mode) {
  bool ok = true;

  if (group != nullptr) {
    if (auto gid = resolve_gid(group)) {
      if (::chown(socket_path.c_str(), static_cast<uid_t>(-1), *gid) == 0) {
        logs::log(logs::info, "API socket {} handed to group {}", socket_path, group);
      } else {
        logs::log(logs::warning, "Unable to hand API socket {} to group {}: {}", socket_path, group, strerror(errno));
        ok = false;
      }
    } else {
      logs::log(logs::warning, "Unknown group {} for the API socket", group);
      ok = false;
    }
  }

  if (mode != nullptr) {
    auto perms = parse_number<unsigned int>(mode, 8);
    if (perms && *perms <= 0777) {
      std::error_code ec;
      std::filesystem::permissions(socket_path,
                                   static_cast<std::filesystem::perms>(*perms),
                                   std::filesystem::perm_options::replace,
                                   ec);
      if (ec) {
        logs::log(logs::warning, "Unable to set API socket {} mode to {}: {}", socket_path, mode, ec.message());
        ok = false;
      } else {
        logs::log(logs::info, "API socket {} mode set to {}", socket_path, mode);
      }
    } else {
      logs::log(logs::warning, "Invalid API socket mode {}, expected octal up to 0777 (e.g. 0660)", mode);
      ok = false;
    }
  }

  return ok;
}

void start_server(std::string_view runtime_dir, immer::box<state::AppState> app_state) {
  auto default_socket_path = std::filesystem::path(runtime_dir) / "wolf.sock";
  auto socket_path = utils::get_env("WOLF_SOCKET_PATH", default_socket_path.c_str());
  logs::log(logs::info, "Starting Wolf API server on {}", socket_path);

  ::unlink(socket_path);
  boost::asio::io_context io_context;
  UnixSocketServer server(io_context, socket_path, app_state);
  auto server_ptr = std::make_shared<UnixSocketServer>(server);
  // The socket now exists; by default its permissions come from the umask (root-only when Wolf runs as root)
  set_socket_permissions(socket_path, utils::get_env("WOLF_SOCKET_GROUP"), utils::get_env("WOLF_SOCKET_MODE"));

  auto global_ev_handler = app_state->event_bus->register_global_handler([server_ptr](events::EventsVariant ev) {
    std::visit(
        [server_ptr](auto &&arg) {
          const auto event_type = rfl::type_name_t<std::decay_t<decltype(*arg)>>().str();
          server_ptr->broadcast_event(event_type, rfl::json::write(*arg));
        },
        ev);
  });

  io_context.run();
}

} // namespace wolf::api