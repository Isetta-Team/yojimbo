/*
    Yojimbo Client/Server Network Library.

    Copyright © 2016, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef PROTOCOL_CONNECTION_H
#define PROTOCOL_CONNECTION_H

#include "yojimbo_packet.h"
#include "yojimbo_message.h"
#include "yojimbo_allocator.h"
#include "yojimbo_sequence_buffer.h"

namespace yojimbo
{
    const int CONNECTION_PACKET = 0;

    struct ConnectionPacket : public Packet
    {
        uint16_t sequence;
        uint16_t ack;
        uint32_t ack_bits;

        ConnectionPacket() : Packet( CONNECTION_PACKET )
        {
            sequence = 0;
            ack = 0;
            ack_bits = 0;
        }

    public:

        template <typename Stream> bool Serialize( Stream & stream )
        {
            bool perfect;
            if ( Stream::IsWriting )
                 perfect = ack_bits == 0xFFFFFFFF;

            serialize_bool( stream, perfect );

            if ( !perfect )
                serialize_bits( stream, ack_bits, 32 );
            else
                ack_bits = 0xFFFFFFFF;

            serialize_bits( stream, sequence, 16 );

            int ack_delta = 0;
            bool ack_in_range = false;

            if ( Stream::IsWriting )
            {
                if ( ack < sequence )
                    ack_delta = sequence - ack;
                else
                    ack_delta = (int)sequence + 65536 - ack;

                assert( ack_delta > 0 );
                assert( sequence - ack_delta == ack );
                
                ack_in_range = ack_delta <= 64;
            }

            serialize_bool( stream, ack_in_range );
    
            if ( ack_in_range )
            {
                serialize_int( stream, ack_delta, 1, 64 );
                if ( Stream::IsReading )
                    ack = sequence - ack_delta;
            }
            else
                serialize_bits( stream, ack, 16 );

            serialize_align( stream );

            return true;
        }

        YOJIMBO_SERIALIZE_FUNCTIONS();

        bool operator ==( const ConnectionPacket & other ) const
        {
            return sequence == other.sequence &&
                        ack == other.ack &&
                   ack_bits == other.ack_bits;
        }

        bool operator !=( const ConnectionPacket & other ) const
        {
            return !( *this == other );
        }

    private:

        ConnectionPacket( const ConnectionPacket & other );
        const ConnectionPacket & operator = ( const ConnectionPacket & other );
    };

    struct ConnectionConfig
    {
        int packetType;                         // connect packet type (override)
        
        int maxPacketSize;                      // maximum connection packet size in bytes

        int slidingWindowSize;                  // size of ack sliding window (in packets)

        float messageResendRate;                // message max resend rate in seconds, until acked.

        int messageSendQueueSize;               // message send queue size

        int messageReceiveQueueSize;            // receive queue size

        int messageSentPacketsSize;             // sent packets sliding window size (# of packets)

        int maxMessagesPerPacket;               // maximum number of messages included in a connection packet

        /*
        int maxMessageSize;             // maximum message size allowed in iserialized bytes, eg. post bitpacker
        int maxSmallBlockSize;          // maximum small block size allowed. messages above this size are fragmented and reassembled.
        int maxLargeBlockSize;          // maximum large block size. these blocks are split up into fragments.
        int blockFragmentSize;          // fragment size that large blocks are split up to for transmission.
        int packetBudget;               // maximum number of bytes this channel may take per-packet. 
        int giveUpBits;                 // give up trying to add more messages to packet if we have less than this # of bits available.
        bool align;                     // if true then insert align at key points, eg. before messages etc. good for dictionary based LZ compressors
        */

        ConnectionConfig()
        {
            packetType = CONNECTION_PACKET;

            maxPacketSize = 4 * 1024;
            
            slidingWindowSize = 256;

            messageResendRate = 0.1f;

            messageSendQueueSize = 1024;

            messageReceiveQueueSize = 1024;

            messageSentPacketsSize = 256;

            maxMessagesPerPacket = 64;
        }
    };

    struct ConnectionSentPacketData { uint8_t acked; };

    struct ConnectionReceivedPacketData {};
    
    typedef SequenceBuffer<ConnectionSentPacketData> ConnectionSentPackets;
    
    typedef SequenceBuffer<ConnectionReceivedPacketData> ConnectionReceivedPackets;

    enum ConnectionCounters
    {
        CONNECTION_COUNTER_PACKETS_READ,                        // number of packets read
        CONNECTION_COUNTER_PACKETS_WRITTEN,                     // number of packets written
        CONNECTION_COUNTER_PACKETS_ACKED,                       // number of packets acked
        CONNECTION_COUNTER_PACKETS_DISCARDED,                   // number of read packets that we discarded (eg. not acked)
        CONNECTION_COUNTER_NUM_COUNTERS
    };

    enum ConnectionError
    {
        CONNECTION_ERROR_NONE = 0,
        CONNECTION_ERROR_SOME_ERROR
    };

    class Connection
    {
    public:

        Connection( Allocator & allocator, PacketFactory & packetFactory, MessageFactory & messageFactory, const ConnectionConfig & config = ConnectionConfig() );

        ~Connection();

        void Reset();

        bool CanSendMessage() const;

        void SendMessage( Message * message );

        Message * ReceiveMessage();

        ConnectionPacket * WritePacket();

        bool ReadPacket( ConnectionPacket * packet );

        void AdvanceTime( double time );

        double GetTime() const;

        uint64_t GetCounter( int index ) const;

        ConnectionError GetError() const;

    protected:

        virtual void OnPacketAcked( uint16_t /*sequence*/ ) {}

    protected:

        struct MessageSendQueueEntry
        {
            Message * message;
            double timeLastSent;
            uint32_t largeBlock : 1;
            uint32_t measuredBits : 30;
        };

        struct MessageSentPacketEntry
        {
            double timeSent;
            uint16_t * messageIds;
            uint64_t numMessageIds : 16;                 // number of messages in this packet
            uint64_t blockId : 16;                       // block id. valid only when sending large block.
            uint64_t fragmentId : 16;                    // fragment id. valid only when sending large block.
            uint64_t acked : 1;                          // 1 if this sent packet has been acked
            uint64_t largeBlock : 1;                     // 1 if this sent packet contains a large block fragment
        };

        struct MessageReceiveQueueEntry
        {
            Message * message;
        };

        struct MessageSendLargeBlockData
        {
            MessageSendLargeBlockData()
            {
                acked_fragment = nullptr;
                time_fragment_last_sent = nullptr;
                Reset();
            }

            void Reset()
            {
                active = false;
                numFragments = 0;
                numAckedFragments = 0;
                blockId = 0;
                blockSize = 0;
            }

            bool active;                                // true if we are currently sending a large block
            int numFragments;                           // number of fragments in the current large block being sent
            int numAckedFragments;                      // number of acked fragments in current block being sent
            int blockSize;                              // send block size in bytes
            uint16_t blockId;                           // the message id for the current large block being sent
            BitArray * acked_fragment;                  // has fragment n been received?
            double * time_fragment_last_sent;           // time fragment last sent in seconds.
        };

        struct MessageReceiveLargeBlockData
        {
            MessageReceiveLargeBlockData()
            {
                blockData = NULL;
                receivedFragment = NULL;
                Reset();
            }

            void Reset()
            {
                // todo: need to add code to free the block data elsewhere
                if ( blockData )
                    assert( false );

                active = false;
                numFragments = 0;
                numReceivedFragments = 0;
                blockId = 0;
                blockSize = 0;
                blockData = NULL;
            }

            bool active;                                // true if we are currently receiving a large block
            int numFragments;                           // number of fragments in this block
            int numReceivedFragments;                   // number of fragments received.
            uint16_t blockId;                           // block id being currently received.
            uint32_t blockSize;                         // block size in bytes.
            uint8_t * blockData;                        // pointer to block data being received.
            BitArray * receivedFragment;                // has fragment n been received?
        };

        struct MessageSendLargeBlockStatus
        {
            bool sending;
            int blockId;
            int blockSize;
            int numFragments;
            int numAckedFragments;
        };

        struct MessageReceiveLargeBlockStatus
        {
            bool receiving;
            int blockId;
            int blockSize;
            int numFragments;
            int numReceivedFragments;
        };

        void ProcessAcks( uint16_t ack, uint32_t ack_bits );

        void PacketAcked( uint16_t sequence );

    private:

        const ConnectionConfig m_config;                                                // const configuration data

        Allocator * m_allocator;                                                        // allocator for allocations matching life cycle of object

        PacketFactory * m_packetFactory;                                                // packet factory for creating and destroying connection packets

        MessageFactory * m_messageFactory;                                              // message factory creates and destroys messages

        double m_time;                                                                  // current connection time

        ConnectionError m_error;                                                        // connection error level

        // ack system

        ConnectionSentPackets * m_sentPackets;                                          // sliding window of recently sent packets

        ConnectionReceivedPackets * m_receivedPackets;                                  // sliding window of recently received packets

        // message system

        int m_maxBlockFragments;                                                        // maximum number of fragments per-block

        int m_messageOverheadBits;                                                      // number of bits overhead per-serialized message

        uint16_t m_sendMessageId;                                                       // id for next message added to send queue

        uint16_t m_receiveMessageId;                                                    // id for next message to be received

        uint16_t m_oldestUnackedMessageId;                                              // id for oldest unacked message in send queue

        SequenceBuffer<MessageSendQueueEntry> * m_messageSendQueue;                     // message send queue

        SequenceBuffer<MessageSentPacketEntry> * m_messageSentPackets;                  // messages in sent packets (for acks)

        SequenceBuffer<MessageReceiveQueueEntry> * m_messageReceiveQueue;               // message receive queue

        MessageSendLargeBlockData m_sendLargeBlock;                                     // data for large block being sent

        MessageReceiveLargeBlockData m_receiveLargeBlock;                               // data for large block being received

        uint16_t * m_sentPacketMessageIds;                                              // array of message ids, n ids per-sent packet

        // counters

        uint64_t m_counters[CONNECTION_COUNTER_NUM_COUNTERS];                           // counters for unit testing, stats etc.
    };
}

#endif
