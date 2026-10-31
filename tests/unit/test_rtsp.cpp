/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Verify RTSP response handlers through their common dispatcher signature.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

// local includes
#include <src/config.h>
#include <src/crypto.h>
#include <src/globals.h>
#include <src/rtsp.h>
#include <src/utility.h>

/**
 * @brief Const RTSP wire headers decode network-order lengths independently of encryption flags.
 */
TEST(RtspHeaderTest, ConstAccessorsPreserveTheSerializedHeader) {
  using rtsp_stream::encrypted_rtsp_header_t;
  for (const auto length : {0U, 1U, 0x7fffffffU}) {
    for (const bool encrypted : {false, true}) {
      const auto flags = encrypted ? encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT : 0U;
      const encrypted_rtsp_header_t header {util::endian::big(length | flags), 0, {}};
      EXPECT_EQ(header.payload_length(), length);
      EXPECT_EQ(header.is_encrypted(), encrypted);
      EXPECT_EQ(header.typeAndLength, util::endian::big(length | flags));
    }
  }
}

namespace rtsp_stream {
  class rtsp_server_t;

  /**
   * @brief Release a parsed RTSP message and its allocation.
   * @param message Message to release.
   */
  void free_msg(PRTSP_MESSAGE message);

  /**
   * @brief Owning RTSP message type used by the dispatcher.
   */
  using msg_t = util::safe_ptr<RTSP_MESSAGE, &free_msg>;

  /**
   * @brief Respond to OPTIONS through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_option(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to DESCRIBE through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_describe(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to PLAY through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_play(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);
}  // namespace rtsp_stream

/**
 * @brief All three handlers respond with the request's sequence even without a server instance.
 */
TEST(RtspResponseTest, CommonDispatcherPreservesResponsesWithoutServerContext) {
  using boost::asio::ip::tcp;
  const std::array handlers {
    &rtsp_stream::cmd_option,
    &rtsp_stream::cmd_describe,
    &rtsp_stream::cmd_play,
  };

  for (const auto handler : handlers) {
    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();

    rtsp_stream::launch_session_t session {};
    rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
    request->sequenceNumber = 42;
    handler(nullptr, server_socket, session, std::move(request));
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with("RTSP/1.0 200 OK\r\n"));
    EXPECT_NE(response.find("CSeq: 42\r\n"), std::string::npos);

    if (handler == &rtsp_stream::cmd_describe) {
      EXPECT_NE(response.find("a=x-ss-general.featureFlags:"), std::string::npos);
      EXPECT_NE(response.find("a=x-ss-general.encryptionSupported:"), std::string::npos);
      EXPECT_NE(response.find("a=fmtp:97 surround-params="), std::string::npos);
    } else {
      EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    }
  }
}

namespace {
  // RTSP listens on this plus RTSP_SETUP_PORT, away from a server running on the default 47989
  constexpr auto test_base_port = 39989;

  std::shared_ptr<rtsp_stream::launch_session_t> encrypted_launch(std::uint32_t id, std::uint8_t key_fill) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    launch->unique_id = "rtsp-key-test-" + std::to_string(id);
    launch->rtsp_cipher = crypto::cipher::gcm_t {crypto::aes_t(16, key_fill), false};
    return launch;
  }

  crypto::aes_t message_iv(std::uint32_t sequence, char origin) {
    crypto::aes_t iv(12);
    std::memcpy(iv.data(), &sequence, sizeof(sequence));
    iv[10] = static_cast<std::uint8_t>(origin);
    iv[11] = 'R';
    return iv;
  }

  // Sends one request encrypted with the key and returns the reply decrypted with it, nullopt when
  // the reply is not this key's (the server answered on behalf of another launch)
  std::optional<std::string> exchange(const std::string &request, std::uint8_t key_fill, std::uint32_t sequence) {
    crypto::cipher::gcm_t cipher {crypto::aes_t(16, key_fill), false};
    std::vector<std::uint8_t> message(8 + crypto::cipher::tag_size + request.size());
    const auto type_and_length = util::endian::big<std::uint32_t>(rtsp_stream::encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT + request.size());
    const auto sequence_number = util::endian::big<std::uint32_t>(sequence);
    std::memcpy(message.data(), &type_and_length, sizeof(type_and_length));
    std::memcpy(message.data() + 4, &sequence_number, sizeof(sequence_number));
    auto iv = message_iv(sequence, 'C');
    cipher.encrypt(request, message.data() + 8, &iv);

    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket {io_context};
    const boost::asio::ip::tcp::endpoint server {boost::asio::ip::make_address("127.0.0.1"), test_base_port + rtsp_stream::RTSP_SETUP_PORT};
    // The server thread binds the listener a moment after it starts
    boost::system::error_code ec {boost::asio::error::not_connected};
    for (auto attempt = 0; ec && attempt < 50; ++attempt) {
      socket.close(ec);
      socket.connect(server, ec);
      if (ec) {
        std::this_thread::sleep_for(std::chrono::milliseconds {100});
      }
    }
    boost::asio::write(socket, boost::asio::buffer(message));

    rtsp_stream::encrypted_rtsp_header_t header {};
    boost::asio::read(socket, boost::asio::buffer(&header, sizeof(header)));
    std::vector<std::uint8_t> reply(crypto::cipher::tag_size + header.payload_length());
    std::memcpy(reply.data(), header.tag.data(), crypto::cipher::tag_size);
    boost::asio::read(socket, boost::asio::buffer(reply.data() + crypto::cipher::tag_size, header.payload_length()));

    std::optional<std::string> response;
    std::vector<std::uint8_t> plaintext;
    auto reply_iv = message_iv(util::endian::big<std::uint32_t>(header.sequenceNumber), 'H');
    if (cipher.decrypt(std::string_view {(const char *) reply.data(), reply.size()}, plaintext, &reply_iv) == 0) {
      response = std::string {plaintext.begin(), plaintext.end()};
    }
    return response;
  }
}  // namespace

/**
 * @brief Two launches of one client wait for their handshakes at once (a stream per screen or
 *        window), and the second client speaks first: its connection must reach its own launch.
 */
TEST(RtspLaunchTest, ConnectionReachesTheLaunchWhoseKeyVerifiesIt) {
  const auto saved_mail = mail::man;
  const auto saved_port = config::sunshine.port;
  mail::man = std::make_shared<safe::mail_raw_t>();
  config::sunshine.port = test_base_port;
  std::thread server {rtsp_stream::start};

  rtsp_stream::launch_session_raise(encrypted_launch(1, 0x11));
  rtsp_stream::launch_session_raise(encrypted_launch(2, 0x22));
  const auto second = exchange("OPTIONS rtsp://127.0.0.1 RTSP/1.0\r\nCSeq: 1\r\n\r\n", 0x22, 1);
  const auto first = exchange("OPTIONS rtsp://127.0.0.1 RTSP/1.0\r\nCSeq: 1\r\n\r\n", 0x11, 1);

  rtsp_stream::launch_session_clear(1);
  rtsp_stream::launch_session_clear(2);
  mail::man->event<bool>(mail::shutdown)->raise(true);
  server.join();
  mail::man = saved_mail;
  config::sunshine.port = saved_port;

  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->starts_with("RTSP/1.0 200 OK")) << *second;
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->starts_with("RTSP/1.0 200 OK")) << *first;
}
