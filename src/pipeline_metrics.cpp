/**
 * @file src/pipeline_metrics.cpp
 * @brief Definitions for bounded video pipeline metrics aggregation.
 */

#include "pipeline_metrics.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace video {
  namespace {
    double sanitize_duration_ms(double value) {
      return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }

    double nearest_rank(
      const std::array<double, pipeline_metrics_collector_t::max_window_samples> &sorted,
      std::size_t count,
      std::size_t percentile
    ) {
      if (count == 0) {
        return 0.0;
      }

      const auto rank = (percentile * count + 99) / 100;
      return sorted[std::max<std::size_t>(rank, 1) - 1];
    }

    template<typename Samples, typename Projection>
    pipeline_latency_metrics_t summarize(
      const Samples &samples,
      std::size_t count,
      Projection projection
    ) {
      std::array<double, pipeline_metrics_collector_t::max_window_samples> values {};
      std::transform(samples.begin(), samples.begin() + count, values.begin(), projection);
      std::sort(values.begin(), values.begin() + count);

      const auto sum = std::accumulate(values.begin(), values.begin() + count, 0.0);
      return {
        .mean_ms = count > 0 ? sum / static_cast<double>(count) : 0.0,
        .p50_ms = nearest_rank(values, count, 50),
        .p95_ms = nearest_rank(values, count, 95),
        .p99_ms = nearest_rank(values, count, 99),
      };
    }
  }  // namespace

  pipeline_metrics_collector_t::pipeline_metrics_collector_t(clock_t::time_point now):
      window_start_ {now},
      network_window_start_ {now} {
  }

  void pipeline_metrics_collector_t::reset(clock_t::time_point now) {
    sample_count_ = 0;
    network_sample_count_ = 0;
    window_frames_ = 0;
    window_bytes_ = 0;
    network_window_frames_ = 0;
    network_window_bytes_ = 0;
    network_window_data_shards_ = 0;
    network_window_fec_shards_ = 0;
    frames_encoded_ = 0;
    frames_dropped_ = 0;
    frames_replaced_before_encode_ = 0;
    frames_dropped_encode_ = 0;
    frames_dropped_encoded_queue_ = 0;
    frames_dropped_send_deadline_ = 0;
    frames_dropped_packet_deadline_ = 0;
    frames_dropped_recovery_wait_ = 0;
    frames_dropped_reference_superseded_ = 0;
    idr_requests_accepted_ = 0;
    idr_requests_rate_limited_ = 0;
    published_windows_ = 0;
    network_published_windows_ = 0;
    width_ = 0;
    height_ = 0;
    window_start_ = now;
    network_window_start_ = now;
    published_ = {};
    network_published_ = {};
  }

  void pipeline_metrics_collector_t::set_resolution(int width, int height) {
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    published_.width = width_;
    published_.height = height_;
  }

  void pipeline_metrics_collector_t::record_frame(
    duration_t encode_time,
    duration_t capture_to_encode_time,
    std::size_t packet_bytes,
    clock_t::time_point now
  ) {
    if (sample_count_ < samples_.size()) {
      samples_[sample_count_++] = {
        .encode_ms = sanitize_duration_ms(encode_time.count()),
        .capture_to_encode_ms = sanitize_duration_ms(capture_to_encode_time.count()),
      };
    }

    ++window_frames_;
    ++frames_encoded_;
    window_bytes_ += packet_bytes;

    if (now - window_start_ >= publish_interval) {
      publish(now);
    }
  }

  void pipeline_metrics_collector_t::record_drop(pipeline_drop_reason_e reason, uint64_t count) {
    frames_dropped_ += count;
    switch (reason) {
      case pipeline_drop_reason_e::capture_replaced:
        frames_replaced_before_encode_ += count;
        break;
      case pipeline_drop_reason_e::encode:
        frames_dropped_encode_ += count;
        break;
      case pipeline_drop_reason_e::encoded_queue:
        frames_dropped_encoded_queue_ += count;
        break;
      case pipeline_drop_reason_e::send_deadline:
        frames_dropped_send_deadline_ += count;
        break;
      case pipeline_drop_reason_e::packet_deadline:
        frames_dropped_packet_deadline_ += count;
        break;
      case pipeline_drop_reason_e::recovery_wait:
        frames_dropped_recovery_wait_ += count;
        break;
      case pipeline_drop_reason_e::reference_superseded:
        frames_dropped_reference_superseded_ += count;
        break;
    }

    published_.frames_dropped = frames_dropped_;
    published_.frames_replaced_before_encode = frames_replaced_before_encode_;
    published_.frames_dropped_encode = frames_dropped_encode_;
    published_.frames_dropped_encoded_queue = frames_dropped_encoded_queue_;
    published_.frames_dropped_send_deadline =
      frames_dropped_send_deadline_;
    published_.frames_dropped_packet_deadline =
      frames_dropped_packet_deadline_;
    published_.frames_dropped_recovery_wait =
      frames_dropped_recovery_wait_;
    published_.frames_dropped_reference_superseded =
      frames_dropped_reference_superseded_;
  }

  void pipeline_metrics_collector_t::record_idr_request(
    bool accepted,
    uint64_t count
  ) {
    if (accepted) {
      idr_requests_accepted_ += count;
    } else {
      idr_requests_rate_limited_ += count;
    }

    published_.idr_requests_accepted =
      idr_requests_accepted_;
    published_.idr_requests_rate_limited =
      idr_requests_rate_limited_;
  }

  void pipeline_metrics_collector_t::record_network_frame(
    duration_t send_queue_time,
    duration_t packetization_time,
    duration_t fec_time,
    duration_t pacer_time,
    duration_t send_time,
    duration_t capture_to_last_send_time,
    std::size_t wire_bytes,
    uint64_t data_shards,
    uint64_t fec_shards,
    clock_t::time_point now
  ) {
    if (network_sample_count_ < network_samples_.size()) {
      network_samples_[network_sample_count_++] = {
        .send_queue_ms = sanitize_duration_ms(send_queue_time.count()),
        .packetization_ms = sanitize_duration_ms(packetization_time.count()),
        .fec_ms = sanitize_duration_ms(fec_time.count()),
        .pacer_ms = sanitize_duration_ms(pacer_time.count()),
        .send_ms = sanitize_duration_ms(send_time.count()),
        .capture_to_last_send_ms = sanitize_duration_ms(capture_to_last_send_time.count()),
      };
    }

    ++network_window_frames_;
    network_window_bytes_ += wire_bytes;
    network_window_data_shards_ += data_shards;
    network_window_fec_shards_ += fec_shards;

    if (now - network_window_start_ >= publish_interval) {
      publish_network(now);
    }
  }

  pipeline_metrics_t pipeline_metrics_collector_t::snapshot() const {
    auto snapshot = published_;
    snapshot.network = network_published_;
    return snapshot;
  }

  void pipeline_metrics_collector_t::publish(clock_t::time_point now) {
    const auto elapsed = std::chrono::duration<double>(now - window_start_).count();
    if (elapsed <= 0.0 || window_frames_ == 0 || sample_count_ == 0) {
      window_start_ = now;
      return;
    }

    const auto encode = summarize(samples_, sample_count_, [](const frame_sample_t &sample) {
      return sample.encode_ms;
    });
    const auto capture_to_encode = summarize(samples_, sample_count_, [](const frame_sample_t &sample) {
      return sample.capture_to_encode_ms;
    });

    published_ = {
      .valid = true,
      .encode_ms = encode.mean_ms,
      .encode_p50_ms = encode.p50_ms,
      .encode_p95_ms = encode.p95_ms,
      .encode_p99_ms = encode.p99_ms,
      .capture_to_encode_ms = capture_to_encode.mean_ms,
      .capture_to_encode_p50_ms = capture_to_encode.p50_ms,
      .capture_to_encode_p95_ms = capture_to_encode.p95_ms,
      .capture_to_encode_p99_ms = capture_to_encode.p99_ms,
      .fps = static_cast<double>(window_frames_) / elapsed,
      .bitrate_kbps = (static_cast<double>(window_bytes_) * 8.0 / 1000.0) / elapsed,
      .window_sequence = ++published_windows_,
      .window_duration_ms = elapsed * 1000.0,
      .window_frames = window_frames_,
      .sampled_frames = sample_count_,
      .frames_encoded = frames_encoded_,
      .frames_dropped = frames_dropped_,
      .frames_replaced_before_encode = frames_replaced_before_encode_,
      .frames_dropped_encode = frames_dropped_encode_,
      .frames_dropped_encoded_queue = frames_dropped_encoded_queue_,
      .frames_dropped_send_deadline =
        frames_dropped_send_deadline_,
      .frames_dropped_packet_deadline =
        frames_dropped_packet_deadline_,
      .frames_dropped_recovery_wait =
        frames_dropped_recovery_wait_,
      .frames_dropped_reference_superseded =
        frames_dropped_reference_superseded_,
      .idr_requests_accepted = idr_requests_accepted_,
      .idr_requests_rate_limited =
        idr_requests_rate_limited_,
      .width = width_,
      .height = height_,
    };

    sample_count_ = 0;
    window_frames_ = 0;
    window_bytes_ = 0;
    window_start_ = now;
  }

  void pipeline_metrics_collector_t::publish_network(clock_t::time_point now) {
    const auto elapsed = std::chrono::duration<double>(now - network_window_start_).count();
    if (elapsed <= 0.0 || network_window_frames_ == 0 || network_sample_count_ == 0) {
      network_window_start_ = now;
      return;
    }

    network_published_ = {
      .valid = true,
      .send_queue = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.send_queue_ms;
      }),
      .packetization = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.packetization_ms;
      }),
      .fec = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.fec_ms;
      }),
      .pacer = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.pacer_ms;
      }),
      .send = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.send_ms;
      }),
      .capture_to_last_send = summarize(network_samples_, network_sample_count_, [](const network_sample_t &sample) {
        return sample.capture_to_last_send_ms;
      }),
      .wire_bitrate_kbps = (static_cast<double>(network_window_bytes_) * 8.0 / 1000.0) / elapsed,
      .fec_overhead_percent = network_window_data_shards_ > 0 ? static_cast<double>(network_window_fec_shards_) * 100.0 / static_cast<double>(network_window_data_shards_) : 0.0,
      .window_sequence = ++network_published_windows_,
      .window_duration_ms = elapsed * 1000.0,
      .window_frames = network_window_frames_,
      .sampled_frames = network_sample_count_,
      .data_shards = network_window_data_shards_,
      .fec_shards = network_window_fec_shards_,
    };

    network_sample_count_ = 0;
    network_window_frames_ = 0;
    network_window_bytes_ = 0;
    network_window_data_shards_ = 0;
    network_window_fec_shards_ = 0;
    network_window_start_ = now;
  }

  pipeline_metrics_registry_t::pipeline_metrics_registry_t(clock_t::time_point now):
      aggregate_ {now} {
  }

  bool pipeline_metrics_registry_t::register_session(session_id_t session_id, clock_t::time_point now) {
    if (session_id == 0) {
      return false;
    }
    if (find(session_id)) {
      return true;
    }

    auto entry = std::find_if(entries_.begin(), entries_.end(), [](const entry_t &candidate) {
      return !candidate.active;
    });
    if (entry == entries_.end()) {
      return false;
    }

    if (active_sessions_ == 0) {
      aggregate_.reset(now);
      aggregate_resolution_set_ = false;
      aggregate_resolution_mixed_ = false;
      aggregate_width_ = 0;
      aggregate_height_ = 0;
    }

    entry->active = true;
    entry->session_id = session_id;
    entry->collector.reset(now);
    ++active_sessions_;
    return true;
  }

  bool pipeline_metrics_registry_t::unregister_session(session_id_t session_id) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->active = false;
    entry->session_id = 0;
    entry->collector.reset();
    --active_sessions_;
    return true;
  }

  void pipeline_metrics_registry_t::reset(clock_t::time_point now) {
    for (auto &entry : entries_) {
      entry.active = false;
      entry.session_id = 0;
      entry.collector.reset(now);
    }
    aggregate_.reset(now);
    active_sessions_ = 0;
    aggregate_resolution_set_ = false;
    aggregate_resolution_mixed_ = false;
    aggregate_width_ = 0;
    aggregate_height_ = 0;
  }

  bool pipeline_metrics_registry_t::set_resolution(session_id_t session_id, int width, int height) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->collector.set_resolution(width, height);
    if (!aggregate_resolution_set_) {
      aggregate_width_ = std::max(width, 0);
      aggregate_height_ = std::max(height, 0);
      aggregate_resolution_set_ = true;
      aggregate_.set_resolution(aggregate_width_, aggregate_height_);
    } else if (width != aggregate_width_ || height != aggregate_height_) {
      aggregate_resolution_mixed_ = true;
      aggregate_.set_resolution(0, 0);
    } else if (!aggregate_resolution_mixed_) {
      aggregate_.set_resolution(aggregate_width_, aggregate_height_);
    }
    return true;
  }

  bool pipeline_metrics_registry_t::record_frame(
    session_id_t session_id,
    duration_t encode_time,
    duration_t capture_to_encode_time,
    std::size_t packet_bytes,
    clock_t::time_point now
  ) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->collector.record_frame(encode_time, capture_to_encode_time, packet_bytes, now);
    aggregate_.record_frame(encode_time, capture_to_encode_time, packet_bytes, now);
    return true;
  }

  bool pipeline_metrics_registry_t::record_drop(session_id_t session_id, pipeline_drop_reason_e reason, uint64_t count) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->collector.record_drop(reason, count);
    aggregate_.record_drop(reason, count);
    return true;
  }

  bool pipeline_metrics_registry_t::record_idr_request(
    session_id_t session_id,
    bool accepted,
    uint64_t count
  ) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->collector.record_idr_request(accepted, count);
    aggregate_.record_idr_request(accepted, count);
    return true;
  }

  bool pipeline_metrics_registry_t::record_network_frame(
    session_id_t session_id,
    duration_t send_queue_time,
    duration_t packetization_time,
    duration_t fec_time,
    duration_t pacer_time,
    duration_t send_time,
    duration_t capture_to_last_send_time,
    std::size_t wire_bytes,
    uint64_t data_shards,
    uint64_t fec_shards,
    clock_t::time_point now
  ) {
    auto entry = find(session_id);
    if (!entry) {
      return false;
    }

    entry->collector.record_network_frame(
      send_queue_time,
      packetization_time,
      fec_time,
      pacer_time,
      send_time,
      capture_to_last_send_time,
      wire_bytes,
      data_shards,
      fec_shards,
      now
    );
    aggregate_.record_network_frame(
      send_queue_time,
      packetization_time,
      fec_time,
      pacer_time,
      send_time,
      capture_to_last_send_time,
      wire_bytes,
      data_shards,
      fec_shards,
      now
    );
    return true;
  }

  std::optional<pipeline_metrics_t> pipeline_metrics_registry_t::snapshot(session_id_t session_id) const {
    const auto entry = find(session_id);
    if (!entry) {
      return std::nullopt;
    }
    return entry->collector.snapshot();
  }

  pipeline_metrics_t pipeline_metrics_registry_t::aggregate_snapshot() const {
    return aggregate_.snapshot();
  }

  std::size_t pipeline_metrics_registry_t::active_sessions() const {
    return active_sessions_;
  }

  pipeline_metrics_registry_t::entry_t *pipeline_metrics_registry_t::find(session_id_t session_id) {
    auto entry = std::find_if(entries_.begin(), entries_.end(), [session_id](const entry_t &candidate) {
      return candidate.active && candidate.session_id == session_id;
    });
    return entry == entries_.end() ? nullptr : &*entry;
  }

  const pipeline_metrics_registry_t::entry_t *pipeline_metrics_registry_t::find(session_id_t session_id) const {
    auto entry = std::find_if(entries_.begin(), entries_.end(), [session_id](const entry_t &candidate) {
      return candidate.active && candidate.session_id == session_id;
    });
    return entry == entries_.end() ? nullptr : &*entry;
  }

}  // namespace video
