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

#include <gtest/gtest.h>

#include "mockssandra_test.hpp"

#include "connection_pool_manager_initializer.hpp"
#include "constants.hpp"
#include "ssl.hpp"

#define NUM_NODES 3
#define PROTOCOL_VERSION CASS_HIGHEST_SUPPORTED_PROTOCOL_VERSION
#define WAIT_FOR_TIME 5 * 1000 * 1000 // 5 seconds

using namespace cass;

class PoolUnitTest : public mockssandra::SimpleClusterTest {
public:
  template <class State>
  class Status {
  public:
    size_t count(State state) {
      size_t count = 0;
      for (typename Vector<State>::const_iterator it = results_.begin(),
           end = results_.end(); it != end; ++it) {
        if (*it == state) count++;
      }
      return count;
    }

    Vector<State> results() {
      return results_;
    }

  protected:
    void set(State state) {
      results_.push_back(state);
    }

  private:
    Vector<State> results_;
  };

  struct RequestState {
    enum Enum {
      SUCCESS,
      ERROR_NO_CONNECTION,
      ERROR_FAILED_WRITE,
      ERROR,
      ERROR_RESPONSE,
      TIMEOUT
    };

    static const char* to_string(Enum state) {
      switch (state) {
        case SUCCESS: return "SUCCESS";
        case ERROR_NO_CONNECTION: return "ERROR_NO_CONNECTION";
        case ERROR_FAILED_WRITE: return "ERROR_FAILED_WRITE";
        case ERROR: return "ERROR";
        case ERROR_RESPONSE: return "ERROR_RESPONSE";
        case TIMEOUT: return "TIMEOUT";
      }
      return "";
    }
  };

  class RequestStatus
      : public RequestState
      , public Status<RequestState::Enum> {
  public:
    RequestStatus(uv_loop_t* loop, int num_nodes = NUM_NODES)
      : Status()
      , loop_(loop)
      , remaining_(num_nodes) { }

    virtual void set(RequestState::Enum state) {
      Status::set(state);
      if (--remaining_ == 0) uv_stop(loop_);
    }

    void success() { set(SUCCESS); }
    void error_failed_write() { set(ERROR_FAILED_WRITE); }
    void error_no_connection() { set(ERROR_NO_CONNECTION); }
    void error() { set(ERROR); }
    void error_response() { set(ERROR_RESPONSE); }
    void timeout() { set(TIMEOUT); }

  protected:
    uv_loop_t* loop_;
    size_t remaining_;
  };

  class RequestStatusWithManager
      : public RequestStatus {
  public:
    RequestStatusWithManager(uv_loop_t* loop, int num_nodes = NUM_NODES)
      : RequestStatus(loop, num_nodes) { }

    ~RequestStatusWithManager() {
      ConnectionPoolManager::Ptr temp(manager());
      if (temp) temp->close();
      uv_run(loop_, UV_RUN_DEFAULT); // Allow the loop to cleanup
    }

    void set_manager(const ConnectionPoolManager::Ptr& manager) {
      manager_ = manager;
    }

    ConnectionPoolManager::Ptr manager() {
      return manager_;
    }

    virtual void set(RequestState::Enum state) {
      RequestStatus::set(state);
    }

  private:
    ConnectionPoolManager::Ptr manager_;
  };

  struct ListenerState {
    enum Enum {
      UP,
      DOWN,
      CRITICAL_ERROR,
      CRITICAL_ERROR_INVALID_PROTOCOL,
      CRITICAL_ERROR_KEYSPACE,
      CRITICAL_ERROR_AUTH,
      CRITICAL_ERROR_SSL_HANDSHAKE,
      CRITICAL_ERROR_SSL_VERIFY
    };

    static const char* to_string(Enum state) {
      switch (state) {
        case UP: return "UP";
        case DOWN: return "DOWN";
        case CRITICAL_ERROR: return "CRITICAL_ERROR";
        case CRITICAL_ERROR_INVALID_PROTOCOL: return "CRITICAL_ERROR_INVALID_PROTOCOL";
        case CRITICAL_ERROR_KEYSPACE: return "CRITICAL_ERROR_KEYSPACE";
        case CRITICAL_ERROR_AUTH: return "CRITICAL_ERROR_AUTH";
        case CRITICAL_ERROR_SSL_HANDSHAKE: return "CRITICAL_ERROR_SSL_HANDSHAKE";
        case CRITICAL_ERROR_SSL_VERIFY: return "CRITICAL_ERROR_SSL_VERIFY";
      }
      return "";
    }
  };

  class ListenerStatus
      : public ListenerState
      , public Status<ListenerState::Enum> {
  public:
    ListenerStatus(uv_loop_t* loop, int num_nodes = NUM_NODES)
      : loop_(loop)
      , count_(num_nodes)
      , remaining_(num_nodes) { }

    void reset() {
      remaining_ = count_;
    }

    void up() { set(UP); }
    void down() { set(DOWN); }
    void critical_error() { set(CRITICAL_ERROR); }
    void critical_error_invalid_protocol() { set(CRITICAL_ERROR_INVALID_PROTOCOL); }
    void critical_error_keyspace() { set(CRITICAL_ERROR_KEYSPACE); }
    void critical_error_auth() { set(CRITICAL_ERROR_AUTH); }
    void critical_error_ssl_handshake() { set(CRITICAL_ERROR_SSL_HANDSHAKE); }
    void critical_error_ssl_verify() { set(CRITICAL_ERROR_SSL_VERIFY); }

  private:
    virtual void set(ListenerState::Enum state) {
      Status::set(state);
      if (--remaining_ == 0) uv_stop(loop_);
    }

  private:
    uv_loop_t* loop_;
    size_t count_;
    size_t remaining_;
  };

  class Listener : public ConnectionPoolManagerListener {
  public:
    Listener(ListenerStatus* status)
      : status_(status) { }

    void reset(ListenerStatus* status) {
      status_ = status;
    }

    ListenerStatus* status() const {
      return status_;
    }

    virtual void on_pool_up(const Address& address)  {
      status_->up();
    }

    virtual void on_pool_down(const Address& address) {
      status_->down();
    }

    virtual void on_pool_critical_error(const Address& address,
                                   Connector::ConnectionError code,
                                   const String& message)  {
      switch (code) {
        case Connector::CONNECTION_ERROR_INVALID_PROTOCOL:
          status_->critical_error_invalid_protocol();
          break;
        case Connector::CONNECTION_ERROR_KEYSPACE:
          status_->critical_error_keyspace();
          break;
        case Connector::CONNECTION_ERROR_AUTH:
          status_->critical_error_auth();
          break;
        case Connector::CONNECTION_ERROR_SSL_HANDSHAKE:
          status_->critical_error_ssl_handshake();
          break;
        case Connector::CONNECTION_ERROR_SSL_VERIFY:
          status_->critical_error_ssl_verify();
          break;
        default:
          status_->critical_error();
          break;
      }
    }

    virtual void on_close(ConnectionPoolManager* manager) {
      Memory::deallocate(this);
    }

  private:
    ListenerStatus* status_;
  };


  class RequestCallback : public SimpleRequestCallback {
  public:
    RequestCallback(RequestStatus* status)
      : SimpleRequestCallback("SELECT * FROM blah")
      , status_(status) { }

    virtual void on_internal_set(ResponseMessage* response) {
      if (response->response_body()->opcode() == CQL_OPCODE_RESULT) {
        status_->success();
      } else {
        status_->error_response();
      }
    }

    virtual void on_internal_error(CassError code, const String& message) {
      status_->error();
    }

    virtual void on_internal_timeout() {
      status_->timeout();
    }

  private:
    RequestStatus* status_;
  };

  PoolUnitTest()
    : mockssandra::SimpleClusterTest(NUM_NODES) { }

  AddressVec addresses() const {
    mockssandra::Ipv4AddressGenerator generator;
    AddressVec addresses;
    for (int i = 0; i < NUM_NODES; ++i) {
      addresses.push_back(generator.next());
    }
    return addresses;
  }

  ConnectionPoolManagerSettings use_ssl() {
    ConnectionPoolManagerSettings settings;
    settings.connection_settings = mockssandra::SimpleClusterTest::use_ssl();
    return settings;
  }

  virtual void SetUp() {
    mockssandra::SimpleClusterTest::SetUp();
    ASSERT_EQ(0, uv_loop_init(&loop_));
  }

  virtual void TearDown() {
    uv_loop_close(&loop_);
    mockssandra::SimpleClusterTest::TearDown();
  }

  uv_loop_t* loop() {
    return &loop_;
  }

  void run_request(const ConnectionPoolManager::Ptr& manager, const Address& address) {
    PooledConnection::Ptr connection = manager->find_least_busy(address);
    if (connection) {
      RequestStatus status(manager->loop(), 1);
      RequestCallback::Ptr callback(Memory::allocate<RequestCallback>(&status));
      EXPECT_TRUE(connection->write(callback.get())) << "Unable to write request to connection " << address.to_string();
      connection->flush(); // Flush requests to avoid unnecessary timeouts
      uv_run(loop(), UV_RUN_DEFAULT);
      EXPECT_EQ(status.count(RequestState::SUCCESS), 1u) << status.results();
    } else {
      EXPECT_TRUE(false) << "No connection available for " << address.to_string();
    }
  }

  static void on_pool_connected(ConnectionPoolManagerInitializer* initializer) {
    RequestStatusWithManager* status = static_cast<RequestStatusWithManager*>(initializer->data());

    mockssandra::Ipv4AddressGenerator generator;
    ConnectionPoolManager::Ptr manager = initializer->release_manager();
    status->set_manager(manager);

    for (int i = 0; i < NUM_NODES; ++i) {
      PooledConnection::Ptr connection = manager->find_least_busy(generator.next());
      if (connection) {
        RequestCallback::Ptr callback(Memory::allocate<RequestCallback>(status));
        if(!connection->write(callback.get())) {
          status->error_failed_write();
        }
      } else {
        status->error_no_connection();
      }
      manager->flush(); // Flush requests to avoid unnecessary timeouts
    }
  }

  static void on_pool_nop(ConnectionPoolManagerInitializer* initializer) {
    RequestStatusWithManager* request_status = static_cast<RequestStatusWithManager*>(initializer->data());
    ConnectionPoolManager::Ptr manager = initializer->release_manager();
    request_status->set_manager(manager);
  }

private:
  uv_loop_t loop_;
};

std::ostream& operator<<(std::ostream& os, const Vector<PoolUnitTest::RequestState::Enum>& states) {
  os << "[";
  bool first = true;;
  for (Vector<PoolUnitTest::RequestState::Enum>::const_iterator it = states.begin(),
       end = states.end(); it != end; ++it) {
    if (!first) {
      os << ", ";
    } else {
      first = false;
    }
    os << PoolUnitTest::RequestState::to_string(*it);
  }
  os << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, const Vector<PoolUnitTest::ListenerState::Enum>& states) {
  os << "[";
  bool first = true;;
  for (Vector<PoolUnitTest::ListenerState::Enum>::const_iterator it = states.begin(),
       end = states.end(); it != end; ++it) {
    if (!first) {
      os << ", ";
    } else {
      first = false;
    }
    os << PoolUnitTest::ListenerState::to_string(*it);
  }
  os << "]";
  return os;
}

TEST_F(PoolUnitTest, Simple) {
  start_all();

  RequestStatusWithManager status(loop());

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&status),
          on_pool_connected));

  initializer
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(status.count(RequestStatus::SUCCESS), NUM_NODES) << status.results();
}

TEST_F(PoolUnitTest, Keyspace) {
  mockssandra::SimpleRequestHandlerBuilder builder;

  builder
      .on(mockssandra::OPCODE_QUERY)
      .use_keyspace("foo")
      .validate_query().void_result();
  mockssandra::SimpleCluster cluster(builder.build(), NUM_NODES);

  cluster.start_all();

  RequestStatusWithManager status(loop());

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&status),
          on_pool_connected));

  AddressVec addresses = this->addresses();

  initializer
      ->with_keyspace("foo")
      ->initialize(loop(), addresses);
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(status.count(RequestStatus::SUCCESS), NUM_NODES) << status.results();

  ConnectionPoolManager::Ptr manager = status.manager();
  ASSERT_TRUE(manager);

  for (size_t i = 0; i < NUM_NODES; ++i) {
    PooledConnection::Ptr connection = manager->find_least_busy(addresses[i]);
    if (connection) {
      EXPECT_EQ(connection->keyspace(), "foo");
    } else {
      EXPECT_TRUE(false) << "Unable to get connection for " << addresses[i].to_string();
    }
  }
}

TEST_F(PoolUnitTest, Auth) {
  mockssandra::SimpleCluster cluster(
        mockssandra::AuthRequestHandlerBuilder().build(), NUM_NODES);
  cluster.start_all();

  RequestStatusWithManager status(loop());

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&status),
          on_pool_connected));

  ConnectionPoolManagerSettings settings;
  settings.connection_settings.auth_provider.reset(Memory::allocate<PlainTextAuthProvider>("cassandra", "cassandra"));

  initializer
      ->with_settings(settings)
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(status.count(RequestStatus::SUCCESS), NUM_NODES) << status.results();
}

TEST_F(PoolUnitTest, Ssl) {
  ConnectionPoolManagerSettings settings(use_ssl());

  start_all();

  RequestStatusWithManager status(loop());

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&status),
          on_pool_connected));

  initializer
      ->with_settings(settings)
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(status.count(RequestStatus::SUCCESS), NUM_NODES) << status.results();
}

TEST_F(PoolUnitTest, Listener) {
  start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  initializer
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::UP), NUM_NODES) << listener_status.results();
  EXPECT_EQ(initializer->failures().size(), 0u);
}

TEST_F(PoolUnitTest, ListenerDown) {
  start(1);

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  initializer
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::UP), 1u) << listener_status.results();
  EXPECT_EQ(listener_status.count(ListenerStatus::DOWN), NUM_NODES - 1) << listener_status.results();
  EXPECT_EQ(initializer->failures().size(), 0u);
}

TEST_F(PoolUnitTest, AddRemove) {
  start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  AddressVec addresses = this->addresses();

  initializer
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses);
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::UP), NUM_NODES) << listener_status.results();

  ConnectionPoolManager::Ptr manager = request_status.manager();
  ASSERT_TRUE(manager);

  for (size_t i = 0; i < NUM_NODES; ++i) {
    ListenerStatus single_listener_status(loop(), 1);
    static_cast<Listener*>(manager->listener())->reset(&single_listener_status);

    manager->remove(addresses[i]); // Remove node
    uv_run(loop(), UV_RUN_DEFAULT);
    EXPECT_EQ(single_listener_status.count(ListenerStatus::DOWN), 1u) << single_listener_status.results();
    EXPECT_FALSE(manager->find_least_busy(addresses[i]));

    single_listener_status.reset();

    manager->add(addresses[i]); // Add node
    uv_run(loop(), UV_RUN_DEFAULT);
    EXPECT_EQ(single_listener_status.count(ListenerStatus::UP), 1u) << single_listener_status.results();
    run_request(manager, addresses[i]);
  }
}

TEST_F(PoolUnitTest, Reconnect) {
  start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  AddressVec addresses = this->addresses();

  ConnectionPoolManagerSettings settings;
  settings.reconnect_wait_time_ms = 0; // Reconnect immediately

  initializer
      ->with_settings(settings)
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses);
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::UP), NUM_NODES) << listener_status.results();

  ConnectionPoolManager::Ptr manager = request_status.manager();
  ASSERT_TRUE(manager);

  for (size_t i = 0; i < NUM_NODES; ++i) {
    ListenerStatus single_listener_status(loop(), 1);
    static_cast<Listener*>(manager->listener())->reset(&single_listener_status);

    stop(i + 1); // Stop node
    uv_run(loop(), UV_RUN_DEFAULT);
    EXPECT_EQ(single_listener_status.count(ListenerStatus::DOWN), 1u) << single_listener_status.results();
    EXPECT_FALSE(manager->find_least_busy(addresses[i]));

    single_listener_status.reset();

    start(i + 1); // Start node
    uv_run(loop(), UV_RUN_DEFAULT);
    EXPECT_EQ(single_listener_status.count(ListenerStatus::UP), 1u) << single_listener_status.results();
    run_request(manager, addresses[i]);
  }
}

TEST_F(PoolUnitTest, Timeout) {
  mockssandra::RequestHandler::Builder builder;
  builder.on(mockssandra::OPCODE_STARTUP).no_result(); // Don't return a response

  mockssandra::SimpleCluster cluster(builder.build(), NUM_NODES);
  cluster.start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  ConnectionPoolManagerSettings settings;
  settings.connection_settings.connect_timeout_ms = 200;

  initializer
      ->with_settings(settings)
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::DOWN), NUM_NODES) << listener_status.results();
}


TEST_F(PoolUnitTest, InvalidProtocol) {
  start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          0x7F,  // Invalid protocol version
          static_cast<void*>(&request_status),
          on_pool_nop));

  initializer
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::CRITICAL_ERROR_INVALID_PROTOCOL), NUM_NODES) << listener_status.results();

  ConnectionPoolConnector::Vec failures = initializer->failures();
  EXPECT_EQ(failures.size(), NUM_NODES);

  for (ConnectionPoolConnector::Vec::const_iterator it = failures.begin(),
       end = failures.end(); it != end; ++it) {
    EXPECT_EQ((*it)->error_code(), Connector::CONNECTION_ERROR_INVALID_PROTOCOL);
  }
}

TEST_F(PoolUnitTest, InvalidKeyspace) {
  mockssandra::SimpleRequestHandlerBuilder builder;
  builder
      .on(mockssandra::OPCODE_QUERY)
      .use_keyspace("foo")
      .validate_query().void_result();
  mockssandra::SimpleCluster cluster(builder.build(), NUM_NODES);

  cluster.start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  initializer
      ->with_keyspace("invalid")
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::CRITICAL_ERROR_KEYSPACE), NUM_NODES) << listener_status.results();
}

TEST_F(PoolUnitTest, InvalidAuth) {
  mockssandra::SimpleCluster cluster(
        mockssandra::AuthRequestHandlerBuilder().build(), NUM_NODES);
  cluster.start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  ConnectionPoolManagerSettings settings;
  settings.connection_settings.auth_provider.reset(Memory::allocate<PlainTextAuthProvider>("invalid", "invalid"));

  initializer
      ->with_settings(settings)
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::CRITICAL_ERROR_AUTH), NUM_NODES) << listener_status.results();
}

TEST_F(PoolUnitTest, InvalidNoSsl) {
  start_all(); // Start without ssl

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  SslContext::Ptr ssl_context(SslContextFactory::create());

  ConnectionPoolManagerSettings settings;
  settings.connection_settings.socket_settings.ssl_context = ssl_context;
  settings.connection_settings.socket_settings.hostname_resolution_enabled = true;

  initializer
      ->with_settings(settings)
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::CRITICAL_ERROR_SSL_HANDSHAKE), NUM_NODES) << listener_status.results();
}

TEST_F(PoolUnitTest, InvalidSsl) {
  use_ssl();
  start_all();

  ListenerStatus listener_status(loop());
  RequestStatusWithManager request_status(loop(), 0);

  ConnectionPoolManagerInitializer::Ptr initializer(
        Memory::allocate<ConnectionPoolManagerInitializer>(
          PROTOCOL_VERSION,
          static_cast<void*>(&request_status),
          on_pool_nop));

  SslContext::Ptr ssl_context(SslContextFactory::create()); // No trusted cert

  ConnectionPoolManagerSettings settings;
  settings.connection_settings.socket_settings.ssl_context = ssl_context;
  settings.connection_settings.socket_settings.hostname_resolution_enabled = true;

  initializer
      ->with_settings(settings)
      ->with_listener(Memory::allocate<Listener>(&listener_status))
      ->initialize(loop(), addresses());
  uv_run(loop(), UV_RUN_DEFAULT);

  EXPECT_EQ(listener_status.count(ListenerStatus::CRITICAL_ERROR_SSL_VERIFY), NUM_NODES) << listener_status.results();
}

TEST_F(PoolUnitTest, PartialReconnect) {
  // TODO:
}

TEST_F(PoolUnitTest, LowNumberOfStreams) {
  // TODO:
}
