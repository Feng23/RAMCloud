/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * Implementation of #RAMCloud::FastTransport.
 */

#include "FastTransport.h"
#include "MockDriver.h"

namespace RAMCloud {

// --- FastTransport ---

// - public -

/**
 * Create a FastTransport attached to a particular Driver
 *
 * \param driver
 *      The lower-level driver (presumably to an unreliable mechanism) to
 *      send/receive fragments on.
 */
FastTransport::FastTransport(Driver* driver)
    : driver(driver)
    , clientSessions(this)
    , serverSessions(this)
    , serverReadyQueue()
    , timerList()
{
    TAILQ_INIT(&serverReadyQueue);
    LIST_INIT(&timerList);
}

// See Transport::clientSend().
FastTransport::ClientRpc*
FastTransport::clientSend(Service* service,
                          Buffer* request,
                          Buffer* response)
{
    // Clear the response buffer if needed
    uint32_t length = response->getTotalLength();
    if (length)
        response->truncateFront(length);

    ClientRpc* rpc = new(request, MISC) ClientRpc(this, service,
                                                  request, response);
    rpc->start();
    return rpc;
}

// See Transport::serverRecv().
FastTransport::ServerRpc*
FastTransport::serverRecv()
{
    ServerRpc* rpc;
    while ((rpc = TAILQ_FIRST(&serverReadyQueue)) == NULL)
        poll();
    TAILQ_REMOVE(&serverReadyQueue, rpc, readyQueueEntries);
    return rpc;
}

// - private -

/**
 * Schedule Timer::fireTimer() to be called when the system TSC reaches when.
 *
 * If timer is already scheduled it will be rescheduled for when.
 *
 * \param timer
 *      The Timer on which to call fireTimer().
 * \param when
 *      fireTimer() is called when rdtsc() is at or beyond this timestamp.
 */
void
FastTransport::addTimer(Timer* timer, uint64_t when)
{
    timer->when = when;
    if (!LIST_IS_IN(timer, listEntries)) {
        LIST_INSERT_HEAD(&timerList, timer, listEntries);
    }
}

/**
 * \return
 *      Number of bytes of RPC data that can fit in a fragment (including the
 *      RPC headers).
 */
uint32_t
FastTransport::dataPerFragment()
{
    return driver->getMaxPayloadSize() - sizeof(Header);
}

/**
 * Invoke fireTimer() on any expired, scheduled Timer after removing it
 * from the timer event queue.
 *
 * Any timer that would like to be fired again later needs to
 * reschedule itself.
 */
void
FastTransport::fireTimers()
{
    uint64_t now = rdtsc();
    Timer* timer, *tmp;
    // Safe version needed because updates can occur during iteration
    LIST_FOREACH_SAFE(timer, &timerList, listEntries, tmp) {
        if (timer->when && timer->when <= now) {
            removeTimer(timer);
            timer->fireTimer(now);
       }
    }
}

/**
 * Reuse an existing ClientSession or create and return a new one.
 *
 * \return
 *      A ClientSession that is IDLE and ready for use.
 */
FastTransport::ClientSession*
FastTransport::getClientSession()
{
    clientSessions.expire();
    return clientSessions.get();
}

/**
 * Number of fragments that would be required to send dataBuffer over
 * this transport.
 *
 * \param dataBuffer
 *      A Buffer intended for transmission over this transport.
 * \return
 *      See method description.
 */
uint32_t
FastTransport::numFrags(const Buffer* dataBuffer)
{
    return ((dataBuffer->getTotalLength() + dataPerFragment() - 1) /
             dataPerFragment());
}

/**
 * Deschedule a Timer.
 *
 * \param timer
 *      The Timer to deschedule.
 */
void
FastTransport::removeTimer(Timer* timer)
{
    timer->when = 0;
    if (LIST_IS_IN(timer, listEntries)) {
        LIST_REMOVE(timer, listEntries);
    }
}

/**
 * Try to get a request from the Driver and queue it, dispatching
 * ready timer events in between.
 */
void
FastTransport::poll()
{
    while (tryProcessPacket())
        fireTimers();
    fireTimers();
}

/**
 * Send a fragment through the transport's driver.
 *
 * Randomly augments fragments with pleaseDrop bit for testing.
 * See Driver::sendPacket().
 */
void
FastTransport::sendPacket(const sockaddr* address,
                          socklen_t addressLength,
                          Header* header,
                          Buffer::Iterator* payload)
{
    header->pleaseDrop = (generateRandom() % 100) < PACKET_LOSS_PERCENTAGE;
    driver->sendPacket(address, addressLength,
                       header, sizeof(*header),
                       payload);
}

/**
 * Get a packet from the Driver and dispatch it to the appropriate handler.
 *
 * Dispatch is decided on Header::direction then to the appropriate
 * ClientSession or ServerSession.  If the request is to the Server and is
 * a SESSION_OPEN request then a new ServerSession is created and the
 * appropriate SessionOpenResponse is sent to the client.
 *
 * \retval false
 *      if the Driver didn't have a packet ready or encountered an
 *      error.
 * \retval true
 *      otherwise.
 */
bool
FastTransport::tryProcessPacket()
{
    Driver::Received received;
    if (!driver->tryRecvPacket(&received)) {
        TEST_LOG("no packet ready");
        return false;
    }

    Header* header = received.getOffset<Header>(0);
    if (header == NULL) {
        LOG(DEBUG, "packet too small");
        return true;
    }
    if (header->pleaseDrop) {
        TEST_LOG("dropped");
        return true;
    }

    if (header->getDirection() == Header::CLIENT_TO_SERVER) {
        if (header->serverSessionHint < serverSessions.size()) {
            ServerSession* session = serverSessions[header->serverSessionHint];
            if (session->getToken() == header->sessionToken) {
                TEST_LOG("calling ServerSession::processInboundPacket");
                session->processInboundPacket(&received);
                return true;
            } else {
                LOG(DEBUG, "bad token");
            }
        }
        switch (header->getPayloadType()) {
        case Header::SESSION_OPEN: {
            LOG(DEBUG, "session open");
            serverSessions.expire();
            ServerSession* session = serverSessions.get();
            session->startSession(&received.addr,
                                  received.addrlen,
                                  header->clientSessionHint);
            break;
        }
        default: {
            LOG(DEBUG, "bad session");
            Header replyHeader;
            replyHeader.sessionToken = header->sessionToken;
            replyHeader.rpcId = header->rpcId;
            replyHeader.clientSessionHint = header->clientSessionHint;
            replyHeader.serverSessionHint = header->serverSessionHint;
            replyHeader.channelId = header->channelId;
            replyHeader.payloadType = Header::BAD_SESSION;
            replyHeader.direction = Header::SERVER_TO_CLIENT;
            sendPacket(&received.addr, received.addrlen,
                       &replyHeader, NULL);
            break;
        }
        }
    } else {
        if (header->clientSessionHint < clientSessions.size()) {
            ClientSession* session = clientSessions[header->clientSessionHint];
            TEST_LOG("client session processing packet");
            session->processInboundPacket(&received);
        } else {
            LOG(DEBUG, "Bad client session hint");
        }
    }
    return true;
}

// --- ClientRpc ---

/**
 * Create an RPC over a Transport to a Service with a specific request
 * payload and a destination Buffer for response.
 *
 * \param transport
 *      The Transport this RPC is to be emitted on.
 * \param service
 *      The Service this RPC is addressed to.
 * \param request
 *      The request payload including RPC headers.
 * \param[out] response
 *      The response payload including the RPC headers.
 */
FastTransport::ClientRpc::ClientRpc(FastTransport* transport,
                                    Service* service,
                                    Buffer* request,
                                    Buffer* response)
    : requestBuffer(request)
    , responseBuffer(response)
    , state(IDLE)
    , transport(transport)
    , service(service)
    , serverAddress()
    , serverAddressLen(0)
    , channelQueueEntries()
{
    sockaddr_in *addr = const_cast<sockaddr_in*>(
        reinterpret_cast<const sockaddr_in*>(&serverAddress));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(service->getPort());
    if (inet_aton(service->getIp(), &addr->sin_addr) == 0)
        throw Exception("inet_aton failed");
    serverAddressLen = sizeof(*addr);
}

// TODO(stutsman) getReply should return something or be renamed
/**
 * Blocks until the response buffer associated with this RPC is valid and
 * populated.
 *
 * This method must be called for each RPC before its result can be used.
 *
 * \throws TransportException
 *      If the RPC aborted.
 */
void
FastTransport::ClientRpc::getReply()
{
    while (true) {
        switch (state) {
        case IDLE:
            LOG(ERROR, "getReply() shouldn't be possible while IDLE");
            return;
        case IN_PROGRESS:
        default:
            transport->poll();
            break;
        case COMPLETED:
            return;
        case ABORTED:
            throw TransportException("RPC aborted");
        }
    }
}

/// Change state to ABORTED.  Internal to FastTransport.
void
FastTransport::ClientRpc::aborted()
{
    state = ABORTED;
}

/// Change state to COMPLETED.  Internal to FastTransport.
void
FastTransport::ClientRpc::completed()
{
    state = COMPLETED;
}

/**
 * Begin a RPC.  Internal to FastTransport.
 *
 * The RPC will reuse a session that is cached in the Service instance
 * it is connecting to or will acquire a new session automatically in order
 * to complete this RPC.
 *
 * Pre-conditions
 *  - The caller must ensure that this RPC is IDLE.
 * Post-conditions
 *  - RPC is IN_PROGRESS.
 *  - session is valid and connected.
 * Side-effects
 *  - The RPC's Service will be populated with the current session for reuse
 *    on future calls.
 */
void
FastTransport::ClientRpc::start()
{
    state = IN_PROGRESS;
    ClientSession* session =
        static_cast<ClientSession*>(service->getSession());
    if (!session)
        session = transport->clientSessions.get();
    if (!session->isConnected())
        session->connect(&serverAddress, serverAddressLen);
    service->setSession(session);
    LOG(DEBUG, "Using session id %u", session->id);
    session->startRpc(this);
}

// --- ServerRpc ---

/**
 * Create a ServerRpc attached to a ServerSession on a particular channel.
 *
 * \param session
 *      The ServerSession this RPC is associated with.
 * \param channelId
 *      The channel in session on which to handle this RPC.
 */
FastTransport::ServerRpc::ServerRpc(ServerSession* session,
                                    uint8_t channelId)
    : session(session)
    , channelId(channelId)
    , readyQueueEntries()
{
}

/**
 * Begin sending the RPC response.
 */
void
FastTransport::ServerRpc::sendReply()
{
    session->beginSending(channelId);
}

// --- PayloadChunk ---

FastTransport::PayloadChunk*
FastTransport::PayloadChunk::prependToBuffer(Buffer* buffer,
                                             char* data,
                                             uint32_t dataLength,
                                             Driver* driver,
                                             char* payload,
                                             uint32_t payloadLength)
{
    PayloadChunk* chunk =
        new(buffer, CHUNK) PayloadChunk(data,
                                        dataLength,
                                        driver,
                                        payload,
                                        payloadLength);
    Buffer::Chunk::prependChunkToBuffer(buffer, chunk);
    return chunk;
}

FastTransport::PayloadChunk*
FastTransport::PayloadChunk::appendToBuffer(Buffer* buffer,
                                            char* data,
                                            uint32_t dataLength,
                                            Driver* driver,
                                            char* payload,
                                            uint32_t payloadLength)
{
    PayloadChunk* chunk =
        new(buffer, CHUNK) PayloadChunk(data,
                                        dataLength,
                                        driver,
                                        payload,
                                        payloadLength);
    Buffer::Chunk::appendChunkToBuffer(buffer, chunk);
    return chunk;
}

/// Returns memory to the Driver once the Chunk is discarded.
FastTransport::PayloadChunk::~PayloadChunk()
{
    if (driver)
        driver->release(payload, payloadLength);
}

FastTransport::PayloadChunk::PayloadChunk(void* data,
                                          uint32_t dataLength,
                                          Driver* driver,
                                          char* const payload,
                                          uint32_t payloadLength)
    : Buffer::Chunk(data, dataLength)
    , driver(driver)
    , payload(payload)
    , payloadLength(payloadLength)
{
}

// --- InboundMessage ---

/**
 * Construct an InboundMessage which is NOT yet ready to use.
 *
 * NOTE: until setup() and init() have been called this instance
 * is not ready to receive fragments.
 */
FastTransport::InboundMessage::InboundMessage()
    : transport(NULL)
    , session(NULL)
    , channelId(0)
    , totalFrags(0)
    , firstMissingFrag(0)
    , dataStagingRing()
    , dataBuffer(NULL)
    , timer(false, this)
{
}

/**
 * Cleanup an InboundMessage, releasing any unaccounted for packet
 * data back to the Driver.
 */
FastTransport::InboundMessage::~InboundMessage()
{
    clear();
}

/**
 * One-time initialization that permanently attaches this instance to
 * a particular Session, channelId, and timer status.
 *
 * This method is necessary since the Channels in which they are contained
 * are allocated as an array (hence with the default constructor) requiring
 * additional post-constructor setup.
 *
 * \param transport
 *      The FastTranport this message is associated with.
 * \param session
 *      The Session belonging to this message.
 * \param channelId
 *      The ID of the channel this message belongs to.
 * \param useTimer
 *      Whether this message should respond to timer events.
 */
void
FastTransport::InboundMessage::setup(FastTransport* transport,
                                     Session* session,
                                     uint32_t channelId,
                                     bool useTimer)
{
    this->transport = transport;
    this->session = session;
    this->channelId = channelId;
    if (timer.useTimer)
        transport->removeTimer(&timer);
    timer.when = 0;
    timer.numTimeouts = 0;
    timer.useTimer = useTimer;
}

/**
 * Creates and transmits an ACK decribing which fragments are still missing.
 */
void
FastTransport::InboundMessage::sendAck()
{
    Header header;
    session->fillHeader(&header, channelId);
    header.payloadType = Header::ACK;
    Buffer payloadBuffer;
    AckResponse *ackResponse =
        new(&payloadBuffer, APPEND) AckResponse(firstMissingFrag);
    for (uint32_t i = 0; i < dataStagingRing.getLength(); i++) {
        std::pair<char*, uint32_t> elt = dataStagingRing[i];
        if (elt.first)
            ackResponse->stagingVector |= (1 << i);
    }
    socklen_t addrlen;
    const sockaddr* addr = session->getAddress(&addrlen);
    Buffer::Iterator iter(payloadBuffer);
    transport->sendPacket(addr,
                          addrlen,
                          &header,
                          &iter);
}

/**
 * Cleans up an InboundMessage and marks it inactive.
 *
 * A subsequent call to init() will set it up to be reused.  This call
 * also returns any memory held in the incoming window to the Driver and
 * removes any timer events associated with the message.
 */
void
FastTransport::InboundMessage::clear()
{
    totalFrags = 0;
    firstMissingFrag = 0;
    dataBuffer = NULL;
    for (uint32_t i = 0; i < dataStagingRing.getLength(); i++) {
        std::pair<char*, uint32_t> elt = dataStagingRing[i];
        if (elt.first)
            transport->driver->release(elt.first, elt.second);
    }
    dataStagingRing.clear();
    timer.numTimeouts = 0;
    if (timer.useTimer)
         transport->removeTimer(&timer);
}

/**
 * Initialize a previously cleared InboundMessage for use.
 *
 * This must be called before a previously inactive InboundMessage is ready
 * to receive fragments.
 *
 * \param totalFrags
 *      The total number of incoming fragments the message should expect.
 * \param[out] dataBuffer
 *      The buffer to fill with the data from this message.
 */
void
FastTransport::InboundMessage::init(uint16_t totalFrags,
                                    Buffer* dataBuffer)
{
    clear();
    this->totalFrags = totalFrags;
    this->dataBuffer = dataBuffer;
    if (timer.useTimer)
        transport->addTimer(&timer, rdtsc() + TIMEOUT_NS);
}

/**
 * Take a single fragment and incorporate it as part of this message.
 *
 * If the fragment header disagrees on the total length of the message
 * with the value set on init() the packet is ignored.
 *
 * If the fragNumber matches the firstMissingFrag of this message then
 * it is concatenated to the message's buffer along with any other packets
 * following this fragments in the dataStagingRing that are contiguous and
 * have no other missing fragments preceding them in the message.
 *
 * If the fragNumber exceeds the firstMissingFrag of this message then
 * it is stored in the dataStagingRing to be appended according to the
 * case outlined in the paragraph above.
 *
 * Any packet data that will be returned as part of the Buffer is "stolen"
 * from the Driver::Received object, and the Buffer's Chunk object is
 * responsible for returning the memory to the Driver later.  Any packet data
 * that goes unused (that which is not stolen) will be returned to the driver
 * when the Driver::Received is deleted.
 *
 * This code also notices if the incoming fragment has sendAck set.  If it
 * does then sendAck is called directly after handling this packet as above.
 *
 * \param received
 *      A single fragment wrapped in a Driver::Received.
 * \return
 *      Whether the full message has been received and the dataBuffer is now
 *      complete and valid.
 */
bool
FastTransport::InboundMessage::processReceivedData(Driver::Received* received)
{
    assert(received->len >= sizeof(Header));
    Header *header = reinterpret_cast<Header*>(received->payload);

    if (header->totalFrags != totalFrags) {
        // What's wrong with the other end?
        LOG(DEBUG, "header->totalFrags != totalFrags");
        return firstMissingFrag == totalFrags;
    }

    if (header->fragNumber == firstMissingFrag) {
        uint32_t length;
        char *payload = received->steal(&length);
        PayloadChunk::appendToBuffer(dataBuffer,
                                     payload + sizeof(Header),
                                     length - sizeof(Header),
                                     transport->driver,
                                     payload,
                                     length);
        firstMissingFrag++;
        while (true) {
            // TODO(stutsman) ring serializer for unit test assertions
            std::pair<char*, uint32_t> pair = dataStagingRing[0];
            char* payload = pair.first;
            uint32_t length = pair.second;
            dataStagingRing.advance(1);
            if (!payload)
                break;
            PayloadChunk::appendToBuffer(dataBuffer,
                                         payload + sizeof(Header),
                                         length - sizeof(Header),
                                         transport->driver,
                                         payload,
                                         length);
            firstMissingFrag++;
        }
    } else if (header->fragNumber > firstMissingFrag) {
        if ((header->fragNumber - firstMissingFrag) >
            MAX_STAGING_FRAGMENTS) {
            LOG(DEBUG, "fragNumber too big");
        } else {
            uint32_t i = header->fragNumber - firstMissingFrag - 1;
            if (!dataStagingRing[i].first) {
                uint32_t length;
                char* payload = received->steal(&length);
                dataStagingRing[i] =
                    std::pair<char*, uint32_t>(payload, length);
            } else {
                LOG(DEBUG, "duplicate fragment %d received",
                    header->fragNumber);
            }
        }
    } else { // header->fragNumber < firstMissingFrag
        // stale, no-op
    }

    // TODO(ongaro): Have caller call this->sendAck() instead.
    // TODO(stutsman) this stuff should move?
    if (header->requestAck)
        sendAck();
    if (timer.useTimer)
         transport->addTimer(&timer, rdtsc() + TIMEOUT_NS);

    return firstMissingFrag == totalFrags;
}

// --- OutboundMessage ---

/**
 * Construct an OutboundMessage which is NOT yet ready to use.
 *
 * NOTE: until setup() has been called this instance
 * is not ready to send fragments.
 */
FastTransport::OutboundMessage::OutboundMessage()
    : transport(NULL)
    , session(NULL)
    , channelId(0)
    , sendBuffer(0)
    , firstMissingFrag(0)
    , totalFrags(0)
    , packetsSinceAckReq(0)
    , sentTimes()
    , numAcked(0)
    , timer(false, this)
{
}

/**
 * One-time initialization that permanently attaches this instance to
 * a particular Session, channelId, and timer status.
 *
 * This method is necessary since the Channels in which they are contained
 * are allocated as an array (hence with the default constructor) requiring
 * additional post-constructor setup.
 *
 * \param transport
 *      The FastTranport this message is associated with.
 * \param session
 *      The Session this message is associated with.
 * \param channelId
 *      The ID of the channel this message belongs to.
 * \param useTimer
 *      Whether this message should respond to timer events.
 */
void
FastTransport::OutboundMessage::setup(FastTransport* transport,
                                      Session* session,
                                      uint32_t channelId,
                                      bool useTimer)
{
    this->transport = transport;
    this->session = session;
    this->channelId = channelId;
    clear();
    timer.useTimer = useTimer;
}

/**
 * Cleans up an OutboundMessage and marks it inactive.
 *
 * This must be called before an actively used instance can be recycled
 * by calling beginSending() on it.
 */
void
FastTransport::OutboundMessage::clear()
{
    sendBuffer = NULL;
    firstMissingFrag = 0;
    totalFrags = 0;
    packetsSinceAckReq = 0;
    sentTimes.clear();
    numAcked = 0;
    if (timer.useTimer)
        transport->removeTimer(&timer);
    timer.when = 0;
    timer.numTimeouts = 0;
}

/**
 * Begin sending a buffer.  Requires the buffer be inactive (clear() was
 * called on it).
 *
 * \param dataBuffer
 *      Buffer of data to send.
 */
void
FastTransport::OutboundMessage::beginSending(Buffer* dataBuffer)
{
    assert(!sendBuffer);
    sendBuffer = dataBuffer;
    totalFrags = transport->numFrags(sendBuffer);
    send();
}

/**
 * Send out data packets and update timestamps/status in sentTimes.
 *
 * If a packet is retransmitted due to a timeout it is sent with a request
 * for ACK and no further packets are transmitted until the next event
 * (either an additional timeout or an ACK is processed).  If no packet
 * is retransmitted then the call will send as many fresh data packets as
 * the window allows with every REQ_ACK_AFTER th packet marked as request
 * for ACK.
 *
 * Pre-conditions:
 *  - beginSending() must have been called since the last call to clear().
 *
 * Side-effects:
 *  - sentTimes is updated to reflect any sent packets.
 *  - If timers are enabled for this message then the timer is scheduled
 *    to fire when the next packet retransmit timeout occurs.
 */
void
FastTransport::OutboundMessage::send()
{
    uint64_t now = rdtsc();

    // First, decide on candidate range of packets to send/resend
    // Only fragments less than stop will be considered for (re-)send

    // Can't send beyond the last fragment
    uint32_t stop = totalFrags;
    // Can't send beyond the window
    stop = std::min(stop, numAcked + WINDOW_SIZE);
    // Can't send beyond what the receiver is willing to accept
    stop = std::min(stop, firstMissingFrag + MAX_STAGING_FRAGMENTS + 1);

    // Send frags from candidate range
    for (uint32_t i = 0; i < stop - firstMissingFrag; i++) {
        uint64_t sentTime = sentTimes[i];
        // skip if ACKED or if already sent but not yet timed out
        if ((sentTime == ACKED) ||
            (sentTime != 0 && sentTime + TIMEOUT_NS >= now))
            continue;
        // isRetransmit if already sent and timed out (guaranteed by if above)
        bool isRetransmit = sentTime != 0;
        uint32_t fragNumber = firstMissingFrag + i;
        // requestAck if retransmit or
        // haven't asked for ack in awhile and this is not the last frag
        bool requestAck = isRetransmit ||
            (packetsSinceAckReq == REQ_ACK_AFTER - 1 &&
             fragNumber != totalFrags - 1);
        sendOneData(fragNumber, requestAck);
        sentTimes[i] = now;
        if (isRetransmit)
            break;
    }

    // find the packet that will cause timeout the earliest and
    // schedule a timer just after that
    if (timer.useTimer) {
        uint64_t oldest = ~(0lu);
        for (uint32_t i = 0; i < stop - firstMissingFrag; i++) {
            uint64_t sentTime = sentTimes[i];
            // if we reach a not-sent, the rest must be not-sent
            if (!sentTime)
                break;
            if (sentTime != ACKED && sentTime > 0)
                if (sentTime < oldest)
                    oldest = sentTime;
        }
        if (oldest != ~(0lu))
            transport->addTimer(&timer, oldest + TIMEOUT_NS);
    }
}

/**
 * Process an AckResponse and advance window, if possible.
 *
 * Notice this function calls send() to try to send additional fragments.
 *
 * Side-effects:
 *  - firstMissingFrag and sentTimes may advance.
 *  - fragments may be marked as ACKED.
 *  - send() may be freed up to send further packets.
 *
 * \param received
 *      Data received from a sender containing a packet header and a valid
 *      AckResponse payload.
 */
bool
FastTransport::OutboundMessage::processReceivedAck(Driver::Received* received)
{
    if (!sendBuffer)
        return false;

    assert(received->len >= sizeof(Header) + sizeof(AckResponse));
    AckResponse *ack =
        received->getOffset<AckResponse>(sizeof(Header));

    if (ack->firstMissingFrag < firstMissingFrag) {
        LOG(DEBUG, "OutboundMessage dropped stale ACK");
    } else if (ack->firstMissingFrag > totalFrags) {
        LOG(DEBUG, "OutboundMessage dropped invalid ACK"
                   "(shouldn't happen)");
    } else if (ack->firstMissingFrag >
             (firstMissingFrag + sentTimes.getLength())) {
        LOG(DEBUG, "OutboundMessage dropped ACK that advanced too far "
                   "(shouldn't happen)");
    } else {
        sentTimes.advance(ack->firstMissingFrag - firstMissingFrag);
        firstMissingFrag = ack->firstMissingFrag;
        numAcked = ack->firstMissingFrag;
        for (uint32_t i = 0; i < sentTimes.getLength() - 1; i++) {
            bool acked = (ack->stagingVector >> i) & 1;
            if (acked) {
                sentTimes[i + 1] = ACKED;
                numAcked++;
            }
        }
    }
    send();
    return firstMissingFrag == totalFrags;
}

/**
 * Send out a single data fragment drawn from sendBuffer.
 *
 * This function also augments the packet header with the request ACK bit
 * if REQ_ACK_AFTER packets have been sent without requesting an ACK.
 *
 * \param fragNumber
 *      The fragment number to place in the packet header.
 * \param requestAck
 *      The packet header will have the request ACK bit set.
 */
void
FastTransport::OutboundMessage::sendOneData(uint32_t fragNumber,
                                            bool requestAck)
{
    Header header;
    session->fillHeader(&header, channelId);
    header.fragNumber = fragNumber;
    header.totalFrags = totalFrags;
    header.requestAck = requestAck;
    header.payloadType = Header::DATA;
    uint32_t dataPerFragment = transport->dataPerFragment();
    Buffer::Iterator iter(*sendBuffer,
                          fragNumber * dataPerFragment,
                          dataPerFragment);

    socklen_t addrlen;
    const sockaddr* addr = session->getAddress(&addrlen);
    transport->sendPacket(addr, addrlen, &header, &iter);

    if (requestAck)
        packetsSinceAckReq = 0;
    else
        packetsSinceAckReq++;
}

// --- ServerSession ---

const uint64_t FastTransport::ServerSession::INVALID_TOKEN =
                                                        0xcccccccccccccccclu;
const uint32_t FastTransport::ServerSession::INVALID_HINT = 0xccccccccu;

/**
 * Create a session associated with a particular transport with offset
 * into the transport's serverSessionTable of sessionId.
 *
 * \param transport
 *      The FastTransport with which this Session is associated.
 * \param sessionId
 *      This session's offset in FastTransport::serverSessionTable.  This
 *      is used as the serverSessionHint in packets to the client so the
 *      client can return the hint and get fast access to this ServerSession.
 */
FastTransport::ServerSession::ServerSession(FastTransport* transport,
                                            uint32_t sessionId)
    : Session(transport, sessionId)
    , nextFree(FastTransport::SessionTable<ServerSession>::NONE)
    , clientAddress()
    , clientAddressLen(0)
    , clientSessionHint(INVALID_HINT)
    , lastActivityTime(0)
    , token(INVALID_TOKEN)
{
    memset(&clientAddress, 0xcc, sizeof(clientAddress));
    for (uint32_t i = 0; i < NUM_CHANNELS_PER_SESSION; i++)
        channels[i].setup(transport, this, i);
}

/**
 * Switch from PROCESSING to SENDING_WAITING and initiate transfer of the
 * RPC response from the server to the client.
 *
 * Preconditions:
 *  - The caller must ensure that the indicated channel is PROCESSING.
 *
 * \param channelId
 *      Send the response on this channelId, which is currently PROCESSING.
 */
void
FastTransport::ServerSession::beginSending(uint8_t channelId)
{
    ServerChannel* channel = &channels[channelId];
    assert(channel->state == ServerChannel::PROCESSING);
    channel->state = ServerChannel::SENDING_WAITING;
    Buffer* responseBuffer = &channel->currentRpc->replyPayload;
    channel->outboundMsg.beginSending(responseBuffer);
    lastActivityTime = rdtsc();
}

/// This shouldn't ever be called.
void
FastTransport::ServerSession::close()
{
    LOG(WARNING, "ServerSession::close should never be called");
}

// See Session::expire().
bool
FastTransport::ServerSession::expire()
{
    if (lastActivityTime == 0)
        return true;

    for (uint32_t i = 0; i < NUM_CHANNELS_PER_SESSION; i++) {
        if (channels[i].state == ServerChannel::PROCESSING)
            return false;
    }

    for (uint32_t i = 0; i < NUM_CHANNELS_PER_SESSION; i++) {
        if (channels[i].state == ServerChannel::IDLE)
            continue;
        channels[i].state = ServerChannel::IDLE;
        channels[i].rpcId = ~0U;
        delete channels[i].currentRpc;
        channels[i].currentRpc = NULL;
        channels[i].inboundMsg.clear();
        channels[i].outboundMsg.clear();
    }

    token = INVALID_TOKEN;
    clientSessionHint = INVALID_HINT;
    lastActivityTime = 0;
    memset(&clientAddress, 0xcc, sizeof(clientAddress));

    return true;
}

// See Session::fillHeader().
void
FastTransport::ServerSession::fillHeader(Header* const header,
                                         uint8_t channelId) const
{
    header->rpcId = channels[channelId].rpcId;
    header->channelId = channelId;
    header->direction = Header::SERVER_TO_CLIENT;
    header->clientSessionHint = clientSessionHint;
    header->serverSessionHint = id;
    header->sessionToken = token;
}

// See Session::getAddress().
const sockaddr*
FastTransport::ServerSession::getAddress(socklen_t *len)
{
    *len = clientAddressLen;
    return &clientAddress;
}

// See Session::getLastActivityTime().
uint64_t
FastTransport::ServerSession::getLastActivityTime()
{
    return lastActivityTime;
}

/**
 * Returns the authentication token the client needs to succesfully
 * reassociate with this session.
 *
 * \return
 *      See method description.
 */
uint64_t
FastTransport::ServerSession::getToken()
{
    return token;
}

/**
 * Dispatch an incoming packet to the correct action for this session.
 *
 * \param received
 *      A packet wrapped up in a Driver::Received.
 */
void
FastTransport::ServerSession::processInboundPacket(Driver::Received* received)
{
    lastActivityTime = rdtsc();
    Header* header = received->getOffset<Header>(0);
    if (header->channelId >= NUM_CHANNELS_PER_SESSION) {
        LOG(DEBUG, "drop due to invalid channel");
        return;
    }

    ServerChannel* channel = &channels[header->channelId];
    if (channel->rpcId == header->rpcId) {
        // Incoming packet is part of the current RPC
        switch (header->getPayloadType()) {
        case Header::DATA:
            TEST_LOG("processReceivedData");
            processReceivedData(channel, received);
            break;
        case Header::ACK:
            TEST_LOG("processReceivedAck");
            processReceivedAck(channel, received);
            break;
        default:
            LOG(DEBUG, "drop current rpcId with bad type");
        }
    } else if (channel->rpcId + 1 == header->rpcId) {
        TEST_LOG("start a new RPC");
        // Incoming packet is part of the next RPC
        // clear everything out and setup for next RPC
        switch (header->getPayloadType()) {
        case Header::DATA: {
            channel->state = ServerChannel::RECEIVING;
            channel->rpcId = header->rpcId;
            channel->inboundMsg.clear();
            channel->outboundMsg.clear();
            delete channel->currentRpc;
            channel->currentRpc = new ServerRpc(this,
                                                header->channelId);
            Buffer* recvBuffer = &channel->currentRpc->recvPayload;
            channel->inboundMsg.init(header->totalFrags, recvBuffer);
            TEST_LOG("processReceivedData");
            processReceivedData(channel, received);
            break;
        }
        default:
            LOG(DEBUG, "drop new rpcId with bad type");
        }
    } else {
        LOG(DEBUG, "drop old packet");
    }
}

/**
 * Create a new session and send the SessionOpenResponse to the client.
 *
 * \param clientAddress
 *      Address of the session initiator.
 * \param clientAddressLen
 *      Length of clientAddress.
 * \param clientSessionHint
 *      The offset into the client's SessionTable to quickly associate
 *      packets from this session with the correct ClientSession.
 */
void
FastTransport::ServerSession::startSession(const sockaddr *clientAddress,
                                           socklen_t clientAddressLen,
                                           uint32_t clientSessionHint)
{
    assert(sizeof(this->clientAddress) >= clientAddressLen);
    memcpy(&this->clientAddress, clientAddress, clientAddressLen);
    this->clientAddressLen = clientAddressLen;
    this->clientSessionHint = clientSessionHint;
    token = ((generateRandom() << 32) | generateRandom());

    // send session open response
    Header header;
    header.direction = Header::SERVER_TO_CLIENT;
    header.clientSessionHint = clientSessionHint;
    header.serverSessionHint = id;
    header.sessionToken = token;
    header.rpcId = 0;
    header.channelId = 0;
    header.payloadType = Header::SESSION_OPEN;

    Buffer payload;
    SessionOpenResponse* sessionOpen;
    sessionOpen = new(&payload, APPEND) SessionOpenResponse;
    sessionOpen->maxChannelId = NUM_CHANNELS_PER_SESSION - 1;
    Buffer::Iterator payloadIter(payload);
    transport->sendPacket(&this->clientAddress, this->clientAddressLen,
                          &header, &payloadIter);
    lastActivityTime = rdtsc();
}

// - private -

/**
 * Process an ACK on a particular channel.
 *
 * Side-effect:
 *  - This may free some window and transmit more packets.
 *
 * \param channel
 *      The channel to process the ACK on.
 * \param received
 *      The ACK packet encapsulated in a Driver::Received.
 */
void
FastTransport::ServerSession::processReceivedAck(ServerChannel* channel,
                                                 Driver::Received* received)
{
    if (channel->state == ServerChannel::SENDING_WAITING)
        channel->outboundMsg.processReceivedAck(received);
}

/**
 * Process a data fragment on a particular channel.
 *
 * Routing the packet to the correct handler is a function of the current
 * state of the channel.
 *
 * Side-effect:
 *  - channel->state will transition from RECEIVING to PROCESSING if the
 *    full request has been received.
 *
 * \param channel
 *      The channel to process the ACK on.
 * \param received
 *      The ACK packet encapsulated in a Driver::Received.
 */
void
FastTransport::ServerSession::processReceivedData(ServerChannel* channel,
                                                  Driver::Received* received)
{
    Header* header = received->getOffset<Header>(0);
    switch (channel->state) {
    case ServerChannel::IDLE:
        break;
    case ServerChannel::RECEIVING:
        if (channel->inboundMsg.processReceivedData(received)) {
            TAILQ_INSERT_TAIL(&transport->serverReadyQueue,
                              channel->currentRpc,
                              readyQueueEntries);
            channel->state = ServerChannel::PROCESSING;
        }
        break;
    case ServerChannel::PROCESSING:
        if (header->requestAck)
            channel->inboundMsg.sendAck();
        break;
    case ServerChannel::SENDING_WAITING:
        // TODO(stutsman) Need to understand why this happens
        // and eliminate this send
        LOG(WARNING, "Received extraneous packet while sending");
        channel->outboundMsg.send();
        break;
    }
}

// --- ClientSession ---

const uint64_t FastTransport::ClientSession::INVALID_TOKEN =
                                                        0xcccccccccccccccclu;
const uint32_t FastTransport::ClientSession::INVALID_HINT = 0xccccccccu;

/**
 * Create a session associated with a particular transport with offset
 * into the transport's serverSessionTable of sessionId.
 *
 * \param transport
 *      The FastTransport with which this Session is associated.
 * \param sessionId
 *      This session's offset in FastTransport::serverSessionTable.  This
 *      is used as the serverSessionHint in packets to the client so the
 *      client can return the hint and get fast access to this ServerSession.
 */
FastTransport::ClientSession::ClientSession(FastTransport* transport,
                                            uint32_t sessionId)
    : Session(transport, sessionId)
    , nextFree(FastTransport::SessionTable<ClientSession>::NONE)
    , channels(0)
    , channelQueue()
    , lastActivityTime(0)
    , numChannels(0)
    , token(INVALID_TOKEN)
    , serverAddress()
    , serverAddressLen(0)
    , serverSessionHint(INVALID_HINT)
{
    memset(&serverAddress, 0xcc, sizeof(serverAddress));
    TAILQ_INIT(&channelQueue);
}

// See Session::close().
void
FastTransport::ClientSession::close()
{
    LOG(DEBUG, "Closing session");
    for (uint32_t i = 0; i < numChannels; i++) {
        if (channels[i].currentRpc)
            channels[i].currentRpc->aborted();
    }
    while (!TAILQ_EMPTY(&channelQueue)) {
        ClientRpc* rpc = TAILQ_FIRST(&channelQueue);
        TAILQ_REMOVE(&channelQueue, rpc, channelQueueEntries);
        rpc->aborted();
    }
    clearChannels();
    serverSessionHint = INVALID_HINT;
    token = INVALID_TOKEN;
}

/**
 * Send a session open request to serverAddress and establishing an
 * open ServerSession on the remote end.
 *
 * \param serverAddress
 *      The address of the RPC server to connect to.
 * \param serverAddressLen
 *      The length of serverAddress.
 */
void
FastTransport::ClientSession::connect(const sockaddr* serverAddress,
                                      socklen_t serverAddressLen)
{
    if (serverAddress) {
        assert(sizeof(this->serverAddress) >= serverAddressLen);
        memcpy(&this->serverAddress, serverAddress, serverAddressLen);
        this->serverAddressLen = serverAddressLen;
    }

    // TODO(ongaro): Would it be possible to open a session like other
    // RPCs?

    Header header;
    header.direction = Header::CLIENT_TO_SERVER;
    header.clientSessionHint = id;
    header.serverSessionHint = serverSessionHint;
    header.sessionToken = token;
    header.rpcId = 0;
    header.channelId = 0;
    header.requestAck = 0;
    header.payloadType = Header::SESSION_OPEN;
    transport->sendPacket(&this->serverAddress,
                          this->serverAddressLen,
                          &header, NULL);
    lastActivityTime = rdtsc();
}

// See Session::expire().
bool
FastTransport::ClientSession::expire()
{
    for (uint32_t i = 0; i < numChannels; i++) {
        if (channels[i].currentRpc)
            return false;
    }
    if (!TAILQ_EMPTY(&channelQueue))
        return false;
    close();
    return true;
}

// See Session::fillHeader().
void
FastTransport::ClientSession::fillHeader(Header* const header,
                                         uint8_t channelId) const
{
    header->rpcId = channels[channelId].rpcId;
    header->channelId = channelId;
    header->direction = Header::CLIENT_TO_SERVER;
    header->clientSessionHint = id;
    header->serverSessionHint = serverSessionHint;
    header->sessionToken = token;
}

// See Session::getAddress().
const sockaddr*
FastTransport::ClientSession::getAddress(socklen_t *len)
{
    *len = serverAddressLen;
    return &serverAddress;
}

// See Session::getLastActivityTime().
uint64_t
FastTransport::ClientSession::getLastActivityTime()
{
    return lastActivityTime;
}

/**
 * Return whether this session is currently connect to a remote endpoint.
 *
 * \return
 *      See method description.
 */
bool
FastTransport::ClientSession::isConnected()
{
    return (numChannels != 0);
}

/**
 * Dispatch an incoming packet to the correct action for this session.
 *
 * \param received
 *      A packet wrapped up in a Driver::Received.
 */
void
FastTransport::ClientSession::processInboundPacket(Driver::Received* received)
{
    lastActivityTime = rdtsc();
    Header* header = received->getOffset<Header>(0);
    if (header->channelId >= numChannels) {
        if (header->getPayloadType() == Header::SESSION_OPEN)
            processSessionOpenResponse(received);
        else
            LOG(DEBUG, "drop due to invalid channel");
        return;
    }

    ClientChannel* channel = &channels[header->channelId];
    if (channel->rpcId == header->rpcId) {
        switch (header->getPayloadType()) {
        case Header::DATA:
            processReceivedData(channel, received);
            break;
        case Header::ACK:
            processReceivedAck(channel, received);
            break;
        case Header::BAD_SESSION:
            // if any current RPCs in channels requeue them
            // and try to reconnect
            for (uint32_t i = 0; i < numChannels; i++) {
                if (channels[i].currentRpc) {
                    TAILQ_INSERT_TAIL(&channelQueue,
                                      channels[i].currentRpc,
                                      channelQueueEntries);
                }
            }
            clearChannels();
            serverSessionHint = INVALID_HINT;
            token = INVALID_TOKEN;
            connect(0, 0);
            break;
        default:
            LOG(DEBUG, "drop current rpcId with bad type");
        }
    } else {
        if (header->getPayloadType() == Header::DATA &&
            header->requestAck) {
            LOG(DEBUG, "TODO: fake a full ACK response");
        } else {
            LOG(DEBUG, "drop old packet");
        }
    }
}

/**
 * Perform a ClientRpc.
 *
 * rpc will be performed immediately on the first available channel or
 * queued until a channel is idle if none are currently available.
 *
 * \param rpc
 *      The ClientRpc to perform.
 */
void
FastTransport::ClientSession::startRpc(ClientRpc* rpc)
{
    lastActivityTime = rdtsc();
    ClientChannel* channel = getAvailableChannel();
    if (!channel) {
        LOG(DEBUG, "Queueing RPC");
        TAILQ_INSERT_TAIL(&channelQueue, rpc, channelQueueEntries);
    } else {
        assert(channel->state == ClientChannel::IDLE);
        channel->state = ClientChannel::SENDING;
        channel->currentRpc = rpc;
        channel->outboundMsg.beginSending(rpc->requestBuffer);
    }
}

// - private -

/**
 * Allocates numChannels worth of Channels in this Session.
 *
 * This is separated out so that testing methods can allocate Channels
 * without having to mock out a SessionOpenResponse.
 */
void
FastTransport::ClientSession::allocateChannels()
{
    channels = new ClientChannel[numChannels];
    for (uint32_t i = 0; i < numChannels; i++)
        channels[i].setup(transport, this, i);
}

/**
 * Reset this session to 0 channels and free associated resources.
 */
void
FastTransport::ClientSession::clearChannels()
{
    numChannels = 0;
    delete[] channels;
    channels = 0;
}

/**
 * Return an IDLE channel which can be used to service an RPC or 0
 * if none are IDLE.
 *
 * \return
 *      See method description.
 */
FastTransport::ClientSession::ClientChannel*
FastTransport::ClientSession::getAvailableChannel()
{
    for (uint32_t i = 0; i < numChannels; i++) {
        if (channels[i].state == ClientChannel::IDLE)
            return &channels[i];
    }
    return 0;
}

/**
 * Process an ACK on a particular channel.
 *
 * Side-effect:
 *  - This may free some window and transmit more packets.
 *
 * \param channel
 *      The channel to process the ACK on.
 * \param received
 *      The ACK packet encapsulated in a Driver::Received.
 */
void
FastTransport::ClientSession::processReceivedAck(ClientChannel* channel,
                                                 Driver::Received* received)
{
    if (channel->state == ClientChannel::SENDING)
        channel->outboundMsg.processReceivedAck(received);
}

/**
 * Process a data fragment on a particular channel.
 *
 * Routing the packet to the correct handler is a function of the current
 * state of the channel.
 *
 * Side-effects:
 *  - If data is received while the channel is SENDING it transitions to
 *    RECEIVING.
 *  - If the channel completes its RPC it goes onto the available channel
 *    queue.
 *
 * \param channel
 *      The channel to process the ACK on.
 * \param received
 *      The ACK packet encapsulated in a Driver::Received.
 */
void
FastTransport::ClientSession::processReceivedData(ClientChannel* channel,
                                                  Driver::Received* received)
{
    // Discard if idle
    if (channel->state == ClientChannel::IDLE)
        return;
    Header* header = received->getOffset<Header>(0);
    // If sending end sending and start receiving
    if (channel->state == ClientChannel::SENDING) {
        channel->outboundMsg.clear();
        channel->inboundMsg.init(header->totalFrags,
                                 channel->currentRpc->responseBuffer);
        channel->state = ClientChannel::RECEIVING;
    }
    if (channel->inboundMsg.processReceivedData(received)) {
        // InboundMsg has gotten its last fragment
        channel->currentRpc->completed();
        channel->rpcId += 1;
        channel->outboundMsg.clear();
        channel->inboundMsg.clear();
        if (TAILQ_EMPTY(&channelQueue)) {
            channel->state = ClientChannel::IDLE;
            channel->currentRpc = 0;
        } else {
            ClientRpc *rpc = TAILQ_FIRST(&channelQueue);
            assert(rpc->channelQueueEntries.tqe_prev);
            TAILQ_REMOVE(&channelQueue, rpc, channelQueueEntries);
            channel->state = ClientChannel::SENDING;
            channel->currentRpc = rpc;
            channel->outboundMsg.beginSending(rpc->requestBuffer);
        }
    }
}

/**
 * Establishes a connected session and begins any queued RPCs on as many
 * channels as are available.
 *
 * \param received
 *      A Driver::Received wrapping an RPC packet with a SessionOpenResponse
 *      contained within.
 */
void
FastTransport::ClientSession::processSessionOpenResponse(
        Driver::Received* received)
{
    // TODO(stutsman) Better idea just to log this and allow it to go through?
    if (numChannels > 0)
        return;
    Header* header = received->getOffset<Header>(0);
    SessionOpenResponse* response =
        received->getOffset<SessionOpenResponse>(sizeof(*header));
    serverSessionHint = header->serverSessionHint;
    token = header->sessionToken;
    LOG(DEBUG, "response max avail: %u", response->maxChannelId);
    numChannels = response->maxChannelId + 1;
    if (MAX_NUM_CHANNELS_PER_SESSION < numChannels)
        numChannels = MAX_NUM_CHANNELS_PER_SESSION;
    LOG(DEBUG, "Session open response: numChannels: %u", numChannels);
    allocateChannels();
    for (uint32_t i = 0; i < numChannels; i++) {
        if (TAILQ_EMPTY(&channelQueue))
            break;
        LOG(DEBUG, "Assigned RPC to channel: %u", i);
        ClientRpc* rpc = TAILQ_FIRST(&channelQueue);
        TAILQ_REMOVE(&channelQueue, rpc, channelQueueEntries);
        channels[i].state = ClientChannel::SENDING;
        channels[i].currentRpc = rpc;
        channels[i].outboundMsg.beginSending(rpc->requestBuffer);
    }
}

} // end RAMCloud