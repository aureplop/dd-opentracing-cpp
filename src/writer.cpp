#include "writer.h"
#include <iostream>
#include "version_number.h"

namespace datadog {
namespace opentracing {

namespace {
const std::string agent_api_path = "/v0.3/traces";
const std::string agent_protocol = "https://";
// Max amount of time to wait between sending spans to agent. Agent discards spans older than 10s,
// so that is the upper bound.
const std::chrono::milliseconds default_write_period = std::chrono::seconds(1);
const size_t max_queued_messages = 7000;
}  // namespace

template <class Message>
AgentWriter<Message>::AgentWriter(std::string host, uint32_t port)
    : AgentWriter(std::unique_ptr<Handle>{new CurlHandle{}}, config::tracer_version,
                  default_write_period, max_queued_messages, host, port){};

template <class Message>
AgentWriter<Message>::AgentWriter(std::unique_ptr<Handle> handle, std::string tracer_version,
                                  std::chrono::milliseconds write_period,
                                  size_t max_queued_messages, std::string host, uint32_t port)
    : tracer_version_(tracer_version),
      write_period_(write_period),
      max_queued_messages_(max_queued_messages) {
  setUpHandle(handle, host, port);
  startWriting(std::move(handle));
}

template <class Message>
void AgentWriter<Message>::setUpHandle(std::unique_ptr<Handle> &handle, std::string host,
                                       uint32_t port) {
  // Some options are the same for all actions, set them here.
  // Set the agent URI.
  std::stringstream agent_uri;
  agent_uri << agent_protocol << host << ":" << port << agent_api_path;
  auto rcode = handle->setopt(CURLOPT_URL, agent_uri.str().c_str());
  if (rcode != CURLE_OK) {
    throw std::runtime_error(std::string("Unable to set agent URL: ") + curl_easy_strerror(rcode));
  }
  // Set the common HTTP headers.
  rcode = handle->appendHeaders({"Content-Type: application/msgpack", "Datadog-Meta-Lang: cpp",
                                 "Datadog-Meta-Tracer-Version: " + tracer_version_});
  if (rcode != CURLE_OK) {
    throw std::runtime_error(std::string("Unable to set agent connection headers: ") +
                             curl_easy_strerror(rcode));
  }
}

template <class Message>
AgentWriter<Message>::~AgentWriter() {
  stop();
}

template <class Message>
void AgentWriter<Message>::stop() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_writing_) {
      return;  // Already stopped.
    }
    stop_writing_ = true;
  }
  condition_.notify_all();
  worker_->join();
}

template <class Message>
void AgentWriter<Message>::write(Message &&message) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_writing_) {
    return;
  }
  if (messages_.size() >= max_queued_messages_) {
    return;
  }
  messages_.push_back(std::move(message));
};

template <class Message>
void AgentWriter<Message>::startWriting(std::unique_ptr<Handle> handle) {
  // Start worker that sends Messages to agent.
  // We can capture 'this' because destruction of this stops the thread and the lambda.
  worker_ = std::make_unique<std::thread>(
      [this](std::unique_ptr<Handle> handle) {
        std::stringstream buffer;
        size_t num_messages = 0;
        while (true) {
          // Encode messages when there are new ones.
          {
            // Wait to be told about new messages (or to stop).
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, write_period_,
                                [&]() -> bool { return flush_worker_ || stop_writing_; });
            if (stop_writing_) {
              return;  // Stop the thread.
            }
            num_messages = messages_.size();
            if (num_messages == 0) {
              continue;
            }
            // Clear the buffer but keep the allocated memory.
            buffer.clear();
            buffer.str(std::string{});
            // Why does the agent want extra nesting?
            std::array<std::reference_wrapper<std::deque<Message>>, 1> wrapped_messages{messages_};
            msgpack::pack(buffer, wrapped_messages);
            messages_.clear();
          }  // lock on mutex_ ends.
          // Send messages, not in critical period.
          AgentWriter<Message>::postMessages(handle, buffer, num_messages);
          // Let thread calling 'flush' that we're done flushing.
          {
            std::unique_lock<std::mutex> lock(mutex_);
            flush_worker_ = false;
          }
          condition_.notify_all();
        }
      },
      std::move(handle));
}  // namespace opentracing

template <class Message>
void AgentWriter<Message>::flush() try {
  std::unique_lock<std::mutex> lock(mutex_);
  flush_worker_ = true;
  condition_.notify_all();
  // Wait until flush is complete.
  condition_.wait(lock, [&]() -> bool { return !flush_worker_ || stop_writing_; });
} catch (const std::bad_alloc &) {
}

template <class Message>
void AgentWriter<Message>::postMessages(std::unique_ptr<Handle> &handle, std::stringstream &buffer,
                                        size_t num_messages) try {
  auto rcode = handle->appendHeaders({"X-Datadog-Trace-Count: " + std::to_string(num_messages)});
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent communication headers: " << curl_easy_strerror(rcode)
              << std::endl;
    return;
  }

  // We have to set the size manually, because msgpack uses null characters.
  std::string post_fields = buffer.str();
  rcode = handle->setopt(CURLOPT_POSTFIELDSIZE, post_fields.size());
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent request size: " << curl_easy_strerror(rcode) << std::endl;
    return;
  }

  rcode = handle->setopt(CURLOPT_POSTFIELDS, post_fields.data());
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent request body: " << curl_easy_strerror(rcode) << std::endl;
    return;
  }

  rcode = handle->perform();
  if (rcode != CURLE_OK) {
    std::cerr << "Error sending traces to agent: " << curl_easy_strerror(rcode) << std::endl
              << handle->getError() << std::endl;
    return;
  }
} catch (const std::bad_alloc &) {
  // Drop messages, but live to fight another day.
}

// Make sure we generate code for a Span-writing Writer.
template class AgentWriter<Span>;

}  // namespace opentracing
}  // namespace datadog