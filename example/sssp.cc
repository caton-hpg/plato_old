/*
  Tencent is pleased to support the open source community by making
  Plato available.
  Copyright (C) 2019 THL A29 Limited, a Tencent company.
  All rights reserved.

  Licensed under the BSD 3-Clause License (the "License"); you may
  not use this file except in compliance with the License. You may
  obtain a copy of the License at

  https://opensource.org/licenses/BSD-3-Clause

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" basis,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
  implied. See the License for the specific language governing
  permissions and limitations under the License.

  See the AUTHORS file for names of contributors.
*/

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

#include "glog/logging.h"
#include "gflags/gflags.h"

#include "boost/format.hpp"
#include "boost/iostreams/stream.hpp"
#include "boost/iostreams/filter/gzip.hpp"
#include "boost/iostreams/filtering_stream.hpp"
#include "nebula/client/Init.h"
#include "nebula/client/Config.h"
#include "nebula/client/ConnectionPool.h"

#include "plato/util/perf.hpp"
#include "plato/util/hdfs.hpp"
#include "plato/util/atomic.hpp"
#include "plato/util/nebula_writer.h"
#include "plato/graph/base.hpp"
#include "plato/graph/state.hpp"
#include "plato/graph/structure.hpp"
#include "plato/graph/message_passing.hpp"

DEFINE_string(input,       "",      "input file, in csv format, without edge data");
DEFINE_string(output,      "",      "output directory");
DEFINE_bool(is_directed,   false,   "is graph directed or not");
DEFINE_bool(need_encode,   false,                    "");
DEFINE_string(vtype,       "uint32",                 "");
DEFINE_string(encoder,     "single","single or distributed vid encoder");
DEFINE_bool(part_by_in,    false,   "partition by in-degree");
DEFINE_int32(alpha,        -1,      "alpha value used in sequence balance partition");
DEFINE_string(source,      "",      "source");
DEFINE_uint32(iterations,   10,      "iterations");

bool string_not_empty(const char*, const std::string& value) {
  if (0 == value.length()) { return false; }
  return true;
}

DEFINE_validator(input,  &string_not_empty);
DEFINE_validator(output, &string_not_empty);
DEFINE_validator(source, &string_not_empty);

void init(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::LogToStderr();
}

template <typename VID_T>
inline typename std::enable_if<std::is_integral<VID_T>::value, VID_T>::type get_source_vid(const std::string& source) {
  return (VID_T)std::strtoul(FLAGS_source.c_str(), nullptr, 0);
}

template <typename VID_T>
inline typename std::enable_if<!std::is_integral<VID_T>::value, VID_T>::type get_source_vid(const std::string& source) {
  return source;
}

template <typename VID_T>
void run_sssp() {
  plato::stop_watch_t watch;
  auto& cluster_info = plato::cluster_info_t::get_instance();

  watch.mark("t0");

  VID_T source = get_source_vid<VID_T>(FLAGS_source);

  using edge_value_t = float;

  plato::vid_encoder_t<edge_value_t, VID_T> single_data_encoder;
  plato::distributed_vid_encoder_t<edge_value_t, VID_T> distributed_data_encoder;

  plato::vencoder_t<edge_value_t, VID_T> encoder_ptr = nullptr;
  if (FLAGS_need_encode) {
    if (FLAGS_encoder == "single") {
      encoder_ptr = &single_data_encoder;
    } else {
      encoder_ptr = &distributed_data_encoder;
    }
  }

  encoder_ptr->set_vids({source});

  // init graph
  plato::graph_info_t graph_info(FLAGS_is_directed);
  plato::decoder_with_default_t<edge_value_t> decoder((edge_value_t)1);
  auto pdcsc = plato::create_dcsc_seqs_from_path<edge_value_t, VID_T>( // seqs means seq part_by_src [master src]->[mirror dst] dense
    &graph_info, FLAGS_input, plato::edge_format_t::CSV,
    decoder, FLAGS_alpha, FLAGS_part_by_in, encoder_ptr
  );

  std::vector<plato::vid_t> encoded_vids;
  encoder_ptr->get_vids(encoded_vids);
  DCHECK(encoded_vids.size() == 1);
  plato::vid_t root = encoded_vids[0];

  using graph_spec_t         = typename std::remove_reference<decltype(*pdcsc)>::type;
  using partition_t          = typename graph_spec_t::partition_t;
  using adj_unit_list_spec_t = typename graph_spec_t::adj_unit_list_spec_t;
  using rank_state_t         = typename plato::dense_state_t<double, partition_t>;

  // init state

  std::shared_ptr<rank_state_t> distance(new rank_state_t(graph_info.max_v_i_, pdcsc->partitioner()));

  watch.mark("t1");

  watch.mark("t2"); // do computation

  distance->fill(1e9);
  (*distance)[root] = 0.0;

  using context_spec_t = plato::mepa_ag_context_t<double>;
  using message_spec_t = plato::mepa_ag_message_t<double>;
  
  for (uint32_t epoch_i = 0; epoch_i < FLAGS_iterations; ++epoch_i) {

    watch.mark("t1");

    if (0 == cluster_info.partition_id_) {
      LOG(INFO) << "[epoch-" << epoch_i  << "] init-next cost: "
        << watch.show("t1") / 1000.0 << "s";
    }

    watch.mark("t1");
    // pagerank 只用dense模式
    plato::aggregate_message<double, int, graph_spec_t> (*pdcsc,
      [&](const context_spec_t& context, plato::vid_t v_i, const adj_unit_list_spec_t& adjs) {
        double min_distance = 1e9;
        double cur_distance = 0.0;
        for (auto it = adjs.begin_; adjs.end_ != it; ++it) {
          cur_distance = (*distance)[it->neighbour_] + it->edata_;
          if (cur_distance < min_distance) {
            min_distance = cur_distance;
          }
        }
        context.send(message_spec_t { v_i, min_distance });
      },
      [&](int /*p_i*/, message_spec_t& msg) {
        plato::write_min(&(*distance)[msg.v_i_], msg.message_);
        return 0;
      }
    );

    if (0 == cluster_info.partition_id_) {
      LOG(INFO) << "[epoch-" << epoch_i  << "] message-passing cost: "
        << watch.show("t1") / 1000.0 << "s";
    }
  }

  if (0 == cluster_info.partition_id_) {
    LOG(INFO) << "iteration done, cost: " << watch.show("t2") / 1000.0 << "s";
    LOG(INFO) << "whole cost: " << watch.show("t0") / 1000.0 << "s";
  }

  watch.mark("t1");
  {
    if (!boost::starts_with(FLAGS_output, "nebula:")) {
      plato::thread_local_fs_output os(FLAGS_output, (boost::format("%04d_") % cluster_info.partition_id_).str(), true);
      distance->template foreach<int> (
        [&](plato::vid_t v_i, double* pval) {
          auto& fs_output = os.local();
          if (encoder_ptr != nullptr) {
            fs_output << encoder_ptr->decode(v_i) << "," << *pval << "\n";
          } else {
            fs_output << v_i << "," << *pval << "\n";
          }
          return 0;
        }
      );
    } else {
      if (encoder_ptr != nullptr) {
        struct Item {
          VID_T vid;
          double pval;
          std::string toString() const {
            return std::to_string(pval);
          }
        };
        plato::thread_local_nebula_writer<Item> writer(FLAGS_output);
        LOG(INFO) << "thread_local_nebula_writer is constructed....";
        distance->template foreach<int> (
          [&](plato::vid_t v_i, double* pval) {
            auto& buffer = writer.local();
            buffer.add(Item{encoder_ptr->decode(v_i), *pval});
            return 0;
          }
        );
      } else {
        struct Item {
          plato::vid_t vid;
          double pval;
          std::string toString() const {
            return std::to_string(pval);
          }
        };
        plato::thread_local_nebula_writer<Item> writer(FLAGS_output);
        distance->template foreach<int> (
          [&](plato::vid_t v_i, double* pval) {
            auto& buffer = writer.local();
            buffer.add(Item{v_i, *pval});
            return 0;
          }
        );
      }
    }
  }
  if (0 == cluster_info.partition_id_) {
    LOG(INFO) << "save result cost: " << watch.show("t1") / 1000.0 << "s";
  }

  plato::mem_status_t mstatus;
  plato::self_mem_usage(&mstatus);
  LOG(INFO) << "memory usage: " << (double)mstatus.vm_rss / 1024.0 << " MBytes";
  LOG(INFO) << "pagerank done const: " << watch.show("t0") / 1000.0 << "s";
}

int main(int argc, char** argv) {
  auto& cluster_info = plato::cluster_info_t::get_instance();

  //nebula::init(&argc, &argv);
  init(argc, argv);
  cluster_info.initialize(&argc, &argv);

  if (FLAGS_vtype == "uint32") {
    run_sssp<uint32_t>();
  } else if (FLAGS_vtype == "int32")  {
    run_sssp<int32_t>();
  } else if (FLAGS_vtype == "uint64") {
    run_sssp<uint64_t>();
  } else if (FLAGS_vtype == "int64") {
    run_sssp<int64_t>();
  } else if (FLAGS_vtype == "string") {
    run_sssp<std::string>();
  } else {
    LOG(FATAL) << "unknown vtype: " << FLAGS_vtype;
  }

  return 0;
}

