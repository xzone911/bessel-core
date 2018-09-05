//------------------------------------------------------------------------------
/*
    This file is part of skywelld: https://github.com/skywell/skywelld
    Copyright (c) 2012, 2013 Skywell Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef SKYWELL_OVERLAY_OVERLAYIMPL_H_INCLUDED
#define SKYWELL_OVERLAY_OVERLAYIMPL_H_INCLUDED

#include <network/overlay/Overlay.h>
#include <network/peerfinder/Manager.h>
#include <services/server/Handoff.h>
#include <services/server/ServerHandler.h>
#include <common/base/Resolver.h>
#include <common/base/seconds_clock.h>
#include <common/base/UnorderedContainers.h>
#include <network/resource/Manager.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/basic_waitable_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <boost/asio.hpp>
#include <common/misc/Utility.h>

namespace skywell {

class PeerImp;
class BasicConfig;

enum
{
    maxTTL = 2
};

class OverlayImpl : public Overlay
{
public:
    class Child
    {
    protected:
        OverlayImpl& overlay_;

        explicit
        Child (OverlayImpl& overlay);

        virtual ~Child();

    public:
        virtual void stop() = 0;
    };

private:
    using clock_type = std::chrono::steady_clock;
    using socket_type = boost::asio::ip::tcp::socket;
    using address_type = boost::asio::ip::address;
    using endpoint_type = boost::asio::ip::tcp::endpoint;
    using error_code = boost::system::error_code;

    struct Timer
        : Child
        , std::enable_shared_from_this<Timer>
    {
        boost::asio::basic_waitable_timer <clock_type> timer_;

        explicit
        Timer (OverlayImpl& overlay);

        void
        stop() override;

        void
        run();

        void
        on_timer (error_code ec);
    };

    boost::asio::io_service& io_service_;
    boost::optional<boost::asio::io_service::work> work_;
    boost::asio::io_service::strand strand_;

    std::recursive_mutex mutex_; //  use std::mutex
    std::condition_variable_any cond_;
    std::weak_ptr<Timer> timer_;
    boost::container::flat_map<
        Child*, std::weak_ptr<Child>> list_;

    Setup setup_;
    beast::Journal journal_;
    ServerHandler& serverHandler_;

    Resource::Manager& m_resourceManager;

    std::unique_ptr <PeerFinder::Manager> m_peerFinder;

    hash_map <PeerFinder::Slot::ptr,
        std::weak_ptr <PeerImp>> m_peers;

    hash_map<SkywellAddress, std::weak_ptr<PeerImp>> m_publicKeyMap;

    hash_map<Peer::id_t, std::weak_ptr<PeerImp>> m_shortIdMap;

    Resolver& m_resolver;

    std::atomic <Peer::id_t> next_id_;

    int timer_count_;

    //--------------------------------------------------------------------------

public:
    OverlayImpl (Setup const& setup
               , Stoppable& parent
               , ServerHandler& serverHandler
               , Resource::Manager& resourceManager
               , Resolver& resolver
               , boost::asio::io_service& io_service
               , BasicConfig const& config);

    ~OverlayImpl();

    OverlayImpl (OverlayImpl const&) = delete;
    OverlayImpl& operator= (OverlayImpl const&) = delete;

    PeerFinder::Manager&
    peerFinder()
    {
        return *m_peerFinder;
    }

    Resource::Manager&
    resourceManager()
    {
        return m_resourceManager;
    }

    ServerHandler&
    serverHandler()
    {
        return serverHandler_;
    }

    Setup const&
    setup() const
    {
        return setup_;
    }

    Handoff
    onHandoff (std::unique_ptr <sslbundle>&& bundle
             , beast::http::message&& request
             , endpoint_type remote_endpoint) override;

    PeerSequence
    getActivePeers () override;

    void
    check () override;

    void
    checkSanity (std::uint32_t) override;

    Peer::ptr
    findPeerByShortID (Peer::id_t const& id) override;

    void
    send (protocol::TMProposeSet& m) override;

    void
    send (protocol::TMValidation& m) override;

    void
    relay (protocol::TMProposeSet& m, uint256 const& uid) override;

    void
    relay (protocol::TMValidation& m, uint256 const& uid) override;

    //--------------------------------------------------------------------------
    //
    // OverlayImpl
    //

    void
    add_active (std::shared_ptr<PeerImp> const& peer);

    void
    remove (PeerFinder::Slot::ptr const& slot);

    /** Called when a peer has connected successfully
        This is called after the peer handshake has been completed and during
        peer activation. At this point, the peer address and the public key
        are known.
    */
    void
    activate (std::shared_ptr<PeerImp> const& peer);

    /** Called when an active peer is destroyed. */
    void
    onPeerDeactivate (Peer::id_t id, SkywellAddress const& publicKey);

    // UnaryFunc will be called as
    //  void(std::shared_ptr<PeerImp>&&)
    //
    template <class UnaryFunc>
    void
    for_each_unlocked (UnaryFunc&& f)
    {
        for (auto const& e : m_publicKeyMap)
        {
            auto sp = e.second.lock ();
            if (sp)
                f(std::move(sp));
        }
    }

    template <class UnaryFunc>
    void
    for_each (UnaryFunc&& f)
    {
        std::lock_guard<decltype (mutex_)> lock (mutex_);
        for_each_unlocked (f);
    }

    std::size_t
    selectPeers (PeerSet& set
              , std::size_t limit
              , std::function<bool(std::shared_ptr<Peer> const&)> score) override;

    static
    bool
    isPeerUpgrade (beast::http::message const& request);

    static
    std::string
    makePrefix (std::uint32_t id);

private:
    std::shared_ptr<HTTP::Writer>
    makeRedirectResponse (PeerFinder::Slot::ptr const& slot
                        , beast::http::message const& request
                        , address_type remote_address);

    void
    connect (boost::asio::ip::tcp::endpoint const& remote_endpoint) override;

    std::size_t
    size() override;

    Json::Value
    crawl() override;

    Json::Value
    json() override;

    bool
    processRequest (beast::http::message const& req
                , Handoff& handoff);

    //--------------------------------------------------------------------------

    //
    // Stoppable
    //

    void
    checkStopped();

    void
    onPrepare() override;

    void
    onStart() override;

    void
    onStop() override;

    void
    onChildrenStopped() override;

    //
    // PropertyStream
    //

    void
    onWrite (beast::PropertyStream::Map& stream);

    //--------------------------------------------------------------------------

    void
    add (std::shared_ptr<PeerImp> const& peer);

    void
    remove (Child& child);

    void
    stop ();

    void
    autoConnect ();

    void
    sendEndpoints ();
};

} // skywell

#endif
