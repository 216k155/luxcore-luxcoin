#ifndef SAM_H__
#define SAM_H__

#include <inttypes.h>
#include <string>
#include <map>
#include <list>
#include <thread>
#include <mutex>
#include <memory>
#include <boost/asio.hpp>
#include "Identity.h"
#include "LeaseSet.h"
#include "Streaming.h"
#include "Destination.h"

namespace i2p
{
namespace client
{
	const size_t SAM_SOCKET_BUFFER_SIZE = 8192;
	const int SAM_SOCKET_CONNECTION_MAX_IDLE = 3600; // in seconds
	const int SAM_SESSION_READINESS_CHECK_INTERVAL = 20; // in seconds
	const char SAM_HANDSHAKE[] = "HELLO VERSION";
	const char SAM_HANDSHAKE_REPLY[] = "HELLO REPLY RESULT=OK VERSION=%s\n";
	const char SAM_HANDSHAKE_I2P_ERROR[] = "HELLO REPLY RESULT=I2P_ERROR\n";
	const char SAM_SESSION_CREATE[] = "SESSION CREATE";
	const char SAM_SESSION_CREATE_REPLY_OK[] = "SESSION STATUS RESULT=OK DESTINATION=%s\n";
	const char SAM_SESSION_CREATE_DUPLICATED_ID[] = "SESSION STATUS RESULT=DUPLICATED_ID\n";
	const char SAM_SESSION_CREATE_DUPLICATED_DEST[] = "SESSION STATUS RESULT=DUPLICATED_DEST\n";
	const char SAM_SESSION_STATUS_INVALID_KEY[] = "SESSION STATUS RESULT=INVALID_KEY\n";
	const char SAM_SESSION_STATUS_I2P_ERROR[] = "SESSION STATUS RESULT=I2P_ERROR MESSAGE=%s\n";
	const char SAM_STREAM_CONNECT[] = "STREAM CONNECT";
	const char SAM_STREAM_STATUS_OK[] = "STREAM STATUS RESULT=OK\n";
	const char SAM_STREAM_STATUS_INVALID_ID[] = "STREAM STATUS RESULT=INVALID_ID\n";
	const char SAM_STREAM_STATUS_CANT_REACH_PEER[] = "STREAM STATUS RESULT=CANT_REACH_PEER\n";
	const char SAM_STREAM_STATUS_I2P_ERROR[] = "STREAM STATUS RESULT=I2P_ERROR\n";
	const char SAM_STREAM_ACCEPT[] = "STREAM ACCEPT";
	const char SAM_DATAGRAM_SEND[] = "DATAGRAM SEND";
	const char SAM_DEST_GENERATE[] = "DEST GENERATE";
	const char SAM_DEST_REPLY[] = "DEST REPLY PUB=%s PRIV=%s\n";
	const char SAM_DEST_REPLY_I2P_ERROR[] = "DEST REPLY RESULT=I2P_ERROR\n";
	const char SAM_NAMING_LOOKUP[] = "NAMING LOOKUP";
	const char SAM_NAMING_REPLY[] = "NAMING REPLY RESULT=OK NAME=ME VALUE=%s\n";
	const char SAM_DATAGRAM_RECEIVED[] = "DATAGRAM RECEIVED DESTINATION=%s SIZE=%lu\n";
	const char SAM_NAMING_REPLY_INVALID_KEY[] = "NAMING REPLY RESULT=INVALID_KEY NAME=%s\n";
	const char SAM_NAMING_REPLY_KEY_NOT_FOUND[] = "NAMING REPLY RESULT=INVALID_KEY_NOT_FOUND NAME=%s\n";
	const char SAM_PARAM_MIN[] = "MIN";
	const char SAM_PARAM_MAX[] = "MAX";
	const char SAM_PARAM_STYLE[] = "STYLE";
	const char SAM_PARAM_ID[] = "ID";
	const char SAM_PARAM_SILENT[] = "SILENT";
	const char SAM_PARAM_DESTINATION[] = "DESTINATION";
	const char SAM_PARAM_NAME[] = "NAME";
	const char SAM_PARAM_SIGNATURE_TYPE[] = "SIGNATURE_TYPE";
	const char SAM_PARAM_SIZE[] = "SIZE";
	const char SAM_VALUE_TRANSIENT[] = "TRANSIENT";
	const char SAM_VALUE_STREAM[] = "STREAM";
	const char SAM_VALUE_DATAGRAM[] = "DATAGRAM";
	const char SAM_VALUE_RAW[] = "RAW";
	const char SAM_VALUE_TRUE[] = "true";
	const char SAM_VALUE_FALSE[] = "false";
	const char SAM_VALUE_HOST[] = "HOST";
	const char SAM_VALUE_PORT[] = "PORT";

	enum SAMSocketType
	{
		eSAMSocketTypeUnknown,
		eSAMSocketTypeSession,
		eSAMSocketTypeStream,
		eSAMSocketTypeAcceptor,
		eSAMSocketTypeTerminated
	};

	class SAMBridge;
	struct SAMSession;
	class SAMSocket: public std::enable_shared_from_this<SAMSocket>
	{
		public:

			SAMSocket (SAMBridge& owner);
			~SAMSocket ();			
			void CloseStream (const char* reason); // TODO: implement it better

			boost::asio::ip::tcp::socket& GetSocket () { return m_Socket; };
			void ReceiveHandshake ();
			void SetSocketType (SAMSocketType socketType) { m_SocketType = socketType; };
			SAMSocketType GetSocketType () const { return m_SocketType; };

                        void Terminate (const char* reason);

                private:

			void HandleHandshakeReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred);
			void HandleHandshakeReplySent (const boost::system::error_code& ecode, std::size_t bytes_transferred);
			void HandleMessage (const boost::system::error_code& ecode, std::size_t bytes_transferred);
			void SendMessageReply (const char * msg, size_t len, bool close);
			void HandleMessageReplySent (const boost::system::error_code& ecode, std::size_t bytes_transferred, bool close);
			void Receive ();
			void HandleReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred);

			void I2PReceive ();
			void HandleI2PReceive (const boost::system::error_code& ecode, std::size_t bytes_transferred);
			void HandleI2PAccept (std::shared_ptr<i2p::stream::Stream> stream);
			void HandleWriteI2PData (const boost::system::error_code& ecode);
			void HandleI2PDatagramReceive (const i2p::data::IdentityEx& from, uint16_t fromPort, uint16_t toPort, const uint8_t * buf, size_t len);

			void ProcessSessionCreate (char * buf, size_t len);
			void ProcessStreamConnect (char * buf, size_t len, size_t rem);
			void ProcessStreamAccept (char * buf, size_t len);
			void ProcessDestGenerate (char * buf, size_t len);
			void ProcessNamingLookup (char * buf, size_t len);
			void SendI2PError(const std::string & msg);
			size_t ProcessDatagramSend (char * buf, size_t len, const char * data); // from SAM 1.0
			void ExtractParams (char * buf, std::map<std::string, std::string>& params);

			void Connect (std::shared_ptr<const i2p::data::LeaseSet> remote);
			void HandleConnectLeaseSetRequestComplete (std::shared_ptr<i2p::data::LeaseSet> leaseSet);
			void SendNamingLookupReply (std::shared_ptr<const i2p::data::IdentityEx> identity);
			void HandleNamingLookupLeaseSetRequestComplete (std::shared_ptr<i2p::data::LeaseSet> leaseSet, i2p::data::IdentHash ident);
			void HandleSessionReadinessCheckTimer (const boost::system::error_code& ecode);
			void SendSessionCreateReplyOk ();

		private:

			SAMBridge& m_Owner;
			boost::asio::ip::tcp::socket m_Socket;
			boost::asio::deadline_timer m_Timer;
			char m_Buffer[SAM_SOCKET_BUFFER_SIZE + 1];
			size_t m_BufferOffset;
			uint8_t m_StreamBuffer[SAM_SOCKET_BUFFER_SIZE];
			SAMSocketType m_SocketType;
			std::string m_ID; // nickname
			bool m_IsSilent;
			bool m_IsAccepting; // for eSAMSocketTypeAcceptor only 
			std::shared_ptr<i2p::stream::Stream> m_Stream;
			std::shared_ptr<SAMSession> m_Session;
	};

	struct SAMSession
	{
		std::shared_ptr<ClientDestination> localDestination;
		std::list<std::shared_ptr<SAMSocket> > m_Sockets;
		std::shared_ptr<boost::asio::ip::udp::endpoint> UDPEndpoint;
		std::mutex m_SocketsMutex;

		/** safely add a socket to this session */
		void AddSocket(const std::shared_ptr<SAMSocket> & sock) {
			std::lock_guard<std::mutex> lock(m_SocketsMutex);
			m_Sockets.push_back(sock);
		}

		/** safely remove a socket from this session */
		void DelSocket(const std::shared_ptr<SAMSocket> & sock) {
			std::lock_guard<std::mutex> lock(m_SocketsMutex);
			m_Sockets.remove(sock);
		}

		/** get a list holding a copy of all sam sockets from this session */
		std::list<std::shared_ptr<SAMSocket> > ListSockets() {
			std::list<std::shared_ptr<SAMSocket> > l;
			{
				std::lock_guard<std::mutex> lock(m_SocketsMutex);
				for(const auto& sock : m_Sockets ) l.push_back(sock);
			}
			return l;
		}

		SAMSession (std::shared_ptr<ClientDestination> dest);
		~SAMSession ();

		void CloseStreams ();
	};

	class SAMBridge
	{
		public:

			SAMBridge (const std::string& address, int port);
			~SAMBridge ();

			void Start ();
			void Stop ();

			boost::asio::io_service& GetService () { return m_Service; };
			std::shared_ptr<SAMSession> CreateSession (const std::string& id, const std::string& destination, // empty string  means transient
				const std::map<std::string, std::string> * params);
			void CloseSession (const std::string& id);
			std::shared_ptr<SAMSession> FindSession (const std::string& id) const;

			/** send raw data to remote endpoint from our UDP Socket */
			void SendTo(const uint8_t * buf, size_t len, std::shared_ptr<boost::asio::ip::udp::endpoint> remote);

		private:

			void Run ();

			void Accept ();
			void HandleAccept(const boost::system::error_code& ecode, std::shared_ptr<SAMSocket> socket);

			void ReceiveDatagram ();
			void HandleReceivedDatagram (const boost::system::error_code& ecode, std::size_t bytes_transferred);

		private:

			bool m_IsRunning;
			std::thread * m_Thread;
			boost::asio::io_service m_Service;
			boost::asio::ip::tcp::acceptor m_Acceptor;
			boost::asio::ip::udp::endpoint m_DatagramEndpoint, m_SenderEndpoint;
			boost::asio::ip::udp::socket m_DatagramSocket;
			mutable std::mutex m_SessionsMutex;
			std::map<std::string, std::shared_ptr<SAMSession> > m_Sessions;
			uint8_t m_DatagramReceiveBuffer[i2p::datagram::MAX_DATAGRAM_SIZE+1];

		public:

			// for HTTP
			const decltype(m_Sessions)& GetSessions () const { return m_Sessions; };
	};
}
}

#endif
