#pragma once

#include "common.h"
#include "console.h"

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdio>

// Represents a single output segment with display type
struct output_segment {
  display_type type = DISPLAY_TYPE_RESET;
  std::string content;
};

enum display_type_extended  {

  DISPLAY_TYPE_SUBAGENT =7 

};

// Buffered output for a single subagent task
// Callects output segments and flushes them atomically to console
class subagent_output_buffer {
public:
  explicit subagent_output_buffer(const std::string &task_id);

  // Buffer text with a display type
  void write(display_type type, const char *fmt, ...);
  void write(display_type_extended type, const char *fmt, ...);

  // Buffer text without changing display type (uses DISPLAY_TYPE_RESET)
  void write(const char *fmt, ...);

  // Flush all buffered content atomically to console
  // Optionally prefix each line with task ID
  void flush(bool with_task_prefix = true);

  // Clear buffer without flushing
  void clear();

  // Check if buffer has content
  bool empty() const;

  // Get task ID
  const std::string& task_id() const { return task_id_; }

private:
  std::string task_id_;
  std::vector<output_segment> segments_;
  mutable std::mutex buffer_mutex_;
};

// Manager for all active subagent output buffer
// Thread-safe singleton
class subagent_output_manager {
public:
  static subagent_output_manager &instance();

  // Create buffer for a new task (returns raw pointer, manager owns the buffer)
  subagent_output_buffer *create_buffer(const std::string &task_id);

  // Get buffer for an existing task (returns nullptr if not found)
  subagent_output_buffer *get_buffer(const std::string &task_id);

  // Remove and destroy buffer for a task
  void remove_buffer(const std::string &task_id);

  // Flush all buffers (for status display or shutdown)
  void flush_all();

  // Get count of active buffers
  size_t active_count() const;

private:
  subagent_output_manager() = default;

  mutable std::mutex buffer_mutex_;
  std::map<std::string, std::unique_ptr<subagent_output_buffer>> buffers_;
};


// RAII guard for atomic multi-part console output
// Holds the console mutex for the lifetime of the object
// Use this when you need to output multiple lines/parts atomically
class output_guard {
public:
  output_guard();
  ~output_guard();

  // Non-copyable, non-movable
  output_guard(const output_guard &) = delete;
  output_guard &operator=(const output_guard &) = delete;

  LLAMA_COMMON_ATTRIBUTE_FORMAT(2, 3)
  void write(const char *fmt, ...);

  // Change display type (mutex already held)
  void set_display(display_type type);
  void set_display(display_type_extended type);
  // Flush output (mutex already held)
  void flush();
};
