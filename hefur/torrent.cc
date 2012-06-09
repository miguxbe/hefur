#include <mimosa/bencode/copy.hh>
#include <mimosa/bencode/decoder.hh>
#include <mimosa/bencode/encoder.hh>
#include <mimosa/stream/fd-stream.hh>
#include <mimosa/stream/hash.hh>
#include <mimosa/time.hh>

#include "options.hh"
#include "torrent.hh"
#include "log.hh"

namespace hefur
{
  Torrent::Torrent(const InfoSha1 &    info_sha1,
                   const std::string & name,
                   const std::string & path,
                   uint64_t            length)
    : info_sha1_(info_sha1),
      name_(name),
      path_(path),
      length_(length),
      timeouts_(),
      peers_(),
      leechers_(0),
      seeders_(0),
      completed_(0)
  {
  }

  Torrent::~Torrent()
  {
    while (!timeouts_.empty())
    {
      auto peer = timeouts_.front();
      timeouts_.erase(peer);
      delete peer;
    }
  }

  void
  Torrent::cleanup()
  {
    mimosa::Time now = mimosa::monotonicTimeCoarse();

    while (!timeouts_.empty())
    {
      auto peer = timeouts_.front();
      if (peer->timeout_ts_ > now)
        return;

      log->debug("remove peer (%v) because %v > %v",
          peer->addr_.ipStr(),
          now / mimosa::second,
          peer->timeout_ts_ / mimosa::second);
      removePeer(peer);
    }
  }

  AnnounceResponse::Ptr
  Torrent::announce(AnnounceRequest::Ptr request)
  {
    // prepare the response
    AnnounceResponse::Ptr response = new AnnounceResponse;
    response->error_      = false;
    response->error_msg_.clear();
    response->nleechers_  = leechers_;
    response->nseeders_   = seeders_;
    response->ncompleted_ = completed_;

    // find the peer
    Peer * peer = peers_.find(request->addr_.key());

    // update counters to reflect a leecher becoming seeder
    if (peer && peer->left_ > 0 && request->left_ == 0 &&
        request->event_ == AnnounceRequest::kCompleted)
      ++completed_;

    // the peer wants to stop, so remove it from the peers
    if (request->event_ == AnnounceRequest::kStopped)
    {
      if (peer)
        removePeer(peer);
      return response;
    }

    // create the peer
    if (!createOrUpdatePeer(request, peer))
    {
      response->error_ = true;
      response->error_msg_ = "internal error (2)";
      return response;
    }

    if (request->num_want_ > 100)
      request->num_want_ = 100;

    for (auto it = timeouts_.rbegin(); it != timeouts_.rend(); ++it)
    {
      if (response->addrs_.size() >= request->num_want_)
        break;

      if (!::memcmp(it->peerid_, request->peerid_, 20) ||
          request->addr_ == it->addr_)
        continue;

      response->addrs_.push_back(it->addr_);
    }

    // check that we have less than MAX_PEERS
    if (leechers_ + seeders_ > MAX_PEERS)
      removePeer(timeouts_.front());

    return response;
  }

  Peer *
  Torrent::createOrUpdatePeer(AnnounceRequest::Ptr request, Peer * peer)
  {
    bool update = !!peer;

    // decrements the counters to simplify the completed event
    if (peer)
    {
      if (peer->left_ == 0)
        --seeders_;
      else
        --leechers_;
    }

    if (!peer)
      peer = new Peer;
    peer->left_       = request->left_;
    peer->downloaded_ = request->downloaded_;
    peer->uploaded_   = request->uploaded_;
    memcpy(&peer->addr_, &request->addr_, sizeof (request->addr_));
    memcpy(peer->peerid_, request->peerid_, 20);

    // increments the counters
    if (peer->left_ == 0)
      ++seeders_;
    else
      ++leechers_;
    if (!update)
      peers_.insert(peer);
    updateTimeout(peer, update);
    return peer;
  }

  void
  Torrent::updateTimeout(Peer * peer, bool remove)
  {
    if (remove)
      timeouts_.erase(peer);
    peer->timeout_ts_ = mimosa::monotonicTimeCoarse()
      + mimosa::minute * (ANNOUNCE_INTERVAL + 5);
    timeouts_.pushBack(peer);
  }

  void
  Torrent::removePeer(Peer * peer)
  {
    if (peer->left_ == 0)
      --seeders_;
    else
      --leechers_;

    timeouts_.erase(peer);
    peers_.erase(peer);
    delete peer;
  }

  Torrent::Ptr
  Torrent::parseFile(const std::string & path)
  {
    auto        input = mimosa::stream::FdStream::openFile(path.c_str());
    std::string name;
    uint64_t    length = 0;

    if (!input)
      return nullptr;

    mimosa::bencode::Decoder dec(input);
    auto token = dec.pull();
    if (token != mimosa::bencode::kDict)
      return nullptr;

    while (true)
    {
      token = dec.pull();
      if (token != mimosa::bencode::kData)
        return nullptr;

      if (dec.getData() != "info")
      {
        dec.eatValue();
        continue;
      }

      mimosa::stream::Sha1::Ptr sha1(new mimosa::stream::Sha1);
      mimosa::bencode::Encoder enc(sha1);

      token = dec.pull();
      if (token != mimosa::bencode::kDict ||
          !mimosa::bencode::copyToken(token, dec, enc))
        return nullptr;

      while (true)
      {
        token = dec.pull();
        if (!mimosa::bencode::copyToken(token, dec, enc))
          return nullptr;

        if (token == mimosa::bencode::kData)
        {
          if (!::strcasecmp(dec.getData().c_str(), "name"))
          {
            token = dec.pull();
            if (token != mimosa::bencode::kData ||
                !mimosa::bencode::copyToken(token, dec, enc))
              return nullptr;

            name = dec.getData();
            continue;
          }
          else if (!::strcasecmp(dec.getData().c_str(), "length"))
          {
            token = dec.pull();
            if (token != mimosa::bencode::kInt ||
                !mimosa::bencode::copyToken(token, dec, enc))
              return nullptr;

            length = dec.getInt();
            continue;
          }
        }
        else if (token == mimosa::bencode::kEnd)
          break;

        if (!mimosa::bencode::copyValue(dec, enc))
          return nullptr;
      }

      InfoSha1 info;
      memcpy(info.bytes_, sha1->digest(), 20);
      return new Torrent(info, name, path, length);
    }
  }
}
