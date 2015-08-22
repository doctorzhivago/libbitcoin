/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/bitcoin/network/channel_proxy.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <system_error>
#include <boost/asio.hpp>
#include <boost/date_time.hpp>
#include <boost/format.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/system/error_code.hpp>
#include <bitcoin/bitcoin/chain/block.hpp>
#include <bitcoin/bitcoin/chain/transaction.hpp>
#include <bitcoin/bitcoin/config/authority.hpp>
#include <bitcoin/bitcoin/error.hpp>
#include <bitcoin/bitcoin/math/checksum.hpp>
#include <bitcoin/bitcoin/message/address.hpp>
#include <bitcoin/bitcoin/message/get_address.hpp>
#include <bitcoin/bitcoin/message/get_blocks.hpp>
#include <bitcoin/bitcoin/message/get_data.hpp>
#include <bitcoin/bitcoin/message/header.hpp>
#include <bitcoin/bitcoin/message/inventory.hpp>
#include <bitcoin/bitcoin/message/ping_pong.hpp>
#include <bitcoin/bitcoin/message/verack.hpp>
#include <bitcoin/bitcoin/message/version.hpp>
#include <bitcoin/bitcoin/network/channel_loader_module.hpp>
#include <bitcoin/bitcoin/network/shared_const_buffer.hpp>
#include <bitcoin/bitcoin/network/timeout.hpp>
#include <bitcoin/bitcoin/utility/assert.hpp>
#include <bitcoin/bitcoin/utility/container_source.hpp>
#include <bitcoin/bitcoin/utility/data.hpp>
#include <bitcoin/bitcoin/utility/deadline.hpp>
#include <bitcoin/bitcoin/utility/endian.hpp>
#include <bitcoin/bitcoin/utility/logger.hpp>
#include <bitcoin/bitcoin/utility/random.hpp>
#include <bitcoin/bitcoin/utility/dispatcher.hpp>
#include <bitcoin/bitcoin/utility/serializer.hpp>
#include <bitcoin/bitcoin/utility/string.hpp>

namespace libbitcoin {
namespace network {

using std::placeholders::_1;
using std::placeholders::_2;
using boost::asio::buffer;
using boost::asio::ip::tcp;
using boost::format;
using boost::posix_time::time_duration;

// The proxy will have no config with timers moved to channel.
channel_proxy::channel_proxy(socket_ptr socket, threadpool& pool,
    const timeout& timeouts)
  : socket_(socket),
    dispatch_(pool),
    timeouts_(timeouts),
    expiration_(std::make_shared<deadline>(pool, timeouts.expiration)),
    inactivity_(std::make_shared<deadline>(pool, timeouts.inactivity)),
    revival_(std::make_shared<deadline>(pool, timeouts.revival)),
    revival_handler_(nullptr),
    stopped_(false),
    version_subscriber_(std::make_shared<version_subscriber>(pool)),
    verack_subscriber_(std::make_shared<verack_subscriber>(pool)),
    address_subscriber_(std::make_shared<address_subscriber>(pool)),
    get_address_subscriber_(std::make_shared<get_address_subscriber>(pool)),
    inventory_subscriber_(std::make_shared<inventory_subscriber>(pool)),
    get_data_subscriber_(std::make_shared<get_data_subscriber>(pool)),
    get_blocks_subscriber_(std::make_shared<get_blocks_subscriber>(pool)),
    transaction_subscriber_(std::make_shared<transaction_subscriber>(pool)),
    block_subscriber_(std::make_shared<block_subscriber>(pool)),
    ping_subscriber_(std::make_shared<ping_subscriber>(pool)),
    pong_subscriber_(std::make_shared<pong_subscriber>(pool)),
    raw_subscriber_(std::make_shared<raw_subscriber>(pool)),
    stop_subscriber_(std::make_shared<stop_subscriber>(pool))
{
    establish_relay<message::version>(version_subscriber_);
    establish_relay<message::verack>(verack_subscriber_);
    establish_relay<message::address>(address_subscriber_);
    establish_relay<message::get_address>(get_address_subscriber_);
    establish_relay<message::inventory>(inventory_subscriber_);
    establish_relay<message::get_data>(get_data_subscriber_);
    establish_relay<message::get_blocks>(get_blocks_subscriber_);
    establish_relay<chain::transaction>(transaction_subscriber_);
    establish_relay<chain::block>(block_subscriber_);
    establish_relay<message::ping>(ping_subscriber_);
    establish_relay<message::pong>(pong_subscriber_);
}

channel_proxy::~channel_proxy()
{
    do_stop(error::channel_stopped);
}

template<typename Message, class Subscriber>
void channel_proxy::establish_relay(Subscriber subscriber)
{
    const auto message_handler = [subscriber](const std::error_code& ec,
        const Message& message)
    {
        subscriber->relay(ec, message);
    };

    stream_loader_.add<Message>(message_handler);
}

// Subscribing must be immediate, we cannot switch thread contexts.
template <typename Message, class Subscriber, typename Callback>
void channel_proxy::subscribe(Subscriber subscriber, Callback handler) const
{
    if (stopped())
        subscriber->relay(error::channel_stopped, Message());
    else
        subscriber->subscribe(handler);
}

// Subscriber doesn't have an unsubscribe, we just send service_stopped.
// The subscriber then has the option to not resubscribe in the handler.
template <typename Message, class Subscriber>
void channel_proxy::notify_stop(Subscriber subscriber) const
{
    subscriber->relay(error::channel_stopped, Message());
}

void channel_proxy::start()
{
    read_header();
    start_timers();
}

config::authority channel_proxy::address() const
{
    boost::system::error_code ec;
    const auto endpoint = socket_->remote_endpoint(ec);

    // The endpoint may have become disconnected.
    return ec ? config::authority() : config::authority(endpoint);
}

bool channel_proxy::stopped() const
{
    return stopped_;
}

void channel_proxy::stop(const boost::system::error_code& ec)
{
    stop(error::boost_to_error_code(ec));
}

void channel_proxy::stop(const std::error_code& ec)
{
    if (stopped())
        return;

    dispatch_.queue(
        std::bind(&channel_proxy::do_stop,
            shared_from_this(), ec));
}

void channel_proxy::do_stop(const std::error_code& ec)
{
    if (stopped())
        return;

    stopped_ = true;
    clear_timers();

    // Shutter the socket, ignore the error code.
    boost::system::error_code code;
    socket_->shutdown(tcp::socket::shutdown_both, code);
    socket_->close(code);

    // Clear all message subscriptions and notify with stop reason code.
    clear_subscriptions(ec);
}

void channel_proxy::clear_subscriptions(const std::error_code& ec)
{
    notify_stop<message::version>(version_subscriber_);
    notify_stop<message::verack>(verack_subscriber_);
    notify_stop<message::address>(address_subscriber_);
    notify_stop<message::get_address>(get_address_subscriber_);
    notify_stop<message::inventory>(inventory_subscriber_);
    notify_stop<message::get_data>(get_data_subscriber_);
    notify_stop<message::get_blocks>(get_blocks_subscriber_);
    notify_stop<chain::transaction>(transaction_subscriber_);
    notify_stop<chain::block>(block_subscriber_);
    notify_stop<message::ping>(ping_subscriber_);
    notify_stop<message::pong>(pong_subscriber_);
    raw_subscriber_->relay(ec, message::header(), data_chunk());
    stop_subscriber_->relay(ec);
}

void channel_proxy::clear_timers()
{
    expiration_->cancel();
    inactivity_->cancel();
    revival_->cancel();
    revival_handler_ = nullptr;
}

void channel_proxy::start_timers()
{
    if (stopped())
        return;

    start_expiration();
    start_revival();
    start_inactivity();
}

// public
void channel_proxy::reset_revival()
{
    if (stopped())
        return;

    start_revival();
}

// public
void channel_proxy::set_revival_handler(revival_handler handler)
{
    if (stopped())
        return;

    revival_handler_ = handler;
}

void channel_proxy::start_expiration()
{
    if (stopped())
        return;

    const auto timeout = pseudo_randomize(timeouts_.expiration);

    expiration_->start(
        std::bind(&channel_proxy::handle_expiration,
            shared_from_this(), _1), timeout);
}

void channel_proxy::start_inactivity()
{
    if (stopped())
        return;

    inactivity_->start(
        std::bind(&channel_proxy::handle_inactivity,
            shared_from_this(), _1));
}

void channel_proxy::start_revival()
{
    if (stopped())
        return;

    revival_->start(
        std::bind(&channel_proxy::handle_revival,
            shared_from_this(), _1));
}

void channel_proxy::handle_expiration(const std::error_code& ec)
{
    if (stopped())
        return;

    if (deadline::canceled(ec))
        return;

    log_info(LOG_NETWORK)
        << "Channel lifetime expired [" << address() << "]";

    stop(error::channel_timeout);
}

void channel_proxy::handle_inactivity(const std::error_code& ec)
{
    if (stopped())
        return;

    if (deadline::canceled(ec))
        return;

    log_info(LOG_NETWORK)
        << "Channel inactivity timeout [" << address() << "]";

    stop(error::channel_timeout);
}

void channel_proxy::handle_revival(const std::error_code& ec)
{
    if (stopped())
        return;

    if (deadline::canceled(ec))
        return;

    // Nothing to do, no handler registered.
    if (revival_handler_ == nullptr)
        return;

    revival_handler_(ec);
}

void channel_proxy::read_header()
{
    if (stopped())
        return;

    async_read(*socket_, buffer(inbound_header_),
        dispatch_.sync(&channel_proxy::handle_read_header,
            shared_from_this(), _1, _2));
}

void channel_proxy::read_checksum(const message::header& header)
{
    if (stopped())
        return;

    async_read(*socket_, buffer(inbound_checksum_),
        dispatch_.sync(&channel_proxy::handle_read_checksum,
            shared_from_this(), _1, _2, header));
}

void channel_proxy::read_payload(const message::header& header)
{
    if (stopped())
        return;

    inbound_payload_.resize(header.payload_length);
    async_read(*socket_, buffer(inbound_payload_, header.payload_length),
        dispatch_.sync(&channel_proxy::handle_read_payload,
            shared_from_this(), _1, _2, header));
}

void channel_proxy::handle_read_header(const boost::system::error_code& ec,
    size_t DEBUG_ONLY(bytes_transferred))
{
    if (stopped())
        return;

    if (ec)
    {
        log_debug(LOG_NETWORK)
            << "Channel failure [" << address() << "] "
            << std::error_code(error::boost_to_error_code(ec)).message();
        stop(ec);
        return;
    }

    BITCOIN_ASSERT(bytes_transferred == message::header::size);
    BITCOIN_ASSERT(bytes_transferred == inbound_header_.size());

    typedef byte_source<message::header::header_bytes> header_source;
    typedef boost::iostreams::stream<header_source> header_stream;

    message::header header;
    const auto parsed = header.from_data(header_stream(inbound_header_));
    if (!parsed || header.magic != bc::magic_value)
    {
        log_warning(LOG_NETWORK) 
            << "Invalid header received [" << address() << "]";
        stop(error::bad_stream);
        return;
    }

    log_debug(LOG_NETWORK)
        << "Receive " << header.command << " [" << address() << "] ("
        << header.payload_length << " bytes)";

    read_checksum(header);
    start_inactivity();
}

void channel_proxy::handle_read_checksum(const boost::system::error_code& ec,
    size_t bytes_transferred, message::header& header)
{
    if (stopped())
        return;

    // Client may have disconnected after sending, so allow bad channel.
    if (ec)
    {
        // But make sure we got the required data, as this may fail depending 
        // when the client disconnected.
        if (bytes_transferred != message::header::checksum_size ||
            bytes_transferred != inbound_checksum_.size())
        {
            // TODO: No error if we aborted, causing the invalid data?
            log_warning(LOG_NETWORK)
                << "Invalid checksum from [" << address() << "] "
                << std::error_code(error::boost_to_error_code(ec)).message();
            stop(ec);
            return;
        }
    }

    header.checksum = from_little_endian<uint32_t>(inbound_checksum_.begin(),
        inbound_checksum_.end());

    read_payload(header);
    start_inactivity();
}

void channel_proxy::handle_read_payload(const boost::system::error_code& ec,
    size_t bytes_transferred, const message::header& header)
{
    if (stopped())
        return;

    // Client may have disconnected, so allow bad channel.
    if (ec)
    {
        // But make sure we got the required data, as this may fail depending 
        // when the client disconnected.
        if (bytes_transferred != header.payload_length ||
            bytes_transferred != inbound_payload_.size())
        {
            // TODO: No error if we aborted, causing the invalid data?
            log_warning(LOG_NETWORK)
                << "Invalid payload from [" << address() << "] "
                << std::error_code(error::boost_to_error_code(ec)).message();
            stop(ec);
            return;
        }
    }

    if (header.checksum != bitcoin_checksum(inbound_payload_))
    {
        log_warning(LOG_NETWORK) 
            << "Invalid bitcoin checksum from [" << address() << "]";
        stop(error::bad_stream);
        return;
    }

    // Parse and publish the raw payload to subscribers.
    raw_subscriber_->relay(error::success, header, inbound_payload_);

    // Copy the buffer before registering for new messages.
    const data_chunk payload_copy(inbound_payload_);

    // This must be called before calling subscribe notification handlers.
    if (!ec)
        read_header();

    start_inactivity();

    typedef byte_source<data_chunk> payload_source;
    typedef boost::iostreams::stream<payload_source> payload_stream;

    // Parse and publish the payload to message subscribers.
    payload_source source(payload_copy);
    payload_stream istream(source);

    if (stream_loader_.load(header.command, istream))
        if (istream.peek() != std::istream::traits_type::eof())
            log_warning(LOG_NETWORK)
                << "Valid message [" << header.command
                << "] handled, unused bytes remain in payload.";

    // Now we stop the channel if there was an error and we aren't yet stopped.
    if (ec)
        stop(ec);
}

void channel_proxy::subscribe_version(
    receive_version_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::version());
    else
        subscribe<message::version>(version_subscriber_, handle_receive);
}

void channel_proxy::subscribe_verack(
    receive_verack_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::verack());
    else
        subscribe<message::verack>(verack_subscriber_, handle_receive);
}

void channel_proxy::subscribe_address(
    receive_address_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::address());
    else
        subscribe<message::address>(address_subscriber_, handle_receive);
}

void channel_proxy::subscribe_get_address(
    receive_get_address_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::get_address());
    else
        subscribe<message::get_address>(get_address_subscriber_, handle_receive);
}

void channel_proxy::subscribe_inventory(
    receive_inventory_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::inventory());
    else
        subscribe<message::inventory>(inventory_subscriber_, handle_receive);
}

void channel_proxy::subscribe_get_data(
    receive_get_data_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::get_data());
    else
        subscribe<message::get_data>(get_data_subscriber_, handle_receive);
}

void channel_proxy::subscribe_get_blocks(
    receive_get_blocks_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::get_blocks());
    else
        subscribe<message::get_blocks>(get_blocks_subscriber_, handle_receive);
}

void channel_proxy::subscribe_transaction(
    receive_transaction_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, chain::transaction());
    else
        subscribe<chain::transaction>(transaction_subscriber_, handle_receive);
}

void channel_proxy::subscribe_block(
    receive_block_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, chain::block());
    else
        subscribe<chain::block>(block_subscriber_, handle_receive);
}

void channel_proxy::subscribe_ping(
    receive_ping_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::ping());
    else
        subscribe<message::ping>(ping_subscriber_, handle_receive);
}

void channel_proxy::subscribe_pong(
    receive_pong_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::pong());
    else
        subscribe<message::pong>(pong_subscriber_, handle_receive);
}

void channel_proxy::subscribe_raw(receive_raw_handler handle_receive)
{
    if (stopped())
        handle_receive(error::channel_stopped, message::header(), data_chunk());
    else
        raw_subscriber_->subscribe(handle_receive);
}

void channel_proxy::subscribe_stop(stop_handler handle_stop)
{
    if (stopped())
        handle_stop(error::channel_stopped);
    else
        stop_subscriber_->subscribe(handle_stop);
}

void channel_proxy::do_send(const data_chunk& message,
    send_handler handle_send, const std::string& command)
{
    if (stopped())
    {
        handle_send(error::channel_stopped);
        return;
    }

    log_debug(LOG_NETWORK)
        << "Send " << command << " [" << address() << "] ("
        << message.size() << " bytes)";

    const shared_const_buffer buffer(message);
    async_write(*socket_, buffer,
        std::bind(&channel_proxy::call_handle_send,
            shared_from_this(), _1, handle_send));
}

void channel_proxy::call_handle_send(const boost::system::error_code& ec,
    send_handler handle_send)
{
    handle_send(error::boost_to_error_code(ec));
}

void channel_proxy::send_raw(const message::header& packet_header,
    const data_chunk& payload, send_handler handle_send)
{
    if (stopped())
    {
        handle_send(error::channel_stopped);
        return;
    }

    dispatch_.queue(
        std::bind(&channel_proxy::do_send_raw,
            shared_from_this(), packet_header, payload, handle_send));
}

void channel_proxy::do_send_raw(const message::header& packet_header,
    const data_chunk& payload, send_handler handle_send)
{
    if (stopped())
    {
        handle_send(error::channel_stopped);
        return;
    }

    auto message = packet_header.to_data();
    extend_data(message, payload);
    do_send(message, handle_send, packet_header.command);
}

} // namespace network
} // namespace libbitcoin
