//
// Created by Alexander Lenhardt on 11/16/15.
//

#ifndef OGGOPUSDEC_OGGOPUSDEC_H_H
#define OGGOPUSDEC_OGGOPUSDEC_H_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    unsigned char*  payload;
    int64_t         granulepos;
    size_t          len;
} OpusPacket;

#ifdef  __cplusplus
#include <istream>
#include <vector>
#include <memory>

class OggDecoder
{
    struct Impl; std::unique_ptr<Impl> m_priv;
public:
    OggDecoder();
    ~OggDecoder();
    void read(std::istream& stream);
    bool tryParseFullPage();
    bool tryReadPacket(OpusPacket* outPkt);
    void setLastSeekingPosition(int64_t lastSeekingPoistion);
    void flush();
};
#endif

typedef void* COggDecoder;

#ifdef  __cplusplus
extern "C" {
#endif
    COggDecoder createOggDecoder        ();
    void        destroyOggDecoder       (COggDecoder);
    void        onDataReceived          (COggDecoder oggDec, char* buffer, size_t bufferLen);
    int         readPage                (COggDecoder oggDec);
    int         readPacket              (COggDecoder oggDec, OpusPacket* pkt);
    void        setLastSeekingPosition  (COggDecoder oggDec, int64_t lastSeekingPoistion);
    void        flushDecoder            (COggDecoder oggDec);
#ifdef  __cplusplus
}
#endif
#endif //OGGOPUSDEC_OGGOPUSDEC_H_H
