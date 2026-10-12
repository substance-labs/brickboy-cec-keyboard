// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * brickboy-cec-keyboard
 *
 * GPL helper that converts HDMI-CEC remote keys into Linux uinput events
 * and publishes non-key CEC observations over a simple JSON-lines socket.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#endif

#if defined(BRICKBOY_CEC_WITH_LIBCEC) && BRICKBOY_CEC_WITH_LIBCEC
#include <cec.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

constexpr int CEC_OPCODE_USER_CONTROL_PRESSED_BB = 0x44;
constexpr int CEC_OPCODE_USER_CONTROL_RELEASE_BB = 0x45;
constexpr int CEC_OPCODE_STANDBY_BB = 0x36;
constexpr int CEC_OPCODE_REPORT_POWER_STATUS_BB = 0x90;
constexpr int CEC_OPCODE_DEVICE_VENDOR_ID_BB = 0x87;
constexpr int CEC_OPCODE_VENDOR_REMOTE_BUTTON_UP_BB = 0x8b;
constexpr int CEC_OPCODE_REPORT_PHYSICAL_ADDRESS_BB = 0x84;
constexpr int CEC_OPCODE_SET_SYSTEM_AUDIO_MODE_BB = 0x72;
constexpr int CEC_OPCODE_SYSTEM_AUDIO_MODE_STATUS_BB = 0x7e;
constexpr int CEC_OPCODE_ACTIVE_SOURCE_BB = 0x82;

constexpr int CEC_KEY_SELECT_BB = 0x00;
constexpr int CEC_KEY_UP_BB = 0x01;
constexpr int CEC_KEY_DOWN_BB = 0x02;
constexpr int CEC_KEY_LEFT_BB = 0x03;
constexpr int CEC_KEY_RIGHT_BB = 0x04;
constexpr int CEC_KEY_EXIT_BB = 0x0d;
constexpr int CEC_KEY_CLEAR_BB = 0x2c;
constexpr int CEC_KEY_ENTER_BB = 0x2b;
constexpr int CEC_KEY_PLAY_BB = 0x44;
constexpr int CEC_KEY_PAUSE_BB = 0x46;
constexpr int CEC_KEY_REWIND_BB = 0x48;
constexpr int CEC_KEY_FAST_FORWARD_BB = 0x49;
constexpr int CEC_KEY_FORWARD_BB = 0x4b;
constexpr int CEC_KEY_BACKWARD_BB = 0x4c;
constexpr int CEC_KEY_PLAY_FUNCTION_BB = 0x60;
constexpr int CEC_KEY_PAUSE_PLAY_FUNCTION_BB = 0x61;
constexpr int CEC_KEY_F1_BLUE_BB = 0x71;
constexpr int CEC_KEY_F2_RED_BB = 0x72;
constexpr int CEC_KEY_F3_GREEN_BB = 0x73;
constexpr int CEC_KEY_F4_YELLOW_BB = 0x74;
constexpr int CEC_KEY_AN_RETURN_BB = 0x91;

#ifndef KEY_UP
constexpr int KEY_UP = 103;
#endif
#ifndef KEY_DOWN
constexpr int KEY_DOWN = 108;
#endif
#ifndef KEY_LEFT
constexpr int KEY_LEFT = 105;
#endif
#ifndef KEY_RIGHT
constexpr int KEY_RIGHT = 106;
#endif
#ifndef KEY_Q
constexpr int KEY_Q = 16;
#endif
#ifndef KEY_W
constexpr int KEY_W = 17;
#endif
#ifndef KEY_A
constexpr int KEY_A = 30;
#endif
#ifndef KEY_B
constexpr int KEY_B = 48;
#endif
#ifndef KEY_Z
constexpr int KEY_Z = 44;
#endif
#ifndef KEY_X
constexpr int KEY_X = 45;
#endif
#ifndef KEY_ENTER
constexpr int KEY_ENTER = 28;
#endif
#ifndef KEY_KPENTER
constexpr int KEY_KPENTER = 96;
#endif
#ifndef KEY_BACKSPACE
constexpr int KEY_BACKSPACE = 14;
#endif
#ifndef KEY_DELETE
constexpr int KEY_DELETE = 111;
#endif
#ifndef KEY_LEFTMETA
constexpr int KEY_LEFTMETA = 125;
#endif
#ifndef KEY_MENU
constexpr int KEY_MENU = 139;
#endif
#ifndef BTN_SELECT
constexpr int BTN_SELECT = 0x13a;
#endif
#ifndef BTN_START
constexpr int BTN_START = 0x13b;
#endif
#ifndef BTN_MODE
constexpr int BTN_MODE = 0x13c;
#endif

struct KeyMapping {
  int cec_key;
  std::string cec_name;
  int linux_key;
  std::string linux_name;
};

struct Options {
  bool dry_run = false;
  bool self_test = false;
  bool socket_enabled = true;
  bool control_socket_enabled = true;
  bool activate_source = false;
  bool address_probe_enabled = true;
  bool force_address_probe = false;
  bool verbose = false;
  int release_timeout_ms = -1;
  int activate_source_delay_ms = 2000;
  int address_probe_interval_ms = 8000;
  std::optional<uint8_t> hdmi_port;
  std::optional<uint16_t> physical_address;
  std::string socket_path = "/run/brickboy-cec.sock";
  std::string control_socket_path = "/run/brickboy-cec-control.sock";
  std::string port;
  std::vector<KeyMapping> mappings;
};

std::optional<KeyMapping> mapping_for_cec_key(const std::vector<KeyMapping> &mappings,
                                              int cec_key) {
  auto it = std::find_if(mappings.begin(), mappings.end(), [&](const KeyMapping &mapping) {
    return mapping.cec_key == cec_key;
  });
  if (it == mappings.end()) {
    return std::nullopt;
  }
  return *it;
}

std::string hex_byte(int value) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << (value & 0xff);
  return out.str();
}

std::string hex_word(int value) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(4) << std::setfill('0') << (value & 0xffff);
  return out.str();
}

struct NamedCode {
  const char *name;
  int code;
};

struct ParsedCode {
  int code;
  std::string name;
};

std::string normalized_name(const std::string &value, bool uppercase) {
  std::string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (ch == '-' || ch == ' ' || ch == '.') {
      normalized.push_back('_');
    } else if (uppercase) {
      normalized.push_back(static_cast<char>(std::toupper(ch)));
    } else {
      normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return normalized;
}

const std::vector<NamedCode> &cec_key_names() {
  static const std::vector<NamedCode> names = {
      {"select", CEC_KEY_SELECT_BB},
      {"up", CEC_KEY_UP_BB},
      {"down", CEC_KEY_DOWN_BB},
      {"left", CEC_KEY_LEFT_BB},
      {"right", CEC_KEY_RIGHT_BB},
      {"exit", CEC_KEY_EXIT_BB},
      {"back", CEC_KEY_EXIT_BB},
      {"clear", CEC_KEY_CLEAR_BB},
      {"enter", CEC_KEY_ENTER_BB},
      {"play", CEC_KEY_PLAY_BB},
      {"pause", CEC_KEY_PAUSE_BB},
      {"rewind", CEC_KEY_REWIND_BB},
      {"fast_forward", CEC_KEY_FAST_FORWARD_BB},
      {"forward", CEC_KEY_FORWARD_BB},
      {"backward", CEC_KEY_BACKWARD_BB},
      {"play_function", CEC_KEY_PLAY_FUNCTION_BB},
      {"pause_play_function", CEC_KEY_PAUSE_PLAY_FUNCTION_BB},
      {"pause_play", CEC_KEY_PAUSE_PLAY_FUNCTION_BB},
      {"blue", CEC_KEY_F1_BLUE_BB},
      {"f1_blue", CEC_KEY_F1_BLUE_BB},
      {"red", CEC_KEY_F2_RED_BB},
      {"f2_red", CEC_KEY_F2_RED_BB},
      {"green", CEC_KEY_F3_GREEN_BB},
      {"f3_green", CEC_KEY_F3_GREEN_BB},
      {"yellow", CEC_KEY_F4_YELLOW_BB},
      {"f4_yellow", CEC_KEY_F4_YELLOW_BB},
      {"samsung_return", CEC_KEY_AN_RETURN_BB},
      {"an_return", CEC_KEY_AN_RETURN_BB},
  };
  return names;
}

const std::vector<NamedCode> &linux_key_names() {
  static const std::vector<NamedCode> names = {
      {"KEY_UP", KEY_UP},
      {"KEY_DOWN", KEY_DOWN},
      {"KEY_LEFT", KEY_LEFT},
      {"KEY_RIGHT", KEY_RIGHT},
      {"KEY_Q", KEY_Q},
      {"KEY_W", KEY_W},
      {"KEY_A", KEY_A},
      {"KEY_B", KEY_B},
      {"KEY_Z", KEY_Z},
      {"KEY_X", KEY_X},
      {"KEY_ENTER", KEY_ENTER},
      {"KEY_KPENTER", KEY_KPENTER},
      {"KEY_BACKSPACE", KEY_BACKSPACE},
      {"KEY_DELETE", KEY_DELETE},
      {"KEY_LEFTMETA", KEY_LEFTMETA},
      {"KEY_MENU", KEY_MENU},
      {"BTN_SELECT", BTN_SELECT},
      {"BTN_START", BTN_START},
      {"BTN_MODE", BTN_MODE},
  };
  return names;
}

std::optional<int> parse_integer_code(const std::string &value, bool cec_hex_byte_default) {
  if (value.empty()) {
    return std::nullopt;
  }

  int base = 10;
  std::string text = value;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
  } else if (cec_hex_byte_default && text.size() <= 2 &&
             std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isxdigit(ch);
             })) {
    base = 16;
  }

  try {
    size_t parsed = 0;
    int result = std::stoi(text, &parsed, base);
    if (parsed != text.size()) {
      return std::nullopt;
    }
    return result;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<ParsedCode> find_named_code(const std::vector<NamedCode> &names,
                                          const std::string &name) {
  auto it = std::find_if(names.begin(), names.end(), [&](const NamedCode &entry) {
    return name == entry.name;
  });
  if (it == names.end()) {
    return std::nullopt;
  }
  return ParsedCode{it->code, it->name};
}

std::optional<std::string> known_name_for_code(const std::vector<NamedCode> &names, int code) {
  auto it = std::find_if(names.begin(), names.end(), [&](const NamedCode &entry) {
    return code == entry.code;
  });
  if (it == names.end()) {
    return std::nullopt;
  }
  return std::string(it->name);
}

std::optional<ParsedCode> parse_cec_key_spec(const std::string &value) {
  std::string normalized = normalized_name(value, false);
  if (auto named = find_named_code(cec_key_names(), normalized)) {
    return named;
  }

  auto code = parse_integer_code(value, true);
  if (!code || *code < 0 || *code > 0xff) {
    return std::nullopt;
  }
  auto known = known_name_for_code(cec_key_names(), *code);
  return ParsedCode{*code, known ? *known : hex_byte(*code)};
}

std::optional<ParsedCode> parse_linux_key_spec(const std::string &value) {
  std::string normalized = normalized_name(value, true);
  if (normalized.size() == 1 && std::isalpha(static_cast<unsigned char>(normalized[0]))) {
    normalized = "KEY_" + normalized;
  }

  if (auto named = find_named_code(linux_key_names(), normalized)) {
    return named;
  }
  if (normalized.find("KEY_") != 0 && normalized.find("BTN_") != 0) {
    if (auto named = find_named_code(linux_key_names(), "KEY_" + normalized)) {
      return named;
    }
    if (auto named = find_named_code(linux_key_names(), "BTN_" + normalized)) {
      return named;
    }
  }

  auto code = parse_integer_code(value, false);
  if (!code || *code < 0) {
    return std::nullopt;
  }
  auto known = known_name_for_code(linux_key_names(), *code);
  return ParsedCode{*code, known ? *known : std::to_string(*code)};
}

std::optional<KeyMapping> parse_mapping_spec(const std::string &value, std::string &error) {
  auto equals = value.find('=');
  if (equals == std::string::npos || equals == 0 || equals + 1 >= value.size()) {
    error = "expected CEC_KEY=LINUX_KEY";
    return std::nullopt;
  }

  std::string cec_text = value.substr(0, equals);
  std::string linux_text = value.substr(equals + 1);
  auto cec = parse_cec_key_spec(cec_text);
  if (!cec) {
    error = "unknown CEC key '" + cec_text + "'";
    return std::nullopt;
  }
  auto linux_key = parse_linux_key_spec(linux_text);
  if (!linux_key) {
    error = "unknown Linux input key '" + linux_text + "'";
    return std::nullopt;
  }

  return KeyMapping{cec->code, cec->name, linux_key->code, linux_key->name};
}

void set_mapping(std::vector<KeyMapping> &mappings, const KeyMapping &mapping) {
  auto it = std::find_if(mappings.begin(), mappings.end(), [&](const KeyMapping &existing) {
    return existing.cec_key == mapping.cec_key;
  });
  if (it == mappings.end()) {
    mappings.push_back(mapping);
  } else {
    *it = mapping;
  }
}

std::optional<int> parse_hex_nibble(char ch) {
  unsigned char uch = static_cast<unsigned char>(ch);
  if (uch >= '0' && uch <= '9') {
    return uch - '0';
  }
  uch = static_cast<unsigned char>(std::tolower(uch));
  if (uch >= 'a' && uch <= 'f') {
    return 10 + uch - 'a';
  }
  return std::nullopt;
}

std::optional<uint16_t> parse_physical_address(const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }

  uint16_t result = 0;
  if (value.find('.') != std::string::npos) {
    int nibbles = 0;
    std::string part;
    std::istringstream input(value);
    while (std::getline(input, part, '.')) {
      if (part.size() != 1) {
        return std::nullopt;
      }
      auto nibble = parse_hex_nibble(part[0]);
      if (!nibble) {
        return std::nullopt;
      }
      result = static_cast<uint16_t>((result << 4) | *nibble);
      ++nibbles;
    }
    return nibbles == 4 ? std::optional<uint16_t>(result) : std::nullopt;
  }

  std::string text = value;
  if (text.size() == 6 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text = text.substr(2);
  }
  if (text.size() != 4) {
    return std::nullopt;
  }
  for (char ch : text) {
    auto nibble = parse_hex_nibble(ch);
    if (!nibble) {
      return std::nullopt;
    }
    result = static_cast<uint16_t>((result << 4) | *nibble);
  }
  return result;
}

bool is_valid_playback_physical_address(uint16_t physical) {
  return physical != 0x0000 && physical != 0xffff;
}

struct EdidPhysicalAddress {
  uint16_t physical;
  std::string source;
};

std::vector<uint8_t> read_binary_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::string value;
  std::getline(input, value);
  return value;
}

std::optional<uint16_t> parse_edid_physical_address(const std::vector<uint8_t> &edid) {
  static constexpr uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  if (edid.size() < 128 || !std::equal(std::begin(header), std::end(header), edid.begin())) {
    return std::nullopt;
  }

  int extension_count = edid[0x7e];
  for (int extension = 0; extension < extension_count; ++extension) {
    size_t offset = 128 * static_cast<size_t>(extension + 1);
    if (offset + 128 > edid.size() || edid[offset] != 0x02) {
      continue;
    }

    int data_end = edid[offset + 2];
    if (data_end <= 4 || data_end > 127) {
      data_end = 127;
    }

    size_t pos = offset + 4;
    size_t end = offset + static_cast<size_t>(data_end);
    while (pos < end) {
      uint8_t block_header = edid[pos++];
      int tag = block_header >> 5;
      int length = block_header & 0x1f;
      if (pos + static_cast<size_t>(length) > offset + 128) {
        break;
      }

      const uint8_t *payload = edid.data() + pos;
      if (tag == 3 && length >= 5 && payload[0] == 0x03 && payload[1] == 0x0c &&
          payload[2] == 0x00) {
        uint16_t physical = static_cast<uint16_t>((payload[3] << 8) | payload[4]);
        if (is_valid_playback_physical_address(physical)) {
          return physical;
        }
      }
      pos += static_cast<size_t>(length);
    }
  }

  return std::nullopt;
}

[[maybe_unused]] std::optional<EdidPhysicalAddress> find_edid_physical_address() {
  DIR *dir = ::opendir("/sys/class/drm");
  if (!dir) {
    return std::nullopt;
  }

  std::optional<EdidPhysicalAddress> result;
  while (dirent *entry = ::readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == ".." || name.find("HDMI") == std::string::npos) {
      continue;
    }

    std::string root = "/sys/class/drm/" + name;
    std::string status = read_text_file(root + "/status");
    if (!status.empty() && status != "connected") {
      continue;
    }

    std::vector<uint8_t> edid = read_binary_file(root + "/edid");
    auto physical = parse_edid_physical_address(edid);
    if (physical) {
      result = EdidPhysicalAddress{*physical, name};
      break;
    }
  }

  ::closedir(dir);
  return result;
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
      } else {
        out << ch;
      }
    }
  }
  return out.str();
}

std::string json_string(const std::string &value) {
  return "\"" + json_escape(value) + "\"";
}

const char *opcode_name(int opcode) {
  switch (opcode) {
  case CEC_OPCODE_USER_CONTROL_PRESSED_BB:
    return "user_control_pressed";
  case CEC_OPCODE_USER_CONTROL_RELEASE_BB:
    return "user_control_release";
  case CEC_OPCODE_STANDBY_BB:
    return "standby";
  case CEC_OPCODE_REPORT_POWER_STATUS_BB:
    return "report_power_status";
  case CEC_OPCODE_DEVICE_VENDOR_ID_BB:
    return "device_vendor_id";
  case CEC_OPCODE_VENDOR_REMOTE_BUTTON_UP_BB:
    return "vendor_remote_button_up";
  case CEC_OPCODE_REPORT_PHYSICAL_ADDRESS_BB:
    return "report_physical_address";
  case CEC_OPCODE_SET_SYSTEM_AUDIO_MODE_BB:
    return "set_system_audio_mode";
  case CEC_OPCODE_SYSTEM_AUDIO_MODE_STATUS_BB:
    return "system_audio_mode_status";
  case CEC_OPCODE_ACTIVE_SOURCE_BB:
    return "active_source";
  default:
    return "unknown";
  }
}

const char *logical_name(int address) {
  switch (address) {
  case 0:
    return "tv";
  case 1:
    return "recording1";
  case 2:
    return "recording2";
  case 3:
    return "tuner1";
  case 4:
    return "playback1";
  case 5:
    return "audio";
  case 6:
    return "tuner2";
  case 7:
    return "tuner3";
  case 8:
    return "playback2";
  case 9:
    return "recording3";
  case 10:
    return "tuner4";
  case 11:
    return "playback3";
  case 14:
    return "freeuse";
  case 15:
    return "broadcast";
  default:
    return "unknown";
  }
}

const char *power_status_name(int status) {
  switch (status) {
  case 0x00:
    return "on";
  case 0x01:
    return "standby";
  case 0x02:
    return "standby_to_on";
  case 0x03:
    return "on_to_standby";
  default:
    return "unknown";
  }
}

const char *system_audio_mode_name(int mode) {
  switch (mode) {
  case 0:
    return "off";
  case 1:
    return "on";
  default:
    return "unknown";
  }
}

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

std::string trim_ascii(const std::string &value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::optional<std::string> json_string_field(const std::string &json, const std::string &field) {
  std::string needle = "\"" + field + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') {
    return std::nullopt;
  }
  ++pos;

  std::string value;
  while (pos < json.size()) {
    char ch = json[pos++];
    if (ch == '"') {
      return value;
    }
    if (ch != '\\') {
      value.push_back(ch);
      continue;
    }
    if (pos >= json.size()) {
      return std::nullopt;
    }
    char escaped = json[pos++];
    switch (escaped) {
    case '"':
    case '\\':
    case '/':
      value.push_back(escaped);
      break;
    case 'n':
      value.push_back('\n');
      break;
    case 'r':
      value.push_back('\r');
      break;
    case 't':
      value.push_back('\t');
      break;
    default:
      return std::nullopt;
    }
  }
  return std::nullopt;
}

[[maybe_unused]] std::optional<std::string> parse_control_command_name(const std::string &line) {
  std::string trimmed = trim_ascii(line);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (trimmed.front() == '{') {
    auto cmd = json_string_field(trimmed, "cmd");
    if (!cmd) {
      return std::nullopt;
    }
    return trim_ascii(*cmd);
  }
  return trimmed;
}

class SocketPublisher {
public:
  SocketPublisher() = default;
  SocketPublisher(const SocketPublisher &) = delete;
  SocketPublisher &operator=(const SocketPublisher &) = delete;

  ~SocketPublisher() {
    close_all();
  }

  bool start(const std::string &path) {
    path_ = path;
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      std::cerr << "socket: " << std::strerror(errno) << "\n";
      return false;
    }

    set_nonblocking(server_fd_);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      std::cerr << "socket path too long: " << path << "\n";
      return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    (void)::unlink(path.c_str());

    if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      std::cerr << "bind " << path << ": " << std::strerror(errno) << "\n";
      return false;
    }
    (void)::chmod(path.c_str(), 0666);

    if (::listen(server_fd_, 8) < 0) {
      std::cerr << "listen " << path << ": " << std::strerror(errno) << "\n";
      return false;
    }
    return true;
  }

  void publish(const std::string &json_line) {
    accept_pending();
    if (clients_.empty()) {
      return;
    }

    std::string line = json_line;
    line.push_back('\n');

    for (auto it = clients_.begin(); it != clients_.end();) {
#ifdef MSG_NOSIGNAL
      ssize_t written = ::send(*it, line.data(), line.size(), MSG_NOSIGNAL);
#else
      ssize_t written = ::send(*it, line.data(), line.size(), 0);
#endif
      if (written < 0) {
        ::close(*it);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void retain(const std::string &key, const std::string &json_line) {
    auto it = std::find_if(retained_.begin(), retained_.end(), [&](const auto &entry) {
      return entry.first == key;
    });
    if (it == retained_.end()) {
      retained_.push_back({key, json_line});
      return;
    }
    it->second = json_line;
  }

  void accept_pending() {
    if (server_fd_ < 0) {
      return;
    }
    for (;;) {
      int client = ::accept(server_fd_, nullptr, nullptr);
      if (client < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          std::cerr << "accept: " << std::strerror(errno) << "\n";
        }
        return;
      }
      set_nonblocking(client);
      send_retained(client);
      clients_.push_back(client);
    }
  }

private:
  bool send_line(int client, const std::string &json_line) {
    std::string line = json_line;
    line.push_back('\n');
#ifdef MSG_NOSIGNAL
    ssize_t written = ::send(client, line.data(), line.size(), MSG_NOSIGNAL);
#else
    ssize_t written = ::send(client, line.data(), line.size(), 0);
#endif
    return written >= 0;
  }

  void send_retained(int client) {
    for (const auto &entry : retained_) {
      if (!send_line(client, entry.second)) {
        return;
      }
    }
  }

  void close_all() {
    for (int client : clients_) {
      ::close(client);
    }
    clients_.clear();
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    if (!path_.empty()) {
      (void)::unlink(path_.c_str());
    }
  }

  int server_fd_ = -1;
  std::string path_;
  std::vector<int> clients_;
  std::vector<std::pair<std::string, std::string>> retained_;
};

class ControlSocketServer {
public:
  using Handler = std::function<std::string(const std::string &)>;

  ControlSocketServer() = default;
  ControlSocketServer(const ControlSocketServer &) = delete;
  ControlSocketServer &operator=(const ControlSocketServer &) = delete;

  ~ControlSocketServer() {
    close_all();
  }

  bool start(const std::string &path) {
    path_ = path;
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
      std::cerr << "control socket: " << std::strerror(errno) << "\n";
      return false;
    }

    set_nonblocking(server_fd_);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      std::cerr << "control socket path too long: " << path << "\n";
      return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    (void)::unlink(path.c_str());

    if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      std::cerr << "bind " << path << ": " << std::strerror(errno) << "\n";
      return false;
    }
    (void)::chmod(path.c_str(), 0666);

    if (::listen(server_fd_, 8) < 0) {
      std::cerr << "listen " << path << ": " << std::strerror(errno) << "\n";
      return false;
    }
    return true;
  }

  void poll(const Handler &handler) {
    accept_pending();
    read_clients(handler);
  }

private:
  struct Client {
    int fd;
    std::string buffer;
  };

  void accept_pending() {
    if (server_fd_ < 0) {
      return;
    }
    for (;;) {
      int client = ::accept(server_fd_, nullptr, nullptr);
      if (client < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          std::cerr << "control accept: " << std::strerror(errno) << "\n";
        }
        return;
      }
      set_nonblocking(client);
      clients_.push_back(Client{client, ""});
    }
  }

  bool send_line(int client, const std::string &json_line) {
    std::string line = json_line;
    line.push_back('\n');
#ifdef MSG_NOSIGNAL
    ssize_t written = ::send(client, line.data(), line.size(), MSG_NOSIGNAL);
#else
    ssize_t written = ::send(client, line.data(), line.size(), 0);
#endif
    return written >= 0;
  }

  void process_lines(Client &client, const Handler &handler) {
    for (;;) {
      size_t newline = client.buffer.find('\n');
      if (newline == std::string::npos) {
        return;
      }
      std::string line = client.buffer.substr(0, newline);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      client.buffer.erase(0, newline + 1);
      std::string trimmed = trim_ascii(line);
      if (trimmed.empty()) {
        continue;
      }
      if (!send_line(client.fd, handler(trimmed))) {
        return;
      }
    }
  }

  void read_clients(const Handler &handler) {
    char buffer[512];
    for (auto it = clients_.begin(); it != clients_.end();) {
      bool close_client = false;
      for (;;) {
        ssize_t read_count = ::recv(it->fd, buffer, sizeof(buffer), 0);
        if (read_count > 0) {
          it->buffer.append(buffer, static_cast<size_t>(read_count));
          process_lines(*it, handler);
          if (it->buffer.size() > 4096) {
            (void)send_line(it->fd, "{\"type\":\"control_response\",\"ok\":false,\"error\":\"command too long\"}");
            close_client = true;
            break;
          }
          continue;
        }
        if (read_count == 0) {
          close_client = true;
          break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        close_client = true;
        break;
      }

      if (close_client) {
        ::close(it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void close_all() {
    for (const auto &client : clients_) {
      ::close(client.fd);
    }
    clients_.clear();
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    if (!path_.empty()) {
      (void)::unlink(path_.c_str());
    }
  }

  int server_fd_ = -1;
  std::string path_;
  std::vector<Client> clients_;
};

class InputEmitter {
public:
  InputEmitter() = default;
  InputEmitter(const InputEmitter &) = delete;
  InputEmitter &operator=(const InputEmitter &) = delete;

  ~InputEmitter() {
    close_device();
  }

  bool open_device(bool dry_run, const std::vector<KeyMapping> &mappings) {
#ifndef __linux__
    (void)mappings;
#endif
    dry_run_ = dry_run;
    if (dry_run_) {
      std::cout << "dry-run input device enabled\n";
      return true;
    }

#ifndef __linux__
    std::cerr << "/dev/uinput is only available on Linux; use --dry-run here\n";
    return false;
#else
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
      std::cerr << "open /dev/uinput: " << std::strerror(errno) << "\n";
      return false;
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0) {
      std::cerr << "uinput set event bits: " << std::strerror(errno) << "\n";
      return false;
    }

    for (const auto &mapping : mappings) {
      if (ioctl(fd_, UI_SET_KEYBIT, mapping.linux_key) < 0) {
        std::cerr << "uinput set key bit " << mapping.linux_name << ": " << std::strerror(errno)
                  << "\n";
        return false;
      }
    }

    uinput_setup setup{};
    std::snprintf(setup.name, sizeof(setup.name), "Brickboy CEC Keyboard");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x2708;
    setup.id.product = 0x1001;
    setup.id.version = 1;

    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) {
      std::cerr << "uinput setup: " << std::strerror(errno) << "\n";
      return false;
    }
    if (ioctl(fd_, UI_DEV_CREATE) < 0) {
      std::cerr << "uinput create: " << std::strerror(errno) << "\n";
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return true;
#endif
  }

  bool emit_key(int linux_key, int value, const char *linux_name) {
    if (dry_run_) {
      std::cout << "input " << linux_name << " value=" << value << "\n";
      return true;
    }

#ifndef __linux__
    (void)linux_key;
    (void)value;
    (void)linux_name;
    return false;
#else
    input_event event{};
    event.type = EV_KEY;
    event.code = static_cast<unsigned short>(linux_key);
    event.value = value;
    if (::write(fd_, &event, sizeof(event)) != sizeof(event)) {
      std::cerr << "write key event: " << std::strerror(errno) << "\n";
      return false;
    }

    input_event sync{};
    sync.type = EV_SYN;
    sync.code = SYN_REPORT;
    sync.value = 0;
    if (::write(fd_, &sync, sizeof(sync)) != sizeof(sync)) {
      std::cerr << "write sync event: " << std::strerror(errno) << "\n";
      return false;
    }
    return true;
#endif
  }

private:
  void close_device() {
#ifdef __linux__
    if (fd_ >= 0) {
      (void)ioctl(fd_, UI_DEV_DESTROY);
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

  bool dry_run_ = false;
#ifdef __linux__
  int fd_ = -1;
#endif
};

class CecKeyboardApp {
public:
  explicit CecKeyboardApp(Options options) : options_(std::move(options)) {}

  bool start_io() {
    if (!input_.open_device(options_.dry_run, options_.mappings)) {
      return false;
    }
    if (options_.socket_enabled && !socket_.start(options_.socket_path)) {
      return false;
    }
    publish_retained("ready", "{\"type\":\"ready\"}");
    return true;
  }

  void tick() {
    socket_.accept_pending();
    if (!active_key_ || options_.release_timeout_ms < 0) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - active_key_->last_seen);
    if (elapsed.count() >= options_.release_timeout_ms) {
      release_active_key("timeout");
    }
  }

  void handle_command(int from, int to, int opcode, const std::vector<uint8_t> &params) {
    publish_raw(from, to, opcode, params);

    switch (opcode) {
    case CEC_OPCODE_USER_CONTROL_PRESSED_BB:
      if (!params.empty()) {
        handle_key_pressed(params[0]);
      }
      break;
    case CEC_OPCODE_USER_CONTROL_RELEASE_BB:
      release_active_key("cec_release");
      break;
    case CEC_OPCODE_VENDOR_REMOTE_BUTTON_UP_BB:
      release_active_key("vendor_remote_button_up");
      break;
    case CEC_OPCODE_STANDBY_BB:
      publish("{\"type\":\"power\",\"source\":\"" + std::string(logical_name(from)) +
              "\",\"state\":\"standby\"}");
      if (from == 0) {
        publish_display_status(std::optional<bool>{true}, "standby", "standby", std::nullopt);
      }
      break;
    case CEC_OPCODE_REPORT_POWER_STATUS_BB:
      if (!params.empty()) {
        std::string state = power_status_name(params[0]);
        publish("{\"type\":\"power\",\"source\":\"" + std::string(logical_name(from)) +
                "\",\"state\":\"" + state + "\"}");
        if (from == 0) {
          publish_display_status(std::optional<bool>{true}, "report_power_status", state,
                                 std::nullopt);
        }
      }
      break;
    case CEC_OPCODE_DEVICE_VENDOR_ID_BB:
      if (params.size() >= 3) {
        std::string vendor = hex_byte(params[0]) + hex_byte(params[1]) + hex_byte(params[2]);
        publish("{\"type\":\"device\",\"logical\":" + std::to_string(from) + ",\"role\":\"" +
                logical_name(from) + "\",\"vendor_id\":\"" + vendor + "\"}");
        if (from == 0) {
          publish_display_status(std::optional<bool>{true}, "device_vendor_id", std::nullopt,
                                 vendor);
        }
      }
      break;
    case CEC_OPCODE_REPORT_PHYSICAL_ADDRESS_BB:
      if (params.size() >= 3) {
        int physical = (params[0] << 8) | params[1];
        publish("{\"type\":\"device\",\"logical\":" + std::to_string(from) +
                ",\"physical_address\":\"" + hex_word(physical) + "\",\"device_type\":" +
                std::to_string(params[2]) + "}");
        if (from == 0) {
          publish_display_status(std::optional<bool>{true}, "report_physical_address",
                                 std::nullopt, std::nullopt);
        }
      }
      break;
    case CEC_OPCODE_SET_SYSTEM_AUDIO_MODE_BB:
    case CEC_OPCODE_SYSTEM_AUDIO_MODE_STATUS_BB:
      if (!params.empty()) {
        publish("{\"type\":\"audio\",\"source\":\"" + std::string(logical_name(from)) +
                "\",\"system_audio_mode\":\"" + system_audio_mode_name(params[0]) + "\"}");
      }
      break;
    case CEC_OPCODE_ACTIVE_SOURCE_BB:
      if (params.size() >= 2) {
        int physical = (params[0] << 8) | params[1];
        publish_active_source(from, static_cast<uint16_t>(physical), "bus");
      }
      break;
    default:
      break;
    }
  }

  void publish_alert(const std::string &alert) {
    publish("{\"type\":\"alert\",\"name\":" + json_string(alert) + "}");
  }

  void publish_display_status(std::optional<bool> connected, const std::string &reason,
                              std::optional<std::string> power_state = std::nullopt,
                              std::optional<std::string> vendor_id = std::nullopt) {
    if (connected) {
      display_connected_ = *connected;
      display_status_known_ = true;
      if (!*connected) {
        display_power_state_.clear();
      }
    }
    if (power_state) {
      display_power_state_ = *power_state;
    }
    if (vendor_id) {
      display_vendor_id_ = *vendor_id;
    }

    std::string event = "{\"type\":\"display\",\"source\":\"tv\"";
    if (display_status_known_) {
      event += ",\"connected\":";
      event += display_connected_ ? "true" : "false";
    }
    if (!display_power_state_.empty()) {
      event += ",\"power_state\":" + json_string(display_power_state_);
    }
    if (!display_vendor_id_.empty()) {
      event += ",\"vendor_id\":\"" + display_vendor_id_ + "\"";
    }
    event += ",\"reason\":" + json_string(reason) + "}";
    publish_retained("display", event);
  }

  void publish_log(const std::string &level, const std::string &message) {
    if (!options_.verbose) {
      return;
    }
    publish("{\"type\":\"libcec_log\",\"level\":" + json_string(level) + ",\"message\":" +
            json_string(message) + "}");
  }

  void publish_raw_keypress(int cec_key, unsigned int duration_ms) {
    publish("{\"type\":\"raw_keypress\",\"cec_code\":\"" + hex_byte(cec_key) +
            "\",\"duration_ms\":" + std::to_string(duration_ms) + "}");
  }

  void publish_menu_state(const std::string &state) {
    publish("{\"type\":\"menu_state\",\"state\":" + json_string(state) + "}");
  }

  void publish_cec_address(const std::string &action, uint16_t physical,
                           const std::string &source) {
    publish_retained("cec_address",
                     "{\"type\":\"cec_address\",\"action\":" + json_string(action) +
                         ",\"physical_address\":\"" + hex_word(physical) +
                         "\",\"source\":" + json_string(source) + "}");
  }

  void publish_active_source(int logical, uint16_t physical, const std::string &reason) {
    std::string event = "{\"type\":\"active_source\",\"source\":" +
                        json_string(logical_name(logical)) +
                        ",\"logical\":" + std::to_string(logical) +
                        ",\"physical_address\":\"" + hex_word(physical) + "\"";
    if (!reason.empty()) {
      event += ",\"reason\":" + json_string(reason);
    }
    event += "}";
    publish(event);
  }

  void publish_control_result(const std::string &command, bool ok, const std::string &message) {
    std::string event = "{\"type\":\"control\",\"cmd\":" + json_string(command) + ",\"ok\":";
    event += ok ? "true" : "false";
    if (!message.empty()) {
      event += ",\"message\":" + json_string(message);
    }
    event += "}";
    publish(event);
  }

  bool cec_key_seen() const {
    return cec_key_seen_.load();
  }

  void handle_key_pressed(int cec_key) {
    cec_key_seen_ = true;
    auto mapping = mapping_for_cec_key(options_.mappings, cec_key);
    if (!mapping) {
      publish("{\"type\":\"key\",\"action\":\"unmapped\",\"cec_code\":\"" + hex_byte(cec_key) +
              "\"}");
      return;
    }

    auto now = std::chrono::steady_clock::now();
    if (active_key_ && active_key_->mapping.linux_key != mapping->linux_key) {
      release_active_key("superseded");
    }

    if (active_key_ && active_key_->mapping.linux_key == mapping->linux_key) {
      active_key_->last_seen = now;
      (void)input_.emit_key(mapping->linux_key, 2, mapping->linux_name.c_str());
      publish_key_event("repeat", *mapping, "");
      return;
    }

    active_key_ = ActiveKey{*mapping, now};
    (void)input_.emit_key(mapping->linux_key, 1, mapping->linux_name.c_str());
    publish_key_event("down", *mapping, "");
  }

private:
  struct ActiveKey {
    KeyMapping mapping;
    std::chrono::steady_clock::time_point last_seen;
  };

  void release_active_key(const std::string &reason) {
    if (!active_key_) {
      return;
    }
    KeyMapping mapping = active_key_->mapping;
    active_key_.reset();
    (void)input_.emit_key(mapping.linux_key, 0, mapping.linux_name.c_str());
    publish_key_event("up", mapping, reason);
  }

  void publish_key_event(const std::string &action, const KeyMapping &mapping,
                         const std::string &reason) {
    std::string event = "{\"type\":\"key\",\"action\":" + json_string(action) +
                        ",\"cec_key\":" + json_string(mapping.cec_name) +
                        ",\"cec_code\":\"" + hex_byte(mapping.cec_key) +
                        "\",\"linux_key\":" + json_string(mapping.linux_name);
    if (!reason.empty()) {
      event += ",\"reason\":" + json_string(reason);
    }
    event += "}";
    publish(event);
  }

  void publish_raw(int from, int to, int opcode, const std::vector<uint8_t> &params) {
    std::string event = "{\"type\":\"raw\",\"from\":" + std::to_string(from) + ",\"to\":" +
                        std::to_string(to) + ",\"from_name\":" + json_string(logical_name(from)) +
                        ",\"to_name\":" + json_string(logical_name(to)) + ",\"opcode\":\"" +
                        hex_byte(opcode) + "\",\"opcode_name\":" + json_string(opcode_name(opcode)) +
                        ",\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i != 0) {
        event += ",";
      }
      event += json_string(hex_byte(params[i]));
    }
    event += "]}";
    publish(event);
  }

  void publish(const std::string &json_line) {
    if (options_.socket_enabled) {
      socket_.publish(json_line);
    }
    if (options_.dry_run || options_.verbose) {
      std::cout << json_line << "\n";
    }
  }

  void publish_retained(const std::string &key, const std::string &json_line) {
    if (options_.socket_enabled) {
      socket_.retain(key, json_line);
    }
    publish(json_line);
  }

  Options options_;
  InputEmitter input_;
  SocketPublisher socket_;
  std::optional<ActiveKey> active_key_;
  bool display_status_known_ = false;
  bool display_connected_ = false;
  std::string display_power_state_;
  std::string display_vendor_id_;
  std::atomic<bool> cec_key_seen_{false};
};

Options parse_args(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const char *name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--self-test") {
      options.self_test = true;
      options.dry_run = true;
      options.socket_enabled = false;
      options.control_socket_enabled = false;
    } else if (arg == "--no-socket") {
      options.socket_enabled = false;
    } else if (arg == "--socket") {
      options.socket_path = need_value("--socket");
    } else if (arg == "--no-control-socket") {
      options.control_socket_enabled = false;
    } else if (arg == "--control-socket") {
      options.control_socket_path = need_value("--control-socket");
    } else if (arg == "--activate-source") {
      options.activate_source = true;
    } else if (arg == "--activate-source-delay") {
      options.activate_source_delay_ms = std::stoi(need_value("--activate-source-delay"));
    } else if (arg == "--no-address-probe") {
      options.address_probe_enabled = false;
    } else if (arg == "--probe-addresses") {
      options.force_address_probe = true;
      options.address_probe_enabled = true;
    } else if (arg == "--address-probe-interval") {
      options.address_probe_interval_ms = std::stoi(need_value("--address-probe-interval"));
      if (options.address_probe_interval_ms < 0) {
        std::cerr << "--address-probe-interval expects a non-negative value\n";
        std::exit(2);
      }
    } else if (arg == "--hdmi-port") {
      int port = std::stoi(need_value("--hdmi-port"));
      if (port < 1 || port > 15) {
        std::cerr << "--hdmi-port expects a value from 1 to 15\n";
        std::exit(2);
      }
      options.hdmi_port = static_cast<uint8_t>(port);
    } else if (arg == "--physical-address") {
      auto parsed = parse_physical_address(need_value("--physical-address"));
      if (!parsed) {
        std::cerr << "--physical-address expects 4 nibbles like 2000, 0x2000, or 2.0.0.0\n";
        std::exit(2);
      }
      options.physical_address = *parsed;
      if (!options.hdmi_port && (*parsed & 0x0fff) == 0) {
        uint8_t derived_port = static_cast<uint8_t>((*parsed >> 12) & 0x0f);
        if (derived_port >= 1 && derived_port <= 15) {
          options.hdmi_port = derived_port;
        }
      }
    } else if (arg == "--release-timeout") {
      options.release_timeout_ms = std::stoi(need_value("--release-timeout"));
    } else if (arg == "--map") {
      std::string value = need_value("--map");
      std::string error;
      auto mapping = parse_mapping_spec(value, error);
      if (!mapping) {
        std::cerr << "--map " << value << ": " << error << "\n";
        std::exit(2);
      }
      set_mapping(options.mappings, *mapping);
    } else if (arg == "--port") {
      options.port = need_value("--port");
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: brickboy-cec-keyboard [options]\n\n"
          << "  --activate-source        request active-source status on startup\n"
          << "  --activate-source-delay MS  delay active-source request, default 2000\n"
          << "  --probe-addresses        probe candidate CEC physical addresses even when EDID is present\n"
          << "  --no-address-probe       disable physical-address probing\n"
          << "  --address-probe-interval MS  delay between probe candidates, default 8000\n"
          << "  --hdmi-port PORT         override autodetected HDMI port, 1-15\n"
          << "  --physical-address ADDR  override autodetected CEC physical address, e.g. 2000 or 2.0.0.0\n"
          << "  --dry-run                log input events instead of opening /dev/uinput\n"
          << "  --self-test              run synthetic mapping test and exit\n"
          << "  --socket PATH            JSON-lines Unix socket path\n"
          << "  --no-socket              disable JSON-lines socket\n"
          << "  --control-socket PATH    JSON-lines command Unix socket path\n"
          << "  --no-control-socket      disable command socket\n"
          << "  --release-timeout MS     stuck-key release timeout, -1 disables, default -1\n"
          << "  --map CEC=LINUX          map CEC remote key to Linux input key; repeatable; required\n"
          << "  --port PORT              CEC port, default autodetect\n"
          << "  --verbose                print JSON events and libCEC logs to stdout\n";
      std::exit(0);
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      std::exit(2);
    }
  }
  if (options.mappings.empty()) {
    std::cerr << "at least one --map CEC=LINUX mapping is required\n";
    std::exit(2);
  }
  return options;
}

void on_signal(int) {
  g_stop = true;
}

int run_self_test(Options options) {
  auto mappings = options.mappings;
  CecKeyboardApp app(std::move(options));
  if (!app.start_io()) {
    return 1;
  }

  for (const auto &mapping : mappings) {
    std::cout << "self-test " << mapping.cec_name << " -> " << mapping.linux_name << "\n";
    app.handle_command(0, 4, CEC_OPCODE_USER_CONTROL_PRESSED_BB,
                       {static_cast<uint8_t>(mapping.cec_key)});
    app.handle_command(0, 4, CEC_OPCODE_VENDOR_REMOTE_BUTTON_UP_BB,
                       {static_cast<uint8_t>(mapping.cec_key)});
  }

  app.handle_command(0, 15, CEC_OPCODE_STANDBY_BB, {});
  app.handle_command(5, 15, CEC_OPCODE_SET_SYSTEM_AUDIO_MODE_BB, {0});
  app.handle_command(4, 15, CEC_OPCODE_DEVICE_VENDOR_ID_BB, {0x08, 0x00, 0x46});
  std::cout << "self-test ok\n";
  return 0;
}

#if defined(BRICKBOY_CEC_WITH_LIBCEC) && BRICKBOY_CEC_WITH_LIBCEC
const char *log_level_name(CEC::cec_log_level level) {
  switch (level) {
  case CEC::CEC_LOG_ERROR:
    return "error";
  case CEC::CEC_LOG_WARNING:
    return "warning";
  case CEC::CEC_LOG_NOTICE:
    return "notice";
  case CEC::CEC_LOG_TRAFFIC:
    return "traffic";
  case CEC::CEC_LOG_DEBUG:
    return "debug";
  default:
    return "unknown";
  }
}

const char *alert_name(CEC::libcec_alert alert) {
  switch (alert) {
  case CEC::CEC_ALERT_SERVICE_DEVICE:
    return "service_device";
  case CEC::CEC_ALERT_CONNECTION_LOST:
    return "connection_lost";
  case CEC::CEC_ALERT_PERMISSION_ERROR:
    return "permission_error";
  case CEC::CEC_ALERT_PORT_BUSY:
    return "port_busy";
  case CEC::CEC_ALERT_PHYSICAL_ADDRESS_ERROR:
    return "physical_address_error";
  case CEC::CEC_ALERT_TV_POLL_FAILED:
    return "tv_poll_failed";
  default:
    return "unknown";
  }
}

class LibCecRunner {
public:
  LibCecRunner(CecKeyboardApp &app, const Options &options) : app_(app), options_(options) {}

  bool start() {
    callbacks_.Clear();
    callbacks_.commandReceived = &LibCecRunner::on_command;
    callbacks_.keyPress = &LibCecRunner::on_keypress;
    callbacks_.alert = &LibCecRunner::on_alert;
    callbacks_.logMessage = &LibCecRunner::on_log;
    callbacks_.menuStateChanged = &LibCecRunner::on_menu_state_changed;
    callbacks_.commandHandler = &LibCecRunner::on_command_handler;

    config_.Clear();
    config_.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    std::snprintf(config_.strDeviceName, sizeof(config_.strDeviceName), "Brickboy");
    config_.deviceTypes.Clear();
    config_.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    config_.bActivateSource = 0;
    config_.bPowerOffOnStandby = 0;
    prepare_preopen_physical_address();
    config_.callbackParam = this;
    config_.callbacks = &callbacks_;

    adapter_ = CECInitialise(&config_);
    if (!adapter_) {
      std::cerr << "CECInitialise failed\n";
      return false;
    }

    adapter_->InitVideoStandalone();

    std::string port = options_.port;
    if (port.empty()) {
      CEC::cec_adapter_descriptor devices[10];
      int8_t count = adapter_->DetectAdapters(devices, 10, nullptr, true);
      if (count <= 0) {
        std::cerr << "no CEC adapters detected\n";
        return false;
      }
      int selected = 0;
      for (int8_t i = 0; i < count; ++i) {
        if (std::string(devices[i].strComName) == "RPI") {
          selected = i;
          break;
        }
      }
      port = devices[selected].strComName;
      if (port.empty()) {
        port = devices[selected].strComPath;
      }
      std::cerr << "using CEC adapter " << devices[selected].strComPath << " / "
                << devices[selected].strComName << "\n";
    }

    if (!adapter_->Open(port.c_str())) {
      std::cerr << "unable to open CEC adapter on port " << port << "\n";
      return false;
    }

    if (options_.control_socket_enabled && !control_socket_.start(options_.control_socket_path)) {
      return false;
    }

    detect_adapter_physical_address();
    finalize_physical_address_candidates();
    start_deferred_actions();
    return true;
  }

  void run() {
    while (!g_stop.load()) {
      app_.tick();
      if (options_.control_socket_enabled) {
        control_socket_.poll([this](const std::string &line) {
          return handle_control_command(line);
        });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  ~LibCecRunner() {
    if (deferred_thread_.joinable()) {
      deferred_thread_.join();
    }
    if (adapter_) {
      adapter_->Close();
      CECDestroy(adapter_);
      adapter_ = nullptr;
    }
  }

private:
  struct PhysicalAddressCandidate {
    uint16_t physical;
    std::string source;
  };

  void prepare_preopen_physical_address() {
    if (options_.physical_address) {
      config_.iPhysicalAddress = *options_.physical_address;
      config_.bAutodetectAddress = 0;
      add_physical_address_candidate(*options_.physical_address, "explicit");
      app_.publish_log("notice",
                       "using explicit CEC physical address " +
                           hex_word(*options_.physical_address));
      return;
    }

    if (options_.hdmi_port) {
      uint16_t physical = static_cast<uint16_t>(*options_.hdmi_port) << 12;
      config_.baseDevice = CEC::CECDEVICE_TV;
      config_.iHDMIPort = *options_.hdmi_port;
      add_physical_address_candidate(physical, "hdmi_port");
      app_.publish_log("notice", "using explicit HDMI port " + std::to_string(*options_.hdmi_port) +
                                     " as CEC physical address " + hex_word(physical));
      return;
    }

    edid_physical_address_ = find_edid_physical_address();
    if (edid_physical_address_) {
      config_.iPhysicalAddress = edid_physical_address_->physical;
      config_.bAutodetectAddress = 0;
      add_physical_address_candidate(edid_physical_address_->physical,
                                     "edid:" + edid_physical_address_->source);
      app_.publish_log("notice",
                       "using EDID CEC physical address " +
                           hex_word(edid_physical_address_->physical) + " from " +
                           edid_physical_address_->source);
    }
  }

  bool explicit_physical_address_configured() const {
    return options_.physical_address.has_value() || options_.hdmi_port.has_value();
  }

  void add_physical_address_candidate(uint16_t physical, const std::string &source) {
    if (!is_valid_playback_physical_address(physical)) {
      return;
    }
    auto it = std::find_if(physical_address_candidates_.begin(),
                           physical_address_candidates_.end(),
                           [&](const PhysicalAddressCandidate &candidate) {
                             return candidate.physical == physical;
                           });
    if (it != physical_address_candidates_.end()) {
      if (it->source.find(source) == std::string::npos) {
        it->source += "," + source;
      }
      return;
    }
    physical_address_candidates_.push_back(PhysicalAddressCandidate{physical, source});
  }

  void add_default_probe_candidates() {
    for (uint16_t port = 1; port <= 4; ++port) {
      add_physical_address_candidate(static_cast<uint16_t>(port << 12),
                                     "probe_hdmi_" + std::to_string(port));
    }
  }

  void select_physical_address(const PhysicalAddressCandidate &candidate,
                               const std::string &action) {
    selected_physical_address_ = candidate.physical;
    selected_physical_address_source_ = candidate.source;
    app_.publish_log("notice", action + " CEC physical address " + hex_word(candidate.physical) +
                                   " from " + candidate.source);
    app_.publish_cec_address(action, candidate.physical, candidate.source);
  }

  void finalize_physical_address_candidates() {
    if (!explicit_physical_address_configured()) {
      add_default_probe_candidates();
    }
    if (!selected_physical_address_ && !physical_address_candidates_.empty()) {
      select_physical_address(physical_address_candidates_.front(), "selected");
    }
  }

  void detect_adapter_physical_address() {
    if (!adapter_) {
      return;
    }

    CEC::cec_logical_addresses addresses = adapter_->GetLogicalAddresses();
    if (!addresses.IsEmpty()) {
      uint16_t physical = adapter_->GetDevicePhysicalAddress(addresses.primary);
      if (is_valid_playback_physical_address(physical)) {
        detected_physical_address_ = physical;
        add_physical_address_candidate(physical, "libcec");
        app_.publish_log("notice", "detected CEC physical address " + hex_word(physical));
        return;
      }
      app_.publish_log("warning",
                       "libCEC returned unusable physical address " + hex_word(physical));
    } else {
      app_.publish_log("warning", "libCEC did not report a logical address yet");
    }

    if (is_valid_playback_physical_address(config_.iPhysicalAddress)) {
      detected_physical_address_ = config_.iPhysicalAddress;
      add_physical_address_candidate(config_.iPhysicalAddress, "libcec_config");
      app_.publish_log("notice",
                       "using libCEC configuration physical address " +
                           hex_word(config_.iPhysicalAddress));
    }
  }

  void start_deferred_actions() {
    if ((!options_.activate_source && !options_.physical_address) || deferred_thread_.joinable()) {
      return;
    }
    app_.publish_log("notice", "starting deferred CEC action worker");
    deferred_thread_ = std::thread([this]() {
      const char entered[] = "deferred CEC action worker entered\n";
      (void)::write(STDERR_FILENO, entered, sizeof(entered) - 1);
      if (options_.activate_source_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.activate_source_delay_ms));
      }
      if (g_stop.load() || !adapter_) {
        return;
      }
      if (options_.physical_address && !options_.hdmi_port) {
        const char forcing[] = "deferred CEC action worker forcing physical address\n";
        (void)::write(STDERR_FILENO, forcing, sizeof(forcing) - 1);
        adapter_->SetPhysicalAddress(*options_.physical_address);
        app_.publish_log("notice", "forcing physical address " + hex_word(*options_.physical_address));
      }
      if (options_.activate_source) {
        const char activating[] = "deferred CEC action worker sending active-source\n";
        (void)::write(STDERR_FILENO, activating, sizeof(activating) - 1);
        send_active_source();
        app_.publish_log("notice", "sending delayed active-source request");
        probe_physical_addresses_if_needed();
      }
    });
  }

  bool should_probe_physical_addresses() const {
    if (!options_.address_probe_enabled || explicit_physical_address_configured()) {
      return false;
    }
    if (physical_address_candidates_.size() <= 1) {
      return false;
    }
    if (options_.force_address_probe) {
      return true;
    }
    return !edid_physical_address_.has_value();
  }

  void probe_physical_addresses_if_needed() {
    if (!should_probe_physical_addresses()) {
      return;
    }

    auto selected = selected_physical_address_;
    for (const auto &candidate : physical_address_candidates_) {
      if (g_stop.load() || app_.cec_key_seen()) {
        return;
      }
      if (selected && candidate.physical == *selected) {
        continue;
      }
      if (options_.address_probe_interval_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.address_probe_interval_ms));
      }
      if (g_stop.load() || app_.cec_key_seen()) {
        return;
      }
      select_physical_address(candidate, "probing");
      send_active_source();
    }

    if (!app_.cec_key_seen() && selected) {
      auto it = std::find_if(physical_address_candidates_.begin(),
                             physical_address_candidates_.end(),
                             [&](const PhysicalAddressCandidate &candidate) {
                               return candidate.physical == *selected;
                             });
      if (it != physical_address_candidates_.end()) {
        select_physical_address(*it, "restored");
        send_active_source();
      }
    }
  }

  std::optional<uint16_t> configured_physical_address() const {
    if (selected_physical_address_) {
      return *selected_physical_address_;
    }
    return std::nullopt;
  }

  bool send_active_source() {
    if (transmit_active_source_raw()) {
      return true;
    }
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    return adapter_ && adapter_->SetActiveSource();
  }

  bool send_standby_tv() {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    return adapter_ && adapter_->StandbyDevices(CEC::CECDEVICE_TV);
  }

  bool send_power_on_tv() {
    std::lock_guard<std::mutex> lock(adapter_mutex_);
    return adapter_ && adapter_->PowerOnDevices(CEC::CECDEVICE_TV);
  }

  bool query_active_source(bool &known) {
    known = false;
    CEC::cec_logical_address initiator = CEC::CECDEVICE_PLAYBACKDEVICE2;
    uint64_t observed_generation = 0;

    {
      std::lock_guard<std::mutex> lock(active_source_mutex_);
      observed_generation = active_source_generation_;
    }

    {
      std::lock_guard<std::mutex> lock(adapter_mutex_);
      if (!adapter_) {
        return false;
      }
      CEC::cec_logical_addresses addresses = adapter_->GetLogicalAddresses();
      if (!addresses.IsEmpty()) {
        initiator = addresses.primary;
      }

      CEC::cec_command request;
      CEC::cec_command::Format(request, initiator, CEC::CECDEVICE_BROADCAST,
                               CEC::CEC_OPCODE_REQUEST_ACTIVE_SOURCE, 1000);
      if (!adapter_->Transmit(request)) {
        return false;
      }
    }

    CEC::cec_logical_address active = CEC::CECDEVICE_UNKNOWN;
    uint16_t physical = CEC_INVALID_PHYSICAL_ADDRESS;
    {
      std::unique_lock<std::mutex> lock(active_source_mutex_);
      bool received = active_source_condition_.wait_for(
          lock, std::chrono::milliseconds(500),
          [&] { return active_source_generation_ != observed_generation; });
      if (!received) {
        // Never report libCEC's cached Active Source here: after the TV is
        // switched off that cache can remain stale and cause a false wake.
        return true;
      }
      active = last_active_source_logical_;
      physical = last_active_source_physical_;
    }

    known = true;
    app_.publish_active_source(static_cast<int>(active), physical, "query");
    return true;
  }

  std::string control_response(const std::string &command, bool ok,
                               const std::string &message) {
    app_.publish_control_result(command, ok, message);
    std::string response = "{\"type\":\"control_response\",\"cmd\":" + json_string(command) +
                           ",\"ok\":";
    response += ok ? "true" : "false";
    if (!message.empty()) {
      response += ",\"message\":" + json_string(message);
    }
    response += "}";
    return response;
  }

  std::string handle_control_command(const std::string &line) {
    auto parsed = parse_control_command_name(line);
    if (!parsed || parsed->empty()) {
      return "{\"type\":\"control_response\",\"ok\":false,\"error\":\"expected command or JSON {\\\"cmd\\\":\\\"...\\\"}\"}";
    }

    std::string command = normalized_name(*parsed, false);
    if (command == "standby_tv" || command == "tv_standby" || command == "standby" ||
        command == "power_off_tv" || command == "power_off") {
      bool ok = send_standby_tv();
      return control_response("standby_tv", ok, ok ? "sent standby to tv" : "failed to send standby");
    }

    if (command == "power_on_tv" || command == "tv_power_on" || command == "power_on" ||
        command == "on") {
      bool ok = send_power_on_tv();
      return control_response("power_on_tv", ok,
                              ok ? "sent power on to tv" : "failed to send power on");
    }

    if (command == "active_source" || command == "activate_source" || command == "as") {
      bool ok = send_active_source();
      return control_response("active_source", ok,
                              ok ? "sent active source" : "failed to send active source");
    }

    if (command == "get_active_source" || command == "query_active_source" ||
        command == "request_active_source") {
      bool known = false;
      bool ok = query_active_source(known);
      return control_response(
          "get_active_source", ok,
          !ok ? "failed to request active source" :
          known ? "reported active source" : "requested active source; no response");
    }

    return "{\"type\":\"control_response\",\"cmd\":" + json_string(command) +
           ",\"ok\":false,\"error\":\"unknown command\"}";
  }

  bool transmit_active_source_raw() {
    auto physical = configured_physical_address();
    if (!physical || !adapter_) {
      return false;
    }

    std::lock_guard<std::mutex> lock(adapter_mutex_);
    if (!adapter_) {
      return false;
    }
    CEC::cec_logical_address initiator = CEC::CECDEVICE_PLAYBACKDEVICE2;
    CEC::cec_logical_addresses addresses = adapter_->GetLogicalAddresses();
    if (!addresses.IsEmpty()) {
      initiator = addresses.primary;
    }

    CEC::cec_command command;
    CEC::cec_command::Format(command, initiator, CEC::CECDEVICE_BROADCAST,
                             CEC::CEC_OPCODE_ACTIVE_SOURCE, 1000);
    command.PushBack(static_cast<uint8_t>((*physical >> 8) & 0xff));
    command.PushBack(static_cast<uint8_t>(*physical & 0xff));
    return adapter_->Transmit(command);
  }

  static void CEC_CDECL on_command(void *cbparam, const CEC::cec_command *command) {
    auto *self = static_cast<LibCecRunner *>(cbparam);
    if (!self || !command || !command->opcode_set) {
      return;
    }

    std::vector<uint8_t> params;
    for (uint8_t i = 0; i < command->parameters.size; ++i) {
      params.push_back(command->parameters[i]);
    }
    self->observe_command(*command);
    self->app_.handle_command(static_cast<int>(command->initiator),
                              static_cast<int>(command->destination),
                              static_cast<int>(command->opcode), params);
  }

  static void CEC_CDECL on_keypress(void *cbparam, const CEC::cec_keypress *key) {
    auto *self = static_cast<LibCecRunner *>(cbparam);
    if (!self || !key) {
      return;
    }
    self->app_.publish_raw_keypress(static_cast<int>(key->keycode), key->duration);
  }

  static int CEC_CDECL on_menu_state_changed(void *cbparam, const CEC::cec_menu_state state) {
    auto *self = static_cast<LibCecRunner *>(cbparam);
    if (self) {
      self->app_.publish_menu_state(state == CEC::CEC_MENU_STATE_ACTIVATED ? "activated"
                                                                           : "deactivated");
    }
    return 0;
  }

  static int CEC_CDECL on_command_handler(void *, const CEC::cec_command *) {
    return 0;
  }

  void observe_command(const CEC::cec_command &command) {
    if (!command.opcode_set || command.opcode != CEC::CEC_OPCODE_ACTIVE_SOURCE ||
        command.parameters.size < 2) {
      return;
    }

    std::lock_guard<std::mutex> lock(active_source_mutex_);
    last_active_source_logical_ = command.initiator;
    last_active_source_physical_ =
        static_cast<uint16_t>((command.parameters[0] << 8) | command.parameters[1]);
    ++active_source_generation_;
    active_source_condition_.notify_all();
  }

  static void CEC_CDECL on_alert(void *cbparam, const CEC::libcec_alert alert,
                                 const CEC::libcec_parameter) {
    auto *self = static_cast<LibCecRunner *>(cbparam);
    if (self) {
      self->app_.publish_alert(alert_name(alert));
      if (alert == CEC::CEC_ALERT_TV_POLL_FAILED) {
        self->app_.publish_display_status(std::optional<bool>{false}, "tv_poll_failed");
      } else if (alert == CEC::CEC_ALERT_CONNECTION_LOST) {
        self->app_.publish_display_status(std::optional<bool>{false}, "connection_lost");
      }
    }
  }

  static void CEC_CDECL on_log(void *cbparam, const CEC::cec_log_message *message) {
    auto *self = static_cast<LibCecRunner *>(cbparam);
    if (self && message && message->message) {
      self->observe_log_message(message->message);
      self->app_.publish_log(log_level_name(message->level), message->message);
    }
  }

  void observe_log_message(const std::string &message) {
    const std::string marker = "TV (0): device status changed into '";
    auto pos = message.find(marker);
    if (pos == std::string::npos) {
      return;
    }

    pos += marker.size();
    auto end = message.find('\'', pos);
    std::string status = message.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (status == "present") {
      app_.publish_display_status(std::optional<bool>{true}, "tv_status_present");
    } else if (status == "not present" || status == "unknown") {
      app_.publish_display_status(std::optional<bool>{false}, "tv_status_" + status);
    }
  }

  CecKeyboardApp &app_;
  const Options &options_;
  CEC::ICECAdapter *adapter_ = nullptr;
  CEC::ICECCallbacks callbacks_;
  CEC::libcec_configuration config_;
  ControlSocketServer control_socket_;
  std::thread deferred_thread_;
  std::mutex adapter_mutex_;
  std::mutex active_source_mutex_;
  std::condition_variable active_source_condition_;
  uint64_t active_source_generation_ = 0;
  CEC::cec_logical_address last_active_source_logical_ = CEC::CECDEVICE_UNKNOWN;
  uint16_t last_active_source_physical_ = CEC_INVALID_PHYSICAL_ADDRESS;
  std::vector<PhysicalAddressCandidate> physical_address_candidates_;
  std::optional<EdidPhysicalAddress> edid_physical_address_;
  std::optional<uint16_t> detected_physical_address_;
  std::optional<uint16_t> selected_physical_address_;
  std::string selected_physical_address_source_;
};
#endif

int run_daemon(const Options &options) {
#if !defined(BRICKBOY_CEC_WITH_LIBCEC) || !BRICKBOY_CEC_WITH_LIBCEC
  (void)options;
  std::cerr << "built without libCEC support; use --self-test or rebuild with BRICKBOY_CEC_WITH_LIBCEC=ON\n";
  return 1;
#else
  CecKeyboardApp app(options);
  if (!app.start_io()) {
    return 1;
  }

  LibCecRunner runner(app, options);
  if (!runner.start()) {
    return 1;
  }
  runner.run();
  return 0;
#endif
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Options options = parse_args(argc, argv);
  if (options.self_test) {
    return run_self_test(options);
  }
  return run_daemon(options);
}
