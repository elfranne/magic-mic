#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
// must be included for custom formatting of enums
#include <spdlog/fmt/ostr.h>

#include "pipesource_virtual_mic.hpp"

using std::stringstream;

// reference:
// https://github.com/gavv/snippets/blob/master/pa/pa_play_async_poll.c That
// gives a pretty good way to structure. I'm making some changes to make it a
// little more cpp imo

PipeSourceVirtualMic::PipeSourceVirtualMic(
    AudioProcessor *denoiser, std::shared_ptr<spdlog::logger> logger)
    : denoiser(), logger(logger), pipesource_module_idx(-1),
      module_load_operation(nullptr), state(InitContext),
      pb_state(PlaybackState::StreamEmpty), cur_act() {
  logger->trace("Init PipeSourceVirtualMic");

  this->denoiser = denoiser;
  shared_sample_spec.channels = 1;
  shared_sample_spec.rate = denoiser->get_sample_rate();
  switch (denoiser->get_audio_format()) {
  case AudioFormat::FLOAT32_LE:
    shared_sample_spec.format = PA_SAMPLE_FLOAT32;
    format_module = "float32";
    break;
  case AudioFormat::S16_LE:
    shared_sample_spec.format = PA_SAMPLE_S16NE;
    format_module = "s16ne";
    break;
  }

  buffer = new uint8_t[buffer_length];
  write_idx = 0;
  read_idx = 0;

  should_run = true;

  mainloop = shared_ptr<pa_mainloop>(pa_mainloop_new(), pa_mainloop_free);
  connect();

  exception_promise = promise<std::exception_ptr>();
  async_thread = thread(&PipeSourceVirtualMic::run, this);
}

void PipeSourceVirtualMic::changeState(State s) {
  logger->trace("Pipesource changing from state {} to state {}", state, s);
  state = s;
}

void PipeSourceVirtualMic::connect() {
  ctx = shared_ptr<pa_context>(
      pa_context_new(pa_mainloop_get_api(mainloop.get()), client_name),
      free_pa_context);
  if (!ctx) {
    throw std::runtime_error("pa_context_new failed");
  }

  int err =
      pa_context_connect(ctx.get(), nullptr, (pa_context_flags)0, nullptr);
  if (err) {
    stringstream ss;
    ss << "pa_context_connect failed: " << pa_strerror(err);
    throw std::runtime_error(ss.str());
  }
  changeState(WaitContextReady);
}

void PipeSourceVirtualMic::run() {
  int err;
  bool swapped_file = false;

  try {
    while (should_run) {
      std::size_t willspew;
      {
        lock_guard<mutex> lock(mainloop_mutex);
        willspew = denoiser->willspew();
      }
      // block when we won't spew
      if ((err = pa_mainloop_iterate(
               mainloop.get(),
               state == Denoise && (0 == willspew || write_idx != read_idx),
               NULL)) < 0) {
        stringstream ss;
        ss << "pa_mainloop_iterate: " << pa_strerror(err);
        throw std::runtime_error(ss.str());
      }
      lock_guard<mutex> lock(mainloop_mutex);
      // TODO figure out a way to respond to state changes. maybe do this along
      // side of switching over to a state bitfield
      switch (state) {
      case InitContext:
        throw std::runtime_error("Context should have been inited already");
      case WaitContextReady:
        poll_context();
        break;
      case InitCheckModuleLoaded:
        check_module_loaded();
        break;
      case WaitCheckModuleLoaded:
        poll_operation();
        break;
      case InitModule:
        load_pipesource_module();
        break;
      case WaitModuleReady:
        poll_operation();
        break;
      case GetDefaultSource:
        if (!get_default_source_op) {
          get_default_source_op =
              pa_context_get_server_info(ctx.get(), serv_info_cb, this);
        }
        break;
      case InitRecStream:
        start_recording_stream();
        break;
      case WaitRecStreamReady:
        poll_recording_stream();
        break;
      case Denoise:
        if (denoiser->get_buffer_size() > max_denoiser_buffer) {
          logger->trace("Cutting buffer from {} to {}",
                        denoiser->get_buffer_size(), max_denoiser_buffer / 2);
          denoiser->drop_samples(denoiser->get_buffer_size() -
                                 max_denoiser_buffer / 2);
        }
        check_mic_active();
        // If nothing is listening
        if (pb_state != Loopback && virtmic_source_state != PA_SOURCE_RUNNING) {
          // Don't waste cpu
          denoiser->set_should_denoise(false);
        } else {
          denoiser->set_should_denoise(should_denoise);
          // If we're just getting back, clear the buffer of old stuff that no
          // one needs to hear
          // denoiser->drop_samples(denoiser->get_buffer_size());
        }
        poll_recording_stream();
        write_to_outputs();
        break;
      }
      switch (cur_act.action) {
      case CurrentAction::GetMicrophones:
        get_microphones();
        break;
      case CurrentAction::SetMicrophone:
        set_microphone();
        break;
      case CurrentAction::NoAction:
        break;
      }
      switch (pb_state) {
      case PlaybackState::StreamEmpty:
        break;
      case PlaybackState::InitStream:
        start_pb_stream();
        break;
      case PlaybackState::WaitingOnStream:
        poll_pb_stream();
        break;
      case PlaybackState::StreamReady:
        // I guess, don't do anything speacial in here
        break;
      }
    }
  } catch (...) {
    exception_promise.set_value(std::current_exception());
  }
}
void PipeSourceVirtualMic::check_module_loaded() {
  module_load_operation =
      pa_context_get_source_info_list(ctx.get(), source_info_cb, this);
  changeState(WaitCheckModuleLoaded);
}
void PipeSourceVirtualMic::set_microphone() {
  assert(cur_act.action == CurrentAction::SetMicrophone);
  switch (cur_act.set_mic.state) {
  case SetMicrophone::SetMicrophoneActionState::InitGettingSource:
    cur_act.set_mic.op = pa_context_get_source_info_by_index(
        ctx.get(), cur_act.set_mic.ind, source_info_cb, this);
    cur_act.set_mic.state =
        SetMicrophone::SetMicrophoneActionState::WaitGettingSource;
    break;
  }
}
void PipeSourceVirtualMic::serv_info_cb(pa_context *c, const pa_server_info *i,
                                        void *u) {
  PipeSourceVirtualMic *m = (PipeSourceVirtualMic *)u;
  if (i) {
    m->source = i->default_source_name;
    pa_operation_unref(m->get_default_source_op);
    m->get_default_source_op = nullptr;
    m->state = InitRecStream;
  } else {
    m->logger->error("serv_info_cb get null pa_serve_info");
  }
}
void PipeSourceVirtualMic::source_info_cb(pa_context *c,
                                          const pa_source_info *i, int eol,
                                          void *u) {
  PipeSourceVirtualMic *m = (PipeSourceVirtualMic *)u;
  if (m->state == WaitCheckModuleLoaded) {
    if (!i) {
      return;
    }
    if (string(i->name) == "virtmic") {
      // We should try to derive this from the actual module if possible
      m->pipe_file_name = "/tmp/virtmic";
      m->pipesource_module_idx = i->owner_module;
    }
  } else {
    switch (m->cur_act.action) {
    case CurrentAction::SetMicrophone:
      if (i) {
        m->cur_act.set_mic.name = string(i->name);
        m->source = m->cur_act.set_mic.name.c_str();
        m->cur_act.set_mic.state =
            SetMicrophone::SetMicrophoneActionState::UpdatingStream;
        m->state = State::InitRecStream;
      } else if (m->cur_act.set_mic.state !=
                 SetMicrophone::SetMicrophoneActionState::UpdatingStream) {
        // This cannot be the best way to do this
        m->cur_act.action = CurrentAction::NoAction;
        try {
          throw std::runtime_error("Source not found");
        } catch (...) {
          m->cur_act.set_mic.p.set_exception(std::current_exception());
        }
      }
      break;
    case CurrentAction::GetMicrophones:
      if (i && string(i->name) != "virtmic") {
        // This is probably not the best way to do this, but it should work for
        // now
        m->cur_act.get_mics.list.push_back(
            std::make_pair(i->index, string(i->description)));
      } else {
        int ind = pa_stream_get_device_index(m->rec_stream.get());
        m->cur_act.get_mics.p.set_value(
            std::make_pair(ind, m->cur_act.get_mics.list));
        pa_operation_unref(m->cur_act.get_mics.op);
        m->cur_act.get_mics.state =
            GetMicrophones::GetMicrophonesActionState::Done;
        m->cur_act.action = CurrentAction::NoAction;
      }
      break;
    }
  }
}
void PipeSourceVirtualMic::get_microphones() {
  assert(cur_act.action == CurrentAction::GetMicrophones);
  switch (cur_act.get_mics.state) {
  case GetMicrophones::GetMicrophonesActionState::Init:
    cur_act.get_mics.op =
        pa_context_get_source_info_list(ctx.get(), source_info_cb, this);
    cur_act.get_mics.state = GetMicrophones::GetMicrophonesActionState::Wait;
    break;
  }
}
void PipeSourceVirtualMic::write_to_outputs() {
  size_t spew_size;
  // logger->trace("Write, read: {}, {}", write_idx, read_idx);
  if ((spew_size = denoiser->willspew())) {
    spew_size = spew_size > buffer_length ? buffer_length : spew_size;
    size_t spewed;
    if (spew_size > buffer_length - write_idx) {
      uint8_t *temp_buffer = new uint8_t[spew_size];
      spewed = denoiser->spew(temp_buffer, spew_size);
      assert(spewed == spew_size);

      if (pb_state == PlaybackState::Loopback) {
        pa_stream_write(pb_stream.get(), temp_buffer, spewed, nullptr, 0,
                        PA_SEEK_RELATIVE);
      }

      size_t remaining_size = buffer_length - write_idx;
      std::copy(temp_buffer, temp_buffer + remaining_size, buffer + write_idx);
      std::copy(temp_buffer + remaining_size, temp_buffer + spew_size, buffer);
      write_idx = spew_size - remaining_size;
      delete[] temp_buffer;
    } else {
      spewed = denoiser->spew(buffer + write_idx, buffer_length - write_idx);
      if (pb_state == PlaybackState::Loopback) {
        pa_stream_write(pb_stream.get(), buffer + write_idx, spewed, nullptr, 0,
                        PA_SEEK_RELATIVE);
      }
      write_idx = (write_idx + spewed) % buffer_length;
    }

    // Let's hope that the playback buffer is long enough because maintaining a
    // seperate buffer seems annoying
  }
  size_t to_write;
  if (read_idx > write_idx) {
    to_write = buffer_length - read_idx;
  } else if (write_idx > read_idx) {
    to_write = write_idx - read_idx;
  } else {
    return;
  }
  // Let's just make sure that to_write is a multiple of float just to be safe
  // with alignment stuff. Not sure if it matters here, but just being safe
  ssize_t written =
      write(pipe_fd, buffer + read_idx, to_write - (to_write % sizeof(float)));
  if (written == -1) {
    switch (errno) {
    case EINTR:
    case EAGAIN:
      break;
    default:
      stringstream ss;
      ss << "pa_mainloop_iterate, write: " << strerror(errno);
      throw std::runtime_error(ss.str());
    }
  } else {
    // logger->trace("written: {}", written);
    read_idx = (written + read_idx) % buffer_length;
  }
}
void PipeSourceVirtualMic::poll_context() {
  pa_context_state_t state = pa_context_get_state(ctx.get());

  switch (state) {
  case PA_CONTEXT_READY:
    /* context connected to server, */
    if (this->state == WaitContextReady) {
      changeState(InitCheckModuleLoaded);
    } else {
      // TODO when I get logging setup log something here because it shouldn't
      // really happen
    }
    break;

  case PA_CONTEXT_FAILED:
  case PA_CONTEXT_TERMINATED:
    /* context connection failed */
    throw std::runtime_error("failed to connect to pulseaudio context!");
  default:
    /* nothing interesting */
    break;
  }
}

void PipeSourceVirtualMic::poll_operation() {
  assert(module_load_operation);

  switch (pa_operation_get_state(module_load_operation)) {
  case PA_OPERATION_CANCELLED:
    throw std::runtime_error("module_load_operation cancelled!");
  case PA_OPERATION_DONE:
    pa_operation_unref(module_load_operation);
    module_load_operation = nullptr;
    if (-1 == pipesource_module_idx && state == WaitModuleReady) {
      throw std::runtime_error("Failed to load module-pipesource");
    } else if (pipesource_module_idx == -1) {
      changeState(InitModule);
    } else {
      changeState(InitRecStream);

      // Not sure how O_APPEND works on pipes, but shouldn't hurt
      pipe_fd = open(pipe_file_name.c_str(), O_WRONLY | O_APPEND | O_NONBLOCK);
      if (-1 == pipe_fd) {
        stringstream ss;
        ss << "Failed to open file \"" << pipe_file_name << "\" ("
           << strerror(errno) << ")";
        throw std::runtime_error(ss.str());
      }
    }
  default:
    break;
  }
}

void PipeSourceVirtualMic::load_pipesource_module() {
  // TODO: use mktemp or whatever variant is appropraite
  pipe_file_name = "/tmp/virtmic";
  stringstream ss;
  ss << "source_name=virtmic "
     << "file=/tmp/virtmic "
     << "format=" << format_module << " "
     << "rate=" << shared_sample_spec.rate << " "
     << "channels=1 "
     << "source_properties=\"device.description='Magic Mic'\"",

      module_load_operation = pa_context_load_module(
          ctx.get(), "module-pipe-source", ss.str().c_str(),
          PipeSourceVirtualMic::index_cb, this);
  changeState(WaitModuleReady);
}

void PipeSourceVirtualMic::index_cb(pa_context *c, unsigned int idx, void *u) {
  PipeSourceVirtualMic *m = (PipeSourceVirtualMic *)u;
  m->logger->debug("Module loaded, idx={}", idx);
  m->pipesource_module_idx = idx;
}
void PipeSourceVirtualMic::start_pb_stream() {
  pb_stream = shared_ptr<pa_stream>(
      pa_stream_new(ctx.get(), pb_stream_name, &shared_sample_spec, nullptr),
      free_pa_stream);
  if (!pb_stream) {
    stringstream ss;
    ss << "pa_stream_new failed (pb): "
       << pa_strerror(pa_context_errno(ctx.get()));
    throw std::runtime_error(ss.str());
  }

  int err = pa_stream_connect_playback(pb_stream.get(), nullptr, nullptr,
                                       (pa_stream_flags)0, nullptr, nullptr);
  if (err != 0) {
    stringstream ss;
    ss << "pa_stream_connect_playback: " << pa_strerror(err);
    throw std::runtime_error(ss.str());
  }
  pb_state = WaitingOnStream;
}
void PipeSourceVirtualMic::start_recording_stream() {
  // TODO: make this configurable
  rec_stream = shared_ptr<pa_stream>(
      pa_stream_new(ctx.get(), rec_stream_name, &shared_sample_spec, nullptr),
      free_pa_stream);
  if (!rec_stream) {
    stringstream ss;
    ss << "pa_stream_new failed (rec): "
       << pa_strerror(pa_context_errno(ctx.get()));
    throw std::runtime_error(ss.str());
  }
  pa_buffer_attr attr = {
      .maxlength = (uint32_t)-1,
      .tlength = (uint32_t)-1,
      .prebuf = (uint32_t)-1,
      .minreq = (uint32_t)-1,
      .fragsize = (uint32_t)buffer_length,
  };
  int err = pa_stream_connect_record(rec_stream.get(), source, &attr,
                                     (pa_stream_flags)PA_STREAM_ADJUST_LATENCY);
  if (err != 0) {
    stringstream ss;
    ss << "pa_stream_connect_record: " << pa_strerror(err);
    throw std::runtime_error(ss.str());
  }
  changeState(WaitRecStreamReady);
}
void PipeSourceVirtualMic::poll_pb_stream() {
  pa_stream_state_t state = pa_stream_get_state(pb_stream.get());
  switch (state) {
  case PA_STREAM_READY:
    /* stream is writable, proceed now */
    if (pb_state == PlaybackState::WaitingOnStream) {
      pb_state = Loopback;
    }
    if (cur_act.action == CurrentAction::Loopback) {
      pb_promise.set_value(true);
      cur_act.action = CurrentAction::NoAction;
    }
    break;

  case PA_STREAM_FAILED:
  case PA_STREAM_TERMINATED:
    /* stream is closed, exit */
    throw std::runtime_error("playback_stream closed unexpectedly");
  default:
    /* stream is not ready yet */
    return;
  }
}
void PipeSourceVirtualMic::poll_recording_stream() {
  pa_stream_state_t state = pa_stream_get_state(rec_stream.get());

  switch (state) {
  case PA_STREAM_READY: {
    /* stream is writable, proceed now */
    if (this->state == WaitRecStreamReady) {
      changeState(Denoise);
      if (cur_act.action == CurrentAction::SetMicrophone) {
        cur_act.set_mic.p.set_value();
        cur_act.action = CurrentAction::NoAction;
      }
    }
    size_t readable_size = pa_stream_readable_size(rec_stream.get());
    if (readable_size > max_read_stream_buffer) {
      should_denoise = false;
      denoiser->set_should_denoise(should_denoise);
      {
        std::lock_guard<mutex> lg(updates_mutex);
        updates.push({
            .update = VirtualMicUpdate::UpdateAudioProcessing,
            .audioProcessingValue = false,
        });
      }
    }
    if (readable_size > 0) {
      const void *data;
      size_t nbytes;
      int err = pa_stream_peek(rec_stream.get(), &data, &nbytes);
      if (err) {
        stringstream ss;
        ss << "pa_stream_peak: " << pa_strerror(err);
        throw std::runtime_error(ss.str());
      }
      denoiser->feed((uint8_t *)data, nbytes);
      pa_stream_drop(rec_stream.get());
    }
    break;
  }
  case PA_STREAM_FAILED:
  case PA_STREAM_TERMINATED:
    /* stream is closed, exit */
    throw std::runtime_error("recording_stream closed unexpectedly");
  default:
    /* stream is not ready yet */
    return;
  }
}
void PipeSourceVirtualMic::free_pa_context(pa_context *ctx) {
  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
}
void PipeSourceVirtualMic::free_pa_stream(pa_stream *s) {
  pa_stream_disconnect(s);
  pa_stream_unref(s);
}
PipeSourceVirtualMic::~PipeSourceVirtualMic() {
  // TODO err is pretty ugly in here. shoudl fix that
  int err = 0;

  if (pipe_fd) {
    err = close(pipe_fd);
  }
  if (err) {
    logger->error("Error closing pipe_fd: {}", strerror(err));
  }
  err = 0;
  delete[] buffer;
  if (-1 != pipesource_module_idx && ctx &&
      PA_CONTEXT_READY == pa_context_get_state(ctx.get())) {
    // TODO make this use logger when I get that
    logger->trace("Unloading module");
    pa_operation *op = pa_context_unload_module(
        ctx.get(), pipesource_module_idx, nullptr, nullptr);
    int i;
    for (i = 0; !err && i < 1000; i++) {
      if ((err = pa_mainloop_iterate(mainloop.get(), 0, NULL)) < 0) {
        logger->error("Error unloading module in pa_mainloop_iterate(...): {}",
                      pa_strerror(err));
        break;
      }
      err = 0;
      switch (pa_operation_get_state(op)) {
      case PA_OPERATION_RUNNING:
        continue;
      case PA_OPERATION_DONE:
        i = 1000;
        break;
      case PA_OPERATION_CANCELLED:
        err = 1;
        logger->warn("Unload operation cancelled");
        break;
      }
    }
    if (err) {
      logger->error("Error unloading module");
    }
  }
}
void PipeSourceVirtualMic::check_mic_active() {
  pa_usec_t cur = pa_rtclock_now();
  if (cur - mic_active_last_check >= mic_active_interval) {
    mic_active_last_check = cur;

    if (mic_active_op) {
      pa_operation_cancel(mic_active_op);
      pa_operation_unref(mic_active_op);
    }

    mic_active_op = pa_context_get_source_info_by_name(
        ctx.get(), "virtmic", mic_active_source_info_cb, this);
  }
}
void PipeSourceVirtualMic::mic_active_source_info_cb(pa_context *ctx,
                                                     const pa_source_info *i,
                                                     int eol, void *u) {
  PipeSourceVirtualMic *m = (PipeSourceVirtualMic *)u;
  if (i) {
    if (i->state != m->virtmic_source_state) {
      m->logger->trace("VirtMic state is {}", i->state);
    }
    m->virtmic_source_state = i->state;
  }
}
void PipeSourceVirtualMic::stop() {
  //   lock_guard<mutex> lock(mainloop_mutex);
  should_run = false;
  pa_mainloop_wakeup(mainloop.get());
  async_thread.join();
}
future<bool> PipeSourceVirtualMic::getStatus() {
  lock_guard<mutex> lock(mainloop_mutex);
  promise<bool> p;
  p.set_value(state >= InitRecStream);
  return p.get_future();
}

future<pair<int, vector<pair<int, string>>>>
PipeSourceVirtualMic::getMicrophones() {
  lock_guard<mutex> lock(mainloop_mutex);
  if (state < InitRecStream) {
    throw std::runtime_error("Not ready to get Microphones");
  }
  // TODO: we don't support multiple actions at a time yet
  if (cur_act.action != CurrentAction::NoAction) {
    throw std::runtime_error("Can only handle single action at a time");
  }
  cur_act.action = CurrentAction::GetMicrophones;
  cur_act.get_mics = {.state = GetMicrophones::GetMicrophonesActionState::Init,
                      .p = promise<pair<int, vector<pair<int, string>>>>()};
  pa_mainloop_wakeup(mainloop.get());
  return cur_act.get_mics.p.get_future();
}
future<void> PipeSourceVirtualMic::setMicrophone(int ind) {
  lock_guard<mutex> lock(mainloop_mutex);
  if (state < InitRecStream) {
    throw std::runtime_error("Not ready to get Microphones");
  }
  // TODO: we don't support multiple actions at a time yet
  if (cur_act.action != CurrentAction::NoAction) {
    throw std::runtime_error("Can only handle single action at a time");
  }
  cur_act.action = CurrentAction::SetMicrophone;
  cur_act.set_mic = {
      .state = SetMicrophone::SetMicrophoneActionState::InitGettingSource,
      .ind = ind,
      .p = promise<void>()};

  pa_mainloop_wakeup(mainloop.get());
  return cur_act.set_mic.p.get_future();
}
future<void> PipeSourceVirtualMic::setRemoveNoise(bool b) {
  lock_guard<mutex> lock(mainloop_mutex);
  should_denoise = b;

  promise<void> p;
  p.set_value();
  return p.get_future();
}
future<bool> PipeSourceVirtualMic::setLoopback(bool b) {
  lock_guard<mutex> lock(mainloop_mutex);
  if (state < InitRecStream) {
    throw std::runtime_error("Not ready to get Microphones");
  }
  // TODO: we don't support multiple actions at a time yet
  if (cur_act.action != CurrentAction::NoAction) {
    throw std::runtime_error("Can only handle single action at a time");
  }
  pb_promise = promise<bool>();
  switch (pb_state) {
  case StreamEmpty:
    if (b) {
      pb_state = PlaybackState::InitStream;
      cur_act.action = CurrentAction::Loopback;
    } else {
      pb_promise.set_value(true);
    }
    break;
  case StreamReady:
    if (b) {
      pb_state = PlaybackState::Loopback;
    }
    pb_promise.set_value(true);
    break;
  case Loopback:
    if (b) {
      pb_promise.set_value(true);
    } else {
      pb_state = StreamReady;
      pb_promise.set_value(true);
    }
    break;
  case StreamBroken:
  default:
    pb_promise.set_value(false);
    break;
  }

  pa_mainloop_wakeup(mainloop.get());
  return pb_promise.get_future();
}
void PipeSourceVirtualMic::abortLastRequest() {
  lock_guard<mutex> lock(mainloop_mutex);
  switch (cur_act.action) {
  case CurrentAction::NoAction:
    break;
  case CurrentAction::GetMicrophones:
    if (cur_act.get_mics.state ==
        GetMicrophones::GetMicrophonesActionState::Wait) {
      pa_operation_cancel(cur_act.get_mics.op);
      pa_operation_unref(cur_act.get_mics.op);
    }
    break;
  case CurrentAction::SetMicrophone:
    // TODO
    logger->warn("Abort attempted on setMicrophone");
    break;
  case CurrentAction::Loopback:
    // TODO
    logger->warn("Abort attempted on loopback");
    break;
  }
  pa_mainloop_wakeup(mainloop.get());
  cur_act.action = CurrentAction::NoAction;
}
std::ostream &operator<<(std::ostream &out,
                         const PipeSourceVirtualMic::State value) {
  const char *s = 0;

#define PROCESS_VAL(c, p)                                                      \
  case (c):                                                                    \
    s = p;                                                                     \
    break;

  switch (value) {
    PROCESS_VAL(PipeSourceVirtualMic::State::InitContext, "InitContext");
    PROCESS_VAL(PipeSourceVirtualMic::State::WaitContextReady,
                "WaitContextReady");
    PROCESS_VAL(PipeSourceVirtualMic::State::InitCheckModuleLoaded,
                "InitCheckModuleLoaded");
    PROCESS_VAL(PipeSourceVirtualMic::State::WaitCheckModuleLoaded,
                "WaitCheckModuleLoaded");
    PROCESS_VAL(PipeSourceVirtualMic::State::InitModule, "InitModule");
    PROCESS_VAL(PipeSourceVirtualMic::State::WaitModuleReady,
                "WaitModuleReady");
    PROCESS_VAL(PipeSourceVirtualMic::State::InitRecStream, "InitRecStream");
    PROCESS_VAL(PipeSourceVirtualMic::State::WaitRecStreamReady,
                "WaitRecStreamReady");
    PROCESS_VAL(PipeSourceVirtualMic::State::Denoise, "Denoise");
  }
#undef PROCESS_VAL

  return out << s;
}
std::ostream &operator<<(std::ostream &out, const pa_source_state_t state) {
  const char *s;

#define PROCESS_VAL(c)                                                         \
  case (c):                                                                    \
    s = #c;                                                                    \
    break;

  switch (state) {
    PROCESS_VAL(PA_SOURCE_RUNNING);
    PROCESS_VAL(PA_SOURCE_SUSPENDED);
    PROCESS_VAL(PA_SOURCE_IDLE);
    PROCESS_VAL(PA_SOURCE_INVALID_STATE);
  default:
    s = "Unknown";
    break;
  }
#undef PROCESS_VAL
  return out << s;
}
optional<VirtualMicUpdate> PipeSourceVirtualMic::get_update() {
  lock_guard<mutex> lock(updates_mutex);
  if (!updates.empty()) {
    auto out = updates.front();
    updates.pop();
    return out;
  }
  return std::nullopt;
}
