#ifndef NETDB_H__
#define NETDB_H__
// this file is called NetDb.hpp to resolve conflict with libc's netdb.h on case insensitive fs
#include <inttypes.h>
#include <set>
#include <map>
#include <list>
#include <string>
#include <thread>
#include <mutex>

#include "Base.h"
#include "Gzip.h"
#include "FS.h"
#include "Queue.h"
#include "I2NPProtocol.h"
#include "RouterInfo.h"
#include "LeaseSet.h"
#include "Tunnel.h"
#include "TunnelPool.h"
#include "Reseed.h"
#include "NetDbRequests.h"
#include "Family.h"

namespace i2p
{
namespace data
{		
	const int NETDB_MIN_ROUTERS = 90;
	const int NETDB_FLOODFILL_EXPIRATION_TIMEOUT = 60*60; // 1 hour, in seconds
	const int NETDB_INTRODUCEE_EXPIRATION_TIMEOUT = 65*60;
	const int NETDB_MIN_EXPIRATION_TIMEOUT = 90*60; // 1.5 hours
	const int NETDB_MAX_EXPIRATION_TIMEOUT = 27*60*60; // 27 hours
	const int NETDB_PUBLISH_INTERVAL = 60*40;

	/** function for visiting a leaseset stored in a floodfill */
	typedef std::function<void(const IdentHash, std::shared_ptr<LeaseSet>)> LeaseSetVisitor;

	/** function for visiting a router info we have locally */
	typedef std::function<void(std::shared_ptr<const i2p::data::RouterInfo>)> RouterInfoVisitor; 

	/** function for visiting a router info and determining if we want to use it */
	typedef std::function<bool(std::shared_ptr<const i2p::data::RouterInfo>)> RouterInfoFilter;
  
	class NetDb
	{
		public:

			NetDb ();
			~NetDb ();

			void Start ();
			void Stop ();
    
			bool AddRouterInfo (const uint8_t * buf, int len);
			bool AddRouterInfo (const IdentHash& ident, const uint8_t * buf, int len);
			bool AddLeaseSet (const IdentHash& ident, const uint8_t * buf, int len, std::shared_ptr<i2p::tunnel::InboundTunnel> from);
			std::shared_ptr<RouterInfo> FindRouter (const IdentHash& ident) const;
			std::shared_ptr<LeaseSet> FindLeaseSet (const IdentHash& destination) const;
			std::shared_ptr<RouterProfile> FindRouterProfile (const IdentHash& ident) const;

			void RequestDestination (const IdentHash& destination, RequestedDestination::RequestComplete requestComplete = nullptr);			
		void RequestDestinationFrom (const IdentHash& destination, const IdentHash & from, bool exploritory, RequestedDestination::RequestComplete requestComplete = nullptr);			
			
			void HandleDatabaseStoreMsg (std::shared_ptr<const I2NPMessage> msg);
			void HandleDatabaseSearchReplyMsg (std::shared_ptr<const I2NPMessage> msg);
			void HandleDatabaseLookupMsg (std::shared_ptr<const I2NPMessage> msg);			

			std::shared_ptr<const RouterInfo> GetRandomRouter () const;
			std::shared_ptr<const RouterInfo> GetRandomRouter (std::shared_ptr<const RouterInfo> compatibleWith) const;
			std::shared_ptr<const RouterInfo> GetHighBandwidthRandomRouter (std::shared_ptr<const RouterInfo> compatibleWith) const;
			std::shared_ptr<const RouterInfo> GetRandomPeerTestRouter (bool v4only = true) const;
			std::shared_ptr<const RouterInfo> GetRandomIntroducer () const;
			std::shared_ptr<const RouterInfo> GetClosestFloodfill (const IdentHash& destination, const std::set<IdentHash>& excluded, bool closeThanUsOnly = false) const;
			std::vector<IdentHash> GetClosestFloodfills (const IdentHash& destination, size_t num,
				std::set<IdentHash>& excluded, bool closeThanUsOnly = false) const;
			std::shared_ptr<const RouterInfo> GetClosestNonFloodfill (const IdentHash& destination, const std::set<IdentHash>& excluded) const;
      std::shared_ptr<const RouterInfo> GetRandomRouterInFamily(const std::string & fam) const;
			void SetUnreachable (const IdentHash& ident, bool unreachable);			

			void PostI2NPMsg (std::shared_ptr<const I2NPMessage> msg);

      /** set hidden mode, aka don't publish our RI to netdb and don't explore */
      void SetHidden(bool hide); 
      
			void Reseed ();
			Families& GetFamilies () { return m_Families; };

			// for web interface
			int GetNumRouters () const { return m_RouterInfos.size (); };
			int GetNumFloodfills () const { return m_Floodfills.size (); };
			int GetNumLeaseSets () const { return m_LeaseSets.size (); };

			/** visit all lease sets we currently store */
			void VisitLeaseSets(LeaseSetVisitor v);
			/** visit all router infos we have currently on disk, usually insanely expensive, does not access in memory RI */
			void VisitStoredRouterInfos(RouterInfoVisitor v);
			/** visit all router infos we have loaded in memory, cheaper than VisitLocalRouterInfos but locks access while visiting */
			void VisitRouterInfos(RouterInfoVisitor v);
			/** visit N random router that match using filter, then visit them with a visitor, return number of RouterInfos that were visited */
			size_t VisitRandomRouterInfos(RouterInfoFilter f, RouterInfoVisitor v, size_t n);

			void ClearRouterInfos () { m_RouterInfos.clear (); };

		private:

			void Load ();
			bool LoadRouterInfo (const std::string & path);
			void SaveUpdated ();
			void Run (); // exploratory thread
			void Explore (int numDestinations);	
			void Publish ();
			void ManageLeaseSets ();
			void ManageRequests ();

		void ReseedFromFloodfill(const RouterInfo & ri, int numRouters=40, int numFloodfills=20);
		
    	template<typename Filter>
        std::shared_ptr<const RouterInfo> GetRandomRouter (Filter filter) const;	
		
		private:
		
			mutable std::mutex m_LeaseSetsMutex;
			std::map<IdentHash, std::shared_ptr<LeaseSet> > m_LeaseSets;
			mutable std::mutex m_RouterInfosMutex;
			std::map<IdentHash, std::shared_ptr<RouterInfo> > m_RouterInfos;
			mutable std::mutex m_FloodfillsMutex;
			std::list<std::shared_ptr<RouterInfo> > m_Floodfills;
			
			bool m_IsRunning;
			uint64_t m_LastLoad;
			std::thread * m_Thread;	
			i2p::util::Queue<std::shared_ptr<const I2NPMessage> > m_Queue; // of I2NPDatabaseStoreMsg

			GzipInflator m_Inflator;
			Reseeder * m_Reseeder;
			Families m_Families;
			i2p::fs::HashedStorage m_Storage;

			friend class NetDbRequests; 
			NetDbRequests m_Requests;

		/** router info we are bootstrapping from or nullptr if we are not currently doing that*/
		std::shared_ptr<RouterInfo> m_FloodfillBootstrap;
				

      /** true if in hidden mode */
      bool m_HiddenMode;
	};

	extern NetDb netdb;
}
}

#endif
