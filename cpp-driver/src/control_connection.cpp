/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "control_connection.hpp"

#include "collection_iterator.hpp"
#include "constants.hpp"
#include "event_response.hpp"
#include "load_balancing.hpp"
#include "logger.hpp"
#include "metadata.hpp"
#include "query_request.hpp"
#include "result_iterator.hpp"
#include "error_response.hpp"
#include "result_response.hpp"
#include "session.hpp"
#include "timer.hpp"
#include "utils.hpp"
#include "vector.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>

#define SELECT_LOCAL "SELECT data_center, rack, release_version FROM system.local WHERE key='local'"
#define SELECT_LOCAL_TOKENS "SELECT data_center, rack, release_version, partitioner, tokens FROM system.local WHERE key='local'"
#define SELECT_PEERS "SELECT peer, data_center, rack, release_version, rpc_address FROM system.peers"
#define SELECT_PEERS_TOKENS "SELECT peer, data_center, rack, release_version, rpc_address, tokens FROM system.peers"

#define SELECT_KEYSPACES_20 "SELECT * FROM system.schema_keyspaces"
#define SELECT_COLUMN_FAMILIES_20 "SELECT * FROM system.schema_columnfamilies"
#define SELECT_COLUMNS_20 "SELECT * FROM system.schema_columns"
#define SELECT_USERTYPES_21 "SELECT * FROM system.schema_usertypes"
#define SELECT_FUNCTIONS_22 "SELECT * FROM system.schema_functions"
#define SELECT_AGGREGATES_22 "SELECT * FROM system.schema_aggregates"

#define SELECT_KEYSPACES_30 "SELECT * FROM system_schema.keyspaces"
#define SELECT_TABLES_30 "SELECT * FROM system_schema.tables"
#define SELECT_VIEWS_30 "SELECT * FROM system_schema.views"
#define SELECT_COLUMNS_30 "SELECT * FROM system_schema.columns"
#define SELECT_INDEXES_30 "SELECT * FROM system_schema.indexes"
#define SELECT_USERTYPES_30 "SELECT * FROM system_schema.types"
#define SELECT_FUNCTIONS_30 "SELECT * FROM system_schema.functions"
#define SELECT_AGGREGATES_30 "SELECT * FROM system_schema.aggregates"

namespace cass {

class ControlStartupQueryPlan : public QueryPlan {
public:
  ControlStartupQueryPlan(const HostMap& hosts, Random* random)
    : index_(random != NULL ? random->next(std::max(static_cast<size_t>(1), hosts.size())) : 0)
    , count_(0) {
    hosts_.reserve(hosts.size());
    std::transform(hosts.begin(), hosts.end(), std::back_inserter(hosts_), GetHost());
  }

  virtual Host::Ptr compute_next() {
    const size_t size = hosts_.size();
    if (count_ >= size) return Host::Ptr();
    size_t index = (index_ + count_) % size;
    ++count_;
    return hosts_[index];
  }

private:
  HostVec hosts_;
  size_t index_;
  size_t count_;
};

/**
 * A request callback for handle query requests for the control connection.
 */
class ControlRequestCallback : public SimpleRequestCallback {
public:
  typedef SharedRefPtr<ControlRequestCallback> Ptr;
  typedef void (*Callback)(ControlRequestCallback*);

  ControlRequestCallback(const String& query,
                         ControlConnection* control_connection,
                         Callback callback);

  ControlRequestCallback(const Request::ConstPtr& request,
                         ControlConnection* control_connection,
                         Callback callback);

  virtual void on_internal_set(ResponseMessage* response);
  virtual void on_internal_error(CassError code, const String& message);
  virtual void on_internal_timeout();

  ControlConnection* control_connection() { return control_connection_; }
  const ResultResponse::Ptr& result() const { return result_; }

private:
  ControlConnection* control_connection_;
  Callback callback_;
  ResultResponse::Ptr result_;
};

ControlRequestCallback::ControlRequestCallback(const String& query,
                                               ControlConnection* control_connection,
                                               ControlRequestCallback::Callback callback)
  : SimpleRequestCallback(query)
  , control_connection_(control_connection)
  , callback_(callback) {
  // We need to update the loop time to prevent new requests from timing out
  // in cases where a callback took a long time to execute.
  // TODO: In the future, we might improve this by executing the these long
  // running callbacks on a seperate thread.
  uv_update_time(control_connection->session_->loop());
}

ControlRequestCallback::ControlRequestCallback(const Request::ConstPtr& request,
                                               ControlConnection* control_connection,
                                               Callback callback)
  : SimpleRequestCallback(request)
  , control_connection_(control_connection)
  , callback_(callback) {
  uv_update_time(control_connection->session_->loop());
}

void ControlRequestCallback::on_internal_set(ResponseMessage* response) {
  const Response* response_body = response->response_body().get();
  if (control_connection_->handle_query_invalid_response(response_body)) {
    return;
  }
  result_ = ResultResponse::Ptr(response->response_body());
  callback_(this);
}

void ControlRequestCallback::on_internal_error(CassError code, const String& message) {
  control_connection_->handle_query_failure(code, message);
}

void ControlRequestCallback::on_internal_timeout() {
  control_connection_->handle_query_timeout();
}

/**
 * A request callback for handling multiple control connection queries as a
 * single request. This is useful for processing multiple schema queries as a
 * single request. For example, table refreshes require schema data from the
 * table, columns, index, and materialized view schema tables.
 */
class ChainedControlRequestCallback : public ChainedRequestCallback {
public:
  typedef SharedRefPtr<ChainedControlRequestCallback> Ptr;
  typedef void (*Callback)(ChainedControlRequestCallback*);

  ChainedControlRequestCallback(const String& key, const String& query,
                                ControlConnection* control_connection,
                                Callback callback);

  virtual void on_chain_set();
  virtual void on_chain_error(CassError code, const String& message);
  virtual void on_chain_timeout();

  ControlConnection* control_connection() { return control_connection_; }

private:
  ControlConnection* control_connection_;
  Callback callback_;
};

ChainedControlRequestCallback::ChainedControlRequestCallback(const String& key, const String& query,
                                                             ControlConnection* control_connection,
                                                             Callback callback)
  : ChainedRequestCallback(key, query)
  , control_connection_(control_connection)
  , callback_(callback) {
  // We need to update the loop time to prevent new requests from timing out
  // in cases where a callback took a long time to execute.
  // TODO: In the future, we might improve this by executing the these long
  // running callbacks on a seperate thread.
  uv_update_time(control_connection->session_->loop());
}

void ChainedControlRequestCallback::on_chain_set() {
  bool has_error = false;
  for (Map::const_iterator it = responses().begin(),
       end = responses().end(); it != end; ++it) {
    if (control_connection_->handle_query_invalid_response(it->second.get())) {
      has_error = true;
    }
  }
  if (has_error) return;
  callback_(this);
}

void ChainedControlRequestCallback::on_chain_error(CassError code, const String& message) {
  control_connection_->handle_query_failure(code, message);
}

void ChainedControlRequestCallback::on_chain_timeout() {
  control_connection_->handle_query_timeout();
}

class RefreshNodeCallback : public ControlRequestCallback {
public:
  RefreshNodeCallback(const Host::Ptr& host, bool is_new_node,
                      const String& query,
                      ControlConnection* control_connection,
                      Callback callback)
    : ControlRequestCallback(query, control_connection, callback)
    , host(host)
    , is_new_node(is_new_node) { }

  const Host::Ptr host;
  const bool is_new_node;
};

class RefreshTableCallback : public ChainedControlRequestCallback {
public:
  RefreshTableCallback(const String& keyspace_name, const String& table_or_view_name,
                       const String& key, const String& query,
                       ControlConnection* control_connection,
                       Callback callback)
    : ChainedControlRequestCallback(key, query, control_connection, callback)
    , keyspace_name(keyspace_name)
    , table_or_view_name(table_or_view_name) { }

  const String keyspace_name;
  const String table_or_view_name;
};

class RefreshFunctionCallback : public ControlRequestCallback {
public:
  typedef Vector<String> StringVec;

  RefreshFunctionCallback(const String& keyspace_name, const String& function_name,
                          const StringVec& arg_types, bool is_aggregate,
                          const Request::ConstPtr& request,
                          ControlConnection* control_connection,
                          Callback callback)
    : ControlRequestCallback(request, control_connection, callback)
    , keyspace_name(keyspace_name)
    , function_name(function_name)
    , arg_types(arg_types)
    , is_aggregate(is_aggregate) { }

  const String keyspace_name;
  const String function_name;
  const StringVec arg_types;
  const bool is_aggregate;
};

class RefreshKeyspaceCallback : public ControlRequestCallback {
public:
  RefreshKeyspaceCallback(const String& keyspace_name,
                          const String& query,
                          ControlConnection* control_connection,
                          Callback callback)
    : ControlRequestCallback(query, control_connection, callback)
    , keyspace_name(keyspace_name) { }

  const String keyspace_name;
};

class RefreshTypeCallback : public ControlRequestCallback {
public:
  RefreshTypeCallback(const String& keyspace_name, const String& type_name,
                      const String& query,
                      ControlConnection* control_connection,
                      Callback callback)
    : ControlRequestCallback(query, control_connection, callback)
    , keyspace_name(keyspace_name)
    , type_name(type_name) { }

  const String keyspace_name;
  const String type_name;
};

bool ControlConnection::determine_address_for_peer_host(const Address& connected_address,
                                                        const Value* peer_value,
                                                        const Value* rpc_value,
                                                        Address* output) {
  Address peer_address;
  if (!peer_value->decoder().as_inet(peer_value->size(),
                                     connected_address.port(),
                                     &peer_address)) {
    LOG_WARN("Invalid address format for peer address");
    return false;
  }
  if (!rpc_value->is_null()) {
    if (!rpc_value->decoder().as_inet(rpc_value->size(),
                                      connected_address.port(),
                                      output)) {
      LOG_WARN("Invalid address format for rpc address");
      return false;
    }
    if (connected_address == *output || connected_address == peer_address) {
      LOG_DEBUG("system.peers on %s contains a line with rpc_address for itself. "
                "This is not normal, but is a known problem for some versions of DSE. "
                "Ignoring this entry.", connected_address.to_string(false).c_str());
      return false;
    }
    if (Address::BIND_ANY_IPV4.compare(*output, false) == 0 ||
        Address::BIND_ANY_IPV6.compare(*output, false) == 0) {
      LOG_WARN("Found host with 'bind any' for rpc_address; using listen_address (%s) to contact instead. "
               "If this is incorrect you should configure a specific interface for rpc_address on the server.",
               peer_address.to_string(false).c_str());
      *output = peer_address;
    }
  } else {
    LOG_WARN("No rpc_address for host %s in system.peers on %s. "
             "Ignoring this entry.", peer_address.to_string(false).c_str(),
             connected_address.to_string(false).c_str());
    return false;
  }
  return true;
}

ControlConnection::ControlConnection()
  : state_(CONTROL_STATE_NEW)
  , session_(NULL)
  , connection_(NULL)
  , event_types_(0)
  , protocol_version_(0)
  , use_schema_(false)
  , token_aware_routing_(false) { }

const Host::Ptr& ControlConnection::connected_host() const {
  return current_host_;
}

void ControlConnection::clear() {
  state_ = CONTROL_STATE_NEW;
  session_ = NULL;
  connection_ = NULL;
  reconnect_timer_.stop();
  query_plan_.reset();
  protocol_version_ = 0;
  last_connection_error_.clear();
  use_schema_ = false;
  token_aware_routing_ = false;
}

void ControlConnection::connect(Session* session) {
  session_ = session;
  query_plan_.reset(Memory::allocate<ControlStartupQueryPlan>(session_->hosts_, // No hosts lock necessary (read-only)
                                                              session_->random_.get()));
  protocol_version_ = session_->config().protocol_version();
  use_schema_ = session_->config().use_schema();
  token_aware_routing_ = session_->config().default_profile().token_aware_routing();
  if (protocol_version_ < 0) {
    protocol_version_ = CASS_HIGHEST_SUPPORTED_PROTOCOL_VERSION;
  }

  if (use_schema_ || token_aware_routing_) {
    event_types_ = CASS_EVENT_TOPOLOGY_CHANGE | CASS_EVENT_STATUS_CHANGE |
                   CASS_EVENT_SCHEMA_CHANGE;
  } else {
    event_types_ = CASS_EVENT_TOPOLOGY_CHANGE | CASS_EVENT_STATUS_CHANGE;
  }

  reconnect(false);
}

void ControlConnection::close() {
  state_ = CONTROL_STATE_CLOSED;
  if (connection_ != NULL) {
    connection_->close();
  }
  reconnect_timer_.stop();
}

void ControlConnection::schedule_reconnect(uint64_t ms) {
  reconnect_timer_.start(session_->loop(),
                         ms,
                         this,
                         ControlConnection::on_reconnect);
}

void ControlConnection::reconnect(bool retry_current_host) {
  if (state_ == CONTROL_STATE_CLOSED) {
    return;
  }

  if (!retry_current_host) {
    current_host_ = query_plan_->compute_next();
    if (!current_host_) {
      if (state_ == CONTROL_STATE_READY) {
        schedule_reconnect(1000); // TODO(mpenick): Configurable?
      } else {
        session_->on_control_connection_error(CASS_ERROR_LIB_NO_HOSTS_AVAILABLE,
                                              "No hosts available for the control connection");
      }
      return;
    }
  }

  if (connection_ != NULL) {
    connection_->close();
  }

  Connector::Ptr connector(Memory::allocate<Connector>(current_host_->address(),
                                                       protocol_version_,
                                                       this,
                                                       on_connect));

  connector->with_settings(ConnectionSettings(session_->config()))
           ->with_event_types(event_types_)
           ->with_listener(this)
           ->with_metrics(session_->metrics())
           ->connect(session_->loop());
}

void ControlConnection::on_close(Connection* connection) {
  // This pointer to the connection is no longer valid once it's closed
  connection_ = NULL;

  if (state_ != CONTROL_STATE_CLOSED) {
    LOG_WARN("Lost control connection to host %s",
             connection->address().to_string().c_str());
  }

  reconnect(false);
}

void ControlConnection::on_event(const EventResponse* response) {
  // Only process events after an initial set of hosts and schema have been
  // established. Adding a host from an UP/NEW_NODE event before the initial
  // set will cause the driver to hang waiting for an invalid pending pool
  // count.
  if (state_ != CONTROL_STATE_READY) return;

  switch (response->event_type()) {
    case CASS_EVENT_TOPOLOGY_CHANGE: {
      String address_str = response->affected_node().to_string();
      switch (response->topology_change()) {
        case EventResponse::NEW_NODE: {
          LOG_INFO("New node %s added", address_str.c_str());
          Host::Ptr host = session_->get_host(response->affected_node());
          if (!host) {
            host = session_->add_host(response->affected_node(), true);
            refresh_node_info(host, true, true);
          }
          break;
        }

        case EventResponse::REMOVED_NODE: {
          LOG_INFO("Node %s removed", address_str.c_str());
          Host::Ptr host = session_->get_host(response->affected_node());
          if (host) {
            session_->on_remove(host);
            session_->token_map_host_remove(host);
          } else {
            LOG_DEBUG("Tried to remove host %s that doesn't exist", address_str.c_str());
          }
          break;
        }

        case EventResponse::MOVED_NODE:
          LOG_INFO("Node %s moved", address_str.c_str());
          Host::Ptr host = session_->get_host(response->affected_node());
          if (host) {
            refresh_node_info(host, false, true);
          } else {
            LOG_DEBUG("Move event for host %s that doesn't exist", address_str.c_str());
            session_->token_map_host_remove(host);
          }
          break;
      }
      break;
    }

    case CASS_EVENT_STATUS_CHANGE: {
      String address_str = response->affected_node().to_string();
      switch (response->status_change()) {
        case EventResponse::UP: {
          LOG_INFO("Node %s is up", address_str.c_str());
          on_up(response->affected_node());
          break;
        }

        case EventResponse::DOWN: {
          LOG_INFO("Node %s is down", address_str.c_str());
          on_down(response->affected_node());
          break;
        }
      }
      break;
    }

    case CASS_EVENT_SCHEMA_CHANGE:
      // Only handle keyspace events when using token-aware routing
      if (!use_schema_ &&
          response->schema_change_target() != EventResponse::KEYSPACE) {
        return;
      }

      LOG_DEBUG("Schema change (%d): %.*s %.*s\n",
                response->schema_change(),
                (int)response->keyspace().size(), response->keyspace().data(),
                (int)response->target().size(), response->target().data());

      switch (response->schema_change()) {
        case EventResponse::CREATED:
        case EventResponse::UPDATED:
          switch (response->schema_change_target()) {
            case EventResponse::KEYSPACE:
              refresh_keyspace(response->keyspace());
              break;
            case EventResponse::TABLE:
              refresh_table_or_view(response->keyspace(), response->target());
              break;
            case EventResponse::TYPE:
              refresh_type(response->keyspace(), response->target());
              break;
            case EventResponse::FUNCTION:
            case EventResponse::AGGREGATE:
              refresh_function(response->keyspace(),
                               response->target(),
                               response->arg_types(),
                               response->schema_change_target() == EventResponse::AGGREGATE);
              break;
          }
          break;

        case EventResponse::DROPPED:
          switch (response->schema_change_target()) {
            case EventResponse::KEYSPACE:
              session_->metadata().drop_keyspace(response->keyspace().to_string());
              break;
            case EventResponse::TABLE:
              session_->metadata().drop_table_or_view(response->keyspace().to_string(),
                                                      response->target().to_string());
              break;
            case EventResponse::TYPE:
              session_->metadata().drop_user_type(response->keyspace().to_string(),
                                                  response->target().to_string());
              break;
            case EventResponse::FUNCTION:
              session_->metadata().drop_function(response->keyspace().to_string(),
                                                 Metadata::full_function_name(response->target().to_string(),
                                                                              to_strings(response->arg_types())));
              break;
            case EventResponse::AGGREGATE:
              session_->metadata().drop_aggregate(response->keyspace().to_string(),
                                                  Metadata::full_function_name(response->target().to_string(),
                                                                               to_strings(response->arg_types())));
              break;
          }
          break;

      }
      break;

    default:
      assert(false);
      break;
  }
}

void ControlConnection::on_connect(Connector* connector) {
  ControlConnection* control_connection = static_cast<ControlConnection*>(connector->data());
  control_connection->handle_connect(connector);
}

void ControlConnection::handle_connect(Connector* connector) {
  if (connector->is_ok()) {
    LOG_DEBUG("Connection ready on host %s",
              connector->address().to_string().c_str());

    connection_ = connector->release_connection().get();

    // The control connection has to refresh meta when there's a reconnect because
    // events could have been missed while not connected.
    query_meta_hosts();
  } else {
    bool retry_current_host = false;

    if (state_ == CONTROL_STATE_NEW) {
      if (connector->is_invalid_protocol()) {
        if (protocol_version_ <= 1) {
          LOG_ERROR("Host %s does not support any valid protocol version",
                    connector->address().to_string().c_str());
          session_->on_control_connection_error(CASS_ERROR_LIB_UNABLE_TO_DETERMINE_PROTOCOL,
                                                "Not even protocol version 1 is supported");
          return;
        }

        int previous_version = protocol_version_;
        bool is_dse_version = protocol_version_ & DSE_PROTOCOL_VERSION_BIT;
        if (is_dse_version) {
          int dse_version = protocol_version_ & DSE_PROTOCOL_VERSION_MASK;
          if (dse_version <= 1) {
            // Start trying Cassandra protocol versions
            protocol_version_ = CASS_HIGHEST_SUPPORTED_PROTOCOL_VERSION;
          } else {
            protocol_version_--;
          }
        } else {
          protocol_version_--;
        }

        LOG_WARN("Host %s does not support protocol version %s. "
                 "Trying protocol version %s...",
                 connector->address().to_string().c_str(),
                 protocol_version_to_string(previous_version).c_str(),
                 protocol_version_to_string(protocol_version_).c_str());

        retry_current_host = true;
      } else if (connector->is_auth_error()) {
        session_->on_control_connection_error(CASS_ERROR_SERVER_BAD_CREDENTIALS,
                                              connector->error_message());
        return;
      } else if (connector->is_ssl_error()) {
        session_->on_control_connection_error(CASS_ERROR_LIB_UNABLE_TO_CONNECT,
                                              connector->error_message());
        return;
      }
    }

    // Don't log if the control connection is closing/closed or retrying because of
    // an invalid protocol error.
    if (state_ != CONTROL_STATE_CLOSED && !retry_current_host) {
      // Log only as an error if it's the initial attempt
      if (state_ == CONTROL_STATE_NEW) {
        LOG_ERROR("Unable to establish a control connection to host %s because of the following error: %s",
                  connector->address().to_string().c_str(),
                  connector->error_message().c_str());
      } else {
        LOG_WARN("Unable to reconnect control connection to host %s because of the following error: %s",
                 connector->address().to_string().c_str(),
                 connector->error_message().c_str());
      }
    }

    reconnect(retry_current_host);
  }
}

void ControlConnection::query_meta_hosts() {
  // This needs to happen before other schema metadata queries so that we have
  // a valid Cassandra version because this version determines which follow up
  // schema metadata queries are executed.
  ChainedRequestCallback::Ptr callback(
        Memory::allocate<ChainedControlRequestCallback>(
          "local", token_aware_routing_ ? SELECT_LOCAL_TOKENS : SELECT_LOCAL,
          this, ControlConnection::on_query_hosts)
        ->chain("peers", token_aware_routing_ ? SELECT_PEERS_TOKENS : SELECT_PEERS));

  connection_->write_and_flush(callback);
}

void ControlConnection::on_query_hosts(ChainedControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Connection* connection = control_connection->connection_;
  if (connection == NULL) {
    return;
  }

  Session* session = control_connection->session_;

  if (control_connection->token_aware_routing_) {
    session->token_map_hosts_cleared();
  }

  bool is_initial_connection = (control_connection->state_ == CONTROL_STATE_NEW);

  // If the 'system.local' table is empty the connection isn't used as a control
  // connection because at least one node's information is required (itself). An
  // empty 'system.local' can happen during the bootstrapping process on some
  // versions of Cassandra. If this happens we defunct the connection and move
  // to the next node in the query plan.
  {
    Host::Ptr host = session->get_host(connection->address());
    if (host) {
      host->set_mark(session->current_host_mark_);

      ResultResponse::Ptr local_result(callback->result("local"));
      if (local_result && local_result->row_count() > 0) {
        control_connection->update_node_info(host, &local_result->first_row(), ADD_HOST);
        control_connection->cassandra_version_ = host->cassandra_version();
      } else {
        LOG_WARN("No row found in %s's local system table",
                 connection->address_string().c_str());
        connection->defunct();
        return;
      }
    } else {
      LOG_WARN("Host %s from local system table not found",
               connection->address_string().c_str());
      connection->defunct();
      return;
    }
  }

  {
    ResultResponse::Ptr peers_result(callback->result("peers"));
    if (peers_result) {
      ResultIterator rows(peers_result.get());
      while (rows.next()) {
        Address address;
        const Row* row = rows.row();
        if (!determine_address_for_peer_host(connection->address(),
                                             row->get_by_name("peer"),
                                             row->get_by_name("rpc_address"),
                                             &address)) {
          continue;
        }

        Host::Ptr host = session->get_host(address);
        bool is_new = false;
        if (!host) {
          is_new = true;
          host = session->add_host(address);
        }

        host->set_mark(session->current_host_mark_);

        control_connection->update_node_info(host, rows.row(), ADD_HOST);
        if (is_new && !is_initial_connection) {
          session->on_add(host);
        }
      }
    }
  }

  session->purge_hosts(is_initial_connection);

  if (control_connection->use_schema_ ||
      control_connection->token_aware_routing_) {
    control_connection->query_meta_schema();
  } else if (is_initial_connection) {
    control_connection->state_ = CONTROL_STATE_READY;
    session->on_control_connection_ready();
    // Create a new query plan that considers all the new hosts from the
    // "system" tables.
    control_connection->query_plan_.reset(session->new_query_plan());
  }
}

//TODO: query and callbacks should be in Metadata
// punting for now because of tight coupling of Session and CC state
void ControlConnection::query_meta_schema() {
  if (!use_schema_ && !token_aware_routing_) return;

  ChainedRequestCallback::Ptr callback;

  if (cassandra_version_ >= VersionNumber(3, 0, 0)) {
    callback = ChainedRequestCallback::Ptr(Memory::allocate<ChainedControlRequestCallback>(
                                             "keyspaces", SELECT_KEYSPACES_30,
                                             this, ControlConnection::on_query_meta_schema));
    if (use_schema_) {
      callback = callback
                 ->chain("tables", SELECT_TABLES_30)
                 ->chain("views", SELECT_VIEWS_30)
                 ->chain("columns", SELECT_COLUMNS_30)
                 ->chain("indexes", SELECT_INDEXES_30)
                 ->chain("user_types", SELECT_USERTYPES_30)
                 ->chain("functions", SELECT_FUNCTIONS_30)
                 ->chain("aggregates", SELECT_AGGREGATES_30);
    }
  } else {
    callback = ChainedRequestCallback::Ptr(Memory::allocate<ChainedControlRequestCallback>(
                                             "keyspaces", SELECT_KEYSPACES_20,
                                             this, ControlConnection::on_query_meta_schema));
    if (use_schema_) {
      callback = callback
                 ->chain("tables", SELECT_COLUMN_FAMILIES_20)
                 ->chain("columns", SELECT_COLUMNS_20);


      if (cassandra_version_ >= VersionNumber(2, 1, 0)) {
        callback = callback->chain("user_types", SELECT_USERTYPES_21);
      }
      if (cassandra_version_ >= VersionNumber(2, 2, 0)) {
        callback = callback
                   ->chain("functions", SELECT_FUNCTIONS_22)
                   ->chain("aggregates", SELECT_AGGREGATES_22);
      }
    }
  }

  connection_->write_and_flush(callback);
}

void ControlConnection::on_query_meta_schema(ChainedControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Connection* connection = control_connection->connection_;
  if (connection == NULL) {
    return;
  }

  Session* session = control_connection->session_;
  const VersionNumber& cassandra_version = control_connection->cassandra_version_;

  bool is_initial_connection = (control_connection->state_ == CONTROL_STATE_NEW);

  if (control_connection->token_aware_routing_) {
    ResultResponse::Ptr keyspaces_result(callback->result("keyspaces"));
    session->token_map_keyspaces_add(cassandra_version, keyspaces_result);
  }

  if (control_connection->use_schema_) {
    session->metadata().clear_and_update_back(cassandra_version);

    ResultResponse::Ptr keyspaces_result(callback->result("keyspaces"));
    if (keyspaces_result) {
      session->metadata().update_keyspaces(cassandra_version, keyspaces_result.get());
    }

    ResultResponse::Ptr tables_result(callback->result("tables"));
    if (tables_result) {
      session->metadata().update_tables(cassandra_version, tables_result.get());
    }

    ResultResponse::Ptr views_result(callback->result("views"));
    if (views_result) {
      session->metadata().update_views(cassandra_version, views_result.get());
    }

    ResultResponse::Ptr columns_result(callback->result("columns"));
    if (columns_result) {
      session->metadata().update_columns(cassandra_version, columns_result.get());
    }

    ResultResponse::Ptr indexes_result(callback->result("indexes"));
    if (indexes_result) {
      session->metadata().update_indexes(cassandra_version, indexes_result.get());
    }

    ResultResponse::Ptr user_types_result(callback->result("user_types"));
    if (user_types_result) {
      session->metadata().update_user_types(cassandra_version, user_types_result.get());
    }

    ResultResponse::Ptr functions_result(callback->result("functions"));
    if (functions_result) {
      session->metadata().update_functions(cassandra_version, functions_result.get());
    }

    ResultResponse::Ptr aggregates_result(callback->result("aggregates"));
    if (aggregates_result) {
      session->metadata().update_aggregates(cassandra_version, aggregates_result.get());
    }

    session->metadata().swap_to_back_and_update_front();
  }

  if (is_initial_connection) {
    control_connection->state_ = CONTROL_STATE_READY;
    session->on_control_connection_ready();
    // Create a new query plan that considers all the new hosts from the
    // "system" tables.
    control_connection->query_plan_.reset(session->new_query_plan());
  }
}

void ControlConnection::refresh_node_info(Host::Ptr host,
                                          bool is_new_node,
                                          bool query_tokens) {
  if (connection_ == NULL) {
    return;
  }

  bool is_connected_host = host->address() == connection_->address();

  String query;
  ControlRequestCallback::Callback callback;

  bool token_query = token_aware_routing_ && (host->was_just_added() || query_tokens);
  if (is_connected_host || !host->listen_address().empty()) {
    if (is_connected_host) {
      query.assign(token_query ? SELECT_LOCAL_TOKENS : SELECT_LOCAL);
    } else {
      query.assign(token_query ? SELECT_PEERS_TOKENS : SELECT_PEERS);
      query.append(" WHERE peer = '");
      query.append(host->listen_address());
      query.append("'");
    }
    callback = ControlConnection::on_refresh_node_info;
  } else {
    query.assign(token_query ? SELECT_PEERS_TOKENS : SELECT_PEERS);
    callback = ControlConnection::on_refresh_node_info_all;
  }

  LOG_DEBUG("refresh_node_info: %s", query.c_str());

  if (!connection_->write_and_flush(RequestCallback::Ptr(
                                      Memory::allocate<RefreshNodeCallback>(
                                        host, is_new_node, query, this, callback)))) {
    LOG_ERROR("No more stream available while attempting to refresh node info");
    connection_->defunct();
  }
}

void ControlConnection::on_refresh_node_info(ControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Connection* connection = control_connection->connection_;
  if (connection == NULL) {
    return;
  }

  const ResultResponse::Ptr result = callback->result();
  RefreshNodeCallback* refresh_node_callback = static_cast<RefreshNodeCallback*>(callback);

  if (result->row_count() == 0) {
    String host_address_str = refresh_node_callback->host->address().to_string();
    LOG_ERROR("No row found for host %s in %s's local/peers system table. "
              "%s will be ignored.",
              host_address_str.c_str(),
              connection->address_string().c_str(),
              host_address_str.c_str());
    return;
  }
  control_connection->update_node_info(refresh_node_callback->host, &result->first_row(), UPDATE_HOST_AND_BUILD);

  if (refresh_node_callback->is_new_node) {
    control_connection->session_->on_add(refresh_node_callback->host);
  }
}

void ControlConnection::on_refresh_node_info_all(ControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Connection* connection = control_connection->connection_;
  if (connection == NULL) {
    return;
  }

  const ResultResponse::Ptr result = callback->result();
  RefreshNodeCallback* refresh_node_callback = static_cast<RefreshNodeCallback*>(callback);

  if (result->row_count() == 0) {
    String host_address_str = refresh_node_callback->host->address().to_string();
    LOG_ERROR("No row found for host %s in %s's peers system table. "
              "%s will be ignored.",
              host_address_str.c_str(),
              connection->address_string().c_str(),
              host_address_str.c_str());
    return;
  }

  ResultIterator rows(result.get());
  while (rows.next()) {
    const Row* row = rows.row();
    Address address;
    bool is_valid_address
        = determine_address_for_peer_host(connection->address(),
                                          row->get_by_name("peer"),
                                          row->get_by_name("rpc_address"),
                                          &address);
    if (is_valid_address && refresh_node_callback->host->address() == address) {
      control_connection->update_node_info(refresh_node_callback->host, row, UPDATE_HOST_AND_BUILD);
      if (refresh_node_callback->is_new_node) {
        control_connection->session_->on_add(refresh_node_callback->host);
      }
      break;
    }
  }
}

void ControlConnection::update_node_info(Host::Ptr host, const Row* row, UpdateHostType type) {
  const Value* v;

  String rack;
  row->get_string_by_name("rack", &rack);

  String dc;
  row->get_string_by_name("data_center", &dc);

  String release_version;
  row->get_string_by_name("release_version", &release_version);

  // This value is not present in the "system.local" query
  v = row->get_by_name("peer");
  if (v != NULL) {
    Address listen_address;
    if (v->decoder().as_inet(v->size(),
                             connection_->address().port(),
                             &listen_address)) {
      host->set_listen_address(listen_address.to_string());
    } else {
      LOG_WARN("Invalid address format for listen address");
    }
  }

  if ((!rack.empty() && rack != host->rack()) ||
      (!dc.empty() && dc != host->dc())) {
    if (!host->was_just_added()) {
      session_->load_balancing_policy_host_add_remove(host, false);
    }
    host->set_rack_and_dc(rack, dc);
    if (!host->was_just_added()) {
      session_->load_balancing_policy_host_add_remove(host, true);
    }
  }

  VersionNumber cassandra_version;
  if (cassandra_version.parse(release_version)) {
    host->set_cassaandra_version(cassandra_version);
  } else {
    LOG_WARN("Invalid release version string \"%s\" on host %s",
             release_version.c_str(),
             host->address().to_string().c_str());
  }

  if (token_aware_routing_) {
    bool is_connected_host = connection_ != NULL && host->address() == connection_->address();
    String partitioner;
    if (is_connected_host && row->get_string_by_name("partitioner", &partitioner)) {
      if (!session_->token_map_init(partitioner)) {
        LOG_TRACE("Token map has already been initialized");
      }
    }
    v = row->get_by_name("tokens");
    if (v != NULL && v->is_collection()) {
      if (type == UPDATE_HOST_AND_BUILD) {
        session_->token_map_host_update(host, v);
      } else {
        session_->token_map_host_add(host, v);
      }
    }
  }
}

void ControlConnection::refresh_keyspace(const StringRef& keyspace_name) {
  String query;

  if (cassandra_version_ >= VersionNumber(3, 0, 0)) {
    query.assign(SELECT_KEYSPACES_30);
  }  else {
    query.assign(SELECT_KEYSPACES_20);
  }
  query.append(" WHERE keyspace_name='")
       .append(keyspace_name.data(), keyspace_name.size())
       .append("'");

  LOG_DEBUG("Refreshing keyspace %s", query.c_str());

  if (!connection_->write_and_flush(
        RequestCallback::Ptr(
          Memory::allocate<RefreshKeyspaceCallback>(
            keyspace_name.to_string(), query, this, on_refresh_keyspace)))) {
    LOG_ERROR("No more stream available while attempting to refresh keyspace info");
    connection_->defunct();
  }
}

void ControlConnection::on_refresh_keyspace(ControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  const ResultResponse::Ptr result = callback->result();
  if (result->row_count() == 0) {
    RefreshKeyspaceCallback* refresh_keyspace_callback = static_cast<RefreshKeyspaceCallback*>(callback);
    LOG_ERROR("No row found for keyspace %s in system schema table.",
              refresh_keyspace_callback->keyspace_name.c_str());
    return;
  }

  Session* session = control_connection->session_;
  const VersionNumber& cassandra_version = control_connection->cassandra_version_;

  if (control_connection->token_aware_routing_) {
    session->token_map_keyspaces_update(cassandra_version, result);
  }

  if (control_connection->use_schema_) {
    session->metadata().update_keyspaces(cassandra_version, result.get());
  }
}

void ControlConnection::refresh_table_or_view(const StringRef& keyspace_name,
                                              const StringRef& table_or_view_name) {
  String table_query;
  String view_query;
  String column_query;
  String index_query;

  if (cassandra_version_ >= VersionNumber(3, 0, 0)) {
    table_query.assign(SELECT_TABLES_30);
    table_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND table_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    view_query.assign(SELECT_VIEWS_30);
    view_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND view_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    column_query.assign(SELECT_COLUMNS_30);
    column_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND table_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    index_query.assign(SELECT_INDEXES_30);
    index_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND table_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    LOG_DEBUG("Refreshing table/view %s; %s; %s; %s", table_query.c_str(), view_query.c_str(),
                                                      column_query.c_str(), index_query.c_str());
  } else {
    table_query.assign(SELECT_COLUMN_FAMILIES_20);
    table_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND columnfamily_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    column_query.assign(SELECT_COLUMNS_20);
    column_query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
        .append("' AND columnfamily_name='").append(table_or_view_name.data(), table_or_view_name.size()).append("'");

    LOG_DEBUG("Refreshing table %s; %s", table_query.c_str(), column_query.c_str());
  }

  ChainedRequestCallback::Ptr callback(
        Memory::allocate<RefreshTableCallback>(
          keyspace_name.to_string(), table_or_view_name.to_string(),
          "tables", table_query,
          this,
          ControlConnection::on_refresh_table_or_view));

  callback = callback->chain("columns", column_query);

  if (!view_query.empty()) {
    callback = callback->chain("views", view_query);
  }
  if (!index_query.empty()) {
    callback = callback->chain("indexes", index_query);
  }

  connection_->write_and_flush(callback);
}

void ControlConnection::on_refresh_table_or_view(ChainedControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Session* session = control_connection->session_;
  const VersionNumber& cassandra_version = control_connection->cassandra_version_;

  ResultResponse::Ptr tables_result(callback->result("tables"));
  if (!tables_result || tables_result->row_count() == 0) {
    ResultResponse::Ptr views_result(callback->result("views"));
    if (!views_result || views_result->row_count() == 0) {
      RefreshTableCallback* refresh_table_callback = static_cast<RefreshTableCallback*>(callback);
      LOG_ERROR("No row found for table (or view) %s.%s in system schema tables.",
                refresh_table_callback->keyspace_name.c_str(),
                refresh_table_callback->table_or_view_name.c_str());
      return;
    }
    session->metadata().update_views(cassandra_version, views_result.get());
  } else {
    session->metadata().update_tables(cassandra_version, tables_result.get());
  }

  ResultResponse::Ptr columns_result(callback->result("columns"));
  if (columns_result) {
    session->metadata().update_columns(cassandra_version, columns_result.get());
  }

  ResultResponse::Ptr indexes_result(callback->result("indexes"));
  if (indexes_result) {
    session->metadata().update_indexes(cassandra_version, indexes_result.get());
  }
}


void ControlConnection::refresh_type(const StringRef& keyspace_name,
                                     const StringRef& type_name) {

  String query;
  if (cassandra_version_ >= VersionNumber(3, 0, 0)) {
    query.assign(SELECT_USERTYPES_30);
  } else {
    query.assign(SELECT_USERTYPES_21);
  }

  query.append(" WHERE keyspace_name='").append(keyspace_name.data(), keyspace_name.size())
                .append("' AND type_name='").append(type_name.data(), type_name.size()).append("'");

  LOG_DEBUG("Refreshing type %s", query.c_str());

  if (!connection_->write_and_flush(
        RequestCallback::Ptr(
          Memory::allocate<RefreshTypeCallback>(
            keyspace_name.to_string(), type_name.to_string(),
            query, this, on_refresh_type)))) {
    LOG_ERROR("No more stream available while attempting to refresh type info");
    connection_->defunct();
  }
}

void ControlConnection::on_refresh_type(ControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Session* session = control_connection->session_;
  const ResultResponse::Ptr result = callback->result();
  const VersionNumber& cassandra_version = control_connection->cassandra_version_;
  if (result->row_count() == 0) {
    RefreshTypeCallback* refresh_type_callback = static_cast<RefreshTypeCallback*>(callback);
    LOG_ERROR("No row found for keyspace %s and type %s in system schema.",
              refresh_type_callback->keyspace_name.c_str(),
              refresh_type_callback->type_name.c_str());
    return;
  }
  session->metadata().update_user_types(cassandra_version, result.get());
}

void ControlConnection::refresh_function(const StringRef& keyspace_name,
                                         const StringRef& function_name,
                                         const StringRefVec& arg_types,
                                         bool is_aggregate) {

  String query;
  if (cassandra_version_ >= VersionNumber(3, 0, 0)) {
    if (is_aggregate) {
      query.assign(SELECT_AGGREGATES_30);
      query.append(" WHERE keyspace_name=? AND aggregate_name=? AND argument_types=?");
    } else {
      query.assign(SELECT_FUNCTIONS_30);
      query.append(" WHERE keyspace_name=? AND function_name=? AND argument_types=?");
    }
  } else {
    if (is_aggregate) {
      query.assign(SELECT_AGGREGATES_22);
      query.append(" WHERE keyspace_name=? AND aggregate_name=? AND signature=?");
    } else {
      query.assign(SELECT_FUNCTIONS_22);
      query.append(" WHERE keyspace_name=? AND function_name=? AND signature=?");
    }
  }

  LOG_DEBUG("Refreshing %s %s in keyspace %s",
            is_aggregate ? "aggregate" : "function",
            Metadata::full_function_name(function_name.to_string(), to_strings(arg_types)).c_str(),
            String(keyspace_name.data(), keyspace_name.length()).c_str());

  SharedRefPtr<QueryRequest> request(Memory::allocate<QueryRequest>(query, 3));
  SharedRefPtr<Collection> signature(Memory::allocate<Collection>(CASS_COLLECTION_TYPE_LIST, arg_types.size()));

  for (StringRefVec::const_iterator i = arg_types.begin(),
       end = arg_types.end();
       i != end;
       ++i) {
    signature->append(CassString(i->data(), i->size()));
  }

  request->set(0, CassString(keyspace_name.data(), keyspace_name.size()));
  request->set(1, CassString(function_name.data(), function_name.size()));
  request->set(2, signature.get());

  if (!connection_->write_and_flush(
        RequestCallback::Ptr(
          Memory::allocate<RefreshFunctionCallback>(
            keyspace_name.to_string(), function_name.to_string(),
            to_strings(arg_types), is_aggregate,
            request, this, on_refresh_function)))) {
    LOG_ERROR("No more stream available while attempting to refresh function info");
    connection_->defunct();
  }
}

void ControlConnection::on_refresh_function(ControlRequestCallback* callback) {
  ControlConnection* control_connection = callback->control_connection();
  Session* session = control_connection->session_;
  const VersionNumber& cassandra_version = control_connection->cassandra_version_;
  const ResultResponse::Ptr result = callback->result();
  RefreshFunctionCallback* refresh_function_callback = static_cast<RefreshFunctionCallback*>(callback);
  if (result->row_count() == 0) {
    LOG_ERROR("No row found for keyspace %s and %s %s",
              refresh_function_callback->keyspace_name.c_str(),
              refresh_function_callback->is_aggregate ? "aggregate" : "function",
              Metadata::full_function_name(refresh_function_callback->function_name,
                                           refresh_function_callback->arg_types).c_str());
    return;
  }
  if (refresh_function_callback->is_aggregate) {
    session->metadata().update_aggregates(cassandra_version, result.get());
  } else {
    session->metadata().update_functions(cassandra_version, result.get());
  }
}

bool ControlConnection::handle_query_invalid_response(const Response* response) {
  if (check_error_or_invalid_response("ControlConnection", CQL_OPCODE_RESULT,
                                      response)) {
    if (connection_ != NULL) {
      connection_->defunct();
    }
    return true;
  }
  return false;
}

void ControlConnection::handle_query_failure(CassError code, const String& message) {
  // TODO(mpenick): This is a placeholder and might not be the right action for
  // all error scenarios
  if (connection_ != NULL) {
    connection_->defunct();
  }
}

void ControlConnection::handle_query_timeout() {
  // TODO(mpenick): Is this the best way to handle a timeout?
  if (connection_ != NULL) {
    connection_->defunct();
  }
}

void ControlConnection::on_up(const Address& address) {
  Host::Ptr host = session_->get_host(address);
  if (host) {
    if (host->is_up()) return;

    // Immediately mark the node as up and asynchronously attempt
    // to refresh the node's information. This is done because
    // a control connection may not be available because it's
    // waiting for a node to be marked as up.
    session_->on_up(host);
    refresh_node_info(host, false);
  } else {
    host = session_->add_host(address);
    refresh_node_info(host, true);
  }
}

void ControlConnection::on_down(const Address& address) {
  Host::Ptr host = session_->get_host(address);
  if (host) {
    if (host->is_down()) return;

    session_->on_down(host);
  } else {
    LOG_DEBUG("Tried to down host %s that doesn't exist", address.to_string().c_str());
  }
}

void ControlConnection::on_reconnect(Timer* timer) {
  ControlConnection* control_connection = static_cast<ControlConnection*>(timer->data());
  control_connection->query_plan_.reset(control_connection->session_->new_query_plan());
  control_connection->reconnect(false);
}

} // namespace cass
