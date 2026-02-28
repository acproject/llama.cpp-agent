#include "subagent-output.h"
#include "console.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

static std::mutex g_console_mutex;

static display_type map_display_type(display_type_extended type) {
  switch (type) {
  case DISPLAY_TYPE_SUBAGENT:
    return DISPLAY_TYPE_INFO;
  }
  return DISPLAY_TYPE_RESET;
}

output_guard::output_guard() { g_console_mutex.lock(); }

output_guard::~output_guard() { g_console_mutex.unlock(); }

void output_guard::write(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);

  std::string content(size + 1, '\0');
  vsnprintf(&content[0], size + 1, fmt, args);
  content.resize(size);

  va_end(args);

  console::log("%s", content.c_str());
}

void output_guard::set_display(display_type type) { console::set_display(type); }

void output_guard::set_display(display_type_extended type) {
  console::set_display(map_display_type(type));
}

void output_guard::flush() { console::flush(); }

subagent_output_buffer::subagent_output_buffer(const std::string &task_id)
    : task_id_(task_id) {}

void subagent_output_buffer::write(display_type type, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);

  std::string content(size + 1, '\0');
  vsnprintf(&content[0], size + 1, fmt, args);
  content.resize(size);

  va_end(args);

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  segments_.push_back({type, std::move(content)});
}

void subagent_output_buffer::write(display_type_extended type, const char *fmt,
                                  ...) {
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);

  std::string content(size + 1, '\0');
  vsnprintf(&content[0], size + 1, fmt, args);
  content.resize(size);

  va_end(args);

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  segments_.push_back({map_display_type(type), std::move(content)});
}

void subagent_output_buffer::write(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(nullptr, 0, fmt, args_copy);
  va_end(args_copy);

  std::string content(size + 1, '\0');
  vsnprintf(&content[0], size + 1, fmt, args);
  content.resize(size);

  va_end(args);

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  segments_.push_back({DISPLAY_TYPE_RESET, std::move(content)});
}

void subagent_output_buffer::flush(bool with_task_prefix) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  if (segments_.empty()) {
    return;
  }

  output_guard guard;

  std::string prefix;
  if (with_task_prefix && !task_id_.empty()) {
    std::string short_id = task_id_;
    if (short_id.substr(0, 5) == "task-" && short_id.length() > 9) {
      short_id = short_id.substr(5, 4);
    }
    prefix = "[" + short_id + "] ";
  }

  bool at_line_start = true;

  for (const auto &seg : segments_) {
    guard.set_display(seg.type);

    for (size_t i = 0; i < seg.content.size(); ++i) {
      char c = seg.content[i];

      if (at_line_start && !prefix.empty()) {
        guard.set_display(DISPLAY_TYPE_REASONING);
        guard.write("%s", prefix.c_str());
        guard.set_display(seg.type);
        at_line_start = false;
      }

      guard.write("%c", c);
      if (c == '\n') {
        at_line_start = true;
      }
    }
  }

  guard.set_display(DISPLAY_TYPE_RESET);
  guard.flush();

  segments_.clear();
}

void subagent_output_buffer::clear() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  segments_.clear();
}

bool subagent_output_buffer::empty() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return segments_.empty();
}

subagent_output_manager &subagent_output_manager::instance() {
  static subagent_output_manager instance;
  return instance;
}

subagent_output_buffer *
subagent_output_manager::create_buffer(const std::string &task_id) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  auto buffer = std::make_unique<subagent_output_buffer>(task_id);
  auto *ptr = buffer.get();
  buffers_[task_id] = std::move(buffer);
  return ptr;
}

subagent_output_buffer *
subagent_output_manager::get_buffer(const std::string &task_id) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  auto it = buffers_.find(task_id);
  if (it == buffers_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void subagent_output_manager::remove_buffer(const std::string &task_id) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffers_.erase(task_id);
}

void subagent_output_manager::flush_all() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  for (auto &pair : buffers_) {
    pair.second->flush(true);
  }
}

size_t subagent_output_manager::active_count() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return buffers_.size();
}
