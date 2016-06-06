//
// Created by Alexander Lenhardt on 11/16/15.
//

#include "oggOpusDec.h"
#include <ogg/ogg.h>
#include <cassert>
#include <iostream>
#include <cstring>

using std::istream;

struct MemoryBuffer : std::streambuf {
    MemoryBuffer(char* begin, char* end) {
        setg(begin, begin, end);
    }
};


enum StreamType {
  TYPE_VORBIS,
  TYPE_THEORA,
  TYPE_OPUS,
  TYPE_UNKNOWN
};

class OggStream
{
public:
  int mSerial;
  ogg_stream_state mState;
  StreamType mType;
  bool mActive;
  ogg_int64_t granule;
  ogg_page current_page;
public:
  OggStream(int serial = -1) :
  mSerial(serial),
  mType(TYPE_UNKNOWN),
  mActive(true),
  granule(0)
  {
    std::memset(&mState, 0, sizeof (ogg_stream_state));
  }
  
  ~OggStream() {
    ogg_stream_clear(&mState);
  }
};

struct OggDecoder::Impl {
    ogg_sync_state state;
    OggStream stream;
    int64_t lastSeekingPosition;

    Impl() : lastSeekingPosition(-1) {
      ogg_sync_init(&state);
    }
    ~Impl() {
      ogg_sync_clear(&state);
    }

    bool read_complete_page() {
        if (ogg_sync_pageout(&state, &stream.current_page) == 1) {
            if (stream.mSerial == -1) {
                stream.mSerial = ogg_page_serialno(&stream.current_page);
                ogg_stream_init(&stream.mState, stream.mSerial);
            }
            if (ogg_page_serialno(&stream.current_page) != stream.mState.serialno) {
                /* so all streams are read. */
                stream.mSerial = ogg_page_serialno(&stream.current_page);
                ogg_stream_reset_serialno(&stream.mState, stream.mSerial);
            }
            /*Add page to the bitstream*/
            ogg_stream_pagein(&stream.mState, &stream.current_page);
            stream.granule = ogg_page_granulepos(&stream.current_page);
            return true;
        } else {
            return false;
        }
    }

    int read_packet(ogg_packet *packet) {
        return ogg_stream_packetout(&stream.mState, packet);
    }

};

OggDecoder::OggDecoder() : m_priv(new Impl) {}
OggDecoder::~OggDecoder() {}

int64_t OggDecoder::getCurrentPageGranule() {
    return m_priv->stream.granule;
}

void OggDecoder::read(istream& is) {
    const size_t blockSize = 1024 * 8;
    while (is) {
        /*Read bitstream from input file*/
        char *buf = ogg_sync_buffer(&m_priv->state, blockSize);
        is.read(buf, blockSize);
        std::streamsize nb_read = is.gcount();
        ogg_sync_wrote(&m_priv->state, nb_read);
    }
}

bool OggDecoder::tryParseFullPage() {
    return m_priv->read_complete_page();
}

bool OggDecoder::tryReadPacket(OpusPacket* opPkt) {
    bool ret = false;

    ogg_packet pkt;
    pkt.bytes = 0;
    int result = 0;

    if ((result = m_priv->read_packet(&pkt)) == 1)  {
        opPkt->granulepos = pkt.granulepos;
        opPkt->len        = pkt.bytes;
        opPkt->payload    = pkt.packet;
        ret = true;
    }
    else if (result == -1) {
      if ( (result = m_priv->read_packet(&pkt)) == 1 ) {
        opPkt->granulepos = pkt.granulepos;
        opPkt->len        = pkt.bytes;
        opPkt->payload    = pkt.packet;
        ret = true;
      }
    }
    return ret;
}

void OggDecoder::setLastSeekingPosition(int64_t lastSeekingPosition) {
    m_priv->lastSeekingPosition = lastSeekingPosition;
}

void OggDecoder::flush() {
    ogg_sync_reset(&m_priv->state);
    m_priv->stream = OggStream();
}

/*
 * C interface
 */

COggDecoder createOggDecoder() {
    return reinterpret_cast<COggDecoder>(new OggDecoder());
}
void destroyOggDecoder(COggDecoder dec) {
    delete reinterpret_cast<OggDecoder*>(dec);
}

void onDataReceived(COggDecoder oggDec, char* buffer, size_t bufferLen)
{
    if (bufferLen == 0) 
        return;

    MemoryBuffer mbuf(buffer, buffer + bufferLen);
    std::istream streamData(&mbuf);

    // read() may throw, so catch that and transform it to an error message.
    try {
        reinterpret_cast<OggDecoder *>(oggDec)->read(streamData);
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
    }
}

int readPage(COggDecoder oggDec){
    OggDecoder* dec = reinterpret_cast<OggDecoder *>(oggDec);
    return dec->tryParseFullPage() ? 1 : 0;
}

int readPacket(COggDecoder oggDec, OpusPacket* opPkt) {
    OggDecoder* dec = reinterpret_cast<OggDecoder *>(oggDec);
    return dec->tryReadPacket(opPkt) ? 1 : 0;
}

void setLastSeekingPosition(COggDecoder oggDec, int64_t lastSeekingPosition) {
    OggDecoder *decoder = reinterpret_cast<OggDecoder *>(oggDec);
    decoder->setLastSeekingPosition(lastSeekingPosition);
}

void flushDecoder(COggDecoder oggDec) {
    OggDecoder *decoder = reinterpret_cast<OggDecoder *>(oggDec);
    decoder->flush();
    
}

int64_t getCurrentPageGranule (COggDecoder oggDec) {
    OggDecoder *decoder = reinterpret_cast<OggDecoder *>(oggDec);
    return decoder->getCurrentPageGranule();
}
