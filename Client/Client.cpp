#include <memory>
#include <atomic>
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
		ServerConnection(asio::io_context& asioContext, asio::ip::tcp::socket socket,
						 const asio::ip::tcp::resolver::results_type& endpoints,
						 ThreadSafeQueue<Message>& incommingMessageQue):
			mAsioContext(asioContext),
			mSocket(std::move(socket)),
			arIncommingMessageQue(incommingMessageQue) {
			asio::async_connect(mSocket, endpoints, [this](std::error_code ec, asio::ip::tcp::endpoint endpoint) {
				if (!ec) {
					readHeader();
				}
			});
		}

		virtual ~ServerConnection() {
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
			asio::async_read(mSocket, asio::buffer(&mTemporaryIncommingMessage.header, sizeof(MessageHeader)),
							 [this](std::error_code ec, std::size_t length) {
								 if (!ec) {
									 mTemporaryIncommingMessage.header.returnTime =
										 std::chrono::high_resolution_clock::now().time_since_epoch().count();
									 if (mTemporaryIncommingMessage.header.size > 0) {
										 mTemporaryIncommingMessage.body.resize(mTemporaryIncommingMessage.header.size);
										 readBody();
									 } else {
										 arIncommingMessageQue.push_back(mTemporaryIncommingMessage);
										 readHeader();
									 }
								 } else {
									 spdlog::warn("Failed to read header send by server");
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
						arIncommingMessageQue.push_back(mTemporaryIncommingMessage);
						readHeader();
					} else {
						spdlog::warn("Failed to read body send by server");
						mSocket.close();
					}
				});
		}

		/**
		 * @brief Used to send header of a message
		 *
		 */
		void writeHeader() {
			auto& header	= mOutgoingMessageQueue.front().header;
			header.sendTime = std::chrono::high_resolution_clock::now().time_since_epoch().count();
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
									  spdlog::warn("Failed to write header send by server");
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
						spdlog::warn("Failed to write body send by server");
						mSocket.close();
					}
				});
		}

	  protected:
		asio::ip::tcp::socket mSocket;
		asio::io_context& mAsioContext;
		ThreadSafeQueue<Message> mOutgoingMessageQueue;
		ThreadSafeQueue<Message>& arIncommingMessageQue;
		Message mTemporaryIncommingMessage;
	};

	class Client {
	  public:
		/**
		 * @brief Used to create client
		 *
		 * @param host host name or ip
		 * @port port
		 */
		Client(const std::string& host, const uint16_t port) {
			try {
				asio::ip::tcp::resolver resolver(mAsioContext);
				asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(host, std::to_string(port));

				m_connection = std::make_unique<ServerConnection>(mAsioContext, asio::ip::tcp::socket(mAsioContext),
																  endpoints, mIncommingMessageQueue);

				mWorkerThread = std::thread([this]() { mAsioContext.run(); });
			} catch (std::exception& e) {
				std::throw_with_nested(std::runtime_error("Client couldn't connect to server"));
			}
		}

		virtual ~Client() {
			disconnect();
		}

		void disconnect() {
			if (isConnected()) {
				m_connection->disconnect();
			}

			mAsioContext.stop();
			if (mWorkerThread.joinable())
				mWorkerThread.join();
			m_connection.release();
		}

		bool isConnected() {
			if (m_connection)
				return m_connection->isConnected();
			else
				return false;
		}

		// Send message to server
		bool send(std::string msg) {
			if (isConnected()) {
				Message newMsg;
				newMsg.header.correlationId = messageId++;
				newMsg.header.size			= msg.size();
				newMsg.header.creationTime	= std::chrono::high_resolution_clock::now().time_since_epoch().count();
				newMsg.body.resize(newMsg.header.size);
				memcpy(newMsg.body.data(), msg.data(), newMsg.header.size);
				m_connection->send(newMsg);
				return true;
			}
			return false;
		}

		ThreadSafeQueue<Message>& getIncomingMessage() {
			return mIncommingMessageQueue;
		}

	  protected:
		asio::io_context mAsioContext;
		std::thread mWorkerThread;
		std::unique_ptr<ServerConnection> m_connection;

	  private:
		std::atomic<uint32_t> messageId = 1;
		ThreadSafeQueue<Message> mIncommingMessageQueue;
	};

} // namespace SP
using namespace std::chrono_literals;
int main(int argc, char* argv[]) {
	spdlog::set_level(spdlog::level::info);
	auto parser	  = args::ArgumentParser("Simple TCP client");
	auto help	  = args::HelpFlag(parser, "help", "Display help menu", {'h', "help"});
	auto hostFlag = args::ValueFlag<std::string>(parser, "host", "host", {'p', "host"}, "127.0.0.1");
	auto portFlag = args::ValueFlag<uint16_t>(parser, "port", "Port", {'p', "port"}, 8888);
	auto numberOfMsgFlag =
		args::ValueFlag<size_t>(parser, "numOfMsg", "Number of messages used for testing", {'n', "numOfMsg"}, 10000);
	try {
		parser.ParseCLI(argc, argv);
		auto client		   = SP::Client(hostFlag.Get(), portFlag.Get());
		auto msgCount	   = numberOfMsgFlag.Get();
		spdlog::info("Sending {} messages", msgCount);
		size_t messageSent = 0;
		for (; messageSent < msgCount; messageSent++) {
			auto msg = fmt::format("Echo {}", messageSent);
			spdlog::debug("Client message to server=> {}", msg);
			if (!client.send(msg))
				break;
		}
		auto& incommingMessages	  = client.getIncomingMessage();
		long long totalTravelTime = 0;
		size_t messageRecieved	  = 0;
		spdlog::info("Checking recieved {} messages", msgCount);
		while (messageRecieved < msgCount) {
			if (!client.isConnected()) {
				break;
			}
			if (incommingMessages.empty()) {
				std::this_thread::sleep_for(1s);
				continue;
			}
			auto msg = incommingMessages.pop_front();
			spdlog::debug("Server message to client=> {}", msg);
			totalTravelTime += msg.header.returnTime - msg.header.sendTime;
			messageRecieved++;
			messageSent--;
		}
		//converting nano seconds to milliseconds
		totalTravelTime /= 1000000;
		long long avgTimeTaken = totalTravelTime / msgCount;
		spdlog::info("Average time taken {} micro seconds", avgTimeTaken);
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