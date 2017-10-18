#include "Log.h"
#include "Crypto.h"
#include "RouterContext.h"
#include "I2NPProtocol.h"
#include "NetDb.hpp"
#include "Transports.h"
#include "Config.h"
#include "HTTP.h"
#ifdef WITH_EVENTS
#include "Event.h"
#include "util.h"
#endif

using namespace i2p::data;

namespace i2p
{
namespace transport
{
	DHKeysPairSupplier::DHKeysPairSupplier (int size):
		m_QueueSize (size), m_IsRunning (false), m_Thread (nullptr)
	{
	}

	DHKeysPairSupplier::~DHKeysPairSupplier ()
	{
		Stop ();
	}

	void DHKeysPairSupplier::Start ()
	{
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&DHKeysPairSupplier::Run, this));
	}

	void DHKeysPairSupplier::Stop ()
	{
		m_IsRunning = false;
		m_Acquired.notify_one ();
		if (m_Thread)
		{
			m_Thread->join ();
			delete m_Thread;
			m_Thread = 0;
		}
	}

	void DHKeysPairSupplier::Run ()
	{
		while (m_IsRunning)
		{
			int num, total = 0;
			while ((num = m_QueueSize - (int)m_Queue.size ()) > 0 && total < 20)
			{
				CreateDHKeysPairs (num);
				total += num;
			}
			if (total >= 20)
			{
				LogPrint (eLogWarning, "Transports: ", total, " DH keys generated at the time");
				std::this_thread::sleep_for (std::chrono::seconds(1)); // take a break
			}
			else
			{
				std::unique_lock<std::mutex>	l(m_AcquiredMutex);
				m_Acquired.wait (l); // wait for element gets aquired
			}
		}
	}

	void DHKeysPairSupplier::CreateDHKeysPairs (int num)
	{
		if (num > 0)
		{
			for (int i = 0; i < num; i++)
			{
				auto pair = std::make_shared<i2p::crypto::DHKeys> ();
				pair->GenerateKeys ();
				std::unique_lock<std::mutex>	l(m_AcquiredMutex);
				m_Queue.push (pair);
			}
		}
	}

	std::shared_ptr<i2p::crypto::DHKeys> DHKeysPairSupplier::Acquire ()
	{
		{
			std::unique_lock<std::mutex>	l(m_AcquiredMutex);
			if (!m_Queue.empty ())
			{
				auto pair = m_Queue.front ();
				m_Queue.pop ();
				m_Acquired.notify_one ();
				return pair;
			}
		}
		// queue is empty, create new
		auto pair = std::make_shared<i2p::crypto::DHKeys> ();
		pair->GenerateKeys ();
		return pair;
	}

	void DHKeysPairSupplier::Return (std::shared_ptr<i2p::crypto::DHKeys> pair)
	{
		if (pair)
		{
			std::unique_lock<std::mutex>l(m_AcquiredMutex);
			if ((int)m_Queue.size () < 2*m_QueueSize)
				m_Queue.push (pair);
		}
		else
			LogPrint(eLogError, "Transports: return null DHKeys");
	}

	Transports transports;

	Transports::Transports ():
		m_IsOnline (true), m_IsRunning (false), m_IsNAT (true), m_Thread (nullptr), m_Service (nullptr),
		m_Work (nullptr), m_PeerCleanupTimer (nullptr), m_PeerTestTimer (nullptr),
		m_NTCPServer (nullptr), m_SSUServer (nullptr), m_DHKeysPairSupplier (5), // 5 pre-generated keys
		m_TotalSentBytes(0), m_TotalReceivedBytes(0), m_TotalTransitTransmittedBytes (0),
 		m_InBandwidth (0), m_OutBandwidth (0), m_TransitBandwidth(0),
		m_LastInBandwidthUpdateBytes (0), m_LastOutBandwidthUpdateBytes (0),
		m_LastTransitBandwidthUpdateBytes (0), m_LastBandwidthUpdateTime (0)
	{
	}

	Transports::~Transports ()
	{
		Stop ();
		if (m_Service)
		{
			delete m_PeerCleanupTimer; m_PeerCleanupTimer = nullptr;
			delete m_PeerTestTimer; m_PeerTestTimer = nullptr;
			delete m_Work; m_Work = nullptr;
			delete m_Service; m_Service = nullptr;
		}
	}

	void Transports::Start (bool enableNTCP, bool enableSSU)
	{
		if (!m_Service)
		{
			m_Service = new boost::asio::io_service ();
			m_Work = new boost::asio::io_service::work (*m_Service);
			m_PeerCleanupTimer = new boost::asio::deadline_timer (*m_Service);
	 		m_PeerTestTimer = new boost::asio::deadline_timer (*m_Service);
		}

                i2p::config::GetOption("nat", m_IsNAT);

		m_DHKeysPairSupplier.Start ();
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&Transports::Run, this));
		std::string ntcpproxy; i2p::config::GetOption("ntcpproxy", ntcpproxy);
		i2p::http::URL proxyurl;
		if(ntcpproxy.size() && enableNTCP)
		{
			if(proxyurl.parse(ntcpproxy))
			{
				if(proxyurl.schema == "socks" || proxyurl.schema == "http")
				{
					m_NTCPServer = new NTCPServer();

					NTCPServer::ProxyType proxytype = NTCPServer::eSocksProxy;

					if (proxyurl.schema == "http")
						proxytype = NTCPServer::eHTTPProxy;

					m_NTCPServer->UseProxy(proxytype, proxyurl.host, proxyurl.port) ;
					m_NTCPServer->Start();
					if(!m_NTCPServer->NetworkIsReady())
					{
						LogPrint(eLogError, "Transports: NTCP failed to start with proxy");
						m_NTCPServer->Stop();
						delete m_NTCPServer;
						m_NTCPServer = nullptr;
					}
				}
				else
					LogPrint(eLogError, "Transports: unsupported NTCP proxy URL ", ntcpproxy);
			}
			else
				LogPrint(eLogError, "Transports: invalid NTCP proxy url ", ntcpproxy);
			return;
		}

		// create acceptors
		auto& addresses = context.GetRouterInfo ().GetAddresses ();
		for (const auto& address : addresses)
		{
			if (!address) continue;
			if (m_NTCPServer == nullptr && enableNTCP)
			{
				m_NTCPServer = new NTCPServer ();
				m_NTCPServer->Start ();
				if (!(m_NTCPServer->IsBoundV6() || m_NTCPServer->IsBoundV4())) {
					/** failed to bind to NTCP */
					LogPrint(eLogError, "Transports: failed to bind to TCP");
					m_NTCPServer->Stop();
					delete m_NTCPServer;
					m_NTCPServer = nullptr;
				}
			}

			if (address->transportStyle == RouterInfo::eTransportSSU)
			{
				if (m_SSUServer == nullptr && enableSSU)
				{
					if (address->host.is_v4())
						m_SSUServer = new SSUServer (address->port);
					else
						m_SSUServer = new SSUServer (address->host, address->port);
					LogPrint (eLogInfo, "Transports: Start listening UDP port ", address->port);
					try {
						m_SSUServer->Start ();
					} catch ( std::exception & ex ) {
						LogPrint(eLogError, "Transports: Failed to bind to UDP port", address->port);
						delete m_SSUServer;
						m_SSUServer = nullptr;
						continue;
					}
					DetectExternalIP ();
				}
				else
					LogPrint (eLogError, "Transports: SSU server already exists");
			}
		}
		m_PeerCleanupTimer->expires_from_now (boost::posix_time::seconds(5*SESSION_CREATION_TIMEOUT));
		m_PeerCleanupTimer->async_wait (std::bind (&Transports::HandlePeerCleanupTimer, this, std::placeholders::_1));

                if (m_IsNAT)
                {
                    m_PeerTestTimer->expires_from_now (boost::posix_time::minutes(PEER_TEST_INTERVAL));
                    m_PeerTestTimer->async_wait (std::bind (&Transports::HandlePeerTestTimer, this, std::placeholders::_1));
                }
	}

	void Transports::Stop ()
	{
		if (m_PeerCleanupTimer) m_PeerCleanupTimer->cancel ();
		if (m_PeerTestTimer) m_PeerTestTimer->cancel ();
		m_Peers.clear ();
		if (m_SSUServer)
		{
			m_SSUServer->Stop ();
			delete m_SSUServer;
			m_SSUServer = nullptr;
		}
		if (m_NTCPServer)
		{
			m_NTCPServer->Stop ();
			delete m_NTCPServer;
			m_NTCPServer = nullptr;
		}

		m_DHKeysPairSupplier.Stop ();
		m_IsRunning = false;
		if (m_Service) m_Service->stop ();
		if (m_Thread)
		{
			m_Thread->join ();
			delete m_Thread;
			m_Thread = nullptr;
		}
	}

	void Transports::Run ()
	{
		while (m_IsRunning && m_Service)
		{
			try
			{
				m_Service->run ();
			}
			catch (std::exception& ex)
			{
				LogPrint (eLogError, "Transports: runtime exception: ", ex.what ());
			}
		}
	}

	void Transports::UpdateBandwidth ()
	{
		uint64_t ts = i2p::util::GetMillisecondsSinceEpoch ();
		if (m_LastBandwidthUpdateTime > 0)
		{
			auto delta = ts - m_LastBandwidthUpdateTime;
			if (delta > 0)
			{
				m_InBandwidth = (m_TotalReceivedBytes - m_LastInBandwidthUpdateBytes)*1000/delta; // per second
				m_OutBandwidth = (m_TotalSentBytes - m_LastOutBandwidthUpdateBytes)*1000/delta; // per second
				m_TransitBandwidth = (m_TotalTransitTransmittedBytes - m_LastTransitBandwidthUpdateBytes)*1000/delta;
			}
		}
		m_LastBandwidthUpdateTime = ts;
		m_LastInBandwidthUpdateBytes = m_TotalReceivedBytes;
		m_LastOutBandwidthUpdateBytes = m_TotalSentBytes;
		m_LastTransitBandwidthUpdateBytes = m_TotalTransitTransmittedBytes;
	}

	bool Transports::IsBandwidthExceeded () const
	{
		auto limit = i2p::context.GetBandwidthLimit() * 1024; // convert to bytes
		auto bw = std::max (m_InBandwidth, m_OutBandwidth);
		return bw > limit;
	}

	bool Transports::IsTransitBandwidthExceeded () const
	{
		auto limit = i2p::context.GetTransitBandwidthLimit() * 1024; // convert to bytes
		return m_TransitBandwidth > limit;
	}

	void Transports::SendMessage (const i2p::data::IdentHash& ident, std::shared_ptr<i2p::I2NPMessage> msg)
	{
		SendMessages (ident, std::vector<std::shared_ptr<i2p::I2NPMessage> > {msg });
	}

	void Transports::SendMessages (const i2p::data::IdentHash& ident, const std::vector<std::shared_ptr<i2p::I2NPMessage> >& msgs)
	{
#ifdef WITH_EVENTS
		QueueIntEvent("transport.send", ident.ToBase64(), msgs.size());
#endif
		m_Service->post (std::bind (&Transports::PostMessages, this, ident, msgs));
	}

	void Transports::PostMessages (i2p::data::IdentHash ident, std::vector<std::shared_ptr<i2p::I2NPMessage> > msgs)
	{
		if (ident == i2p::context.GetRouterInfo ().GetIdentHash ())
		{
			// we send it to ourself
			for (auto& it: msgs)
				m_LoopbackHandler.PutNextMessage (it);
			m_LoopbackHandler.Flush ();
			return;
		}
		if(RoutesRestricted() && ! IsRestrictedPeer(ident)) return;
		auto it = m_Peers.find (ident);
		if (it == m_Peers.end ())
		{
			bool connected = false;
			try
			{
				auto r = netdb.FindRouter (ident);
				{
					std::unique_lock<std::mutex>	l(m_PeersMutex);
					it = m_Peers.insert (std::pair<i2p::data::IdentHash, Peer>(ident, { 0, r, {},
						i2p::util::GetSecondsSinceEpoch (), {} })).first;
				}
				connected = ConnectToPeer (ident, it->second);
			}
			catch (std::exception& ex)
			{
				LogPrint (eLogError, "Transports: PostMessages exception:", ex.what ());
			}
			if (!connected) return;
		}
		if (!it->second.sessions.empty ())
			it->second.sessions.front ()->SendI2NPMessages (msgs);
		else
		{
			if (it->second.delayedMessages.size () < MAX_NUM_DELAYED_MESSAGES)
			{
				for (auto& it1: msgs)
					it->second.delayedMessages.push_back (it1);
			}
			else
			{
				LogPrint (eLogWarning, "Transports: delayed messages queue size exceeds ", MAX_NUM_DELAYED_MESSAGES);
				std::unique_lock<std::mutex> l(m_PeersMutex);
				m_Peers.erase (it);
			}
		}
	}

	bool Transports::ConnectToPeer (const i2p::data::IdentHash& ident, Peer& peer)
	{
		if (peer.router) // we have RI already
		{
			if (!peer.numAttempts) // NTCP
			{
				peer.numAttempts++;
				auto address = peer.router->GetNTCPAddress (!context.SupportsV6 ());
				if (address && m_NTCPServer)
				{
#if BOOST_VERSION >= 104900
					if (!address->host.is_unspecified ()) // we have address now
#else
					boost::system::error_code ecode;
				  address->host.to_string (ecode);
					if (!ecode)
#endif
					{
						if (!peer.router->UsesIntroducer () && !peer.router->IsUnreachable ())
						{
							auto s = std::make_shared<NTCPSession> (*m_NTCPServer, peer.router);
							if(m_NTCPServer->UsingProxy())
							{
								NTCPServer::RemoteAddressType remote = NTCPServer::eIP4Address;
								std::string addr = address->host.to_string();

								if(address->host.is_v6())
									remote = NTCPServer::eIP6Address;

								m_NTCPServer->ConnectWithProxy(addr, address->port, remote, s);
							}
							else
								m_NTCPServer->Connect (address->host, address->port, s);
							return true;
						}
					}
					else // we don't have address
					{
						if (address->addressString.length () > 0) // trying to resolve
						{
							if(m_NTCPServer->UsingProxy())
							{
								auto s = std::make_shared<NTCPSession> (*m_NTCPServer, peer.router);
								m_NTCPServer->ConnectWithProxy(address->addressString, address->port, NTCPServer::eHostname, s);
							}
							else
							{
								LogPrint (eLogDebug, "Transports: Resolving NTCP ", address->addressString);
								NTCPResolve (address->addressString, ident);
							}
							return true;
						}
					}
				}
				else
					LogPrint (eLogDebug, "Transports: NTCP address is not present for ", i2p::data::GetIdentHashAbbreviation (ident), ", trying SSU");
			}
			if (peer.numAttempts == 1)// SSU
			{
				peer.numAttempts++;
				if (m_SSUServer && peer.router->IsSSU (!context.SupportsV6 ()))
				{
					auto address = peer.router->GetSSUAddress (!context.SupportsV6 ());
#if BOOST_VERSION >= 104900
					if (!address->host.is_unspecified ()) // we have address now
#else
					boost::system::error_code ecode;
					address->host.to_string (ecode);
					if (!ecode)
#endif
					{
						m_SSUServer->CreateSession (peer.router, address->host, address->port);
						return true;
					}
					else // we don't have address
					{
						if (address->addressString.length () > 0) // trying to resolve
						{
							LogPrint (eLogDebug, "Transports: Resolving SSU ", address->addressString);
							SSUResolve (address->addressString, ident);
							return true;
						}
					}
				}
			}
			LogPrint (eLogInfo, "Transports: No NTCP or SSU addresses available");
			peer.Done ();
			std::unique_lock<std::mutex> l(m_PeersMutex);
			m_Peers.erase (ident);
			return false;
		}
		else // otherwise request RI
		{
			LogPrint (eLogInfo, "Transports: RouterInfo for ", ident.ToBase64 (), " not found, requested");
			i2p::data::netdb.RequestDestination (ident, std::bind (
				&Transports::RequestComplete, this, std::placeholders::_1, ident));
		}
		return true;
	}

	void Transports::RequestComplete (std::shared_ptr<const i2p::data::RouterInfo> r, const i2p::data::IdentHash& ident)
	{
		m_Service->post (std::bind (&Transports::HandleRequestComplete, this, r, ident));
	}

	void Transports::HandleRequestComplete (std::shared_ptr<const i2p::data::RouterInfo> r, i2p::data::IdentHash ident)
	{
		auto it = m_Peers.find (ident);
		if (it != m_Peers.end ())
		{
			if (r)
			{
				LogPrint (eLogDebug, "Transports: RouterInfo for ", ident.ToBase64 (), " found, Trying to connect");
				it->second.router = r;
				ConnectToPeer (ident, it->second);
			}
			else
			{
				LogPrint (eLogWarning, "Transports: RouterInfo not found, Failed to send messages");
				std::unique_lock<std::mutex> l(m_PeersMutex);
				m_Peers.erase (it);
			}
		}
	}

	void Transports::NTCPResolve (const std::string& addr, const i2p::data::IdentHash& ident)
	{
		auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*m_Service);
		resolver->async_resolve (boost::asio::ip::tcp::resolver::query (addr, ""),
			std::bind (&Transports::HandleNTCPResolve, this,
				std::placeholders::_1, std::placeholders::_2, ident, resolver));
	}

	void Transports::HandleNTCPResolve (const boost::system::error_code& ecode, boost::asio::ip::tcp::resolver::iterator it,
		i2p::data::IdentHash ident, std::shared_ptr<boost::asio::ip::tcp::resolver> resolver)
	{
		auto it1 = m_Peers.find (ident);
		if (it1 != m_Peers.end ())
		{
			auto& peer = it1->second;
			if (!ecode && peer.router)
			{
				while (it != boost::asio::ip::tcp::resolver::iterator())
				{
					auto address = (*it).endpoint ().address ();
					LogPrint (eLogDebug, "Transports: ", (*it).host_name (), " has been resolved to ", address);
					if (address.is_v4 () || context.SupportsV6 ())
					{
						auto addr = peer.router->GetNTCPAddress (); // TODO: take one we requested
						if (addr)
						{
							auto s = std::make_shared<NTCPSession> (*m_NTCPServer, peer.router);
							m_NTCPServer->Connect (address, addr->port, s);
							return;
						}
						break;
					}
					else
						LogPrint (eLogInfo, "Transports: NTCP ", address, " is not supported");
					it++;
				}
			}
			LogPrint (eLogError, "Transports: Unable to resolve NTCP address: ", ecode.message ());
			std::unique_lock<std::mutex> l(m_PeersMutex);
			m_Peers.erase (it1);
		}
	}

	void Transports::SSUResolve (const std::string& addr, const i2p::data::IdentHash& ident)
	{
		auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*m_Service);
		resolver->async_resolve (boost::asio::ip::tcp::resolver::query (addr, ""),
			std::bind (&Transports::HandleSSUResolve, this,
				std::placeholders::_1, std::placeholders::_2, ident, resolver));
	}

	void Transports::HandleSSUResolve (const boost::system::error_code& ecode, boost::asio::ip::tcp::resolver::iterator it,
		i2p::data::IdentHash ident, std::shared_ptr<boost::asio::ip::tcp::resolver> resolver)
	{
		auto it1 = m_Peers.find (ident);
		if (it1 != m_Peers.end ())
		{
			auto& peer = it1->second;
			if (!ecode && peer.router)
			{
				while (it != boost::asio::ip::tcp::resolver::iterator())
				{
					auto address = (*it).endpoint ().address ();
					LogPrint (eLogDebug, "Transports: ", (*it).host_name (), " has been resolved to ", address);
					if (address.is_v4 () || context.SupportsV6 ())
					{
						auto addr = peer.router->GetSSUAddress (); // TODO: take one we requested
						if (addr)
						{
							m_SSUServer->CreateSession (peer.router, address, addr->port);
							return;
						}
						break;
					}
					else
						LogPrint (eLogInfo, "Transports: SSU ", address, " is not supported");
					it++;
				}
			}
			LogPrint (eLogError, "Transports: Unable to resolve SSU address: ", ecode.message ());
			std::unique_lock<std::mutex> l(m_PeersMutex);
			m_Peers.erase (it1);
		}
	}

	void Transports::CloseSession (std::shared_ptr<const i2p::data::RouterInfo> router)
	{
		if (!router) return;
		m_Service->post (std::bind (&Transports::PostCloseSession, this, router));
	}

	void Transports::PostCloseSession (std::shared_ptr<const i2p::data::RouterInfo> router)
	{
		auto ssuSession = m_SSUServer ? m_SSUServer->FindSession (router) : nullptr;
		if (ssuSession) // try SSU first
		{
			m_SSUServer->DeleteSession (ssuSession);
			LogPrint (eLogDebug, "Transports: SSU session closed");
		}
		auto ntcpSession = m_NTCPServer ? m_NTCPServer->FindNTCPSession(router->GetIdentHash()) : nullptr;
		if (ntcpSession) // try deleting ntcp session too
		{
			ntcpSession->Terminate ();
			LogPrint(eLogDebug, "Transports: NTCP session closed");
		}
	}

	void Transports::DetectExternalIP ()
	{
		if (RoutesRestricted())
  		{
			LogPrint(eLogInfo, "Transports: restricted routes enabled, not detecting ip");
			i2p::context.SetStatus (eRouterStatusOK);
			return;
		}
		if (m_SSUServer)
		{
			bool isv4 = i2p::context.SupportsV4 ();
			if (m_IsNAT && isv4)
				i2p::context.SetStatus (eRouterStatusTesting);
			for (int i = 0; i < 5; i++)
			{
				auto router = i2p::data::netdb.GetRandomPeerTestRouter (isv4); // v4 only if v4
				if (router)
					m_SSUServer->CreateSession (router, true, isv4);	// peer test
				else
				{
					// if not peer test capable routers found pick any
					router = i2p::data::netdb.GetRandomRouter ();
					if (router && router->IsSSU ())
						m_SSUServer->CreateSession (router);		// no peer test
				}
			}
		}
		else
			LogPrint (eLogError, "Transports: Can't detect external IP. SSU is not available");
	}

	void Transports::PeerTest ()
	{
		if (RoutesRestricted() || !i2p::context.SupportsV4 ()) return;
		if (m_SSUServer)
		{
			bool statusChanged = false;
			for (int i = 0; i < 5; i++)
			{
				auto router = i2p::data::netdb.GetRandomPeerTestRouter (true); // v4 only
				if (router)
				{
					if (!statusChanged)
					{
						statusChanged = true;
						i2p::context.SetStatus (eRouterStatusTesting); // first time only
					}
					m_SSUServer->CreateSession (router, true, true); // peer test v4
				}
			}
			if (!statusChanged)
				LogPrint (eLogWarning, "Can't find routers for peer test");
		}
	}

	std::shared_ptr<i2p::crypto::DHKeys> Transports::GetNextDHKeysPair ()
	{
		return m_DHKeysPairSupplier.Acquire ();
	}

	void Transports::ReuseDHKeysPair (std::shared_ptr<i2p::crypto::DHKeys> pair)
	{
		m_DHKeysPairSupplier.Return (pair);
	}

	void Transports::PeerConnected (std::shared_ptr<TransportSession> session)
	{
		m_Service->post([session, this]()
		{
			auto remoteIdentity = session->GetRemoteIdentity ();
			if (!remoteIdentity) return;
			auto ident = remoteIdentity->GetIdentHash ();
			auto it = m_Peers.find (ident);
			if (it != m_Peers.end ())
			{
#ifdef WITH_EVENTS
				EmitEvent({{"type" , "transport.connected"}, {"ident", ident.ToBase64()}, {"inbound", "false"}});
#endif
				bool sendDatabaseStore = true;
				if (it->second.delayedMessages.size () > 0)
				{
					// check if first message is our DatabaseStore (publishing)
					auto firstMsg = it->second.delayedMessages[0];
					if (firstMsg && firstMsg->GetTypeID () == eI2NPDatabaseStore &&
							i2p::data::IdentHash(firstMsg->GetPayload () + DATABASE_STORE_KEY_OFFSET) == i2p::context.GetIdentHash ())
						sendDatabaseStore = false; // we have it in the list already
				}
				if (sendDatabaseStore)
					session->SendI2NPMessages ({ CreateDatabaseStoreMsg () });
				else
					session->SetTerminationTimeout (10); // most likely it's publishing, no follow-up messages expected, set timeout to 10 seconds
				it->second.sessions.push_back (session);
				session->SendI2NPMessages (it->second.delayedMessages);
				it->second.delayedMessages.clear ();
			}
			else // incoming connection
			{
				if(RoutesRestricted() && ! IsRestrictedPeer(ident)) {
					// not trusted
					LogPrint(eLogWarning, "Transports: closing untrusted inbound connection from ", ident.ToBase64());
					session->Done();
					return;
				}
#ifdef WITH_EVENTS
				EmitEvent({{"type" , "transport.connected"}, {"ident", ident.ToBase64()}, {"inbound", "true"}});
#endif
				session->SendI2NPMessages ({ CreateDatabaseStoreMsg () }); // send DatabaseStore
				std::unique_lock<std::mutex>	l(m_PeersMutex);
				m_Peers.insert (std::make_pair (ident, Peer{ 0, nullptr, { session }, i2p::util::GetSecondsSinceEpoch (), {} }));
			}
		});
	}

	void Transports::PeerDisconnected (std::shared_ptr<TransportSession> session)
	{
		m_Service->post([session, this]()
		{
			auto remoteIdentity = session->GetRemoteIdentity ();
			if (!remoteIdentity) return;
			auto ident = remoteIdentity->GetIdentHash ();
#ifdef WITH_EVENTS
			EmitEvent({{"type" , "transport.disconnected"}, {"ident", ident.ToBase64()}});
#endif
			auto it = m_Peers.find (ident);
			if (it != m_Peers.end ())
			{
				it->second.sessions.remove (session);
				if (it->second.sessions.empty ()) // TODO: why?
				{
					if (it->second.delayedMessages.size () > 0)
						ConnectToPeer (ident, it->second);
					else
					{
						std::unique_lock<std::mutex> l(m_PeersMutex);
						m_Peers.erase (it);
					}
				}
			}
		});
	}

	bool Transports::IsConnected (const i2p::data::IdentHash& ident) const
	{
		std::unique_lock<std::mutex> l(m_PeersMutex);
		auto it = m_Peers.find (ident);
		return it != m_Peers.end ();
	}

	void Transports::HandlePeerCleanupTimer (const boost::system::error_code& ecode)
	{
		if (ecode != boost::asio::error::operation_aborted)
		{
			auto ts = i2p::util::GetSecondsSinceEpoch ();
			for (auto it = m_Peers.begin (); it != m_Peers.end (); )
			{
				if (it->second.sessions.empty () && ts > it->second.creationTime + SESSION_CREATION_TIMEOUT)
				{
					LogPrint (eLogWarning, "Transports: Session to peer ", it->first.ToBase64 (), " has not been created in ", SESSION_CREATION_TIMEOUT, " seconds");
					auto profile = i2p::data::GetRouterProfile(it->first);
					if (profile)
					{
						profile->TunnelNonReplied();
						profile->Save(it->first);
					}
					std::unique_lock<std::mutex>	l(m_PeersMutex);
					it = m_Peers.erase (it);
				}
				else
					++it;
			}
			UpdateBandwidth (); // TODO: use separate timer(s) for it
			if (i2p::context.GetStatus () == eRouterStatusTesting) // if still testing,	 repeat peer test
				DetectExternalIP ();
			m_PeerCleanupTimer->expires_from_now (boost::posix_time::seconds(5*SESSION_CREATION_TIMEOUT));
			m_PeerCleanupTimer->async_wait (std::bind (&Transports::HandlePeerCleanupTimer, this, std::placeholders::_1));
		}
	}

	void Transports::HandlePeerTestTimer (const boost::system::error_code& ecode)
	{
		if (ecode != boost::asio::error::operation_aborted)
		{
			PeerTest ();
			m_PeerTestTimer->expires_from_now (boost::posix_time::minutes(PEER_TEST_INTERVAL));
			m_PeerTestTimer->async_wait (std::bind (&Transports::HandlePeerTestTimer, this, std::placeholders::_1));
		}
	}

	std::shared_ptr<const i2p::data::RouterInfo> Transports::GetRandomPeer () const
	{
		if (m_Peers.empty ()) return nullptr;
		std::unique_lock<std::mutex> l(m_PeersMutex);
		auto it = m_Peers.begin ();
		std::advance (it, rand () % m_Peers.size ());
		return it != m_Peers.end () ? it->second.router : nullptr;
	}
	void Transports::RestrictRoutesToFamilies(std::set<std::string> families)
	{
		std::lock_guard<std::mutex> lock(m_FamilyMutex);
		m_TrustedFamilies.clear();
		for ( const auto& fam : families )
			m_TrustedFamilies.push_back(fam);
	}

	void Transports::RestrictRoutesToRouters(std::set<i2p::data::IdentHash> routers)
	{
		std::unique_lock<std::mutex> lock(m_TrustedRoutersMutex);
		m_TrustedRouters.clear();
		for (const auto & ri : routers )
			m_TrustedRouters.push_back(ri);
	}

	bool Transports::RoutesRestricted() const {
		std::unique_lock<std::mutex> famlock(m_FamilyMutex);
		std::unique_lock<std::mutex> routerslock(m_TrustedRoutersMutex);
		return m_TrustedFamilies.size() > 0 || m_TrustedRouters.size() > 0;
	}

	/** XXX: if routes are not restricted this dies */
	std::shared_ptr<const i2p::data::RouterInfo> Transports::GetRestrictedPeer() const
	{
		{
			std::lock_guard<std::mutex> l(m_FamilyMutex);
			std::string fam;
			auto sz = m_TrustedFamilies.size();
			if(sz > 1)
			{
				auto it = m_TrustedFamilies.begin ();
				std::advance(it, rand() % sz);
				fam = *it;
				boost::to_lower(fam);
			}
			else if (sz == 1)
			{
				fam = m_TrustedFamilies[0];
			}
			if (fam.size())
				return i2p::data::netdb.GetRandomRouterInFamily(fam);
		}
		{
			std::unique_lock<std::mutex> l(m_TrustedRoutersMutex);
			auto sz = m_TrustedRouters.size();
			if (sz)
			{
				if(sz == 1)
					return i2p::data::netdb.FindRouter(m_TrustedRouters[0]);
				auto it = m_TrustedRouters.begin();
				std::advance(it, rand() % sz);
				return i2p::data::netdb.FindRouter(*it);
			}
		}
		return nullptr;
	}

	bool Transports::IsRestrictedPeer(const i2p::data::IdentHash & ih) const
	{
		{
			std::unique_lock<std::mutex> l(m_TrustedRoutersMutex);
			for (const auto & r : m_TrustedRouters )
				if ( r == ih ) return true;
		}
		{
			std::unique_lock<std::mutex> l(m_FamilyMutex);
			auto ri = i2p::data::netdb.FindRouter(ih);
			for (const auto & fam : m_TrustedFamilies)
				if(ri->IsFamily(fam)) return true;
		}
		return false;
	}
}
}
