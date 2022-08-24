#include <memory>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>
#include <spdlog/spdlog.h>
#include <args.hxx>
#include "Common/Message.hpp"
#include "Common/ThreadSafeQueue.hpp"

namespace SP {

	/**
	 * @brief Represents a client connection with the server
	 *
	 */
	class ServerConnection: public std::enable_shared_from_this<ServerConnection> {
	  public:
		/**
		 * @brief Message recieved from remote connection
		 *
		 */
		struct ClientMessage {
			std::shared_ptr<ServerConnection> remote = nullptr;
			Message msg;
		};

	  public:
		ServerConnection(uint32_t uid, asio::io_context& asioContext, asio::ip::tcp::socket socket,
						 ThreadSafeQueue<ClientMessage>& incommingMessageQue):
			id(uid),
			mAsioContext(asioContext),
			mSocket(std::move(socket)),
			arIncommingMessageQue(incommingMessageQue) {
			if (mSocket.is_open()) {
				readHeader();
			}
		}

		virtual ~ServerConnection() {
		}

		/**
		 * @brief Used to get Id of the client
		 *
		 * @return uint32_t
		 */
		uint32_t getID() const {
			return id;
		}

	  public:
		void disconnect() {
			if (isConnected())
				asio::post(mAsioContext, [this]() { mSocket.close(); });
		}

		bool isConnected() const {
			return mSocket.is_open();
		}

	  public:
		/**
		 * @brief Used to submit message to be sent to client
		 *
		 * @param msg
		 */
		void send(const Message& msg) {
			asio::post(mAsioContext, [this, msg]() {
				bool currentlyWrittingMessage = !mOutgoingMessageQueue.empty();
				mOutgoingMessageQueue.push_back(msg);
				if (!currentlyWrittingMessage) {
					writeHeader();
				}
			});
		}

	  private:
		/**
		 * @brief Reads header of the incoming message
		 *
		 */
		void readHeader() {
			asio::async_read(
				mSocket, asio::buffer(&mTemporaryIncommingMessage.header, sizeof(MessageHeader)),
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						if (mTemporaryIncommingMessage.header.size > 0) {
							mTemporaryIncommingMessage.body.resize(mTemporaryIncommingMessage.header.size);
							readBody();
						} else {
							arIncommingMessageQue.push_back({this->shared_from_this(), mTemporaryIncommingMessage});
							readHeader();
						}
					} else {
						spdlog::warn("Failed to read header for client with id: {}", id);
						mSocket.close();
					}
				});
		}

		/**
		 * @brief Reads body of the incoming message
		 *
		 */
		void readBody() {
			asio::async_read(
				mSocket, asio::buffer(mTemporaryIncommingMessage.body.data(), mTemporaryIncommingMessage.body.size()),
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						arIncommingMessageQue.push_back({this->shared_from_this(), mTemporaryIncommingMessage});
						readHeader();
					} else {
						spdlog::warn("Failed to read body for client with id: {}", id);
						mSocket.close();
					}
				});
		}

		/**
		 * @brief Used to send header of a message
		 *
		 */
		void writeHeader() {
			asio::async_write(mSocket, asio::buffer(&mOutgoingMessageQueue.front().header, sizeof(MessageHeader)),
							  [this](std::error_code ec, std::size_t length) {
								  if (!ec) {
									  if (mOutgoingMessageQueue.front().body.size() > 0) {
										  writeBody();
									  } else {
										  mOutgoingMessageQueue.pop_front();

										  if (!mOutgoingMessageQueue.empty()) {
											  writeHeader();
										  }
									  }
								  } else {
									  spdlog::warn("Failed to write header for client with id: {}", id);
									  mSocket.close();
								  }
							  });
		}

		/**
		 * @brief Used to send body of a message
		 *
		 */
		void writeBody() {
			asio::async_write(
				mSocket,
				asio::buffer(mOutgoingMessageQueue.front().body.data(), mOutgoingMessageQueue.front().body.size()),
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						mOutgoingMessageQueue.pop_front();
						if (!mOutgoingMessageQueue.empty()) {
							writeHeader();
						}
					} else {
						spdlog::warn("Failed to write body for client with id: {}", id);
						mSocket.close();
					}
				});
		}

	  protected:
		// Each connection has a unique socket to a remote
		asio::ip::tcp::socket mSocket;

		// This context is shared with the whole asio instance
		asio::io_context& mAsioContext;

		// This queue holds all messages to be sent to the remote side
		// of this connection
		ThreadSafeQueue<Message> mOutgoingMessageQueue;

		// This references the incoming queue of the parent object
		ThreadSafeQueue<ClientMessage>& arIncommingMessageQue;

		// Temporary to message to store message
		Message mTemporaryIncommingMessage;

		uint32_t id = 0;
	};

	class Server {
	  public:
		/**
		 * @brief Construct a new Server object
		 *
		 * @param port ranging from 1024 to 65536
		 */
		Server(uint16_t port): mAsioAcceptor(mAsioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
		}

		virtual ~Server() {
			Stop();
		}

		// Starts the server!
		void start() {
			try {
				waitForClientConnection();

				mWorkerThread = std::thread([this]() { mAsioContext.run(); });
				spdlog::info("Server Started");
				while (!mAsioContext.stopped()) {
					if (mIncommingMessageQueue.empty()) {
						mIncommingMessageQueue.wait();
					}
					auto msg = mIncommingMessageQueue.pop_front();
					onMessage(msg.remote, msg.msg);
				}
			} catch (std::exception& e) {
				spdlog::error("Failed to start server with error: {}", e.what());
			}
		}

		// Stops the server!
		void Stop() {
			mAsioContext.stop();
			if (mWorkerThread.joinable())
				mWorkerThread.join();
			spdlog::info("Server Stopped");
		}

	  private:
		// Starts waiting for client connection
		void waitForClientConnection() {
			mAsioAcceptor.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
				if (!ec) {
					auto remoteEndpoint = socket.remote_endpoint();
					spdlog::info("New Connection: {}", remoteEndpoint.address().to_string());

					auto newconn = std::make_shared<ServerConnection>(mIDCounter++, mAsioContext, std::move(socket),
																	  mIncommingMessageQueue);

					mConnectionsList.push_back(std::move(newconn));

					spdlog::info("Connection completed with id: {}", mConnectionsList.back()->getID());
				} else {
					spdlog::warn("Failed to connect to client with error: {}", ec.message());
				}
				waitForClientConnection();
			});
		}

		/**
		 * @brief Used to send a message to client
		 *
		 * @param client
		 * @param msg
		 */
		void messageClient(std::shared_ptr<ServerConnection> client, const Message& msg) {
			if (client && client->isConnected()) {
				client->send(msg);
			} else {
				client.reset();
				mConnectionsList.erase(std::remove(mConnectionsList.begin(), mConnectionsList.end(), client),
									   mConnectionsList.end());
			}
		}

	  protected:
		/**
		 * @brief Called when a message is recieved from client
		 *
		 * @param client
		 * @param msg
		 */
		virtual void onMessage(std::shared_ptr<ServerConnection> client, Message& msg) {
			// simply echoing
			client->send(msg);
		}

	  protected:
		// Thread Safe Queue for incoming message packets
		ThreadSafeQueue<ServerConnection::ClientMessage> mIncommingMessageQueue;

		// Container of active connections
		std::deque<std::shared_ptr<ServerConnection>> mConnectionsList;

		asio::io_context mAsioContext;
		std::thread mWorkerThread;

		// Handles new incoming connection attempts...
		asio::ip::tcp::acceptor mAsioAcceptor;

		// Clients will be identified in the "wider system" via an ID
		uint32_t mIDCounter = 10000;
	};

} // namespace SP

template<> struct fmt::formatter<SP::ServerConnection::ClientMessage> {
	constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
		return ctx.end();
	}

	template<typename FormatContext>
	auto format(const SP::ServerConnection::ClientMessage& input, FormatContext& ctx) -> decltype(ctx.out()) {
		return format_to(ctx.out(), "{{ \"header\": {{ \"correlationId\": {}, \"size\": {} }}, \"body\": \"{}\" }}",
						 input.msg.header.correlationId, input.msg.header.size,
						 std::string_view(reinterpret_cast<char*>(input.msg.body.data()), input.msg.body.size()););
	}
};

int main(int argc, char* argv[]) {
	spdlog::set_level(spdlog::level::info);
	auto parser	  = args::ArgumentParser("Simple TCP server");
	auto help	  = args::HelpFlag(parser, "help", "Display help menu", {'h', "help"});
	auto portFlag = args::ValueFlag<uint16_t>(parser, "port", "Port", {'p', "port"}, 8888);
	try {
		parser.ParseCLI(argc, argv);
		auto server = SP::Server(portFlag.Get());
		server.start();
	} catch (args::Help const&) {
		spdlog::error(parser.Help());
	} catch (args::ParseError const& e) {
		spdlog::error(parser.Help());
	} catch (args::ValidationError const& e) {
		spdlog::error(e.what());
		spdlog::info(parser.Help());
	}
	return 0;
}