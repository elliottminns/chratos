#include <chratos/node/node.hpp>

#include <chratos/lib/interface.h>
#include <chratos/node/common.hpp>
#include <chratos/node/rpc.hpp>

#include <algorithm>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <upnpcommands.h>

double constexpr chratos::node::price_max;
double constexpr chratos::node::free_cutoff;
std::chrono::seconds constexpr chratos::node::period;
std::chrono::seconds constexpr chratos::node::cutoff;
std::chrono::seconds constexpr chratos::node::syn_cookie_cutoff;
std::chrono::minutes constexpr chratos::node::backup_interval;
int constexpr chratos::port_mapping::mapping_timeout;
int constexpr chratos::port_mapping::check_timeout;
unsigned constexpr chratos::active_transactions::announce_interval_ms;
size_t constexpr chratos::block_arrival::arrival_size_min;
std::chrono::seconds constexpr chratos::block_arrival::arrival_time_min;

chratos::endpoint chratos::map_endpoint_to_v6 (chratos::endpoint const & endpoint_a)
{
  auto endpoint_l (endpoint_a);
  if (endpoint_l.address ().is_v4 ())
  {
    endpoint_l = chratos::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
  }
  return endpoint_l;
}

chratos::network::network (chratos::node & node_a, uint16_t port) :
socket (node_a.service, chratos::endpoint (boost::asio::ip::address_v6::any (), port)),
resolver (node_a.service),
node (node_a),
on (true)
{
}

void chratos::network::receive ()
{
  if (node.config.logging.network_packet_logging ())
  {
    BOOST_LOG (node.log) << "Receiving packet";
  }
  std::unique_lock<std::mutex> lock (socket_mutex);
  socket.async_receive_from (boost::asio::buffer (buffer.data (), buffer.size ()), remote, [this](boost::system::error_code const & error, size_t size_a) {
    receive_action (error, size_a);
  });
}

void chratos::network::stop ()
{
  on = false;
  socket.close ();
  resolver.cancel ();
}

void chratos::network::send_keepalive (chratos::endpoint const & endpoint_a)
{
  assert (endpoint_a.address ().is_v6 ());
  chratos::keepalive message;
  node.peers.random_fill (message.peers);
  std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
  {
    chratos::vectorstream stream (*bytes);
    message.serialize (stream);
  }
  if (node.config.logging.network_keepalive_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Keepalive req sent to %1%") % endpoint_a);
  }
  std::weak_ptr<chratos::node> node_w (node.shared ());
  send_buffer (bytes->data (), bytes->size (), endpoint_a, [bytes, node_w, endpoint_a](boost::system::error_code const & ec, size_t) {
    if (auto node_l = node_w.lock ())
    {
      if (ec && node_l->config.logging.network_keepalive_logging ())
      {
        BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending keepalive to %1%: %2%") % endpoint_a % ec.message ());
      }
      else
      {
        node_l->stats.inc (chratos::stat::type::message, chratos::stat::detail::keepalive, chratos::stat::dir::out);
      }
    }
  });
}

void chratos::node::keepalive (std::string const & address_a, uint16_t port_a)
{
  auto node_l (shared_from_this ());
  network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a)), [node_l, address_a, port_a](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
    if (!ec)
    {
      for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
      {
        node_l->send_keepalive (chratos::map_endpoint_to_v6 (i->endpoint ()));
      }
    }
    else
    {
      BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving address: %1%:%2%: %3%") % address_a % port_a % ec.message ());
    }
  });
}

void chratos::network::send_node_id_handshake (chratos::endpoint const & endpoint_a, boost::optional<chratos::uint256_union> const & query, boost::optional<chratos::uint256_union> const & respond_to)
{
  assert (endpoint_a.address ().is_v6 ());
  boost::optional<std::pair<chratos::account, chratos::signature>> response (boost::none);
  if (respond_to)
  {
    response = std::make_pair (node.node_id.pub, chratos::sign_message (node.node_id.prv, node.node_id.pub, *respond_to));
    assert (!chratos::validate_message (response->first, *respond_to, response->second));
  }
  chratos::node_id_handshake message (query, response);
  std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
  {
    chratos::vectorstream stream (*bytes);
    message.serialize (stream);
  }
  if (node.config.logging.network_node_id_handshake_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Node ID handshake sent with node ID %1% to %2%: query %3%, respond_to %4% (signature %5%)") % node.node_id.pub.to_account () % endpoint_a % (query ? query->to_string () : std::string ("[none]")) % (respond_to ? respond_to->to_string () : std::string ("[none]")) % (response ? response->second.to_string () : std::string ("[none]")));
  }
  node.stats.inc (chratos::stat::type::message, chratos::stat::detail::node_id_handshake, chratos::stat::dir::out);
  std::weak_ptr<chratos::node> node_w (node.shared ());
  send_buffer (bytes->data (), bytes->size (), endpoint_a, [bytes, node_w, endpoint_a](boost::system::error_code const & ec, size_t) {
    if (auto node_l = node_w.lock ())
    {
      if (ec && node_l->config.logging.network_node_id_handshake_logging ())
      {
        BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending node ID handshake to %1% %2%") % endpoint_a % ec.message ());
      }
    }
  });
}

void chratos::network::republish (chratos::block_hash const & hash_a, std::shared_ptr<std::vector<uint8_t>> buffer_a, chratos::endpoint endpoint_a)
{
  if (node.config.logging.network_publish_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Publishing %1% to %2%") % hash_a.to_string () % endpoint_a);
  }
  std::weak_ptr<chratos::node> node_w (node.shared ());
  send_buffer (buffer_a->data (), buffer_a->size (), endpoint_a, [buffer_a, node_w, endpoint_a](boost::system::error_code const & ec, size_t size) {
    if (auto node_l = node_w.lock ())
    {
      if (ec && node_l->config.logging.network_logging ())
      {
        BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending publish to %1%: %2%") % endpoint_a % ec.message ());
      }
      else
      {
        node_l->stats.inc (chratos::stat::type::message, chratos::stat::detail::publish, chratos::stat::dir::out);
      }
    }
  });
}

template <typename T>
bool confirm_block (MDB_txn * transaction_a, chratos::node & node_a, T & list_a, std::shared_ptr<chratos::block> block_a)
{
  bool result (false);
  if (node_a.config.enable_voting)
  {
    node_a.wallets.foreach_representative (transaction_a, [&result, &block_a, &list_a, &node_a, &transaction_a](chratos::public_key const & pub_a, chratos::raw_key const & prv_a) {
      result = true;
      auto vote (node_a.store.vote_generate (transaction_a, pub_a, prv_a, block_a));
      chratos::confirm_ack confirm (vote);
      std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
      {
        chratos::vectorstream stream (*bytes);
        confirm.serialize (stream);
      }
      for (auto j (list_a.begin ()), m (list_a.end ()); j != m; ++j)
      {
        node_a.network.confirm_send (confirm, bytes, *j);
      }
    });
  }
  return result;
}

template <>
bool confirm_block (MDB_txn * transaction_a, chratos::node & node_a, chratos::endpoint & peer_a, std::shared_ptr<chratos::block> block_a)
{
  std::array<chratos::endpoint, 1> endpoints;
  endpoints[0] = peer_a;
  auto result (confirm_block (transaction_a, node_a, endpoints, std::move (block_a)));
  return result;
}

void chratos::network::republish_block (MDB_txn * transaction, std::shared_ptr<chratos::block> block, bool enable_voting)
{
  auto hash (block->hash ());
  auto list (node.peers.list_fanout ());
  // If we're a representative, broadcast a signed confirm, otherwise an unsigned publish
  if (!enable_voting || !confirm_block (transaction, node, list, block))
  {
    chratos::publish message (block);
    std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
    {
      chratos::vectorstream stream (*bytes);
      message.serialize (stream);
    }
    auto hash (block->hash ());
    for (auto i (list.begin ()), n (list.end ()); i != n; ++i)
    {
      republish (hash, bytes, *i);
    }
    if (node.config.logging.network_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was republished to peers") % hash.to_string ());
    }
  }
  else
  {
    if (node.config.logging.network_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was confirmed to peers") % hash.to_string ());
    }
  }
}

// In order to rate limit network traffic we republish:
// 1) Only if they are a non-replay vote of a block that's actively settling. Settling blocks are limited by block PoW
// 2) The rep has a weight > Y to prevent creating a lot of small-weight accounts to send out votes
// 3) Only if a vote for this block from this representative hasn't been received in the previous X second.
//    This prevents rapid publishing of votes with increasing sequence numbers.
//
// These rules are implemented by the caller, not this function.
void chratos::network::republish_vote (std::shared_ptr<chratos::vote> vote_a)
{
  chratos::confirm_ack confirm (vote_a);
  std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
  {
    chratos::vectorstream stream (*bytes);
    confirm.serialize (stream);
  }
  auto list (node.peers.list_fanout ());
  for (auto j (list.begin ()), m (list.end ()); j != m; ++j)
  {
    node.network.confirm_send (confirm, bytes, *j);
  }
}

void chratos::network::broadcast_confirm_req (std::shared_ptr<chratos::block> block_a)
{
  auto list (std::make_shared<std::vector<chratos::peer_information>> (node.peers.representatives (std::numeric_limits<size_t>::max ())));
  if (list->empty () || node.peers.total_weight () < node.config.online_weight_minimum.number ())
  {
    // broadcast request to all peers
    list = std::make_shared<std::vector<chratos::peer_information>> (node.peers.list_vector ());
  }
  broadcast_confirm_req_base (block_a, list, 0);
}

void chratos::network::broadcast_confirm_req_base (std::shared_ptr<chratos::block> block_a, std::shared_ptr<std::vector<chratos::peer_information>> endpoints_a, unsigned delay_a)
{
  const size_t max_reps = 10;
  if (node.config.logging.network_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Broadcasting confirm req for block %1% to %2% representatives") % block_a->hash ().to_string () % std::min (endpoints_a->size (), max_reps));
  }
  auto count (0);
  while (!endpoints_a->empty () && count < max_reps)
  {
    send_confirm_req (endpoints_a->back ().endpoint, block_a);
    endpoints_a->pop_back ();
    count++;
  }
  if (!endpoints_a->empty ())
  {
    std::weak_ptr<chratos::node> node_w (node.shared ());
    node.alarm.add (std::chrono::steady_clock::now () + std::chrono::milliseconds (delay_a), [node_w, block_a, endpoints_a, delay_a]() {
      if (auto node_l = node_w.lock ())
      {
        node_l->network.broadcast_confirm_req_base (block_a, endpoints_a, delay_a + 50);
      }
    });
  }
}

void chratos::network::send_confirm_req (chratos::endpoint const & endpoint_a, std::shared_ptr<chratos::block> block)
{
  chratos::confirm_req message (block);
  std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
  {
    chratos::vectorstream stream (*bytes);
    message.serialize (stream);
  }
  if (node.config.logging.network_message_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm req to %1%") % endpoint_a);
  }
  std::weak_ptr<chratos::node> node_w (node.shared ());
  node.stats.inc (chratos::stat::type::message, chratos::stat::detail::confirm_req, chratos::stat::dir::out);
  send_buffer (bytes->data (), bytes->size (), endpoint_a, [bytes, node_w](boost::system::error_code const & ec, size_t size) {
    if (auto node_l = node_w.lock ())
    {
      if (ec && node_l->config.logging.network_logging ())
      {
        BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending confirm request: %1%") % ec.message ());
      }
    }
  });
}

template <typename T>
void rep_query (chratos::node & node_a, T const & peers_a)
{
  chratos::transaction transaction (node_a.store.environment, nullptr, false);
  std::shared_ptr<chratos::block> block (node_a.store.block_random (transaction));
  auto hash (block->hash ());
  node_a.rep_crawler.add (hash);
  for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
  {
    node_a.peers.rep_request (*i);
    node_a.network.send_confirm_req (*i, block);
  }
  std::weak_ptr<chratos::node> node_w (node_a.shared ());
  node_a.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w, hash]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->rep_crawler.remove (hash);
    }
  });
}

template <>
void rep_query (chratos::node & node_a, chratos::endpoint const & peers_a)
{
  std::array<chratos::endpoint, 1> peers;
  peers[0] = peers_a;
  rep_query (node_a, peers);
}

namespace
{
class network_message_visitor : public chratos::message_visitor
{
public:
  network_message_visitor (chratos::node & node_a, chratos::endpoint const & sender_a) :
  node (node_a),
  sender (sender_a)
  {
  }
  virtual ~network_message_visitor () = default;
  void keepalive (chratos::keepalive const & message_a) override
  {
    if (node.config.logging.network_keepalive_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Received keepalive message from %1%") % sender);
    }
    node.stats.inc (chratos::stat::type::message, chratos::stat::detail::keepalive, chratos::stat::dir::in);
    if (node.peers.contacted (sender, message_a.header.version_using))
    {
      auto endpoint_l (chratos::map_endpoint_to_v6 (sender));
      auto cookie (node.peers.assign_syn_cookie (endpoint_l));
      if (cookie)
      {
        node.network.send_node_id_handshake (endpoint_l, *cookie, boost::none);
      }
    }
    node.network.merge_peers (message_a.peers);
  }
  void publish (chratos::publish const & message_a) override
  {
    if (node.config.logging.network_message_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Publish message from %1% for %2%") % sender % message_a.block->hash ().to_string ());
    }
    node.stats.inc (chratos::stat::type::message, chratos::stat::detail::publish, chratos::stat::dir::in);
    node.peers.contacted (sender, message_a.header.version_using);
    node.process_active (message_a.block);
    node.active.publish (message_a.block);
  }
  void confirm_req (chratos::confirm_req const & message_a) override
  {
    if (node.config.logging.network_message_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Confirm_req message from %1% for %2%") % sender % message_a.block->hash ().to_string ());
    }
    node.stats.inc (chratos::stat::type::message, chratos::stat::detail::confirm_req, chratos::stat::dir::in);
    node.peers.contacted (sender, message_a.header.version_using);
    node.process_active (message_a.block);
    node.active.publish (message_a.block);
    chratos::transaction transaction_a (node.store.environment, nullptr, false);
    auto successor (node.ledger.successor (transaction_a, message_a.block->root ()));
    if (successor != nullptr)
    {
      confirm_block (transaction_a, node, sender, std::move (successor));
    }
  }
  void confirm_ack (chratos::confirm_ack const & message_a) override
  {
    if (node.config.logging.network_message_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_ack message from %1% for %2%sequence %3%") % sender % message_a.vote->hashes_string () % std::to_string (message_a.vote->sequence));
    }
    node.stats.inc (chratos::stat::type::message, chratos::stat::detail::confirm_ack, chratos::stat::dir::in);
    node.peers.contacted (sender, message_a.header.version_using);
    for (auto & vote_block : message_a.vote->blocks)
    {
      if (!vote_block.which ())
      {
        auto block (boost::get<std::shared_ptr<chratos::block>> (vote_block));
        node.process_active (block);
        node.active.publish (block);
      }
    }
    node.vote_processor.vote (message_a.vote, sender);
  }
  void bulk_pull (chratos::bulk_pull const &) override
  {
    assert (false);
  }
  void bulk_pull_account (chratos::bulk_pull_account const &) override
  {
    assert (false);
  }
  void bulk_pull_blocks (chratos::bulk_pull_blocks const &) override
  {
    assert (false);
  }
  void bulk_push (chratos::bulk_push const &) override
  {
    assert (false);
  }
  void frontier_req (chratos::frontier_req const &) override
  {
    assert (false);
  }
  void node_id_handshake (chratos::node_id_handshake const & message_a) override
  {
    if (node.config.logging.network_node_id_handshake_logging ())
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("Received node_id_handshake message from %1% with query %2% and response account %3%") % sender % (message_a.query ? message_a.query->to_string () : std::string ("[none]")) % (message_a.response ? message_a.response->first.to_account () : std::string ("[none]")));
    }
    node.stats.inc (chratos::stat::type::message, chratos::stat::detail::node_id_handshake, chratos::stat::dir::in);
    auto endpoint_l (chratos::map_endpoint_to_v6 (sender));
    boost::optional<chratos::uint256_union> out_query;
    boost::optional<chratos::uint256_union> out_respond_to;
    if (message_a.query)
    {
      out_respond_to = message_a.query;
    }
    auto validated_response (false);
    if (message_a.response)
    {
      if (!node.peers.validate_syn_cookie (endpoint_l, message_a.response->first, message_a.response->second))
      {
        validated_response = true;
        if (message_a.response->first != node.node_id.pub)
        {
          node.peers.insert (endpoint_l, message_a.header.version_using);
        }
      }
      else if (node.config.logging.network_node_id_handshake_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Failed to validate syn cookie signature %1% by %2%") % message_a.response->second.to_string () % message_a.response->first.to_account ());
      }
    }
    if (!validated_response && !node.peers.known_peer (endpoint_l))
    {
      out_query = node.peers.assign_syn_cookie (endpoint_l);
    }
    if (out_query || out_respond_to)
    {
      node.network.send_node_id_handshake (sender, out_query, out_respond_to);
    }
  }
  chratos::node & node;
  chratos::endpoint sender;
};
}

void chratos::network::receive_action (boost::system::error_code const & error, size_t size_a)
{
  if (!error && on)
  {
    if (!chratos::reserved_address (remote, false) && remote != endpoint ())
    {
      network_message_visitor visitor (node, remote);
      chratos::message_parser parser (visitor, node.work);
      parser.deserialize_buffer (buffer.data (), size_a);
      if (parser.status != chratos::message_parser::parse_status::success)
      {
        node.stats.inc (chratos::stat::type::error);

        if (parser.status == chratos::message_parser::parse_status::insufficient_work)
        {
          if (node.config.logging.insufficient_work_logging ())
          {
            BOOST_LOG (node.log) << "Insufficient work in message";
          }

          // We've already increment error count, update detail only
          node.stats.inc_detail_only (chratos::stat::type::error, chratos::stat::detail::insufficient_work);
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_message_type)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid message type in message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_header)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid header in message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_keepalive_message)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid keepalive message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_publish_message)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid publish message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_confirm_req_message)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid confirm_req message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_confirm_ack_message)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid confirm_ack message";
          }
        }
        else if (parser.status == chratos::message_parser::parse_status::invalid_node_id_handshake_message)
        {
          if (node.config.logging.network_logging ())
          {
            BOOST_LOG (node.log) << "Invalid node_id_handshake message";
          }
        }
        else
        {
          BOOST_LOG (node.log) << "Could not deserialize buffer";
        }
      }
      else
      {
        node.stats.add (chratos::stat::type::traffic, chratos::stat::dir::in, size_a);
      }
    }
    else
    {
      if (node.config.logging.network_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Reserved sender %1%") % remote.address ().to_string ());
      }

      node.stats.inc_detail_only (chratos::stat::type::error, chratos::stat::detail::bad_sender);
    }
    receive ();
  }
  else
  {
    if (error)
    {
      if (node.config.logging.network_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("UDP Receive error: %1%") % error.message ());
      }
    }
    if (on)
    {
      node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [this]() { receive (); });
    }
  }
}

// Send keepalives to all the peers we've been notified of
void chratos::network::merge_peers (std::array<chratos::endpoint, 8> const & peers_a)
{
  for (auto i (peers_a.begin ()), j (peers_a.end ()); i != j; ++i)
  {
    if (!node.peers.reachout (*i))
    {
      send_keepalive (*i);
    }
  }
}

bool chratos::operation::operator> (chratos::operation const & other_a) const
{
  return wakeup > other_a.wakeup;
}

chratos::alarm::alarm (boost::asio::io_service & service_a) :
service (service_a),
thread ([this]() { run (); })
{
}

chratos::alarm::~alarm ()
{
  add (std::chrono::steady_clock::now (), nullptr);
  thread.join ();
}

void chratos::alarm::run ()
{
  std::unique_lock<std::mutex> lock (mutex);
  auto done (false);
  while (!done)
  {
    if (!operations.empty ())
    {
      auto & operation (operations.top ());
      if (operation.function)
      {
        if (operation.wakeup <= std::chrono::steady_clock::now ())
        {
          service.post (operation.function);
          operations.pop ();
        }
        else
        {
          auto wakeup (operation.wakeup);
          condition.wait_until (lock, wakeup);
        }
      }
      else
      {
        done = true;
      }
    }
    else
    {
      condition.wait (lock);
    }
  }
}

void chratos::alarm::add (std::chrono::steady_clock::time_point const & wakeup_a, std::function<void()> const & operation)
{
  std::lock_guard<std::mutex> lock (mutex);
  operations.push (chratos::operation ({ wakeup_a, operation }));
  condition.notify_all ();
}

chratos::logging::logging () :
ledger_logging_value (false),
ledger_duplicate_logging_value (false),
vote_logging_value (false),
network_logging_value (true),
network_message_logging_value (false),
network_publish_logging_value (false),
network_packet_logging_value (false),
network_keepalive_logging_value (false),
network_node_id_handshake_logging_value (false),
node_lifetime_tracing_value (false),
insufficient_work_logging_value (true),
log_rpc_value (true),
bulk_pull_logging_value (false),
work_generation_time_value (true),
log_to_cerr_value (false),
max_size (16 * 1024 * 1024),
rotation_size (4 * 1024 * 1024),
flush (true)
{
}

void chratos::logging::init (boost::filesystem::path const & application_path_a)
{
  static std::atomic_flag logging_already_added = ATOMIC_FLAG_INIT;
  if (!logging_already_added.test_and_set ())
  {
    boost::log::add_common_attributes ();
    if (log_to_cerr ())
    {
      boost::log::add_console_log (std::cerr, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
    }
    boost::log::add_file_log (boost::log::keywords::target = application_path_a / "log", boost::log::keywords::file_name = application_path_a / "log" / "log_%Y-%m-%d_%H-%M-%S.%N.log", boost::log::keywords::rotation_size = rotation_size, boost::log::keywords::auto_flush = flush, boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching, boost::log::keywords::max_size = max_size, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
  }
}

void chratos::logging::serialize_json (boost::property_tree::ptree & tree_a) const
{
  tree_a.put ("version", "4");
  tree_a.put ("ledger", ledger_logging_value);
  tree_a.put ("ledger_duplicate", ledger_duplicate_logging_value);
  tree_a.put ("vote", vote_logging_value);
  tree_a.put ("network", network_logging_value);
  tree_a.put ("network_message", network_message_logging_value);
  tree_a.put ("network_publish", network_publish_logging_value);
  tree_a.put ("network_packet", network_packet_logging_value);
  tree_a.put ("network_keepalive", network_keepalive_logging_value);
  tree_a.put ("network_node_id_handshake", network_node_id_handshake_logging_value);
  tree_a.put ("node_lifetime_tracing", node_lifetime_tracing_value);
  tree_a.put ("insufficient_work", insufficient_work_logging_value);
  tree_a.put ("log_rpc", log_rpc_value);
  tree_a.put ("bulk_pull", bulk_pull_logging_value);
  tree_a.put ("work_generation_time", work_generation_time_value);
  tree_a.put ("log_to_cerr", log_to_cerr_value);
  tree_a.put ("max_size", max_size);
  tree_a.put ("rotation_size", rotation_size);
  tree_a.put ("flush", flush);
}

bool chratos::logging::upgrade_json (unsigned version_a, boost::property_tree::ptree & tree_a)
{
  auto result (false);
  switch (version_a)
  {
    case 1:
      tree_a.put ("vote", vote_logging_value);
      tree_a.put ("version", "2");
      result = true;
    case 2:
      tree_a.put ("rotation_size", "4194304");
      tree_a.put ("flush", "true");
      tree_a.put ("version", "3");
      result = true;
    case 3:
      tree_a.put ("network_node_id_handshake", "false");
      tree_a.put ("version", "4");
      result = true;
    case 4:
      break;
    default:
      throw std::runtime_error ("Unknown logging_config version");
      break;
  }
  return result;
}

bool chratos::logging::deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
{
  auto result (false);
  try
  {
    auto version_l (tree_a.get_optional<std::string> ("version"));
    if (!version_l)
    {
      tree_a.put ("version", "1");
      version_l = "1";
      auto work_peers_l (tree_a.get_child_optional ("work_peers"));
      if (!work_peers_l)
      {
        tree_a.add_child ("work_peers", boost::property_tree::ptree ());
      }
      upgraded_a = true;
    }
    upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
    ledger_logging_value = tree_a.get<bool> ("ledger");
    ledger_duplicate_logging_value = tree_a.get<bool> ("ledger_duplicate");
    vote_logging_value = tree_a.get<bool> ("vote");
    network_logging_value = tree_a.get<bool> ("network");
    network_message_logging_value = tree_a.get<bool> ("network_message");
    network_publish_logging_value = tree_a.get<bool> ("network_publish");
    network_packet_logging_value = tree_a.get<bool> ("network_packet");
    network_keepalive_logging_value = tree_a.get<bool> ("network_keepalive");
    network_node_id_handshake_logging_value = tree_a.get<bool> ("network_node_id_handshake");
    node_lifetime_tracing_value = tree_a.get<bool> ("node_lifetime_tracing");
    insufficient_work_logging_value = tree_a.get<bool> ("insufficient_work");
    log_rpc_value = tree_a.get<bool> ("log_rpc");
    bulk_pull_logging_value = tree_a.get<bool> ("bulk_pull");
    work_generation_time_value = tree_a.get<bool> ("work_generation_time");
    log_to_cerr_value = tree_a.get<bool> ("log_to_cerr");
    max_size = tree_a.get<uintmax_t> ("max_size");
    rotation_size = tree_a.get<uintmax_t> ("rotation_size", 4194304);
    flush = tree_a.get<bool> ("flush", true);
  }
  catch (std::runtime_error const &)
  {
    result = true;
  }
  return result;
}

bool chratos::logging::ledger_logging () const
{
  return ledger_logging_value;
}

bool chratos::logging::ledger_duplicate_logging () const
{
  return ledger_logging () && ledger_duplicate_logging_value;
}

bool chratos::logging::vote_logging () const
{
  return vote_logging_value;
}

bool chratos::logging::network_logging () const
{
  return network_logging_value;
}

bool chratos::logging::network_message_logging () const
{
  return network_logging () && network_message_logging_value;
}

bool chratos::logging::network_publish_logging () const
{
  return network_logging () && network_publish_logging_value;
}

bool chratos::logging::network_packet_logging () const
{
  return network_logging () && network_packet_logging_value;
}

bool chratos::logging::network_keepalive_logging () const
{
  return network_logging () && network_keepalive_logging_value;
}

bool chratos::logging::network_node_id_handshake_logging () const
{
  return network_logging () && network_node_id_handshake_logging_value;
}

bool chratos::logging::node_lifetime_tracing () const
{
  return node_lifetime_tracing_value;
}

bool chratos::logging::insufficient_work_logging () const
{
  return network_logging () && insufficient_work_logging_value;
}

bool chratos::logging::log_rpc () const
{
  return network_logging () && log_rpc_value;
}

bool chratos::logging::bulk_pull_logging () const
{
  return network_logging () && bulk_pull_logging_value;
}

bool chratos::logging::callback_logging () const
{
  return network_logging ();
}

bool chratos::logging::work_generation_time () const
{
  return work_generation_time_value;
}

bool chratos::logging::log_to_cerr () const
{
  return log_to_cerr_value;
}

chratos::node_init::node_init () :
block_store_init (false),
wallet_init (false)
{
}

bool chratos::node_init::error ()
{
  return block_store_init || wallet_init;
}

chratos::node_config::node_config () :
node_config (chratos::network::node_port, chratos::logging ())
{
}

chratos::node_config::node_config (uint16_t peering_port_a, chratos::logging const & logging_a) :
peering_port (peering_port_a),
logging (logging_a),
bootstrap_fraction_numerator (1),
receive_minimum (chratos::chr_ratio),
online_weight_minimum (60000 * chratos::Mchr_ratio),
dividend_minimum (chratos::minimum_dividend_amount),
online_weight_quorum (50),
password_fanout (1024),
io_threads (std::max<unsigned> (4, std::thread::hardware_concurrency ())),
work_threads (std::max<unsigned> (4, std::thread::hardware_concurrency ())),
enable_voting (true),
bootstrap_connections (4),
bootstrap_connections_max (64),
callback_port (0),
lmdb_max_dbs (128)
{
  const char * epoch_message ("epoch v1 block");
  strncpy ((char *)epoch_block_link.bytes.data (), epoch_message, epoch_block_link.bytes.size ());
  epoch_block_signer = chratos::genesis_account;
  switch (chratos::chratos_network)
  {
    case chratos::chratos_networks::chratos_test_network:
      preconfigured_representatives.push_back (chratos::genesis_account);
      break;
    case chratos::chratos_networks::chratos_beta_network:
      preconfigured_representatives.push_back (chratos::genesis_account);
      preconfigured_peers.push_back ("chratos-beta.vidaru.org");
      break;
    case chratos::chratos_networks::chratos_live_network:
      preconfigured_representatives.push_back (chratos::genesis_account);
      preconfigured_peers.push_back ("chratos.seeds.vidaru.org");

      // 2018-09-01 UTC 00:00 in unix time
      // Technically, time_t is never defined to be unix time, but compilers implement it as such
      generate_hash_votes_at = std::chrono::system_clock::from_time_t (1535760000);
      break;
    default:
      assert (false);
      break;
  }
}

void chratos::node_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
  tree_a.put ("version", "14");
  tree_a.put ("peering_port", std::to_string (peering_port));
  tree_a.put ("bootstrap_fraction_numerator", std::to_string (bootstrap_fraction_numerator));
  tree_a.put ("receive_minimum", receive_minimum.to_string_dec ());
  boost::property_tree::ptree logging_l;
  logging.serialize_json (logging_l);
  tree_a.add_child ("logging", logging_l);
  boost::property_tree::ptree work_peers_l;
  for (auto i (work_peers.begin ()), n (work_peers.end ()); i != n; ++i)
  {
    boost::property_tree::ptree entry;
    entry.put ("", boost::str (boost::format ("%1%:%2%") % i->first % i->second));
    work_peers_l.push_back (std::make_pair ("", entry));
  }
  tree_a.add_child ("work_peers", work_peers_l);
  boost::property_tree::ptree preconfigured_peers_l;
  for (auto i (preconfigured_peers.begin ()), n (preconfigured_peers.end ()); i != n; ++i)
  {
    boost::property_tree::ptree entry;
    entry.put ("", *i);
    preconfigured_peers_l.push_back (std::make_pair ("", entry));
  }
  tree_a.add_child ("preconfigured_peers", preconfigured_peers_l);
  boost::property_tree::ptree preconfigured_representatives_l;
  for (auto i (preconfigured_representatives.begin ()), n (preconfigured_representatives.end ()); i != n; ++i)
  {
    boost::property_tree::ptree entry;
    entry.put ("", i->to_account ());
    preconfigured_representatives_l.push_back (std::make_pair ("", entry));
  }
  tree_a.add_child ("preconfigured_representatives", preconfigured_representatives_l);
  tree_a.put ("online_weight_minimum", online_weight_minimum.to_string_dec ());
  tree_a.put ("online_weight_quorum", std::to_string (online_weight_quorum));
  tree_a.put ("password_fanout", std::to_string (password_fanout));
  tree_a.put ("io_threads", std::to_string (io_threads));
  tree_a.put ("work_threads", std::to_string (work_threads));
  tree_a.put ("enable_voting", enable_voting);
  tree_a.put ("bootstrap_connections", bootstrap_connections);
  tree_a.put ("bootstrap_connections_max", bootstrap_connections_max);
  tree_a.put ("callback_address", callback_address);
  tree_a.put ("callback_port", std::to_string (callback_port));
  tree_a.put ("callback_target", callback_target);
  tree_a.put ("lmdb_max_dbs", lmdb_max_dbs);
  tree_a.put ("generate_hash_votes_at", std::chrono::system_clock::to_time_t (generate_hash_votes_at));
}

bool chratos::node_config::upgrade_json (unsigned version, boost::property_tree::ptree & tree_a)
{
  auto result (false);
  switch (version)
  {
    case 1:
    {
      auto reps_l (tree_a.get_child ("preconfigured_representatives"));
      boost::property_tree::ptree reps;
      for (auto i (reps_l.begin ()), n (reps_l.end ()); i != n; ++i)
      {
        chratos::uint256_union account;
        account.decode_account (i->second.get<std::string> (""));
        boost::property_tree::ptree entry;
        entry.put ("", account.to_account ());
        reps.push_back (std::make_pair ("", entry));
      }
      tree_a.erase ("preconfigured_representatives");
      tree_a.add_child ("preconfigured_representatives", reps);
      tree_a.erase ("version");
      tree_a.put ("version", "2");
      result = true;
    }
    case 2:
    {
      tree_a.put ("inactive_supply", chratos::uint128_union (0).to_string_dec ());
      tree_a.put ("password_fanout", std::to_string (1024));
      tree_a.put ("io_threads", std::to_string (io_threads));
      tree_a.put ("work_threads", std::to_string (work_threads));
      tree_a.erase ("version");
      tree_a.put ("version", "3");
      result = true;
    }
    case 3:
      tree_a.erase ("receive_minimum");
      tree_a.put ("receive_minimum", chratos::chr_ratio.convert_to<std::string> ());
      tree_a.erase ("version");
      tree_a.put ("version", "4");
      result = true;
    case 4:
      tree_a.erase ("receive_minimum");
      tree_a.put ("receive_minimum", chratos::chr_ratio.convert_to<std::string> ());
      tree_a.erase ("version");
      tree_a.put ("version", "5");
      result = true;
    case 5:
      tree_a.put ("enable_voting", enable_voting);
      tree_a.erase ("packet_delay_microseconds");
      tree_a.erase ("rebroadcast_delay");
      tree_a.erase ("creation_rebroadcast");
      tree_a.erase ("version");
      tree_a.put ("version", "6");
      result = true;
    case 6:
      tree_a.put ("bootstrap_connections", 16);
      tree_a.put ("callback_address", "");
      tree_a.put ("callback_port", "0");
      tree_a.put ("callback_target", "");
      tree_a.erase ("version");
      tree_a.put ("version", "7");
      result = true;
    case 7:
      tree_a.put ("lmdb_max_dbs", "128");
      tree_a.erase ("version");
      tree_a.put ("version", "8");
      result = true;
    case 8:
      tree_a.put ("bootstrap_connections_max", "64");
      tree_a.erase ("version");
      tree_a.put ("version", "9");
      result = true;
    case 9:
      tree_a.put ("state_block_parse_canary", chratos::block_hash (0).to_string ());
      tree_a.put ("state_block_generate_canary", chratos::block_hash (0).to_string ());
      tree_a.erase ("version");
      tree_a.put ("version", "10");
      result = true;
    case 10:
      tree_a.put ("online_weight_minimum", online_weight_minimum.to_string_dec ());
      tree_a.put ("online_weight_quorom", std::to_string (online_weight_quorum));
      tree_a.erase ("inactive_supply");
      tree_a.erase ("version");
      tree_a.put ("version", "11");
      result = true;
    case 11:
    {
      auto online_weight_quorum_l (tree_a.get<std::string> ("online_weight_quorom"));
      tree_a.erase ("online_weight_quorom");
      tree_a.put ("online_weight_quorum", online_weight_quorum_l);
      tree_a.erase ("version");
      tree_a.put ("version", "12");
      result = true;
    }
    case 12:
      tree_a.erase ("state_block_parse_canary");
      tree_a.erase ("state_block_generate_canary");
      tree_a.erase ("version");
      tree_a.put ("version", "13");
      result = true;
    case 13:
      tree_a.put ("generate_hash_votes_at", std::chrono::system_clock::to_time_t (generate_hash_votes_at));
      tree_a.erase ("version");
      tree_a.put ("version", "14");
      result = true;
    case 14:
      break;
    default:
      throw std::runtime_error ("Unknown node_config version");
  }
  return result;
}

bool chratos::node_config::deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
{
  auto result (false);
  try
  {
    auto version_l (tree_a.get_optional<std::string> ("version"));
    if (!version_l)
    {
      tree_a.put ("version", "1");
      version_l = "1";
      auto work_peers_l (tree_a.get_child_optional ("work_peers"));
      if (!work_peers_l)
      {
        tree_a.add_child ("work_peers", boost::property_tree::ptree ());
      }
      upgraded_a = true;
    }
    upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
    auto peering_port_l (tree_a.get<std::string> ("peering_port"));
    auto bootstrap_fraction_numerator_l (tree_a.get<std::string> ("bootstrap_fraction_numerator"));
    auto receive_minimum_l (tree_a.get<std::string> ("receive_minimum"));
    auto & logging_l (tree_a.get_child ("logging"));
    work_peers.clear ();
    auto work_peers_l (tree_a.get_child ("work_peers"));
    for (auto i (work_peers_l.begin ()), n (work_peers_l.end ()); i != n; ++i)
    {
      auto work_peer (i->second.get<std::string> (""));
      auto port_position (work_peer.rfind (':'));
      result |= port_position == -1;
      if (!result)
      {
        auto port_str (work_peer.substr (port_position + 1));
        uint16_t port;
        result |= parse_port (port_str, port);
        if (!result)
        {
          auto address (work_peer.substr (0, port_position));
          work_peers.push_back (std::make_pair (address, port));
        }
      }
    }
    auto preconfigured_peers_l (tree_a.get_child ("preconfigured_peers"));
    preconfigured_peers.clear ();
    for (auto i (preconfigured_peers_l.begin ()), n (preconfigured_peers_l.end ()); i != n; ++i)
    {
      auto bootstrap_peer (i->second.get<std::string> (""));
      preconfigured_peers.push_back (bootstrap_peer);
    }
    auto preconfigured_representatives_l (tree_a.get_child ("preconfigured_representatives"));
    preconfigured_representatives.clear ();
    for (auto i (preconfigured_representatives_l.begin ()), n (preconfigured_representatives_l.end ()); i != n; ++i)
    {
      chratos::account representative (0);
      result = result || representative.decode_account (i->second.get<std::string> (""));
      preconfigured_representatives.push_back (representative);
    }
    if (preconfigured_representatives.empty ())
    {
      result = true;
    }
    auto stat_config_l (tree_a.get_child_optional ("statistics"));
    if (stat_config_l)
    {
      result |= stat_config.deserialize_json (stat_config_l.get ());
    }
    auto online_weight_minimum_l (tree_a.get<std::string> ("online_weight_minimum"));
    auto online_weight_quorum_l (tree_a.get<std::string> ("online_weight_quorum"));
    auto password_fanout_l (tree_a.get<std::string> ("password_fanout"));
    auto io_threads_l (tree_a.get<std::string> ("io_threads"));
    auto work_threads_l (tree_a.get<std::string> ("work_threads"));
    enable_voting = tree_a.get<bool> ("enable_voting");
    auto bootstrap_connections_l (tree_a.get<std::string> ("bootstrap_connections"));
    auto bootstrap_connections_max_l (tree_a.get<std::string> ("bootstrap_connections_max"));
    callback_address = tree_a.get<std::string> ("callback_address");
    auto callback_port_l (tree_a.get<std::string> ("callback_port"));
    callback_target = tree_a.get<std::string> ("callback_target");
    auto lmdb_max_dbs_l = tree_a.get<std::string> ("lmdb_max_dbs");
    result |= parse_port (callback_port_l, callback_port);
    auto generate_hash_votes_at_l = tree_a.get<time_t> ("generate_hash_votes_at");
    generate_hash_votes_at = std::chrono::system_clock::from_time_t (generate_hash_votes_at_l);
    try
    {
      peering_port = std::stoul (peering_port_l);
      bootstrap_fraction_numerator = std::stoul (bootstrap_fraction_numerator_l);
      password_fanout = std::stoul (password_fanout_l);
      io_threads = std::stoul (io_threads_l);
      work_threads = std::stoul (work_threads_l);
      bootstrap_connections = std::stoul (bootstrap_connections_l);
      bootstrap_connections_max = std::stoul (bootstrap_connections_max_l);
      lmdb_max_dbs = std::stoi (lmdb_max_dbs_l);
      online_weight_quorum = std::stoul (online_weight_quorum_l);
      result |= peering_port > std::numeric_limits<uint16_t>::max ();
      result |= logging.deserialize_json (upgraded_a, logging_l);
      result |= receive_minimum.decode_dec (receive_minimum_l);
      result |= online_weight_minimum.decode_dec (online_weight_minimum_l);
      result |= online_weight_quorum > 100;
      result |= password_fanout < 16;
      result |= password_fanout > 1024 * 1024;
      result |= io_threads == 0;
    }
    catch (std::logic_error const &)
    {
      result = true;
    }
  }
  catch (std::runtime_error const &)
  {
    result = true;
  }
  return result;
}

chratos::account chratos::node_config::random_representative ()
{
  assert (preconfigured_representatives.size () > 0);
  size_t index (chratos::random_pool.GenerateWord32 (0, preconfigured_representatives.size () - 1));
  auto result (preconfigured_representatives[index]);
  return result;
}

chratos::vote_processor::vote_processor (chratos::node & node_a) :
node (node_a),
started (false),
stopped (false),
active (false),
thread ([this]() { process_loop (); })
{
  std::unique_lock<std::mutex> lock (mutex);
  while (!started)
  {
    condition.wait (lock);
  }
}

void chratos::vote_processor::process_loop ()
{
  std::unique_lock<std::mutex> lock (mutex);
  started = true;
  condition.notify_all ();
  while (!stopped)
  {
    if (!votes.empty ())
    {
      std::deque<std::pair<std::shared_ptr<chratos::vote>, chratos::endpoint>> votes_l;
      votes_l.swap (votes);
      active = true;
      lock.unlock ();
      {
        chratos::transaction transaction (node.store.environment, nullptr, false);
        for (auto & i : votes_l)
        {
          vote_blocking (transaction, i.first, i.second);
        }
      }
      lock.lock ();
      active = false;
      condition.notify_all ();
    }
    else
    {
      condition.wait (lock);
    }
  }
}

void chratos::vote_processor::vote (std::shared_ptr<chratos::vote> vote_a, chratos::endpoint endpoint_a)
{
  assert (endpoint_a.address ().is_v6 ());
  std::lock_guard<std::mutex> lock (mutex);
  if (!stopped)
  {
    votes.push_back (std::make_pair (vote_a, endpoint_a));
    condition.notify_all ();
  }
}

chratos::vote_code chratos::vote_processor::vote_blocking (MDB_txn * transaction_a, std::shared_ptr<chratos::vote> vote_a, chratos::endpoint endpoint_a)
{
  assert (endpoint_a.address ().is_v6 ());
  auto result (chratos::vote_code::invalid);
  if (!vote_a->validate ())
  {
    result = chratos::vote_code::replay;
    auto max_vote (node.store.vote_max (transaction_a, vote_a));
    if (!node.active.vote (vote_a) || max_vote->sequence > vote_a->sequence)
    {
      result = chratos::vote_code::vote;
    }
    switch (result)
    {
      case chratos::vote_code::vote:
        node.observers.vote.notify (vote_a, endpoint_a);
      case chratos::vote_code::replay:
        // This tries to assist rep nodes that have lost track of their highest sequence number by replaying our highest known vote back to them
        // Only do this if the sequence number is significantly different to account for network reordering
        // Amplify attack considerations: We're sending out a confirm_ack in response to a confirm_ack for no net traffic increase
        if (max_vote->sequence > vote_a->sequence + 10000)
        {
          chratos::confirm_ack confirm (max_vote);
          std::shared_ptr<std::vector<uint8_t>> bytes (new std::vector<uint8_t>);
          {
            chratos::vectorstream stream (*bytes);
            confirm.serialize (stream);
          }
          node.network.confirm_send (confirm, bytes, endpoint_a);
        }
      case chratos::vote_code::invalid:
        break;
    }
  }
  if (node.config.logging.vote_logging ())
  {
    char const * status;
    switch (result)
    {
      case chratos::vote_code::invalid:
        status = "Invalid";
        node.stats.inc (chratos::stat::type::vote, chratos::stat::detail::vote_invalid);
        break;
      case chratos::vote_code::replay:
        status = "Replay";
        node.stats.inc (chratos::stat::type::vote, chratos::stat::detail::vote_replay);
        break;
      case chratos::vote_code::vote:
        status = "Vote";
        node.stats.inc (chratos::stat::type::vote, chratos::stat::detail::vote_valid);
        break;
    }
    BOOST_LOG (node.log) << boost::str (boost::format ("Vote from: %1% sequence: %2% block(s): %3%status: %4%") % vote_a->account.to_account () % std::to_string (vote_a->sequence) % vote_a->hashes_string () % status);
  }
  return result;
}

void chratos::vote_processor::stop ()
{
  {
    std::lock_guard<std::mutex> lock (mutex);
    stopped = true;
    condition.notify_all ();
  }
  if (thread.joinable ())
  {
    thread.join ();
  }
}

void chratos::vote_processor::flush ()
{
  std::unique_lock<std::mutex> lock (mutex);
  while (active || !votes.empty ())
  {
    condition.wait (lock);
  }
}

void chratos::rep_crawler::add (chratos::block_hash const & hash_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  active.insert (hash_a);
}

void chratos::rep_crawler::remove (chratos::block_hash const & hash_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  active.erase (hash_a);
}

bool chratos::rep_crawler::exists (chratos::block_hash const & hash_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  return active.count (hash_a) != 0;
}

chratos::block_processor::block_processor (chratos::node & node_a) :
stopped (false),
active (false),
node (node_a),
next_log (std::chrono::steady_clock::now ())
{
}

chratos::block_processor::~block_processor ()
{
  stop ();
}

void chratos::block_processor::stop ()
{
  std::lock_guard<std::mutex> lock (mutex);
  stopped = true;
  condition.notify_all ();
}

void chratos::block_processor::flush ()
{
  std::unique_lock<std::mutex> lock (mutex);
  while (!stopped && (!blocks.empty () || active))
  {
    condition.wait (lock);
  }
}

bool chratos::block_processor::full ()
{
  std::unique_lock<std::mutex> lock (mutex);
  return blocks.size () > 16384;
}

void chratos::block_processor::add (std::shared_ptr<chratos::block> block_a, std::chrono::steady_clock::time_point origination)
{
  if (!chratos::work_validate (block_a->root (), block_a->block_work ()))
  {
    std::lock_guard<std::mutex> lock (mutex);
    if (blocks_hashes.find (block_a->hash ()) == blocks_hashes.end ())
    {
      blocks.push_back (std::make_pair (block_a, origination));
      blocks_hashes.insert (block_a->hash ());
      condition.notify_all ();
    }
  }
  else
  {
    BOOST_LOG (node.log) << "chratos::block_processor::add called for hash " << block_a->hash ().to_string () << " with invalid work " << chratos::to_string_hex (block_a->block_work ());
    assert (false && "chratos::block_processor::add called with invalid work");
  }
}

void chratos::block_processor::force (std::shared_ptr<chratos::block> block_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  forced.push_back (block_a);
  condition.notify_all ();
}

void chratos::block_processor::process_blocks ()
{
  std::unique_lock<std::mutex> lock (mutex);
  while (!stopped)
  {
    if (have_blocks ())
    {
      active = true;
      lock.unlock ();
      process_receive_many (lock);
      lock.lock ();
      active = false;
    }
    else
    {
      condition.notify_all ();
      condition.wait (lock);
    }
  }
}

bool chratos::block_processor::should_log ()
{
  auto result (false);
  auto now (std::chrono::steady_clock::now ());
  if (next_log < now)
  {
    next_log = now + std::chrono::seconds (15);
    result = true;
  }
  return result;
}

bool chratos::block_processor::have_blocks ()
{
  assert (!mutex.try_lock ());
  return !blocks.empty () || !forced.empty ();
}

void chratos::block_processor::process_receive_many (std::unique_lock<std::mutex> & lock_a)
{
  {
    chratos::transaction transaction (node.store.environment, nullptr, true);
    auto cutoff (std::chrono::steady_clock::now () + chratos::transaction_timeout);
    lock_a.lock ();
    auto count (0);
    while (have_blocks () && count < 16384)
    {
      if (blocks.size () > 64 && should_log ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("%1% blocks in processing queue") % blocks.size ());
      }
      std::pair<std::shared_ptr<chratos::block>, std::chrono::steady_clock::time_point> block;
      bool force (false);
      if (forced.empty ())
      {
        block = blocks.front ();
        blocks.pop_front ();
        blocks_hashes.erase (block.first->hash ());
      }
      else
      {
        block = std::make_pair (forced.front (), std::chrono::steady_clock::now ());
        forced.pop_front ();
        force = true;
      }
      lock_a.unlock ();
      auto hash (block.first->hash ());
      if (force)
      {
        auto successor (node.ledger.successor (transaction, block.first->root ()));
        if (successor != nullptr && successor->hash () != hash)
        {
          // Replace our block with the winner and roll back any dependent blocks
          BOOST_LOG (node.log) << boost::str (boost::format ("Rolling back %1% and replacing with %2%") % successor->hash ().to_string () % hash.to_string ());
          node.ledger.rollback (transaction, successor->hash ());
        }
      }
      auto process_result (process_receive_one (transaction, block.first, block.second));
      (void)process_result;
      lock_a.lock ();
      ++count;
    }
  }
  lock_a.unlock ();
}

chratos::process_return chratos::block_processor::process_receive_one (MDB_txn * transaction_a, std::shared_ptr<chratos::block> block_a, std::chrono::steady_clock::time_point origination)
{
  chratos::process_return result;
  auto hash (block_a->hash ());
  result = node.ledger.process (transaction_a, *block_a);
  switch (result.code)
  {
    case chratos::process_result::progress:
    {
      if (node.config.logging.ledger_logging ())
      {
        std::string block;
        block_a->serialize_json (block);
        BOOST_LOG (node.log) << boost::str (boost::format ("Processing block %1%: %2%") % hash.to_string () % block);
      }
      if (node.block_arrival.recent (hash))
      {
        node.active.start (block_a);
      }
      queue_unchecked (transaction_a, hash);
      break;
    }
    case chratos::process_result::gap_previous:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Gap previous for: %1%") % hash.to_string ());
      }
      node.store.unchecked_put (transaction_a, block_a->previous (), block_a);
      node.gap_cache.add (transaction_a, block_a);
      break;
    }
    case chratos::process_result::gap_source:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Gap source for: %1%") % hash.to_string ());
      }
      node.store.unchecked_put (transaction_a, node.ledger.block_source (transaction_a, *block_a), block_a);
      node.gap_cache.add (transaction_a, block_a);
      break;
    }
    case chratos::process_result::old:
    {
      if (node.config.logging.ledger_duplicate_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Old for: %1%") % block_a->hash ().to_string ());
      }
      queue_unchecked (transaction_a, hash);
      break;
    }
    case chratos::process_result::bad_signature:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Bad signature for: %1%") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::negative_spend:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Negative spend for: %1%") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::unreceivable:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Unreceivable for: %1%") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::fork:
    {
      if (origination < std::chrono::steady_clock::now () - std::chrono::seconds (15))
      {
        // Only let the bootstrap attempt know about forked blocks that not originate recently.
        node.process_fork (transaction_a, block_a);
      }
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Fork for: %1% root: %2%") % hash.to_string () % block_a->root ().to_string ());
      }
      break;
    }
    case chratos::process_result::opened_burn_account:
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("*** Rejecting open block for burn account ***: %1%") % hash.to_string ());
      break;
    }
    case chratos::process_result::balance_mismatch:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Balance mismatch for: %1%") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::representative_mismatch:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Representative mismatch for: %1%") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::block_position:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% cannot follow predecessor %2%") % hash.to_string () % block_a->previous ().to_string ());
      }
      break;
    }
    case chratos::process_result::outstanding_pendings:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Dividend %1% has outstanding pendings") % block_a->dividend ().to_string ());
      }
      break;
    }
    case chratos::process_result::dividend_too_small:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Dividend %1% is too small to be accepted") % hash.to_string ());
      }
      break;
    }
    case chratos::process_result::incorrect_dividend:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% cannot be sent without the account claiming for the dividend first") % hash.to_string ());
      }
      node.store.unchecked_put (transaction_a, block_a->dividend (), block_a);
      break;
    }
    case chratos::process_result::dividend_fork:
    {
      if (origination < std::chrono::steady_clock::now () - std::chrono::seconds (15))
      {
        // Only let the bootstrap attempt know about forked blocks that not originate recently.
        node.process_dividend_fork (transaction_a, block_a);
      }
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Fork for: %1% root: %2%") % hash.to_string () % block_a->root ().to_string ());
      }
      break;
    }
    case chratos::process_result::invalid_dividend_account:
    {
      if (node.config.logging.ledger_logging ())
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("Account %1% cannot create a dividend") % block_a->source ().to_account ());
      }
      break;
    }
  }
  return result;
}

void chratos::block_processor::queue_unchecked (MDB_txn * transaction_a, chratos::block_hash const & hash_a)
{
  auto cached (node.store.unchecked_get (transaction_a, hash_a));
  for (auto i (cached.begin ()), n (cached.end ()); i != n; ++i)
  {
    node.store.unchecked_del (transaction_a, hash_a, *i);
    add (*i, std::chrono::steady_clock::time_point ());
  }
  std::lock_guard<std::mutex> lock (node.gap_cache.mutex);
  node.gap_cache.blocks.get<1> ().erase (hash_a);
}

chratos::node::node (chratos::node_init & init_a, boost::asio::io_service & service_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, chratos::alarm & alarm_a, chratos::logging const & logging_a, chratos::work_pool & work_a) :
node (init_a, service_a, application_path_a, alarm_a, chratos::node_config (peering_port_a, logging_a), work_a)
{
}

chratos::node::node (chratos::node_init & init_a, boost::asio::io_service & service_a, boost::filesystem::path const & application_path_a, chratos::alarm & alarm_a, chratos::node_config const & config_a, chratos::work_pool & work_a) :
service (service_a),
config (config_a),
alarm (alarm_a),
work (work_a),
store (init_a.block_store_init, application_path_a / "data.ldb", config_a.lmdb_max_dbs),
gap_cache (*this),
ledger (store, stats, config.epoch_block_link, config.epoch_block_signer),
active (*this),
network (*this, config.peering_port),
bootstrap_initiator (*this),
bootstrap (service_a, config.peering_port, *this),
peers (network.endpoint ()),
application_path (application_path_a),
wallets (init_a.block_store_init, *this),
port_mapping (*this),
vote_processor (*this),
warmed_up (0),
block_processor (*this),
block_processor_thread ([this]() { this->block_processor.process_blocks (); }),
online_reps (*this),
stats (config.stat_config)
{
  wallets.observer = [this](bool active) {
    observers.wallet.notify (active);
  };
  peers.peer_observer = [this](chratos::endpoint const & endpoint_a) {
    observers.endpoint.notify (endpoint_a);
  };
  peers.disconnect_observer = [this]() {
    observers.disconnect.notify ();
  };
  observers.blocks.add ([this](std::shared_ptr<chratos::block> block_a, chratos::account const & account_a, chratos::amount const & amount_a, bool is_state_send_a) {
    if (this->block_arrival.recent (block_a->hash ()))
    {
      auto node_l (shared_from_this ());
      background ([node_l, block_a, account_a, amount_a, is_state_send_a]() {
        if (!node_l->config.callback_address.empty ())
        {
          boost::property_tree::ptree event;
          event.add ("account", account_a.to_account ());
          event.add ("hash", block_a->hash ().to_string ());
          std::string block_text;
          block_a->serialize_json (block_text);
          event.add ("block", block_text);
          event.add ("amount", amount_a.to_string_dec ());
          if (is_state_send_a)
          {
            event.add ("is_send", is_state_send_a);
          }
          std::stringstream ostream;
          boost::property_tree::write_json (ostream, event);
          ostream.flush ();
          auto body (std::make_shared<std::string> (ostream.str ()));
          auto address (node_l->config.callback_address);
          auto port (node_l->config.callback_port);
          auto target (std::make_shared<std::string> (node_l->config.callback_target));
          auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_l->service));
          resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [node_l, address, port, target, body, resolver](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
            if (!ec)
            {
              for (auto i (i_a), n (boost::asio::ip::tcp::resolver::iterator{}); i != n; ++i)
              {
                auto sock (std::make_shared<boost::asio::ip::tcp::socket> (node_l->service));
                sock->async_connect (i->endpoint (), [node_l, target, body, sock, address, port](boost::system::error_code const & ec) {
                  if (!ec)
                  {
                    auto req (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
                    req->method (boost::beast::http::verb::post);
                    req->target (*target);
                    req->version (11);
                    req->insert (boost::beast::http::field::host, address);
                    req->insert (boost::beast::http::field::content_type, "application/json");
                    req->body () = *body;
                    //req->prepare (*req);
                    //boost::beast::http::prepare(req);
                    req->prepare_payload ();
                    boost::beast::http::async_write (*sock, *req, [node_l, sock, address, port, req](boost::system::error_code const & ec, size_t bytes_transferred) {
                      if (!ec)
                      {
                        auto sb (std::make_shared<boost::beast::flat_buffer> ());
                        auto resp (std::make_shared<boost::beast::http::response<boost::beast::http::string_body>> ());
                        boost::beast::http::async_read (*sock, *sb, *resp, [node_l, sb, resp, sock, address, port](boost::system::error_code const & ec, size_t bytes_transferred) {
                          if (!ec)
                          {
                            if (resp->result () == boost::beast::http::status::ok)
                            {
                            }
                            else
                            {
                              if (node_l->config.logging.callback_logging ())
                              {
                                BOOST_LOG (node_l->log) << boost::str (boost::format ("Callback to %1%:%2% failed with status: %3%") % address % port % resp->result ());
                              }
                            }
                          }
                          else
                          {
                            if (node_l->config.logging.callback_logging ())
                            {
                              BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable complete callback: %1%:%2%: %3%") % address % port % ec.message ());
                            }
                          };
                        });
                      }
                      else
                      {
                        if (node_l->config.logging.callback_logging ())
                        {
                          BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable to send callback: %1%:%2%: %3%") % address % port % ec.message ());
                        }
                      }
                    });
                  }
                  else
                  {
                    if (node_l->config.logging.callback_logging ())
                    {
                      BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable to connect to callback address: %1%:%2%: %3%") % address % port % ec.message ());
                    }
                  }
                });
              }
            }
            else
            {
              if (node_l->config.logging.callback_logging ())
              {
                BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving callback: %1%:%2%: %3%") % address % port % ec.message ());
              }
            }
          });
        }
      });
    }
  });
  observers.endpoint.add ([this](chratos::endpoint const & endpoint_a) {
    this->network.send_keepalive (endpoint_a);
    rep_query (*this, endpoint_a);
  });
  observers.vote.add ([this](std::shared_ptr<chratos::vote> vote_a, chratos::endpoint const & endpoint_a) {
    assert (endpoint_a.address ().is_v6 ());
    this->gap_cache.vote (vote_a);
    this->online_reps.vote (vote_a);
    chratos::uint128_t rep_weight;
    chratos::uint128_t min_rep_weight;
    {
      chratos::transaction transaction (store.environment, nullptr, false);
      rep_weight = ledger.weight (transaction, vote_a->account);
      min_rep_weight = online_reps.online_stake () / 1000;
    }
    if (rep_weight > min_rep_weight)
    {
      bool rep_crawler_exists (false);
      for (auto hash : *vote_a)
      {
        if (this->rep_crawler.exists (hash))
        {
          rep_crawler_exists = true;
          break;
        }
      }
      if (rep_crawler_exists)
      {
        // We see a valid non-replay vote for a block we requested, this node is probably a representative
        if (this->peers.rep_response (endpoint_a, vote_a->account, rep_weight))
        {
          BOOST_LOG (log) << boost::str (boost::format ("Found a representative at %1%") % endpoint_a);
          // Rebroadcasting all active votes to new representative
          auto blocks (this->active.list_blocks ());
          for (auto i (blocks.begin ()), n (blocks.end ()); i != n; ++i)
          {
            if (*i != nullptr)
            {
              this->network.send_confirm_req (endpoint_a, *i);
            }
          }
        }
      }
    }
  });
  BOOST_LOG (log) << "Node starting, version: " << RAIBLOCKS_VERSION_MAJOR << "." << RAIBLOCKS_VERSION_MINOR;
  BOOST_LOG (log) << boost::str (boost::format ("Work pool running %1% threads") % work.threads.size ());
  if (!init_a.error ())
  {
    if (config.logging.node_lifetime_tracing ())
    {
      BOOST_LOG (log) << "Constructing node";
    }
    chratos::transaction transaction (store.environment, nullptr, true);
    if (store.latest_begin (transaction) == store.latest_end ())
    {
      // Store was empty meaning we just created it, add the genesis block
      chratos::genesis genesis;
      store.initialize (transaction, genesis);
    }
    node_id = chratos::keypair (store.get_node_id (transaction));
    BOOST_LOG (log) << "Node ID: " << node_id.pub.to_account ();
  }
  peers.online_weight_minimum = config.online_weight_minimum.number ();
  if (chratos::chratos_network == chratos::chratos_networks::chratos_live_network)
  {
    extern const char chratos_bootstrap_weights[];
    extern const size_t chratos_bootstrap_weights_size;
    chratos::bufferstream weight_stream ((const uint8_t *)chratos_bootstrap_weights, chratos_bootstrap_weights_size);
    chratos::uint128_union block_height;
    if (!chratos::read (weight_stream, block_height))
    {
      auto max_blocks = (uint64_t)block_height.number ();
      chratos::transaction transaction (store.environment, nullptr, false);
      if (ledger.store.block_count (transaction).sum () < max_blocks)
      {
        ledger.bootstrap_weight_max_blocks = max_blocks;
        while (true)
        {
          chratos::account account;
          if (chratos::read (weight_stream, account.bytes))
          {
            break;
          }
          chratos::amount weight;
          if (chratos::read (weight_stream, weight.bytes))
          {
            break;
          }
          BOOST_LOG (log) << "Using bootstrap rep weight: " << account.to_account () << " -> " << weight.format_balance (Mchr_ratio, 0, true) << " CHR";
          ledger.bootstrap_weights[account] = weight.number ();
        }
      }
    }
  }
}

chratos::node::~node ()
{
  if (config.logging.node_lifetime_tracing ())
  {
    BOOST_LOG (log) << "Destructing node";
  }
  stop ();
}

bool chratos::node::copy_with_compaction (boost::filesystem::path const & destination_file)
{
  return !mdb_env_copy2 (store.environment.environment,
  destination_file.string ().c_str (), MDB_CP_COMPACT);
}

void chratos::node::send_keepalive (chratos::endpoint const & endpoint_a)
{
  network.send_keepalive (chratos::map_endpoint_to_v6 (endpoint_a));
}

void chratos::node::process_fork (MDB_txn * transaction_a, std::shared_ptr<chratos::block> block_a)
{
  auto root (block_a->root ());
  if (!store.block_exists (transaction_a, block_a->hash ()) && store.root_exists (transaction_a, block_a->root ()))
  {
    std::shared_ptr<chratos::block> ledger_block (ledger.forked_block (transaction_a, *block_a));
    if (ledger_block)
    {
      std::weak_ptr<chratos::node> this_w (shared_from_this ());
      if (!active.start (std::make_pair (ledger_block, block_a), [this_w, root](std::shared_ptr<chratos::block>) {
            if (auto this_l = this_w.lock ())
            {
              auto attempt (this_l->bootstrap_initiator.current_attempt ());
              if (attempt)
              {
                chratos::transaction transaction (this_l->store.environment, nullptr, false);
                auto account (this_l->ledger.store.frontier_get (transaction, root));
                if (!account.is_zero ())
                {
                  attempt->requeue_pull (chratos::pull_info (account, root, root));
                }
                else if (this_l->ledger.store.account_exists (transaction, root))
                {
                  attempt->requeue_pull (chratos::pull_info (root, chratos::block_hash (0), chratos::block_hash (0)));
                }
              }
            }
          }))
      {
        BOOST_LOG (log) << boost::str (boost::format ("Resolving fork between our block: %1% and block %2% both with root %3%") % ledger_block->hash ().to_string () % block_a->hash ().to_string () % block_a->root ().to_string ());
        network.broadcast_confirm_req (ledger_block);
      }
    }
  }
}

void chratos::node::process_dividend_fork (MDB_txn * transaction_a, std::shared_ptr<chratos::block> block_a)
{
  // TODO - Handle dividend forks explicitly
}

chratos::gap_cache::gap_cache (chratos::node & node_a) :
node (node_a)
{
}

void chratos::gap_cache::add (MDB_txn * transaction_a, std::shared_ptr<chratos::block> block_a)
{
  auto hash (block_a->hash ());
  std::lock_guard<std::mutex> lock (mutex);
  auto existing (blocks.get<1> ().find (hash));
  if (existing != blocks.get<1> ().end ())
  {
    blocks.get<1> ().modify (existing, [](chratos::gap_information & info) {
      info.arrival = std::chrono::steady_clock::now ();
    });
  }
  else
  {
    blocks.insert ({ std::chrono::steady_clock::now (), hash, std::unordered_set<chratos::account> () });
    if (blocks.size () > max)
    {
      blocks.get<0> ().erase (blocks.get<0> ().begin ());
    }
  }
}

void chratos::gap_cache::vote (std::shared_ptr<chratos::vote> vote_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  chratos::transaction transaction (node.store.environment, nullptr, false);
  for (auto hash : *vote_a)
  {
    auto existing (blocks.get<1> ().find (hash));
    if (existing != blocks.get<1> ().end ())
    {
      auto is_new (false);
      blocks.get<1> ().modify (existing, [&](chratos::gap_information & info) { is_new = info.voters.insert (vote_a->account).second; });
      if (is_new)
      {
        uint128_t tally;
        for (auto & voter : existing->voters)
        {
          tally += node.ledger.weight (transaction, voter);
        }
        if (tally > bootstrap_threshold (transaction))
        {
          auto node_l (node.shared ());
          auto now (std::chrono::steady_clock::now ());
          node.alarm.add (chratos::chratos_network == chratos::chratos_networks::chratos_test_network ? now + std::chrono::milliseconds (5) : now + std::chrono::seconds (5), [node_l, hash]() {
            chratos::transaction transaction (node_l->store.environment, nullptr, false);
            if (!node_l->store.block_exists (transaction, hash))
            {
              if (!node_l->bootstrap_initiator.in_progress ())
              {
                BOOST_LOG (node_l->log) << boost::str (boost::format ("Missing confirmed block %1%") % hash.to_string ());
              }
              node_l->bootstrap_initiator.bootstrap ();
            }
          });
        }
      }
    }
  }
}

chratos::uint128_t chratos::gap_cache::bootstrap_threshold (MDB_txn * transaction_a)
{
  auto result ((node.online_reps.online_stake () / 256) * node.config.bootstrap_fraction_numerator);
  return result;
}

void chratos::gap_cache::purge_old ()
{
  auto cutoff (std::chrono::steady_clock::now () - std::chrono::seconds (10));
  std::lock_guard<std::mutex> lock (mutex);
  auto done (false);
  while (!done && !blocks.empty ())
  {
    auto first (blocks.get<1> ().begin ());
    if (first->arrival < cutoff)
    {
      blocks.get<1> ().erase (first);
    }
    else
    {
      done = true;
    }
  }
}

void chratos::network::confirm_send (chratos::confirm_ack const & confirm_a, std::shared_ptr<std::vector<uint8_t>> bytes_a, chratos::endpoint const & endpoint_a)
{
  if (node.config.logging.network_publish_logging ())
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm_ack for block(s) %1%to %2% sequence %3%") % confirm_a.vote->hashes_string () % endpoint_a % std::to_string (confirm_a.vote->sequence));
  }
  std::weak_ptr<chratos::node> node_w (node.shared ());
  node.network.send_buffer (bytes_a->data (), bytes_a->size (), endpoint_a, [bytes_a, node_w, endpoint_a](boost::system::error_code const & ec, size_t size_a) {
    if (auto node_l = node_w.lock ())
    {
      if (ec && node_l->config.logging.network_logging ())
      {
        BOOST_LOG (node_l->log) << boost::str (boost::format ("Error broadcasting confirm_ack to %1%: %2%") % endpoint_a % ec.message ());
      }
      else
      {
        node_l->stats.inc (chratos::stat::type::message, chratos::stat::detail::confirm_ack, chratos::stat::dir::out);
      }
    }
  });
}

void chratos::node::process_active (std::shared_ptr<chratos::block> incoming)
{
  if (!block_arrival.add (incoming->hash ()))
  {
    block_processor.add (incoming, std::chrono::steady_clock::now ());
  }
}

chratos::process_return chratos::node::process (chratos::block const & block_a)
{
  chratos::transaction transaction (store.environment, nullptr, true);
  auto result (ledger.process (transaction, block_a));
  return result;
}

// Simulating with sqrt_broadcast_simulate shows we only need to broadcast to sqrt(total_peers) random peers in order to successfully publish to everyone with high probability
std::deque<chratos::endpoint> chratos::peer_container::list_fanout ()
{
  auto peers (random_set (size_sqrt ()));
  std::deque<chratos::endpoint> result;
  for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
  {
    result.push_back (*i);
  }
  return result;
}

std::deque<chratos::endpoint> chratos::peer_container::list ()
{
  std::deque<chratos::endpoint> result;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
  {
    result.push_back (i->endpoint);
  }
  std::random_shuffle (result.begin (), result.end ());
  return result;
}

std::map<chratos::endpoint, unsigned> chratos::peer_container::list_version ()
{
  std::map<chratos::endpoint, unsigned> result;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
  {
    result.insert (std::pair<chratos::endpoint, unsigned> (i->endpoint, i->network_version));
  }
  return result;
}

std::vector<chratos::peer_information> chratos::peer_container::list_vector ()
{
  std::vector<peer_information> result;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
  {
    result.push_back (*i);
  }
  std::random_shuffle (result.begin (), result.end ());
  return result;
}

chratos::endpoint chratos::peer_container::bootstrap_peer ()
{
  chratos::endpoint result (boost::asio::ip::address_v6::any (), 0);
  std::lock_guard<std::mutex> lock (mutex);
  ;
  for (auto i (peers.get<4> ().begin ()), n (peers.get<4> ().end ()); i != n;)
  {
    if (i->network_version >= 0x5)
    {
      result = i->endpoint;
      peers.get<4> ().modify (i, [](chratos::peer_information & peer_a) {
        peer_a.last_bootstrap_attempt = std::chrono::steady_clock::now ();
      });
      i = n;
    }
    else
    {
      ++i;
    }
  }
  return result;
}

boost::optional<chratos::uint256_union> chratos::peer_container::assign_syn_cookie (chratos::endpoint const & endpoint)
{
  auto ip_addr (endpoint.address ());
  assert (ip_addr.is_v6 ());
  std::unique_lock<std::mutex> lock (syn_cookie_mutex);
  unsigned & ip_cookies = syn_cookies_per_ip[ip_addr];
  boost::optional<chratos::uint256_union> result;
  if (ip_cookies < max_peers_per_ip)
  {
    if (syn_cookies.find (endpoint) == syn_cookies.end ())
    {
      chratos::uint256_union query;
      random_pool.GenerateBlock (query.bytes.data (), query.bytes.size ());
      syn_cookie_info info{ query, std::chrono::steady_clock::now () };
      syn_cookies[endpoint] = info;
      ++ip_cookies;
      result = query;
    }
  }
  return result;
}

bool chratos::peer_container::validate_syn_cookie (chratos::endpoint const & endpoint, chratos::account node_id, chratos::signature sig)
{
  auto ip_addr (endpoint.address ());
  assert (ip_addr.is_v6 ());
  std::unique_lock<std::mutex> lock (syn_cookie_mutex);
  auto result (true);
  auto cookie_it (syn_cookies.find (endpoint));
  if (cookie_it != syn_cookies.end () && !chratos::validate_message (node_id, cookie_it->second.cookie, sig))
  {
    result = false;
    syn_cookies.erase (cookie_it);
    unsigned & ip_cookies = syn_cookies_per_ip[ip_addr];
    if (ip_cookies > 0)
    {
      --ip_cookies;
    }
    else
    {
      assert (false && "More SYN cookies deleted than created for IP");
    }
  }
  return result;
}

bool chratos::parse_port (std::string const & string_a, uint16_t & port_a)
{
  bool result;
  size_t converted;
  try
  {
    port_a = std::stoul (string_a, &converted);
    result = converted != string_a.size () || converted > std::numeric_limits<uint16_t>::max ();
  }
  catch (...)
  {
    result = true;
  }
  return result;
}

bool chratos::parse_address_port (std::string const & string, boost::asio::ip::address & address_a, uint16_t & port_a)
{
  auto result (false);
  auto port_position (string.rfind (':'));
  if (port_position != std::string::npos && port_position > 0)
  {
    std::string port_string (string.substr (port_position + 1));
    try
    {
      uint16_t port;
      result = parse_port (port_string, port);
      if (!result)
      {
        boost::system::error_code ec;
        auto address (boost::asio::ip::address_v6::from_string (string.substr (0, port_position), ec));
        if (!ec)
        {
          address_a = address;
          port_a = port;
        }
        else
        {
          result = true;
        }
      }
      else
      {
        result = true;
      }
    }
    catch (...)
    {
      result = true;
    }
  }
  else
  {
    result = true;
  }
  return result;
}

bool chratos::parse_endpoint (std::string const & string, chratos::endpoint & endpoint_a)
{
  boost::asio::ip::address address;
  uint16_t port;
  auto result (parse_address_port (string, address, port));
  if (!result)
  {
    endpoint_a = chratos::endpoint (address, port);
  }
  return result;
}

bool chratos::parse_tcp_endpoint (std::string const & string, chratos::tcp_endpoint & endpoint_a)
{
  boost::asio::ip::address address;
  uint16_t port;
  auto result (parse_address_port (string, address, port));
  if (!result)
  {
    endpoint_a = chratos::tcp_endpoint (address, port);
  }
  return result;
}

void chratos::node::start ()
{
  network.receive ();
  ongoing_keepalive ();
  ongoing_syn_cookie_cleanup ();
  ongoing_bootstrap ();
  ongoing_store_flush ();
  ongoing_rep_crawl ();
  bootstrap.start ();
  backup_wallet ();
  online_reps.recalculate_stake ();
  port_mapping.start ();
  add_initial_peers ();
  observers.started.notify ();
}

void chratos::node::stop ()
{
  BOOST_LOG (log) << "Node stopping";
  block_processor.stop ();
  if (block_processor_thread.joinable ())
  {
    block_processor_thread.join ();
  }
  active.stop ();
  network.stop ();
  bootstrap_initiator.stop ();
  bootstrap.stop ();
  port_mapping.stop ();
  vote_processor.stop ();
  wallets.stop ();
}

void chratos::node::keepalive_preconfigured (std::vector<std::string> const & peers_a)
{
  for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
  {
    keepalive (*i, chratos::network::node_port);
  }
}

chratos::block_hash chratos::node::latest (chratos::account const & account_a)
{
  chratos::transaction transaction (store.environment, nullptr, false);
  return ledger.latest (transaction, account_a);
}

chratos::uint128_t chratos::node::balance (chratos::account const & account_a)
{
  chratos::transaction transaction (store.environment, nullptr, false);
  return ledger.account_balance (transaction, account_a);
}

std::unique_ptr<chratos::block> chratos::node::block (chratos::block_hash const & hash_a)
{
  chratos::transaction transaction (store.environment, nullptr, false);
  return store.block_get (transaction, hash_a);
}

std::pair<chratos::uint128_t, chratos::uint128_t> chratos::node::balance_pending (chratos::account const & account_a)
{
  std::pair<chratos::uint128_t, chratos::uint128_t> result;
  chratos::transaction transaction (store.environment, nullptr, false);
  result.first = ledger.account_balance (transaction, account_a);
  result.second = ledger.account_pending (transaction, account_a);
  return result;
}

chratos::uint128_t chratos::node::weight (chratos::account const & account_a)
{
  chratos::transaction transaction (store.environment, nullptr, false);
  return ledger.weight (transaction, account_a);
}

chratos::account chratos::node::representative (chratos::account const & account_a)
{
  chratos::transaction transaction (store.environment, nullptr, false);
  chratos::account_info info;
  chratos::account result (0);
  if (!store.account_get (transaction, account_a, info))
  {
    result = info.rep_block;
  }
  return result;
}

void chratos::node::ongoing_keepalive ()
{
  keepalive_preconfigured (config.preconfigured_peers);
  auto peers_l (peers.purge_list (std::chrono::steady_clock::now () - cutoff));
  for (auto i (peers_l.begin ()), j (peers_l.end ()); i != j && std::chrono::steady_clock::now () - i->last_attempt > period; ++i)
  {
    network.send_keepalive (i->endpoint);
  }
  std::weak_ptr<chratos::node> node_w (shared_from_this ());
  alarm.add (std::chrono::steady_clock::now () + period, [node_w]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->ongoing_keepalive ();
    }
  });
}

void chratos::node::ongoing_syn_cookie_cleanup ()
{
  peers.purge_syn_cookies (std::chrono::steady_clock::now () - syn_cookie_cutoff);
  std::weak_ptr<chratos::node> node_w (shared_from_this ());
  alarm.add (std::chrono::steady_clock::now () + (syn_cookie_cutoff * 2), [node_w]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->ongoing_syn_cookie_cleanup ();
    }
  });
}

void chratos::node::ongoing_rep_crawl ()
{
  auto now (std::chrono::steady_clock::now ());
  auto peers_l (peers.rep_crawl ());
  rep_query (*this, peers_l);
  if (network.on)
  {
    std::weak_ptr<chratos::node> node_w (shared_from_this ());
    alarm.add (now + std::chrono::seconds (4), [node_w]() {
      if (auto node_l = node_w.lock ())
      {
        node_l->ongoing_rep_crawl ();
      }
    });
  }
}

void chratos::node::ongoing_bootstrap ()
{
  auto next_wakeup (300);
  if (warmed_up < 3)
  {
    // Re-attempt bootstrapping more aggressively on startup
    next_wakeup = 5;
    if (!bootstrap_initiator.in_progress () && !peers.empty ())
    {
      ++warmed_up;
    }
  }
  bootstrap_initiator.bootstrap ();
  std::weak_ptr<chratos::node> node_w (shared_from_this ());
  alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (next_wakeup), [node_w]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->ongoing_bootstrap ();
    }
  });
}

void chratos::node::ongoing_store_flush ()
{
  {
    chratos::transaction transaction (store.environment, nullptr, true);
    store.flush (transaction);
  }
  std::weak_ptr<chratos::node> node_w (shared_from_this ());
  alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->ongoing_store_flush ();
    }
  });
}

void chratos::node::backup_wallet ()
{
  chratos::transaction transaction (store.environment, nullptr, false);
  for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
  {
    auto backup_path (application_path / "backup");
    boost::filesystem::create_directories (backup_path);
    i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
  }
  auto this_l (shared ());
  alarm.add (std::chrono::steady_clock::now () + backup_interval, [this_l]() {
    this_l->backup_wallet ();
  });
}

int chratos::node::price (chratos::uint128_t const & balance_a, int amount_a)
{
  assert (balance_a >= amount_a * chratos::Gchr_ratio);
  auto balance_l (balance_a);
  double result (0.0);
  for (auto i (0); i < amount_a; ++i)
  {
    balance_l -= chratos::Gchr_ratio;
    auto balance_scaled ((balance_l / chratos::Mchr_ratio).convert_to<double> ());
    auto units (balance_scaled / 1000.0);
    auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
    result += std::min (std::max (0.0, unit_price), price_max);
  }
  return static_cast<int> (result * 100.0);
}

namespace
{
class work_request
{
public:
  work_request (boost::asio::io_service & service_a, boost::asio::ip::address address_a, uint16_t port_a) :
  address (address_a),
  port (port_a),
  socket (service_a)
  {
  }
  boost::asio::ip::address address;
  uint16_t port;
  boost::beast::flat_buffer buffer;
  boost::beast::http::response<boost::beast::http::string_body> response;
  boost::asio::ip::tcp::socket socket;
};
class distributed_work : public std::enable_shared_from_this<distributed_work>
{
public:
  distributed_work (std::shared_ptr<chratos::node> const & node_a, chratos::block_hash const & root_a, std::function<void(uint64_t)> callback_a, unsigned int backoff_a = 1) :
  callback (callback_a),
  node (node_a),
  root (root_a),
  backoff (backoff_a),
  need_resolve (node_a->config.work_peers)
  {
    completed.clear ();
  }
  void start ()
  {
    if (need_resolve.empty ())
    {
      start_work ();
    }
    else
    {
      auto current (need_resolve.back ());
      need_resolve.pop_back ();
      auto this_l (shared_from_this ());
      boost::system::error_code ec;
      auto parsed_address (boost::asio::ip::address_v6::from_string (current.first, ec));
      if (!ec)
      {
        outstanding[parsed_address] = current.second;
        start ();
      }
      else
      {
        node->network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (current.first, std::to_string (current.second)), [current, this_l](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
          if (!ec)
          {
            for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
            {
              auto endpoint (i->endpoint ());
              this_l->outstanding[endpoint.address ()] = endpoint.port ();
            }
          }
          else
          {
            BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Error resolving work peer: %1%:%2%: %3%") % current.first % current.second % ec.message ());
          }
          this_l->start ();
        });
      }
    }
  }
  void start_work ()
  {
    if (!outstanding.empty ())
    {
      auto this_l (shared_from_this ());
      std::lock_guard<std::mutex> lock (mutex);
      for (auto const & i : outstanding)
      {
        auto host (i.first);
        auto service (i.second);
        node->background ([this_l, host, service]() {
          auto connection (std::make_shared<work_request> (this_l->node->service, host, service));
          connection->socket.async_connect (chratos::tcp_endpoint (host, service), [this_l, connection](boost::system::error_code const & ec) {
            if (!ec)
            {
              std::string request_string;
              {
                boost::property_tree::ptree request;
                request.put ("action", "work_generate");
                request.put ("hash", this_l->root.to_string ());
                std::stringstream ostream;
                boost::property_tree::write_json (ostream, request);
                request_string = ostream.str ();
              }
              auto request (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
              request->method (boost::beast::http::verb::post);
              request->target ("/");
              request->version (11);
              request->body () = request_string;
              request->prepare_payload ();
              boost::beast::http::async_write (connection->socket, *request, [this_l, connection, request](boost::system::error_code const & ec, size_t bytes_transferred) {
                if (!ec)
                {
                  boost::beast::http::async_read (connection->socket, connection->buffer, connection->response, [this_l, connection](boost::system::error_code const & ec, size_t bytes_transferred) {
                    if (!ec)
                    {
                      if (connection->response.result () == boost::beast::http::status::ok)
                      {
                        this_l->success (connection->response.body (), connection->address);
                      }
                      else
                      {
                        BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Work peer responded with an error %1% %2%: %3%") % connection->address % connection->port % connection->response.result ());
                        this_l->failure (connection->address);
                      }
                    }
                    else
                    {
                      BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to read from work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ());
                      this_l->failure (connection->address);
                    }
                  });
                }
                else
                {
                  BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to write to work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ());
                  this_l->failure (connection->address);
                }
              });
            }
            else
            {
              BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to connect to work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ());
              this_l->failure (connection->address);
            }
          });
        });
      }
    }
    else
    {
      handle_failure (true);
    }
  }
  void stop ()
  {
    auto this_l (shared_from_this ());
    std::lock_guard<std::mutex> lock (mutex);
    for (auto const & i : outstanding)
    {
      auto host (i.first);
      auto service (i.second);
      node->background ([this_l, host, service]() {
        std::string request_string;
        {
          boost::property_tree::ptree request;
          request.put ("action", "work_cancel");
          request.put ("hash", this_l->root.to_string ());
          std::stringstream ostream;
          boost::property_tree::write_json (ostream, request);
          request_string = ostream.str ();
        }
        boost::beast::http::request<boost::beast::http::string_body> request;
        request.method (boost::beast::http::verb::post);
        request.target ("/");
        request.version (11);
        request.body () = request_string;
        request.prepare_payload ();
        auto socket (std::make_shared<boost::asio::ip::tcp::socket> (this_l->node->service));
        boost::beast::http::async_write (*socket, request, [socket](boost::system::error_code const & ec, size_t bytes_transferred) {
        });
      });
    }
    outstanding.clear ();
  }
  void success (std::string const & body_a, boost::asio::ip::address const & address)
  {
    auto last (remove (address));
    std::stringstream istream (body_a);
    try
    {
      boost::property_tree::ptree result;
      boost::property_tree::read_json (istream, result);
      auto work_text (result.get<std::string> ("work"));
      uint64_t work;
      if (!chratos::from_string_hex (work_text, work))
      {
        if (!chratos::work_validate (root, work))
        {
          set_once (work);
          stop ();
        }
        else
        {
          BOOST_LOG (node->log) << boost::str (boost::format ("Incorrect work response from %1% for root %2%: %3%") % address % root.to_string () % work_text);
          handle_failure (last);
        }
      }
      else
      {
        BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't a number: %2%") % address % work_text);
        handle_failure (last);
      }
    }
    catch (...)
    {
      BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't parsable: %2%") % address % body_a);
      handle_failure (last);
    }
  }
  void set_once (uint64_t work_a)
  {
    if (!completed.test_and_set ())
    {
      callback (work_a);
    }
  }
  void failure (boost::asio::ip::address const & address)
  {
    auto last (remove (address));
    handle_failure (last);
  }
  void handle_failure (bool last)
  {
    if (last)
    {
      if (!completed.test_and_set ())
      {
        if (node->config.work_threads != 0 || node->work.opencl)
        {
          auto callback_l (callback);
          node->work.generate (root, [callback_l](boost::optional<uint64_t> const & work_a) {
            callback_l (work_a.value ());
          });
        }
        else
        {
          if (backoff == 1 && node->config.logging.work_generation_time ())
          {
            BOOST_LOG (node->log) << "Work peer(s) failed to generate work for root " << root.to_string () << ", retrying...";
          }
          auto now (std::chrono::steady_clock::now ());
          auto root_l (root);
          auto callback_l (callback);
          std::weak_ptr<chratos::node> node_w (node);
          auto next_backoff (std::min (backoff * 2, (unsigned int)60 * 5));
          node->alarm.add (now + std::chrono::seconds (backoff), [node_w, root_l, callback_l, next_backoff] {
            if (auto node_l = node_w.lock ())
            {
              auto work_generation (std::make_shared<distributed_work> (node_l, root_l, callback_l, next_backoff));
              work_generation->start ();
            }
          });
        }
      }
    }
  }
  bool remove (boost::asio::ip::address const & address)
  {
    std::lock_guard<std::mutex> lock (mutex);
    outstanding.erase (address);
    return outstanding.empty ();
  }
  std::function<void(uint64_t)> callback;
  unsigned int backoff; // in seconds
  std::shared_ptr<chratos::node> node;
  chratos::block_hash root;
  std::mutex mutex;
  std::map<boost::asio::ip::address, uint16_t> outstanding;
  std::vector<std::pair<std::string, uint16_t>> need_resolve;
  std::atomic_flag completed;
};
}

void chratos::node::work_generate_blocking (chratos::block & block_a)
{
  block_a.block_work_set (work_generate_blocking (block_a.root ()));
}

void chratos::node::work_generate (chratos::uint256_union const & hash_a, std::function<void(uint64_t)> callback_a)
{
  auto work_generation (std::make_shared<distributed_work> (shared (), hash_a, callback_a));
  work_generation->start ();
}

uint64_t chratos::node::work_generate_blocking (chratos::uint256_union const & hash_a)
{
  std::promise<uint64_t> promise;
  work_generate (hash_a, [&promise](uint64_t work_a) {
    promise.set_value (work_a);
  });
  return promise.get_future ().get ();
}

void chratos::node::add_initial_peers ()
{
}

void chratos::node::block_confirm (std::shared_ptr<chratos::block> block_a)
{
  active.start (block_a);
  network.broadcast_confirm_req (block_a);
}

chratos::uint128_t chratos::node::delta ()
{
  auto result ((online_reps.online_stake () / 100) * config.online_weight_quorum);
  return result;
}

namespace
{
class confirmed_visitor : public chratos::block_visitor
{
public:
  confirmed_visitor (MDB_txn * transaction_a, chratos::node & node_a, std::shared_ptr<chratos::block> block_a, chratos::block_hash const & hash_a, chratos::block_hash const & dividend_a) :
  transaction (transaction_a),
  node (node_a),
  block (block_a),
  hash (hash_a),
  dividend (dividend_a)
  {
  }
  virtual ~confirmed_visitor () = default;
  void scan_receivable (chratos::account const & account_a)
  {
    for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
    {
      auto wallet (i->second);
      if (wallet->store.exists (transaction, account_a))
      {
        chratos::account representative;
        chratos::pending_info pending;
        representative = wallet->store.representative (transaction);
        auto error (node.store.pending_get (transaction, chratos::pending_key (account_a, hash), pending));
        if (!error)
        {
          auto node_l (node.shared ());
          auto amount (pending.amount.number ());
          wallet->receive_async (block, representative, amount, [](std::shared_ptr<chratos::block>) {});
        }
        else
        {
          if (!node.store.block_exists (transaction, hash))
          {
            BOOST_LOG (node.log) << boost::str (boost::format ("Confirmed block is missing:  %1%") % hash.to_string ());
            assert (false && "Confirmed block is missing");
          }
          else
          {
            BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% has already been received") % hash.to_string ());
          }
        }
      }
    }
  }
  void claim_dividend (chratos::dividend_block const & block_a)
  {
    std::shared_ptr<chratos::block> dividend_l = node.store.block_get (transaction, block_a.hash ());
    for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
    {
      auto wallet (i->second);
      auto accounts = wallet->search_unclaimed (block_a.hash ());
      chratos::account representative;
      representative = wallet->store.representative (transaction);

      for (auto & account : accounts)
      {
        // Check pending and claim outstanding
        receive_outstanding_pendings (wallet, account, block_a.hash ());
        // Check dividend points to the account's last claimed
        chratos::account_info info;
        node.store.account_get (transaction, account, info);
        if (info.dividend_block == dividend_l->dividend ()) {
          // Claim dividends
          wallet->claim_dividend_async (dividend_l, account, representative, [](std::shared_ptr<chratos::block>) {});
        } else {
          auto prev_hash = dividend_l->dividend ();
          std::shared_ptr<chratos::block> previous = node.store.block_get (transaction, prev_hash);
          chratos::dividend_block const * prev_dividend (dynamic_cast<chratos::dividend_block const *> (previous.get ()));
          claim_dividend (*prev_dividend);
          claim_dividend (block_a);
        }
      }
    }
  }

  void receive_outstanding_pendings (std::shared_ptr<chratos::wallet> wallet, chratos::account const & account_a, chratos::block_hash const & dividend_a)
  {
    wallet->receive_outstanding_pendings_sync (transaction, account_a, dividend_a);
  }
  void state_block (chratos::state_block const & block_a) override
  {
    scan_receivable (block_a.hashables.link);
  }
  void dividend_block (chratos::dividend_block const & block_a) override
  {
    claim_dividend (block_a);
  }
  void claim_block (chratos::claim_block const &) override
  {
  }
  MDB_txn * transaction;
  chratos::node & node;
  std::shared_ptr<chratos::block> block;
  chratos::block_hash const & hash;
  chratos::block_hash const & dividend;
};
}

void chratos::node::process_confirmed (std::shared_ptr<chratos::block> block_a)
{
  auto hash (block_a->hash ());
  bool exists (ledger.block_exists (hash));
  // Attempt to process confirmed block if it's not in ledger yet
  if (!exists)
  {
    chratos::transaction transaction (store.environment, nullptr, true);
    block_processor.process_receive_one (transaction, block_a);
    exists = store.block_exists (transaction, hash);
  }
  if (exists)
  {
    auto dividend (block_a->dividend ());
    chratos::transaction transaction (store.environment, nullptr, false);
    confirmed_visitor visitor (transaction, *this, block_a, hash, dividend);
    block_a->visit (visitor);
    auto account (ledger.account (transaction, hash));
    auto amount (ledger.amount (transaction, hash));
    bool is_state_send (false);
    chratos::account pending_account (0);
    if (auto state = dynamic_cast<chratos::state_block *> (block_a.get ()))
    {
      is_state_send = ledger.is_send (transaction, *state);
      pending_account = state->hashables.link;
    }
    observers.blocks.notify (block_a, account, amount, is_state_send);
    if (amount > 0)
    {
      observers.account_balance.notify (account, false);
      if (!pending_account.is_zero ())
      {
        observers.account_balance.notify (pending_account, true);
      }
    }
  }
}

void chratos::node::process_message (chratos::message & message_a, chratos::endpoint const & sender_a)
{
  network_message_visitor visitor (*this, sender_a);
  message_a.visit (visitor);
}

chratos::endpoint chratos::network::endpoint ()
{
  boost::system::error_code ec;
  auto port (socket.local_endpoint (ec).port ());
  if (ec)
  {
    BOOST_LOG (node.log) << "Unable to retrieve port: " << ec.message ();
  }
  return chratos::endpoint (boost::asio::ip::address_v6::loopback (), port);
}

bool chratos::block_arrival::add (chratos::block_hash const & hash_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  auto now (std::chrono::steady_clock::now ());
  auto inserted (arrival.insert (chratos::block_arrival_info{ now, hash_a }));
  auto result (!inserted.second);
  return result;
}

bool chratos::block_arrival::recent (chratos::block_hash const & hash_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  auto now (std::chrono::steady_clock::now ());
  while (arrival.size () > arrival_size_min && arrival.begin ()->arrival + arrival_time_min < now)
  {
    arrival.erase (arrival.begin ());
  }
  return arrival.get<1> ().find (hash_a) != arrival.get<1> ().end ();
}

chratos::online_reps::online_reps (chratos::node & node) :
node (node)
{
}

void chratos::online_reps::vote (std::shared_ptr<chratos::vote> const & vote_a)
{
  auto rep (vote_a->account);
  std::lock_guard<std::mutex> lock (mutex);
  auto now (std::chrono::steady_clock::now ());
  chratos::transaction transaction (node.store.environment, nullptr, false);
  auto current (reps.begin ());
  while (current != reps.end () && current->last_heard + std::chrono::seconds (chratos::node::cutoff) < now)
  {
    auto old_stake (online_stake_total);
    online_stake_total -= node.ledger.weight (transaction, current->representative);
    if (online_stake_total > old_stake)
    {
      // underflow
      online_stake_total = 0;
    }
    current = reps.erase (current);
  }
  auto rep_it (reps.get<1> ().find (rep));
  auto info (chratos::rep_last_heard_info{ now, rep });
  if (rep_it == reps.get<1> ().end ())
  {
    auto old_stake (online_stake_total);
    online_stake_total += node.ledger.weight (transaction, rep);
    if (online_stake_total < old_stake)
    {
      // overflow
      online_stake_total = std::numeric_limits<chratos::uint128_t>::max ();
    }
    reps.insert (info);
  }
  else
  {
    reps.get<1> ().replace (rep_it, info);
  }
}

void chratos::online_reps::recalculate_stake ()
{
  std::lock_guard<std::mutex> lock (mutex);
  online_stake_total = 0;
  chratos::transaction transaction (node.store.environment, nullptr, false);
  for (auto it : reps)
  {
    online_stake_total += node.ledger.weight (transaction, it.representative);
  }
  auto now (std::chrono::steady_clock::now ());
  std::weak_ptr<chratos::node> node_w (node.shared ());
  node.alarm.add (now + std::chrono::minutes (5), [node_w]() {
    if (auto node_l = node_w.lock ())
    {
      node_l->online_reps.recalculate_stake ();
    }
  });
}

chratos::uint128_t chratos::online_reps::online_stake ()
{
  std::lock_guard<std::mutex> lock (mutex);
  return std::max (online_stake_total, node.config.online_weight_minimum.number ());
}

std::deque<chratos::account> chratos::online_reps::list ()
{
  std::deque<chratos::account> result;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (reps.begin ()), n (reps.end ()); i != n; ++i)
  {
    result.push_back (i->representative);
  }
  return result;
}

std::unordered_set<chratos::endpoint> chratos::peer_container::random_set (size_t count_a)
{
  std::unordered_set<chratos::endpoint> result;
  result.reserve (count_a);
  std::lock_guard<std::mutex> lock (mutex);
  // Stop trying to fill result with random samples after this many attempts
  auto random_cutoff (count_a * 2);
  auto peers_size (peers.size ());
  // Usually count_a will be much smaller than peers.size()
  // Otherwise make sure we have a cutoff on attempting to randomly fill
  if (!peers.empty ())
  {
    for (auto i (0); i < random_cutoff && result.size () < count_a; ++i)
    {
      auto index (random_pool.GenerateWord32 (0, peers_size - 1));
      result.insert (peers.get<3> ()[index].endpoint);
    }
  }
  // Fill the remainder with most recent contact
  for (auto i (peers.get<1> ().begin ()), n (peers.get<1> ().end ()); i != n && result.size () < count_a; ++i)
  {
    result.insert (i->endpoint);
  }
  return result;
}

void chratos::peer_container::random_fill (std::array<chratos::endpoint, 8> & target_a)
{
  auto peers (random_set (target_a.size ()));
  assert (peers.size () <= target_a.size ());
  auto endpoint (chratos::endpoint (boost::asio::ip::address_v6{}, 0));
  assert (endpoint.address ().is_v6 ());
  std::fill (target_a.begin (), target_a.end (), endpoint);
  auto j (target_a.begin ());
  for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i, ++j)
  {
    assert (i->address ().is_v6 ());
    assert (j < target_a.end ());
    *j = *i;
  }
}

// Request a list of the top known representatives
std::vector<chratos::peer_information> chratos::peer_container::representatives (size_t count_a)
{
  std::vector<peer_information> result;
  result.reserve (std::min (count_a, size_t (16)));
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (peers.get<6> ().begin ()), n (peers.get<6> ().end ()); i != n && result.size () < count_a; ++i)
  {
    if (!i->rep_weight.is_zero ())
    {
      result.push_back (*i);
    }
  }
  return result;
}

void chratos::peer_container::purge_syn_cookies (std::chrono::steady_clock::time_point const & cutoff)
{
  std::lock_guard<std::mutex> lock (syn_cookie_mutex);
  auto it (syn_cookies.begin ());
  while (it != syn_cookies.end ())
  {
    auto info (it->second);
    if (info.created_at < cutoff)
    {
      unsigned & per_ip = syn_cookies_per_ip[it->first.address ()];
      if (per_ip > 0)
      {
        --per_ip;
      }
      else
      {
        assert (false && "More SYN cookies deleted than created for IP");
      }
      it = syn_cookies.erase (it);
    }
    else
    {
      ++it;
    }
  }
}

std::vector<chratos::peer_information> chratos::peer_container::purge_list (std::chrono::steady_clock::time_point const & cutoff)
{
  std::vector<chratos::peer_information> result;
  {
    std::lock_guard<std::mutex> lock (mutex);
    auto pivot (peers.get<1> ().lower_bound (cutoff));
    result.assign (pivot, peers.get<1> ().end ());
    for (auto i (peers.get<1> ().begin ()); i != pivot; ++i)
    {
      if (i->network_version < chratos::node_id_version)
      {
        if (legacy_peers > 0)
        {
          --legacy_peers;
        }
        else
        {
          assert (false && "More legacy peers removed than added");
        }
      }
    }
    // Remove peers that haven't been heard from past the cutoff
    peers.get<1> ().erase (peers.get<1> ().begin (), pivot);
    for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
    {
      peers.modify (i, [](chratos::peer_information & info) { info.last_attempt = std::chrono::steady_clock::now (); });
    }

    // Remove keepalive attempt tracking for attempts older than cutoff
    auto attempts_pivot (attempts.get<1> ().lower_bound (cutoff));
    attempts.get<1> ().erase (attempts.get<1> ().begin (), attempts_pivot);
  }
  if (result.empty ())
  {
    disconnect_observer ();
  }
  return result;
}

std::vector<chratos::endpoint> chratos::peer_container::rep_crawl ()
{
  std::vector<chratos::endpoint> result;
  // If there is enough observed peers weight, crawl 10 peers. Otherwise - 40
  uint16_t max_count = (total_weight () > online_weight_minimum) ? 10 : 40;
  result.reserve (max_count);
  std::lock_guard<std::mutex> lock (mutex);
  uint16_t count (0);
  for (auto i (peers.get<5> ().begin ()), n (peers.get<5> ().end ()); i != n && count < max_count; ++i, ++count)
  {
    result.push_back (i->endpoint);
  };
  return result;
}

size_t chratos::peer_container::size ()
{
  std::lock_guard<std::mutex> lock (mutex);
  return peers.size ();
}

size_t chratos::peer_container::size_sqrt ()
{
  auto result (std::ceil (std::sqrt (size ())));
  return result;
}

chratos::uint128_t chratos::peer_container::total_weight ()
{
  chratos::uint128_t result (0);
  std::unordered_set<chratos::account> probable_reps;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (peers.get<6> ().begin ()), n (peers.get<6> ().end ()); i != n; ++i)
  {
    // Calculate if representative isn't recorded for several IP addresses
    if (probable_reps.find (i->probable_rep_account) == probable_reps.end ())
    {
      result = result + i->rep_weight.number ();
      probable_reps.insert (i->probable_rep_account);
    }
  }
  return result;
}

bool chratos::peer_container::empty ()
{
  return size () == 0;
}

bool chratos::peer_container::not_a_peer (chratos::endpoint const & endpoint_a, bool blacklist_loopback)
{
  bool result (false);
  if (endpoint_a.address ().to_v6 ().is_unspecified ())
  {
    result = true;
  }
  else if (chratos::reserved_address (endpoint_a, blacklist_loopback))
  {
    result = true;
  }
  else if (endpoint_a == self)
  {
    result = true;
  }
  return result;
}

bool chratos::peer_container::rep_response (chratos::endpoint const & endpoint_a, chratos::account const & rep_account_a, chratos::amount const & weight_a)
{
  assert (endpoint_a.address ().is_v6 ());
  auto updated (false);
  std::lock_guard<std::mutex> lock (mutex);
  auto existing (peers.find (endpoint_a));
  if (existing != peers.end ())
  {
    peers.modify (existing, [weight_a, &updated, rep_account_a](chratos::peer_information & info) {
      info.last_rep_response = std::chrono::steady_clock::now ();
      if (info.rep_weight < weight_a)
      {
        updated = true;
        info.rep_weight = weight_a;
        info.probable_rep_account = rep_account_a;
      }
    });
  }
  return updated;
}

void chratos::peer_container::rep_request (chratos::endpoint const & endpoint_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  auto existing (peers.find (endpoint_a));
  if (existing != peers.end ())
  {
    peers.modify (existing, [](chratos::peer_information & info) {
      info.last_rep_request = std::chrono::steady_clock::now ();
    });
  }
}

bool chratos::peer_container::reachout (chratos::endpoint const & endpoint_a)
{
  // Don't contact invalid IPs
  bool error = not_a_peer (endpoint_a, false);
  if (!error)
  {
    auto endpoint_l (chratos::map_endpoint_to_v6 (endpoint_a));
    // Don't keepalive to nodes that already sent us something
    error |= known_peer (endpoint_l);
    std::lock_guard<std::mutex> lock (mutex);
    auto existing (attempts.find (endpoint_l));
    error |= existing != attempts.end ();
    attempts.insert ({ endpoint_l, std::chrono::steady_clock::now () });
  }
  return error;
}

bool chratos::peer_container::insert (chratos::endpoint const & endpoint_a, unsigned version_a)
{
  assert (endpoint_a.address ().is_v6 ());
  auto unknown (false);
  auto is_legacy (version_a < chratos::node_id_version);
  auto result (not_a_peer (endpoint_a, false));
  if (!result)
  {
    if (version_a >= chratos::protocol_version_min)
    {
      std::lock_guard<std::mutex> lock (mutex);
      auto existing (peers.find (endpoint_a));
      if (existing != peers.end ())
      {
        peers.modify (existing, [](chratos::peer_information & info) {
          info.last_contact = std::chrono::steady_clock::now ();
          // Don't update `network_version` here unless you handle the legacy peer caps (both global and per IP)
          // You'd need to ensure that an upgrade from network version 7 to 8 entails a node ID handshake
        });
        result = true;
      }
      else
      {
        unknown = true;
        if (is_legacy)
        {
          if (legacy_peers < max_legacy_peers)
          {
            ++legacy_peers;
          }
          else
          {
            result = true;
          }
        }
        if (!result && chratos_network != chratos_networks::chratos_test_network)
        {
          auto peer_it_range (peers.get<chratos::peer_by_ip_addr> ().equal_range (endpoint_a.address ()));
          auto i (peer_it_range.first);
          auto n (peer_it_range.second);
          unsigned ip_peers (0);
          unsigned legacy_ip_peers (0);
          while (i != n)
          {
            ++ip_peers;
            if (i->network_version < chratos::node_id_version)
            {
              ++legacy_ip_peers;
            }
            ++i;
          }
          if (ip_peers >= max_peers_per_ip || (is_legacy && legacy_ip_peers >= max_legacy_peers_per_ip))
          {
            result = true;
          }
        }
        if (!result)
        {
          peers.insert (chratos::peer_information (endpoint_a, version_a));
        }
      }
    }
  }
  if (unknown && !result)
  {
    peer_observer (endpoint_a);
  }
  return result;
}

namespace
{
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long address_a)
{
  return boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (address_a));
}
}

bool chratos::reserved_address (chratos::endpoint const & endpoint_a, bool blacklist_loopback)
{
  assert (endpoint_a.address ().is_v6 ());
  auto bytes (endpoint_a.address ().to_v6 ());
  auto result (false);
  static auto const rfc1700_min (mapped_from_v4_bytes (0x00000000ul));
  static auto const rfc1700_max (mapped_from_v4_bytes (0x00fffffful));
  static auto const ipv4_loopback_min (mapped_from_v4_bytes (0x7f000000ul));
  static auto const ipv4_loopback_max (mapped_from_v4_bytes (0x7ffffffful));
  static auto const rfc1918_1_min (mapped_from_v4_bytes (0x0a000000ul));
  static auto const rfc1918_1_max (mapped_from_v4_bytes (0x0afffffful));
  static auto const rfc1918_2_min (mapped_from_v4_bytes (0xac100000ul));
  static auto const rfc1918_2_max (mapped_from_v4_bytes (0xac1ffffful));
  static auto const rfc1918_3_min (mapped_from_v4_bytes (0xc0a80000ul));
  static auto const rfc1918_3_max (mapped_from_v4_bytes (0xc0a8fffful));
  static auto const rfc6598_min (mapped_from_v4_bytes (0x64400000ul));
  static auto const rfc6598_max (mapped_from_v4_bytes (0x647ffffful));
  static auto const rfc5737_1_min (mapped_from_v4_bytes (0xc0000200ul));
  static auto const rfc5737_1_max (mapped_from_v4_bytes (0xc00002fful));
  static auto const rfc5737_2_min (mapped_from_v4_bytes (0xc6336400ul));
  static auto const rfc5737_2_max (mapped_from_v4_bytes (0xc63364fful));
  static auto const rfc5737_3_min (mapped_from_v4_bytes (0xcb007100ul));
  static auto const rfc5737_3_max (mapped_from_v4_bytes (0xcb0071fful));
  static auto const ipv4_multicast_min (mapped_from_v4_bytes (0xe0000000ul));
  static auto const ipv4_multicast_max (mapped_from_v4_bytes (0xeffffffful));
  static auto const rfc6890_min (mapped_from_v4_bytes (0xf0000000ul));
  static auto const rfc6890_max (mapped_from_v4_bytes (0xfffffffful));
  static auto const rfc6666_min (boost::asio::ip::address_v6::from_string ("100::"));
  static auto const rfc6666_max (boost::asio::ip::address_v6::from_string ("100::ffff:ffff:ffff:ffff"));
  static auto const rfc3849_min (boost::asio::ip::address_v6::from_string ("2001:db8::"));
  static auto const rfc3849_max (boost::asio::ip::address_v6::from_string ("2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"));
  static auto const rfc4193_min (boost::asio::ip::address_v6::from_string ("fc00::"));
  static auto const rfc4193_max (boost::asio::ip::address_v6::from_string ("fd00:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
  static auto const ipv6_multicast_min (boost::asio::ip::address_v6::from_string ("ff00::"));
  static auto const ipv6_multicast_max (boost::asio::ip::address_v6::from_string ("ff00:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
  if (bytes >= rfc1700_min && bytes <= rfc1700_max)
  {
    result = true;
  }
  else if (bytes >= rfc5737_1_min && bytes <= rfc5737_1_max)
  {
    result = true;
  }
  else if (bytes >= rfc5737_2_min && bytes <= rfc5737_2_max)
  {
    result = true;
  }
  else if (bytes >= rfc5737_3_min && bytes <= rfc5737_3_max)
  {
    result = true;
  }
  else if (bytes >= ipv4_multicast_min && bytes <= ipv4_multicast_max)
  {
    result = true;
  }
  else if (bytes >= rfc6890_min && bytes <= rfc6890_max)
  {
    result = true;
  }
  else if (bytes >= rfc6666_min && bytes <= rfc6666_max)
  {
    result = true;
  }
  else if (bytes >= rfc3849_min && bytes <= rfc3849_max)
  {
    result = true;
  }
  else if (bytes >= ipv6_multicast_min && bytes <= ipv6_multicast_max)
  {
    result = true;
  }
  else if (blacklist_loopback && bytes.is_loopback ())
  {
    result = true;
  }
  else if (blacklist_loopback && bytes >= ipv4_loopback_min && bytes <= ipv4_loopback_max)
  {
    result = true;
  }
  else if (chratos::chratos_network == chratos::chratos_networks::chratos_live_network)
  {
    if (bytes >= rfc1918_1_min && bytes <= rfc1918_1_max)
    {
      result = true;
    }
    else if (bytes >= rfc1918_2_min && bytes <= rfc1918_2_max)
    {
      result = true;
    }
    else if (bytes >= rfc1918_3_min && bytes <= rfc1918_3_max)
    {
      result = true;
    }
    else if (bytes >= rfc6598_min && bytes <= rfc6598_max)
    {
      result = true;
    }
    else if (bytes >= rfc4193_min && bytes <= rfc4193_max)
    {
      result = true;
    }
  }
  return result;
}

chratos::peer_information::peer_information (chratos::endpoint const & endpoint_a, unsigned network_version_a) :
endpoint (endpoint_a),
ip_address (endpoint_a.address ()),
last_contact (std::chrono::steady_clock::now ()),
last_attempt (last_contact),
last_bootstrap_attempt (std::chrono::steady_clock::time_point ()),
last_rep_request (std::chrono::steady_clock::time_point ()),
last_rep_response (std::chrono::steady_clock::time_point ()),
rep_weight (0),
network_version (network_version_a),
node_id ()
{
}

chratos::peer_information::peer_information (chratos::endpoint const & endpoint_a, std::chrono::steady_clock::time_point const & last_contact_a, std::chrono::steady_clock::time_point const & last_attempt_a) :
endpoint (endpoint_a),
ip_address (endpoint_a.address ()),
last_contact (last_contact_a),
last_attempt (last_attempt_a),
last_bootstrap_attempt (std::chrono::steady_clock::time_point ()),
last_rep_request (std::chrono::steady_clock::time_point ()),
last_rep_response (std::chrono::steady_clock::time_point ()),
rep_weight (0),
node_id (),
network_version (chratos::protocol_version)
{
}

chratos::peer_container::peer_container (chratos::endpoint const & self_a) :
self (self_a),
peer_observer ([](chratos::endpoint const &) {}),
disconnect_observer ([]() {}),
legacy_peers (0)
{
}

bool chratos::peer_container::contacted (chratos::endpoint const & endpoint_a, unsigned version_a)
{
  auto endpoint_l (chratos::map_endpoint_to_v6 (endpoint_a));
  auto should_handshake (false);
  if (version_a < chratos::node_id_version)
  {
    insert (endpoint_l, version_a);
  }
  else if (!known_peer (endpoint_l) && peers.get<chratos::peer_by_ip_addr> ().count (endpoint_l.address ()) < max_peers_per_ip)
  {
    should_handshake = true;
  }
  return should_handshake;
}

void chratos::network::send_buffer (uint8_t const * data_a, size_t size_a, chratos::endpoint const & endpoint_a, std::function<void(boost::system::error_code const &, size_t)> callback_a)
{
  std::unique_lock<std::mutex> lock (socket_mutex);
  if (node.config.logging.network_packet_logging ())
  {
    BOOST_LOG (node.log) << "Sending packet";
  }
  socket.async_send_to (boost::asio::buffer (data_a, size_a), endpoint_a, [this, callback_a](boost::system::error_code const & ec, size_t size_a) {
    callback_a (ec, size_a);
    this->node.stats.add (chratos::stat::type::traffic, chratos::stat::dir::out, size_a);
    if (this->node.config.logging.network_packet_logging ())
    {
      BOOST_LOG (this->node.log) << "Packet send complete";
    }
  });
}

bool chratos::peer_container::known_peer (chratos::endpoint const & endpoint_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  auto existing (peers.find (endpoint_a));
  return existing != peers.end ();
}

std::shared_ptr<chratos::node> chratos::node::shared ()
{
  return shared_from_this ();
}

chratos::election_vote_result::election_vote_result () :
replay (false),
processed (false)
{
}

chratos::election_vote_result::election_vote_result (bool replay_a, bool processed_a)
{
  replay = replay_a;
  processed = processed_a;
}

chratos::election::election (chratos::node & node_a, std::shared_ptr<chratos::block> block_a, std::function<void(std::shared_ptr<chratos::block>)> const & confirmation_action_a) :
confirmation_action (confirmation_action_a),
root (block_a->root ()),
node (node_a),
status ({ block_a, 0 }),
confirmed (false),
aborted (false)
{
  last_votes.insert (std::make_pair (chratos::not_an_account, chratos::vote_info{ std::chrono::steady_clock::now (), 0, block_a->hash () }));
  blocks.insert (std::make_pair (block_a->hash (), block_a));
}

void chratos::election::compute_rep_votes (MDB_txn * transaction_a)
{
  if (node.config.enable_voting)
  {
    node.wallets.foreach_representative (transaction_a, [this, transaction_a](chratos::public_key const & pub_a, chratos::raw_key const & prv_a) {
      auto vote (this->node.store.vote_generate (transaction_a, pub_a, prv_a, status.winner));
      this->node.vote_processor.vote (vote, this->node.network.endpoint ());
    });
  }
}

void chratos::election::confirm_once (MDB_txn * transaction_a)
{
  if (!confirmed.exchange (true))
  {
    auto winner_l (status.winner);
    auto node_l (node.shared ());
    auto confirmation_action_l (confirmation_action);
    node.background ([node_l, winner_l, confirmation_action_l]() {
      node_l->process_confirmed (winner_l);
      confirmation_action_l (winner_l);
    });
  }
}

void chratos::election::abort ()
{
  aborted = true;
}

bool chratos::election::have_quorum (chratos::tally_t const & tally_a)
{
  auto i (tally_a.begin ());
  auto first (i->first);
  ++i;
  auto second (i != tally_a.end () ? i->first : 0);
  auto delta_l (node.delta ());
  auto result (tally_a.begin ()->first > (second + delta_l));
  return result;
}

chratos::tally_t chratos::election::tally (MDB_txn * transaction_a)
{
  std::unordered_map<chratos::block_hash, chratos::uint128_t> block_weights;
  for (auto vote_info : last_votes)
  {
    block_weights[vote_info.second.hash] += node.ledger.weight (transaction_a, vote_info.first);
  }
  last_tally = block_weights;
  chratos::tally_t result;
  for (auto item : block_weights)
  {
    auto block (blocks.find (item.first));
    if (block != blocks.end ())
    {
      result.insert (std::make_pair (item.second, block->second));
    }
  }
  return result;
}

void chratos::election::confirm_if_quorum (MDB_txn * transaction_a)
{
  auto tally_l (tally (transaction_a));
  assert (tally_l.size () > 0);
  auto winner (tally_l.begin ());
  auto block_l (winner->second);
  status.tally = winner->first;
  chratos::uint128_t sum (0);
  for (auto & i : tally_l)
  {
    sum += i.first;
  }
  if (sum >= node.config.online_weight_minimum.number () && !(*block_l == *status.winner))
  {
    auto node_l (node.shared ());
    node_l->block_processor.force (block_l);
    status.winner = block_l;
  }
  if (have_quorum (tally_l))
  {
    if (node.config.logging.vote_logging () || blocks.size () > 1)
    {
      log_votes (tally_l);
    }
    confirm_once (transaction_a);
  }
}

void chratos::election::log_votes (chratos::tally_t const & tally_a)
{
  BOOST_LOG (node.log) << boost::str (boost::format ("Vote tally for root %1%") % status.winner->root ().to_string ());
  for (auto i (tally_a.begin ()), n (tally_a.end ()); i != n; ++i)
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% weight %2%") % i->second->hash ().to_string () % i->first.convert_to<std::string> ());
  }
  for (auto i (last_votes.begin ()), n (last_votes.end ()); i != n; ++i)
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("%1% %2%") % i->first.to_account () % i->second.hash.to_string ());
  }
}

chratos::election_vote_result chratos::election::vote (chratos::account rep, uint64_t sequence, chratos::block_hash block_hash)
{
  // see republish_vote documentation for an explanation of these rules
  chratos::transaction transaction (node.store.environment, nullptr, false);
  auto replay (false);
  auto supply (node.online_reps.online_stake ());
  auto weight (node.ledger.weight (transaction, rep));
  auto should_process (false);
  if (chratos::chratos_network == chratos::chratos_networks::chratos_test_network || weight > supply / 1000) // 0.1% or above
  {
    unsigned int cooldown;
    if (weight < supply / 100) // 0.1% to 1%
    {
      cooldown = 15;
    }
    else if (weight < supply / 20) // 1% to 5%
    {
      cooldown = 5;
    }
    else // 5% or above
    {
      cooldown = 1;
    }
    auto should_process (false);
    auto last_vote_it (last_votes.find (rep));
    if (last_vote_it == last_votes.end ())
    {
      should_process = true;
    }
    else
    {
      auto last_vote (last_vote_it->second);
      if (last_vote.sequence < sequence || (last_vote.sequence == sequence && last_vote.hash < block_hash))
      {
        if (last_vote.time <= std::chrono::steady_clock::now () - std::chrono::seconds (cooldown))
        {
          should_process = true;
        }
      }
      else
      {
        replay = true;
      }
    }
    if (should_process)
    {
      last_votes[rep] = { std::chrono::steady_clock::now (), sequence, block_hash };
      if (!confirmed)
      {
        confirm_if_quorum (transaction);
      }
    }
  }
  return chratos::election_vote_result (replay, should_process);
}

bool chratos::election::publish (std::shared_ptr<chratos::block> block_a)
{
  auto result (false);
  if (blocks.size () >= 10)
  {
    if (last_tally[block_a->hash ()] < node.online_reps.online_stake () / 10)
    {
      result = true;
    }
  }
  if (!result)
  {
    blocks.insert (std::make_pair (block_a->hash (), block_a));
  }
  return result;
}

void chratos::active_transactions::announce_votes ()
{
  std::unordered_set<chratos::block_hash> inactive;
  chratos::transaction transaction (node.store.environment, nullptr, false);
  unsigned unconfirmed_count (0);
  unsigned unconfirmed_announcements (0);
  unsigned mass_request_count (0);
  std::vector<chratos::block_hash> blocks_bundle;

  for (auto i (roots.begin ()), n (roots.end ()); i != n; ++i)
  {
    auto election_l (i->election);
    if ((election_l->confirmed || election_l->aborted) && i->announcements >= announcement_min - 1)
    {
      if (election_l->confirmed)
      {
        confirmed.push_back (i->election->status);
        if (confirmed.size () > election_history_size)
        {
          confirmed.pop_front ();
        }
      }
      inactive.insert (election_l->root);
    }
    else
    {
      if (i->announcements > announcement_long)
      {
        ++unconfirmed_count;
        unconfirmed_announcements += i->announcements;
        // Log votes for very long unconfirmed elections
        if (i->announcements % 50 == 1)
        {
          auto tally_l (election_l->tally (transaction));
          election_l->log_votes (tally_l);
        }
      }
      if (i->announcements < announcement_long || i->announcements % announcement_long == 1)
      {
        // Broadcast winner
        if (node.ledger.could_fit (transaction, *election_l->status.winner))
        {
          if (node.config.enable_voting && std::chrono::system_clock::now () >= node.config.generate_hash_votes_at)
          {
            node.network.republish_block (transaction, election_l->status.winner, false);
            blocks_bundle.push_back (election_l->status.winner->hash ());
            if (blocks_bundle.size () >= 12)
            {
              node.wallets.foreach_representative (transaction, [&](chratos::public_key const & pub_a, chratos::raw_key const & prv_a) {
                auto vote (this->node.store.vote_generate (transaction, pub_a, prv_a, blocks_bundle));
                this->node.vote_processor.vote (vote, this->node.network.endpoint ());
              });
              blocks_bundle.clear ();
            }
          }
          else
          {
            election_l->compute_rep_votes (transaction);
            node.network.republish_block (transaction, election_l->status.winner);
          }
        }
        else if (i->announcements > 3)
        {
          election_l->abort ();
        }
      }
      if (i->announcements % 4 == 1)
      {
        auto reps (std::make_shared<std::vector<chratos::peer_information>> (node.peers.representatives (std::numeric_limits<size_t>::max ())));
        std::unordered_set<chratos::account> probable_reps;
        chratos::uint128_t total_weight (0);
        for (auto j (reps->begin ()), m (reps->end ()); j != m;)
        {
          auto & rep_votes (i->election->last_votes);
          auto rep_acct (j->probable_rep_account);
          // Calculate if representative isn't recorded for several IP addresses
          if (probable_reps.find (rep_acct) == probable_reps.end ())
          {
            total_weight = total_weight + j->rep_weight.number ();
            probable_reps.insert (rep_acct);
          }
          if (rep_votes.find (rep_acct) != rep_votes.end ())
          {
            std::swap (*j, reps->back ());
            reps->pop_back ();
            m = reps->end ();
          }
          else
          {
            ++j;
            if (node.config.logging.vote_logging ())
            {
              BOOST_LOG (node.log) << "Representative did not respond to confirm_req, retrying: " << rep_acct.to_account ();
            }
          }
        }
        if (!reps->empty () && (total_weight > node.config.online_weight_minimum.number () || mass_request_count > 20))
        {
          // broadcast_confirm_req_base modifies reps, so we clone it once to avoid aliasing
          node.network.broadcast_confirm_req_base (i->confirm_req_options.first, std::make_shared<std::vector<chratos::peer_information>> (*reps), 0);
        }
        else
        {
          // broadcast request to all peers
          node.network.broadcast_confirm_req_base (i->confirm_req_options.first, std::make_shared<std::vector<chratos::peer_information>> (node.peers.list_vector ()), 0);
          ++mass_request_count;
        }
      }
    }
    roots.modify (i, [](chratos::conflict_info & info_a) {
      ++info_a.announcements;
    });
  }
  if (node.config.enable_voting && !blocks_bundle.empty ())
  {
    node.wallets.foreach_representative (transaction, [&](chratos::public_key const & pub_a, chratos::raw_key const & prv_a) {
      auto vote (this->node.store.vote_generate (transaction, pub_a, prv_a, blocks_bundle));
      this->node.vote_processor.vote (vote, this->node.network.endpoint ());
    });
  }
  for (auto i (inactive.begin ()), n (inactive.end ()); i != n; ++i)
  {
    auto root_it (roots.find (*i));
    assert (root_it != roots.end ());
    for (auto successor : root_it->election->blocks)
    {
      auto successor_it (successors.find (successor.first));
      if (successor_it != successors.end ())
      {
        assert (successor_it->second == root_it->election);
        successors.erase (successor_it);
      }
      else
      {
        assert (false && "election successor not in active_transactions blocks table");
      }
    }
    roots.erase (root_it);
  }
  if (unconfirmed_count > 0)
  {
    BOOST_LOG (node.log) << boost::str (boost::format ("%1% blocks have been unconfirmed averaging %2% announcements") % unconfirmed_count % (unconfirmed_announcements / unconfirmed_count));
  }
}

void chratos::active_transactions::announce_loop ()
{
  std::unique_lock<std::mutex> lock (mutex);
  started = true;
  condition.notify_all ();
  while (!stopped)
  {
    announce_votes ();
    condition.wait_for (lock, std::chrono::milliseconds (announce_interval_ms));
  }
}

void chratos::active_transactions::stop ()
{
  {
    std::unique_lock<std::mutex> lock (mutex);
    while (!started)
    {
      condition.wait (lock);
    }
    stopped = true;
    roots.clear ();
    condition.notify_all ();
  }
  if (thread.joinable ())
  {
    thread.join ();
  }
}

bool chratos::active_transactions::start (std::shared_ptr<chratos::block> block_a, std::function<void(std::shared_ptr<chratos::block>)> const & confirmation_action_a)
{
  return start (std::make_pair (block_a, nullptr), confirmation_action_a);
}

bool chratos::active_transactions::start (std::pair<std::shared_ptr<chratos::block>, std::shared_ptr<chratos::block>> blocks_a, std::function<void(std::shared_ptr<chratos::block>)> const & confirmation_action_a)
{
  assert (blocks_a.first != nullptr);
  auto error (true);
  std::lock_guard<std::mutex> lock (mutex);
  if (!stopped)
  {
    auto primary_block (blocks_a.first);
    auto root (primary_block->root ());
    auto existing (roots.find (root));
    if (existing == roots.end ())
    {
      auto election (std::make_shared<chratos::election> (node, primary_block, confirmation_action_a));
      roots.insert (chratos::conflict_info{ root, election, 0, blocks_a });
      successors.insert (std::make_pair (primary_block->hash (), election));
    }
    error = existing != roots.end ();
  }
  return error;
}

// Validate a vote and apply it to the current election if one exists
bool chratos::active_transactions::vote (std::shared_ptr<chratos::vote> vote_a)
{
  std::shared_ptr<chratos::election> election;
  bool replay (false);
  bool processed (false);
  {
    std::lock_guard<std::mutex> lock (mutex);
    for (auto vote_block : vote_a->blocks)
    {
      chratos::election_vote_result result;
      if (vote_block.which ())
      {
        auto block_hash (boost::get<chratos::block_hash> (vote_block));
        auto existing (successors.find (block_hash));
        if (existing != successors.end ())
        {
          result = existing->second->vote (vote_a->account, vote_a->sequence, block_hash);
        }
      }
      else
      {
        auto block (boost::get<std::shared_ptr<chratos::block>> (vote_block));
        auto existing (roots.find (block->root ()));
        if (existing != roots.end ())
        {
          result = existing->election->vote (vote_a->account, vote_a->sequence, block->hash ());
        }
      }
      replay = replay || result.replay;
      processed = processed || result.processed;
    }
  }
  if (processed)
  {
    node.network.republish_vote (vote_a);
  }
  return replay;
}

bool chratos::active_transactions::active (chratos::block const & block_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  return roots.find (block_a.root ()) != roots.end ();
}

// List of active blocks in elections
std::deque<std::shared_ptr<chratos::block>> chratos::active_transactions::list_blocks ()
{
  std::deque<std::shared_ptr<chratos::block>> result;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto i (roots.begin ()), n (roots.end ()); i != n; ++i)
  {
    result.push_back (i->election->status.winner);
  }
  return result;
}

void chratos::active_transactions::erase (chratos::block const & block_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  if (roots.find (block_a.root ()) != roots.end ())
  {
    roots.erase (block_a.root ());
    BOOST_LOG (node.log) << boost::str (boost::format ("Election erased for block block %1% root %2%") % block_a.hash ().to_string () % block_a.root ().to_string ());
  }
}

chratos::active_transactions::active_transactions (chratos::node & node_a) :
node (node_a),
started (false),
stopped (false),
thread ([this]() { announce_loop (); })
{
  std::unique_lock<std::mutex> lock (mutex);
  while (!started)
  {
    condition.wait (lock);
  }
}

chratos::active_transactions::~active_transactions ()
{
  stop ();
}

bool chratos::active_transactions::publish (std::shared_ptr<chratos::block> block_a)
{
  std::lock_guard<std::mutex> lock (mutex);
  auto existing (roots.find (block_a->root ()));
  auto result (true);
  if (existing != roots.end ())
  {
    result = existing->election->publish (block_a);
    if (!result)
    {
      successors.insert (std::make_pair (block_a->hash (), existing->election));
    }
  }
  return result;
}

int chratos::node::store_version ()
{
  chratos::transaction transaction (store.environment, nullptr, false);
  return store.version_get (transaction);
}

chratos::thread_runner::thread_runner (boost::asio::io_service & service_a, unsigned service_threads_a)
{
  for (auto i (0); i < service_threads_a; ++i)
  {
    threads.push_back (std::thread ([&service_a]() {
      try
      {
        service_a.run ();
      }
      catch (...)
      {
#ifndef NDEBUG
        /*
         * In a release build, catch and swallow the
         * service exception, in debug mode pass it
         * on
         */
        throw;
#endif
      }
    }));
  }
}

chratos::thread_runner::~thread_runner ()
{
  join ();
}

void chratos::thread_runner::join ()
{
  for (auto & i : threads)
  {
    if (i.joinable ())
    {
      i.join ();
    }
  }
}

chratos::inactive_node::inactive_node (boost::filesystem::path const & path) :
path (path),
service (boost::make_shared<boost::asio::io_service> ()),
alarm (*service),
work (1, nullptr)
{
  boost::filesystem::create_directories (path);
  logging.max_size = std::numeric_limits<std::uintmax_t>::max ();
  logging.init (path);
  node = std::make_shared<chratos::node> (init, *service, 24000, path, alarm, logging, work);
}

chratos::inactive_node::~inactive_node ()
{
  node->stop ();
}

chratos::port_mapping::port_mapping (chratos::node & node_a) :
node (node_a),
devices (nullptr),
protocols ({ { { "TCP", 0, boost::asio::ip::address_v4::any (), 0 }, { "UDP", 0, boost::asio::ip::address_v4::any (), 0 } } }),
check_count (0),
on (false)
{
  urls = { 0 };
  data = { { 0 } };
}

void chratos::port_mapping::start ()
{
  check_mapping_loop ();
}

void chratos::port_mapping::refresh_devices ()
{
  if (chratos::chratos_network != chratos::chratos_networks::chratos_test_network)
  {
    std::lock_guard<std::mutex> lock (mutex);
    int discover_error = 0;
    freeUPNPDevlist (devices);
    devices = upnpDiscover (2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, false, 2, &discover_error);
    std::array<char, 64> local_address;
    local_address.fill (0);
    auto igd_error (UPNP_GetValidIGD (devices, &urls, &data, local_address.data (), sizeof (local_address)));
    if (igd_error == 1 || igd_error == 2)
    {
      boost::system::error_code ec;
      address = boost::asio::ip::address_v4::from_string (local_address.data (), ec);
    }
    if (check_count % 15 == 0)
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("UPnP local address: %1%, discovery: %2%, IGD search: %3%") % local_address.data () % discover_error % igd_error);
      for (auto i (devices); i != nullptr; i = i->pNext)
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("UPnP device url: %1% st: %2% usn: %3%") % i->descURL % i->st % i->usn);
      }
    }
  }
}

void chratos::port_mapping::refresh_mapping ()
{
  if (chratos::chratos_network != chratos::chratos_networks::chratos_test_network)
  {
    std::lock_guard<std::mutex> lock (mutex);
    auto node_port (std::to_string (node.network.endpoint ().port ()));

    // Intentionally omitted: we don't map the RPC port because, unless RPC authentication was added, this would almost always be a security risk
    for (auto & protocol : protocols)
    {
      std::array<char, 6> actual_external_port;
      actual_external_port.fill (0);
      auto add_port_mapping_error (UPNP_AddAnyPortMapping (urls.controlURL, data.first.servicetype, node_port.c_str (), node_port.c_str (), address.to_string ().c_str (), nullptr, protocol.name, nullptr, std::to_string (mapping_timeout).c_str (), actual_external_port.data ()));
      if (check_count % 15 == 0)
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("UPnP %1% port mapping response: %2%, actual external port %3%") % protocol.name % add_port_mapping_error % actual_external_port.data ());
      }
      if (add_port_mapping_error == UPNPCOMMAND_SUCCESS)
      {
        protocol.external_port = std::atoi (actual_external_port.data ());
      }
      else
      {
        protocol.external_port = 0;
      }
    }
  }
}

int chratos::port_mapping::check_mapping ()
{
  int result (3600);
  if (chratos::chratos_network != chratos::chratos_networks::chratos_test_network)
  {
    // Long discovery time and fast setup/teardown make this impractical for testing
    std::lock_guard<std::mutex> lock (mutex);
    auto node_port (std::to_string (node.network.endpoint ().port ()));
    for (auto & protocol : protocols)
    {
      std::array<char, 64> int_client;
      std::array<char, 6> int_port;
      std::array<char, 16> remaining_mapping_duration;
      remaining_mapping_duration.fill (0);
      auto verify_port_mapping_error (UPNP_GetSpecificPortMappingEntry (urls.controlURL, data.first.servicetype, node_port.c_str (), protocol.name, nullptr, int_client.data (), int_port.data (), nullptr, nullptr, remaining_mapping_duration.data ()));
      if (verify_port_mapping_error == UPNPCOMMAND_SUCCESS)
      {
        protocol.remaining = result;
      }
      else
      {
        protocol.remaining = 0;
      }
      result = std::min (result, protocol.remaining);
      std::array<char, 64> external_address;
      external_address.fill (0);
      auto external_ip_error (UPNP_GetExternalIPAddress (urls.controlURL, data.first.servicetype, external_address.data ()));
      if (external_ip_error == UPNPCOMMAND_SUCCESS)
      {
        boost::system::error_code ec;
        protocol.external_address = boost::asio::ip::address_v4::from_string (external_address.data (), ec);
      }
      else
      {
        protocol.external_address = boost::asio::ip::address_v4::any ();
      }
      if (check_count % 15 == 0)
      {
        BOOST_LOG (node.log) << boost::str (boost::format ("UPnP %1% mapping verification response: %2%, external ip response: %3%, external ip: %4%, internal ip: %5%, remaining lease: %6%") % protocol.name % verify_port_mapping_error % external_ip_error % external_address.data () % address.to_string () % remaining_mapping_duration.data ());
      }
    }
  }
  return result;
}

void chratos::port_mapping::check_mapping_loop ()
{
  int wait_duration = check_timeout;
  refresh_devices ();
  if (devices != nullptr)
  {
    auto remaining (check_mapping ());
    // If the mapping is lost, refresh it
    if (remaining == 0)
    {
      refresh_mapping ();
    }
  }
  else
  {
    wait_duration = 300;
    if (check_count < 10)
    {
      BOOST_LOG (node.log) << boost::str (boost::format ("UPnP No IGD devices found"));
    }
  }
  ++check_count;
  if (on)
  {
    auto node_l (node.shared ());
    node.alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (wait_duration), [node_l]() {
      node_l->port_mapping.check_mapping_loop ();
    });
  }
}

void chratos::port_mapping::stop ()
{
  on = false;
  std::lock_guard<std::mutex> lock (mutex);
  for (auto & protocol : protocols)
  {
    if (protocol.external_port != 0)
    {
      // Be a good citizen for the router and shut down our mapping
      auto delete_error (UPNP_DeletePortMapping (urls.controlURL, data.first.servicetype, std::to_string (protocol.external_port).c_str (), protocol.name, address.to_string ().c_str ()));
      BOOST_LOG (node.log) << boost::str (boost::format ("Shutdown port mapping response: %1%") % delete_error);
    }
  }
  freeUPNPDevlist (devices);
  devices = nullptr;
}
