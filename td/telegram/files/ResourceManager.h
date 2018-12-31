//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2017
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/telegram/files/FileLoaderActor.h"
#include "td/telegram/files/ResourceState.h"

#include "td/utils/Container.h"
#include "td/utils/Heap.h"

#include <utility>

namespace td {
class ResourceManager : public Actor {
 public:
  enum class Mode { Baseline, Greedy };
  explicit ResourceManager(Mode mode) : mode_(mode) {
  }
  // use through ActorShared
  void update_priority(int32 priority);
  void update_resources(const ResourceState &resource_state);

  void register_worker(ActorShared<FileLoaderActor> callback, int32 priority);

 private:
  Mode mode_;
  using NodeId = uint64;
  struct Node : public HeapNode {
    NodeId node_id;

    ResourceState resource_state_;
    ActorShared<FileLoaderActor> callback_;

    HeapNode *as_heap_node() {
      return static_cast<HeapNode *>(this);
    }
    static Node *from_heap_node(HeapNode *heap_node) {
      return static_cast<Node *>(heap_node);
    }
  };

  Container<std::unique_ptr<Node>> nodes_container_;
  vector<std::pair<int32, NodeId>> to_xload_;
  KHeap<int64> by_estimated_extra_;
  ResourceState resource_state_;

  ActorShared<> parent_;
  bool stop_flag_ = false;

  void hangup_shared() override;

  void loop() override;

  void add_to_heap(Node *node);
  bool satisfy_node(NodeId file_node_id);
  void add_node(NodeId node_id, int32 priority);
  bool remove_node(NodeId node_id);
};
}  // namespace td