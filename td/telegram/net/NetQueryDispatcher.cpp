//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2017
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/net/NetQueryDispatcher.h"

#include "td/telegram/net/DcAuthManager.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/net/NetQueryDelayer.h"
#include "td/telegram/net/PublicRsaKeyWatchdog.h"
#include "td/telegram/net/SessionMultiProxy.h"

#include "td/telegram/ConfigShared.h"
#include "td/telegram/Global.h"
#include "td/telegram/Td.h"

#include "td/db/Pmc.h"

#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/thread.h"
#include "td/utils/Slice.h"

#include <algorithm>

namespace td {
void NetQueryDispatcher::dispatch(NetQueryPtr net_query) {
  net_query->debug("dispatch");
  if (stop_flag_.load(std::memory_order_relaxed)) {
    // Set error to avoid warning
    // No need to send result somewhere, it probably will be ignored anyway
    net_query->set_error(Status::Error(500, "Internal Server Error: closing"));
    net_query->clear();
    net_query.reset();
    // G()->net_query_creator().release(std::move(net_query));
    return;
  }

  if (net_query->is_ready()) {
    if (net_query->is_error()) {
      auto code = net_query->error().code();
      if (code == 303) {
        try_fix_migrate(net_query);
      } else if (code == NetQuery::Resend) {
        net_query->resend();
      } else if (code < 0 || code == 500 || code == 420) {
        net_query->debug("sent to NetQueryDelayer");
        return send_closure(delayer_, &NetQueryDelayer::delay, std::move(net_query));
      }
    }
  }

  if (!net_query->is_ready()) {
    if (net_query->dispatch_ttl == 0) {
      net_query->set_error(Status::Error("DispatchTtlError"));
    }
  }

  auto dest_dc_id = net_query->dc_id();
  if (dest_dc_id.is_main()) {
    dest_dc_id = DcId::internal(main_dc_id_.load(std::memory_order_relaxed));
  }
  if (!net_query->is_ready() && wait_dc_init(dest_dc_id, true).is_error()) {
    net_query->set_error(Status::Error(PSLICE() << "No such dc " << dest_dc_id));
  }

  if (net_query->is_ready()) {
    auto callback = net_query->move_callback();
    if (callback.empty()) {
      net_query->debug("sent to td (no callback)");
      send_closure(G()->td(), &NetQueryCallback::on_result, std::move(net_query));
    } else {
      net_query->debug("sent to callback", true);
      send_closure(std::move(callback), &NetQueryCallback::on_result, std::move(net_query));
    }
    return;
  }

  if (net_query->dispatch_ttl > 0) {
    net_query->dispatch_ttl--;
  }

  size_t dc_pos = static_cast<size_t>(dest_dc_id.get_raw_id() - 1);
  CHECK(dc_pos < dcs_.size());
  switch (net_query->type()) {
    case NetQuery::Type::Common:
      net_query->debug(PSTRING() << "sent to main session multi proxy " << dest_dc_id);
      send_closure_later(dcs_[dc_pos].main_session_, &SessionMultiProxy::send, std::move(net_query));
      break;
    case NetQuery::Type::Upload:
      net_query->debug(PSTRING() << "sent to upload session multi proxy " << dest_dc_id);
      send_closure_later(dcs_[dc_pos].upload_session_, &SessionMultiProxy::send, std::move(net_query));
      break;
    case NetQuery::Type::Download:
      net_query->debug(PSTRING() << "sent to download session multi proxy " << dest_dc_id);
      send_closure_later(dcs_[dc_pos].download_session_, &SessionMultiProxy::send, std::move(net_query));
      break;
    case NetQuery::Type::DownloadSmall:
      net_query->debug(PSTRING() << "sent to download small session multi proxy " << dest_dc_id);
      send_closure_later(dcs_[dc_pos].download_small_session_, &SessionMultiProxy::send, std::move(net_query));
      break;
  }
}

Status NetQueryDispatcher::wait_dc_init(DcId dc_id, bool force) {
  // TODO: optimize
  if (!dc_id.is_exact()) {
    return Status::Error("Not exact DC");
  }
  size_t pos = static_cast<size_t>(dc_id.get_raw_id() - 1);
  if (pos >= dcs_.size()) {
    return Status::Error("Too big DC id");
  }
  auto &dc = dcs_[pos];

  bool should_init = false;
  if (!dc.is_valid_) {
    if (!force) {
      return Status::Error("Invalid DC");
    }
    bool expected = false;
    should_init =
        dc.is_valid_.compare_exchange_strong(expected, true, std::memory_order_seq_cst, std::memory_order_seq_cst);
  }

  if (should_init) {
    std::lock_guard<std::mutex> guard(main_dc_id_mutex_);
    if (stop_flag_.load(std::memory_order_relaxed)) {
      return Status::Error("Closing");
    }
    // init dc
    decltype(common_public_rsa_key_) public_rsa_key;
    bool is_cdn = false;
    if (dc_id.is_internal()) {
      public_rsa_key = common_public_rsa_key_;
    } else {
      public_rsa_key = std::make_shared<PublicRsaKeyShared>(dc_id);
      send_closure_later(public_rsa_key_watchdog_, &PublicRsaKeyWatchdog::add_public_rsa_key, public_rsa_key);
      is_cdn = true;
    }
    auto auth_data = AuthDataShared::create(dc_id, std::move(public_rsa_key));
    int32 session_count = get_session_count();
    bool use_pfs = get_use_pfs();

    int32 slow_net_scheduler_id = G()->get_slow_net_scheduler_id();

    auto raw_dc_id = dc_id.get_raw_id();
    dc.main_session_ = create_actor<SessionMultiProxy>(PSLICE() << "SessionMultiProxy:" << raw_dc_id << ":main",
                                                       session_count, auth_data, raw_dc_id == main_dc_id_,
                                                       use_pfs || (session_count > 1), false, false, is_cdn);
    dc.upload_session_ = create_actor_on_scheduler<SessionMultiProxy>(
        PSLICE() << "SessionMultiProxy:" << raw_dc_id << ":upload", slow_net_scheduler_id,
        raw_dc_id != 2 && raw_dc_id != 4 ? 8 : 4, auth_data, false, use_pfs || (session_count > 1), false, true,
        is_cdn);
    dc.download_session_ = create_actor_on_scheduler<SessionMultiProxy>(
        PSLICE() << "SessionMultiProxy:" << raw_dc_id << ":download", slow_net_scheduler_id, 1, auth_data, false,
        use_pfs, true, true, is_cdn);
    dc.download_small_session_ = create_actor_on_scheduler<SessionMultiProxy>(
        PSLICE() << "SessionMultiProxy:" << raw_dc_id << ":download_small", slow_net_scheduler_id, 1, auth_data, false,
        use_pfs, true, true, is_cdn);
    dc.is_inited_ = true;
    if (dc_id.is_internal()) {
      send_closure_later(dc_auth_manager_, &DcAuthManager::add_dc, std::move(auth_data));
    }
  } else {
    while (!dc.is_inited_) {
      if (stop_flag_.load(std::memory_order_relaxed)) {
        return Status::Error("Closing");
      }
#if !TD_THREAD_UNSUPPORTED
      td::this_thread::yield();
#endif
    }
  }
  return Status::OK();
}

void NetQueryDispatcher::dispatch_with_callback(NetQueryPtr net_query, ActorShared<NetQueryCallback> callback) {
  net_query->set_callback(std::move(callback));
  dispatch(std::move(net_query));
}

void NetQueryDispatcher::stop() {
  std::lock_guard<std::mutex> guard(main_dc_id_mutex_);
  stop_flag_ = true;
  delayer_.hangup();
  for (const auto &dc : dcs_) {
    dc.main_session_.hangup();
    dc.upload_session_.hangup();
    dc.download_session_.hangup();
    dc.download_small_session_.hangup();
  }
  public_rsa_key_watchdog_.reset();
  dc_auth_manager_.reset();
}

void NetQueryDispatcher::update_session_count() {
  std::lock_guard<std::mutex> guard(main_dc_id_mutex_);
  int32 session_count = get_session_count();
  bool use_pfs = get_use_pfs();
  for (size_t i = 1; i < MAX_DC_COUNT; i++) {
    if (is_dc_inited(narrow_cast<int32>(i))) {
      send_closure_later(dcs_[i - 1].main_session_, &SessionMultiProxy::update_options, session_count,
                         use_pfs || (session_count > 1));
    }
  }
}

void NetQueryDispatcher::update_use_pfs() {
  std::lock_guard<std::mutex> guard(main_dc_id_mutex_);
  int32 session_count = get_session_count();
  bool use_pfs = get_use_pfs();
  for (size_t i = 1; i < MAX_DC_COUNT; i++) {
    if (is_dc_inited(narrow_cast<int32>(i))) {
      send_closure_later(dcs_[i - 1].main_session_, &SessionMultiProxy::update_use_pfs, use_pfs || (session_count > 1));
      send_closure_later(dcs_[i - 1].upload_session_, &SessionMultiProxy::update_use_pfs, use_pfs);
      send_closure_later(dcs_[i - 1].download_session_, &SessionMultiProxy::update_use_pfs, use_pfs);
      send_closure_later(dcs_[i - 1].download_small_session_, &SessionMultiProxy::update_use_pfs, use_pfs);
    }
  }
}
void NetQueryDispatcher::update_valid_dc(DcId dc_id) {
  wait_dc_init(dc_id, true);
}

bool NetQueryDispatcher::is_dc_inited(int32 raw_dc_id) {
  return dcs_[raw_dc_id - 1].is_valid_.load(std::memory_order_relaxed);
}
int32 NetQueryDispatcher::get_session_count() {
  return std::max(G()->shared_config().get_option_integer("session_count"), 1);
}

bool NetQueryDispatcher::get_use_pfs() {
  return G()->shared_config().get_option_boolean("use_pfs");
}

NetQueryDispatcher::NetQueryDispatcher(std::function<ActorShared<>()> create_reference) {
  auto s_main_dc_id = G()->td_db()->get_binlog_pmc()->get("main_dc_id");
  if (!s_main_dc_id.empty()) {
    main_dc_id_ = to_integer<int32>(s_main_dc_id);
  }
  LOG(INFO) << tag("main_dc_id", main_dc_id_.load(std::memory_order_relaxed));
  delayer_ = create_actor<NetQueryDelayer>("NetQueryDelayer", create_reference());
  dc_auth_manager_ = create_actor<DcAuthManager>("DcAuthManager", create_reference());
  common_public_rsa_key_ = std::make_shared<PublicRsaKeyShared>(DcId::empty());
  public_rsa_key_watchdog_ = create_actor<PublicRsaKeyWatchdog>("PublicRsaKeyWatchdog", create_reference());
}

NetQueryDispatcher::NetQueryDispatcher() = default;
NetQueryDispatcher::~NetQueryDispatcher() = default;

void NetQueryDispatcher::try_fix_migrate(NetQueryPtr &net_query) {
  auto msg = net_query->error().message();
  static constexpr CSlice prefixes[] = {"PHONE_MIGRATE_", "NETWORK_MIGRATE_", "USER_MIGRATE_"};
  for (auto &prefix : prefixes) {
    if (msg.substr(0, prefix.size()) == prefix) {
      int32 new_main_dc_id = to_integer<int32>(msg.substr(prefix.size()));
      if (!DcId::is_valid(new_main_dc_id)) {
        LOG(FATAL) << "Receive " << prefix << " to wrong dc " << new_main_dc_id;
      }
      if (new_main_dc_id != main_dc_id_.load(std::memory_order_relaxed)) {
        // Very rare event. Mutex is ok.
        std::lock_guard<std::mutex> guard(main_dc_id_mutex_);
        if (new_main_dc_id != main_dc_id_) {
          LOG(INFO) << "Update: " << tag("main_dc_id", main_dc_id_.load(std::memory_order_relaxed));
          if (is_dc_inited(main_dc_id_.load(std::memory_order_relaxed))) {
            send_closure_later(dcs_[main_dc_id_ - 1].main_session_, &SessionMultiProxy::update_main_flag, false);
          }
          main_dc_id_ = new_main_dc_id;
          if (is_dc_inited(main_dc_id_.load(std::memory_order_relaxed))) {
            send_closure_later(dcs_[main_dc_id_ - 1].main_session_, &SessionMultiProxy::update_main_flag, true);
          }
          send_closure_later(dc_auth_manager_, &DcAuthManager::update_main_dc,
                             DcId::internal(main_dc_id_.load(std::memory_order_relaxed)));
          G()->td_db()->get_binlog_pmc()->set("main_dc_id", to_string(main_dc_id_.load(std::memory_order_relaxed)));
        }
      }

      if (!net_query->dc_id().is_main()) {
        LOG(ERROR) << msg << " from query to non-main dc " << net_query->dc_id();
        net_query->resend(DcId::internal(new_main_dc_id));
      } else {
        net_query->resend();
      }
      break;
    }
  }
}

}  // namespace td
