/*
   Copyright (C) 2011 - 2018 by Sergey Popov <loonycyborg@gmail.com>
   Part of the Battle for Wesnoth Project https://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "wesnothd_connection.hpp"

#include "gettext.hpp"
#include "log.hpp"
#include "serialization/parser.hpp"
#include "utils/functional.hpp"

#include <cstdint>
#include <deque>

static lg::log_domain log_network("network");
#define DBG_NW LOG_STREAM(debug, log_network)
#define LOG_NW LOG_STREAM(info, log_network)
#define WRN_NW LOG_STREAM(warn, log_network)
#define ERR_NW LOG_STREAM(err, log_network)

#if 0
// code for the travis test
#include <sys/types.h>
#include <unistd.h>
namespace {
struct mptest_log
{
	mptest_log(const char* functionname)
	{
		WRN_NW << "Process:" << getpid() << " Thread:" << std::this_thread::get_id() << " Function: " << functionname << " Start\n";
	}
};
}
#define MPTEST_LOG mptest_log mptest_log__(__func__)
#else
#define MPTEST_LOG ((void)0)
#endif

using boost::system::error_code;
using boost::system::system_error;

// main thread
wesnothd_connection::wesnothd_connection(const std::string& host, const std::string& service, bool encrypted)
	: encrypted_(encrypted)
	, worker_thread_()
	, io_service_()
	, ssl_ctx_(boost::asio::ssl::context::sslv23)
	, resolver_(io_service_)
	, socket_(io_service_, ssl_ctx_)
	, last_error_()
	, last_error_mutex_()
	, handshake_finished_()
	, read_buf_()
	, handshake_response_()
	, recv_queue_()
	, recv_queue_mutex_()
	, recv_queue_lock_()
	, payload_size_(0)
	, bytes_to_write_(0)
	, bytes_written_(0)
	, bytes_to_read_(0)
	, bytes_read_(0)
{
	MPTEST_LOG;

/*
 * Connecting to server cases:
 * 		CASE                            FORM          SSL                DETERMINED BY		   
 * 		----                            ----          ---                -------------
 * 		official redirector           - server:port - requires non-SSL - server and port match game_config.cfg
 * 		official server (redirected)  - server:port - maybe            - redirect response
 * 		official server (no redirect) - server:port - assume SSL       - server matches game_config.cfg, port doesn't match
 * 		LAN server                    - IP:port     - requires non-SSL - has an IP address instead of a server name or is "localhost"
 * 		unofficial server             - server:port - assume no SSL    - server doesn't match game_config.cfg
 * 		boost unit test               - N/A         - no               - hard-coded to false in the test initialization
 */

	// TODO: windows needs special handling?
	//      test, and if so, then see https://stackoverflow.com/questions/39772878/reliable-way-to-get-root-ca-certificates-on-windows
	//                                https://stackoverflow.com/questions/40307541/boost-asio-ssl-context-not-verifying-certificates

	// the boost unit tests silently fail with encryption disabled, but loudly fail with it enabled
	// this is because they're actually trying to connect to a wesnothd instance that for hopefully obvious reasons doesn't exist
	if(encrypted_) {
		ssl_ctx_.set_default_verify_paths();

		// TODO: can remove this if using system-validated cert
		//       might still be need for the MP unit tests? how to determine if this is needed - only use file if it exists?
		ssl_ctx_.load_verify_file("certs/"+host+".crt");
		socket_.set_verify_mode(boost::asio::ssl::verify_fail_if_no_peer_cert);
		socket_.set_verify_callback(boost::asio::ssl::rfc2818_verification(host));

		resolver_.async_resolve(resolver::query(host, service),
			std::bind(&wesnothd_connection::handle_resolve, this, _1, _2));
	}

	// Starts the worker thread. Do this *after* the above async_resolve call or it will just exit immediately!
	worker_thread_ = std::thread([this]() {
		try {
			io_service_.run();
		} catch(const boost::system::system_error&) {
			try {
				// Attempt to pass the exception on to the handshake promise.
				handshake_finished_.set_exception(std::current_exception());
			} catch(const std::future_error&) {
				// Handshake already complete. Do nothing.
			}
		}

		LOG_NW << "wesnothd_connection::io_service::run() returned\n";
	});

	LOG_NW << "Resolving hostname: " << host << '\n';
}

wesnothd_connection::~wesnothd_connection()
{
	MPTEST_LOG;

	// Stop the io_service and wait for the worker thread to terminate.
	stop();
	worker_thread_.join();
}

// worker thread
void wesnothd_connection::handle_resolve(const error_code& ec, resolver::iterator iterator)
{
	MPTEST_LOG;
	if(ec) {
		LOG_NW << __func__ << " Throwing: " << ec << "\n";
		throw system_error(ec);
	}

	connect(iterator);
}

// worker thread
void wesnothd_connection::connect(resolver::iterator iterator)
{
	MPTEST_LOG;
	boost::asio::async_connect(socket_.lowest_layer(), iterator, std::bind(&wesnothd_connection::handle_connect, this, _1, iterator));
	LOG_NW << "Connecting to " << iterator->endpoint().address() << '\n';
}

// worker thread
void wesnothd_connection::handle_connect(const boost::system::error_code& ec, resolver::iterator iterator)
{
	MPTEST_LOG;
	if(ec) {
		WRN_NW << "Failed to connect to " << iterator->endpoint().address() << ": " << ec.message() << '\n';
		// TODO: need to test this part somehow
		//       not sure if this works, or if I need to use shutdown() and create an entirely new socket
		//       though the intention is to try multiple IPs through the same ssl context, so this sounds correct at least
		socket_.lowest_layer().close();

		if(++iterator == resolver::iterator()) {
			ERR_NW << "Tried all IPs. Giving up" << std::endl;
			socket_.shutdown();
			throw system_error(ec);
		} else {
			connect(iterator);
		}
	} else {
		LOG_NW << "Connected to " << iterator->endpoint().address() << '\n';
		handshake();
	}
}

// worker thread
void wesnothd_connection::handshake()
{
	MPTEST_LOG;
	static const uint32_t handshake = 0;

	socket_.async_handshake(boost::asio::ssl::stream_base::client,
		[this](const error_code& hs_ec)
		{
			if(hs_ec) {
				throw system_error(hs_ec);
			} else {
				boost::asio::async_write(socket_, boost::asio::buffer(reinterpret_cast<const char*>(&handshake), 4),
					[](const error_code& ec, std::size_t) { if(ec) { throw system_error(ec); } });

				boost::asio::async_read(socket_, boost::asio::buffer(&handshake_response_.binary, 4),
					std::bind(&wesnothd_connection::handle_handshake, this, _1));
			}
		});
}

// worker thread
void wesnothd_connection::handle_handshake(const error_code& ec)
{
	MPTEST_LOG;
	if(ec) {
		LOG_NW << __func__ << " Throwing: " << ec << "\n";
		throw system_error(ec);
	}

	handshake_finished_.set_value();
	recv();
}

// main thread
void wesnothd_connection::wait_for_handshake()
{
	MPTEST_LOG;
	LOG_NW << "Waiting for handshake" << std::endl;

	try {
		handshake_finished_.get_future().get();
	} catch(const boost::system::system_error& err) {
		if(err.code() == boost::asio::error::operation_aborted || err.code() == boost::asio::error::eof) {
			return;
		}

		WRN_NW << __func__ << " Rethrowing: " << err.code() << "\n";
		throw error(err.code());
	} catch(const std::future_error& e) {
		if(e.code() == std::future_errc::future_already_retrieved) {
			return;
		}
	}
}

// main thread
void wesnothd_connection::send_data(const configr_of& request)
{
	MPTEST_LOG;

#if BOOST_VERSION >= 106600
	auto buf_ptr = std::make_unique<boost::asio::streambuf>();
#else
	auto buf_ptr = std::make_shared<boost::asio::streambuf>();
#endif

	std::ostream os(buf_ptr.get());
	write_gz(os, request);

	// No idea why io_service::post doesn't like this lambda while asio::post does.
#if BOOST_VERSION >= 106600
	boost::asio::post(io_service_, [this, buf_ptr = std::move(buf_ptr)]() mutable {
#else
	io_service_.post([this, buf_ptr]() {
#endif
		DBG_NW << "In wesnothd_connection::send_data::lambda\n";
		send_queue_.push(std::move(buf_ptr));

		if(send_queue_.size() == 1) {
			send();
		}
	});
}

// main thread
void wesnothd_connection::cancel()
{
	MPTEST_LOG;
	if(socket_.lowest_layer().is_open()) {
		boost::system::error_code ec;

#ifdef _MSC_VER
// Silence warning about boost::asio::basic_socket<Protocol>::cancel always
// returning an error on XP, which we don't support anymore.
#pragma warning(push)
#pragma warning(disable:4996)
#endif
		socket_.lowest_layer().cancel(ec);
#ifdef _MSC_VER
#pragma warning(pop)
#endif

		if(ec) {
			WRN_NW << "Failed to cancel network operations: " << ec.message() << std::endl;
		}
	}
}

// main thread
void wesnothd_connection::stop()
{
	MPTEST_LOG;
	boost::system::error_code ec;
	socket_.shutdown(ec);
	if(ec) {
		WRN_NW << "Unable to stop socket: " << ec.message() << "\n";
	}
	io_service_.stop();
}

// worker thread
std::size_t wesnothd_connection::is_write_complete(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	MPTEST_LOG;
	if(ec) {
		{
			std::lock_guard<std::mutex> lock(last_error_mutex_);
			last_error_ = ec;
		}

		LOG_NW << __func__ << " Error: " << ec << "\n";

		io_service_.stop();
		return bytes_to_write_ - bytes_transferred;
	}

	bytes_written_ = bytes_transferred;
	return bytes_to_write_ - bytes_transferred;
}

// worker thread
void wesnothd_connection::handle_write(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	MPTEST_LOG;
	DBG_NW << "Written " << bytes_transferred << " bytes.\n";

	send_queue_.pop();

	if(ec) {
		{
			std::lock_guard<std::mutex> lock(last_error_mutex_);
			last_error_ = ec;
		}

		LOG_NW << __func__ << " Error: " << ec << "\n";

		io_service_.stop();
		return;
	}

	if(!send_queue_.empty()) {
		send();
	}
}

// worker thread
std::size_t wesnothd_connection::is_read_complete(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	// We use custom is_write/read_complete function to be able to see the current progress of the upload/download
	MPTEST_LOG;
	if(ec) {
		{
			std::lock_guard<std::mutex> lock(last_error_mutex_);
			last_error_ = ec;
		}

		LOG_NW << __func__ << " Error: " << ec << "\n";

		io_service_.stop();
		return bytes_to_read_ - bytes_transferred;
	}

	bytes_read_ = bytes_transferred;

	if(bytes_transferred < 4) {
		return 4;
	}

	if(!bytes_to_read_) {
		std::istream is(&read_buf_);
		data_union data_size;

		is.read(data_size.binary, 4);
		bytes_to_read_ = ntohl(data_size.num) + 4;

		// Close immediately if we receive an invalid length
		if(bytes_to_read_ < 4) {
			bytes_to_read_ = bytes_transferred;
		}
	}

	return bytes_to_read_ - bytes_transferred;
}

// worker thread
void wesnothd_connection::handle_read(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
	MPTEST_LOG;
	DBG_NW << "Read " << bytes_transferred << " bytes.\n";

	bytes_to_read_ = 0;
	if(last_error_ && ec != boost::asio::error::eof) {
		{
			std::lock_guard<std::mutex> lock(last_error_mutex_);
			last_error_ = ec;
		}

		LOG_NW << __func__ << " Error: " << ec << "\n";

		io_service_.stop();
		return;
	}

	std::istream is(&read_buf_);
	config data;
	read_gz(data, is);
	if(!data.empty()) { DBG_NW << "Received:\n" << data; }

	{
		std::lock_guard<std::mutex> lock(recv_queue_mutex_);
		recv_queue_.emplace(std::move(data));
		recv_queue_lock_.notify_all();
	}

	recv();
}

// worker thread
void wesnothd_connection::send()
{
	MPTEST_LOG;
	auto& buf = *send_queue_.front();

	std::size_t buf_size = buf.size();
	bytes_to_write_ = buf_size + 4;
	bytes_written_ = 0;
	payload_size_ = htonl(buf_size);

	boost::asio::streambuf::const_buffers_type gzipped_data = buf.data();
	std::deque<boost::asio::const_buffer> bufs(gzipped_data.begin(), gzipped_data.end());

	bufs.push_front(boost::asio::buffer(reinterpret_cast<const char*>(&payload_size_), 4));

	boost::asio::async_write(socket_, bufs,
		std::bind(&wesnothd_connection::is_write_complete, this, _1, _2),
		std::bind(&wesnothd_connection::handle_write, this, _1, _2));
}

// worker thread
void wesnothd_connection::recv()
{
	MPTEST_LOG;

	boost::asio::async_read(socket_, read_buf_,
		std::bind(&wesnothd_connection::is_read_complete, this, _1, _2),
		std::bind(&wesnothd_connection::handle_read, this, _1, _2));
}

// main thread
bool wesnothd_connection::receive_data(config& result)
{
	MPTEST_LOG;

	{
		std::lock_guard<std::mutex> lock(recv_queue_mutex_);
		if(!recv_queue_.empty()) {
			result.swap(recv_queue_.front());
			recv_queue_.pop();
			return true;
		}
	}

	{
		std::lock_guard<std::mutex> lock(last_error_mutex_);
		if(last_error_) {
			std::string user_msg;

			if(last_error_ == boost::asio::error::eof) {
				user_msg = _("Disconnected from server.");
			}

			throw error(last_error_, user_msg);
		}
	}

	return false;
}

bool wesnothd_connection::wait_and_receive_data(config& data)
{
	{
		std::unique_lock<std::mutex> lock(recv_queue_mutex_);
		recv_queue_lock_.wait(lock, [this]() { return has_data_received(); });
	}

	return receive_data(data);
};
